// Copyright PoFig Games Studio. All Rights Reserved.


#include "SteamCommon.h"


namespace PoFigGames::Steam
{
	const FLazyName SteamRelayProtocol { TEXT("SteamRelay") };
	const FLazyName SteamIpProtocol { TEXT("SteamIp") };

	/** Far outside the range of real local players, so it can never collide with a controller slot. */
	static constexpr int32 GameServerPlatformUserIndex { 0xFFF };

	const FPlatformUserId SteamGameServerPlatformId { FPlatformMisc::GetPlatformUserForUserIndex(GameServerPlatformUserIndex) };
	const FPlatformUserId SteamClientPlatformId { FPlatformMisc::GetPlatformUserForUserIndex(0x0) };

	const FString SteamClientSocketDescription { TEXT("Steam relay client") };
	const FString SteamServerSocketDescription { TEXT("Steam relay server") };

	const FName STEAM_SUBSYSTEMNAME { TEXT("STEAM") };
}
