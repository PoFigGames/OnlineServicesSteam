// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/LeaderboardsSteam.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "Online/OnlineIdSteam.h"
#include "Online/OnlineServicesSteam.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/SteamResult.h"

// Engine
#include "Online/OnlineErrorDefinitions.h"


namespace PoFigGames::Online
{
	namespace Private
	{
		/** Steam counts the rows of a board from one, while the interface counts them from zero. */
		static constexpr int32 FirstSteamRank { 1 };

		/** How a board of the configuration would have to be created, if it turns out not to exist. */
		static TTuple<ELeaderboardSortMethod, ELeaderboardDisplayType> GetCreateInfo(const UE::Online::FLeaderboardDefinition& BoardDefinition)
		{
			// Steam offers a display type as well, for its own community pages; nothing in the definition
			// speaks of one, so a score is presented as the number it is.
			return MakeTuple(BoardDefinition.OrderMethod == UE::Online::ELeaderboardOrderMethod::Ascending
				? k_ELeaderboardSortMethodAscending
				: k_ELeaderboardSortMethodDescending, k_ELeaderboardDisplayTypeNumeric);
		}

		/** How a score of the configuration is folded into the one already on the board. */
		static ELeaderboardUploadScoreMethod GetUploadMethod(const UE::Online::FLeaderboardDefinition* BoardDefinition)
		{
			return BoardDefinition != nullptr && BoardDefinition->UpdateMethod == UE::Online::ELeaderboardUpdateMethod::Force
				? k_ELeaderboardUploadScoreMethodForceUpdate
				: k_ELeaderboardUploadScoreMethodKeepBest;
		}

		/** Steam stores a score as a whole number of 32 bits, which is narrower than the interface allows. */
		static int32 ToSteamScore(const uint64 Score)
		{
			return static_cast<int32>(FMath::Min<uint64>(Score, TNumericLimits<int32>::Max()));
		}
	}

	TFuture<FLeaderboardsSteam::FBoardHandleResult> FLeaderboardsSteam::ResolveBoardHandle(const FString& BoardName)
	{
		if (const auto BoardHandle = BoardHandles.Find(BoardName))
		{
			return MakeFulfilledPromise<FBoardHandleResult>(FBoardHandleResult(*BoardHandle)).GetFuture();
		}

		if (BoardName.IsEmpty())
		{
			return MakeFulfilledPromise<FBoardHandleResult>(FBoardHandleResult(UE::Online::Errors::InvalidParams())).GetFuture();
		}

		Steam::Wrappers::FSteamFindLeaderboard::Params FindParams { .BoardName = BoardName };

		// Only a board the configuration describes can be created, because only the configuration says how
		// it would have to be sorted.
		if (const auto BoardDefinition = LeaderboardDefinitions.Find(BoardName))
		{
			FindParams.CreateAs.Emplace(Private::GetCreateInfo(*BoardDefinition));
		}

		TPromise<FBoardHandleResult> Promise;
		auto Future = Promise.GetFuture();

		SteamCall<Steam::Wrappers::FSteamFindLeaderboard>(MoveTemp(FindParams))
		.Next([this, Promise = MoveTemp(Promise), BoardName](Steam::TSteamResult<Steam::Wrappers::FSteamFindLeaderboard>&& FindResult) mutable
		{
			if (FindResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLeaderboardsSteam] Steam->FindLeaderboard Failed: Board [%s], Result [%s]"),
					*BoardName, *FindResult.GetErrorValue().GetLogString());

				Promise.EmplaceValue(FBoardHandleResult(MoveTemp(FindResult.GetErrorValue())));
				return;
			}

			const auto BoardHandle = FindResult.GetOkValue().BoardHandle;
			BoardHandles.Emplace(BoardName, BoardHandle);

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLeaderboardsSteam] Resolved the leaderboard: Board [%s]"), *BoardName);

			Promise.EmplaceValue(FBoardHandleResult(BoardHandle));
		});

		return Future;
	}

	TFuture<FLeaderboardsSteam::FEntriesResult> FLeaderboardsSteam::DownloadEntriesAroundUser(const SteamLeaderboard_t BoardHandle,
		const CSteamID& TargetSteamId, const bool bIsLocalUser, const int32 Offset, const uint32 Limit)
	{
		const int32 RowCount = FMath::Max(static_cast<int32>(Limit), 1);

		if (bIsLocalUser)
		{
			// Steam counts from the user itself here, so a negative start asks for the rows above them.
			return SteamCall<Steam::Wrappers::FSteamDownloadLeaderboardEntries>({
				.BoardHandle = BoardHandle,
				.RequestType = k_ELeaderboardDataRequestGlobalAroundUser,
				.RangeStart = Offset,
				.RangeEnd = Offset + RowCount - 1 });
		}

		TPromise<FEntriesResult> Promise;
		auto Future = Promise.GetFuture();

		// Where somebody else stands has to be asked first; the range is then an ordinary global one.
		SteamCall<Steam::Wrappers::FSteamDownloadLeaderboardEntries>({ .BoardHandle = BoardHandle, .Users = { TargetSteamId } })
		.Next([this, Promise = MoveTemp(Promise), BoardHandle, Offset, RowCount](FEntriesResult&& RankResult) mutable
		{
			if (RankResult.IsError())
			{
				Promise.EmplaceValue(MoveTemp(RankResult));
				return;
			}

			const auto& Entries = RankResult.GetOkValue().Entries;
			if (Entries.IsEmpty())
			{
				// The user is not on the board, so there is no rank to take a range around.
				Promise.EmplaceValue(FEntriesResult(UE::Online::Errors::NotFound()));
				return;
			}

			const int32 RangeStart = FMath::Max(Entries[0].Rank + Offset, Private::FirstSteamRank);

			SteamCall<Steam::Wrappers::FSteamDownloadLeaderboardEntries>({
				.BoardHandle = BoardHandle,
				.RequestType = k_ELeaderboardDataRequestGlobal,
				.RangeStart = RangeStart,
				.RangeEnd = RangeStart + RowCount - 1 })
			.Next([Promise = MoveTemp(Promise)](FEntriesResult&& EntriesResult) mutable
			{
				Promise.EmplaceValue(MoveTemp(EntriesResult));
			});
		});

		return Future;
	}

	TArray<UE::Online::FLeaderboardEntry> FLeaderboardsSteam::TranslateEntries(const TArray<Steam::Wrappers::FSteamLeaderboardEntry>& SteamEntries)
	{
		TArray<UE::Online::FLeaderboardEntry> Entries;
		Entries.Reserve(SteamEntries.Num());

		for (const auto& SteamEntry : SteamEntries)
		{
			Entries.Emplace(UE::Online::FLeaderboardEntry {
				.AccountId = FindOrAddAccountId(SteamEntry.UserId),
				.Rank = SteamEntry.Rank,
				.Score = static_cast<int64>(SteamEntry.Score) });
		}

		return Entries;
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FReadEntriesForUsers> FLeaderboardsSteam::ReadEntriesForUsers(UE::Online::FReadEntriesForUsers::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FReadEntriesForUsers>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FReadEntriesForUsers>& InAsyncOp)
			{
				const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
				{
					InAsyncOp.SetError(MoveTemp(Error));
					return MakeFulfilledPromise<FBoardHandleResult>(FBoardHandleResult(UE::Online::Errors::Cancelled())).GetFuture();
				};

				const auto& Params = InAsyncOp.GetParams();

				if (auto LocalUser = ResolveLocalSteamUser(Params.LocalAccountId, TEXT("FLeaderboardsSteam::ReadEntriesForUsers")); LocalUser.IsError())
				{
					return FailWith(MoveTemp(LocalUser.GetErrorValue()));
				}

				if (Params.AccountIds.IsEmpty() || Params.AccountIds.Num() > Steam::Wrappers::FSteamDownloadLeaderboardEntries::MaxUsersPerRequest)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLeaderboardsSteam::ReadEntriesForUsers] Failed: Steam answers for one to %d users at a time, and %d were asked about."),
						Steam::Wrappers::FSteamDownloadLeaderboardEntries::MaxUsersPerRequest, Params.AccountIds.Num());

					return FailWith(UE::Online::Errors::InvalidParams());
				}

				return ResolveBoardHandle(Params.BoardName);
			})
			.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FReadEntriesForUsers>& InAsyncOp, FBoardHandleResult&& BoardHandleResult)
			{
				if (BoardHandleResult.IsError())
				{
					InAsyncOp.SetError(MoveTemp(BoardHandleResult.GetErrorValue()));
					return MakeFulfilledPromise<FEntriesResult>(FEntriesResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				TArray<CSteamID> Users;
				Users.Reserve(InAsyncOp.GetParams().AccountIds.Num());

				for (const auto& AccountId : InAsyncOp.GetParams().AccountIds)
				{
					// A user Steam has no identity for is left out rather than failing the whole read; the
					// answer names who is on the board, and they are not.
					if (const CSteamID UserSteamId = GetSteamUserId(AccountId); UserSteamId.IsValid())
					{
						Users.Emplace(UserSteamId);
					}
				}

				if (Users.IsEmpty())
				{
					InAsyncOp.SetError(UE::Online::Errors::NotFound());
					return MakeFulfilledPromise<FEntriesResult>(FEntriesResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				return SteamCall<Steam::Wrappers::FSteamDownloadLeaderboardEntries>({
					.BoardHandle = BoardHandleResult.GetOkValue(), .Users = MoveTemp(Users) });
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamDownloadLeaderboardEntries>(TEXT("FLeaderboardsSteam::ReadEntriesForUsers"),
				[this](UE::Online::TOnlineAsyncOp<UE::Online::FReadEntriesForUsers>& InAsyncOp, Steam::Wrappers::FSteamDownloadLeaderboardEntries::Result&& Result)
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLeaderboardsSteam::ReadEntriesForUsers] Succeeded: Board [%s], Entries [%d]"),
					*InAsyncOp.GetParams().BoardName, Result.Entries.Num());

				InAsyncOp.SetResult({ .Entries = TranslateEntries(Result.Entries) });
			}))
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FReadEntriesAroundRank> FLeaderboardsSteam::ReadEntriesAroundRank(UE::Online::FReadEntriesAroundRank::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FReadEntriesAroundRank>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FReadEntriesAroundRank>& InAsyncOp)
			{
				const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
				{
					InAsyncOp.SetError(MoveTemp(Error));
					return MakeFulfilledPromise<FBoardHandleResult>(FBoardHandleResult(UE::Online::Errors::Cancelled())).GetFuture();
				};

				const auto& Params = InAsyncOp.GetParams();

				if (auto LocalUser = ResolveLocalSteamUser(Params.LocalAccountId, TEXT("FLeaderboardsSteam::ReadEntriesAroundRank")); LocalUser.IsError())
				{
					return FailWith(MoveTemp(LocalUser.GetErrorValue()));
				}

				if (Params.Limit == 0u)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLeaderboardsSteam::ReadEntriesAroundRank] Failed: No entries asked for."));
					return FailWith(UE::Online::Errors::InvalidParams());
				}

				return ResolveBoardHandle(Params.BoardName);
			})
			.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FReadEntriesAroundRank>& InAsyncOp, FBoardHandleResult&& BoardHandleResult)
			{
				if (BoardHandleResult.IsError())
				{
					InAsyncOp.SetError(MoveTemp(BoardHandleResult.GetErrorValue()));
					return MakeFulfilledPromise<FEntriesResult>(FEntriesResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				const auto& Params = InAsyncOp.GetParams();

				// The interface counts the rows of a board from zero and Steam counts them from one.
				const auto RangeStart = static_cast<int32>(Params.Rank) + Private::FirstSteamRank;

				return SteamCall<Steam::Wrappers::FSteamDownloadLeaderboardEntries>({
					.BoardHandle = BoardHandleResult.GetOkValue(),
					.RequestType = k_ELeaderboardDataRequestGlobal,
					.RangeStart = RangeStart,
					.RangeEnd = RangeStart + static_cast<int32>(Params.Limit) - 1 });
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamDownloadLeaderboardEntries>(TEXT("FLeaderboardsSteam::ReadEntriesAroundRank"),
				[this](UE::Online::TOnlineAsyncOp<UE::Online::FReadEntriesAroundRank>& InAsyncOp, Steam::Wrappers::FSteamDownloadLeaderboardEntries::Result&& Result)
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLeaderboardsSteam::ReadEntriesAroundRank] Succeeded: Board [%s], Entries [%d]"),
					*InAsyncOp.GetParams().BoardName, Result.Entries.Num());

				InAsyncOp.SetResult({ .Entries = TranslateEntries(Result.Entries) });
			}))
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FReadEntriesAroundUser> FLeaderboardsSteam::ReadEntriesAroundUser(UE::Online::FReadEntriesAroundUser::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FReadEntriesAroundUser>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FReadEntriesAroundUser>& InAsyncOp)
			{
				const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
				{
					InAsyncOp.SetError(MoveTemp(Error));
					return MakeFulfilledPromise<FBoardHandleResult>(FBoardHandleResult(UE::Online::Errors::Cancelled())).GetFuture();
				};

				const auto& Params = InAsyncOp.GetParams();

				auto LocalUser = ResolveLocalSteamUser(Params.LocalAccountId, TEXT("FLeaderboardsSteam::ReadEntriesAroundUser"));
				if (LocalUser.IsError())
				{
					return FailWith(MoveTemp(LocalUser.GetErrorValue()));
				}

				if (Params.Limit == 0u)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLeaderboardsSteam::ReadEntriesAroundUser] Failed: No entries asked for."));
					return FailWith(UE::Online::Errors::InvalidParams());
				}

				const CSteamID TargetSteamId = GetSteamUserId(Params.AccountId);
				if (!TargetSteamId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLeaderboardsSteam::ReadEntriesAroundUser] Failed: No associated steam id found. User [%s]"), *ToLogString(Params.AccountId));
					return FailWith(UE::Online::Errors::NotFound());
				}

				InAsyncOp.Data.Set<CSteamID>(TEXT("TargetSteamId"), TargetSteamId);
				InAsyncOp.Data.Set<bool>(TEXT("bIsLocalUser"), TargetSteamId == LocalUser.GetOkValue());

				return ResolveBoardHandle(Params.BoardName);
			})
			.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FReadEntriesAroundUser>& InAsyncOp, FBoardHandleResult&& BoardHandleResult)
			{
				if (BoardHandleResult.IsError())
				{
					InAsyncOp.SetError(MoveTemp(BoardHandleResult.GetErrorValue()));
					return MakeFulfilledPromise<FEntriesResult>(FEntriesResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				const auto& Params = InAsyncOp.GetParams();
				const auto TargetSteamId = InAsyncOp.Data.Get<CSteamID>(TEXT("TargetSteamId"));
				const auto bIsLocalUser = InAsyncOp.Data.Get<bool>(TEXT("bIsLocalUser"));

				if (TargetSteamId == nullptr || bIsLocalUser == nullptr)
				{
					InAsyncOp.SetError(UE::Online::Errors::InvalidState());
					return MakeFulfilledPromise<FEntriesResult>(FEntriesResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				return DownloadEntriesAroundUser(BoardHandleResult.GetOkValue(), *TargetSteamId, *bIsLocalUser, Params.Offset, Params.Limit);
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamDownloadLeaderboardEntries>(TEXT("FLeaderboardsSteam::ReadEntriesAroundUser"),
				[this](UE::Online::TOnlineAsyncOp<UE::Online::FReadEntriesAroundUser>& InAsyncOp, Steam::Wrappers::FSteamDownloadLeaderboardEntries::Result&& Result)
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLeaderboardsSteam::ReadEntriesAroundUser] Succeeded: Board [%s], User [%s], Entries [%d]"),
					*InAsyncOp.GetParams().BoardName, *ToLogString(InAsyncOp.GetParams().AccountId), Result.Entries.Num());

				InAsyncOp.SetResult({ .Entries = TranslateEntries(Result.Entries) });
			}))
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FWriteLeaderboardScores> FLeaderboardsSteam::WriteLeaderboardScores(UE::Online::FWriteLeaderboardScores::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FWriteLeaderboardScores>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FWriteLeaderboardScores>& InAsyncOp)
		{
			const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
			{
				InAsyncOp.SetError(MoveTemp(Error));
				return MakeFulfilledPromise<FBoardHandleResult>(FBoardHandleResult(UE::Online::Errors::Cancelled())).GetFuture();
			};

			const auto& Params = InAsyncOp.GetParams();

			// Steam writes a score for the user running the game and nobody else.
			if (auto LocalUser = ResolveLocalSteamUser(Params.LocalAccountId, TEXT("FLeaderboardsSteam::WriteLeaderboardScores")); LocalUser.IsError())
			{
				return FailWith(MoveTemp(LocalUser.GetErrorValue()));
			}

			return ResolveBoardHandle(Params.BoardName);
		})
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FWriteLeaderboardScores>& InAsyncOp, FBoardHandleResult&& BoardHandleResult)
		{
			using FUploadResult = Steam::TSteamResult<Steam::Wrappers::FSteamUploadLeaderboardScore>;

			if (BoardHandleResult.IsError())
			{
				InAsyncOp.SetError(MoveTemp(BoardHandleResult.GetErrorValue()));
				return MakeFulfilledPromise<FUploadResult>(FUploadResult(UE::Online::Errors::Cancelled())).GetFuture();
			}

			const auto& Params = InAsyncOp.GetParams();

			return SteamCall<Steam::Wrappers::FSteamUploadLeaderboardScore>({
				.BoardHandle = BoardHandleResult.GetOkValue(),
				.UploadMethod = Private::GetUploadMethod(LeaderboardDefinitions.Find(Params.BoardName)),
				.Score = Private::ToSteamScore(Params.Score) });
		})
		.Then(Steam::Unwrap<Steam::Wrappers::FSteamUploadLeaderboardScore>(TEXT("FLeaderboardsSteam::WriteLeaderboardScores"),
			[this](UE::Online::TOnlineAsyncOp<UE::Online::FWriteLeaderboardScores>& InAsyncOp, Steam::Wrappers::FSteamUploadLeaderboardScore::Result&& Result)
		{
			const auto& Params = InAsyncOp.GetParams();

			// A score which changed nothing is not a failure: a board which keeps the best one says so by
			// leaving what was already there.
			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLeaderboardsSteam::WriteLeaderboardScores] Succeeded: Board [%s], Score [%llu], Changed [%s], Rank [%d -> %d]"),
				*Params.BoardName, Params.Score, *LexToString(Result.bScoreChanged), Result.PreviousRank, Result.NewRank);

			InAsyncOp.SetResult({ });
		}))
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}
}
