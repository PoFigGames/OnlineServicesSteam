// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineError.h"
#include "SteamUtils.h"


namespace PoFigGames::Online::Errors
{
	UE_ONLINE_ERROR_CATEGORY(STEAM, ThirdPartyPlugin, 0x6, "STEAM")

	using FErrorMapperSteamFn = TFunction<UE::Online::FOnlineError(UE::Online::FOnlineError&& Error, EResult Result)>;

	ONLINESERVICESSTEAM_API UE::Online::FOnlineError MapCommonSteamError(UE::Online::FOnlineError&& Error, EResult Result);
	ONLINESERVICESSTEAM_API UE::Online::Errors::ErrorCodeType ErrorCodeFromSteamResult(EResult Result);
	ONLINESERVICESSTEAM_API UE::Online::FOnlineError FromSteamResult(EResult Result, FErrorMapperSteamFn&& MapperFn = &MapCommonSteamError);
}

// Declared outside the namespace so that the test macros can find them.
inline bool operator==(const UE::Online::FOnlineError& OnlineError, const EResult Result)
{
	return OnlineError == PoFigGames::Online::Errors::ErrorCodeFromSteamResult(Result);
}

inline bool operator==(const EResult Result, const UE::Online::FOnlineError& OnlineError)
{
	return OnlineError == Result;
}

inline bool operator!=(const UE::Online::FOnlineError& OnlineError, const EResult Result)
{
	return !(OnlineError == Result);
}

inline bool operator!=(const EResult Result, const UE::Online::FOnlineError& OnlineError)
{
	return OnlineError != Result;
}
