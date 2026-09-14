// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/UserInfoSteam.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamInterfaces.h"
#include "Online/OnlineServicesSteam.h"
#include "Online/UserInfoUtils.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/SteamResult.h"
#include "Steam/Wrappers/SteamUserInfo.h"

// Engine
#include "Misc/Paths.h"


namespace PoFigGames::Online
{
	namespace Private
	{
		/** Name Steam knows a page of its overlay by. */
		static const char* TranslateDialogType(const EUserProfileDialogType DialogType)
		{
			switch (DialogType)
			{
				case EUserProfileDialogType::Chat:
					return "chat";
				case EUserProfileDialogType::JoinTrade:
					return "jointrade";
				case EUserProfileDialogType::Stats:
					return "stats";
				case EUserProfileDialogType::Achievements:
					return "achievements";
				case EUserProfileDialogType::FriendAdd:
					return "friendadd";
				case EUserProfileDialogType::FriendRemove:
					return "friendremove";
				case EUserProfileDialogType::FriendRequestAccept:
					return "friendrequestaccept";
				case EUserProfileDialogType::FriendRequestIgnore:
					return "friendrequestignore";
				case EUserProfileDialogType::SteamId:
					break;
			}

			return "steamid";
		}
	}

	template<typename OpType>
	void FUserInfoSteam::EnqueueQueryUserInfo(const UE::Online::TOnlineAsyncOpRef<OpType>& Op, const bool bRequireNameOnly)
	{
		Op->Then([this](UE::Online::TOnlineAsyncOp<OpType>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			const UE::Online::IAuthPtr Auth = Services.GetAuthInterface();
			if (!Auth.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserInfo] Failed: Authentication Interface is not found"));
				return InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}

			if (!Auth->IsLoggedIn(Params.LocalAccountId))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserInfo] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
				return InAsyncOp.SetError(UE::Online::Errors::NotLoggedIn());
			}

			if (Params.AccountIds.IsEmpty())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserInfo] Failed: No users to query."));
				return InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
			}
		});

		// The targets are known before the operation runs, so every request is chained up front; a step of a
		// failed operation is never entered, which is what stops the chain at the first error.
		for (const auto& TargetAccountId : Op->GetParams().AccountIds)
		{
			Op->Then([this, TargetAccountId, bRequireNameOnly](UE::Online::TOnlineAsyncOp<OpType>& /*InAsyncOp*/)
			{
				Steam::Wrappers::FSteamUserInfo::Params UserInfoParams { .UserId = GetSteamUserId(TargetAccountId), .bRequireNameOnly = bRequireNameOnly };
				return SteamListen<Steam::Wrappers::FSteamUserInfo>(MoveTemp(UserInfoParams));
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamUserInfo>(TEXT("FUserInfoSteam::QueryUserInfo"),
				[this, TargetAccountId](UE::Online::TOnlineAsyncOp<OpType>& InAsyncOp, Steam::Wrappers::FSteamUserInfo::Result&& Result)
			{
				if (!Result.UserId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserInfo] Failed: Invalid steam User Id. User [%s]"), *ToLogString(TargetAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidResults());
				}
			}));
		}

		Op->Then([](UE::Online::TOnlineAsyncOp<OpType>& InAsyncOp)
		{
			InAsyncOp.SetResult({ });
		});

		Op->Enqueue(GetSerialQueue());
	}

	template<typename OpType>
	void FUserInfoSteam::EnqueueQueryUserAvatar(const UE::Online::TOnlineAsyncOpRef<OpType>& Op, const Steam::EAvatarImageSize AvatarSize)
	{
		Op->Then([this, AvatarSize](UE::Online::TOnlineAsyncOp<OpType>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			const UE::Online::IAuthPtr Auth = Services.GetAuthInterface();
			if (!Auth.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserAvatar] Failed: Authentication Interface is not found"));
				return InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}

			if (!Auth->IsLoggedIn(Params.LocalAccountId))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserAvatar] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
				return InAsyncOp.SetError(UE::Online::Errors::NotLoggedIn());
			}

			if (Params.AccountIds.IsEmpty())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserAvatar] Failed: No users to query."));
				return InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
			}

			if (AvatarSize == Steam::EAvatarImageSize::Invalid)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserAvatar] Failed: No avatar size requested."));
				return InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
			}
		});

		for (const auto& TargetAccountId : Op->GetParams().AccountIds)
		{
			Op->Then([this, TargetAccountId, AvatarSize](UE::Online::TOnlineAsyncOp<OpType>& /*InAsyncOp*/)
			{
				Steam::Wrappers::FSteamUserAvatar::Params AvatarParams { .UserId = GetSteamUserId(TargetAccountId), .AvatarSize = AvatarSize };
				return SteamListen<Steam::Wrappers::FSteamUserAvatar>(MoveTemp(AvatarParams));
			})
			// Reading the image calls the Steam API, so it stays on the game thread.
			.Then([this, TargetAccountId, AvatarSize](UE::Online::TOnlineAsyncOp<OpType>& InAsyncOp,
				Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>&& AvatarResult) -> FSaveUserAvatar::Params
			{
				if (AvatarResult.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserAvatar] Failed: User [%s], Result [%s]"),
						*ToLogString(TargetAccountId), *AvatarResult.GetErrorValue().GetLogString());

					InAsyncOp.SetError(MoveTemp(AvatarResult.GetErrorValue()));
					return FSaveUserAvatar::Params { };
				}

				const auto& Avatar = AvatarResult.GetOkValue();

				auto ReadResult = FUserInfoUtils::ReadAvatarImage({
					.SteamId = Avatar.UserId.ConvertToUint64(),
					.AvatarSize = AvatarSize,
					.ImageIndex = Avatar.ImageIndex,
					.ImageWidth = Avatar.ImageWidth,
					.ImageHeight = Avatar.ImageHeight });

				if (ReadResult.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserAvatar] Failed to read the avatar image. User [%s], Result [%s]"),
						*ToLogString(TargetAccountId), *ReadResult.GetErrorValue().GetLogString());

					InAsyncOp.SetError(MoveTemp(ReadResult.GetErrorValue()));
					return FSaveUserAvatar::Params { };
				}

				return FSaveUserAvatar::Params { .Image = MoveTemp(ReadResult.GetOkValue().Image) };
			})
			// Encoding and writing the file touch no Steam API, so they run off the game thread.
			.Then([](UE::Online::TOnlineAsyncOp<OpType>& /*InAsyncOp*/, FSaveUserAvatar::Params&& SaveParams)
			{
				return FUserInfoUtils::SaveAvatarImageToFile(MoveTemp(SaveParams));
			}, UE::Online::FOnlineAsyncExecutionPolicy::RunOnThreadPool())
			.Then([this, TargetAccountId](UE::Online::TOnlineAsyncOp<OpType>& InAsyncOp,
				UE::Online::TDefaultErrorResult<FSaveUserAvatar>&& SaveResult)
			{
				if (SaveResult.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::QueryUserAvatar] Failed to cache the avatar image. User [%s], Result [%s]"),
						*ToLogString(TargetAccountId), *SaveResult.GetErrorValue().GetLogString());

					InAsyncOp.SetError(MoveTemp(SaveResult.GetErrorValue()));
				}
			});
		}

		Op->Then([](UE::Online::TOnlineAsyncOp<OpType>& InAsyncOp)
		{
			InAsyncOp.SetResult({ });
		});

		Op->Enqueue(GetSerialQueue());
	}

	template<typename OpType>
	void FUserInfoSteam::EnqueueShowUserProfile(const UE::Online::TOnlineAsyncOpRef<OpType>& Op, const EUserProfileDialogType DialogType)
	{
		Op->Then([this, DialogType](UE::Online::TOnlineAsyncOp<OpType>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			auto TargetUser = ResolveSteamUser(Params.LocalAccountId, Params.AccountId, TEXT("FUserInfoSteam::ShowUserProfile"));
			if (TargetUser.IsError())
			{
				return InAsyncOp.SetError(MoveTemp(TargetUser.GetErrorValue()));
			}

			const auto Friends = Steam::GetSteamInterface<ISteamFriends>();
			if (Friends == nullptr)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::ShowUserProfile] Failed: Steam friends interface is not available"));
				return InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}

			if (auto Error = CheckOverlayAvailable(TEXT("FUserInfoSteam::ShowUserProfile")); Error != UE::Online::Errors::Success())
			{
				return InAsyncOp.SetError(MoveTemp(Error));
			}

			Friends->ActivateGameOverlayToUser(Private::TranslateDialogType(DialogType), TargetUser.GetOkValue());

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());
	}

	UE::Online::TDefaultErrorResultInternal<FString> FUserInfoSteam::ResolveCachedAvatar(const UE::Online::FAccountId& LocalAccountId,
		const UE::Online::FAccountId& AccountId, const Steam::EAvatarImageSize AvatarSize) const
	{
		auto TargetUser = ResolveSteamUser(LocalAccountId, AccountId, TEXT("FUserInfoSteam::GetUserAvatar"));
		if (TargetUser.IsError())
		{
			return UE::Online::TDefaultErrorResultInternal<FString>(MoveTemp(TargetUser.GetErrorValue()));
		}

		if (AvatarSize == Steam::EAvatarImageSize::Invalid)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::GetUserAvatar] Failed: No avatar size requested."));
			return UE::Online::TDefaultErrorResultInternal<FString>(UE::Online::Errors::InvalidParams());
		}

		FString ImagePath = FUserInfoUtils::GetAvatarUrl(TargetUser.GetOkValue().ConvertToUint64(), AvatarSize);
		if (!FPaths::FileExists(ImagePath))
		{
			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FUserInfoSteam::GetUserAvatar] No avatar cached. User [%s]"), *ToLogString(AccountId));
			return UE::Online::TDefaultErrorResultInternal<FString>(UE::Online::Errors::NotFound());
		}

		return UE::Online::TDefaultErrorResultInternal<FString>(MoveTemp(ImagePath));
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryUserInfo> FUserInfoSteam::QueryUserInfo(UE::Online::FQueryUserInfo::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FQueryUserInfo>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			// The engine operation has no room for the choice, and only the display name is read back
			// through it, so the avatars are left to QueryUserAvatar.
			EnqueueQueryUserInfo(Op, true);
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<FQueryUserInfoSteam> FUserInfoSteam::QueryUserInfo(FQueryUserInfoSteam::Params&& InParams)
	{
		const auto Op = GetJoinableOp<FQueryUserInfoSteam>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			EnqueueQueryUserInfo(Op, Op->GetParams().bRequireNameOnly);
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FGetUserInfo> FUserInfoSteam::GetUserInfo(UE::Online::FGetUserInfo::Params&& InParams)
	{
		auto TargetUser = ResolveSteamUser(InParams.LocalAccountId, InParams.AccountId, TEXT("FUserInfoSteam::GetUserInfo"));
		if (TargetUser.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FGetUserInfo>(MoveTemp(TargetUser.GetErrorValue()));
		}

		const auto Friends = Steam::GetSteamInterface<ISteamFriends>();
		if (Friends == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoSteam::GetUserInfo] Failed: Steam friends interface is not available"));
			return UE::Online::TOnlineResult<UE::Online::FGetUserInfo>(UE::Online::Errors::MissingInterface());
		}

		// Steam answers with a placeholder name for a user it knows nothing about yet, which would read as
		// an answer. Asking for the information again reports whether it is there: a request is only started
		// when it is missing, and the query which then follows is the one the caller should have run first.
		if (Friends->RequestUserInformation(TargetUser.GetOkValue(), true))
		{
			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FUserInfoSteam::GetUserInfo] No persona data cached yet. User [%s]"), *ToLogString(InParams.AccountId));
			return UE::Online::TOnlineResult<UE::Online::FGetUserInfo>(UE::Online::Errors::NotFound());
		}

		auto UserInfo = MakeShared<UE::Online::FUserInfo>();

		UserInfo->AccountId = InParams.AccountId;
		UserInfo->DisplayName = StringCast<TCHAR>(Friends->GetFriendPersonaName(TargetUser.GetOkValue())).Get();

		return UE::Online::TOnlineResult<UE::Online::FGetUserInfo>({ .UserInfo = MoveTemp(UserInfo) });
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryUserAvatar> FUserInfoSteam::QueryUserAvatar(UE::Online::FQueryUserAvatar::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FQueryUserAvatar>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			// The engine operation names no size, so it asks for the one a login caches for the local user.
			EnqueueQueryUserAvatar(Op, DefaultAvatarSize);
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<FQueryUserAvatarSteam> FUserInfoSteam::QueryUserAvatar(FQueryUserAvatarSteam::Params&& InParams)
	{
		const auto Op = GetJoinableOp<FQueryUserAvatarSteam>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			EnqueueQueryUserAvatar(Op, Op->GetParams().AvatarSize);
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FGetUserAvatar> FUserInfoSteam::GetUserAvatar(UE::Online::FGetUserAvatar::Params&& InParams)
	{
		auto AvatarResult = ResolveCachedAvatar(InParams.LocalAccountId, InParams.AccountId, DefaultAvatarSize);
		if (AvatarResult.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FGetUserAvatar>(MoveTemp(AvatarResult.GetErrorValue()));
		}

		return UE::Online::TOnlineResult<UE::Online::FGetUserAvatar>({ .AvatarUrl = MoveTemp(AvatarResult.GetOkValue()) });
	}

	UE::Online::TOnlineResult<FGetUserAvatarSteam> FUserInfoSteam::GetUserAvatar(FGetUserAvatarSteam::Params&& InParams)
	{
		auto AvatarResult = ResolveCachedAvatar(InParams.LocalAccountId, InParams.AccountId, InParams.AvatarSize);
		if (AvatarResult.IsError())
		{
			return UE::Online::TOnlineResult<FGetUserAvatarSteam>(MoveTemp(AvatarResult.GetErrorValue()));
		}

		return UE::Online::TOnlineResult<FGetUserAvatarSteam>({ .AvatarUrl = MoveTemp(AvatarResult.GetOkValue()) });
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FShowUserProfile> FUserInfoSteam::ShowUserProfile(UE::Online::FShowUserProfile::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FShowUserProfile>(MoveTemp(InParams));

		// The engine operation names no page, so it opens the profile of the user.
		EnqueueShowUserProfile(Op, EUserProfileDialogType::SteamId);

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<FShowUserProfileSteam> FUserInfoSteam::ShowUserProfile(FShowUserProfileSteam::Params&& InParams)
	{
		const auto Op = GetOp<FShowUserProfileSteam>(MoveTemp(InParams));

		EnqueueShowUserProfile(Op, Op->GetParams().DialogType);

		return Op->GetHandle();
	}
}
