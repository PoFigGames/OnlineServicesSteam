// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineComponentSteam.h"
#include "Online/StatsCommon.h"
#include "SteamUtils.h"


namespace PoFigGames::Online
{
	/**
	 * @class FStatsSteam
	 *
	 * @brief Steam Stats Online Component
	 *
	 * Steam stores an absolute value per stat and aggregates nothing on its side, so how an update is
	 * folded into what is already there is decided here, against the value Steam holds rather than against
	 * a local cache which may have been filled in another session.
	 *
	 * The set of stats is not readable from the client at all: Steam knows their names and types from the
	 * backend of the title, and answers only for the names it is asked about. The stat definitions of the
	 * configuration are therefore the list of what exists, and their default values say which of the two
	 * Steam types, whole number or floating point, each stat is stored as.
	 *
	 * A client may only write the stats of the user running it. Writing the stats of somebody else is what
	 * a game server does through an interface of its own, which this component does not speak.
	 */
	class FStatsSteam : public TOnlineComponentSteam<UE::Online::FStatsCommon>
	{
	public:
		using Super = FStatsCommon;
		using TOnlineComponentSteam::TOnlineComponentSteam;

	#pragma region IStats
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryStats>      QueryStats(UE::Online::FQueryStats::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FBatchQueryStats> BatchQueryStats(UE::Online::FBatchQueryStats::Params&& Params) override;

	#if !UE_BUILD_SHIPPING
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FResetStats> ResetStats(UE::Online::FResetStats::Params&& Params) override;
	#endif // !UE_BUILD_SHIPPING
	#pragma endregion IStats

	protected:
		/**
		 * Every write goes through here, whether the game called UpdateStats or fired an event, so this is
		 * where the values reach Steam.
		 */
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpRef<UE::Online::FUpdateStats> UpdateStats_Implementation(UE::Online::FUpdateStats::Params&& Params) override;

	private:
		/**
		 * Reads the named stats of one user out of the Steam cache, which holds them once the stats of that
		 * user have been received. A stat the configuration does not describe cannot be read, because
		 * nothing says which of the two Steam types to ask for.
		 */
		ONLINESERVICESSTEAM_API UE::Online::FUserStats ReadUserStats(const UE::Online::FAccountId& TargetAccountId, const CSteamID& TargetSteamId,
			const TArray<FString>& StatNames) const;

		/**
		 * Folds an update into the values Steam holds and writes the result back, without committing it.
		 * Answers with the values as they now stand, so that the cache can be brought in step once the
		 * commit goes through.
		 */
		ONLINESERVICESSTEAM_API UE::Online::TDefaultErrorResultInternal<UE::Online::FUserStats> WriteUserStats(const UE::Online::FUserStats& UpdateUserStats) const;
	};
}
