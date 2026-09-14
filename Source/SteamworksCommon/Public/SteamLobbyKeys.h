// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"


namespace PoFigGames::Steam
{
	/**
	 * Lobby metadata keys the Steam plugin owns rather than the game.
	 *
	 * Steam offers no way to read the join policy of a lobby back, and no way at all to look at the
	 * members of a lobby the local user has not joined, so both are published as ordinary metadata by
	 * whoever owns the lobby. The game server keys mirror what SetLobbyGameServer stores, so that a
	 * travel address arrives through the same attribute path as everything else.
	 *
	 * Every key here is also declared as a service attribute of the lobby base schema under the very
	 * same name, which is what lets a value be written straight to Steam and still be read back as an
	 * ordinary lobby attribute. They are named after the keys Steam fills in itself.
	 *
	 * The names live here rather than with the lobby component so that the socket subsystem, which
	 * publishes the address of the game server it hosts, can name the same attributes without knowing
	 * anything about the implementation of the online services.
	 */
	namespace LobbyKeys
	{
		/** The ELobbyType the owner last applied, as a decimal number. */
		inline constexpr TCHAR JoinPolicy[] { TEXT("__joinPolicy") };

		/** How many members the lobby holds right now, as a decimal number. */
		inline constexpr TCHAR MemberCount[] { TEXT("__memberCount") };

		/** Set by a host which is tearing the lobby down, for the members that are still in it. */
		inline constexpr TCHAR BeingDestroyed[] { TEXT("__beingDestroyed") };

		/** Build of the game the host is running; see GetSteamAppBuildId. */
		inline constexpr TCHAR BuildId[] { TEXT("__buildId") };

		/** How a boolean reads once written; the same text the schema translation produces. */
		inline constexpr TCHAR True[] { TEXT("true") };
	}
}
