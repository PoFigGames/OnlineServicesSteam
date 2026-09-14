// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineComponentSteam.h"
#include "Online/PresenceCommon.h"
#include "SteamUtils.h"

THIRD_PARTY_INCLUDES_START
#include "steam/isteamfriends.h"
THIRD_PARTY_INCLUDES_END


namespace PoFigGames::Online
{
	/**
	 * @class FPresenceSteam
	 *
	 * @brief Steam Presence Online Component
	 *
	 * Steam splits presence in two. What a user is doing is rich presence, a handful of key and value
	 * strings the game publishes for itself and reads back for others; whether that user is online, away
	 * or busy is the state of their Steam account, which the client reports and no game may set. The
	 * component writes the first and reads both.
	 *
	 * Two of the keys are Steam's own and are filled in from the fields of the interface which mean the
	 * same thing: steam_display names a localisation token the Steam client renders in the language of
	 * whoever is looking, which is what the status string of a presence update is, and status is the plain
	 * text older clients show beside the user. Everything else the game publishes is passed through under
	 * its own name.
	 *
	 * Steam keeps the rich presence of somebody the local user can actually see, which means friends and
	 * everybody in the same lobby or on the same server, and nothing at all about anybody else.
	 */
	class FPresenceSteam : public TOnlineComponentSteam<UE::Online::FPresenceCommon>
	{
	public:
		using Super = FPresenceCommon;
		using TOnlineComponentSteam::TOnlineComponentSteam;

	#pragma region TOnlineComponent
		ONLINESERVICESSTEAM_API virtual void Initialize() override;
		ONLINESERVICESSTEAM_API virtual void PreShutdown() override;
	#pragma endregion TOnlineComponent

	#pragma region IPresence
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryPresence>         QueryPresence(UE::Online::FQueryPresence::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FBatchQueryPresence>    BatchQueryPresence(UE::Online::FBatchQueryPresence::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetCachedPresence>            GetCachedPresence(UE::Online::FGetCachedPresence::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FUpdatePresence>        UpdatePresence(UE::Online::FUpdatePresence::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FPartialUpdatePresence> PartialUpdatePresence(UE::Online::FPartialUpdatePresence::Params&& Params) override;
	#pragma endregion IPresence

	private:
		/** Reads everything Steam knows about a user right now into a presence. */
		ONLINESERVICESSTEAM_API TSharedRef<UE::Online::FUserPresence> ReadUserPresence(const UE::Online::FAccountId& TargetAccountId, const CSteamID& TargetSteamId) const;

		/**
		 * Publishes the rich presence of the local user, replacing whatever was published before, and keeps
		 * what was published.
		 *
		 * Reading rich presence back out of Steam is meant for the users one can see rather than for
		 * oneself, so the presence of the local user is remembered here rather than asked for again; only
		 * the state of the account, which the game does not own, is read from Steam every time.
		 */
		ONLINESERVICESSTEAM_API UE::Online::FOnlineError PublishLocalPresence(const UE::Online::FAccountId& LocalAccountId,
			const CSteamID& LocalSteamId, const TSharedRef<UE::Online::FUserPresence>& Presence);

		/** Hands the rich presence to Steam, replacing whatever was published before. */
		ONLINESERVICESSTEAM_API UE::Online::FOnlineError WriteLocalPresence(const UE::Online::FUserPresence& Presence) const;

		/** What the local user last published, or what Steam says about them when they published nothing. */
		ONLINESERVICESSTEAM_API TSharedRef<UE::Online::FUserPresence> GetLocalPresence(const UE::Online::FAccountId& LocalAccountId, const CSteamID& LocalSteamId) const;

		/** Re-reads a user and tells whoever asked to be told, if anybody did. */
		ONLINESERVICESSTEAM_API void RefreshPresence(const CSteamID& TargetSteamId);

		/** Steam announces a change of state or of rich presence to every listener. */
		ONLINESERVICESSTEAM_API void OnPersonaStateChange(PersonaStateChange_t* Message);
		ONLINESERVICESSTEAM_API void OnFriendRichPresenceUpdate(FriendRichPresenceUpdate_t* Message);

		/** Everybody who has been queried, whether or not their changes are still of interest. */
		TMap<UE::Online::FAccountId, TSharedRef<UE::Online::FUserPresence>> CachedPresences { };

		/** Users a caller asked to keep hearing about; the rest are cached but announced to nobody. */
		TSet<UE::Online::FAccountId> WatchedUsers { };

		// Registered while the component is alive; see Initialize and PreShutdown.
		CCallbackManual<FPresenceSteam, PersonaStateChange_t> OnPersonaStateChangeCallback { };
		CCallbackManual<FPresenceSteam, FriendRichPresenceUpdate_t> OnFriendRichPresenceUpdateCallback { };
	};
}
