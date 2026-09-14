// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "UObject/NameTypes.h"

THIRD_PARTY_INCLUDES_START
#include "steam/steamclientpublic.h"
THIRD_PARTY_INCLUDES_END

namespace PoFigGames::Steam
{
	/**
	 * Protocol a Steam address reports: a peer reached through the Steam Datagram Relay, or a server
	 * spoken to over IP. These are names of this plugin's own rather than additions to the engine's
	 * FNetworkProtocolTypes, where the SteamShared plugin declares two of exactly the same name.
	 */
	STEAMWORKSCOMMON_API extern const FLazyName SteamRelayProtocol;
	STEAMWORKSCOMMON_API extern const FLazyName SteamIpProtocol;

	/**
	 * Platform user the anonymous game server account is filed under. There is no real local user behind it,
	 * so the index sits far outside the range a controller slot can ever take; it identifies the account and
	 * does not select which kind of login is performed.
	 */
	STEAMWORKSCOMMON_API extern const FPlatformUserId SteamGameServerPlatformId;

	/** Platform user of the signed in Steam user, which is always the first local player. */
	STEAMWORKSCOMMON_API extern const FPlatformUserId SteamClientPlatformId;

	STEAMWORKSCOMMON_API extern const FString SteamClientSocketDescription;
	STEAMWORKSCOMMON_API extern const FString SteamServerSocketDescription;

	STEAMWORKSCOMMON_API extern const FName STEAM_SUBSYSTEMNAME;
}
