// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SteamPlatformConfig.h"
#include "SteamworksCommonLogChannels.h"

THIRD_PARTY_INCLUDES_START
#include "steam/steam_api_common.h"
THIRD_PARTY_INCLUDES_END


namespace PoFigGames::Steam
{
	/**
	 * @enum ESteamApi
	 *
	 * @brief The two Steamworks APIs a process can bring up. Each of them is process wide, so there is at most one
	 * service per entry no matter how many online services instances exist.
	 */
	enum class ESteamApi : uint8
	{
		Client,
		GameServer
	};

	/** Shuts every Steamworks API down. Called once, when the module unloads. */
	STEAMWORKSCOMMON_API void ShutdownSteamServices();

	/**
	 * @class FSteamServiceBase
	 *
	 * @brief Owns the lifetime of one Steamworks API and dispatches its callbacks.
	 *
	 * There is exactly one service per API in a process, and it lives as long as the process does: the SDK
	 * documents SteamAPI_Shutdown as something to call during process shutdown (steam_api.h) and offers no
	 * supported way to bring an API back up afterwards. The service is therefore created on demand, held by
	 * the module, and shut down once when the module unloads. Online services instances take a handle to use
	 * it - a second instance (PIE, a client hosting a listen server) joins the API which is already running,
	 * and releasing a handle never takes the API down.
	 *
	 * Based on FSteamInstanceHandlerBase.
	 */
	class FSteamServiceBase : public TSharedFromThis<FSteamServiceBase>
	{
	public:
		static constexpr HSteamPipe SteamPipeInvalid { 0 };

		FSteamServiceBase(const FSteamServiceBase&) = delete;
		auto& operator = (const FSteamServiceBase&) = delete;

		virtual ~FSteamServiceBase() = default;

		/** Brings the API up. Called once, by the first holder of the service. */
		STEAMWORKSCOMMON_API bool Init(const FSteamPlatformConfig& Config);

		/** Dispatches the callbacks of this API, at most once per frame however many holders tick it. */
		STEAMWORKSCOMMON_API void PumpDispatch();

		/** Whether the service is successfully initialized */
		bool IsValid() const { return bInitialized; }

		int32 GetGamePort() const { return GamePort; }
		int32 GetSteamAppId() const { return SteamServiceConfig.SteamAppId; }

		/** The configuration this API was brought up with. */
		const FSteamPlatformConfig& GetConfig() const { return SteamServiceConfig; }

	protected:
		/** Only the process shutdown path takes an API down, so that nothing re-initializes it afterwards. */
		friend STEAMWORKSCOMMON_API void ShutdownSteamServices();

		/** Shuts the API down. Safe to call more than once and on a service which never came up. */
		STEAMWORKSCOMMON_API void Shutdown();

		FSteamPlatformConfig SteamServiceConfig { };

		int32 GamePort { -1 };
		bool  bInitialized { false };

		STEAMWORKSCOMMON_API FSteamServiceBase() = default;

		STEAMWORKSCOMMON_API virtual bool CanCleanUp() const;

		/** Brings the API up and resolves its interfaces. The base class owns bInitialized, so this only reports success. */
		virtual bool InternalInit() = 0;
		virtual void InternalShutdown() = 0;

		/** Runs one dispatch pass of this API. */
		virtual void InternalPumpDispatch() const = 0;

		/**
		 * Callback function into a Steam error messaging system
		 * @param Severity - error level
		 * @param Message What Steam reported.
		 */
		STEAMWORKSCOMMON_API static void CDECL SteamworksWarningMessageHook(int Severity, const char* Message);

	private:
		/**
		 * Frame this service last dispatched on, so shared holders do not pump the same API twice.
		 * Starts outside any real frame number, otherwise the very first frame would be taken for a repeat.
		 */
		uint64 LastPumpedFrame { TNumericLimits<uint64>::Max() };
	};

	namespace Private
	{
		/** The service of one API, owned by the module for the lifetime of the process. Empty until first use. */
		STEAMWORKSCOMMON_API TSharedPtr<FSteamServiceBase>& GetSteamServiceSlot(ESteamApi Api);
	}

	/**
	 * The configuration whichever Steamworks API is running was brought up with, preferring the client one.
	 *
	 * The APIs are process wide and are configured once, so this is the same configuration every holder of
	 * a service sees. It lets a module which does not own a service read the settings without having to be
	 * handed them by the one that does.
	 *
	 * @return The configuration, or null while no API is up.
	 */
	STEAMWORKSCOMMON_API const FSteamPlatformConfig* GetRunningSteamConfig();

	/**
	 * @struct FSteamServerDetails
	 *
	 * @brief What a game server tells the Steam master server about the match it is running.
	 *
	 * Everything here changes from one match to the next, which is why it is not part of the settings the
	 * API is brought up with. A server which reports none of it is still listed, as a nameless entry on an
	 * unknown map, which is worse than not being listed at all.
	 */
	struct FSteamServerDetails
	{
		/** Name shown in the server browser. */
		FString ServerName { };

		/** Map the server is running, as the browser and the master server filters know it. */
		FString MapName { };

		/** How many players the match holds, and how many of them are bots. */
		int32 MaxPlayerCount { 0 };
		int32 BotPlayerCount { 0 };

		/** Whether joining takes a password, which the browser shows and filters on. */
		bool bPasswordProtected { false };
	};

	/**
	 * Tells the Steam master server what this process is running.
	 *
	 * Does nothing when this process runs no game server, which is the case for a client that only ever
	 * joins somebody else's game.
	 */
	STEAMWORKSCOMMON_API void SetSteamServerDetails(const FSteamServerDetails& ServerDetails);

	/**
	 * Acquires the service of one Steamworks API, bringing the API up on the first acquisition and joining it
	 * on every later one. The returned handle is for using the API, not for owning it: the API stays up until
	 * the process shuts down.
	 *
	 * @return The service, or null when the API could not be initialized.
	 */
	template<typename ServiceType>
	TSharedPtr<ServiceType> AcquireSteamService(const FSteamPlatformConfig& Config)
	{
		check(IsInGameThread());

		auto& ServiceSlot = Private::GetSteamServiceSlot(ServiceType::Api);

		if (ServiceSlot.IsValid())
		{
			if (!ServiceSlot->IsValid())
			{
				// The API has already been shut down in this process and the SDK offers no way back up.
				UE_LOG(LogSteamService, Error, TEXT("The Steamworks API has been shut down and cannot be initialized again in this process"));
				return nullptr;
			}

			if (ServiceSlot->GetSteamAppId() != Config.SteamAppId)
			{
				UE_LOG(LogSteamService, Warning, TEXT("Steam API is already running for app id %d, the requested %d is ignored"),
					ServiceSlot->GetSteamAppId(), Config.SteamAppId);
			}

			return StaticCastSharedPtr<ServiceType>(ServiceSlot);
		}

		auto Service = MakeShared<ServiceType>();
		if (!Service->Init(Config))
		{
			// The API never came up, so the slot stays empty and a later attempt may try again.
			return nullptr;
		}

		ServiceSlot = Service;

		return Service;
	}
}
