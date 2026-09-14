// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/SocialSteam.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamInterfaces.h"
#include "Online/OnlineIdSteam.h"
#include "Online/OnlineServicesSteam.h"

// Engine
#include "Online/OnlineErrorDefinitions.h"


namespace PoFigGames::Online
{
	namespace Private
	{
		/**
		 * Everybody the interface calls a friend: the friends proper and the invitations waiting in either
		 * direction, which the interface reports as relationships of their own.
		 */
		static constexpr int32 FriendListFlags { k_EFriendFlagImmediate | k_EFriendFlagFriendshipRequested | k_EFriendFlagRequestingFriendship };

		/** Steam keeps a user the local one refuses to hear from under one of these, depending on how it was done. */
		static constexpr int32 BlockedListFlags { k_EFriendFlagBlocked | k_EFriendFlagIgnored | k_EFriendFlagIgnoredFriend };

		/** Pages of the Steam overlay where the user changes a relationship themselves. */
		static constexpr ANSICHAR FriendAddDialog[] { "friendadd" };
		static constexpr ANSICHAR FriendRequestAcceptDialog[] { "friendrequestaccept" };
		static constexpr ANSICHAR FriendRequestIgnoreDialog[] { "friendrequestignore" };

		/** What the interface calls the relationship Steam reports. */
		static UE::Online::ERelationship TranslateRelationship(const EFriendRelationship FriendRelationship)
		{
			switch (FriendRelationship)
			{
				case k_EFriendRelationshipFriend:
					return UE::Online::ERelationship::Friend;
				case k_EFriendRelationshipRequestInitiator:
					return UE::Online::ERelationship::InviteSent;
				case k_EFriendRelationshipRequestRecipient:
					return UE::Online::ERelationship::InviteReceived;
				case k_EFriendRelationshipBlocked:
				case k_EFriendRelationshipIgnored:
				case k_EFriendRelationshipIgnoredFriend:
					return UE::Online::ERelationship::Blocked;
				default:
					break;
			}

			return UE::Online::ERelationship::NotFriend;
		}
	}

	void FSocialSteam::Initialize()
	{
		Super::Initialize();

		// A relationship changes because the user changed it, in the Steam client or in the overlay, and
		// Steam announces that rather than answering anybody, so the component listens while it is alive.
		OnPersonaStateChangeCallback.Register(this, &FSocialSteam::OnPersonaStateChange);
	}

	void FSocialSteam::PreShutdown()
	{
		// The handler below reaches for the lists, so it stops firing before they are released.
		OnPersonaStateChangeCallback.Unregister();

		Friends.Reset();
		BlockedUsers.Reset();

		Super::PreShutdown();
	}

	void FSocialSteam::ReadFriends(const UE::Online::FAccountId& LocalAccountId)
	{
		const auto SteamFriends = Steam::GetSteamInterface<ISteamFriends>();
		if (SteamFriends == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FSocialSteam::QueryFriends] Failed: Steam friends interface is not available"));
			return;
		}

		auto& KnownFriends = Friends.FindOrAdd(LocalAccountId);
		auto& KnownBlockedUsers = BlockedUsers.FindOrAdd(LocalAccountId);

		FFriendMap NewFriends;
		TSet<UE::Online::FAccountId> NewBlockedUsers;

		const int32 FriendCount = SteamFriends->GetFriendCount(Private::FriendListFlags | Private::BlockedListFlags);
		NewFriends.Reserve(FriendCount);

		for (int32 FriendIndex = 0; FriendIndex < FriendCount; ++FriendIndex)
		{
			const CSteamID FriendSteamId = SteamFriends->GetFriendByIndex(FriendIndex, Private::FriendListFlags | Private::BlockedListFlags);
			if (!FriendSteamId.IsValid())
			{
				continue;
			}

			const auto FriendAccountId = FindOrAddAccountId(FriendSteamId);
			const auto Relationship = Private::TranslateRelationship(SteamFriends->GetFriendRelationship(FriendSteamId));

			if (Relationship == UE::Online::ERelationship::Blocked)
			{
				NewBlockedUsers.Emplace(FriendAccountId);
			}
			else if (Relationship == UE::Online::ERelationship::NotFriend)
			{
				continue;
			}

			auto Friend = MakeShared<UE::Online::FFriend>();
			Friend->FriendId = FriendAccountId;
			Friend->Relationship = Relationship;

			// The name Steam answers with already follows whatever nickname the local user gave them, so
			// there is no second name to report; the call which would give one is deprecated.
			Friend->DisplayName = StringCast<TCHAR>(SteamFriends->GetFriendPersonaName(FriendSteamId)).Get();

			NewFriends.Emplace(FriendAccountId, MoveTemp(Friend));
		}

		// Whoever was in the list before and is not any more has stopped being anything to this user.
		for (const auto& KnownFriend : KnownFriends)
		{
			if (const auto NewFriend = NewFriends.Find(KnownFriend.Key))
			{
				if ((*NewFriend)->Relationship != KnownFriend.Value->Relationship)
				{
					BroadcastRelationshipUpdated(LocalAccountId, KnownFriend.Key, KnownFriend.Value->Relationship, (*NewFriend)->Relationship);
				}

				continue;
			}

			BroadcastRelationshipUpdated(LocalAccountId, KnownFriend.Key, KnownFriend.Value->Relationship, UE::Online::ERelationship::NotFriend);
		}

		for (const auto& NewFriend : NewFriends)
		{
			if (!KnownFriends.Contains(NewFriend.Key))
			{
				BroadcastRelationshipUpdated(LocalAccountId, NewFriend.Key, UE::Online::ERelationship::NotFriend, NewFriend.Value->Relationship);
			}
		}

		UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FSocialSteam::QueryFriends] Succeeded: User [%s], Friends [%d], Blocked [%d]"),
			*ToLogString(LocalAccountId), NewFriends.Num(), NewBlockedUsers.Num());

		KnownFriends = MoveTemp(NewFriends);
		KnownBlockedUsers = MoveTemp(NewBlockedUsers);
	}

	void FSocialSteam::OnPersonaStateChange(PersonaStateChange_t* Message)
	{
		if (Message == nullptr || (Message->m_nChangeFlags & k_EPersonaChangeRelationshipChanged) == 0)
		{
			return;
		}

		// Steam names the user whose relationship changed, but what it changed to is read from the list as
		// a whole, which is also what tells the game about everybody who left it.
		const auto LocalAccountId = GetLocalAccountId();
		if (LocalAccountId.IsValid() && Friends.Contains(LocalAccountId))
		{
			ReadFriends(LocalAccountId);
		}
	}

	UE::Online::FOnlineError FSocialSteam::AskUserToChangeRelationship(const CSteamID& TargetSteamId, const ANSICHAR* DialogType, const TCHAR* Context) const
	{
		const auto SteamFriends = Steam::GetSteamInterface<ISteamFriends>();
		if (SteamFriends == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] Failed: Steam friends interface is not available"), Context);
			return UE::Online::Errors::MissingInterface();
		}

		// Without the overlay there is nowhere for the user to answer, and Steam would swallow the request.
		if (auto Error = CheckOverlayAvailable(Context); Error != UE::Online::Errors::Success())
		{
			return Error;
		}

		UE_LOG(LogOnlineServicesSteam, Log, TEXT("[%s] Asking the user: Target [%s], Dialog [%hs]"), Context, *ToLogString(TargetSteamId), DialogType);

		SteamFriends->ActivateGameOverlayToUser(DialogType, TargetSteamId);

		return UE::Online::Errors::Success();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryFriends> FSocialSteam::QueryFriends(UE::Online::FQueryFriends::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FQueryFriends>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryFriends>& InAsyncOp)
			{
				const auto& Params = InAsyncOp.GetParams();

				if (auto LocalUser = ResolveSteamUser(Params.LocalAccountId, Params.LocalAccountId, TEXT("FSocialSteam::QueryFriends")); LocalUser.IsError())
				{
					return InAsyncOp.SetError(MoveTemp(LocalUser.GetErrorValue()));
				}

				// The Steam client holds the friends of the user it is signed in as, so there is nothing to
				// wait for: the list is walked and remembered.
				ReadFriends(Params.LocalAccountId);

				InAsyncOp.SetResult({ });
			})
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FGetFriends> FSocialSteam::GetFriends(UE::Online::FGetFriends::Params&& InParams)
	{
		if (auto LocalUser = ResolveSteamUser(InParams.LocalAccountId, InParams.LocalAccountId, TEXT("FSocialSteam::GetFriends")); LocalUser.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FGetFriends>(MoveTemp(LocalUser.GetErrorValue()));
		}

		const auto KnownFriends = Friends.Find(InParams.LocalAccountId);
		if (KnownFriends == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FSocialSteam::GetFriends] Failed: Call QueryFriends first."));
			return UE::Online::TOnlineResult<UE::Online::FGetFriends>(UE::Online::Errors::InvalidState());
		}

		UE::Online::FGetFriends::Result Result;
		Result.Friends.Reserve(KnownFriends->Num());

		for (const auto& KnownFriend : *KnownFriends)
		{
			Result.Friends.Emplace(KnownFriend.Value);
		}

		return UE::Online::TOnlineResult<UE::Online::FGetFriends>(MoveTemp(Result));
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FSendFriendInvite> FSocialSteam::SendFriendInvite(UE::Online::FSendFriendInvite::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FSendFriendInvite>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FSendFriendInvite>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			auto TargetUser = ResolveSteamUser(Params.LocalAccountId, Params.TargetAccountId, TEXT("FSocialSteam::SendFriendInvite"));
			if (TargetUser.IsError())
			{
				return InAsyncOp.SetError(MoveTemp(TargetUser.GetErrorValue()));
			}

			// The request is the user's to make, so this reports that they were asked; whether they went
			// through with it arrives as the change of relationship Steam announces afterwards.
			if (auto Error = AskUserToChangeRelationship(TargetUser.GetOkValue(), Private::FriendAddDialog, TEXT("FSocialSteam::SendFriendInvite"));
				Error != UE::Online::Errors::Success())
			{
				return InAsyncOp.SetError(MoveTemp(Error));
			}

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAcceptFriendInvite> FSocialSteam::AcceptFriendInvite(UE::Online::FAcceptFriendInvite::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FAcceptFriendInvite>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAcceptFriendInvite>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			auto TargetUser = ResolveSteamUser(Params.LocalAccountId, Params.TargetAccountId, TEXT("FSocialSteam::AcceptFriendInvite"));
			if (TargetUser.IsError())
			{
				return InAsyncOp.SetError(MoveTemp(TargetUser.GetErrorValue()));
			}

			if (auto Error = AskUserToChangeRelationship(TargetUser.GetOkValue(), Private::FriendRequestAcceptDialog, TEXT("FSocialSteam::AcceptFriendInvite"));
				Error != UE::Online::Errors::Success())
			{
				return InAsyncOp.SetError(MoveTemp(Error));
			}

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FRejectFriendInvite> FSocialSteam::RejectFriendInvite(UE::Online::FRejectFriendInvite::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FRejectFriendInvite>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FRejectFriendInvite>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			auto TargetUser = ResolveSteamUser(Params.LocalAccountId, Params.TargetAccountId, TEXT("FSocialSteam::RejectFriendInvite"));
			if (TargetUser.IsError())
			{
				return InAsyncOp.SetError(MoveTemp(TargetUser.GetErrorValue()));
			}

			if (auto Error = AskUserToChangeRelationship(TargetUser.GetOkValue(), Private::FriendRequestIgnoreDialog, TEXT("FSocialSteam::RejectFriendInvite"));
				Error != UE::Online::Errors::Success())
			{
				return InAsyncOp.SetError(MoveTemp(Error));
			}

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryBlockedUsers> FSocialSteam::QueryBlockedUsers(UE::Online::FQueryBlockedUsers::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FQueryBlockedUsers>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryBlockedUsers>& InAsyncOp)
			{
				const auto& Params = InAsyncOp.GetParams();

				if (auto LocalUser = ResolveSteamUser(Params.LocalAccountId, Params.LocalAccountId, TEXT("FSocialSteam::QueryBlockedUsers")); LocalUser.IsError())
				{
					return InAsyncOp.SetError(MoveTemp(LocalUser.GetErrorValue()));
				}

				// Steam keeps the blocked users in the same list as the friends, so one walk answers both.
				ReadFriends(Params.LocalAccountId);

				InAsyncOp.SetResult({ });
			})
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FGetBlockedUsers> FSocialSteam::GetBlockedUsers(UE::Online::FGetBlockedUsers::Params&& InParams)
	{
		if (auto LocalUser = ResolveSteamUser(InParams.LocalAccountId, InParams.LocalAccountId, TEXT("FSocialSteam::GetBlockedUsers")); LocalUser.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FGetBlockedUsers>(MoveTemp(LocalUser.GetErrorValue()));
		}

		const auto KnownBlockedUsers = BlockedUsers.Find(InParams.LocalAccountId);
		if (KnownBlockedUsers == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FSocialSteam::GetBlockedUsers] Failed: Call QueryBlockedUsers first."));
			return UE::Online::TOnlineResult<UE::Online::FGetBlockedUsers>(UE::Online::Errors::InvalidState());
		}

		return UE::Online::TOnlineResult<UE::Online::FGetBlockedUsers>({ KnownBlockedUsers->Array() });
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FBlockUser> FSocialSteam::BlockUser(UE::Online::FBlockUser::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FBlockUser>(MoveTemp(InParams));

		// Steam offers neither a call which blocks a user nor an overlay page which asks the user to do it;
		// blocking happens in the Steam client, on the profile of whoever is being blocked.
		UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FSocialSteam::BlockUser] Failed: Steam has no way for a game to block a user. User [%s]"),
			*ToLogString(Op->GetParams().TargetAccountId));

		Op->SetError(UE::Online::Errors::NotImplemented());

		return Op->GetHandle();
	}
}
