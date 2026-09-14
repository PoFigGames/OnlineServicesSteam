// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/LeaderboardsCommon.h"
#include "Online/OnlineComponentSteam.h"
#include "Steam/Wrappers/SteamLeaderboards.h"


namespace PoFigGames::Online
{
	/**
	 * @class FLeaderboardsSteam
	 *
	 * @brief Steam Leaderboards Online Component
	 *
	 * Every call about a leaderboard goes through the handle Steam knows it by, which is resolved by name
	 * once and kept for the rest of the session. A board the configuration describes is created if it is
	 * not there yet, because the configuration says how it would have to be sorted; a board nothing
	 * describes is only looked up, and a missing one is reported as missing.
	 *
	 * Steam reads rows around a user relative to the user running the game and nobody else, so reading
	 * around somebody else is answered by asking for their row first and then for the range around the
	 * rank it came back with.
	 *
	 * Scores are whole numbers of 32 bits on Steam, which is narrower than the interface allows, so a
	 * score is clamped rather than wrapped on its way out.
	 */
	class FLeaderboardsSteam : public TOnlineComponentSteam<UE::Online::FLeaderboardsCommon>
	{
	public:
		using Super = FLeaderboardsCommon;
		using TOnlineComponentSteam::TOnlineComponentSteam;

	#pragma region ILeaderboards
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FReadEntriesForUsers>   ReadEntriesForUsers(UE::Online::FReadEntriesForUsers::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FReadEntriesAroundRank> ReadEntriesAroundRank(UE::Online::FReadEntriesAroundRank::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FReadEntriesAroundUser> ReadEntriesAroundUser(UE::Online::FReadEntriesAroundUser::Params&& Params) override;
	#pragma endregion ILeaderboards

		/** Writes a score onto a board; this is also where a stat which drives one arrives. */
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FWriteLeaderboardScores> WriteLeaderboardScores(UE::Online::FWriteLeaderboardScores::Params&& Params) override;

	private:
		using FBoardHandleResult = UE::Online::TDefaultErrorResultInternal<SteamLeaderboard_t>;
		using FEntriesResult = Steam::TSteamResult<Steam::Wrappers::FSteamDownloadLeaderboardEntries>;

		/** The handle Steam knows a board by, resolved once and remembered for the rest of the session. */
		ONLINESERVICESSTEAM_API TFuture<FBoardHandleResult> ResolveBoardHandle(const FString& BoardName);

		/**
		 * Reads the rows around a user. Steam only knows how to do that for the user running the game, so
		 * for anybody else their row is fetched first and the range is taken around the rank it names.
		 */
		ONLINESERVICESSTEAM_API TFuture<FEntriesResult> DownloadEntriesAroundUser(SteamLeaderboard_t BoardHandle, const CSteamID& TargetSteamId,
			bool bIsLocalUser, int32 Offset, uint32 Limit);

		/** Turns the rows Steam handed out into leaderboard entries, naming every user in them. */
		static ONLINESERVICESSTEAM_API TArray<UE::Online::FLeaderboardEntry> TranslateEntries(const TArray<Steam::Wrappers::FSteamLeaderboardEntry>& SteamEntries);

		/** Handles resolved so far, by the name the game knows the board under. */
		TMap<FString, SteamLeaderboard_t> BoardHandles { };
	};
}
