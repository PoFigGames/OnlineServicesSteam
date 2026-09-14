// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "SteamNetAddress.h"
#include "Online/LobbiesCommon.h"
#include "Online/OnlineComponentSteam.h"
#include "Steam/Wrappers/SteamLobby.h"
#include "SteamLobbyKeys.h"
#include "SteamUtils.h"

THIRD_PARTY_INCLUDES_START
#include "steam/isteamfriends.h"
#include "steam/isteammatchmaking.h"
THIRD_PARTY_INCLUDES_END

namespace PoFigGames::Online
{
	class FLobbyDataSteam;
	class FLobbyInviteDataSteam;
	class FLobbySearchSteam;
	class FLobbyDataRegistrySteam;


	struct FLobbyPrerequisitesSteam;


	// Declared inline rather than as static references: a reference at namespace scope binds to a
	// materialised temporary, giving every translation unit its own object, its own static initialiser and
	// its own heap allocation for the same four strings.
	inline const FString LOBBY_STEAM_ID_KEY_NAME { TEXT("LobbySteamId") };
	inline const FString LOBBY_DETAILS_KEY_NAME { TEXT("LobbyDetails") };
	inline const FString LOBBY_DATA_KEY_NAME { TEXT("LobbyData") };
	inline const FString LOBBY_SEARCH_KEY_NAME { TEXT("LobbySearch") };


	struct FLobbiesDestroyLobbyImpl
	{
		static constexpr TCHAR Name[] = TEXT("DestroyLobbyImpl");

		struct Params
		{
			// Which lobby goes.
			CSteamID LobbySteamId { k_steamIDNil };

			// The local user acting.
			UE::Online::FAccountId LocalAccountId { };
		};

		struct Result
		{
		};
	};

	struct FLobbiesInviteLobbyMemberImpl
	{
		static constexpr TCHAR Name[] = TEXT("InviteLobbyMemberImpl");

		struct Params
		{
			// The lobby handle data.
			TSharedPtr<FLobbyDataSteam> LobbyData { };

			// The local user acting.
			UE::Online::FAccountId LocalAccountId { };

			// Who is being invited.
			UE::Online::FAccountId TargetAccountId { };
		};

		struct Result
		{
		};
	};

	struct FLobbiesKickLobbyMemberImpl
	{
		static constexpr TCHAR Name[] = TEXT("KickLobbyMemberImpl");

		struct Params
		{
			// The lobby handle data.
			TSharedPtr<FLobbyDataSteam> LobbyData { };

			// The local user acting.
			UE::Online::FAccountId LocalAccountId { };

			// The target user to be kicked.
			UE::Online::FAccountId TargetAccountId { };
		};

		struct Result
		{
		};
	};

	struct FLobbiesPromoteLobbyMemberImpl
	{
		static constexpr TCHAR Name[] = TEXT("PromoteLobbyMemberImpl");

		struct Params
		{
			// The lobby handle data.
			TSharedPtr<FLobbyDataSteam> LobbyData { };

			// The local user acting.
			UE::Online::FAccountId LocalAccountId { };

			// Who becomes the owner.
			UE::Online::FAccountId TargetAccountId { };
		};

		struct Result
		{
		};
	};

	/**
	 * @struct FLobbiesModifyLobbyDataImpl
	 *
	 * @brief This class is responsible for handling modifications to the lobby data.
	 * It provides the implementation required to update, add, or remove lobby-related information in a structured and efficient manner.
	 */
	struct FLobbiesModifyLobbyDataImpl
	{
		static constexpr TCHAR Name[] = TEXT("ModifyLobbyDataImpl");

		struct Params
		{
			// The lobby handle data.
			TSharedPtr<FLobbyDataSteam> LobbyData { nullptr };

			// The local user acting.
			UE::Online::FAccountId LocalAccountId { };

			/** Translated changes to be applied to the service. */
			UE::Online::FLobbyClientServiceChanges ServiceChanges { };
		};

		struct Result
		{
		};
	};

	/**
	 * @struct FLobbiesLeaveLobbyImpl
	 *
	 * @brief This class handles the process of leaving a lobby.
	 * It provides the implementation necessary to manage the disconnection of a user from an existing lobby, ensuring proper cleanup and state updates.
	 */
	struct FLobbiesLeaveLobbyImpl
	{
		static constexpr TCHAR Name[] = TEXT("LeaveLobbyImpl");

		struct Params
		{
			// The lobby handle data.
			TSharedPtr<FLobbyDataSteam> LobbyData { nullptr };

			// The local user acting.
			UE::Online::FAccountId LocalAccountId { };
		};

		struct Result
		{
		};
	};

	/**
	 * @struct FLobbiesProcessLobbyNotificationImpl
	 */
	struct FLobbiesProcessLobbyNotificationImpl
	{
		static constexpr TCHAR Name[] = TEXT("ProcessLobbyNotificationImpl");

		struct Params
		{
			// The lobby handle data.
			TSharedPtr<FLobbyDataSteam> LobbyData { nullptr };

			// Joining / mutated members.
			TSet<CSteamID> MutatedMembers { };

			// Leaving members.
			TMap<CSteamID, UE::Online::ELobbyMemberLeaveReason> LeavingMembers { };
		};

		// Todo: += operator.
		// Merged by lobby, so two requests for one lobby become one.

		struct Result
		{
		};
	};

	/**
	 * @class FLobbiesSteam
	 *
	 * @brief Steam Lobbies Online Component.
	 *
	 * Two operations of the interface stay with the default implementation on purpose. RestoreLobbies
	 * has nothing to restore: Steam drops a user from every lobby the moment their client goes away, so
	 * a lobby never outlives the session that created it. ModifyLobbySchema cannot be written at all
	 * yet, because the engine declares its parameter struct empty and therefore says neither which
	 * lobby to change nor which schema to change it to.
	 */
	class FLobbiesSteam : public TOnlineComponentSteam<UE::Online::FLobbiesCommon>
	{
	public:
		using Super = FLobbiesCommon;
		ONLINESERVICESSTEAM_API FLobbiesSteam(UE::Online::FOnlineServicesCommon& InServices);

		ONLINESERVICESSTEAM_API virtual void Initialize() override;
		ONLINESERVICESSTEAM_API virtual void Tick(float DeltaSeconds) override;
		ONLINESERVICESSTEAM_API virtual void PreShutdown() override;

		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FCreateLobby>                 CreateLobby(UE::Online::FCreateLobby::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FFindLobbies>                 FindLobbies(UE::Online::FFindLobbies::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FJoinLobby>                   JoinLobby(UE::Online::FJoinLobby::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FLeaveLobby>                  LeaveLobby(UE::Online::FLeaveLobby::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FInviteLobbyMember>           InviteLobbyMember(UE::Online::FInviteLobbyMember::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FDeclineLobbyInvitation>      DeclineLobbyInvitation(UE::Online::FDeclineLobbyInvitation::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FKickLobbyMember>             KickLobbyMember(UE::Online::FKickLobbyMember::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FPromoteLobbyMember>          PromoteLobbyMember(UE::Online::FPromoteLobbyMember::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FModifyLobbyJoinPolicy>       ModifyLobbyJoinPolicy(UE::Online::FModifyLobbyJoinPolicy::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FModifyLobbyAttributes>       ModifyLobbyAttributes(UE::Online::FModifyLobbyAttributes::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FModifyLobbyMemberAttributes> ModifyLobbyMemberAttributes(UE::Online::FModifyLobbyMemberAttributes::Params&& Params) override;

		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetJoinedLobbies> GetJoinedLobbies(UE::Online::FGetJoinedLobbies::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetReceivedInvitations> GetReceivedInvitations(UE::Online::FGetReceivedInvitations::Params&& Params) override;

	protected:
		ONLINESERVICESSTEAM_API void AddActiveInvite(const TSharedRef<FLobbyInviteDataSteam>& Invite);
		ONLINESERVICESSTEAM_API void RemoveActiveInvite(const TSharedRef<FLobbyInviteDataSteam>& Invite);
		ONLINESERVICESSTEAM_API TSharedPtr<FLobbyInviteDataSteam> GetActiveInvite(UE::Online::FAccountId TargetUser, UE::Online::FLobbyId TargetLobbyId);

		ONLINESERVICESSTEAM_API TFuture<UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>>         LeaveLobbyImpl(FLobbiesLeaveLobbyImpl::Params&& Params) const;
		ONLINESERVICESSTEAM_API TFuture<UE::Online::TDefaultErrorResult<FLobbiesDestroyLobbyImpl>>       DestroyLobbyImpl(FLobbiesDestroyLobbyImpl::Params&& Params) const;
		ONLINESERVICESSTEAM_API TFuture<UE::Online::TDefaultErrorResult<FLobbiesInviteLobbyMemberImpl>>  InviteLobbyMemberImpl(FLobbiesInviteLobbyMemberImpl::Params&& Params) const;
		ONLINESERVICESSTEAM_API TFuture<UE::Online::TDefaultErrorResult<FLobbiesKickLobbyMemberImpl>>    KickLobbyMemberImpl(FLobbiesKickLobbyMemberImpl::Params&& Params);
		ONLINESERVICESSTEAM_API TFuture<UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>> PromoteLobbyMemberImpl(FLobbiesPromoteLobbyMemberImpl::Params&& Params) const;
		ONLINESERVICESSTEAM_API TFuture<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>    ModifyLobbyDataImpl(FLobbiesModifyLobbyDataImpl::Params&& Params) const;
		ONLINESERVICESSTEAM_API UE::Online::TOnlineAsyncOpHandle<FLobbiesProcessLobbyNotificationImpl>   ProcessLobbyNotificationImplOp(FLobbiesProcessLobbyNotificationImpl::Params&& Params);

	private:
		TSharedPtr<FLobbyPrerequisitesSteam> LobbyPrerequisites { nullptr };
		TSharedPtr<FLobbyDataRegistrySteam> LobbyDataRegistry { nullptr };

		TMap<UE::Online::FAccountId, TSet<TSharedRef<FLobbyDataSteam>>>                             ActiveLobbies { };
		TMap<UE::Online::FAccountId, TMap<UE::Online::FLobbyId, TSharedRef<FLobbyInviteDataSteam>>> ActiveInvites { };
		TMap<UE::Online::FAccountId, TSharedRef<FLobbySearchSteam>>                                 ActiveSearchResults { };

		ONLINESERVICESSTEAM_API void AddActiveLobby(UE::Online::FAccountId LocalAccountId, bool bPresenceEnabled, const TSharedRef<FLobbyDataSteam>& LobbyData);
		ONLINESERVICESSTEAM_API void RemoveActiveLobby(UE::Online::FAccountId LocalAccountId, const TSharedRef<FLobbyDataSteam>& LobbyData);

		/** Runs one notification through the lobby state machine and logs whatever came of it. */
		ONLINESERVICESSTEAM_API void ProcessLobbyNotification(const TSharedRef<FLobbyDataSteam>& LobbyData, const TCHAR* Context, FLobbiesProcessLobbyNotificationImpl::Params&& Params);

	public:
		/** Address of the game server bound to a lobby, or nothing while its host has started none. */
		ONLINESERVICESSTEAM_API TOptional<Steam::Wrappers::FSteamLobbyGameServer> GetLobbyGameServer(UE::Online::FLobbyId LobbyId) const;

	protected:

		/** Leaves a lobby this user was removed from and reports the reason to the game. */
		ONLINESERVICESSTEAM_API void HandleLocalUserKicked(const TSharedRef<FLobbyDataSteam>& LobbyData, UE::Online::FAccountId LocalAccountId);

		/** Turns a lobby the user asked to join outside of the game into a join request the game can act on. */
		ONLINESERVICESSTEAM_API void RequestUiJoin(CSteamID LobbySteamId, UE::Online::FAccountId LocalAccountId, UE::Online::EUILobbyJoinRequestedSource Source);

		/**
		 * Binds the server this process hosts to a lobby it owns, if it hosts one and owns one.
		 *
		 * Steam mirrors the address into the metadata of the lobby by itself and tells the members that
		 * the lobby now has a server, so nothing about the address is published by hand.
		 */
		ONLINESERVICESSTEAM_API void BindGameServerToOwnedLobby(const TSharedPtr<FLobbyDataSteam>& LobbyData);

		/** Answers the two moments a binding can be needed: a server which starts, and a lobby which is created. */
		ONLINESERVICESSTEAM_API void OnListenAddressChanged(const Steam::FSteamNetAddress& ListenAddress);

		ONLINESERVICESSTEAM_API void OnLobbyDataUpdate(LobbyDataUpdate_t* Message);
		ONLINESERVICESSTEAM_API void OnLobbyChatUpdate(LobbyChatUpdate_t* Message);
		ONLINESERVICESSTEAM_API void OnLobbyChatMessage(LobbyChatMsg_t* Message);
		ONLINESERVICESSTEAM_API void OnLobbyInvite(LobbyInvite_t* Message);
		ONLINESERVICESSTEAM_API void OnGameLobbyJoinRequested(GameLobbyJoinRequested_t* Message);

		// Registered for the lifetime of the component and unregistered in PreShutdown, before the state
		// these handlers rely on is released.
		CCallback<FLobbiesSteam, LobbyDataUpdate_t>        OnLobbyDataUpdateCallback;
		CCallback<FLobbiesSteam, LobbyChatUpdate_t>        OnLobbyChatUpdateCallback;
		CCallback<FLobbiesSteam, LobbyChatMsg_t>           OnLobbyChatMessageCallback;
		CCallback<FLobbiesSteam, LobbyInvite_t>            OnLobbyInviteCallback;
		CCallback<FLobbiesSteam, GameLobbyJoinRequested_t> OnGameLobbyJoinRequestedCallback;

		/** Bound for the lifetime of the component; see Initialize and PreShutdown. */
		FDelegateHandle OnListenAddressChangedHandle { };

		/**
		 * Members this host has removed but which Steam has not reported as gone yet, so that their
		 * departure is reported to the game as a removal rather than as an ordinary leave.
		 */
		TMap<UE::Online::FLobbyId, TSet<CSteamID>> PendingKicks { };

		/**
		 * Lobby named by the +connect_lobby argument Steam adds when the game is launched to answer an
		 * invitation. It cannot be acted on until a user is signed in, which is what the tick below waits
		 * for; it is cleared as soon as it has been handled once.
		 */
		CSteamID PendingCommandLineLobby { k_steamIDNil };
	};
}

namespace UE::Online::Meta {
	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesLeaveLobbyImpl::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesLeaveLobbyImpl::Params, LobbyData),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesLeaveLobbyImpl::Params, LocalAccountId)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesLeaveLobbyImpl::Result)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesDestroyLobbyImpl::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesDestroyLobbyImpl::Params, LobbySteamId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesDestroyLobbyImpl::Params, LocalAccountId)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesDestroyLobbyImpl::Result)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesInviteLobbyMemberImpl::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesInviteLobbyMemberImpl::Params, LobbyData),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesInviteLobbyMemberImpl::Params, LocalAccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesInviteLobbyMemberImpl::Params, TargetAccountId)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesInviteLobbyMemberImpl::Result)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesKickLobbyMemberImpl::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesKickLobbyMemberImpl::Params, LobbyData),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesKickLobbyMemberImpl::Params, LocalAccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesKickLobbyMemberImpl::Params, TargetAccountId)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesKickLobbyMemberImpl::Result)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesPromoteLobbyMemberImpl::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesPromoteLobbyMemberImpl::Params, LobbyData),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesPromoteLobbyMemberImpl::Params, LocalAccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesPromoteLobbyMemberImpl::Params, TargetAccountId)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesPromoteLobbyMemberImpl::Result)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesModifyLobbyDataImpl::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesModifyLobbyDataImpl::Params, LobbyData),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesModifyLobbyDataImpl::Params, LocalAccountId)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesModifyLobbyDataImpl::Result)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesProcessLobbyNotificationImpl::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesProcessLobbyNotificationImpl::Params, LobbyData),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesProcessLobbyNotificationImpl::Params, MutatedMembers),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FLobbiesProcessLobbyNotificationImpl::Params, LeavingMembers)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FLobbiesProcessLobbyNotificationImpl::Result)
	END_ONLINE_STRUCT_META()
}
