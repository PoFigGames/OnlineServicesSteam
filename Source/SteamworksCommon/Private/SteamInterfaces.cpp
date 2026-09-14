// Copyright PoFig Games Studio. All Rights Reserved.

#include "SteamInterfaces.h"


namespace PoFigGames::Steam
{
	namespace Private
	{
		/**
		 * The Steamworks API is process wide, so one published set per API is the whole truth. The sets are
		 * owned by the services that resolved them and withdrawn before those services shut the API down.
		 */
		static const FSteamClientInterfaces* PublishedClientInterfaces { nullptr };
		static const FSteamServerInterfaces* PublishedServerInterfaces { nullptr };
	}

	bool FSteamClientInterfaces::Resolve()
	{
		bool bResolved = true;

		bResolved &= STEAM_RESOLVE_INTERFACE(User, SteamUser);
		bResolved &= STEAM_RESOLVE_INTERFACE(Friends, SteamFriends);
		bResolved &= STEAM_RESOLVE_INTERFACE(Utils, SteamUtils);
		bResolved &= STEAM_RESOLVE_INTERFACE(Apps, SteamApps);
		bResolved &= STEAM_RESOLVE_INTERFACE(Matchmaking, SteamMatchmaking);
		bResolved &= STEAM_RESOLVE_INTERFACE(UserStats, SteamUserStats);
		bResolved &= STEAM_RESOLVE_INTERFACE(RemoteStorage, SteamRemoteStorage);
		bResolved &= STEAM_RESOLVE_INTERFACE(NetworkingUtils, SteamNetworkingUtils);
		bResolved &= STEAM_RESOLVE_INTERFACE(NetworkingSockets, SteamNetworkingSockets);
		bResolved &= STEAM_RESOLVE_OPTIONAL_INTERFACE(NetworkingMessages, SteamNetworkingMessages);
		bResolved &= STEAM_RESOLVE_OPTIONAL_INTERFACE(ParentalSettings, SteamParentalSettings);

		return bResolved;
	}

	bool FSteamServerInterfaces::Resolve()
	{
		bool bResolved = true;

		bResolved &= STEAM_RESOLVE_INTERFACE(GameServer, SteamGameServer);
		bResolved &= STEAM_RESOLVE_INTERFACE(Utils, SteamGameServerUtils);
		bResolved &= STEAM_RESOLVE_INTERFACE(NetworkingUtils, SteamNetworkingUtils);
		bResolved &= STEAM_RESOLVE_INTERFACE(NetworkingSockets, SteamGameServerNetworkingSockets);
		bResolved &= STEAM_RESOLVE_OPTIONAL_INTERFACE(NetworkingMessages, SteamGameServerNetworkingMessages);

		return bResolved;
	}

	const FSteamClientInterfaces* GetSteamClientInterfaces()
	{
		return Private::PublishedClientInterfaces;
	}

	const FSteamServerInterfaces* GetSteamServerInterfaces()
	{
		return Private::PublishedServerInterfaces;
	}

	void PublishSteamClientInterfaces(const FSteamClientInterfaces* Interfaces)
	{
		check(IsInGameThread());
		check(Interfaces);

		if (Private::PublishedClientInterfaces != nullptr && Private::PublishedClientInterfaces != Interfaces)
		{
			// The Steamworks API is process wide, so a second client service means a second initialization of
			// the same API: the interfaces are the same objects, only the set describing them differs.
			UE_LOG(LogSteamService, Warning, TEXT("Steam client interfaces are published more than once; the newest set is used"));
		}

		Private::PublishedClientInterfaces = Interfaces;
	}

	void PublishSteamServerInterfaces(const FSteamServerInterfaces* Interfaces)
	{
		check(IsInGameThread());
		check(Interfaces);

		if (Private::PublishedServerInterfaces != nullptr && Private::PublishedServerInterfaces != Interfaces)
		{
			UE_LOG(LogSteamService, Warning, TEXT("Steam game server interfaces are published more than once; the newest set is used"));
		}

		Private::PublishedServerInterfaces = Interfaces;
	}

	void WithdrawSteamClientInterfaces(const FSteamClientInterfaces* Interfaces)
	{
		check(IsInGameThread());

		if (Private::PublishedClientInterfaces == Interfaces)
		{
			Private::PublishedClientInterfaces = nullptr;
		}
	}

	void WithdrawSteamServerInterfaces(const FSteamServerInterfaces* Interfaces)
	{
		check(IsInGameThread());

		if (Private::PublishedServerInterfaces == Interfaces)
		{
			Private::PublishedServerInterfaces = nullptr;
		}
	}
}
