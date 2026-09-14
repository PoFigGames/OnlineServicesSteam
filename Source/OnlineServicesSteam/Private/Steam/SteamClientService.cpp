// Copyright PoFig Games Studio. All Rights Reserved.

#include "SteamClientService.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamDeviceInfo.h"
#include "SteamPlatformConfig.h"

// Engine
#include "GenericPlatform/GenericPlatformFile.h"


namespace PoFigGames::Steam
{
	FSteamClientService::~FSteamClientService()
	{
		Shutdown();
	}

	bool FSteamClientService::InternalInit()
	{
		// Valve requires the relaunch check to run before the API is brought up: if the game was not started
		// from within Steam but is supposed to be, the client relaunches it and this process goes away.
		if (SteamServiceConfig.bRelaunchInSteam && SteamServiceConfig.SteamAppId > 0 && SteamAPI_RestartAppIfNecessary(SteamServiceConfig.SteamAppId))
		{
			if constexpr (FPlatformProperties::IsGameOnly() || FPlatformProperties::IsServerOnly())
			{
				UE_LOG(LogOnlineServicesSteam, Log, TEXT("Relaunching through the Steam client; this process is done"));
				FPlatformMisc::RequestExit(false);
			}

			return false;
		}

		switch (SteamErrMsg OutErrMsg; SteamAPI_InitEx(&OutErrMsg))
		{
			case k_ESteamAPIInitResult_OK:
				break;
			case k_ESteamAPIInitResult_NoSteamClient:
				UE_LOG(LogOnlineServicesSteam, Error, TEXT("SteamAPI_InitEx failed: No Steam client running (%hs)"), OutErrMsg);
				return false;
			case k_ESteamAPIInitResult_VersionMismatch:
				UE_LOG(LogOnlineServicesSteam, Error, TEXT("SteamAPI_InitEx failed: Steam client version mismatch (%hs)"), OutErrMsg);
				return false;
			case k_ESteamAPIInitResult_FailedGeneric:
				UE_LOG(LogOnlineServicesSteam, Error, TEXT("SteamAPI_InitEx failed: Generic failure (%hs)"), OutErrMsg);
				return false;
			default:
				UE_LOG(LogOnlineServicesSteam, Error, TEXT("SteamAPI_InitEx failed: Unknown error (%hs)"), OutErrMsg);
				return false;
		}

		// Resolved once here; everything else in the plugin works through these handles.
		if (!Interfaces.Resolve())
		{
			SteamAPI_Shutdown();
			return false;
		}

		// Make sure the Steam user has valid access to the game.
		bool bIsSubscribed = true;
		if constexpr (FPlatformProperties::IsGameOnly() || FPlatformProperties::IsServerOnly())
		{
			bIsSubscribed = Interfaces.Apps->BIsSubscribed();
		}

		if (!bIsSubscribed)
		{
			UE_LOG(LogOnlineServicesSteam, Error, TEXT("This account does not own the application, shutting down"));
			FPlatformMisc::RequestExit(false);

			SteamAPI_Shutdown();
			return false;
		}

		Interfaces.Utils->SetWarningMessageHook(SteamworksWarningMessageHook);

		if (SteamServiceConfig.bUseSteamTransport)
		{
			Interfaces.NetworkingUtils->InitRelayNetworkAccess();
		}

		PublishSteamClientInterfaces(&Interfaces);

		UE_LOG(LogOnlineServicesSteam, Log, TEXT("[AppId: %d] Client API initialized"), GetSteamAppId());

		// Which device this is, said once where support and telemetry can find it afterwards. The overloads
		// of this namespace hide the one for a boolean, so those are spelled out.
		const auto DeviceInfo = GetSteamDeviceInfo();
		UE_LOG(LogOnlineServicesSteam, Log, TEXT("Steam hardware [%s], suggested settings [%s], Big Picture [%s], VR [%s], Proton [%s]"),
			LexToString(DeviceInfo.Hardware), LexToString(DeviceInfo.SuggestedConfig),
			DeviceInfo.bBigPictureMode ? TEXT("true") : TEXT("false"),
			DeviceInfo.bSteamRunningInVR ? TEXT("true") : TEXT("false"),
			DeviceInfo.bRunningUnderProton ? TEXT("true") : TEXT("false"));

		return true;
	}

	void FSteamClientService::InternalShutdown()
	{
		UE_LOG(LogOnlineServicesSteam, Log, TEXT("Shutting the Steamworks API down"));

		// Nothing may reach for an interface of an API that is going away.
		WithdrawSteamClientInterfaces(&Interfaces);
		Interfaces = FSteamClientInterfaces { };

		SteamAPI_Shutdown();
	}

	void FSteamClientService::InternalPumpDispatch() const
	{
		SteamAPI_RunCallbacks();
	}
}
