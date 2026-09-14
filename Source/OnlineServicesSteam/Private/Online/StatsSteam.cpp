// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/StatsSteam.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamInterfaces.h"
#include "Online/OnlineIdSteam.h"
#include "Online/OnlineServicesSteam.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/SteamResult.h"
#include "Steam/Wrappers/SteamUserStats.h"

// Engine
#include "Online/OnlineErrorDefinitions.h"


namespace PoFigGames::Online
{
	namespace Private
	{
		/** Where the stats of one user wait between the step which read them and the one which answers. */
		static FString MakeUserStatsKey(const UE::Online::FAccountId& TargetAccountId)
		{
			return FString::Printf(TEXT("UserStats_%s"), *ToLogString(TargetAccountId));
		}

		/** Steam stores a whole number as int32 and everything else as float, so a value is clamped into one of the two. */
		static int32 ToSteamInt(const UE::Online::FStatValue& StatValue)
		{
			switch (StatValue.GetType())
			{
				case UE::Online::ESchemaAttributeType::Bool:
					return StatValue.GetBoolean() ? 1 : 0;
				case UE::Online::ESchemaAttributeType::Double:
					return static_cast<int32>(FMath::Clamp(StatValue.GetDouble(),
						static_cast<double>(TNumericLimits<int32>::Min()), static_cast<double>(TNumericLimits<int32>::Max())));
				default:
					break;
			}

			return static_cast<int32>(FMath::Clamp(StatValue.GetInt64(),
				static_cast<int64>(TNumericLimits<int32>::Min()), static_cast<int64>(TNumericLimits<int32>::Max())));
		}

		static float ToSteamFloat(const UE::Online::FStatValue& StatValue)
		{
			switch (StatValue.GetType())
			{
				case UE::Online::ESchemaAttributeType::Bool:
					return StatValue.GetBoolean() ? 1.0f : 0.0f;
				case UE::Online::ESchemaAttributeType::Int64:
					return static_cast<float>(StatValue.GetInt64());
				default:
					break;
			}

			return static_cast<float>(StatValue.GetDouble());
		}

		/** Turns a value Steam answered with back into the type the stat is declared as. */
		static UE::Online::FStatValue FromSteamInt(const UE::Online::ESchemaAttributeType StatType, const int32 SteamValue)
		{
			switch (StatType)
			{
				case UE::Online::ESchemaAttributeType::Bool:
					return UE::Online::FStatValue(SteamValue != 0);
				case UE::Online::ESchemaAttributeType::Double:
					return UE::Online::FStatValue(static_cast<double>(SteamValue));
				default:
					break;
			}

			return UE::Online::FStatValue(static_cast<int64>(SteamValue));
		}

		/** Whether a stat of this type lives in Steam as a floating point number rather than a whole one. */
		static bool IsFloatingPointStat(const UE::Online::ESchemaAttributeType StatType)
		{
			return StatType == UE::Online::ESchemaAttributeType::Double;
		}

		/**
		 * Folds an update into the value which is already there.
		 *
		 * Steam aggregates nothing on its side: SetStat stores exactly what it is given, so the rule the
		 * configuration names is applied here, where the previous value is the one Steam holds.
		 */
		static UE::Online::FStatValue ApplyModifyMethod(const UE::Online::EStatModifyMethod ModifyMethod,
			const UE::Online::FStatValue& CurrentValue, const UE::Online::FStatValue& UpdateValue)
		{
			if (ModifyMethod == UE::Online::EStatModifyMethod::Set)
			{
				return UpdateValue;
			}

			if (IsFloatingPointStat(CurrentValue.GetType()))
			{
				const double Current = CurrentValue.GetDouble();
				const double Update = UpdateValue.GetDouble();

				switch (ModifyMethod)
				{
					case UE::Online::EStatModifyMethod::Sum:
						return UE::Online::FStatValue(Current + Update);
					case UE::Online::EStatModifyMethod::Largest:
						return UE::Online::FStatValue(FMath::Max(Current, Update));
					case UE::Online::EStatModifyMethod::Smallest:
						return UE::Online::FStatValue(FMath::Min(Current, Update));
					default:
						break;
				}

				return UpdateValue;
			}

			const int64 Current = CurrentValue.GetInt64();
			const int64 Update = UpdateValue.GetInt64();

			switch (ModifyMethod)
			{
				case UE::Online::EStatModifyMethod::Sum:
					return UE::Online::FStatValue(Current + Update);
				case UE::Online::EStatModifyMethod::Largest:
					return UE::Online::FStatValue(FMath::Max(Current, Update));
				case UE::Online::EStatModifyMethod::Smallest:
					return UE::Online::FStatValue(FMath::Min(Current, Update));
				default:
					break;
			}

			return UpdateValue;
		}
	}

	UE::Online::FUserStats FStatsSteam::ReadUserStats(const UE::Online::FAccountId& TargetAccountId, const CSteamID& TargetSteamId,
		const TArray<FString>& StatNames) const
	{
		UE::Online::FUserStats UserStats { .AccountId = TargetAccountId };

		const auto SteamUserStats = Steam::GetSteamInterface<ISteamUserStats>();
		if (SteamUserStats == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::ReadUserStats] Failed: Steam user stats interface is not available"));
			return UserStats;
		}

		UserStats.Stats.Reserve(StatNames.Num());

		for (const auto& StatName : StatNames)
		{
			const auto StatDefinition = GetStatDefinition(StatName);
			if (StatDefinition == nullptr)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::ReadUserStats] Skipping a stat the configuration does not describe: Stat [%s]"), *StatName);
				continue;
			}

			const auto SteamStatName = StringCast<ANSICHAR>(*StatName);
			const auto StatType = StatDefinition->DefaultValue.GetType();

			if (Private::IsFloatingPointStat(StatType))
			{
				float SteamValue { 0.0f };
				if (SteamUserStats->GetUserStat(TargetSteamId, SteamStatName.Get(), &SteamValue))
				{
					UserStats.Stats.Emplace(StatName, UE::Online::FStatValue(static_cast<double>(SteamValue)));
					continue;
				}
			}
			else
			{
				int32 SteamValue { 0 };
				if (SteamUserStats->GetUserStat(TargetSteamId, SteamStatName.Get(), &SteamValue))
				{
					UserStats.Stats.Emplace(StatName, Private::FromSteamInt(StatType, SteamValue));
					continue;
				}
			}

			// Steam refuses a stat it has never stored for this user, and equally one the backend of the
			// title does not declare or declares as the other type. The configured default stands in for
			// the first, which is the same answer the common implementation gives from its cache; the rest
			// are a mismatch between the configuration and the backend, and read the same way.
			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FStatsSteam::ReadUserStats] Steam answered no value, using the configured default: User [%s], Stat [%s]"),
				*ToLogString(TargetAccountId), *StatName);

			UserStats.Stats.Emplace(StatName, StatDefinition->DefaultValue);
		}

		return UserStats;
	}

	UE::Online::TDefaultErrorResultInternal<UE::Online::FUserStats> FStatsSteam::WriteUserStats(const UE::Online::FUserStats& UpdateUserStats) const
	{
		using FWriteResult = UE::Online::TDefaultErrorResultInternal<UE::Online::FUserStats>;

		const auto SteamUserStats = Steam::GetSteamInterface<ISteamUserStats>();
		if (SteamUserStats == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::UpdateStats] Failed: Steam user stats interface is not available"));
			return FWriteResult(UE::Online::Errors::MissingInterface());
		}

		UE::Online::FUserStats WrittenUserStats { .AccountId = UpdateUserStats.AccountId };
		WrittenUserStats.Stats.Reserve(UpdateUserStats.Stats.Num());

		for (const auto& UpdatedStat : UpdateUserStats.Stats)
		{
			const auto StatDefinition = GetStatDefinition(UpdatedStat.Key);
			if (StatDefinition == nullptr)
			{
				// Steam knows the stats of the title from its backend and refuses everything else, so a
				// name the configuration does not describe is a mistake rather than a new stat.
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::UpdateStats] Failed: The configuration describes no stat [%s]."), *UpdatedStat.Key);
				return FWriteResult(UE::Online::Errors::InvalidParams());
			}

			const auto SteamStatName = StringCast<ANSICHAR>(*UpdatedStat.Key);
			const auto StatType = StatDefinition->DefaultValue.GetType();

			if (StatType == UE::Online::ESchemaAttributeType::String)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::UpdateStats] Failed: Steam stores no text, so stat [%s] cannot be written."), *UpdatedStat.Key);
				return FWriteResult(UE::Online::Errors::InvalidParams());
			}

			if (Private::IsFloatingPointStat(StatType))
			{
				float CurrentValue { 0.0f };
				SteamUserStats->GetStat(SteamStatName.Get(), &CurrentValue);

				const auto NewValue = Private::ApplyModifyMethod(StatDefinition->ModifyMethod,
					UE::Online::FStatValue(static_cast<double>(CurrentValue)), UpdatedStat.Value);

				if (!SteamUserStats->SetStat(SteamStatName.Get(), Private::ToSteamFloat(NewValue)))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::UpdateStats] Steam->SetStat Failed: Stat [%s]"), *UpdatedStat.Key);
					return FWriteResult(UE::Online::Errors::RequestFailure());
				}

				WrittenUserStats.Stats.Emplace(UpdatedStat.Key, NewValue);
				continue;
			}

			int32 CurrentValue { 0 };
			SteamUserStats->GetStat(SteamStatName.Get(), &CurrentValue);

			const auto NewValue = Private::ApplyModifyMethod(StatDefinition->ModifyMethod,
				Private::FromSteamInt(StatType, CurrentValue), UpdatedStat.Value);

			if (!SteamUserStats->SetStat(SteamStatName.Get(), Private::ToSteamInt(NewValue)))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::UpdateStats] Steam->SetStat Failed: Stat [%s]"), *UpdatedStat.Key);
				return FWriteResult(UE::Online::Errors::RequestFailure());
			}

			WrittenUserStats.Stats.Emplace(UpdatedStat.Key, NewValue);
		}

		return FWriteResult(MoveTemp(WrittenUserStats));
	}

	UE::Online::TOnlineAsyncOpRef<UE::Online::FUpdateStats> FStatsSteam::UpdateStats_Implementation(UE::Online::FUpdateStats::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FUpdateStats>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUpdateStats>& InAsyncOp)
		{
			using FUserStatsResult = Steam::TSteamResult<Steam::Wrappers::FSteamUserStats>;

			const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
			{
				InAsyncOp.SetError(MoveTemp(Error));
				return MakeFulfilledPromise<FUserStatsResult>(FUserStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
			};

			const auto& Params = InAsyncOp.GetParams();

			auto LocalUser = ResolveSteamUser(Params.LocalAccountId, Params.LocalAccountId, TEXT("FStatsSteam::UpdateStats"));
			if (LocalUser.IsError())
			{
				return FailWith(MoveTemp(LocalUser.GetErrorValue()));
			}

			for (const auto& UpdateUserStats : Params.UpdateUsersStats)
			{
				// Writing the stats of another user is what a game server does through an interface of its
				// own; a client may only write its own.
				if (UpdateUserStats.AccountId != Params.LocalAccountId)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::UpdateStats] Failed: A client can only write the stats of the local user. User [%s]"),
						*ToLogString(UpdateUserStats.AccountId));

					return FailWith(UE::Online::Errors::InvalidParams());
				}
			}

			// The value an update is folded into is the one Steam holds, so the stats have to be there
			// before anything is written.
			return SteamCall<Steam::Wrappers::FSteamUserStats>({ .UserId = LocalUser.GetOkValue() });
		})
		// Unwrapping by hand rather than through Steam::Unwrap, because this step has an answer of its own
		// to hand on to the next one.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUpdateStats>& InAsyncOp, Steam::TSteamResult<Steam::Wrappers::FSteamUserStats>&& UserStatsResult)
		{
			using FStoreStatsResult = Steam::TSteamResult<Steam::Wrappers::FSteamStoreStats>;

			if (UserStatsResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::UpdateStats] Steam->RequestUserStats Failed: User [%s], Result [%s]"),
					*ToLogString(InAsyncOp.GetParams().LocalAccountId), *UserStatsResult.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(UserStatsResult.GetErrorValue()));
				return MakeFulfilledPromise<FStoreStatsResult>(FStoreStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
			}

			TArray<UE::Online::FUserStats> WrittenUsersStats;
			WrittenUsersStats.Reserve(InAsyncOp.GetParams().UpdateUsersStats.Num());

			for (const auto& UpdateUserStats : InAsyncOp.GetParams().UpdateUsersStats)
			{
				auto WriteResult = WriteUserStats(UpdateUserStats);
				if (WriteResult.IsError())
				{
					InAsyncOp.SetError(MoveTemp(WriteResult.GetErrorValue()));
					return MakeFulfilledPromise<FStoreStatsResult>(FStoreStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				WrittenUsersStats.Emplace(MoveTemp(WriteResult.GetOkValue()));
			}

			// Kept for the step which follows, which is where the values become the ones the game sees.
			InAsyncOp.Data.Set<TArray<UE::Online::FUserStats>>(TEXT("WrittenUsersStats"), MoveTemp(WrittenUsersStats));

			return SteamListen<Steam::Wrappers::FSteamStoreStats>({ });
		})
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUpdateStats>& InAsyncOp, Steam::TSteamResult<Steam::Wrappers::FSteamStoreStats>&& StoreResult)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto WrittenUsersStats = InAsyncOp.Data.Get<TArray<UE::Online::FUserStats>>(TEXT("WrittenUsersStats"));

			if (StoreResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::UpdateStats] Steam->StoreStats Failed: User [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *StoreResult.GetErrorValue().GetLogString());

				// A rejected commit means Steam sent the stored values back, so what it now holds is read
				// again rather than left as the values it refused.
				if (WrittenUsersStats != nullptr)
				{
					if (auto LocalUser = ResolveSteamUser(Params.LocalAccountId, Params.LocalAccountId, TEXT("FStatsSteam::UpdateStats")); LocalUser.IsOk())
					{
						for (const auto& WrittenUserStats : *WrittenUsersStats)
						{
							TArray<FString> StatNames;
							WrittenUserStats.Stats.GenerateKeyArray(StatNames);

							CacheUserStats(ReadUserStats(Params.LocalAccountId, LocalUser.GetOkValue(), StatNames));
						}
					}
				}

				InAsyncOp.SetError(MoveTemp(StoreResult.GetErrorValue()));
				return;
			}

			if (WrittenUsersStats != nullptr)
			{
				for (const auto& WrittenUserStats : *WrittenUsersStats)
				{
					CacheUserStats(WrittenUserStats);
				}
			}

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FStatsSteam::UpdateStats] Succeeded: User [%s], Users [%d]"),
				*ToLogString(Params.LocalAccountId), Params.UpdateUsersStats.Num());

			InAsyncOp.SetResult({ });

			// What the game is told is what Steam now holds, which is not always what it was asked to
			// store: a summed stat comes back as its total rather than as the amount added to it.
			OnStatsUpdatedEvent.Broadcast(WrittenUsersStats != nullptr
				? UE::Online::FStatsUpdated { .LocalAccountId = Params.LocalAccountId, .UpdateUsersStats = *WrittenUsersStats }
				: Params);
		})
		.Enqueue(GetSerialQueue());

		return Op;
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryStats> FStatsSteam::QueryStats(UE::Online::FQueryStats::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FQueryStats>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryStats>& InAsyncOp)
			{
				using FUserStatsResult = Steam::TSteamResult<Steam::Wrappers::FSteamUserStats>;

				const auto& Params = InAsyncOp.GetParams();

				auto TargetUser = ResolveSteamUser(Params.LocalAccountId, Params.TargetAccountId, TEXT("FStatsSteam::QueryStats"));
				if (TargetUser.IsError())
				{
					InAsyncOp.SetError(MoveTemp(TargetUser.GetErrorValue()));
					return MakeFulfilledPromise<FUserStatsResult>(FUserStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				InAsyncOp.Data.Set<CSteamID>(TEXT("TargetSteamId"), TargetUser.GetOkValue());

				// Stats of another user are not kept up to date, so they are asked for every time rather
				// than read out of whatever an earlier query left behind.
				return SteamCall<Steam::Wrappers::FSteamUserStats>({ .UserId = TargetUser.GetOkValue() });
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamUserStats>(TEXT("FStatsSteam::QueryStats"),
				[this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryStats>& InAsyncOp, Steam::Wrappers::FSteamUserStats::Result&& /*Result*/)
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto TargetSteamId = InAsyncOp.Data.Get<CSteamID>(TEXT("TargetSteamId"));

				if (TargetSteamId == nullptr)
				{
					InAsyncOp.SetError(UE::Online::Errors::InvalidState());
					return;
				}

				auto UserStats = ReadUserStats(Params.TargetAccountId, *TargetSteamId, Params.StatNames);

				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FStatsSteam::QueryStats] Succeeded: User [%s], Stats [%d]"),
					*ToLogString(Params.TargetAccountId), UserStats.Stats.Num());

				CacheUserStats(UserStats);

				InAsyncOp.SetResult({ .Stats = MoveTemp(UserStats.Stats) });
			}))
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FBatchQueryStats> FStatsSteam::BatchQueryStats(UE::Online::FBatchQueryStats::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FBatchQueryStats>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FBatchQueryStats>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			if (auto LocalUser = ResolveSteamUser(Params.LocalAccountId, Params.LocalAccountId, TEXT("FStatsSteam::BatchQueryStats")); LocalUser.IsError())
			{
				return InAsyncOp.SetError(MoveTemp(LocalUser.GetErrorValue()));
			}

			if (Params.TargetAccountIds.IsEmpty())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::BatchQueryStats] Failed: No users to query."));
				return InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
			}
		});

		// Steam answers for one user at a time, so the targets are asked for in turn; they are known before
		// the operation runs, which is why every request is chained up front.
		for (const auto& TargetAccountId : Op->GetParams().TargetAccountIds)
		{
			Op->Then([this, TargetAccountId](UE::Online::TOnlineAsyncOp<UE::Online::FBatchQueryStats>& InAsyncOp)
			{
				using FUserStatsResult = Steam::TSteamResult<Steam::Wrappers::FSteamUserStats>;

				const CSteamID TargetSteamId = GetSteamUserId(TargetAccountId);
				if (!TargetSteamId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::BatchQueryStats] Failed: No associated steam id found. User [%s]"), *ToLogString(TargetAccountId));

					InAsyncOp.SetError(UE::Online::Errors::NotFound());
					return MakeFulfilledPromise<FUserStatsResult>(FUserStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				return SteamCall<Steam::Wrappers::FSteamUserStats>({ .UserId = TargetSteamId });
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamUserStats>(TEXT("FStatsSteam::BatchQueryStats"),
				[this, TargetAccountId](UE::Online::TOnlineAsyncOp<UE::Online::FBatchQueryStats>& InAsyncOp, Steam::Wrappers::FSteamUserStats::Result&& Result)
			{
				auto UserStats = ReadUserStats(TargetAccountId, Result.UserId, InAsyncOp.GetParams().StatNames);

				CacheUserStats(UserStats);

				InAsyncOp.Data.Set<UE::Online::FUserStats>(Private::MakeUserStatsKey(TargetAccountId), MoveTemp(UserStats));
			}));
		}

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FBatchQueryStats>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			UE::Online::FBatchQueryStats::Result Result;
			Result.UsersStats.Reserve(Params.TargetAccountIds.Num());

			for (const auto& TargetAccountId : Params.TargetAccountIds)
			{
				if (const auto UserStats = InAsyncOp.Data.Get<UE::Online::FUserStats>(Private::MakeUserStatsKey(TargetAccountId)))
				{
					Result.UsersStats.Emplace(*UserStats);
				}
			}

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FStatsSteam::BatchQueryStats] Succeeded: User [%s], Users [%d]"),
				*ToLogString(Params.LocalAccountId), Result.UsersStats.Num());

			InAsyncOp.SetResult(MoveTemp(Result));
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

#if !UE_BUILD_SHIPPING
	UE::Online::TOnlineAsyncOpHandle<UE::Online::FResetStats> FStatsSteam::ResetStats(UE::Online::FResetStats::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FResetStats>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FResetStats>& InAsyncOp)
		{
			using FStoreStatsResult = Steam::TSteamResult<Steam::Wrappers::FSteamStoreStats>;

			const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
			{
				InAsyncOp.SetError(MoveTemp(Error));
				return MakeFulfilledPromise<FStoreStatsResult>(FStoreStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
			};

			const auto& Params = InAsyncOp.GetParams();

			auto LocalUser = ResolveSteamUser(Params.LocalAccountId, Params.LocalAccountId, TEXT("FStatsSteam::ResetStats"));
			if (LocalUser.IsError())
			{
				return FailWith(MoveTemp(LocalUser.GetErrorValue()));
			}

			const auto SteamUserStats = Steam::GetSteamInterface<ISteamUserStats>();
			if (SteamUserStats == nullptr)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::ResetStats] Failed: Steam user stats interface is not available"));
				return FailWith(UE::Online::Errors::MissingInterface());
			}

			// The achievements of the user are left alone: they have an operation of their own, and wiping
			// them from here would be a surprise to whoever only asked about stats.
			if (!SteamUserStats->ResetAllStats(false))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FStatsSteam::ResetStats] Steam->ResetAllStats Failed: User [%s]"), *ToLogString(Params.LocalAccountId));
				return FailWith(UE::Online::Errors::RequestFailure());
			}

			return SteamListen<Steam::Wrappers::FSteamStoreStats>({ });
		})
		.Then(Steam::Unwrap<Steam::Wrappers::FSteamStoreStats>(TEXT("FStatsSteam::ResetStats"),
			[this](UE::Online::TOnlineAsyncOp<UE::Online::FResetStats>& InAsyncOp, Steam::Wrappers::FSteamStoreStats::Result&& /*Result*/)
		{
			const auto& Params = InAsyncOp.GetParams();

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FStatsSteam::ResetStats] Succeeded: User [%s]"), *ToLogString(Params.LocalAccountId));

			CachedUsersStats.RemoveAll(UE::Online::FFindUserStatsByAccountId(Params.LocalAccountId));

			InAsyncOp.SetResult({ });
		}))
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}
#endif // !UE_BUILD_SHIPPING
}
