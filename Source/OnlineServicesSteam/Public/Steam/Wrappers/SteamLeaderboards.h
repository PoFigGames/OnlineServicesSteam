// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Steam/SteamCallTraits.h"
#include "Steam/SteamResult.h"
#include "SteamInterfaces.h"


namespace PoFigGames::Steam::Wrappers
{
	/**
	 * @struct FSteamLeaderboardEntry
	 *
	 * @brief One row of a leaderboard, as Steam hands it out.
	 */
	struct FSteamLeaderboardEntry
	{
		CSteamID UserId { k_steamIDNil };

		int32 Rank { 0 };
		int32 Score { 0 };
	};

	/**
	 * @struct FSteamFindLeaderboard
	 *
	 * @brief Resolves the handle Steam knows a leaderboard by, which every other call about it needs.
	 *
	 * A board the configuration describes is created when it is not there yet, because the configuration
	 * says how it would have to be sorted; a board nothing describes is only looked up.
	 */
	struct FSteamFindLeaderboard
	{
		static constexpr TCHAR Name[] = TEXT("SteamFindLeaderboard");

		using SteamCallbackMsgType = LeaderboardFindResult_t;

		struct Params
		{
			FString BoardName { };

			/** How to create the board if it does not exist; unset leaves a missing board missing. */
			TOptional<TTuple<ELeaderboardSortMethod, ELeaderboardDisplayType>> CreateAs { };
		};

		struct Result
		{
			SteamLeaderboard_t BoardHandle { 0 };
		};

		/** Issues the call and returns the handle used to track this request. */
		static SteamAPICall_t Invoke(const Params& In)
		{
			auto Interface = GetSteamInterface<ISteamUserStats>();
			if (Interface == nullptr || In.BoardName.IsEmpty())
			{
				return k_uAPICallInvalid;
			}

			const auto BoardName = StringCast<ANSICHAR>(*In.BoardName);

			if (In.CreateAs.IsSet())
			{
				const auto& [SortMethod, DisplayType] = In.CreateAs.GetValue();
				return Interface->FindOrCreateLeaderboard(BoardName.Get(), SortMethod, DisplayType);
			}

			return Interface->FindLeaderboard(BoardName.Get());
		}

		/** Converts the response; a board Steam does not know is an answer rather than a failure. */
		static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
		{
			if (Message.m_bLeaderboardFound == 0 || Message.m_hSteamLeaderboard == 0)
			{
				return TSteamResultOf<Result>(UE::Online::Errors::NotFound());
			}

			return TSteamResultOf<Result>(Result { .BoardHandle = Message.m_hSteamLeaderboard });
		}
	};

	/**
	 * @struct FSteamDownloadLeaderboardEntries
	 *
	 * @brief Downloads rows of a leaderboard, either a range of it or the rows of named users.
	 *
	 * The rows are read out as the answer is converted: Steam frees them once they have been read, and
	 * the handle they came on is of no use afterwards.
	 */
	struct FSteamDownloadLeaderboardEntries
	{
		static constexpr TCHAR Name[] = TEXT("SteamDownloadLeaderboardEntries");

		using SteamCallbackMsgType = LeaderboardScoresDownloaded_t;

		/** Steam answers for at most this many named users in one request. */
		static constexpr int32 MaxUsersPerRequest { 100 };

		struct Params
		{
			SteamLeaderboard_t BoardHandle { 0 };

			/** Ignored when users are named, which is a request of its own. */
			ELeaderboardDataRequest RequestType { k_ELeaderboardDataRequestGlobal };

			/**
			 * Rows to fetch. A global request counts from one; a request around a user counts from that
			 * user, so a negative start asks for the rows above them.
			 */
			int32 RangeStart { 0 };
			int32 RangeEnd { 0 };

			/** Users to fetch the rows of, instead of a range. */
			TArray<CSteamID> Users { };
		};

		struct Result
		{
			TArray<FSteamLeaderboardEntry> Entries { };
		};

		/** Issues the call and returns the handle used to track this request. */
		static SteamAPICall_t Invoke(const Params& In)
		{
			auto Interface = GetSteamInterface<ISteamUserStats>();
			if (Interface == nullptr || In.BoardHandle == 0)
			{
				return k_uAPICallInvalid;
			}

			if (In.Users.IsEmpty())
			{
				return Interface->DownloadLeaderboardEntries(In.BoardHandle, In.RequestType, In.RangeStart, In.RangeEnd);
			}

			if (In.Users.Num() > MaxUsersPerRequest)
			{
				return k_uAPICallInvalid;
			}

			// Steam takes the users by a mutable pointer, so it is handed a copy of its own.
			TArray<CSteamID> Users = In.Users;
			return Interface->DownloadLeaderboardEntriesForUsers(In.BoardHandle, Users.GetData(), Users.Num());
		}

		/** Reads every downloaded row; an empty answer means nobody asked about is on the board. */
		static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
		{
			auto Interface = GetSteamInterface<ISteamUserStats>();
			if (Interface == nullptr)
			{
				return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
			}

			Result Downloaded { };
			Downloaded.Entries.Reserve(Message.m_cEntryCount);

			for (int32 EntryIndex = 0; EntryIndex < Message.m_cEntryCount; ++EntryIndex)
			{
				LeaderboardEntry_t SteamEntry { };

				if (Interface->GetDownloadedLeaderboardEntry(Message.m_hSteamLeaderboardEntries, EntryIndex, &SteamEntry, nullptr, 0))
				{
					Downloaded.Entries.Emplace(FSteamLeaderboardEntry {
						.UserId = SteamEntry.m_steamIDUser,
						.Rank = SteamEntry.m_nGlobalRank,
						.Score = SteamEntry.m_nScore });
				}
			}

			return TSteamResultOf<Result>(MoveTemp(Downloaded));
		}
	};

	/**
	 * @struct FSteamUploadLeaderboardScore
	 *
	 * @brief Writes the score of the local user onto a leaderboard.
	 */
	struct FSteamUploadLeaderboardScore
	{
		static constexpr TCHAR Name[] = TEXT("SteamUploadLeaderboardScore");

		using SteamCallbackMsgType = LeaderboardScoreUploaded_t;

		struct Params
		{
			SteamLeaderboard_t BoardHandle { 0 };
			ELeaderboardUploadScoreMethod UploadMethod { k_ELeaderboardUploadScoreMethodKeepBest };

			int32 Score { 0 };
		};

		struct Result
		{
			/** False when the score which was already there turned out to be the better one. */
			bool bScoreChanged { false };

			int32 NewRank { 0 };
			int32 PreviousRank { 0 };
		};

		/** Issues the call and returns the handle used to track this request. */
		static SteamAPICall_t Invoke(const Params& In)
		{
			auto Interface = GetSteamInterface<ISteamUserStats>();
			if (Interface == nullptr || In.BoardHandle == 0)
			{
				return k_uAPICallInvalid;
			}

			return Interface->UploadLeaderboardScore(In.BoardHandle, In.UploadMethod, In.Score, nullptr, 0);
		}

		/** Converts the response, which says what became of the score as well as whether it arrived. */
		static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
		{
			if (Message.m_bSuccess == 0)
			{
				return TSteamResultOf<Result>(UE::Online::Errors::RequestFailure());
			}

			return TSteamResultOf<Result>(Result {
				.bScoreChanged = Message.m_bScoreChanged != 0,
				.NewRank = Message.m_nGlobalRankNew,
				.PreviousRank = Message.m_nGlobalRankPrevious });
		}
	};

	static_assert(CSteamCallResultOp<FSteamFindLeaderboard>);
	static_assert(CSteamCallResultOp<FSteamDownloadLeaderboardEntries>);
	static_assert(CSteamCallResultOp<FSteamUploadLeaderboardScore>);
}
