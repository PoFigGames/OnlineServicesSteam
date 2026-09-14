// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"


namespace PoFigGames::Steam
{
	/**
	 * @enum ESteamTransport
	 *
	 * @brief Which of the two current Steam networking APIs carries the game traffic.
	 *
	 * Both relay through the same Steam Datagram Relay and both offer reliable and unreliable delivery;
	 * they differ in what the game has to do about connections. Valve recommends the connection oriented
	 * one for anything which wants to know why a peer went away, and the connectionless one for code
	 * written against UDP which only wants to send to an identity.
	 */
	enum class ESteamTransport : uint8
	{
		/**
		 * ISteamNetworkingSockets: listen sockets, connections and poll groups, with a reason reported
		 * for every connection which fails or closes.
		 */
		Sockets,

		/**
		 * ISteamNetworkingMessages: no listen socket and no connection handle, messages are addressed to
		 * an identity on a channel and sessions are established implicitly.
		 */
		Messages
	};

	STEAMWORKSCOMMON_API const TCHAR* LexToString(ESteamTransport Transport);
	STEAMWORKSCOMMON_API void LexFromString(ESteamTransport& OutTransport, const TCHAR* InString);

	/**
	 * @struct FSteamPlatformConfig
	 *
	 * @brief Settings the Steamworks APIs are brought up with, loaded from the [OnlineServices.Steam] section.
	 */
	struct FSteamPlatformConfig
	{
		/** Whether Steam carries the game traffic at all; without it the engine stays on its own sockets. */
		bool bUseSteamTransport { true };

		/** Whether Steam becomes the socket subsystem the engine reaches for by default. */
		bool bOverrideDefaultSubsystem { true };

		/**
		 * Peers connect to one another without either of them being the one who listens.
		 *
		 * Valve offers this as the answer for connecting two players without assigning the roles of client
		 * and server, in place of the connectionless transport. It applies to ESteamTransport::Sockets;
		 * the connectionless transport has no roles to begin with.
		 */
		bool bUseSymmetricConnect { false };

		/** Which Steam networking API carries the traffic. */
		ESteamTransport Transport { ESteamTransport::Sockets };

		/**
		 * Whether a client also brings the game server API up, so that it can host a listen server.
		 *
		 * A dedicated server always runs the game server API and never this; a client which will only ever
		 * join somebody else's game does not need it either.
		 */
		bool bInitServerOnClient { false };

		/**
		 * Whether a game started outside of Steam relaunches itself through the client.
		 *
		 * Steam only talks to a process it started, so a build run straight from the editor or from a
		 * folder has no API at all unless it is restarted through Steam. Off while developing, on for a
		 * build which is published.
		 */
		bool bRelaunchInSteam { false };

		/**
		 * Whether Steam traffic goes through the Datagram Relay rather than straight to an address.
		 *
		 * One answer for the whole process, host and client alike: a build where this is on is reached by
		 * Steam identity, and a build where it is off is reached by address. Neither can be guessed from
		 * the machine a process happens to run on, and the two sides of a match have to agree.
		 */
		bool bUseRelay { true };

		/** Whether the game server announces itself as VAC secured; clients see this in the server browser. */
		bool bVACEnabled { true };

		/**
		 * Whether the game server sends heartbeats to the Steam master server, which is what puts it in the
		 * server browser and in server list queries.
		 *
		 * A game where a match is found through a lobby rather than through a server list has no use for
		 * that, and a server which is not listed is also not probed by everybody who is browsing.
		 */
		bool bAdvertiseServer { true };

		/**
		 * Name the server browser shows for this server. The command line wins over it, as -ServerName=,
		 * and an empty value leaves the name to whoever reports the match, falling back to ProductName.
		 */
		FString ServerName { };

		/**
		 * Port the Steam master server queries the game server on, for the server browser and the
		 * server list of the game.
		 */
		uint32 GameServerQueryPort { 27015 };

		/**
		 * Steam application id the APIs are initialized for.
		 *
		 * It has to be the id of the app the running build belongs to; Steam refuses to initialize for
		 * anything else, and a development build reads it from steam_appid.txt next to the executable.
		 */
		uint32 SteamAppId { 0 };

		/** Name the game server reports to the Steam master server; defaults to the name of the project. */
		FString ProductName { FApp::GetProjectName() };

		/** Folder name the game server reports, which is how Steam tells one game's servers from another's. */
		FString GameDirectory { TEXT("Server") };

		/**
		 * Version the game server reports.
		 *
		 * Steam compares it against what a client is running to tell whether the two can play together,
		 * so it has to change whenever a build stops being compatible with the previous one.
		 */
		FString GameVersion { FEngineVersion::Current().ToString(EVersionComponent::Changelist) };

		/** Free text the server browser shows next to the server. */
		FString GameDescription { TEXT("Game Description") };
	};
}
