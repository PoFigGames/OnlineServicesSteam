// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/AchievementsSteam.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamInterfaces.h"
#include "Online/OnlineIdSteam.h"
#include "Online/OnlineServicesSteam.h"
#include "Online/UserInfoUtils.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/SteamResult.h"
#include "Steam/Wrappers/SteamUserStats.h"

// Engine
#include "Misc/Paths.h"
#include "Online/AchievementsErrors.h"
#include "Online/OnlineErrorDefinitions.h"


namespace PoFigGames::Online
{
	namespace Private
	{
		/** Folder the icons of the achievements are cached in, beside the avatars. */
		static const FString AchievementIconsFolderName { TEXT("AchievementIcons") };

		/** Keys Steam knows the display attributes of an achievement by. */
		static constexpr ANSICHAR DisplayAttributeName[] { "name" };
		static constexpr ANSICHAR DisplayAttributeDescription[] { "desc" };
		static constexpr ANSICHAR DisplayAttributeHidden[] { "hidden" };

		/** How Steam spells a set flag in a display attribute. */
		static constexpr ANSICHAR DisplayAttributeSet[] { "1" };

		/**
		 * Steam reports a fully unlocked achievement by naming no progress at all; anything else is the
		 * progress notification IndicateAchievementProgress produces.
		 */
		static bool IsFullyUnlocked(const UserAchievementStored_t& Message)
		{
			return Message.m_nCurProgress == 0u && Message.m_nMaxProgress == 0u;
		}

		/** Reads a display attribute of an achievement as text the game can show. */
		static FText GetDisplayAttribute(ISteamUserStats& UserStats, const ANSICHAR* AchievementName, const ANSICHAR* AttributeKey)
		{
			return FText::FromString(StringCast<TCHAR>(UserStats.GetAchievementDisplayAttribute(AchievementName, AttributeKey)).Get());
		}

		/** When Steam recorded no unlock time, which it did not before December 2009, the epoch stands in. */
		static FDateTime GetUnlockTime(const uint32 UnlockTimeInSeconds)
		{
			return UnlockTimeInSeconds > 0u ? FDateTime::FromUnixTimestamp(UnlockTimeInSeconds) : FDateTime { };
		}
	}

	void FAchievementsSteam::Initialize()
	{
		Super::Initialize();

		// Steam announces achievement changes to every listener rather than answering a query, and an icon
		// it had to fetch arrives the same way, so the component listens for as long as it is alive.
		OnUserAchievementStoredCallback.Register(this, &FAchievementsSteam::OnUserAchievementStored);
		OnUserAchievementIconFetchedCallback.Register(this, &FAchievementsSteam::OnUserAchievementIconFetched);
	}

	void FAchievementsSteam::PreShutdown()
	{
		// The handlers below reach for the cached definitions and states, so they stop firing first.
		OnUserAchievementStoredCallback.Unregister();
		OnUserAchievementIconFetchedCallback.Unregister();

		Super::PreShutdown();
	}

	FString FAchievementsSteam::GetAchievementIconUrl(const FString& AchievementId, const bool bAchieved)
	{
		const FString CacheFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());

		// The name of an achievement is chosen by whoever set the game up on Steam, so it is not taken for
		// a file name without asking.
		const FString FileName = FPaths::MakeValidFileName(
			FString::Printf(TEXT("%s_%s.png"), *AchievementId, bAchieved ? TEXT("unlocked") : TEXT("locked")), TEXT('_'));

		return FPaths::Combine(CacheFolder, Private::AchievementIconsFolderName, FileName);
	}

	FString FAchievementsSteam::CacheAchievementIcon(const FString& AchievementId, const bool bAchieved, const int32 ImageIndex)
	{
		const auto Utils = Steam::GetSteamInterface<ISteamUtils>();
		if (Utils == nullptr || ImageIndex == 0)
		{
			return { };
		}

		uint32 ImageWidth { 0 };
		uint32 ImageHeight { 0 };

		if (!Utils->GetImageSize(ImageIndex, &ImageWidth, &ImageHeight))
		{
			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAchievementsSteam::CacheAchievementIcon] Steam->GetImageSize Failed: Achievement [%s]"), *AchievementId);
			return { };
		}

		auto ImageResult = FUserInfoUtils::ReadImage(ImageIndex, ImageWidth, ImageHeight);
		if (ImageResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAchievementsSteam::CacheAchievementIcon] Failed to read the icon. Achievement [%s], Result [%s]"),
				*AchievementId, *ImageResult.GetErrorValue().GetLogString());

			return { };
		}

		const auto& Image = ImageResult.GetOkValue();

		FString IconUrl = GetAchievementIconUrl(AchievementId, bAchieved);
		if (auto Error = FUserInfoUtils::SaveImageToFile(Image.RawImage, Image.ImageWidth, Image.ImageHeight, IconUrl); Error != UE::Online::Errors::Success())
		{
			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAchievementsSteam::CacheAchievementIcon] Failed to cache the icon. Achievement [%s], Result [%s]"),
				*AchievementId, *Error.GetLogString());

			return { };
		}

		return IconUrl;
	}

	void FAchievementsSteam::ApplyAchievementIcon(const FString& AchievementId, const bool bAchieved, const FString& IconUrl)
	{
		if (IconUrl.IsEmpty() || !AchievementDefinitions.IsSet())
		{
			return;
		}

		const auto Definition = AchievementDefinitions->Find(AchievementId);
		if (Definition == nullptr)
		{
			return;
		}

		// Steam keeps one icon per achievement, the one which matches its current state, so only the side
		// of the definition that state calls for can ever be filled in.
		(bAchieved ? Definition->UnlockedIconUrl : Definition->LockedIconUrl) = IconUrl;
	}

	UE::Online::FOnlineError FAchievementsSteam::ReadAchievementDefinitions()
	{
		const auto UserStats = Steam::GetSteamInterface<ISteamUserStats>();
		if (UserStats == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::QueryAchievementDefinitions] Failed: Steam user stats interface is not available"));
			return UE::Online::Errors::MissingInterface();
		}

		const uint32 AchievementCount = UserStats->GetNumAchievements();

		FAchievementDefinitionMap NewAchievementDefinitions;
		NewAchievementDefinitions.Reserve(AchievementCount);

		for (uint32 AchievementIndex = 0u; AchievementIndex < AchievementCount; ++AchievementIndex)
		{
			const auto AchievementName = UserStats->GetAchievementName(AchievementIndex);
			if (AchievementName == nullptr || *AchievementName == '\0')
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::QueryAchievementDefinitions] Skipping an achievement Steam did not name: Index [%u]"), AchievementIndex);
				continue;
			}

			FString AchievementId = StringCast<TCHAR>(AchievementName).Get();

			auto& AchievementDefinition = NewAchievementDefinitions.Emplace(AchievementId);
			AchievementDefinition.AchievementId = AchievementId;

			// Steam holds a single name and a single description, so both sides of the definition read the
			// same; the flavour text it has no notion of at all and is left empty.
			AchievementDefinition.UnlockedDisplayName = Private::GetDisplayAttribute(*UserStats, AchievementName, Private::DisplayAttributeName);
			AchievementDefinition.UnlockedDescription = Private::GetDisplayAttribute(*UserStats, AchievementName, Private::DisplayAttributeDescription);
			AchievementDefinition.LockedDisplayName = AchievementDefinition.UnlockedDisplayName;
			AchievementDefinition.LockedDescription = AchievementDefinition.UnlockedDescription;

			AchievementDefinition.bIsHidden = FCStringAnsi::Strcmp(
				UserStats->GetAchievementDisplayAttribute(AchievementName, Private::DisplayAttributeHidden), Private::DisplayAttributeSet) == 0;

			// Which stats drive an achievement is part of the schema Steam keeps on its backend rather than
			// of anything the client can read, so the answer is the one the game configured.
			for (const auto& UnlockRule : Config.UnlockRules)
			{
				if (UnlockRule.AchievementId != AchievementId)
				{
					continue;
				}

				for (const auto& Condition : UnlockRule.Conditions)
				{
					if (Condition.UnlockThreshold.GetType() != UE::Online::ESchemaAttributeType::Int64)
					{
						continue;
					}

					AchievementDefinition.StatDefinitions.Emplace(UE::Online::FAchievementStatDefinition {
						.StatId = Condition.StatName,
						.UnlockThreshold = static_cast<uint32>(Condition.UnlockThreshold.GetInt64()) });
				}

				break;
			}
		}

		AchievementDefinitions.Emplace(MoveTemp(NewAchievementDefinitions));

		// An icon Steam has yet to fetch answers with zero and arrives on the callback instead, which is
		// why the definitions are published before the icons are asked for.
		for (const auto& Definition : *AchievementDefinitions)
		{
			const auto AchievementName = StringCast<ANSICHAR>(*Definition.Key);

			bool bAchieved { false };
			UserStats->GetAchievement(AchievementName.Get(), &bAchieved);

			ApplyAchievementIcon(Definition.Key, bAchieved, CacheAchievementIcon(Definition.Key, bAchieved, UserStats->GetAchievementIcon(AchievementName.Get())));
		}

		UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAchievementsSteam::QueryAchievementDefinitions] Succeeded: Achievements [%d]"), AchievementDefinitions->Num());

		return UE::Online::Errors::Success();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryAchievementDefinitions> FAchievementsSteam::QueryAchievementDefinitions(UE::Online::FQueryAchievementDefinitions::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FQueryAchievementDefinitions>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryAchievementDefinitions>& InAsyncOp)
			{
				using FUserStatsResult = Steam::TSteamResult<Steam::Wrappers::FSteamUserStats>;

				auto LocalUser = ResolveLocalSteamUser(InAsyncOp.GetParams().LocalAccountId, TEXT("FAchievementsSteam::QueryAchievementDefinitions"));
				if (LocalUser.IsError())
				{
					InAsyncOp.SetError(MoveTemp(LocalUser.GetErrorValue()));
					return MakeFulfilledPromise<FUserStatsResult>(FUserStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				// The schema lives in the client, but only once the stats of the user have been downloaded;
				// asking for them is how a query waits for that rather than reading an empty schema.
				return SteamCall<Steam::Wrappers::FSteamUserStats>({ .UserId = LocalUser.GetOkValue() });
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamUserStats>(TEXT("FAchievementsSteam::QueryAchievementDefinitions"),
				[this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryAchievementDefinitions>& InAsyncOp, Steam::Wrappers::FSteamUserStats::Result&& /*Result*/)
			{
				if (auto Error = ReadAchievementDefinitions(); Error != UE::Online::Errors::Success())
				{
					InAsyncOp.SetError(MoveTemp(Error));
					return;
				}

				InAsyncOp.SetResult({ });
			}))
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FGetAchievementIds> FAchievementsSteam::GetAchievementIds(UE::Online::FGetAchievementIds::Params&& InParams)
	{
		if (auto LocalUser = ResolveLocalSteamUser(InParams.LocalAccountId, TEXT("FAchievementsSteam::GetAchievementIds")); LocalUser.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FGetAchievementIds>(MoveTemp(LocalUser.GetErrorValue()));
		}

		if (!AchievementDefinitions.IsSet())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::GetAchievementIds] Failed: Call QueryAchievementDefinitions first."));
			return UE::Online::TOnlineResult<UE::Online::FGetAchievementIds>(UE::Online::Errors::InvalidState());
		}

		UE::Online::FGetAchievementIds::Result Result;
		AchievementDefinitions->GenerateKeyArray(Result.AchievementIds);

		return UE::Online::TOnlineResult<UE::Online::FGetAchievementIds>(MoveTemp(Result));
	}

	UE::Online::TOnlineResult<UE::Online::FGetAchievementDefinition> FAchievementsSteam::GetAchievementDefinition(UE::Online::FGetAchievementDefinition::Params&& InParams)
	{
		if (auto LocalUser = ResolveLocalSteamUser(InParams.LocalAccountId, TEXT("FAchievementsSteam::GetAchievementDefinition")); LocalUser.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FGetAchievementDefinition>(MoveTemp(LocalUser.GetErrorValue()));
		}

		if (!AchievementDefinitions.IsSet())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::GetAchievementDefinition] Failed: Call QueryAchievementDefinitions first."));
			return UE::Online::TOnlineResult<UE::Online::FGetAchievementDefinition>(UE::Online::Errors::InvalidState());
		}

		const auto AchievementDefinition = AchievementDefinitions->Find(InParams.AchievementId);
		if (AchievementDefinition == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::GetAchievementDefinition] Failed: The game knows no achievement [%s]."), *InParams.AchievementId);
			return UE::Online::TOnlineResult<UE::Online::FGetAchievementDefinition>(UE::Online::Errors::NotFound());
		}

		return UE::Online::TOnlineResult<UE::Online::FGetAchievementDefinition>({ *AchievementDefinition });
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryAchievementStates> FAchievementsSteam::QueryAchievementStates(UE::Online::FQueryAchievementStates::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FQueryAchievementStates>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryAchievementStates>& InAsyncOp)
			{
				using FUserStatsResult = Steam::TSteamResult<Steam::Wrappers::FSteamUserStats>;

				auto LocalUser = ResolveLocalSteamUser(InAsyncOp.GetParams().LocalAccountId, TEXT("FAchievementsSteam::QueryAchievementStates"));
				if (LocalUser.IsError())
				{
					InAsyncOp.SetError(MoveTemp(LocalUser.GetErrorValue()));
					return MakeFulfilledPromise<FUserStatsResult>(FUserStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				if (!AchievementDefinitions.IsSet())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::QueryAchievementStates] Failed: Call QueryAchievementDefinitions first."));

					InAsyncOp.SetError(UE::Online::Errors::InvalidState());
					return MakeFulfilledPromise<FUserStatsResult>(FUserStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
				}

				return SteamCall<Steam::Wrappers::FSteamUserStats>({ .UserId = LocalUser.GetOkValue() });
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamUserStats>(TEXT("FAchievementsSteam::QueryAchievementStates"),
				[this](UE::Online::TOnlineAsyncOp<UE::Online::FQueryAchievementStates>& InAsyncOp, Steam::Wrappers::FSteamUserStats::Result&& /*Result*/)
			{
				const auto UserStats = Steam::GetSteamInterface<ISteamUserStats>();
				if (UserStats == nullptr)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::QueryAchievementStates] Failed: Steam user stats interface is not available"));

					InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
					return;
				}

				const auto& Params = InAsyncOp.GetParams();

				FAchievementStateMap NewAchievementStates;
				NewAchievementStates.Reserve(AchievementDefinitions->Num());

				for (const auto& Definition : *AchievementDefinitions)
				{
					const auto AchievementName = StringCast<ANSICHAR>(*Definition.Key);

					bool bAchieved { false };
					uint32 UnlockTimeInSeconds { 0 };

					if (!UserStats->GetAchievementAndUnlockTime(AchievementName.Get(), &bAchieved, &UnlockTimeInSeconds))
					{
						UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::QueryAchievementStates] Steam->GetAchievementAndUnlockTime Failed: Achievement [%s]"), *Definition.Key);
						continue;
					}

					// Steam publishes no progress of its own: an achievement with a progress bar is driven
					// by the stats behind it, and what it reports is where those stats stand.
					auto& AchievementState = NewAchievementStates.Emplace(Definition.Key);
					AchievementState.AchievementId = Definition.Key;
					AchievementState.Progress = bAchieved ? 1.0f : 0.0f;
					AchievementState.UnlockTime = Private::GetUnlockTime(UnlockTimeInSeconds);
				}

				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAchievementsSteam::QueryAchievementStates] Succeeded: User [%s], Achievements [%d]"),
					*ToLogString(Params.LocalAccountId), NewAchievementStates.Num());

				AchievementStates.Emplace(Params.LocalAccountId, MoveTemp(NewAchievementStates));

				InAsyncOp.SetResult({ });

				// Achievements the game unlocks by stats rather than by hand are settled here, now that
				// there is something to compare the stats against.
				OnAchievementStatesQueried(Params.LocalAccountId);
			}))
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FGetAchievementState> FAchievementsSteam::GetAchievementState(UE::Online::FGetAchievementState::Params&& InParams) const
	{
		if (auto LocalUser = ResolveLocalSteamUser(InParams.LocalAccountId, TEXT("FAchievementsSteam::GetAchievementState")); LocalUser.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FGetAchievementState>(MoveTemp(LocalUser.GetErrorValue()));
		}

		const auto LocalUserAchievementStates = AchievementStates.Find(InParams.LocalAccountId);
		if (LocalUserAchievementStates == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::GetAchievementState] Failed: Call QueryAchievementStates first."));
			return UE::Online::TOnlineResult<UE::Online::FGetAchievementState>(UE::Online::Errors::InvalidState());
		}

		const auto AchievementState = LocalUserAchievementStates->Find(InParams.AchievementId);
		if (AchievementState == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::GetAchievementState] Failed: The game knows no achievement [%s]."), *InParams.AchievementId);
			return UE::Online::TOnlineResult<UE::Online::FGetAchievementState>(UE::Online::Errors::NotFound());
		}

		return UE::Online::TOnlineResult<UE::Online::FGetAchievementState>({ *AchievementState });
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FUnlockAchievements> FAchievementsSteam::UnlockAchievements(UE::Online::FUnlockAchievements::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FUnlockAchievements>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUnlockAchievements>& InAsyncOp)
			{
				using FStoreStatsResult = Steam::TSteamResult<Steam::Wrappers::FSteamStoreStats>;

				const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
				{
					InAsyncOp.SetError(MoveTemp(Error));
					return MakeFulfilledPromise<FStoreStatsResult>(FStoreStatsResult(UE::Online::Errors::Cancelled())).GetFuture();
				};

				const auto& Params = InAsyncOp.GetParams();

				auto LocalUser = ResolveLocalSteamUser(Params.LocalAccountId, TEXT("FAchievementsSteam::UnlockAchievements"));
				if (LocalUser.IsError())
				{
					return FailWith(MoveTemp(LocalUser.GetErrorValue()));
				}

				if (Params.AchievementIds.IsEmpty())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::UnlockAchievements] Failed: No achievements to unlock."));
					return FailWith(UE::Online::Errors::InvalidParams());
				}

				const auto LocalUserAchievementStates = AchievementStates.Find(Params.LocalAccountId);
				if (LocalUserAchievementStates == nullptr)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::UnlockAchievements] Failed: Call QueryAchievementStates first."));
					return FailWith(UE::Online::Errors::InvalidState());
				}

				const auto UserStats = Steam::GetSteamInterface<ISteamUserStats>();
				if (UserStats == nullptr)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::UnlockAchievements] Failed: Steam user stats interface is not available"));
					return FailWith(UE::Online::Errors::MissingInterface());
				}

				// Everything is checked before anything is set, so a request which cannot be honoured in
				// full changes nothing at all.
				for (const auto& AchievementId : Params.AchievementIds)
				{
					const auto AchievementState = LocalUserAchievementStates->Find(AchievementId);
					if (AchievementState == nullptr)
					{
						UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::UnlockAchievements] Failed: The game knows no achievement [%s]."), *AchievementId);
						return FailWith(UE::Online::Errors::NotFound());
					}

					if (FMath::IsNearlyEqual(AchievementState->Progress, 1.0f))
					{
						UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::UnlockAchievements] Failed: Achievement [%s] is already unlocked."), *AchievementId);
						return FailWith(UE::Online::Errors::Achievements::AlreadyUnlocked());
					}
				}

				for (const auto& AchievementId : Params.AchievementIds)
				{
					if (!UserStats->SetAchievement(StringCast<ANSICHAR>(*AchievementId).Get()))
					{
						UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::UnlockAchievements] Steam->SetAchievement Failed: Achievement [%s]"), *AchievementId);
						return FailWith(UE::Online::Errors::RequestFailure());
					}
				}

				// Nothing is unlocked until the whole batch is committed, and Steam announces each unlock
				// it accepted on its own callback afterwards.
				return SteamListen<Steam::Wrappers::FSteamStoreStats>({ });
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamStoreStats>(TEXT("FAchievementsSteam::UnlockAchievements"),
				[this](UE::Online::TOnlineAsyncOp<UE::Online::FUnlockAchievements>& InAsyncOp, Steam::Wrappers::FSteamStoreStats::Result&& /*Result*/)
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto UserStats = Steam::GetSteamInterface<ISteamUserStats>();

				for (const auto& AchievementId : Params.AchievementIds)
				{
					bool bAchieved { false };
					uint32 UnlockTimeInSeconds { 0 };

					if (UserStats != nullptr)
					{
						UserStats->GetAchievementAndUnlockTime(StringCast<ANSICHAR>(*AchievementId).Get(), &bAchieved, &UnlockTimeInSeconds);
					}

					MarkAchievementUnlocked(Params.LocalAccountId, AchievementId, Private::GetUnlockTime(UnlockTimeInSeconds));
				}

				UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FAchievementsSteam::UnlockAchievements] Succeeded: User [%s], Achievements [%d]"),
					*ToLogString(Params.LocalAccountId), Params.AchievementIds.Num());

				InAsyncOp.SetResult({ });
			}))
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FDisplayAchievementUI> FAchievementsSteam::DisplayAchievementUI(UE::Online::FDisplayAchievementUI::Params&& InParams)
	{
		if (auto LocalUser = ResolveLocalSteamUser(InParams.LocalAccountId, TEXT("FAchievementsSteam::DisplayAchievementUI")); LocalUser.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FDisplayAchievementUI>(MoveTemp(LocalUser.GetErrorValue()));
		}

		if (!AchievementDefinitions.IsSet() || !AchievementDefinitions->Contains(InParams.AchievementId))
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::DisplayAchievementUI] Failed: The game knows no achievement [%s]."), *InParams.AchievementId);
			return UE::Online::TOnlineResult<UE::Online::FDisplayAchievementUI>(UE::Online::Errors::NotFound());
		}

		const auto Friends = Steam::GetSteamInterface<ISteamFriends>();
		if (Friends == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAchievementsSteam::DisplayAchievementUI] Failed: Steam friends interface is not available"));
			return UE::Online::TOnlineResult<UE::Online::FDisplayAchievementUI>(UE::Online::Errors::MissingInterface());
		}

		if (auto Error = CheckOverlayAvailable(TEXT("FAchievementsSteam::DisplayAchievementUI")); Error != UE::Online::Errors::Success())
		{
			return UE::Online::TOnlineResult<UE::Online::FDisplayAchievementUI>(MoveTemp(Error));
		}

		// The overlay opens on the list of achievements; Steam offers no way to point it at one of them.
		Friends->ActivateGameOverlay("Achievements");

		return UE::Online::TOnlineResult<UE::Online::FDisplayAchievementUI>(UE::Online::FDisplayAchievementUI::Result { });
	}

	void FAchievementsSteam::MarkAchievementUnlocked(const UE::Online::FAccountId& LocalAccountId, const FString& AchievementId, const FDateTime& UnlockTime)
	{
		const auto LocalUserAchievementStates = AchievementStates.Find(LocalAccountId);
		if (LocalUserAchievementStates == nullptr)
		{
			// The states of this user were never queried, so there is nothing to keep in step.
			return;
		}

		const auto AchievementState = LocalUserAchievementStates->Find(AchievementId);
		if (AchievementState == nullptr || FMath::IsNearlyEqual(AchievementState->Progress, 1.0f))
		{
			// Either the achievement is not one of ours, or it was already reported as unlocked; the answer
			// to a commit and the announcement Steam makes both arrive, and only the first one reports it.
			return;
		}

		AchievementState->Progress = 1.0f;
		AchievementState->UnlockTime = UnlockTime;

		OnAchievementStateUpdatedEvent.Broadcast(UE::Online::FAchievementStateUpdated {
			.LocalAccountId = LocalAccountId,
			.AchievementIds = { AchievementId } });
	}

	void FAchievementsSteam::OnUserAchievementStored(UserAchievementStored_t* Message)
	{
		if (Message == nullptr || !Steam::Wrappers::IsCurrentGame(Message->m_nGameID))
		{
			return;
		}

		// Steam names no user in the payload because there is only ever one: the signed in local user.
		const auto SteamUser = Steam::GetSteamInterface<ISteamUser>();
		if (SteamUser == nullptr)
		{
			return;
		}

		const auto LocalAccountId = FindAccountId(SteamUser->GetSteamID());
		if (!LocalAccountId.IsValid())
		{
			return;
		}

		const FString AchievementId = StringCast<TCHAR>(Message->m_rgchAchievementName).Get();

		if (Private::IsFullyUnlocked(*Message))
		{
			MarkAchievementUnlocked(LocalAccountId, AchievementId, FDateTime::UtcNow());
			return;
		}

		// A progress notification, which is the only progress Steam ever reports; it is worth keeping
		// because nothing else can tell the game how far along an achievement with a bar is.
		const auto LocalUserAchievementStates = AchievementStates.Find(LocalAccountId);
		const auto AchievementState = LocalUserAchievementStates ? LocalUserAchievementStates->Find(AchievementId) : nullptr;

		if (AchievementState == nullptr || Message->m_nMaxProgress == 0u)
		{
			return;
		}

		const float Progress = FMath::Clamp(static_cast<float>(Message->m_nCurProgress) / static_cast<float>(Message->m_nMaxProgress), 0.0f, 1.0f);
		if (FMath::IsNearlyEqual(AchievementState->Progress, Progress))
		{
			return;
		}

		AchievementState->Progress = Progress;

		OnAchievementStateUpdatedEvent.Broadcast(UE::Online::FAchievementStateUpdated {
			.LocalAccountId = LocalAccountId,
			.AchievementIds = { AchievementId } });
	}

	void FAchievementsSteam::OnUserAchievementIconFetched(UserAchievementIconFetched_t* Message)
	{
		if (Message == nullptr || !Steam::Wrappers::IsCurrentGame(Message->m_nGameID.ToUint64()) || Message->m_nIconHandle == 0)
		{
			return;
		}

		const FString AchievementId = StringCast<TCHAR>(Message->m_rgchAchievementName).Get();

		ApplyAchievementIcon(AchievementId, Message->m_bAchieved,
			CacheAchievementIcon(AchievementId, Message->m_bAchieved, Message->m_nIconHandle));
	}
}
