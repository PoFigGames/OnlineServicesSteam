// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/OnlineErrorSteam.h"

// Engine
#include "Online/OnlineErrorDefinitions.h"


namespace PoFigGames::Online::Errors
{
	/**
	 * @class FOnlineErrorDetailsSteam
	 *
	 * @brief Puts Steam's own EResult behind an online error, so that the native code survives the trip.
	 */
	class FOnlineErrorDetailsSteam : public UE::Online::IOnlineErrorDetails
	{
	public:
		static const TSharedRef<const IOnlineErrorDetails>& Get()
		{
			static TSharedRef<const IOnlineErrorDetails> Instance = MakeShared<FOnlineErrorDetailsSteam>();
			return Instance;
		}

		virtual FString GetFriendlyErrorCode(const UE::Online::FOnlineError& OnlineError) const override
		{
			const auto Result = static_cast<EResult>(OnlineError.GetValue());
			return LexToString(Result);
		}

		virtual FText GetText(const UE::Online::FOnlineError& OnlineError) const override
		{
			const auto Result = static_cast<EResult>(OnlineError.GetValue());
			return FText::FromString(LexToString(Result));
		}

		virtual FString GetLogString(const UE::Online::FOnlineError& OnlineError) const override
		{
			const auto Result = static_cast<EResult>(OnlineError.GetValue());
			return LexToString(Result);
		}
	};

	/** This doesn't wrap every single error in Steam, only the common ones that are strongly related to common errors */
	UE::Online::FOnlineError MapCommonSteamError(UE::Online::FOnlineError&& Error, const EResult Result)
	{
		switch (Result)
		{
			case k_EResultOK:
				return UE::Online::Errors::Success(MoveTemp(Error));
			case k_EResultNoConnection:
				return UE::Online::Errors::NoConnection(MoveTemp(Error));
			case k_EResultInvalidPassword:
				return UE::Online::Errors::InvalidCreds(MoveTemp(Error));
			case k_EResultAccessDenied:
				return UE::Online::Errors::AccessDenied(MoveTemp(Error));
			case k_EResultLimitedUserAccount:
				return UE::Online::Errors::AccessDenied(MoveTemp(Error));
			case k_EResultPending:
				return UE::Online::Errors::AlreadyPending(MoveTemp(Error));
			case k_EResultInvalidParam:
				return UE::Online::Errors::InvalidParams(MoveTemp(Error));
			case k_EResultCancelled:
				return UE::Online::Errors::Cancelled(MoveTemp(Error));
			case k_EResultNotModified:
				return UE::Online::Errors::NoChange(MoveTemp(Error));
			case k_EResultLimitExceeded:
				return UE::Online::Errors::TooManyRequests(MoveTemp(Error));
			case k_EResultTimeout:
				return UE::Online::Errors::Timeout(MoveTemp(Error));
			default:
				return MoveTemp(Error);
		}
	}

	UE::Online::Errors::ErrorCodeType ErrorCodeFromSteamResult(const EResult Result)
	{
		return UE::Online::Errors::ErrorCode::Create(ErrorCode::Category::STEAM_System, ErrorCode::Category::STEAM, static_cast<uint32>(Result));
	}

	UE::Online::FOnlineError FromSteamResult(const EResult Result, FErrorMapperSteamFn&& MapperFn)
	{
		return MapperFn(UE::Online::FOnlineError(ErrorCodeFromSteamResult(Result), FOnlineErrorDetailsSteam::Get()), Result);
	}
}
