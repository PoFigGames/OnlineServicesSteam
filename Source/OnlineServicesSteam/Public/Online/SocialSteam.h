// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineComponentSteam.h"
#include "Online/SocialCommon.h"
#include "SteamUtils.h"

THIRD_PARTY_INCLUDES_START
#include "steam/isteamfriends.h"
THIRD_PARTY_INCLUDES_END


namespace PoFigGames::Online
{
	/**
	 * @class FSocialSteam
	 *
	 * @brief Steam Social Online Component
	 *
	 * The friends of a user belong to their Steam account rather than to the game, so the client already
	 * holds the list and reading it takes no request: a query walks what Steam knows and remembers it, and
	 * changes arrive as announcements afterwards.
	 *
	 * Changing a relationship is not something a game may do. Steam offers no call to send, accept or
	 * refuse a friend request, only the overlay pages where the user does it themselves, so those
	 * operations open the page and say that the user was asked; whether they went through with it arrives
	 * later, as the change of relationship Steam announces. Blocking has no page of its own and no call at
	 * all, and is answered as unimplemented rather than pretended to.
	 */
	class FSocialSteam : public TOnlineComponentSteam<UE::Online::FSocialCommon>
	{
	public:
		using Super = FSocialCommon;
		using TOnlineComponentSteam::TOnlineComponentSteam;

	#pragma region TOnlineComponent
		ONLINESERVICESSTEAM_API virtual void Initialize() override;
		ONLINESERVICESSTEAM_API virtual void PreShutdown() override;
	#pragma endregion TOnlineComponent

	#pragma region ISocial
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryFriends>       QueryFriends(UE::Online::FQueryFriends::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetFriends>                GetFriends(UE::Online::FGetFriends::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FSendFriendInvite>   SendFriendInvite(UE::Online::FSendFriendInvite::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FAcceptFriendInvite> AcceptFriendInvite(UE::Online::FAcceptFriendInvite::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FRejectFriendInvite> RejectFriendInvite(UE::Online::FRejectFriendInvite::Params&& Params) override;

		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryBlockedUsers> QueryBlockedUsers(UE::Online::FQueryBlockedUsers::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetBlockedUsers>          GetBlockedUsers(UE::Online::FGetBlockedUsers::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FBlockUser>         BlockUser(UE::Online::FBlockUser::Params&& Params) override;
	#pragma endregion ISocial

	private:
		using FFriendMap = TMap<UE::Online::FAccountId, TSharedRef<UE::Online::FFriend>>;

		/**
		 * Opens the page of the Steam overlay where the user changes a relationship themselves, which is
		 * the only way a game can ask for one.
		 */
		ONLINESERVICESSTEAM_API UE::Online::FOnlineError AskUserToChangeRelationship(const CSteamID& TargetSteamId, const ANSICHAR* DialogType, const TCHAR* Context) const;

		/** Walks the list Steam holds and remembers it, answering with what changed since last time. */
		ONLINESERVICESSTEAM_API void ReadFriends(const UE::Online::FAccountId& LocalAccountId);

		/** Steam announces a change of relationship to every listener. */
		ONLINESERVICESSTEAM_API void OnPersonaStateChange(PersonaStateChange_t* Message);

		/** Friends and pending invitations, by the local user they belong to. */
		TMap<UE::Online::FAccountId, FFriendMap> Friends { };

		/** Users the local user has blocked or ignored. */
		TMap<UE::Online::FAccountId, TSet<UE::Online::FAccountId>> BlockedUsers { };

		/** Registered while the component is alive; see Initialize and PreShutdown. */
		CCallbackManual<FSocialSteam, PersonaStateChange_t> OnPersonaStateChangeCallback { };
	};
}
