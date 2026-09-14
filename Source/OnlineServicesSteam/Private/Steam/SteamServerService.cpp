// Copyright PoFig Games Studio. All Rights Reserved.

#include "SteamServerService.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamPlatformConfig.h"

// Engine
#include "Misc/CommandLine.h"
#include "SocketSubsystem.h"


namespace PoFigGames::Steam
{
	/** Port Steam queries a game server on when nothing names one; the same value FSteamPlatformConfig starts from. */
	constexpr int32 DefaultQueryPort { 27015 };


	FSteamServerService::~FSteamServerService()
	{
		Shutdown();
	}

	bool FSteamServerService::InternalInit()
	{
		// Get the multihome address. If there is no one, this server will be set to listen on any address.
		uint32 LocalServerIP { 0 };
		if (FString MultiHome; FParse::Value(FCommandLine::Get(), TEXT("MULTIHOME="), MultiHome) && !MultiHome.IsEmpty())
		{
			if (const TSharedPtr<FInternetAddr> MultiHomeIP = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetAddressFromString(MultiHome); MultiHomeIP.IsValid())
			{
				MultiHomeIP->GetIp(LocalServerIP);
			}
		}

		// The command line wins over the configured query port, so several servers can share a machine. The
		// switch deliberately does not end in "Port=": FParse::Value searches for a substring with no word
		// boundary, so a switch spelled that way would be found by every search for the game port, this
		// plugin's and FUrlConfig::Init's alike, and the server would come up on its query port.
		if (FParse::Value(FCommandLine::Get(), TEXT("QueryPortOverride="), QueryPort) == false)
		{
			QueryPort = SteamServiceConfig.GameServerQueryPort;
		}

		// Neither source is obliged to name a port at all, and the game server API takes this as a uint16 and
		// ends the process when it does not fit. The game port is clamped the same way, one layer down.
		if (QueryPort <= 0 || QueryPort > MAX_uint16)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("Query port %d is not a usable port, falling back to %d"), QueryPort, DefaultQueryPort);
			QueryPort = DefaultQueryPort;
		}

		UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("Bringing the game server up on 0x%08X, game port %d, query port %d"), LocalServerIP, GamePort, QueryPort);

		const EServerMode ServerMode = SteamServiceConfig.bVACEnabled ? eServerModeAuthenticationAndSecure : eServerModeAuthentication;
		switch (SteamErrMsg OutErrMsg; SteamGameServer_InitEx(LocalServerIP, IntCastChecked<uint16>(GamePort), IntCastChecked<uint16>(QueryPort), ServerMode, TCHAR_TO_UTF8(*SteamServiceConfig.GameVersion), &OutErrMsg))
		{
			case k_ESteamAPIInitResult_OK:
				break;
			case k_ESteamAPIInitResult_NoSteamClient:
				UE_LOG(LogOnlineServicesSteam, Error, TEXT("SteamGameServer_InitEx failed: No Steam running (%hs)"), OutErrMsg);
				return false;
			case k_ESteamAPIInitResult_VersionMismatch:
				UE_LOG(LogOnlineServicesSteam, Error, TEXT("SteamGameServer_InitEx failed: Steam version mismatch (%hs)"), OutErrMsg);
				return false;
			case k_ESteamAPIInitResult_FailedGeneric:
				UE_LOG(LogOnlineServicesSteam, Error, TEXT("SteamGameServer_InitEx failed: Generic failure (%hs)"), OutErrMsg);
				return false;
			default:
				UE_LOG(LogOnlineServicesSteam, Error, TEXT("SteamGameServer_InitEx failed: Unknown error (%hs)"), OutErrMsg);
				return false;
		}

		// Resolved once here, from the game server context: the client accessors of the same interfaces are
		// not valid on a dedicated server.
		if (!Interfaces.Resolve())
		{
			SteamGameServer_Shutdown();
			return false;
		}

		Interfaces.GameServer->SetModDir(TCHAR_TO_UTF8(*SteamServiceConfig.GameDirectory));
		Interfaces.GameServer->SetProduct(TCHAR_TO_UTF8(*SteamServiceConfig.ProductName));
		Interfaces.GameServer->SetGameDescription(TCHAR_TO_UTF8(*SteamServiceConfig.GameDescription));
		Interfaces.GameServer->SetDedicatedServer(IsRunningDedicatedServer());

		if (!Interfaces.GameServer->BLoggedOn())
		{
			Interfaces.GameServer->LogOnAnonymous();
		}

		// What the server is running is reported once it is actually running something; see
		// UNetDriverSteam::PublishServerDetails.
		Interfaces.GameServer->SetAdvertiseServerActive(SteamServiceConfig.bAdvertiseServer);

		Interfaces.Utils->SetWarningMessageHook(SteamworksWarningMessageHook);

		if (SteamServiceConfig.bUseSteamTransport)
		{
			Interfaces.NetworkingUtils->InitRelayNetworkAccess();
		}

		PublishSteamServerInterfaces(&Interfaces);

		UE_LOG(LogOnlineServicesSteam, Log, TEXT("[AppId: %d] Game Server API initialized"), GetSteamAppId());

		return true;
	}

	void FSteamServerService::InternalShutdown()
	{
		// Logging off drops the server from the backend at once instead of leaving it to time out.
		if (Interfaces.GameServer != nullptr)
		{
			Interfaces.GameServer->LogOff();
		}

		// Nothing may reach for an interface of an API that is going away.
		WithdrawSteamServerInterfaces(&Interfaces);
		Interfaces = FSteamServerInterfaces { };

		SteamGameServer_Shutdown();
	}

	void FSteamServerService::InternalPumpDispatch() const
	{
		SteamGameServer_RunCallbacks();
	}
}
