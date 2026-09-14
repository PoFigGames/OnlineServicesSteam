// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineAsyncOp.h"
#include "Steam/Wrappers/SteamUserInfo.h"


namespace PoFigGames::Online
{
	/**
	 * Size the operations which name none work in: the login of a local user, and the getters of the engine
	 * which have no room for a size. They have to agree, or a login would cache an image the getter of the
	 * engine then fails to find.
	 */
	static constexpr Steam::EAvatarImageSize DefaultAvatarSize { Steam::EAvatarImageSize::Medium };

	/**
	 * @struct FSteamImage
	 *
	 * @brief Raw image copied out of the Steam image cache, ready to be encoded and written.
	 *
	 * Steam hands out avatars and achievement icons alike as a handle into one image cache, so both are
	 * read and written the same way and only differ in what they are named after.
	 */
	struct FSteamImage
	{
		uint32 ImageWidth { 0 };
		uint32 ImageHeight { 0 };

		TArray<uint8> RawImage { };
	};

	/**
	 * @struct FSteamAvatarImage
	 *
	 * @brief Raw avatar image copied out of the Steam image cache, ready to be encoded and written.
	 */
	struct FSteamAvatarImage
	{
		uint64 SteamId { 0 };

		/** Which of the three Steam avatar sizes this image is, so that it can be named after it. */
		Steam::EAvatarImageSize AvatarSize { Steam::EAvatarImageSize::Invalid };

		uint32 ImageWidth { 0 };
		uint32 ImageHeight { 0 };

		TArray<uint8> RawImage { };
	};

	/**
	 * @struct FReadUserAvatar
	 *
	 * @brief Reads one avatar out of the Steam image cache.
	 */
	struct FReadUserAvatar
	{
		static constexpr TCHAR Name[] = TEXT("ReadUserAvatar");

		/** Input struct for FUserInfoUtils::ReadAvatarImage */
		struct Params
		{
			uint64 SteamId { 0 };

			Steam::EAvatarImageSize AvatarSize { Steam::EAvatarImageSize::Invalid };

			int32  ImageIndex { 0 };
			uint32 ImageWidth { 0 };
			uint32 ImageHeight { 0 };
		};

		/** Output struct for FUserInfoUtils::ReadAvatarImage */
		struct Result
		{
			FSteamAvatarImage Image { };
		};
	};

	/**
	 * @struct FSaveUserAvatar
	 *
	 * @brief Encodes an avatar image and writes it to the avatar cache.
	 */
	struct FSaveUserAvatar
	{
		static constexpr TCHAR Name[] = TEXT("SaveUserAvatar");

		/** Input struct for FUserInfoUtils::SaveAvatarImageToFile */
		struct Params
		{
			FSteamAvatarImage Image { };
		};

		/** Output struct for FUserInfoUtils::SaveAvatarImageToFile */
		struct Result
		{
			FString AvatarUrl { };
		};
	};

	/**
	 * @class FUserInfoUtils
	 *
	 * @brief Image cache of the plugin: avatars for the auth and user info components, icons for the
	 * achievements one.
	 *
	 * Everything is kept under the saved directory of the project rather than in the Steam user data
	 * folder, so that an image written in one session is still found in the next one.
	 */
	class FUserInfoUtils
	{
	public:
		/** Copies an image out of the Steam cache by handle. Calls the Steam API, so it belongs on the game thread. */
		static UE::Online::TDefaultErrorResultInternal<FSteamImage> ReadImage(int32 ImageIndex, uint32 ImageWidth, uint32 ImageHeight);

		/** Encodes an image as PNG and writes it. Touches no Steam API, so it can run off the game thread. */
		static UE::Online::FOnlineError SaveImageToFile(const TArray<uint8>& RawImage, uint32 ImageWidth, uint32 ImageHeight, const FString& ImagePath);

		/**
		 * Where the avatar of a Steam user is cached. The path does not depend on the Steam API being up, so
		 * an avatar written in one session is still found in the next one.
		 *
		 * The size is part of the name: Steam keeps three images per user, and a caller asking for a large
		 * portrait must not be handed the small icon another caller cached first.
		 */
		static FString GetAvatarUrl(uint64 SteamUserId, Steam::EAvatarImageSize AvatarSize);

		/** Copies the image out of the Steam cache. Calls the Steam API, so it belongs on the game thread. */
		static UE::Online::TDefaultErrorResult<FReadUserAvatar> ReadAvatarImage(FReadUserAvatar::Params&& Params);

		/** Encodes and writes the image. Touches no Steam API, so it can run off the game thread. */
		static UE::Online::TDefaultErrorResult<FSaveUserAvatar> SaveAvatarImageToFile(FSaveUserAvatar::Params&& Params);
	};
}
