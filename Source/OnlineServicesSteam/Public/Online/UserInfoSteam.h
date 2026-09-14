// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineComponentSteam.h"
#include "Online/UserInfoCommon.h"
#include "Steam/Wrappers/SteamUserInfo.h"


namespace PoFigGames::Online
{
	/**
	 * @enum EUserProfileDialogType
	 *
	 * @brief Page of the Steam overlay a profile request opens.
	 */
	enum class EUserProfileDialogType
	{
		// opens the overlay web browser to the specified user or groups profile
		SteamId,
		// opens a chat window to the specified user, or joins the group chat
		Chat,
		// opens a window to a Steam Trading session that was started with the ISteamEconomy/StartTrade Web API
		JoinTrade,
		// opens the overlay web browser to the specified user's stats
		Stats,
		// opens the overlay web browser to the specified user's achievements
		Achievements,
		// opens the overlay in minimal mode, prompting the user to add the target user as a friend
		FriendAdd,
		// opens the overlay in minimal mode, prompting the user to remove the target friend
		FriendRemove,
		// opens the overlay in minimal mode, prompting the user to accept an incoming friend invite
		FriendRequestAccept,
		// opens the overlay in minimal mode, prompting the user to ignore an incoming friend invite
		FriendRequestIgnore
	};

	/**
	 * @struct FQueryUserInfoSteam
	 *
	 * @brief Queries the persona data of a set of users, saying how much of it is wanted.
	 *
	 * The operation of the engine carries no such choice, so it asks for the name alone. A caller who is
	 * about to show avatars as well goes through this one instead and saves the second round trip.
	 */
	struct FQueryUserInfoSteam
	{
		static constexpr TCHAR Name[] = TEXT("QueryUserInfoSteam");

		/** Input struct for FUserInfoSteam::QueryUserInfo */
		struct Params
		{
			/** Local user the query is made on behalf of. */
			UE::Online::FAccountId LocalAccountId { };

			/** Users whose persona data is wanted. */
			TArray<UE::Online::FAccountId> AccountIds { };

			/**
			 * Whether the persona name alone is enough. Avatars are slow to fetch and churn the local Steam
			 * cache, so Valve asks that they only be requested where they are actually shown.
			 */
			bool bRequireNameOnly { true };
		};

		/**
		 * Output struct for FUserInfoSteam::QueryUserInfo
		 * Read the result back with GetUserInfo.
		 */
		struct Result
		{
		};
	};

	/**
	 * @struct FQueryUserAvatarSteam
	 *
	 * @brief Fetches and caches the avatars of a set of users in one of the three sizes Steam keeps.
	 *
	 * The operation of the engine names no size, so it asks for the medium one, which is what a login
	 * caches for the local user as well.
	 */
	struct FQueryUserAvatarSteam
	{
		static constexpr TCHAR Name[] = TEXT("QueryUserAvatarSteam");

		/** Input struct for FUserInfoSteam::QueryUserAvatar */
		struct Params
		{
			/** Local user the query is made on behalf of. */
			UE::Online::FAccountId LocalAccountId { };

			/** Users whose avatar is wanted. */
			TArray<UE::Online::FAccountId> AccountIds { };

			/** Which of the three images Steam keeps per user to fetch. */
			Steam::EAvatarImageSize AvatarSize { Steam::EAvatarImageSize::Medium };
		};

		/**
		 * Output struct for FUserInfoSteam::QueryUserAvatar
		 * Obtain the cached avatar via GetUserAvatar
		 */
		struct Result
		{
		};
	};

	/**
	 * @struct FGetUserAvatarSteam
	 *
	 * @brief Reads back the avatar a query cached, in the size it was asked for.
	 */
	struct FGetUserAvatarSteam
	{
		static constexpr TCHAR Name[] = TEXT("GetUserAvatarSteam");

		/** Input struct for FUserInfoSteam::GetUserAvatar */
		struct Params
		{
			/** Local user the avatar is read on behalf of. */
			UE::Online::FAccountId LocalAccountId { };

			/** User whose avatar is wanted. */
			UE::Online::FAccountId AccountId { };

			/** Size the avatar was queried in; another size is a different image and a different file. */
			Steam::EAvatarImageSize AvatarSize { Steam::EAvatarImageSize::Medium };
		};

		/** Output struct for FUserInfoSteam::GetUserAvatar */
		struct Result
		{
			/* Avatar Url */
			FString AvatarUrl { };
		};
	};

	/**
	 * @struct FShowUserProfileSteam
	 *
	 * @brief Opens a page of the Steam overlay on a user.
	 *
	 * The operation of the engine names no page, so it opens the profile. Everything else the overlay can
	 * do about a user, from adding a friend to answering an invite, is reachable through this one.
	 */
	struct FShowUserProfileSteam
	{
		static constexpr TCHAR Name[] = TEXT("ShowUserProfileSteam");

		/** Input struct for FUserInfoSteam::ShowUserProfile */
		struct Params
		{
			/** Local user the overlay is opened for. */
			UE::Online::FAccountId LocalAccountId { };

			/** User the page is about. */
			UE::Online::FAccountId AccountId { };

			/** Page of the overlay to open. */
			EUserProfileDialogType DialogType { EUserProfileDialogType::SteamId };
		};

		/** Output struct for FUserInfoSteam::ShowUserProfile */
		struct Result
		{
		};
	};

	/**
	 * @class FUserInfoSteam
	 *
	 * @brief Steam User Info Online Component
	 *
	 * Steam answers a persona query with a broadcast callback rather than with a result, so a query only
	 * waits for the data to arrive; reading it back afterwards is a synchronous call into the Steam cache.
	 *
	 * Each operation comes in two forms: the one of the engine, which is what the game and CommonUser call,
	 * and a Steam one carrying the choices the engine parameters have no room for. The two share their
	 * implementation, so the Steam form adds no second code path to keep working.
	 */
	class FUserInfoSteam : public TOnlineComponentSteam<UE::Online::FUserInfoCommon>
	{
	public:
		using Super = FUserInfoCommon;
		using TOnlineComponentSteam::TOnlineComponentSteam;

	#pragma region IUserInfo
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryUserInfo> QueryUserInfo(UE::Online::FQueryUserInfo::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetUserInfo> GetUserInfo(UE::Online::FGetUserInfo::Params&& Params) override;

		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryUserAvatar> QueryUserAvatar(UE::Online::FQueryUserAvatar::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetUserAvatar> GetUserAvatar(UE::Online::FGetUserAvatar::Params&& Params) override;

		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FShowUserProfile> ShowUserProfile(UE::Online::FShowUserProfile::Params&& Params) override;
		#pragma endregion IUserInfo

		/** Queries persona data, saying whether the avatars are wanted along with the names. */
		ONLINESERVICESSTEAM_API UE::Online::TOnlineAsyncOpHandle<FQueryUserInfoSteam> QueryUserInfo(FQueryUserInfoSteam::Params&& Params);

		/** Fetches and caches avatars in the size the caller is going to show. */
		ONLINESERVICESSTEAM_API UE::Online::TOnlineAsyncOpHandle<FQueryUserAvatarSteam> QueryUserAvatar(FQueryUserAvatarSteam::Params&& Params);

		/** Reads back an avatar cached in a size other than the default one. */
		ONLINESERVICESSTEAM_API UE::Online::TOnlineResult<FGetUserAvatarSteam> GetUserAvatar(FGetUserAvatarSteam::Params&& Params);

		/** Opens a page of the Steam overlay other than the profile of a user. */
		ONLINESERVICESSTEAM_API UE::Online::TOnlineAsyncOpHandle<FShowUserProfileSteam> ShowUserProfile(FShowUserProfileSteam::Params&& Params);

	private:
		/**
		 * Bodies shared by the two forms of each operation. They are written against any operation whose
		 * parameters name the local user and the targets, which both forms do, and take what the engine
		 * parameters cannot carry as an argument.
		 */
		template<typename OpType>
		void EnqueueQueryUserInfo(const UE::Online::TOnlineAsyncOpRef<OpType>& Op, bool bRequireNameOnly);

		template<typename OpType>
		void EnqueueQueryUserAvatar(const UE::Online::TOnlineAsyncOpRef<OpType>& Op, Steam::EAvatarImageSize AvatarSize);

		template<typename OpType>
		void EnqueueShowUserProfile(const UE::Online::TOnlineAsyncOpRef<OpType>& Op, EUserProfileDialogType DialogType);

		/** Path of a cached avatar, or the reason the user has none in that size. */
		UE::Online::TDefaultErrorResultInternal<FString> ResolveCachedAvatar(const UE::Online::FAccountId& LocalAccountId,
			const UE::Online::FAccountId& AccountId, Steam::EAvatarImageSize AvatarSize) const;
	};
}

namespace UE::Online::Meta {
	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FQueryUserInfoSteam::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FQueryUserInfoSteam::Params, LocalAccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FQueryUserInfoSteam::Params, AccountIds),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FQueryUserInfoSteam::Params, bRequireNameOnly)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FQueryUserInfoSteam::Result)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FQueryUserAvatarSteam::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FQueryUserAvatarSteam::Params, LocalAccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FQueryUserAvatarSteam::Params, AccountIds),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FQueryUserAvatarSteam::Params, AvatarSize)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FQueryUserAvatarSteam::Result)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FGetUserAvatarSteam::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FGetUserAvatarSteam::Params, LocalAccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FGetUserAvatarSteam::Params, AccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FGetUserAvatarSteam::Params, AvatarSize)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FGetUserAvatarSteam::Result)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FGetUserAvatarSteam::Result, AvatarUrl)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FShowUserProfileSteam::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FShowUserProfileSteam::Params, LocalAccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FShowUserProfileSteam::Params, AccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FShowUserProfileSteam::Params, DialogType)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FShowUserProfileSteam::Result)
	END_ONLINE_STRUCT_META()
}
