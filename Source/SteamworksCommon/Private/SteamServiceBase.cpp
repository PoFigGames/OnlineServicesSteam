// Copyright PoFig Games Studio. All Rights Reserved.

#include "SteamServiceBase.h"

// Project
#include "SteamInterfaces.h"
#include "SteamworksCommonLogChannels.h"
#include "SteamworksCommonModule.h"

// Engine
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"

THIRD_PARTY_INCLUDES_START
#include "steam/steam_api.h"
THIRD_PARTY_INCLUDES_END


DEFINE_LOG_CATEGORY(LogSteamService);

namespace PoFigGames::Steam
{
#if !UE_BUILD_SHIPPING && !UE_BUILD_SHIPPING_WITH_EDITOR && !UE_BUILD_TEST
	namespace Private
	{
		static const FString& GetSteamAppIdFilename()
		{
			/** Filename containing the appid during development */
			static FString SteamAppIdFilename { FString::Printf(TEXT("%ssteam_appid.txt"), FPlatformProcess::BaseDir()) };
			return SteamAppIdFilename;
		}

		/**
		 * Steam reads the app id from a file beside the executable, so it is written before the API comes up
		 */
		static bool WriteSteamAppIdToDisk(const int32 SteamDevAppId)
		{
			if (SteamDevAppId > 0)
			{
				// The physical file system is asked directly, because Steam reads this file from beside the
				// executable and a cooked build would otherwise route the write into the pak.
				const auto& SteamAppIdFilename = GetSteamAppIdFilename();
				if (const TUniquePtr<IFileHandle> Handle { IPlatformFile::GetPlatformPhysical().OpenWrite(*SteamAppIdFilename, false, false) }; Handle)
				{
					TAnsiStringBuilder<16> AppId { InPlace, SteamDevAppId };

					// The handle says whether the bytes reached the disk, and the caller reads the answer as
					// "Steam will find the file", so a full disk must not come back as success.
					return Handle->Write(reinterpret_cast<const uint8*>(AppId.GetData()), AppId.Len());
				}

				return false;
			}

			UE_LOG(LogSteamService, Warning, TEXT("App id %d cannot be used: it has to be greater than zero"), SteamDevAppId);
			return false;
		}

		/**
		 * Removes that file again.
		 */
		static void DeleteSteamAppIdFromDisk()
		{
			const auto& SteamAppIdFilename = GetSteamAppIdFilename();
			if (auto& Physical = IPlatformFile::GetPlatformPhysical(); Physical.FileExists(*SteamAppIdFilename))
			{
				Physical.DeleteFile(*SteamAppIdFilename);
			}
		}
	}
#endif

	/** Port the engine falls back to when neither the command line nor the ini names one; BaseEngine.ini [URL]. */
	constexpr int32 DefaultGamePort { 7777 };


	bool FSteamServiceBase::CanCleanUp() const
	{
		// The module is asked for only once it is known to be there. Get() is LoadModuleChecked, and this is
		// reached from the service destructors, which run while the process is tearing down: by then loading
		// the module again would check() rather than answer.
		if (!bInitialized || !FSteamworksCommonModule::IsAvailable())
		{
			return false;
		}

		return FSteamworksCommonModule::Get().AreSteamDllsLoaded();
	}

	bool FSteamServiceBase::Init(const FSteamPlatformConfig& Config)
	{
		check(IsInGameThread());

		if (bInitialized)
		{
			return true;
		}

		SteamAPI_SetTryCatchCallbacks(false);

		SteamServiceConfig = Config;

		// The port the game listens on is an engine level setting, so it is read the same way FUrlConfig::Init
		// reads it and from the same places: a switch spelled differently here would put this API on one port
		// while the engine listened on another. That search has no word boundary, which is why no switch this
		// plugin introduces may end in "Port=".
		if (FParse::Value(FCommandLine::Get(), TEXT("Port="), GamePort) == false)
		{
			GConfig->GetInt(TEXT("URL"), TEXT("Port"), GamePort, GEngineIni);
		}

		// Neither source is obliged to answer, and the game server API takes this as a uint16 and ends the
		// process when it does not fit, so a value which cannot be a port is replaced here, where there is
		// still something useful to say about it.
		if (GamePort <= 0 || GamePort > MAX_uint16)
		{
			UE_LOG(LogSteamService, Warning, TEXT("Game port %d is not a usable port, falling back to %d"), GamePort, DefaultGamePort);
			GamePort = DefaultGamePort;
		}

	#if !UE_BUILD_SHIPPING && !UE_BUILD_SHIPPING_WITH_EDITOR && !UE_BUILD_TEST
		// Only a process the Steam client did not start needs this file; one launched through the client is
		// told its app id by the environment. Writing it is also what makes bRelaunchInSteam do nothing in a
		// development build, which is wanted: a build started from an IDE should not be bounced into Steam.
		if (!Private::WriteSteamAppIdToDisk(SteamServiceConfig.SteamAppId))
		{
			UE_LOG(LogSteamService, Warning,
				TEXT("Could not write steam_appid.txt; the Steamworks API will only come up if this process was launched from Steam"));
		}
	#endif

		bInitialized = InternalInit();

		UE_CLOG(bInitialized, LogSteamService, Log, TEXT("Steam API initialized"));

	#if !UE_BUILD_SHIPPING && !UE_BUILD_SHIPPING_WITH_EDITOR && !UE_BUILD_TEST
		// Steam reads the file while the API is coming up and never afterwards, so it goes as soon as that
		// is over. Left behind - which is what a crash used to do - it stops the client relaunching any
		// later run out of this directory.
		Private::DeleteSteamAppIdFromDisk();
	#endif

		return bInitialized;
	}

	void FSteamServiceBase::Shutdown()
	{
		check(IsInGameThread());

		if (CanCleanUp())
		{
			InternalShutdown();

			UE_LOG(LogSteamService, Log, TEXT("Steamworks API shut down"));
		}

		// Cleared whether or not the API could be cleaned up: the flag says this service will not be used
		// again, and a service which could not be shut down is the last one that should report itself valid.
		bInitialized = false;
	}

	void FSteamServiceBase::PumpDispatch()
	{
		if (!bInitialized || LastPumpedFrame == GFrameCounter)
		{
			return;
		}

		LastPumpedFrame = GFrameCounter;

		InternalPumpDispatch();
	}

	TSharedPtr<FSteamServiceBase>& Private::GetSteamServiceSlot(const ESteamApi Api)
	{
		static TSharedPtr<FSteamServiceBase> ClientServiceSlot { };
		static TSharedPtr<FSteamServiceBase> GameServerServiceSlot { };

		return Api == ESteamApi::Client ? ClientServiceSlot : GameServerServiceSlot;
	}

	const TCHAR* LexToString(const ESteamTransport Transport)
	{
		switch (Transport)
		{
			case ESteamTransport::Messages:
				return TEXT("Messages");
			case ESteamTransport::Sockets:
				break;
		}

		return TEXT("Sockets");
	}

	void LexFromString(ESteamTransport& OutTransport, const TCHAR* InString)
	{
		if (FCString::Stricmp(InString, TEXT("Messages")) == 0)
		{
			OutTransport = ESteamTransport::Messages;
			return;
		}

		// The two transports take different paths through the socket subsystem, so a misspelled name in the
		// config would quietly change how the game networks. Falling back is still right; doing it silently
		// is not.
		if (FCString::Stricmp(InString, TEXT("Sockets")) != 0)
		{
			UE_LOG(LogSteamService, Warning, TEXT("Unknown Steam transport '%s', falling back to %s"), InString, LexToString(ESteamTransport::Sockets));
		}

		OutTransport = ESteamTransport::Sockets;
	}

	const FSteamPlatformConfig* GetRunningSteamConfig()
	{
		// A slot also holds a service whose API never came up, which has a configuration but nothing running.
		if (const TSharedPtr<FSteamServiceBase>& ClientService = Private::GetSteamServiceSlot(ESteamApi::Client); ClientService.IsValid() && ClientService->IsValid())
		{
			return &ClientService->GetConfig();
		}

		if (const TSharedPtr<FSteamServiceBase>& ServerService = Private::GetSteamServiceSlot(ESteamApi::GameServer); ServerService.IsValid() && ServerService->IsValid())
		{
			return &ServerService->GetConfig();
		}

		return nullptr;
	}

	void SetSteamServerDetails(const FSteamServerDetails& ServerDetails)
	{
		auto Interface = GetSteamInterface<ISteamGameServer>();
		if (Interface == nullptr)
		{
			// This process runs no game server, so there is nobody to describe.
			return;
		}

		UE_LOG(LogSteamService, Verbose, TEXT("Reporting the server to Steam: Name [%s], Map [%s], Players [%d], Bots [%d]"),
			*ServerDetails.ServerName, *ServerDetails.MapName, ServerDetails.MaxPlayerCount, ServerDetails.BotPlayerCount);

		Interface->SetServerName(TCHAR_TO_UTF8(*ServerDetails.ServerName));
		Interface->SetMapName(TCHAR_TO_UTF8(*ServerDetails.MapName));
		Interface->SetMaxPlayerCount(ServerDetails.MaxPlayerCount);
		Interface->SetBotPlayerCount(ServerDetails.BotPlayerCount);
		Interface->SetPasswordProtected(ServerDetails.bPasswordProtected);
	}

	void ShutdownSteamServices()
	{
		check(IsInGameThread());

		// The game server API is layered on the client one where both are up, so it goes first.
		for (const ESteamApi Api : { ESteamApi::GameServer, ESteamApi::Client })
		{
			if (TSharedPtr<FSteamServiceBase>& ServiceSlot = Private::GetSteamServiceSlot(Api); ServiceSlot.IsValid())
			{
				ServiceSlot->Shutdown();
				ServiceSlot.Reset();
			}
		}
	}

	void CDECL FSteamServiceBase::SteamworksWarningMessageHook(const int Severity, const char* Message)
	{
		switch (Severity)
		{
			case 0:
				UE_LOG(LogSteamService, Verbose, TEXT("Steamworks SDK: %s"), UTF8_TO_TCHAR(Message));
				break;
			case 1:
				UE_LOG(LogSteamService, Warning, TEXT("Steamworks SDK: %s"), UTF8_TO_TCHAR(Message));
				break;
			default:  // Unknown severity; new SDK?
				UE_LOG(LogSteamService, Display, TEXT("Steamworks SDK: %s"), UTF8_TO_TCHAR(Message));
				break;
		}
	}
}