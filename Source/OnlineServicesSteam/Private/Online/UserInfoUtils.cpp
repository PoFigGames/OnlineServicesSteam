// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/UserInfoUtils.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamInterfaces.h"

// Engine
#include "IImageWrapperModule.h"
#include "PixelFormat.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Online/OnlineErrorDefinitions.h"


namespace PoFigGames::Online
{
	static constexpr int32 ImageQuality { 100 };
	static const FString AvatarsFolderName { TEXT("Avatars") };
	static const UE::Core::TCheckedFormatString<FString::FmtCharType, unsigned long long, unsigned int> AvatarImageFormat { TEXT("%llu_%u.png") };

	FString FUserInfoUtils::GetAvatarUrl(const uint64 SteamUserId, const Steam::EAvatarImageSize AvatarSize)
	{
		// Deliberately not the Steam user data folder: that one belongs to Steam and is only reachable while
		// the API is up, which would make the same avatar land in two different places across sessions.
		const FString CacheFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		const uint32 ImageSizeInPixels = Steam::GetAvatarImageSizeInPixels(AvatarSize);

		return FPaths::Combine(CacheFolder, AvatarsFolderName, FString::Printf(AvatarImageFormat, SteamUserId, ImageSizeInPixels));
	}

	UE::Online::TDefaultErrorResultInternal<FSteamImage> FUserInfoUtils::ReadImage(const int32 ImageIndex, const uint32 ImageWidth, const uint32 ImageHeight)
	{
		if (ImageWidth == 0u || ImageHeight == 0u)
		{
			// There is no image of that size, which is an answer rather than a zero sized one.
			return UE::Online::TDefaultErrorResultInternal<FSteamImage>(UE::Online::Errors::NotFound());
		}

		auto Utils = Steam::GetSteamInterface<ISteamUtils>();
		if (Utils == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoUtils::ReadImage] Failed: Steam utils interface is not available"));
			return UE::Online::TDefaultErrorResultInternal<FSteamImage>(UE::Online::Errors::MissingInterface());
		}

		FSteamImage Image { .ImageWidth = ImageWidth, .ImageHeight = ImageHeight };

		const int32 ImageSize = ImageWidth * ImageHeight * GPixelFormats[PF_R8G8B8A8].NumComponents;
		Image.RawImage.SetNumUninitialized(ImageSize);

		if (!Utils->GetImageRGBA(ImageIndex, Image.RawImage.GetData(), ImageSize))
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoUtils::ReadImage] Steam->GetImageRGBA Failed: Image [%d]"), ImageIndex);
			return UE::Online::TDefaultErrorResultInternal<FSteamImage>(UE::Online::Errors::InvalidResults());
		}

		return UE::Online::TDefaultErrorResultInternal<FSteamImage>(MoveTemp(Image));
	}

	UE::Online::FOnlineError FUserInfoUtils::SaveImageToFile(const TArray<uint8>& RawImage, const uint32 ImageWidth, const uint32 ImageHeight, const FString& ImagePath)
	{
		if (RawImage.IsEmpty() || ImageWidth == 0u || ImageHeight == 0u)
		{
			return UE::Online::Errors::InvalidParams();
		}

		auto& ImageWrapperModule = FModuleManager::GetModuleChecked<IImageWrapperModule>("ImageWrapper");
		const auto ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);

		if (!ImageWrapper.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoUtils::SaveImageToFile] IImageWrapperModule::CreateImageWrapper Failed"));
			return UE::Online::Errors::InvalidResults();
		}

		if (!ImageWrapper->SetRaw(RawImage.GetData(), RawImage.Num(), ImageWidth, ImageHeight, ERGBFormat::RGBA, 8))
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoUtils::SaveImageToFile] IImageWrapper::SetRaw Failed"));
			return UE::Online::Errors::InvalidResults();
		}

		if (!FFileHelper::SaveArrayToFile(ImageWrapper->GetCompressed(ImageQuality), *ImagePath))
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserInfoUtils::SaveImageToFile] FFileHelper::SaveArrayToFile Failed: Path [%s]"), *ImagePath);
			return UE::Online::Errors::InvalidResults();
		}

		return UE::Online::Errors::Success();
	}

	UE::Online::TDefaultErrorResult<FReadUserAvatar> FUserInfoUtils::ReadAvatarImage(FReadUserAvatar::Params&& Params)
	{
		if (Params.AvatarSize == Steam::EAvatarImageSize::Invalid)
		{
			// Without a size the image cannot be named, so it would be written over an avatar of another size.
			return UE::Online::TDefaultErrorResult<FReadUserAvatar>(UE::Online::Errors::InvalidParams());
		}

		auto ImageResult = ReadImage(Params.ImageIndex, Params.ImageWidth, Params.ImageHeight);
		if (ImageResult.IsError())
		{
			return UE::Online::TDefaultErrorResult<FReadUserAvatar>(MoveTemp(ImageResult.GetErrorValue()));
		}

		auto& ReadImageValue = ImageResult.GetOkValue();

		FSteamAvatarImage Image
		{
			.SteamId = Params.SteamId,
			.AvatarSize = Params.AvatarSize,
			.ImageWidth = ReadImageValue.ImageWidth,
			.ImageHeight = ReadImageValue.ImageHeight,
			.RawImage = MoveTemp(ReadImageValue.RawImage)
		};

		return UE::Online::TDefaultErrorResult<FReadUserAvatar>(FReadUserAvatar::Result { .Image = MoveTemp(Image) });
	}

	UE::Online::TDefaultErrorResult<FSaveUserAvatar> FUserInfoUtils::SaveAvatarImageToFile(FSaveUserAvatar::Params&& Params)
	{
		const auto& Image = Params.Image;

		if (Image.AvatarSize == Steam::EAvatarImageSize::Invalid)
		{
			return UE::Online::TDefaultErrorResult<FSaveUserAvatar>(UE::Online::Errors::InvalidParams());
		}

		FString ImagePath = GetAvatarUrl(Image.SteamId, Image.AvatarSize);
		if (auto Error = SaveImageToFile(Image.RawImage, Image.ImageWidth, Image.ImageHeight, ImagePath); Error != UE::Online::Errors::Success())
		{
			return UE::Online::TDefaultErrorResult<FSaveUserAvatar>(MoveTemp(Error));
		}

		return UE::Online::TDefaultErrorResult<FSaveUserAvatar>(FSaveUserAvatar::Result { .AvatarUrl = MoveTemp(ImagePath) });
	}
}
