// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/LobbiesSteamTypes.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "Online/OnlineIdSteam.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/Wrappers/SteamLobby.h"

// Engine
#include "HAL/IConsoleManager.h"


namespace PoFigGames::Online
{
	static TAutoConsoleVariable<int32> CVarLobbyBuildIdOverride(
		TEXT("OnlineServices.Steam.Lobbies.BuildIdOverride"),
		-1,
		TEXT("Build id to publish lobbies under and to search lobbies for, instead of the one Steam reports for the running game.\n")
		TEXT("Lobbies of a build a client cannot play with are normally hidden from it; setting this points the client at the lobbies of that build instead.\n")
		TEXT("A build Steam did not install itself, which is every build run from the editor, reports a build id of zero.\n")
		TEXT("-1: use the build id of the running game (default)."),
		ECVF_Cheat);

	int32 GetLobbyBuildId()
	{
		if (const int32 OverriddenBuildId = CVarLobbyBuildIdOverride.GetValueOnGameThread(); OverriddenBuildId >= 0)
		{
			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[GetLobbyBuildId] Using the overridden build id [%d] instead of [%d]."),
				OverriddenBuildId, Steam::GetSteamAppBuildId());

			return OverriddenBuildId;
		}

		return Steam::GetSteamAppBuildId();
	}

	ELobbyType TranslateJoinPolicy(const UE::Online::ELobbyJoinPolicy JoinPolicy)
	{
		switch (JoinPolicy)
		{
			case UE::Online::ELobbyJoinPolicy::PublicAdvertised:
				return k_ELobbyTypePublic;
			case UE::Online::ELobbyJoinPolicy::PublicNotAdvertised:
				return k_ELobbyTypeFriendsOnly;
			case UE::Online::ELobbyJoinPolicy::InvitationOnly:
				return k_ELobbyTypePrivate;
		}

		return k_ELobbyTypePrivate;
	}

	UE::Online::ELobbyJoinPolicy TranslateJoinPolicy(const ELobbyType LobbyType)
	{
		switch (LobbyType)
		{
			case k_ELobbyTypePublic:
				return UE::Online::ELobbyJoinPolicy::PublicAdvertised;
			case k_ELobbyTypeFriendsOnly:
				return UE::Online::ELobbyJoinPolicy::PublicNotAdvertised;
			default:
				// Private, Invisible and PrivateUnique all describe a lobby nobody can find on their own.
				break;
		}

		return UE::Online::ELobbyJoinPolicy::InvitationOnly;
	}

	FString ToSteamAttributeValue(const UE::Online::FSchemaVariant& Value)
	{
		switch (Value.GetType())
		{
			case UE::Online::ESchemaAttributeType::String:
				return Value.GetString();
			case UE::Online::ESchemaAttributeType::Int64:
				return LexToString(Value.GetInt64());
			case UE::Online::ESchemaAttributeType::Double:
				// Enough digits that reading the value back yields the same double.
				return FString::Printf(TEXT("%.17g"), Value.GetDouble());
			case UE::Online::ESchemaAttributeType::Bool:
				return Value.GetBoolean() ? TEXT("true") : TEXT("false");
			case UE::Online::ESchemaAttributeType::None:
				break;
		}

		return FString { };
	}

	UE::Online::FSchemaVariant FromSteamAttributeValue(const FString& Value, const UE::Online::ESchemaServiceAttributeSupportedTypeFlags SupportedTypes)
	{
		// A service attribute built from a schema attribute carries exactly one type, so the order these
		// are tested in only matters for a hand written service descriptor which allows several.
		if (EnumHasAnyFlags(SupportedTypes, UE::Online::ESchemaServiceAttributeSupportedTypeFlags::String))
		{
			return UE::Online::FSchemaVariant(Value);
		}

		if (EnumHasAnyFlags(SupportedTypes, UE::Online::ESchemaServiceAttributeSupportedTypeFlags::Int64))
		{
			int64 ParsedValue { 0 };
			LexFromString(ParsedValue, *Value);

			return UE::Online::FSchemaVariant(ParsedValue);
		}

		if (EnumHasAnyFlags(SupportedTypes, UE::Online::ESchemaServiceAttributeSupportedTypeFlags::Double))
		{
			double ParsedValue { 0.0 };
			LexFromString(ParsedValue, *Value);

			return UE::Online::FSchemaVariant(ParsedValue);
		}

		if (EnumHasAnyFlags(SupportedTypes, UE::Online::ESchemaServiceAttributeSupportedTypeFlags::Bool))
		{
			return UE::Online::FSchemaVariant(Value.ToBool());
		}

		return UE::Online::FSchemaVariant { };
	}

	const UE::Online::FSchemaCategoryDefinition* FindLobbySchemaCategory(
		const UE::Online::FSchemaRegistry&      SchemaRegistry,
		const UE::Online::FSchemaCategoryId&    CategoryId,
		const UE::Online::FSchemaId&            DerivedSchemaId)
	{
		if (!DerivedSchemaId.IsNone())
		{
			if (const auto DerivedSchema = SchemaRegistry.GetDefinition(DerivedSchemaId))
			{
				if (const auto DerivedCategory = DerivedSchema->Categories.Find(CategoryId))
				{
					return DerivedCategory;
				}
			}
		}

		if (const TSharedPtr<const UE::Online::FSchemaDefinition> BaseSchema = SchemaRegistry.GetDefinition(UE::Online::LobbyBaseSchemaId))
		{
			return BaseSchema->Categories.Find(CategoryId);
		}

		return nullptr;
	}

	UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>> FLobbyDetailsSteam::CreateFromLobbyId(const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites, UE::Online::FAccountId LocalAccountId, CSteamID LobbySteamId, ELobbyDetailsSource DetailsSource)
	{
		if (!LobbySteamId.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDetailsSteam::CreateFromLobbyId] Failed: Invalid lobby id. User [%s]"), *ToLogString(LocalAccountId));
			return UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>>(UE::Online::Errors::InvalidParams());
		}

		return UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>>(MakeShared<FLobbyDetailsSteam>(Prerequisites, LocalAccountId, LobbySteamId, DetailsSource, UE::Online::ELobbyJoinPolicy::InvitationOnly));
	}

	UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>> FLobbyDetailsSteam::CreateFromSearchResult(const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites, UE::Online::FAccountId LocalAccountId, uint32 ResultIndex)
	{
		const CSteamID LobbySteamId = Prerequisites->LobbyInterfaceHandle->GetLobbyByIndex(ResultIndex);
		if (!LobbySteamId.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDetailsSteam::CreateFromSearchResult] Failed: Invalid lobby id. User [%s]"), *ToLogString(LocalAccountId));
			return UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>>(UE::Online::Errors::InvalidParams());
		}

		return UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>>(MakeShared<FLobbyDetailsSteam>(Prerequisites, LocalAccountId, LobbySteamId, ELobbyDetailsSource::Search));
	}

	UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyServiceSnapshot> FLobbyDetailsSteam::GetLobbySnapshot(const bool bCopyMemberData) const
	{
		using FSnapshotResult = UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyServiceSnapshot>;

		if (Prerequisites->CallDispatcher == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDetailsSteam::GetLobbySnapshot] Failed: Steam services are not available. Lobby [%s]"), *ToLogString(GetLobbySteamId()));
			return FSnapshotResult(UE::Online::Errors::MissingInterface());
		}

		Steam::TSteamResult<Steam::Wrappers::FSteamGetLobbySnapshot> SteamSnapshotResult =
			Prerequisites->CallDispatcher->CallSync<Steam::Wrappers::FSteamGetLobbySnapshot>({ .LobbyId = GetLobbySteamId(), .bFetchMembers = bCopyMemberData });

		if (SteamSnapshotResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDetailsSteam::GetLobbySnapshot] Steam->GetLobbySnapshot Failed: Lobby [%s], Result [%s]"),
				*ToLogString(GetLobbySteamId()), *SteamSnapshotResult.GetErrorValue().GetLogString());

			return FSnapshotResult(MoveTemp(SteamSnapshotResult.GetErrorValue()));
		}

		auto SteamSnapshot = MoveTemp(SteamSnapshotResult.GetOkValue());

		// The compatibility attribute lives in the base schema, so the schema a lobby uses can be read
		// out of its metadata before anything else about it is known. This is the only path a search
		// result has to the attributes its schema adds on top of the base one.
		const auto BaseCategory = FindLobbySchemaCategory(*Prerequisites->SchemaRegistry, UE::Online::LobbySchemaCategoryId, { });

		if (BaseCategory == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDetailsSteam::GetLobbySnapshot] Failed: The lobby base schema declares no lobby category. Lobby [%s]"), *ToLogString(GetLobbySteamId()));
			return FSnapshotResult(UE::Online::Errors::InvalidState());
		}

		// Reassigned below once the schema the lobby uses is known, so the pointer itself is not const.
		auto LobbyCategory = BaseCategory;
		if (!BaseCategory->SchemaCompatibilityServiceAttributeId.IsNone())
		{
			for (const auto& Attribute : SteamSnapshot.Attributes)
			{
				// Looked up, never added: the key comes off the wire, and a name once interned is never
				// reclaimed. Every name the schema declares is already in the table by the time a lobby
				// is read, so Find resolves each of them exactly as Add would.
				if (UE::Online::FSchemaServiceAttributeId(*Attribute.Key, FNAME_Find) != BaseCategory->SchemaCompatibilityServiceAttributeId)
				{
					continue;
				}

				int64 CompatibilityId { 0 };
				LexFromString(CompatibilityId, *Attribute.Value);

				if (const auto DerivedSchema = Prerequisites->SchemaRegistry->GetDefinition(CompatibilityId))
				{
					if (const auto DerivedCategory = DerivedSchema->Categories.Find(UE::Online::LobbySchemaCategoryId))
					{
						LobbyCategory = DerivedCategory;
						LastKnownSchemaId = DerivedSchema->Id;
					}
				}

				break;
			}
		}

		// Interned once, here, rather than rebuilt inside the loop below.
		static const UE::Online::FSchemaServiceAttributeId JoinPolicyAttributeId { Steam::LobbyKeys::JoinPolicy };

		UE::Online::FLobbyServiceSnapshot LobbyServiceSnapshot;
		LobbyServiceSnapshot.MaxMembers = SteamSnapshot.MaxMembers;
		LobbyServiceSnapshot.JoinPolicy = JoinPolicy;

		// Translate the metadata. Steam keeps no type information, so an attribute the schema does not
		// describe cannot be read at all; that includes the keys this plugin reserves for itself and any
		// attribute belonging to a schema this build does not have.
		for (const auto& Attribute : SteamSnapshot.Attributes)
		{
			// A key the lobby's owner made up is not one this build declares, and must not be put into the
			// name table on their say-so: Steam allows many keys per lobby, a browser reads every result,
			// and names are never reclaimed.
			const UE::Online::FSchemaServiceAttributeId AttributeId { *Attribute.Key, FNAME_Find };

			if (AttributeId == JoinPolicyAttributeId)
			{
				int64 LobbyType { k_ELobbyTypePrivate };
				LexFromString(LobbyType, *Attribute.Value);

				// ELobbyType is unscoped with no fixed underlying type, so a value outside its range is not
				// merely wrong but undefined - and this value is decimal text a remote lobby owner wrote.
				// Anything else falls back to the most restrictive policy, which is what TranslateJoinPolicy
				// already intends by its default arm.
				LobbyServiceSnapshot.JoinPolicy = LobbyType >= k_ELobbyTypePrivate && LobbyType <= k_ELobbyTypePrivateUnique
					? TranslateJoinPolicy(static_cast<ELobbyType>(LobbyType))
					: UE::Online::ELobbyJoinPolicy::InvitationOnly;

				// This key belongs to the plugin rather than to the schema, so there is nothing further to
				// look up: falling through logged it as an attribute the schema does not describe.
				continue;
			}

			const auto AttributeDefinition = LobbyCategory->ServiceAttributeDefinitions.Find(AttributeId);
			if (AttributeDefinition == nullptr)
			{
				UE_LOG(LogOnlineServicesSteam, VeryVerbose, TEXT("[FLobbyDetailsSteam::GetLobbySnapshot] Skipping attribute the schema does not describe: Lobby [%s], Key [%s]"),
					*ToLogString(GetLobbySteamId()), *Attribute.Key);
				continue;
			}

			LobbyServiceSnapshot.SchemaServiceSnapshot.Attributes.Emplace(AttributeId, FromSteamAttributeValue(Attribute.Value, AttributeDefinition->Type));
		}

		auto UserSteamIds = MoveTemp(SteamSnapshot.Members);
		if (UserSteamIds.IsEmpty() && SteamSnapshot.OwnerId.IsValid())
		{
			UserSteamIds.Emplace(SteamSnapshot.OwnerId);
		}

		// Naming a member is a lookup in the local account id registry rather than a question for Steam,
		// so the snapshot is complete by the time this returns.
		const auto ResolvedAccountIds = FindOrAddAccountIds(UserSteamIds);

		for (int32 MemberIndex = 0; MemberIndex < UserSteamIds.Num(); ++MemberIndex)
		{
			const auto ResolvedMemberAccountId = ResolvedAccountIds[MemberIndex];

			if (UserSteamIds[MemberIndex] == SteamSnapshot.OwnerId)
			{
				LobbyServiceSnapshot.OwnerAccountId = ResolvedMemberAccountId;
			}

			LobbyServiceSnapshot.Members.Add(ResolvedMemberAccountId);
		}

		UE_LOG(LogOnlineServicesSteam, VeryVerbose, TEXT("[FLobbyDetailsSteam::GetLobbySnapshot] Succeeded: Lobby [%s], Members [%d], Attributes [%d]"),
			*ToLogString(GetLobbySteamId()), LobbyServiceSnapshot.Members.Num(), LobbyServiceSnapshot.SchemaServiceSnapshot.Attributes.Num());

		return FSnapshotResult(MoveTemp(LobbyServiceSnapshot));
	}

	UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyMemberServiceSnapshot> FLobbyDetailsSteam::GetLobbyMemberSnapshot(UE::Online::FAccountId MemberAccountId) const
	{
		UE::Online::FLobbyMemberServiceSnapshot LobbyMemberServiceSnapshot;
		LobbyMemberServiceSnapshot.AccountId = MemberAccountId;

		if (!MemberAccountId.IsValid() || Prerequisites->CallDispatcher == nullptr)
		{
			// A lobby the local user has not joined has no member list, so there is nobody to describe.
			return UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyMemberServiceSnapshot>(MoveTemp(LobbyMemberServiceSnapshot));
		}

		// Steam offers no way to walk the attributes of a lobby member the way it does for a lobby, so
		// the set of keys to ask for has to come from the schema the lobby is using.
		const auto MemberCategory = FindLobbySchemaCategory(*Prerequisites->SchemaRegistry, UE::Online::LobbyMemberSchemaCategoryId, LastKnownSchemaId);

		if (MemberCategory == nullptr || MemberCategory->ServiceAttributeDefinitions.IsEmpty())
		{
			return UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyMemberServiceSnapshot>(MoveTemp(LobbyMemberServiceSnapshot));
		}

		Steam::Wrappers::FSteamGetLobbyMemberData::Params MemberDataParams;
		MemberDataParams.LobbyId = GetLobbySteamId();
		MemberDataParams.MemberId = GetSteamUserIdChecked(MemberAccountId);
		MemberDataParams.Keys.Reserve(MemberCategory->ServiceAttributeDefinitions.Num());

		for (const auto& AttributeDefinition : MemberCategory->ServiceAttributeDefinitions)
		{
			MemberDataParams.Keys.Emplace(AttributeDefinition.Key.ToString());
		}

		Steam::TSteamResult<Steam::Wrappers::FSteamGetLobbyMemberData> MemberDataResult =
			Prerequisites->CallDispatcher->CallSync<Steam::Wrappers::FSteamGetLobbyMemberData>(MemberDataParams);

		if (MemberDataResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDetailsSteam::GetLobbyMemberSnapshot] Steam->GetLobbyMemberData Failed: Member [%s], Lobby [%s], Result [%s]"),
				*ToLogString(MemberAccountId), *ToLogString(LobbySteamId), *MemberDataResult.GetErrorValue().GetLogString());

			return UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyMemberServiceSnapshot>(MoveTemp(MemberDataResult.GetErrorValue()));
		}

		for (const auto& Attribute : MemberDataResult.GetOkValue().Attributes)
		{
			// Looked up rather than added, for the same reason as in GetLobbySnapshot: these keys are chosen
			// locally today, but leaving one of two identical constructions on FNAME_Add invites the next
			// reader to copy the wrong one.
			const UE::Online::FSchemaServiceAttributeId AttributeId { *Attribute.Key, FNAME_Find };
			if (const auto AttributeDefinition = MemberCategory->ServiceAttributeDefinitions.Find(AttributeId))
			{
				LobbyMemberServiceSnapshot.SchemaServiceSnapshot.Attributes.Emplace(AttributeId, FromSteamAttributeValue(Attribute.Value, AttributeDefinition->Type));
			}
		}

		UE_LOG(LogOnlineServicesSteam, VeryVerbose, TEXT("[FLobbyDetailsSteam::GetLobbyMemberSnapshot] Succeeded: Member [%s], Lobby [%s], Attributes [%d]"),
			*ToLogString(MemberAccountId), *ToLogString(LobbySteamId), LobbyMemberServiceSnapshot.SchemaServiceSnapshot.Attributes.Num());

		return UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyMemberServiceSnapshot>(MoveTemp(LobbyMemberServiceSnapshot));
	}

	UE::Online::FOnlineError FLobbyDetailsSteam::ApplyLobbyDataUpdateFromLocalChanges(UE::Online::FAccountId LocalAccountId, const UE::Online::FLobbyClientServiceChanges& ServiceChanges) const
	{
		if (Prerequisites->CallDispatcher == nullptr)
		{
			return UE::Online::Errors::MissingInterface();
		}

		Steam::Wrappers::FSteamUpdateLobbyData::Params UpdateParams;
		UpdateParams.LobbyId = GetLobbySteamId();

		if (ServiceChanges.JoinPolicy)
		{
			const ELobbyType LobbyType = TranslateJoinPolicy(*ServiceChanges.JoinPolicy);

			UpdateParams.NewLobbyType = LobbyType;

			// Steam has no way of reading the type of a lobby back, so it is published as metadata for
			// everybody who is only looking at the lobby rather than owning it.
			UpdateParams.Attributes.Emplace(Steam::FSteamLobbyAttributeData { .Key = Steam::LobbyKeys::JoinPolicy, .Value = LexToString(static_cast<int32>(LobbyType)) });
		}

		for (const auto& UpdatedAttribute : ServiceChanges.UpdatedAttributes)
		{
			UpdateParams.Attributes.Emplace(Steam::FSteamLobbyAttributeData { .Key = UpdatedAttribute.Key.ToString(), .Value = ToSteamAttributeValue(UpdatedAttribute.Value.Value) });
		}

		for (const auto& RemovedAttribute : ServiceChanges.RemovedAttributes)
		{
			UpdateParams.RemovedAttributes.Emplace(RemovedAttribute.ToString());
		}

		for (const auto& UpdatedAttribute : ServiceChanges.UpdatedMemberAttributes)
		{
			UpdateParams.MemberAttributes.Emplace(Steam::FSteamLobbyAttributeData { .Key = UpdatedAttribute.Key.ToString(), .Value = ToSteamAttributeValue(UpdatedAttribute.Value.Value) });
		}

		for (const auto& RemovedAttribute : ServiceChanges.RemovedMemberAttributes)
		{
			UpdateParams.RemovedMemberAttributes.Emplace(RemovedAttribute.ToString());
		}

		Steam::TSteamResult<Steam::Wrappers::FSteamUpdateLobbyData> UpdateResult =
			Prerequisites->CallDispatcher->CallSync<Steam::Wrappers::FSteamUpdateLobbyData>(UpdateParams);

		if (UpdateResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDetailsSteam::ApplyLobbyDataUpdateFromLocalChanges] Steam->UpdateLobbyData Failed: User [%s], Lobby [%s], Result [%s]"),
				*ToLogString(LocalAccountId), *ToLogString(GetLobbySteamId()), *UpdateResult.GetErrorValue().GetLogString());

			return UpdateResult.GetErrorValue();
		}

		UE_LOG(LogOnlineServicesSteam, VeryVerbose, TEXT("[FLobbyDetailsSteam::ApplyLobbyDataUpdateFromLocalChanges] Succeeded: User [%s], Lobby [%s]"),
			*ToLogString(LocalAccountId), *ToLogString(GetLobbySteamId()));

		return UE::Online::Errors::Success();
	}

	void FLobbyDetailsSteam::PublishOwnedMetadata() const
	{
		// The matchmaking handle is only check()ed when the interface comes up, and checks are compiled out
		// of the shipping game, so it is asked for here rather than trusted.
		if (Prerequisites->CallDispatcher == nullptr || Prerequisites->LobbyInterfaceHandle == nullptr)
		{
			return;
		}

		if (!AssociatedLocalUser.IsValid() || Prerequisites->LobbyInterfaceHandle->GetLobbyOwner(GetLobbySteamId()) != GetSteamUserIdChecked(AssociatedLocalUser))
		{
			// Steam only accepts metadata from the owner of a lobby.
			return;
		}

		const int32 MemberCount = Prerequisites->LobbyInterfaceHandle->GetNumLobbyMembers(GetLobbySteamId());
		if (MemberCount <= 0)
		{
			return;
		}

		Steam::Wrappers::FSteamUpdateLobbyData::Params UpdateParams;
		UpdateParams.LobbyId = GetLobbySteamId();

		// Every SetLobbyData Steam accepts is broadcast to every member as a LobbyDataUpdate_t, and this
		// plugin turns each of those into a full lobby snapshot plus one per member. So a value which has
		// not moved is not written again: the build id in particular cannot change for the life of the
		// process, yet this runs on creation, on every join and on every membership change. Reading a key
		// back is a local cache lookup and costs nothing.
		const auto PublishIfChanged = [this, &UpdateParams](const TCHAR* Key, const FString& Value)
		{
			if (UTF8_TO_TCHAR(Prerequisites->LobbyInterfaceHandle->GetLobbyData(GetLobbySteamId(), TCHAR_TO_UTF8(Key))) != Value)
			{
				UpdateParams.Attributes.Emplace(Steam::FSteamLobbyAttributeData { .Key = Key, .Value = Value });
			}
		};

		PublishIfChanged(Steam::LobbyKeys::MemberCount, LexToString(MemberCount));

		// Which build this lobby belongs to, so that a client of another one filters it out before it
		// ever reaches the lobby browser.
		PublishIfChanged(Steam::LobbyKeys::BuildId, LexToString(GetLobbyBuildId()));

		if (UpdateParams.Attributes.IsEmpty())
		{
			return;
		}

		// Steam answers with a failure for anybody who does not own the lobby, which is the normal case
		// for every member other than the host and is not worth reporting.
		Prerequisites->CallDispatcher->CallSync<Steam::Wrappers::FSteamUpdateLobbyData>(UpdateParams);
	}

	FLobbyDetailsSteam::FLobbyDetailsSteam(const TSharedRef<FLobbyPrerequisitesSteam>& InPrerequisites, const UE::Online::FAccountId InLocalAccountId, const CSteamID InLobbySteamId, const ELobbyDetailsSource InLobbyDetailsSource, UE::Online::ELobbyJoinPolicy InJoinPolicy)
		: Prerequisites(InPrerequisites)
		, LobbySteamId(InLobbySteamId)
		, AssociatedLocalUser(InLocalAccountId)
		, LobbyDetailsSource(InLobbyDetailsSource)
		, JoinPolicy(InJoinPolicy)
	{

	}

	UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDataSteam>> FLobbyDataSteam::Create(const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites,
		UE::Online::FLobbyId LobbyId, const TSharedRef<FLobbyDetailsSteam>& LobbyDetails, FUnregisterFn UnregisterFn)
	{
		using FLobbyDataResult = UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDataSteam>>;

		auto SnapshotResult = LobbyDetails->GetLobbySnapshot(false);
		if (SnapshotResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDataSteam::Create] FLobbyDetailsSteam::GetLobbySnapshot Failed: Lobby[%s], Result[%s]"),
				*ToLogString(LobbyId), *SnapshotResult.GetErrorValue().GetLogString());

			return FLobbyDataResult(MoveTemp(SnapshotResult.GetErrorValue()));
		}

		auto LobbySnapshot = MoveTemp(SnapshotResult.GetOkValue());
		auto NewLobbyClientData = MakeShared<UE::Online::FLobbyClientData>(LobbyId, Prerequisites->SchemaRegistry);

		// Read what the members published and fold it into the lobby.
		TMap<UE::Online::FAccountId, UE::Online::FLobbyMemberServiceSnapshot> MemberSnapshots;
		for (const auto& MemberAccountId : LobbySnapshot.Members)
		{
			auto LobbyMemberSnapshotResult = LobbyDetails->GetLobbyMemberSnapshot(MemberAccountId);
			if (LobbyMemberSnapshotResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDataSteam::Create] FLobbyDetailsSteam::GetLobbyMemberSnapshot Failed: Lobby[%s], User[%s], Result[%s]"),
					*ToLogString(LobbyId), *ToLogString(MemberAccountId), *LobbyMemberSnapshotResult.GetErrorValue().GetLogString());

				return FLobbyDataResult(MoveTemp(LobbyMemberSnapshotResult.GetErrorValue()));
			}

			MemberSnapshots.Emplace(MemberAccountId, MoveTemp(LobbyMemberSnapshotResult.GetOkValue()));
		}

		auto PrepareServiceLobbySnapshotResult =
			NewLobbyClientData->PrepareServiceSnapshot({ .LobbySnapshot = MoveTemp(LobbySnapshot), .LobbyMemberSnapshots = MoveTemp(MemberSnapshots) });

		if (PrepareServiceLobbySnapshotResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyDataSteam::Create] FLobbyClientData::PrepareServiceSnapshot Failed: Lobby[%s], Result[%s]"),
				*ToLogString(LobbyId), *PrepareServiceLobbySnapshotResult.GetErrorValue().GetLogString());

			return FLobbyDataResult(MoveTemp(PrepareServiceLobbySnapshotResult.GetErrorValue()));
		}

		NewLobbyClientData->CommitServiceSnapshot({ });

		UE_LOG(LogOnlineServicesSteam, VeryVerbose, TEXT("[FLobbyDataSteam::Create] Succeeded: Lobby[%s]"),
			*ToLogString(LobbyDetails->GetLobbySteamId()));

		return FLobbyDataResult(MakeShared<FLobbyDataSteam>(NewLobbyClientData, LobbyDetails, MoveTemp(UnregisterFn)));
	}

	void FLobbyDataSteam::AddUserLobbyDetails(UE::Online::FAccountId LocalAccountId, const TSharedPtr<FLobbyDetailsSteam>& InLobbyDetails)
	{
		if (const TSharedPtr<FLobbyDetailsSteam> ExistingDetails = GetUserLobbyDetails(LocalAccountId))
		{
			if (ExistingDetails->GetDetailsSource() < InLobbyDetails->GetDetailsSource())
			{
				return;
			}
		}

		UserLobbyDetails.Add(LocalAccountId, InLobbyDetails);
	}

	TSharedPtr<FLobbyDetailsSteam> FLobbyDataSteam::GetUserLobbyDetails(UE::Online::FAccountId LocalAccountId) const
	{
		if (const TSharedPtr<FLobbyDetailsSteam>* Result = UserLobbyDetails.Find(LocalAccountId))
		{
			return *Result;
		}

		return nullptr;
	}

	TSharedPtr<FLobbyDetailsSteam> FLobbyDataSteam::GetActiveLobbyDetails() const
	{
		TSharedPtr<FLobbyDetailsSteam> FoundDetails;

		for (const auto& LobbyDetails : UserLobbyDetails)
		{
			if (LobbyDetails.Value->GetDetailsSource() == ELobbyDetailsSource::Active)
			{
				FoundDetails = LobbyDetails.Value;
				break;
			}
		}

		return FoundDetails;
	}

	FLobbyDataSteam::FLobbyDataSteam(const TSharedRef<UE::Online::FLobbyClientData>& InLobbyClientData,
		const TSharedRef<FLobbyDetailsSteam>& InLobbyDetails, FUnregisterFn InUnregisterFn)
		: LobbyClientData(InLobbyClientData)
		, LobbyDetailsInfo(InLobbyDetails)
		, UnregisterFn(MoveTemp(InUnregisterFn))
	{
	}

	FLobbyDataSteam::~FLobbyDataSteam()
	{
		// The registry indexes lobbies weakly, so the entries it keeps have to be dropped by the lobby
		// itself; without this they pile up for the lifetime of the process.
		if (UnregisterFn)
		{
			UnregisterFn(LobbyClientData->GetPublicData().LobbyId);
		}
	}

	FLobbyDataRegistrySteam::FLobbyDataRegistrySteam(const TSharedRef<FLobbyPrerequisitesSteam>& InPrerequisites)
		: Prerequisites(InPrerequisites)
	{
	}

	TSharedPtr<FLobbyDataSteam> FLobbyDataRegistrySteam::Find(CSteamID LobbySteamID) const
	{
		const auto Result = LobbyIdIndex.Find(LobbySteamID);
		return Result ? Result->Pin() : nullptr;
	}

	TSharedPtr<FLobbyDataSteam> FLobbyDataRegistrySteam::Find(UE::Online::FLobbyId LobbyId) const
	{
		const auto Result = LobbyIdHandleIndex.Find(LobbyId);
		return Result ? Result->Pin() : nullptr;
	}

	UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDataSteam>> FLobbyDataRegistrySteam::FindOrCreateFromLobbyDetails(UE::Online::FAccountId LocalAccountId, const TSharedRef<FLobbyDetailsSteam>& LobbyDetails)
	{
		using FLobbyDataResult = UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDataSteam>>;

		// Reuse the entry if one is already filed.
		if (const TSharedPtr<FLobbyDataSteam> FindResult = Find(LobbyDetails->GetLobbySteamId()))
		{
			UE_LOG(LogOnlineServicesSteam, VeryVerbose, TEXT("[%s] Succeeded: Lobby data already exists. User[%s], Lobby[%s]"), UTF8_TO_TCHAR(__FUNCTION__),
				*ToLogString(LocalAccountId), *ToLogString(FindResult->GetLobbySteamId()));

			FindResult->AddUserLobbyDetails(LocalAccountId, LobbyDetails);
			return FLobbyDataResult(FindResult.ToSharedRef());
		}

		// Create a new lobby id object.
		const UE::Online::FLobbyId LobbyId { UE::Online::EOnlineServices::Steam, NextHandleIndex++ };

		// Build the entry from the details handle.
		auto Result = FLobbyDataSteam::Create(Prerequisites, LobbyId, LobbyDetails, MakeUnregisterFn());
		if (Result.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] Failed: User[%s], Lobby[%s], Result[%s]"), UTF8_TO_TCHAR(__FUNCTION__),
				*ToLogString(LocalAccountId), *ToLogString(LobbyDetails->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

			return Result;
		}

		UE_LOG(LogOnlineServicesSteam, VeryVerbose, TEXT("[%s] Succeeded: Created new lobby data. User[%s], Lobby[%s]"), UTF8_TO_TCHAR(__FUNCTION__),
			*ToLogString(LocalAccountId), *ToLogString(Result.GetOkValue()->GetLobbySteamId()));

		const auto& LobbyDataSteam = Result.GetOkValue();
		LobbyDataSteam->AddUserLobbyDetails(LocalAccountId, LobbyDetails);
		Register(LobbyDataSteam);

		return Result;
	}

	void FLobbyDataRegistrySteam::Register(const TSharedRef<FLobbyDataSteam>& LobbyIdHandleData)
	{
		LobbyIdIndex.Add(LobbyIdHandleData->GetLobbySteamId(), LobbyIdHandleData);
		LobbyIdHandleIndex.Add(LobbyIdHandleData->GetLobbyIdHandle(), LobbyIdHandleData);
	}

	void FLobbyDataRegistrySteam::Unregister(const UE::Online::FLobbyId LobbyId)
	{
		// Called while the lobby is being destroyed, so the weak entry can no longer be resolved back to
		// it; the Steam id has to be found by walking the index instead.
		LobbyIdHandleIndex.Remove(LobbyId);

		for (auto It = LobbyIdIndex.CreateIterator(); It; ++It)
		{
			if (!It.Value().IsValid())
			{
				It.RemoveCurrent();
			}
		}
	}

	FLobbyDataSteam::FUnregisterFn FLobbyDataRegistrySteam::MakeUnregisterFn()
	{
		return [WeakThis = AsWeak()](const UE::Online::FLobbyId LobbyId)
		{
			if (const TSharedPtr<FLobbyDataRegistrySteam> StrongThis = WeakThis.Pin())
			{
				StrongThis->Unregister(LobbyId);
			}
		};
	}

	/** Maps a schema comparison onto the Steam one. Steam offers no equivalent for the set operations. */
	static bool TranslateComparisonOp(const UE::Online::ESchemaAttributeComparisonOp ComparisonOp, ELobbyComparison& OutComparison)
	{
		switch (ComparisonOp)
		{
			case UE::Online::ESchemaAttributeComparisonOp::Equals:
				OutComparison = k_ELobbyComparisonEqual;
				return true;
			case UE::Online::ESchemaAttributeComparisonOp::NotEquals:
				OutComparison = k_ELobbyComparisonNotEqual;
				return true;
			case UE::Online::ESchemaAttributeComparisonOp::GreaterThan:
				OutComparison = k_ELobbyComparisonGreaterThan;
				return true;
			case UE::Online::ESchemaAttributeComparisonOp::GreaterThanEquals:
				OutComparison = k_ELobbyComparisonEqualToOrGreaterThan;
				return true;
			case UE::Online::ESchemaAttributeComparisonOp::LessThan:
				OutComparison = k_ELobbyComparisonLessThan;
				return true;
			case UE::Online::ESchemaAttributeComparisonOp::LessThanEquals:
				OutComparison = k_ELobbyComparisonEqualToOrLessThan;
				return true;
			default:
				break;
		}

		return false;
	}

	/**
	 * Adds one filter to the lobby list request Steam is about to issue. Attribute values are stored
	 * exactly as the game means them, so Steam can compare them itself and only matching lobbies come
	 * back over the network.
	 */
	static void ApplySearchFilter(ISteamMatchmaking& Matchmaking, const UE::Online::FSchemaCategoryDefinition& LobbyCategory, const UE::Online::FFindLobbySearchFilter& Filter)
	{
		const auto AttributeDefinition = LobbyCategory.SchemaAttributeDefinitions.Find(Filter.AttributeName);
		if (AttributeDefinition == nullptr || AttributeDefinition->ServiceAttributeId.IsNone())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbySearchSteam::Create] Ignoring filter on an attribute the lobby base schema does not declare: [%s]"),
				*Filter.AttributeName.ToString());
			return;
		}

		if (!EnumHasAnyFlags(AttributeDefinition->Flags, UE::Online::ESchemaAttributeFlags::Searchable))
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbySearchSteam::Create] Ignoring filter on an attribute which is not searchable: [%s]"),
				*Filter.AttributeName.ToString());
			return;
		}

		ELobbyComparison Comparison { k_ELobbyComparisonEqual };
		if (!TranslateComparisonOp(Filter.ComparisonOp, Comparison))
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbySearchSteam::Create] Ignoring filter on [%s]: Steam has no equivalent of comparison [%s]"),
				*Filter.AttributeName.ToString(), LexToString(Filter.ComparisonOp));
			return;
		}

		const FString AttributeKey = AttributeDefinition->ServiceAttributeId.ToString();

		// Only whole numbers are compared numerically; everything else, booleans included, is stored as
		// text and has to be compared as text.
		if (Filter.ComparisonValue.GetType() == UE::Online::ESchemaAttributeType::Int64)
		{
			Matchmaking.AddRequestLobbyListNumericalFilter(TCHAR_TO_UTF8(*AttributeKey), static_cast<int32>(Filter.ComparisonValue.GetInt64()), Comparison);
		}
		else
		{
			Matchmaking.AddRequestLobbyListStringFilter(TCHAR_TO_UTF8(*AttributeKey), TCHAR_TO_UTF8(*ToSteamAttributeValue(Filter.ComparisonValue)), Comparison);
		}
	}

	UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyInviteDataSteam>> FLobbyInviteDataSteam::Create(
		const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites,
		const TSharedRef<FLobbyDataRegistrySteam>&  LobbyDataRegistry,
		const UE::Online::FAccountId                LocalAccountId,
		const CSteamID                              LobbySteamId,
		const CSteamID                              SenderSteamId)
	{
		using FInviteResult = UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyInviteDataSteam>>;

		auto LobbyDetailsResult = FLobbyDetailsSteam::CreateFromLobbyId(Prerequisites, LocalAccountId, LobbySteamId, ELobbyDetailsSource::Invite);
		if (LobbyDetailsResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyInviteDataSteam::Create] FLobbyDetailsSteam::CreateFromLobbyId Failed: User [%s], Result [%s]"),
				*ToLogString(LocalAccountId), *LobbyDetailsResult.GetErrorValue().GetLogString());

			return FInviteResult(MoveTemp(LobbyDetailsResult.GetErrorValue()));
		}

		// The sender has to become an account id before the invitation can name them.
		const auto SenderAccountId = FindOrAddAccountId(SenderSteamId);
		if (!SenderAccountId.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyInviteDataSteam::Create] Failed: The invitation names no sender. User [%s], Lobby [%s]"),
				*ToLogString(LocalAccountId), *ToLogString(LobbySteamId));

			return FInviteResult(UE::Online::Errors::InvalidParams());
		}

		const auto LobbyDetails = LobbyDetailsResult.GetOkValue();

		auto LobbyDataResult = LobbyDataRegistry->FindOrCreateFromLobbyDetails(LocalAccountId, LobbyDetails);
		if (LobbyDataResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbyInviteDataSteam::Create] FindOrCreateFromLobbyDetails Failed: User [%s], Lobby [%s], Result [%s]"),
				*ToLogString(LocalAccountId), *ToLogString(LobbySteamId), *LobbyDataResult.GetErrorValue().GetLogString());

			return FInviteResult(MoveTemp(LobbyDataResult.GetErrorValue()));
		}

		return FInviteResult(MakeShared<FLobbyInviteDataSteam>(LocalAccountId, SenderAccountId, LobbyDetails, LobbyDataResult.GetOkValue()));
	}

	FLobbyInviteDataSteam::FLobbyInviteDataSteam(
		const UE::Online::FAccountId                InReceiver,
		const UE::Online::FAccountId                InSender,
		const TSharedRef<FLobbyDetailsSteam>&       InLobbyDetails,
		const TSharedRef<FLobbyDataSteam>&          InLobbyData)
		: Receiver(InReceiver)
		, Sender(InSender)
		, LobbyDetails(InLobbyDetails)
		, LobbyData(InLobbyData)
		, InviteId(FString::Printf(TEXT("%llu:%llu"), InLobbyDetails->GetLobbySteamId().ConvertToUint64(), GetSteamUserIdChecked(InSender).ConvertToUint64()))
	{

	}

	TFuture<UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbySearchSteam>>> FLobbySearchSteam::Create(const TSharedRef<FLobbyPrerequisitesSteam>& Prerequisites, const TSharedRef<FLobbyDataRegistrySteam>& LobbyRegistry, const UE::Online::FFindLobbies::Params& Params)
	{
		TPromise<UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbySearchSteam>>> Promise;
		auto Future = Promise.GetFuture();

		// Steam latches every filter onto the matchmaking interface and holds it until the next
		// RequestLobbyList consumes it, so nothing may be applied before the request is known to be
		// issuable: a search which bailed after this point silently poisoned the next one that went out.
		if (Prerequisites->CallDispatcher == nullptr || Prerequisites->LobbyInterfaceHandle == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbySearchSteam::Create] Failed: Steam services are not available. User [%s]"),
				*ToLogString(Params.LocalAccountId));

			Promise.EmplaceValue(UE::Online::Errors::MissingInterface());
			return Future;
		}

		Prerequisites->LobbyInterfaceHandle->AddRequestLobbyListResultCountFilter(Params.MaxResults);

		// Only lobbies of the very same build are worth showing: a client cannot join a version it does
		// not share, and a development build has no business seeing a published one or the other way
		// round. Steam does the comparison itself, so a mismatched lobby never crosses the network.
		Prerequisites->LobbyInterfaceHandle->AddRequestLobbyListNumericalFilter(TCHAR_TO_UTF8(Steam::LobbyKeys::BuildId), GetLobbyBuildId(), k_ELobbyComparisonEqual);

		// Searchable attributes are required to live in the base schema, which is why the filters can be
		// resolved without knowing which schema any particular lobby uses.
		if (const auto LobbyCategory = FindLobbySchemaCategory(*Prerequisites->SchemaRegistry, UE::Online::LobbySchemaCategoryId, { }))
		{
			for (const auto& Filter : Params.Filters)
			{
				ApplySearchFilter(*Prerequisites->LobbyInterfaceHandle, *LobbyCategory, Filter);
			}
		}
		else if (!Params.Filters.IsEmpty())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbySearchSteam::Create] Searching unfiltered: the lobby base schema declares no lobby category. User [%s], Filters [%d]"),
				*ToLogString(Params.LocalAccountId), Params.Filters.Num());
		}

		Steam::Wrappers::FSteamRequestLobbyList::Params RequestLobbyListRequest { /* Use default values */ };
		Prerequisites->CallDispatcher->Call<Steam::Wrappers::FSteamRequestLobbyList>(MoveTemp(RequestLobbyListRequest))
		.Next([Promise = MoveTemp(Promise), Prerequisites, LobbyRegistry, LocalAccountId = Params.LocalAccountId](Steam::TSteamResult<Steam::Wrappers::FSteamRequestLobbyList>&& LobbyListResult) mutable {
			if (LobbyListResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbySearchSteam::Create] Steam->RequestLobbyList Failed: User [%s], Result [%s]"),
					*ToLogString(LocalAccountId), *LobbyListResult.GetErrorValue().GetLogString());

				Promise.EmplaceValue(MoveTemp(LobbyListResult.GetErrorValue()));
				return;
			}

			const auto& Result = LobbyListResult.GetOkValue();

			TArray<TSharedRef<FLobbyDataSteam>> ResolvedResults { };
			ResolvedResults.Reserve(Result.LobbyCount);

			for (uint32 Idx = 0u; Idx < Result.LobbyCount; ++Idx)
			{
				auto LobbyDetails = FLobbyDetailsSteam::CreateFromSearchResult(Prerequisites, LocalAccountId, Idx);
				if (LobbyDetails.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbySearchSteam::Create] FLobbyDetailsSteam::CreateFromSearchResult Failed: User [%s], Result [%s]"),
						*ToLogString(LocalAccountId), *LobbyDetails.GetErrorValue().GetLogString());

					Promise.EmplaceValue(MoveTemp(LobbyDetails.GetErrorValue()));
					return;
				}

				auto LobbyData = LobbyRegistry->FindOrCreateFromLobbyDetails(LocalAccountId, LobbyDetails.GetOkValue());
				if (LobbyData.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbySearchSteam::Create] FLobbyDataRegistrySteam::FindOrCreateFromLobbyDetails Failed: Unable to resolve search result. User [%s], Result [%s]"),
						*ToLogString(LocalAccountId), *LobbyData.GetErrorValue().GetLogString());

					Promise.EmplaceValue(MoveTemp(LobbyData.GetErrorValue()));
					return;
				}

				ResolvedResults.Add(MoveTemp(LobbyData.GetOkValue()));
			}

			UE_LOG(LogOnlineServicesSteam, VeryVerbose, TEXT("[FLobbySearchSteam::Create] Succeeded: User [%s], NumResults [%d]"),
				*ToLogString(LocalAccountId), ResolvedResults.Num());

			Promise.EmplaceValue(MakeShared<FLobbySearchSteam>(MoveTemp(ResolvedResults), Prerequisites));
		});

		return Future;
	}

	TArray<TSharedRef<const UE::Online::FLobby>> FLobbySearchSteam::GetLobbyResults() const
	{
		TArray<TSharedRef<const UE::Online::FLobby>> Result;
		Result.Reserve(Lobbies.Num());

		for (const auto& LobbyData : Lobbies)
		{
			// Members is empty for a lobby the local user has not joined; how full such a lobby is has to
			// be read from the MemberCount attribute its owner publishes.
			Result.Add(MakeShared<UE::Online::FLobby>(*LobbyData->GetLobbyClientData()->GetPublicDataPtr()));
		}

		return Result;
	}

	FLobbySearchSteam::FLobbySearchSteam(TArray<TSharedRef<FLobbyDataSteam>>&& InLobbies, const TSharedRef<FLobbyPrerequisitesSteam>& InPrerequisites)
		: Lobbies(MoveTemp(InLobbies))
		, Prerequisites(InPrerequisites)
	{

	}
}
