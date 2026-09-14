// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Steam/SteamCallTraits.h"
#include "Steam/SteamResult.h"
#include "SteamInterfaces.h"


namespace PoFigGames::Steam
{
	static constexpr int32 AvatarImageLoadingInProcess { -1 };
	static constexpr int32 AvatarImageNotSet { 0 };

	/**
	 * @enum EAvatarImageSize
	 *
	 * Size of an avatar image requested from Steam.
	 */
	enum class EAvatarImageSize : uint8
	{
		// Invalid: Represents an invalid or uninitialized avatar size.
		Invalid = 0,
		// Small: Represents the small size of the avatar image (32x32).
		Small = 1,
		// Medium: Represents the medium size of the avatar image (64x64).
		Medium = 2,
		// Large: Represents the large size of the avatar image (184x184).
		Large = 3
	};

	/**
	 * Side of the square image Steam keeps for an avatar size, in pixels.
	 *
	 * The three sizes are fixed by Steam rather than negotiated, so an avatar can be named after its size
	 * before it has been fetched, which is what lets a cached one be found again.
	 */
	constexpr uint32 GetAvatarImageSizeInPixels(const EAvatarImageSize AvatarSize)
	{
		switch (AvatarSize)
		{
			case EAvatarImageSize::Small:
				return 32u;
			case EAvatarImageSize::Medium:
				return 64u;
			case EAvatarImageSize::Large:
				return 184u;
			case EAvatarImageSize::Invalid:
				break;
		}

		return 0u;
	}

	namespace Wrappers
	{
		/**
		 * @struct FSteamUserAvatar
		 *
		 * Fetches a user's avatar image. Steam answers on a broadcast callback shared by every listener,
		 * unless the image is already cached locally.
		 */
		struct FSteamUserAvatar
		{
			static constexpr TCHAR Name[] = TEXT("SteamUserAvatar");

			using SteamCallbackMsgType = AvatarImageLoaded_t;

			struct Params
			{
				CSteamID UserId { k_steamIDNil };
				EAvatarImageSize AvatarSize { EAvatarImageSize::Invalid };
			};

			struct Result
			{
				CSteamID UserId { k_steamIDNil };
				int32 ImageIndex { AvatarImageNotSet };

				uint32 ImageWidth { 0 };
				uint32 ImageHeight { 0 };
			};

			/**
			 * Asks Steam for the avatar handle.
			 * @return Pending when the image is still being downloaded, Completed when the answer is already known.
			 */
			static ESteamInvokeState Invoke(const Params& In, TSteamResultOf<Result>& OutResult)
			{
				auto Interface = GetSteamInterface<ISteamFriends>();
				if (Interface == nullptr)
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
					return ESteamInvokeState::Completed;
				}

				int32 ImageIndex { AvatarImageNotSet };
				switch (In.AvatarSize)
				{
					case EAvatarImageSize::Small:
						ImageIndex = Interface->GetSmallFriendAvatar(In.UserId);
						break;
					case EAvatarImageSize::Medium:
						ImageIndex = Interface->GetMediumFriendAvatar(In.UserId);
						break;
					case EAvatarImageSize::Large:
						ImageIndex = Interface->GetLargeFriendAvatar(In.UserId);
						break;
					default:
						OutResult = TSteamResultOf<Result>(UE::Online::Errors::InvalidParams());
						return ESteamInvokeState::Completed;
				}

				if (ImageIndex == AvatarImageLoadingInProcess)
				{
					// Steam is downloading the image and will answer on the callback.
					return ESteamInvokeState::Pending;
				}

				if (ImageIndex == AvatarImageNotSet)
				{
					// The user has no avatar, which is an answer rather than an image of index zero.
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::NotFound());
					return ESteamInvokeState::Completed;
				}

				OutResult = MakeAvatarResult(In.UserId, ImageIndex);
				return ESteamInvokeState::Completed;
			}

			/** Converts the callback payload into the avatar result. */
			static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
			{
				if (Message.m_iImage == AvatarImageNotSet)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::NotFound());
				}

				return TSteamResultOf<Result>(Result
				{
					Message.m_steamID,
					Message.m_iImage,
					static_cast<uint32>(Message.m_iWide),
					static_cast<uint32>(Message.m_iTall)
				});
			}

			/** Broadcast callbacks are delivered to every listener, so the payload has to be filtered. */
			static bool IsMatch(const Params& In, const SteamCallbackMsgType& Message)
			{
				return Message.m_steamID == In.UserId;
			}

		private:
			/** Reads the size of a locally cached avatar image. */
			static TSteamResultOf<Result> MakeAvatarResult(const CSteamID& UserId, const int32 ImageIndex)
			{
				auto Interface = GetSteamInterface<ISteamUtils>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				Result Avatar { .UserId = UserId, .ImageIndex = ImageIndex };
				if (!Interface->GetImageSize(Avatar.ImageIndex, &Avatar.ImageWidth, &Avatar.ImageHeight))
				{
					return TSteamResultOf<Result>(UE::Online::Errors::InvalidResults());
				}

				return TSteamResultOf<Result>(MoveTemp(Avatar));
			}
		};

		/**
		 * @struct FSteamUserInfo
		 *
		 * Requests a user's persona data. Steam answers on a broadcast callback shared by every listener,
		 * unless the data is already cached locally.
		 */
		struct FSteamUserInfo
		{
			static constexpr TCHAR Name[] = TEXT("SteamUserInfo");

			using SteamCallbackMsgType = PersonaStateChange_t;

			struct Params
			{
				CSteamID UserId { k_steamIDNil };
				bool bRequireNameOnly { true };
			};

			struct Result
			{
				CSteamID UserId { k_steamIDNil };
			};

			/**
			 * Asks Steam for the user's information.
			 * @return Pending when Steam has to fetch the data, Completed when it is already cached.
			 */
			static ESteamInvokeState Invoke(const Params& In, TSteamResultOf<Result>& OutResult)
			{
				auto Interface = GetSteamInterface<ISteamFriends>();
				if (Interface == nullptr)
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
					return ESteamInvokeState::Completed;
				}

				if (!In.UserId.IsValid())
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::InvalidParams());
					return ESteamInvokeState::Completed;
				}

				if (!Interface->RequestUserInformation(In.UserId, In.bRequireNameOnly))
				{
					// Steam already has the data, so no callback will follow.
					OutResult = TSteamResultOf<Result>(Result { .UserId = In.UserId });
					return ESteamInvokeState::Completed;
				}

				return ESteamInvokeState::Pending;
			}

			/** Converts the callback payload into the user info result. */
			static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
			{
				return TSteamResultOf<Result>(Result { .UserId = Message.m_ulSteamID });
			}

			/** Broadcast callbacks are delivered to every listener, so the payload has to be filtered. */
			static bool IsMatch(const Params& In, const SteamCallbackMsgType& Message)
			{
				return In.UserId == CSteamID(Message.m_ulSteamID);
			}
		};

		static_assert(CSteamCallbackOp<FSteamUserAvatar>);
		static_assert(CSteamCallbackOp<FSteamUserInfo>);
	}
}
