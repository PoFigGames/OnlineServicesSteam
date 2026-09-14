// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SteamworksCommonLogChannels.h"

THIRD_PARTY_INCLUDES_START
#include "steam/isteamnetworkingmessages.h"
#include "steam/steam_api.h"
#include "steam/steam_gameserver.h"
THIRD_PARTY_INCLUDES_END


namespace PoFigGames::Steam
{
	/**
	 * Resolves one Steamworks interface and reports a missing one under its own name.
	 *
	 * The caller's state is never touched: whether the resolve succeeded is the return value, so a set of
	 * these can be combined however the caller needs. A missing optional interface is not a failure.
	 */
	template<typename InterfaceType>
	bool TryResolveSteamInterface(InterfaceType*& OutInterface, InterfaceType* InInterface, const TCHAR* InterfaceName, const bool bRequired = true)
	{
		OutInterface = InInterface;

		if (InInterface == nullptr)
		{
			UE_LOG(LogSteamService, Warning, TEXT("Steamworks interface is not available: %s (%s)"),
				InterfaceName, bRequired ? TEXT("required") : TEXT("optional"));

			return !bRequired;
		}

		return true;
	}

	/** Stringifies the accessor name for the log; carries no state of its own. */
	#define STEAM_RESOLVE_INTERFACE(Target, Accessor) PoFigGames::Steam::TryResolveSteamInterface(Target, Accessor(), TEXT(#Accessor))
	#define STEAM_RESOLVE_OPTIONAL_INTERFACE(Target, Accessor) PoFigGames::Steam::TryResolveSteamInterface(Target, Accessor(), TEXT(#Accessor), false)

	/**
	 * @struct FSteamClientInterfaces
	 *
	 * @brief Steamworks interfaces of the client API, resolved once by the client service after SteamAPI_Init.
	 * The fields are exactly the interfaces this game uses: adding a call means adding a field, so the
	 * validated set and the used set cannot drift apart.
	 */
	struct FSteamClientInterfaces
	{
		ISteamUser*              User { nullptr };
		ISteamFriends*           Friends { nullptr };
		ISteamUtils*             Utils { nullptr };
		ISteamApps*              Apps { nullptr };
		ISteamMatchmaking*       Matchmaking { nullptr };
		ISteamUserStats*         UserStats { nullptr };
		ISteamRemoteStorage*     RemoteStorage { nullptr };
		ISteamNetworkingUtils*   NetworkingUtils { nullptr };
		ISteamNetworkingSockets* NetworkingSockets { nullptr };

		/** The connectionless transport. Only usable once the relay network has been initialized. */
		ISteamNetworkingMessages* NetworkingMessages { nullptr };

		/** Not offered by older Steam clients, so the game has to work without it. */
		ISteamParentalSettings*  ParentalSettings { nullptr };

		/** Resolves every interface, logging the missing ones. Returns false when a required one is absent. */
		STEAMWORKSCOMMON_API bool Resolve();
	};

	/**
	 * @struct FSteamServerInterfaces
	 *
	 * @brief Steamworks interfaces of the game server API, resolved once by the server service after
	 * SteamGameServer_Init. These come from the game server context: the client accessors of the same
	 * interfaces are not valid on a dedicated server.
	 */
	struct FSteamServerInterfaces
	{
		ISteamGameServer*        GameServer { nullptr };
		ISteamUtils*             Utils { nullptr };
		ISteamNetworkingUtils*   NetworkingUtils { nullptr };
		ISteamNetworkingSockets* NetworkingSockets { nullptr };

		/** The connectionless transport. Only usable once the relay network has been initialized. */
		ISteamNetworkingMessages* NetworkingMessages { nullptr };

		/** Resolves every interface, logging the missing ones. Returns false when a required one is absent. */
		STEAMWORKSCOMMON_API bool Resolve();
	};

	/** Interfaces of the running client service, or null when no client service has initialized. */
	STEAMWORKSCOMMON_API const FSteamClientInterfaces* GetSteamClientInterfaces();

	/** Interfaces of the running server service, or null when no server service has initialized. */
	STEAMWORKSCOMMON_API const FSteamServerInterfaces* GetSteamServerInterfaces();

	namespace Private
	{
		/**
		 * @struct TSteamInterfaceTraits
		 *
		 * @brief Says which field of each set holds a given interface. Only the interfaces this game uses are
		 * mapped below, so asking for anything else does not compile rather than silently answering null.
		 */
		template<typename InterfaceType>
		struct TSteamInterfaceTraits;

		/** An interface only the client API offers. */
		template<typename InterfaceType, InterfaceType* FSteamClientInterfaces::* ClientMember>
		struct TSteamClientInterface
		{
			static InterfaceType* FromClient(const FSteamClientInterfaces& Interfaces) { return Interfaces.*ClientMember; }
			static InterfaceType* FromServer(const FSteamServerInterfaces& /*Interfaces*/) { return nullptr; }
		};

		/** An interface only the game server API offers. */
		template<typename InterfaceType, InterfaceType* FSteamServerInterfaces::* ServerMember>
		struct TSteamServerInterface
		{
			static InterfaceType* FromClient(const FSteamClientInterfaces& /*Interfaces*/) { return nullptr; }
			static InterfaceType* FromServer(const FSteamServerInterfaces& Interfaces) { return Interfaces.*ServerMember; }
		};

		/** An interface both APIs offer. */
		template<typename InterfaceType, InterfaceType* FSteamClientInterfaces::* ClientMember, InterfaceType* FSteamServerInterfaces::* ServerMember>
		struct TSteamSharedInterface
		{
			static InterfaceType* FromClient(const FSteamClientInterfaces& Interfaces) { return Interfaces.*ClientMember; }
			static InterfaceType* FromServer(const FSteamServerInterfaces& Interfaces) { return Interfaces.*ServerMember; }
		};

		template<>
		struct TSteamInterfaceTraits<ISteamUser>
			: TSteamClientInterface<ISteamUser, &FSteamClientInterfaces::User>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamFriends>
			: TSteamClientInterface<ISteamFriends, &FSteamClientInterfaces::Friends>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamApps>
			: TSteamClientInterface<ISteamApps, &FSteamClientInterfaces::Apps>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamMatchmaking>
			: TSteamClientInterface<ISteamMatchmaking, &FSteamClientInterfaces::Matchmaking>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamRemoteStorage>
			: TSteamClientInterface<ISteamRemoteStorage, &FSteamClientInterfaces::RemoteStorage>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamUserStats>
			: TSteamClientInterface<ISteamUserStats, &FSteamClientInterfaces::UserStats>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamParentalSettings>
			: TSteamClientInterface<ISteamParentalSettings, &FSteamClientInterfaces::ParentalSettings>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamGameServer>
			: TSteamServerInterface<ISteamGameServer, &FSteamServerInterfaces::GameServer>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamUtils>
			: TSteamSharedInterface<ISteamUtils, &FSteamClientInterfaces::Utils, &FSteamServerInterfaces::Utils>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamNetworkingUtils>
			: TSteamSharedInterface<ISteamNetworkingUtils, &FSteamClientInterfaces::NetworkingUtils, &FSteamServerInterfaces::NetworkingUtils>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamNetworkingMessages>
			: TSteamSharedInterface<ISteamNetworkingMessages, &FSteamClientInterfaces::NetworkingMessages, &FSteamServerInterfaces::NetworkingMessages>
		{
		};

		template<>
		struct TSteamInterfaceTraits<ISteamNetworkingSockets>
			: TSteamSharedInterface<ISteamNetworkingSockets, &FSteamClientInterfaces::NetworkingSockets, &FSteamServerInterfaces::NetworkingSockets>
		{
		};
	}

	/**
	 * Returns one Steamworks interface from whichever API offers it, preferring the client one where both are
	 * running. This is the single place that knows how to look an interface up, so a call site never has to
	 * ask each API in turn.
	 *
	 * @return The interface, or null when no running API offers it.
	 */
	template<typename InterfaceType>
	InterfaceType* GetSteamInterface()
	{
		using FInterfaceTraits = Private::TSteamInterfaceTraits<InterfaceType>;

		if (const FSteamClientInterfaces* ClientInterfaces = GetSteamClientInterfaces())
		{
			if (InterfaceType* ClientInterface = FInterfaceTraits::FromClient(*ClientInterfaces))
			{
				return ClientInterface;
			}
		}

		if (const FSteamServerInterfaces* ServerInterfaces = GetSteamServerInterfaces())
		{
			return FInterfaceTraits::FromServer(*ServerInterfaces);
		}

		return nullptr;
	}

	/** Called by a Steam service once it has resolved its interfaces. */
	STEAMWORKSCOMMON_API void PublishSteamClientInterfaces(const FSteamClientInterfaces* Interfaces);
	STEAMWORKSCOMMON_API void PublishSteamServerInterfaces(const FSteamServerInterfaces* Interfaces);

	/**
	 * Called by a Steam service before it shuts its API down. Withdrawing a set which is not the published
	 * one does nothing: with more than one online services instance alive, the one going away is not
	 * necessarily the one whose interfaces everybody is using.
	 */
	STEAMWORKSCOMMON_API void WithdrawSteamClientInterfaces(const FSteamClientInterfaces* Interfaces);
	STEAMWORKSCOMMON_API void WithdrawSteamServerInterfaces(const FSteamServerInterfaces* Interfaces);
}
