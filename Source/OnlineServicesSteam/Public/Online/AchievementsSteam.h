// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/AchievementsCommon.h"
#include "Online/OnlineComponentSteam.h"

THIRD_PARTY_INCLUDES_START
#include "steam/isteamuserstats.h"
THIRD_PARTY_INCLUDES_END


namespace PoFigGames::Online
{
	/**
	 * @class FAchievementsSteam
	 *
	 * @brief Steam Achievements Online Component
	 *
	 * Steam keeps the achievement schema and the state of the local user in the client itself, downloaded
	 * before the game process starts, so both queries are answered out of the local cache once the stats
	 * of the user have been received. What the game unlocks is set locally and committed in one call, and
	 * Steam announces every change back, including the ones this process made.
	 *
	 * The definitions Steam publishes are thinner than the ones the interface describes: there is a single
	 * name and description rather than a locked and an unlocked pair, no flavour text, and a single icon,
	 * the one matching the current state of the achievement. Progress is not readable either, so an
	 * achievement is either locked or unlocked unless the game reports progress through the stats which
	 * back it. Everything the interface asks for and Steam does not have is left empty rather than filled
	 * with a placeholder.
	 */
	class FAchievementsSteam : public TOnlineComponentSteam<UE::Online::FAchievementsCommon>
	{
	public:
		using Super = FAchievementsCommon;

		using TOnlineComponentSteam::TOnlineComponentSteam;

	#pragma region TOnlineComponent
		ONLINESERVICESSTEAM_API virtual void Initialize() override;
		ONLINESERVICESSTEAM_API virtual void PreShutdown() override;
	#pragma endregion TOnlineComponent

	#pragma region IAchievements
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryAchievementDefinitions> QueryAchievementDefinitions(UE::Online::FQueryAchievementDefinitions::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetAchievementIds>                  GetAchievementIds(UE::Online::FGetAchievementIds::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetAchievementDefinition>           GetAchievementDefinition(UE::Online::FGetAchievementDefinition::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryAchievementStates>      QueryAchievementStates(UE::Online::FQueryAchievementStates::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetAchievementState>                GetAchievementState(UE::Online::FGetAchievementState::Params&& Params) const override;

		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FUnlockAchievements> UnlockAchievements(UE::Online::FUnlockAchievements::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FDisplayAchievementUI>      DisplayAchievementUI(UE::Online::FDisplayAchievementUI::Params&& Params) override;
	#pragma endregion IAchievements

	protected:
		using FAchievementDefinitionMap = TMap<FString, UE::Online::FAchievementDefinition>;
		TOptional<FAchievementDefinitionMap> AchievementDefinitions { };

	private:
		/** Reads the schema Steam holds locally into achievement definitions. */
		ONLINESERVICESSTEAM_API UE::Online::FOnlineError ReadAchievementDefinitions();

		/**
		 * Records that an achievement is unlocked and tells the game, unless it was known to be unlocked
		 * already. Both the answer to a commit and the announcement Steam makes lead here, and whichever
		 * arrives first is the one which reports it.
		 */
		ONLINESERVICESSTEAM_API void MarkAchievementUnlocked(const UE::Online::FAccountId& LocalAccountId, const FString& AchievementId, const FDateTime& UnlockTime);

		/** Steam announces every achievement change to every listener, including the ones this process made. */
		ONLINESERVICESSTEAM_API void OnUserAchievementStored(UserAchievementStored_t* Message);

		/** An icon Steam had to fetch has arrived, so the definition it belongs to can name it. */
		ONLINESERVICESSTEAM_API void OnUserAchievementIconFetched(UserAchievementIconFetched_t* Message);

		/** Where the icon of an achievement is cached, named after the state it depicts. */
		static ONLINESERVICESSTEAM_API FString GetAchievementIconUrl(const FString& AchievementId, bool bAchieved);

		/**
		 * Copies one icon out of the Steam image cache and writes it, answering with the path it landed
		 * on, or with nothing when there was no image to copy. Both the Steam call and the encoding happen
		 * where this is called: an icon is a handful of kilobytes, and a query fetches them once.
		 */
		static ONLINESERVICESSTEAM_API FString CacheAchievementIcon(const FString& AchievementId, bool bAchieved, int32 ImageIndex);

		/** Puts a cached icon on the definition it belongs to, on whichever side of it the state calls for. */
		ONLINESERVICESSTEAM_API void ApplyAchievementIcon(const FString& AchievementId, bool bAchieved, const FString& IconUrl);

		// Registered while the component is alive; see Initialize and PreShutdown.
		CCallbackManual<FAchievementsSteam, UserAchievementStored_t> OnUserAchievementStoredCallback { };
		CCallbackManual<FAchievementsSteam, UserAchievementIconFetched_t> OnUserAchievementIconFetchedCallback { };
	};
}
