// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/PrivilegesSteam.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamInterfaces.h"
#include "Online/AuthSteam.h"
#include "Online/OnlineServicesSteam.h"

// Engine
#include "Online/OnlineUtilsCommon.h"


namespace PoFigGames::Online
{
	/**
	 * Whether the user owns the game in one of the ways Steam recognises.
	 *
	 * Family sharing and a free weekend both grant the right to play, so neither counts as a failure.
	 */
	static bool IsSubscribedToApp(ISteamApps& AppsInterface)
	{
		return AppsInterface.BIsSubscribed() || AppsInterface.BIsSubscribedFromFamilySharing() || AppsInterface.BIsSubscribedFromFreeWeekend();
	}

	/** Whether the Steam client blocks a feature because a parent locked it. */
	static bool IsParentalFeatureBlocked(const EParentalFeature Feature)
	{
		auto ParentalSettings = Steam::GetSteamInterface<ISteamParentalSettings>();

		// Parental settings are optional, so a client which does not offer them restricts nothing.
		return ParentalSettings != nullptr
			&& ParentalSettings->BIsParentalLockEnabled()
			&& ParentalSettings->BIsParentalLockLocked()
			&& ParentalSettings->BIsFeatureBlocked(Feature);
	}

	/** Whether a parent has the Steam client locked down at all. */
	static bool IsParentalLockEngaged()
	{
		auto ParentalSettings = Steam::GetSteamInterface<ISteamParentalSettings>();

		return ParentalSettings != nullptr && ParentalSettings->BIsParentalLockEnabled() && ParentalSettings->BIsParentalLockLocked();
	}

	/** Sends the user to the store page of the game, which is the one restriction they can act on. */
	static void ShowStorePage()
	{
		auto FriendsInterface = Steam::GetSteamInterface<ISteamFriends>();
		auto UtilsInterface = Steam::GetSteamInterface<ISteamUtils>();

		if (FriendsInterface != nullptr && UtilsInterface != nullptr)
		{
			FriendsInterface->ActivateGameOverlayToStore(UtilsInterface->GetAppID(), k_EOverlayToStoreFlag_None);
		}
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryUserPrivilege> FPrivilegesSteam::QueryUserPrivilege(UE::Online::FQueryUserPrivilege::Params&& Params)
	{
		const auto Op = GetOp<UE::Online::FQueryUserPrivilege>(MoveTemp(Params));

		// Step 1: Find the account the question is about.
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryUserPrivilege>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (!Params.LocalAccountId.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPrivilegesSteam::QueryUserPrivilege] Failed: No account id provided."));
				InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
				return;
			}

			const auto AuthPtr = Services.GetAuthInterface();
			if (!AuthPtr.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPrivilegesSteam::QueryUserPrivilege] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
				return;
			}

			UE::Online::FAuthGetLocalOnlineUserByOnlineAccountId::Params GetAccountParams { Params.LocalAccountId };
			auto AccountResult = AuthPtr->GetLocalOnlineUserByOnlineAccountId(MoveTemp(GetAccountParams));

			if (AccountResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPrivilegesSteam::QueryUserPrivilege] Answered UserNotFound: no local user is logged in under account id. User [%s], Privilege [%s]"),
					*ToLogString(Params.LocalAccountId), LexToString(Params.Privilege));

				InAsyncOp.SetResult(UE::Online::FQueryUserPrivilege::Result { .PrivilegeResult = UE::Online::EPrivilegeResults::UserNotFound });
				return;
			}

			InAsyncOp.Data.Set<TSharedRef<FAccountInfoSteam>>(AccountInfoKey, StaticCastSharedRef<FAccountInfoSteam>(AccountResult.GetOkValue().AccountInfo));
		})
		// Step 2: Collect every restriction which applies to it.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryUserPrivilege>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();
			const auto& AccountInfoSteam = GetOpDataChecked<TSharedRef<FAccountInfoSteam>>(InAsyncOp, AccountInfoKey);

			// The anonymous identity of a game server owns nothing and is restricted by nobody, and the
			// interfaces the answer would be read from belong to the client API which it does not run.
			if (AccountInfoSteam->bIsServer)
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FPrivilegesSteam::QueryUserPrivilege] Succeeded: a game server identity owns nothing and is restricted by nobody. User [%s], Privilege [%s]"),
					*ToLogString(Params.LocalAccountId), LexToString(Params.Privilege));

				InAsyncOp.SetResult(UE::Online::FQueryUserPrivilege::Result { .PrivilegeResult = UE::Online::EPrivilegeResults::NoFailures });
				return;
			}

			if (!AccountInfoSteam->SteamAccountId.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPrivilegesSteam::QueryUserPrivilege] Answered UserNotLoggedIn: the local user carries no Steam id. User [%s], Privilege [%s]"),
					*ToLogString(Params.LocalAccountId), LexToString(Params.Privilege));

				InAsyncOp.SetResult(UE::Online::FQueryUserPrivilege::Result { .PrivilegeResult = UE::Online::EPrivilegeResults::UserNotLoggedIn });
				return;
			}

			auto AppsInterface = Steam::GetSteamInterface<ISteamApps>();
			auto UserInterface = Steam::GetSteamInterface<ISteamUser>();

			if (AppsInterface == nullptr || UserInterface == nullptr)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPrivilegesSteam::QueryUserPrivilege] Failed: The Steam client interfaces are not available. User [%s]"),
					*ToLogString(Params.LocalAccountId));

				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
				return;
			}

			// The results are a bitfield: every restriction which applies has to be reported, because the
			// caller decides on its own which of them it is willing to live with.
			UE::Online::EPrivilegeResults PrivilegeResults { UE::Online::EPrivilegeResults::NoFailures };

			// The Steam client keeps working while it is offline, so this says nothing about the user.
			if (!UserInterface->BLoggedOn())
			{
				PrivilegeResults |= UE::Online::EPrivilegeResults::NetworkConnectionUnavailable;
			}

			if (!IsSubscribedToApp(*AppsInterface))
			{
				PrivilegeResults |= UE::Online::EPrivilegeResults::AccountTypeFailure;

				// Buying the game is the only restriction here the user can do something about, and the
				// store page is the only dialog Steam offers for any of them. It is opened only when the
				// caller asks for it outright: a query answered in the background has no business taking
				// the screen away from whatever the user is doing.
				if (Params.ShowExternalUI == UE::Online::EShowPrivilegeResolutionUI::Show)
				{
					ShowStorePage();
				}
			}

			if (IsParentalLockEngaged())
			{
				PrivilegeResults |= UE::Online::EPrivilegeResults::AgeRestrictionFailure;
			}

			switch (Params.Privilege)
			{
				case UE::Online::EUserPrivileges::CanPlay:
					break;

				case UE::Online::EUserPrivileges::CanPlayOnline:
				case UE::Online::EUserPrivileges::CanCrossPlay:
					if (AppsInterface->BIsVACBanned())
					{
						PrivilegeResults |= UE::Online::EPrivilegeResults::OnlinePlayRestricted;
					}
					break;

				case UE::Online::EUserPrivileges::CanCommunicateViaTextOnline:
				case UE::Online::EUserPrivileges::CanCommunicateViaVoiceOnline:
					if (IsParentalFeatureBlocked(k_EFeatureFriends) || IsParentalFeatureBlocked(k_EFeatureCommunity))
					{
						PrivilegeResults |= UE::Online::EPrivilegeResults::ChatRestriction;
					}
					break;

				case UE::Online::EUserPrivileges::CanUseUserGeneratedContent:
					if (IsParentalFeatureBlocked(k_EFeatureCommunity))
					{
						PrivilegeResults |= UE::Online::EPrivilegeResults::UGCRestriction;
					}
					break;

				default:
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FPrivilegesSteam::QueryUserPrivilege] Unknown privilege [%s], reporting a generic failure. User [%s]"),
						LexToString(Params.Privilege), *ToLogString(Params.LocalAccountId));

					PrivilegeResults |= UE::Online::EPrivilegeResults::GenericFailure;
					break;
			}

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FPrivilegesSteam::QueryUserPrivilege] Succeeded: User [%s], Privilege [%s], Results [%s]"),
				*ToLogString(Params.LocalAccountId), LexToString(Params.Privilege), *LexToString(PrivilegeResults));

			InAsyncOp.SetResult(UE::Online::FQueryUserPrivilege::Result { .PrivilegeResult = PrivilegeResults });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}
}
