// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/PresenceSteam.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamInterfaces.h"
#include "Online/OnlineIdSteam.h"
#include "Online/OnlineServicesSteam.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/SteamResult.h"
#include "Steam/Wrappers/SteamPresence.h"

// Engine
#include "Online/OnlineErrorDefinitions.h"


namespace PoFigGames::Online
{
	namespace Private
	{
		/**
		 * Keys Steam gives a meaning of its own. steam_display names a localisation token rendered in the
		 * language of whoever is looking; status is the plain text shown beside the user.
		 */
		static constexpr ANSICHAR RichPresenceDisplayKey[] { "steam_display" };
		static constexpr ANSICHAR RichPresenceStatusKey[] { "status" };

		/**
		 * Whether a user can be joined is not something Steam reports about somebody else, so it travels as
		 * a key of the game like any other value the interface asks for and Steam has no notion of.
		 */
		static constexpr TCHAR JoinabilityKey[] { TEXT("__joinability") };

		/** A display token is named with a leading hash; a status which already carries one keeps it. */
		static FString ToDisplayToken(const FString& StatusString)
		{
			return StatusString.StartsWith(TEXT("#")) ? StatusString : FString::Printf(TEXT("#%s"), *StatusString);
		}

		/** What a user is doing, as far as their Steam account is concerned. */
		static UE::Online::EUserPresenceStatus TranslatePersonaState(const EPersonaState PersonaState)
		{
			switch (PersonaState)
			{
				case k_EPersonaStateOffline:
					return UE::Online::EUserPresenceStatus::Offline;
				case k_EPersonaStateBusy:
					return UE::Online::EUserPresenceStatus::DoNotDisturb;
				case k_EPersonaStateAway:
					return UE::Online::EUserPresenceStatus::Away;
				case k_EPersonaStateSnooze:
					return UE::Online::EUserPresenceStatus::ExtendedAway;
				case k_EPersonaStateOnline:
				case k_EPersonaStateLookingToTrade:
				case k_EPersonaStateLookingToPlay:
					return UE::Online::EUserPresenceStatus::Online;
				default:
					break;
			}

			// Invisible reads as offline to everybody but the user themselves, which is the point of it.
			return UE::Online::EUserPresenceStatus::Unknown;
		}

		/**
		 * Steam stores rich presence as flat text, so a value is written as the text it reads like. A
		 * property holding an array or a map of its own has no such reading and is left out.
		 */
		static bool ToRichPresenceValue(const UE::Online::FPresenceProperty& Property, FString& OutValue)
		{
			if (Property.IsType<FString>())
			{
				OutValue = Property.Get<FString>();
				return true;
			}

			if (Property.IsType<bool>())
			{
				OutValue = LexToString(Property.Get<bool>());
				return true;
			}

			if (Property.IsType<int64>())
			{
				OutValue = LexToString(Property.Get<int64>());
				return true;
			}

			if (Property.IsType<double>())
			{
				OutValue = LexToString(Property.Get<double>());
				return true;
			}

			return false;
		}

		/** Steam refuses a key or a value which is too long, so an oversized one is reported rather than sent. */
		static bool IsWithinSteamLimits(const FString& Key, const FString& Value)
		{
			if (Key.Len() >= k_cchMaxRichPresenceKeyLength || Value.Len() >= k_cchMaxRichPresenceValueLength)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPresenceSteam::UpdatePresence] Skipping a key Steam would refuse: Key [%s], Key length [%d], Value length [%d]"),
					*Key, Key.Len(), Value.Len());

				return false;
			}

			return true;
		}
	}

	void FPresenceSteam::Initialize()
	{
		Super::Initialize();

		// Steam announces a change rather than answering a query, so the component listens for as long as
		// it is alive.
		OnPersonaStateChangeCallback.Register(this, &FPresenceSteam::OnPersonaStateChange);
		OnFriendRichPresenceUpdateCallback.Register(this, &FPresenceSteam::OnFriendRichPresenceUpdate);
	}

	void FPresenceSteam::PreShutdown()
	{
		// The handlers below reach for the cache, so they stop firing before it is released.
		OnPersonaStateChangeCallback.Unregister();
		OnFriendRichPresenceUpdateCallback.Unregister();

		CachedPresences.Reset();
		WatchedUsers.Reset();

		Super::PreShutdown();
	}

	TSharedRef<UE::Online::FUserPresence> FPresenceSteam::ReadUserPresence(const UE::Online::FAccountId& TargetAccountId, const CSteamID& TargetSteamId) const
	{
		auto Presence = MakeShared<UE::Online::FUserPresence>();

		Presence->AccountId = TargetAccountId;
		Presence->PlatformType = UE::Online::EOnlinePlatformType::Steam;

		const auto Friends = Steam::GetSteamInterface<ISteamFriends>();
		if (Friends == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPresenceSteam::QueryPresence] Failed: Steam friends interface is not available"));
			return Presence;
		}

		Presence->Status = Private::TranslatePersonaState(Friends->GetFriendPersonaState(TargetSteamId));

		// Whether the user is in this very game is something Steam answers about anybody it reports on.
		if (FriendGameInfo_t GameInfo { }; Friends->GetFriendGamePlayed(TargetSteamId, &GameInfo))
		{
			const auto Utils = Steam::GetSteamInterface<ISteamUtils>();

			Presence->GameStatus = Utils != nullptr && GameInfo.m_gameID.AppID() == Utils->GetAppID()
				? UE::Online::EUserPresenceGameStatus::PlayingThisGame
				: UE::Online::EUserPresenceGameStatus::PlayingOtherGame;
		}

		// Everything the game published comes back under its own name; the two keys Steam gives a meaning
		// of its own are put back into the fields of the interface which mean the same thing.
		const int32 KeyCount = Friends->GetFriendRichPresenceKeyCount(TargetSteamId);
		for (int32 KeyIndex = 0; KeyIndex < KeyCount; ++KeyIndex)
		{
			const auto RichPresenceKey = Friends->GetFriendRichPresenceKeyByIndex(TargetSteamId, KeyIndex);
			if (RichPresenceKey == nullptr || *RichPresenceKey == '\0')
			{
				continue;
			}

			FString Key = StringCast<TCHAR>(RichPresenceKey).Get();
			FString Value = StringCast<TCHAR>(Friends->GetFriendRichPresence(TargetSteamId, RichPresenceKey)).Get();

			if (Key == StringCast<TCHAR>(Private::RichPresenceDisplayKey).Get())
			{
				Presence->StatusString = MoveTemp(Value);
				continue;
			}

			if (Key == StringCast<TCHAR>(Private::RichPresenceStatusKey).Get())
			{
				Presence->RichPresenceString = MoveTemp(Value);
				continue;
			}

			if (Key == Private::JoinabilityKey)
			{
				LexFromString(Presence->Joinability, *Value);
				continue;
			}

			Presence->Properties.AddVariant(MoveTemp(Key), MoveTemp(Value));
		}

		return Presence;
	}

	TSharedRef<UE::Online::FUserPresence> FPresenceSteam::GetLocalPresence(const UE::Online::FAccountId& LocalAccountId, const CSteamID& LocalSteamId) const
	{
		const auto CachedPresence = CachedPresences.Find(LocalAccountId);

		// What the account itself says is Steam's answer either way, so it is read afresh; what the game
		// published is the copy which was kept when it was published.
		auto Presence = ReadUserPresence(LocalAccountId, LocalSteamId);
		if (CachedPresence == nullptr)
		{
			return Presence;
		}

		auto LocalPresence = MakeShared<UE::Online::FUserPresence>(**CachedPresence);
		LocalPresence->Status = Presence->Status;
		LocalPresence->GameStatus = Presence->GameStatus;

		return LocalPresence;
	}

	UE::Online::FOnlineError FPresenceSteam::PublishLocalPresence(const UE::Online::FAccountId& LocalAccountId,
		const CSteamID& LocalSteamId, const TSharedRef<UE::Online::FUserPresence>& Presence)
	{
		if (auto Error = WriteLocalPresence(*Presence); Error != UE::Online::Errors::Success())
		{
			return Error;
		}

		// The state of the account belongs to Steam, so the copy which is kept carries what Steam reports
		// rather than what the caller asked for.
		const auto AccountPresence = ReadUserPresence(LocalAccountId, LocalSteamId);

		Presence->AccountId = LocalAccountId;
		Presence->PlatformType = UE::Online::EOnlinePlatformType::Steam;
		Presence->Status = AccountPresence->Status;
		Presence->GameStatus = AccountPresence->GameStatus;

		CachedPresences.Emplace(LocalAccountId, Presence);

		return UE::Online::Errors::Success();
	}

	UE::Online::FOnlineError FPresenceSteam::WriteLocalPresence(const UE::Online::FUserPresence& Presence) const
	{
		const auto Friends = Steam::GetSteamInterface<ISteamFriends>();
		if (Friends == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPresenceSteam::UpdatePresence] Failed: Steam friends interface is not available"));
			return UE::Online::Errors::MissingInterface();
		}

		// A presence is published whole: what is not in it any more is not left behind either.
		Friends->ClearRichPresence();

		const auto PublishKey = [&Friends](const FString& Key, const FString& Value)
		{
			if (Value.IsEmpty() || !Private::IsWithinSteamLimits(Key, Value))
			{
				return;
			}

			Friends->SetRichPresence(StringCast<ANSICHAR>(*Key).Get(), StringCast<ANSICHAR>(*Value).Get());
		};

		if (!Presence.StatusString.IsEmpty())
		{
			PublishKey(StringCast<TCHAR>(Private::RichPresenceDisplayKey).Get(), Private::ToDisplayToken(Presence.StatusString));
		}

		PublishKey(StringCast<TCHAR>(Private::RichPresenceStatusKey).Get(), Presence.RichPresenceString);

		if (Presence.Joinability != UE::Online::EUserPresenceJoinability::Unknown)
		{
			PublishKey(Private::JoinabilityKey, LexToString(Presence.Joinability));
		}

		for (const auto& Property : Presence.Properties)
		{
			if (FString Value; Private::ToRichPresenceValue(Property.Value, Value))
			{
				PublishKey(Property.Key, Value);
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPresenceSteam::UpdatePresence] Skipping a property Steam has no reading of: Key [%s]"), *Property.Key);
			}
		}

		UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FPresenceSteam::UpdatePresence] Published: Status [%s], Properties [%d]"),
			*Presence.StatusString, Presence.Properties.Num());

		return UE::Online::Errors::Success();
	}

	void FPresenceSteam::RefreshPresence(const CSteamID& TargetSteamId)
	{
		const auto TargetAccountId = FindAccountId(TargetSteamId);
		if (!TargetAccountId.IsValid() || !CachedPresences.Contains(TargetAccountId))
		{
			// Nobody ever asked about this user, so there is nothing to keep in step.
			return;
		}

		auto Presence = ReadUserPresence(TargetAccountId, TargetSteamId);
		CachedPresences.Emplace(TargetAccountId, Presence);

		if (!WatchedUsers.Contains(TargetAccountId))
		{
			return;
		}

		if (const auto LocalAccountId = GetLocalAccountId(); LocalAccountId.IsValid())
		{
			OnPresenceUpdatedEvent.Broadcast(UE::Online::FPresenceUpdated { .LocalAccountId = LocalAccountId, .UpdatedPresence = MoveTemp(Presence) });
		}
	}

	void FPresenceSteam::OnPersonaStateChange(PersonaStateChange_t* Message)
	{
		constexpr int32 PresenceChangeFlags { k_EPersonaChangeStatus | k_EPersonaChangeComeOnline | k_EPersonaChangeGoneOffline
			| k_EPersonaChangeGamePlayed | k_EPersonaChangeRichPresence };

		if (Message == nullptr || (Message->m_nChangeFlags & PresenceChangeFlags) == 0)
		{
			return;
		}

		RefreshPresence(Message->m_ulSteamID);
	}

	void FPresenceSteam::OnFriendRichPresenceUpdate(FriendRichPresenceUpdate_t* Message)
	{
		if (Message == nullptr)
		{
			return;
		}

		RefreshPresence(Message->m_steamIDFriend);
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryPresence> FPresenceSteam::QueryPresence(UE::Online::FQueryPresence::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FQueryPresence>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryPresence>& InAsyncOp)
			{
				using FRichPresenceResult = Steam::TSteamResult<Steam::Wrappers::FSteamFriendRichPresence>;

				const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
				{
					InAsyncOp.SetError(MoveTemp(Error));
					return MakeFulfilledPromise<FRichPresenceResult>(FRichPresenceResult(UE::Online::Errors::Cancelled())).GetFuture();
				};

				const auto& Params = InAsyncOp.GetParams();

				auto TargetUser = ResolveSteamUser(Params.LocalAccountId, Params.TargetAccountId, TEXT("FPresenceSteam::QueryPresence"));
				if (TargetUser.IsError())
				{
					return FailWith(MoveTemp(TargetUser.GetErrorValue()));
				}

				if (Params.TargetAccountId == Params.LocalAccountId)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPresenceSteam::QueryPresence] Failed: The presence of the local user is known without asking."));
					return FailWith(UE::Online::Errors::CannotQueryLocalUsers());
				}

				InAsyncOp.Data.Set<CSteamID>(TEXT("TargetSteamId"), TargetUser.GetOkValue());

				return SteamListen<Steam::Wrappers::FSteamFriendRichPresence>({ .UserId = TargetUser.GetOkValue() });
			})
			.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryPresence>& InAsyncOp, Steam::TSteamResult<Steam::Wrappers::FSteamFriendRichPresence>&& RichPresenceResult)
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto TargetSteamId = InAsyncOp.Data.Get<CSteamID>(TEXT("TargetSteamId"));

				if (TargetSteamId == nullptr)
				{
					InAsyncOp.SetError(UE::Online::Errors::InvalidState());
					return;
				}

				// Steam says nothing at all about a user who published no rich presence, so a request which
				// went unanswered is that answer: what the account itself reports is still worth having.
				UE_CLOG(RichPresenceResult.IsError(), LogOnlineServicesSteam, Verbose,
					TEXT("[FPresenceSteam::QueryPresence] No rich presence published. User [%s], Result [%s]"),
					*ToLogString(Params.TargetAccountId), *RichPresenceResult.GetErrorValue().GetLogString());

				auto Presence = ReadUserPresence(Params.TargetAccountId, *TargetSteamId);
				CachedPresences.Emplace(Params.TargetAccountId, Presence);

				if (Params.bListenToChanges)
				{
					WatchedUsers.Emplace(Params.TargetAccountId);
				}

				InAsyncOp.SetResult({ .Presence = MoveTemp(Presence) });
			})
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FBatchQueryPresence> FPresenceSteam::BatchQueryPresence(UE::Online::FBatchQueryPresence::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FBatchQueryPresence>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FBatchQueryPresence>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			if (auto LocalUser = ResolveSteamUser(Params.LocalAccountId, Params.LocalAccountId, TEXT("FPresenceSteam::BatchQueryPresence")); LocalUser.IsError())
			{
				return InAsyncOp.SetError(MoveTemp(LocalUser.GetErrorValue()));
			}

			if (Params.TargetAccountIds.IsEmpty())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPresenceSteam::BatchQueryPresence] Failed: No users to query."));
				return InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
			}

			if (Params.TargetAccountIds.Contains(Params.LocalAccountId))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPresenceSteam::BatchQueryPresence] Failed: The presence of the local user is known without asking."));
				return InAsyncOp.SetError(UE::Online::Errors::CannotQueryLocalUsers());
			}
		});

		// Steam answers for one user at a time, and the targets are known before the operation runs, which
		// is why every request is chained up front.
		for (const auto& TargetAccountId : Op->GetParams().TargetAccountIds)
		{
			Op->Then([this, TargetAccountId](UE::Online::TOnlineAsyncOp<UE::Online::FBatchQueryPresence>& InAsyncOp)
			{
				using FRichPresenceResult = Steam::TSteamResult<Steam::Wrappers::FSteamFriendRichPresence>;

				const CSteamID TargetSteamId = GetSteamUserId(TargetAccountId);
				if (!TargetSteamId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPresenceSteam::BatchQueryPresence] Failed: No associated steam id found. User [%s]"), *ToLogString(TargetAccountId));

					InAsyncOp.SetError(UE::Online::Errors::NotFound());
					return MakeFulfilledPromise<FRichPresenceResult>(FRichPresenceResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				return SteamListen<Steam::Wrappers::FSteamFriendRichPresence>({ .UserId = TargetSteamId });
			})
			.Then([this, TargetAccountId](UE::Online::TOnlineAsyncOp<UE::Online::FBatchQueryPresence>& InAsyncOp, Steam::TSteamResult<Steam::Wrappers::FSteamFriendRichPresence>&& /*RichPresenceResult*/)
			{
				auto Presence = ReadUserPresence(TargetAccountId, GetSteamUserId(TargetAccountId));
				CachedPresences.Emplace(TargetAccountId, Presence);

				if (InAsyncOp.GetParams().bListenToChanges)
				{
					WatchedUsers.Emplace(TargetAccountId);
				}
			});
		}

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FBatchQueryPresence>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			UE::Online::FBatchQueryPresence::Result Result;
			Result.Presences.Reserve(Params.TargetAccountIds.Num());

			for (const auto& TargetAccountId : Params.TargetAccountIds)
			{
				if (const auto Presence = CachedPresences.Find(TargetAccountId))
				{
					Result.Presences.Emplace(*Presence);
				}
			}

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FPresenceSteam::BatchQueryPresence] Succeeded: User [%s], Users [%d]"),
				*ToLogString(Params.LocalAccountId), Result.Presences.Num());

			InAsyncOp.SetResult(MoveTemp(Result));
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FGetCachedPresence> FPresenceSteam::GetCachedPresence(UE::Online::FGetCachedPresence::Params&& InParams)
	{
		auto TargetUser = ResolveSteamUser(InParams.LocalAccountId, InParams.TargetAccountId, TEXT("FPresenceSteam::GetCachedPresence"));
		if (TargetUser.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FGetCachedPresence>(MoveTemp(TargetUser.GetErrorValue()));
		}

		// What the local user is doing is known without asking anybody, so it is answered rather than
		// waited for; everybody else has to have been queried first.
		if (InParams.TargetAccountId == InParams.LocalAccountId)
		{
			return UE::Online::TOnlineResult<UE::Online::FGetCachedPresence>({ GetLocalPresence(InParams.LocalAccountId, TargetUser.GetOkValue()) });
		}

		const auto Presence = CachedPresences.Find(InParams.TargetAccountId);
		if (Presence == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FPresenceSteam::GetCachedPresence] No presence cached, call QueryPresence first. User [%s]"), *ToLogString(InParams.TargetAccountId));
			return UE::Online::TOnlineResult<UE::Online::FGetCachedPresence>(UE::Online::Errors::NotFound());
		}

		return UE::Online::TOnlineResult<UE::Online::FGetCachedPresence>({ *Presence });
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FUpdatePresence> FPresenceSteam::UpdatePresence(UE::Online::FUpdatePresence::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FUpdatePresence>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUpdatePresence>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			auto LocalUser = ResolveSteamUser(Params.LocalAccountId, Params.LocalAccountId, TEXT("FPresenceSteam::UpdatePresence"));
			if (LocalUser.IsError())
			{
				return InAsyncOp.SetError(MoveTemp(LocalUser.GetErrorValue()));
			}

			// Whether a user is online, away or busy belongs to their Steam account, and no game may set
			// it; what the caller asked for there is dropped rather than pretended to.
			UE_CLOG(Params.Presence->Status != UE::Online::EUserPresenceStatus::Unknown, LogOnlineServicesSteam, Verbose,
				TEXT("[FPresenceSteam::UpdatePresence] Steam decides the state of an account, so the requested [%s] is ignored."),
				LexToString(Params.Presence->Status));

			if (auto Error = PublishLocalPresence(Params.LocalAccountId, LocalUser.GetOkValue(), Params.Presence); Error != UE::Online::Errors::Success())
			{
				return InAsyncOp.SetError(MoveTemp(Error));
			}

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FPartialUpdatePresence> FPresenceSteam::PartialUpdatePresence(UE::Online::FPartialUpdatePresence::Params&& InParams)
	{
		const auto Op = GetMergeableOp<UE::Online::FPartialUpdatePresence>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FPartialUpdatePresence>& InAsyncOp)
			{
				const auto& Params = InAsyncOp.GetParams();

				auto LocalUser = ResolveSteamUser(Params.LocalAccountId, Params.LocalAccountId, TEXT("FPresenceSteam::PartialUpdatePresence"));
				if (LocalUser.IsError())
				{
					return InAsyncOp.SetError(MoveTemp(LocalUser.GetErrorValue()));
				}

				// Steam publishes a presence whole, so a partial update is folded into what is already out
				// there before it is published again.
				auto Presence = GetLocalPresence(Params.LocalAccountId, LocalUser.GetOkValue());
				const auto& Mutations = Params.Mutations;

				if (Mutations.Joinability.IsSet())
				{
					Presence->Joinability = *Mutations.Joinability;
				}

				if (Mutations.StatusString.IsSet())
				{
					Presence->StatusString = *Mutations.StatusString;
				}

				if (Mutations.RichPresenceString.IsSet())
				{
					Presence->RichPresenceString = *Mutations.RichPresenceString;
				}

				for (const auto& RemovedProperty : Mutations.RemovedProperties)
				{
					Presence->Properties.Remove(RemovedProperty);
				}

				for (const auto& UpdatedProperty : Mutations.UpdatedProperties)
				{
					Presence->Properties.Emplace(UpdatedProperty.Key, UpdatedProperty.Value);
				}

				if (auto Error = PublishLocalPresence(Params.LocalAccountId, LocalUser.GetOkValue(), Presence); Error != UE::Online::Errors::Success())
				{
					return InAsyncOp.SetError(MoveTemp(Error));
				}

				InAsyncOp.SetResult({ });
			})
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}
}
