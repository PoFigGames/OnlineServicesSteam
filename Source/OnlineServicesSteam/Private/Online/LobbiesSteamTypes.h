// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/LobbiesCommonTypes.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/Wrappers/SteamLobby.h"
#include "SteamUtils.h"

THIRD_PARTY_INCLUDES_START
#include "steam/isteammatchmaking.h"
THIRD_PARTY_INCLUDES_END


namespace UE::Online
{
	class FOnlineAsyncOp;
}


namespace PoFigGames::Online
{
	/**
	 * @struct FLobbyPrerequisitesSteam
	 *
	 * @brief What every lobby request is handled through, shared by the component and by each lobby
	 * object it creates.
	 */
	struct FLobbyPrerequisitesSteam
	{
		ISteamMatchmaking* LobbyInterfaceHandle { nullptr };
		Steam::FSteamCallDispatcher* CallDispatcher { nullptr };

		TSharedRef<UE::Online::FSchemaRegistry> SchemaRegistry { };
	};

	/**
	 * @enum ELobbyDetailsSource
	 *
	 * @brief Where the knowledge of a lobby this process is not a member of came from.
	 *
	 * Steam hands out the same lobby id whether it arrived in an invitation, through the overlay or out of a
	 * search, and a lobby is joinable from any of them; only a lobby this process has actually joined can be
	 * written to.
	 */
	enum class ELobbyDetailsSource
	{
		/**
		 * The lobby is joined and live.
		 * Attribute writes go through this one.
		 */
		Active,

		/**
		 * Came in with an invitation.
		 * Valid for joining.
		 */
		Invite,

		/**
		 * Came in from the Steam overlay.
		 * Valid for joining.
		 */
		UiEvent,

		/**
		 * Came out of a search.
		 * Valid for joining.
		 */
		Search
	};

	using Steam::FSteamLobbyAttributeData;

	/** Translates between the Steam lobby types and the join policies of the online services layer. */
	ELobbyType TranslateJoinPolicy(UE::Online::ELobbyJoinPolicy JoinPolicy);
	UE::Online::ELobbyJoinPolicy TranslateJoinPolicy(ELobbyType LobbyType);

	/**
	 * Build a lobby is published under and searched for, which is the build of the running game unless
	 * the OnlineServices.Steam.Lobbies.BuildIdOverride console variable says otherwise.
	 *
	 * The override exists so that a build can be pointed at the lobbies of another one, which is the
	 * only way to debug against a published build; it is a cheat variable and is not available in a
	 * shipping build.
	 */
	int32 GetLobbyBuildId();

	/**
	 * Writes one schema value the way Steam stores it: as plain text with no envelope of our own, so
	 * that the Steam matchmaking filters can compare it server side.
	 */
	FString ToSteamAttributeValue(const UE::Online::FSchemaVariant& Value);

	/**
	 * Reads a Steam value back as the type the schema declares for the attribute. Steam stores no type
	 * information, so an attribute the schema does not know cannot be translated at all.
	 */
	UE::Online::FSchemaVariant FromSteamAttributeValue(const FString& Value, UE::Online::ESchemaServiceAttributeSupportedTypeFlags SupportedTypes);

	/**
	 * The category of the schema a lobby is currently using, which is the only source of type
	 * information for its attributes. DerivedSchemaId may be unset, in which case only the attributes
	 * of the base schema are known; that is the state a search result starts in, until the schema
	 * compatibility attribute in the snapshot says which schema the lobby actually uses.
	 */
	const UE::Online::FSchemaCategoryDefinition* FindLobbySchemaCategory(
		const UE::Online::FSchemaRegistry& SchemaRegistry,
		const UE::Online::FSchemaCategoryId& CategoryId,
		const UE::Online::FSchemaId& DerivedSchemaId);

	/**
	 * @class FLobbyDetailsSteam
	 *
	 * @brief Lobby details are created based on the passed in user and are required to join a lobby.
	 */
	class FLobbyDetailsSteam final : public TSharedFromThis<FLobbyDetailsSteam>
	{
	public:
		UE_NONCOPYABLE(FLobbyDetailsSteam);
		FLobbyDetailsSteam() = delete;
		~FLobbyDetailsSteam() = default;

		static UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>> CreateFromLobbyId(const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites, UE::Online::FAccountId LocalAccountId, CSteamID LobbySteamId, ELobbyDetailsSource DetailsSource = ELobbyDetailsSource::Active);
		static UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>> CreateFromSearchResult(const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites, UE::Online::FAccountId LocalAccountId, uint32 ResultIndex);

		CSteamID            GetLobbySteamId() const { return LobbySteamId; }
		ELobbyDetailsSource GetDetailsSource() const { return LobbyDetailsSource; }

		/**
		 * Retrieve a lobby data snapshot from the Steam lobby.
		 * Attributes are translated through the schema on the way out.
		 */
		UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyServiceSnapshot> GetLobbySnapshot(bool bCopyMemberData) const;

		/**
		 * Retrieve a lobby member data snapshot from the Steam lobby.
		 * Attributes are translated through the schema on the way out.
		 */
		UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyMemberServiceSnapshot> GetLobbyMemberSnapshot(UE::Online::FAccountId MemberAccountId) const;

		/** Apply client side lobby changes to the lobby service. */
		UE::Online::FOnlineError ApplyLobbyDataUpdateFromLocalChanges(UE::Online::FAccountId LocalAccountId, const UE::Online::FLobbyClientServiceChanges& ServiceChanges) const;

		/**
		 * Publishes the metadata this plugin keeps for its own use rather than the game's: how many
		 * members the lobby holds and whether it is hosted by a development build.
		 *
		 * Steam refuses to show the member list of a lobby to anyone who has not joined it, so a lobby
		 * browser has no other way of learning how full a lobby is. Only the owner of a lobby may write
		 * its metadata; for anybody else this does nothing.
		 */
		void PublishOwnedMetadata() const;

	private:
		template <typename, ESPMode>
		friend class SharedPointerInternals::TIntrusiveReferenceController;

		TSharedRef<FLobbyPrerequisitesSteam> Prerequisites { };

		CSteamID                     LobbySteamId { k_steamIDNil };
		UE::Online::FAccountId       AssociatedLocalUser { };
		ELobbyDetailsSource          LobbyDetailsSource { ELobbyDetailsSource::Invite };
		UE::Online::ELobbyJoinPolicy JoinPolicy { UE::Online::ELobbyJoinPolicy::PublicAdvertised };

		/**
		 * Schema the last lobby snapshot reported, remembered so that the member snapshot taken right
		 * after it knows which member attributes to ask Steam for. Unset until the first snapshot.
		 */
		mutable UE::Online::FSchemaId LastKnownSchemaId { };

		FLobbyDetailsSteam(const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites, UE::Online::FAccountId LocalAccountId, CSteamID LobbySteamId, ELobbyDetailsSource LobbyDetailsSource, UE::Online::ELobbyJoinPolicy JoinPolicy = UE::Online::ELobbyJoinPolicy::PublicAdvertised);
	};

	/**
	 * @class FLobbyDataSteam
	 *
	 * @brief Lobby data is the bookkeeping object for a lobby. It contains the client-side representation of a lobby.
	 */
	class FLobbyDataSteam final : public TSharedFromThis<FLobbyDataSteam>
	{
	public:
		/** Removes the lobby from the registry which created it once the last reference to it is gone. */
		using FUnregisterFn = TUniqueFunction<void(UE::Online::FLobbyId)>;

		static UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDataSteam>> Create(
			const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites,
			UE::Online::FLobbyId LobbyId,
			const TSharedRef<FLobbyDetailsSteam>& LobbyDetails,
			FUnregisterFn UnregisterFn);

		FLobbyDataSteam() = delete;
		~FLobbyDataSteam();

		UE::Online::FLobbyId GetLobbyIdHandle() const { return LobbyClientData->GetPublicData().LobbyId; };
		const TSharedRef<UE::Online::FLobbyClientData>& GetLobbyClientData() const { return LobbyClientData; };

		CSteamID GetLobbySteamId() const { return LobbyDetailsInfo->GetLobbySteamId(); }

		/** The details the lobby was first created from; always valid for the lifetime of the lobby. */
		const TSharedRef<FLobbyDetailsSteam>& GetLobbyDetails() const { return LobbyDetailsInfo; }

		void AddUserLobbyDetails(UE::Online::FAccountId LocalAccountId, const TSharedPtr<FLobbyDetailsSteam>& LobbyDetails);
		TSharedPtr<FLobbyDetailsSteam> GetUserLobbyDetails(UE::Online::FAccountId LocalAccountId) const;

		/**
		 * Notifications can only be handled against live details, so this looks one up among
		 * lobby details if available.
		 */
		TSharedPtr<FLobbyDetailsSteam> GetActiveLobbyDetails() const;

	private:
		template <typename, ESPMode>
		friend class SharedPointerInternals::TIntrusiveReferenceController;

		FLobbyDataSteam(const TSharedRef<UE::Online::FLobbyClientData>& LobbyClientData, const TSharedRef<FLobbyDetailsSteam>& LobbyDetails, FUnregisterFn UnregisterFn);

		TSharedRef<UE::Online::FLobbyClientData> LobbyClientData { };
		TSharedRef<FLobbyDetailsSteam> LobbyDetailsInfo { };
		FUnregisterFn UnregisterFn { };

		TMap<UE::Online::FAccountId, TSharedPtr<FLobbyDetailsSteam>> UserLobbyDetails { };
	};


	/**
	 * @class FLobbyDataRegistrySteam
	 *
	 * @brief Every lobby this process knows about, found by the Steam id or by the id the services use.
	 */
	class FLobbyDataRegistrySteam : public TSharedFromThis<FLobbyDataRegistrySteam>
	{
	public:
		FLobbyDataRegistrySteam(const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites);

		TSharedPtr<FLobbyDataSteam> Find(CSteamID LobbySteamID) const;
		TSharedPtr<FLobbyDataSteam> Find(UE::Online::FLobbyId LobbyId) const;

		UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDataSteam>> FindOrCreateFromLobbyDetails(UE::Online::FAccountId LocalAccountId, const TSharedRef<FLobbyDetailsSteam>& LobbyDetails);

	private:
		void Register(const TSharedRef<FLobbyDataSteam>& LobbyIdHandleData);
		void Unregister(UE::Online::FLobbyId LobbyId);

		/** Hands a lobby the means to drop itself out of the indices below when it is destroyed. */
		FLobbyDataSteam::FUnregisterFn MakeUnregisterFn();

		TSharedRef<FLobbyPrerequisitesSteam> Prerequisites { };

		TMap<CSteamID, TWeakPtr<FLobbyDataSteam>>             LobbyIdIndex { };
		TMap<UE::Online::FLobbyId, TWeakPtr<FLobbyDataSteam>> LobbyIdHandleIndex { };

		uint32 NextHandleIndex { 1 };
	};

	/**
	 * @class FLobbyInviteDataSteam
	 *
	 * @brief Lobby invite data will keep the lobby object valid until the invitation has been accepted or rejected.
	 */
	class FLobbyInviteDataSteam final
	{
	public:
		/**
		 * Builds the bookkeeping for one invitation received through LobbyInvite_t.
		 *
		 * Steam issues no handle for an invitation, so an invitation is identified by nothing more than
		 * who sent it, who received it and which lobby it points at.
		 */
		static UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyInviteDataSteam>> Create(
			const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites,
			const TSharedRef<FLobbyDataRegistrySteam>&  LobbyDataRegistry,
			UE::Online::FAccountId                      LocalAccountId,
			CSteamID                                    LobbySteamId,
			CSteamID                                    SenderSteamId);

		TSharedRef<FLobbyDataSteam> GetLobbyData() const { return LobbyData; }

		/** Made up locally, because Steam has no id of its own for an invitation. */
		const FString& GetInviteId() const { return InviteId; }

		UE::Online::FAccountId GetReceiver() const { return Receiver; }
		UE::Online::FAccountId GetSender() const { return Sender; }

	private:
		template <typename, ESPMode>
		friend class SharedPointerInternals::TIntrusiveReferenceController;

		FLobbyInviteDataSteam(
			UE::Online::FAccountId                 Receiver,
			UE::Online::FAccountId                 Sender,
			const TSharedRef<FLobbyDetailsSteam>&  LobbyDetails,
			const TSharedRef<FLobbyDataSteam>&     LobbyData);

		UE::Online::FAccountId         Receiver { };
		UE::Online::FAccountId         Sender { };
		TSharedRef<FLobbyDetailsSteam> LobbyDetails { };
		TSharedRef<FLobbyDataSteam>    LobbyData { };
		FString                        InviteId { };
	};

	/**
	 * @class FLobbySearchSteam
	 *
	 * @brief The lobby search object is meant to hold the lifetimes of lobby search results so that the
	 * the details a join has made available.
	 */
	class FLobbySearchSteam final
	{
	public:
		FLobbySearchSteam() = delete;

		static TFuture<UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbySearchSteam>>> Create(
			const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites,
			const TSharedRef<FLobbyDataRegistrySteam>&  LobbyRegistry,
			const UE::Online::FFindLobbies::Params&     Params);

		TArray<TSharedRef<const UE::Online::FLobby>> GetLobbyResults() const;

	private:
		template <typename, ESPMode>
		friend class SharedPointerInternals::TIntrusiveReferenceController;

		FLobbySearchSteam(TArray<TSharedRef<FLobbyDataSteam>>&& Lobbies, const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites);

		TArray<TSharedRef<FLobbyDataSteam>> Lobbies { };
		TSharedRef<FLobbyPrerequisitesSteam> Prerequisites { };
	};
}
