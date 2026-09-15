// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/LobbiesSteam.h"

// Project
#include "LobbiesSteamTypes.h"
#include "OnlineServicesSteamLogChannels.h"
#include "SocketSubsystemSteam.h"
#include "SteamInterfaces.h"
#include "Online/OnlineIdSteam.h"
#include "Online/OnlineServicesSteam.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/SteamResult.h"
#include "Steam/Wrappers/SteamLobby.h"

// Engine
#include "Online/OnlineUtilsCommon.h"


namespace PoFigGames::Online
{
	void FLobbiesSteam::AddActiveLobby(UE::Online::FAccountId LocalAccountId, bool bPresenceEnabled, const TSharedRef<FLobbyDataSteam>& LobbyData)
	{
		// Add bookkeeping for the user.
		ActiveLobbies.FindOrAdd(LocalAccountId).Add(LobbyData);

		if (bPresenceEnabled)
		{
			PresenceLobbiesUserMap.FindOrAdd(LocalAccountId, LobbyData->GetLobbyIdHandle());
		}

		// A lobby can be created after the server of this process is already up, in which case there is no
		// announcement left to wait for; a member who owns nothing is turned away by the call itself.
		BindGameServerToOwnedLobby(LobbyData);
	}

	void FLobbiesSteam::RemoveActiveLobby(UE::Online::FAccountId LocalAccountId, const TSharedRef<FLobbyDataSteam>& LobbyData)
	{
		// Drop what was filed against this user.
		if (TSet<TSharedRef<FLobbyDataSteam>>* Lobbies = ActiveLobbies.Find(LocalAccountId))
		{
			Lobbies->Remove(LobbyData);
		}

		// A kick which the kicked client never acted on would otherwise leave its record behind, keyed by a
		// lobby that no longer exists, and mislabel a later voluntary departure as a kick.
		PendingKicks.Remove(LobbyData->GetLobbyIdHandle());

		// If presence pointed at this lobby, it points at nothing now.
		auto IsPresenceLobbyResult = IsPresenceLobby({ .LocalAccountId = LocalAccountId, .LobbyId = LobbyData->GetLobbyIdHandle() });
		if (IsPresenceLobbyResult.IsOk())
		{
			if (IsPresenceLobbyResult.GetOkValue().bIsPresenceLobby)
			{
				PresenceLobbiesUserMap.Remove(LocalAccountId);
			}
		}
	}

	FLobbiesSteam::FLobbiesSteam(UE::Online::FOnlineServicesCommon& InServices)
		: TOnlineComponentSteam(InServices)
		, OnLobbyDataUpdateCallback(this, &FLobbiesSteam::OnLobbyDataUpdate)
		, OnLobbyChatUpdateCallback(this, &FLobbiesSteam::OnLobbyChatUpdate)
		, OnLobbyChatMessageCallback(this, &FLobbiesSteam::OnLobbyChatMessage)
		, OnLobbyInviteCallback(this, &FLobbiesSteam::OnLobbyInvite)
		, OnGameLobbyJoinRequestedCallback(this, &FLobbiesSteam::OnGameLobbyJoinRequested)
	{

	}

	void FLobbiesSteam::Initialize()
	{
		Super::Initialize();

		auto Matchmaking = Steam::GetSteamInterface<ISteamMatchmaking>();
		check(Matchmaking);

		LobbyPrerequisites = MakeShared<FLobbyPrerequisitesSteam>(Matchmaking, &GetCallDispatcher(), SchemaRegistry);
		LobbyDataRegistry = MakeShared<FLobbyDataRegistrySteam>(LobbyPrerequisites.ToSharedRef());

		// The socket layer knows where this process listens but not which lobby it owns, so it says where
		// it listens and the binding happens here.
		OnListenAddressChangedHandle = Steam::FSocketSubsystemSteam::OnListenAddressChanged().AddRaw(this, &FLobbiesSteam::OnListenAddressChanged);

		// Steam launches the game with "+connect_lobby <id>" when a user accepts an invitation while the
		// game is not running. Nothing can be done with it until somebody is signed in, so it waits.
		FString CommandLineLobbyId;
		if (FParse::Value(FCommandLine::Get(), TEXT("connect_lobby"), CommandLineLobbyId))
		{
			uint64 LobbyId { 0 };
			LexFromString(LobbyId, *CommandLineLobbyId);

			PendingCommandLineLobby = CSteamID(LobbyId);
		}
	}

	void FLobbiesSteam::Tick(const float DeltaSeconds)
	{
		Super::Tick(DeltaSeconds);

		if (!PendingCommandLineLobby.IsValid())
		{
			return;
		}

		const auto Auth = Services.GetAuthInterface();
		const auto LocalAccountId = GetLocalAccountId();

		if (!Auth.IsValid() || !LocalAccountId.IsValid() || !Auth->IsLoggedIn(LocalAccountId))
		{
			return;
		}

		const CSteamID LobbySteamId = PendingCommandLineLobby;
		PendingCommandLineLobby = k_steamIDNil;

		UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::Tick] Answering the lobby the game was launched for: User [%s], Lobby [%s]"),
			*ToLogString(LocalAccountId), *ToLogString(LobbySteamId));

		RequestUiJoin(LobbySteamId, LocalAccountId, UE::Online::EUILobbyJoinRequestedSource::FromInvitation);
	}

	void FLobbiesSteam::PreShutdown()
	{
		Super::PreShutdown();

		// The handlers below dereference the registry, so they must stop firing before it is released.
		Steam::FSocketSubsystemSteam::OnListenAddressChanged().Remove(OnListenAddressChangedHandle);
		OnListenAddressChangedHandle.Reset();

		OnLobbyDataUpdateCallback.Unregister();
		OnLobbyChatUpdateCallback.Unregister();
		OnLobbyChatMessageCallback.Unregister();
		OnLobbyInviteCallback.Unregister();
		OnGameLobbyJoinRequestedCallback.Unregister();

		LobbyDataRegistry.Reset();
		LobbyPrerequisites.Reset();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FCreateLobby> FLobbiesSteam::CreateLobby(UE::Online::FCreateLobby::Params&& InParams)
	{
		const auto DestroyLobbyDuringCreate = [this](
			UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp,
			UE::Online::FAccountId        LocalAccountId,
			const CSteamID&               LobbySteamId,
			UE::Online::FOnlineError      ErrorResult) -> TFuture<void>
		{
			FLobbiesDestroyLobbyImpl::Params DestroyLobbyParams;
			DestroyLobbyParams.LobbySteamId = LobbySteamId;
			DestroyLobbyParams.LocalAccountId = LocalAccountId;

			TPromise<void> Promise;
			auto Future = Promise.GetFuture();

			DestroyLobbyImpl(MoveTemp(DestroyLobbyParams))
			.Next(
			[
				InAsyncOp = InAsyncOp.AsShared(),
				LocalAccountId,
				LobbySteamId,
				ErrorResult = MoveTemp(ErrorResult),
				Promise = MoveTemp(Promise)
			](UE::Online::TDefaultErrorResult<FLobbiesDestroyLobbyImpl>&& Result) mutable
			{
				if (Result.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::CreateLobby] DestroyLobbyImpl Failed: User [%s], Lobby [%s], Result [%s]"),
						*ToLogString(LocalAccountId), *ToLogString(LobbySteamId), *Result.GetErrorValue().GetLogString());
				}

				// Hand the failure to the operation and stop.
				InAsyncOp->SetError(MoveTemp(ErrorResult));
				Promise.EmplaceValue();
			});

			return Future;
		};

		const auto Op = GetOp<UE::Online::FCreateLobby>(MoveTemp(InParams));

		// Step 1: Check prerequisites
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (const auto Auth = Services.GetAuthInterface())
			{
				if (!Auth->IsLoggedIn(Params.LocalAccountId))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::CreateLobby] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
					return;
				}

				if (!SchemaRegistry->GetDefinition(Params.SchemaId).IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::CreateLobby] Failed: Unknown lobby schema [%s]"), *Params.SchemaId.ToString().ToLower());
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				if (Params.bPresenceEnabled)
				{
					if (const auto PresenceLobbyResult = GetPresenceLobby(UE::Online::FGetPresenceLobby::Params { Params.LocalAccountId }); PresenceLobbyResult.IsOk())
					{
						UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::CreateLobby] Failed: bPresenceEnabled was set to true, but there is already a presence Lobby"));
						InAsyncOp.SetError(UE::Online::Errors::InvalidState());
					}
				}
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::CreateLobby] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}
		})
		// Step 2: Call create a lobby
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			// Steam fixes the size and the type of a lobby when it is created; the type is written again
			// with the rest of the attributes below, but the size can only be given here.
			Steam::Wrappers::FSteamCreateLobby::Params CreateLobbyRequest;
			CreateLobbyRequest.LobbyType = TranslateJoinPolicy(Params.JoinPolicy);
			CreateLobbyRequest.MaxMembers = Params.MaxMembers > 0 ? Params.MaxMembers : CreateLobbyRequest.MaxMembers;

			return SteamCall<Steam::Wrappers::FSteamCreateLobby>(MoveTemp(CreateLobbyRequest));
		})
		// Step 3: Attach the lobby id to the operation data. A failed call fails the operation.
		.Then(Steam::Unwrap<Steam::Wrappers::FSteamCreateLobby>(TEXT("FLobbiesSteam::CreateLobby"),
			[this](UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp, Steam::Wrappers::FSteamCreateLobby::Result&& Result)
		{
			InAsyncOp.Data.Set<CSteamID>(LobbySteamIdKey, Result.LobbyId);
		}))
		// Step 4: Create the lobby details object from the lobby id.
		.Then([this, DestroyLobbyDuringCreate](UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();
			const CSteamID& LobbySteamId = GetOpDataChecked<CSteamID>(InAsyncOp, LobbySteamIdKey);

			// Ask for a details handle before anything else can be done with the lobby.
			auto Result = FLobbyDetailsSteam::CreateFromLobbyId(LobbyPrerequisites.ToSharedRef(), Params.LocalAccountId, LobbySteamId);
			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::CreateLobby] Failed: FLobbyDetailsSteam::CreateFromLobbyId Failed: User [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *Result.GetErrorValue().GetLogString());

				return DestroyLobbyDuringCreate(InAsyncOp, Params.LocalAccountId, LobbySteamId, MoveTemp(Result.GetErrorValue()));
			}

			InAsyncOp.Data.Set<TSharedRef<FLobbyDetailsSteam>>(LobbyDetailsKey, Result.GetOkValue());
			return MakeFulfilledPromise<void>().GetFuture();
		})
		// Step 5: Create the lobby data object from the lobby details.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto LobbyDetails = GetOpDataChecked<TSharedRef<FLobbyDetailsSteam>>(InAsyncOp, LobbyDetailsKey);
			return LobbyDataRegistry->FindOrCreateFromLobbyDetails(Params.LocalAccountId, LobbyDetails);
		})
		// Step 6: Handle errors and store the lobby data on the async op properties.
		.Then([this, DestroyLobbyDuringCreate](UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp, UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDataSteam>>&& Result)
		{
			const auto& Params = InAsyncOp.GetParams();
			const CSteamID& LobbySteamId = GetOpDataChecked<CSteamID>(InAsyncOp, LobbySteamIdKey);

			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::CreateLobby] FLobbyDataRegistrySteam::FindOrCreateFromLobbyDetails Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbySteamId), *Result.GetErrorValue().GetLogString());

				return DestroyLobbyDuringCreate(InAsyncOp, Params.LocalAccountId, LobbySteamId, MoveTemp(Result.GetErrorValue()));
			}

			// Park the lobby on the operation for the steps that follow.
			InAsyncOp.Data.Set<TSharedRef<FLobbyDataSteam>>(LobbyDataKey, MoveTemp(Result.GetOkValue()));
			return MakeFulfilledPromise<void>().GetFuture();
		})
		// Step 7: Set lobby and creator attributes, change to user lobby privacy setting.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			UE::Online::FLobbyClientDataPrepareClientChanges::Params PrepareParams;
			PrepareParams.LocalAccountId = Params.LocalAccountId;
			PrepareParams.ClientChanges.Attributes = { Params.Attributes, { } };
			PrepareParams.ClientChanges.MemberAttributes = { Params.UserAttributes, { } };
			PrepareParams.ClientChanges.JoinPolicy = Params.JoinPolicy;
			PrepareParams.ClientChanges.LobbySchema = Params.SchemaId;
			PrepareParams.ClientChanges.LocalName = Params.LocalName;

			auto PrepareResult = LobbyData->GetLobbyClientData()->PrepareClientChanges(MoveTemp(PrepareParams));
			if (PrepareResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::CreateLobby] PrepareClientChanges Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *PrepareResult.GetErrorValue().GetLogString());
				InAsyncOp.SetError(MoveTemp(PrepareResult.GetErrorValue()));

				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(FLobbiesModifyLobbyDataImpl::Result{}).GetFuture();
			}

			FLobbiesModifyLobbyDataImpl::Params ModifyLobbyDataParams;
			ModifyLobbyDataParams.LobbyData = LobbyData;
			ModifyLobbyDataParams.LocalAccountId = Params.LocalAccountId;
			ModifyLobbyDataParams.ServiceChanges = MoveTemp(PrepareResult.GetOkValue().ServiceChanges);

			return ModifyLobbyDataImpl(MoveTemp(ModifyLobbyDataParams));
		})
		// Step 8: Handle result
		.Then([this, DestroyLobbyDuringCreate](UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp, UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>&& Result) mutable
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::CreateLobby] ModifyLobbyDataImpl Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				return DestroyLobbyDuringCreate(InAsyncOp, Params.LocalAccountId, LobbyData->GetLobbySteamId(), MoveTemp(Result.GetErrorValue()));
			}

			return MakeFulfilledPromise<void>().GetFuture();
		})
		// Step 9: Add the lobby to the active list, apply changes to the cached lobby object, and signal notifications.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FCreateLobby>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			// Mark the lobby active.
			AddActiveLobby(Params.LocalAccountId, Params.bPresenceEnabled, LobbyData);

			// Publish what a lobby browser cannot read out of Steam on its own.
			LobbyData->GetLobbyDetails()->PublishOwnedMetadata();

			// Write the change through and tell whoever is listening.
			LobbyData->GetLobbyClientData()->CommitClientChanges({ &LobbyEvents });

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::CreateLobby] Succeeded: User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(UE::Online::FCreateLobby::Result { .Lobby = LobbyData->GetLobbyClientData()->GetPublicDataPtr() });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FFindLobbies> FLobbiesSteam::FindLobbies(UE::Online::FFindLobbies::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FFindLobbies>(MoveTemp(InParams));

		// Step 1: Check prerequisites
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FFindLobbies>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (const auto Auth = Services.GetAuthInterface())
			{
				if (!Auth->IsLoggedIn(Params.LocalAccountId))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::FindLobbies] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
					return;
				}

				// Invalidate previous search results
				ActiveSearchResults.Remove(Params.LocalAccountId);
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::FindLobbies] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}
		})
		// Step 2: Start Operation
		.Then([this](const UE::Online::TOnlineAsyncOp<UE::Online::FFindLobbies>& InAsyncOp) {
			return FLobbySearchSteam::Create(LobbyPrerequisites.ToSharedRef(), LobbyDataRegistry.ToSharedRef(), InAsyncOp.GetParams());
		})
		// Step 3: Handle the result
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FFindLobbies>& InAsyncOp, UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbySearchSteam>> Result) {
			if (Result.IsError())
			{
				auto& ErrorValue = Result.GetErrorValue();
				const auto& Params = InAsyncOp.GetParams();

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::FindLobbies] FLobbySearchSteam::Create Failed: User [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ErrorValue.GetLogString());

				InAsyncOp.SetError(MoveTemp(ErrorValue));
			}
			else
			{
				InAsyncOp.Data.Set<TSharedRef<FLobbySearchSteam>>(LobbySearchKey, Result.GetOkValue());
			}
		})
		// Step 4: Add the lobby to the active search list and signal notifications.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FFindLobbies>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbySearch = GetOpDataChecked<TSharedRef<FLobbySearchSteam>>(InAsyncOp, LobbySearchKey);

			ActiveSearchResults.Add(Params.LocalAccountId, LobbySearch);

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::FindLobbies] Succeeded: User [%s]"), *ToLogString(Params.LocalAccountId));

			InAsyncOp.SetResult(UE::Online::FFindLobbies::Result { .Lobbies = LobbySearch->GetLobbyResults() });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FJoinLobby> FLobbiesSteam::JoinLobby(UE::Online::FJoinLobby::Params&& InParams)
	{
		// One place to fail from, so the steps below read straight through.
		auto LeaveLobbyDuringJoin = [this](
			UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp,
			const UE::Online::FAccountId LocalAccountId,
			const TSharedPtr<FLobbyDataSteam>& LobbyData,
			UE::Online::FOnlineError ErrorResult) -> TFuture<void>
		{
			// Attributes did not take, so the lobby is left rather than kept half configured.
			FLobbiesLeaveLobbyImpl::Params LeaveLobbyParams;
			LeaveLobbyParams.LobbyData = LobbyData;
			LeaveLobbyParams.LocalAccountId = LocalAccountId;

			TPromise<void> Promise;
			auto Future = Promise.GetFuture();

			LeaveLobbyImpl(MoveTemp(LeaveLobbyParams))
			.Next(
			[
				LocalAccountId = LocalAccountId,
				LobbyData,
				InAsyncOp = InAsyncOp.AsShared(),
				ErrorResult = MoveTemp(ErrorResult),
				Promise = MoveTemp(Promise)
			](UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>&& Result) mutable
			{
				if (Result.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] LeaveLobbyImpl Failed: User [%s], Lobby [%s], Result [%s]"),
						*ToLogString(LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());
				}

				InAsyncOp->SetError(MoveTemp(ErrorResult));
				Promise.EmplaceValue();
			});

			return Future;
		};

		const auto Op = GetOp<UE::Online::FJoinLobby>(MoveTemp(InParams));

		// Step 1: Check prerequisites
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (const auto Auth = Services.GetAuthInterface())
			{
				if (!Auth->IsLoggedIn(Params.LocalAccountId))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
					return;
				}

				if (!Params.LobbyId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] Failed: Lobby id is invalid."));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				if (Params.bPresenceEnabled)
				{
					if (const auto PresenceLobbyResult = GetPresenceLobby(UE::Online::FGetPresenceLobby::Params { Params.LocalAccountId }); PresenceLobbyResult.IsOk())
					{
						UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] Failed: bPresenceEnabled was set to true, but there is already a presence Lobby"));
						InAsyncOp.SetError(UE::Online::Errors::InvalidState());
						return;
					}
				}

				const auto LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
				if (!LobbyData)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] Failed: Unable to find lobby data. LobbyId [%s]"), *ToLogString(Params.LobbyId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				const auto LobbyDetails = LobbyData->GetUserLobbyDetails(Params.LocalAccountId);
				if (!LobbyDetails)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] Failed: The lobby was not discovered by this user. User [%s], Lobby [%s]"),
						*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));
					InAsyncOp.SetError(UE::Online::Errors::InvalidState());
					return;
				}

				InAsyncOp.Data.Set<TSharedRef<FLobbyDataSteam>>(LobbyDataKey, LobbyData.ToSharedRef());
				InAsyncOp.Data.Set<TSharedRef<FLobbyDetailsSteam>>(LobbyDetailsKey, LobbyDetails.ToSharedRef());
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}
		})
		// Step 2. Join the lobby.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp)
		{
			const auto& LobbyDetails = GetOpDataChecked<TSharedRef<FLobbyDetailsSteam>>(InAsyncOp, LobbyDetailsKey);
			Steam::Wrappers::FSteamJoinLobby::Params JoinLobbyRequest { LobbyDetails->GetLobbySteamId() };

			return SteamCall<Steam::Wrappers::FSteamJoinLobby>(MoveTemp(JoinLobbyRequest));
		})
		// Step 3. Handle join result.
		.Then([this, LeaveLobbyDuringJoin](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp, Steam::TSteamResult<Steam::Wrappers::FSteamJoinLobby>&& Result)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] Steam->JoinLobby Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				// The lobby was entered by Steam before it refused, so the membership has to be given back.
				return LeaveLobbyDuringJoin(InAsyncOp, Params.LocalAccountId, LobbyData, MoveTemp(Result.GetErrorValue()));
			}

			return MakeFulfilledPromise<void>().GetFuture();
		})
		// Step 4. Promote a lobby details source to active.
		.Then([this, LeaveLobbyDuringJoin](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			// Ask for a details handle before anything else can be done with the lobby.
			UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>> Result =
				FLobbyDetailsSteam::CreateFromLobbyId(LobbyPrerequisites.ToSharedRef(), Params.LocalAccountId, LobbyData->GetLobbySteamId());

			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] FLobbyDetailsSteam::CreateFromLobbyId Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				return LeaveLobbyDuringJoin(InAsyncOp, Params.LocalAccountId, LobbyData, MoveTemp(Result.GetErrorValue()));
			}

			// Carry the details forward; the next step registers them.
			InAsyncOp.Data.Set<TSharedRef<FLobbyDetailsSteam>>(LobbyDetailsKey, Result.GetOkValue());
			return MakeFulfilledPromise<void>().GetFuture();
		})
		// Step 5: Create the lobby data object from the lobby details.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp)
		{
			const auto LobbyDetails = GetOpDataChecked<TSharedRef<FLobbyDetailsSteam>>(InAsyncOp, LobbyDetailsKey);
			return LobbyDataRegistry->FindOrCreateFromLobbyDetails(InAsyncOp.GetParams().LocalAccountId, LobbyDetails);
		})
		// Step 6: Handle possible errors from FindOrCreateFromLobbyDetails.
		.Then([this, LeaveLobbyDuringJoin](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp, UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDataSteam>>&& Result)
		{
			const auto& Params = InAsyncOp.GetParams();

			if (Result.IsError())
			{
				const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] FLobbyDataRegistrySteam::FindOrCreateFromLobbyDetails Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				return LeaveLobbyDuringJoin(InAsyncOp, Params.LocalAccountId, LobbyData, MoveTemp(Result.GetErrorValue()));
			}

			return MakeFulfilledPromise<void>().GetFuture();
		})
		// Step 7: Update lobby data
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp)
		{
			const auto LobbyDetails = GetOpDataChecked<TSharedRef<FLobbyDetailsSteam>>(InAsyncOp, LobbyDetailsKey);
			return LobbyDetails->GetLobbySnapshot(true);
		})
		// Step 8: Handle errors
		.Then([this, LeaveLobbyDuringJoin](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp, UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyServiceSnapshot>&& Result)
		{
			auto LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);
			const auto LobbyDetails = GetOpDataChecked<TSharedRef<FLobbyDetailsSteam>>(InAsyncOp, LobbyDetailsKey);
			const auto& Params = InAsyncOp.GetParams();

			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] GetLobbySnapshot Failed. Lobby [%s], Result [%s]"),
					*ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				return LeaveLobbyDuringJoin(InAsyncOp, Params.LocalAccountId, LobbyData, MoveTemp(Result.GetErrorValue()));
			}

			auto LobbySnapshot = MoveTemp(Result.GetOkValue());

			// Read what the members published and fold it into the lobby.
			TMap<UE::Online::FAccountId, UE::Online::FLobbyMemberServiceSnapshot> MemberSnapshots;
			for (auto MemberAccountId : LobbySnapshot.Members)
			{
				auto LobbyMemberSnapshotResult = LobbyDetails->GetLobbyMemberSnapshot(MemberAccountId);
				if (LobbyMemberSnapshotResult.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] FLobbyDataSteam::GetLobbyMemberSnapshot Failed: Lobby[%s], User[%s], Result[%s]"),
						*ToLogString(LobbyData->GetLobbySteamId()), *ToLogString(MemberAccountId), *LobbyMemberSnapshotResult.GetErrorValue().GetLogString());

					return LeaveLobbyDuringJoin(InAsyncOp, Params.LocalAccountId, LobbyData, MoveTemp(LobbyMemberSnapshotResult.GetErrorValue()));
				}

				MemberSnapshots.Emplace(MemberAccountId, MoveTemp(LobbyMemberSnapshotResult.GetOkValue()));
			}

			UE::Online::TOnlineResult<UE::Online::FLobbyClientDataPrepareServiceSnapshot> PrepareServiceLobbySnapshotResult =
				LobbyData->GetLobbyClientData()->PrepareServiceSnapshot({ .LobbySnapshot = MoveTemp(LobbySnapshot), .LobbyMemberSnapshots = MoveTemp(MemberSnapshots) });

			if (PrepareServiceLobbySnapshotResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] PrepareServiceSnapshot Failed. Lobby [%s], Result [%s]"),
					*ToLogString(LobbyData->GetLobbySteamId()), *PrepareServiceLobbySnapshotResult.GetErrorValue().GetLogString());

				return LeaveLobbyDuringJoin(InAsyncOp, Params.LocalAccountId, LobbyData, MoveTemp(PrepareServiceLobbySnapshotResult.GetErrorValue()));
			}

			// Fold the change in, then notify.
			auto [LeavingLocalMembers] = LobbyData->GetLobbyClientData()->CommitServiceSnapshot({ &LobbyEvents });

			// Anyone who is no longer a member stops being one here.
			for (const auto LeavingMember : LeavingLocalMembers)
			{
				RemoveActiveLobby(LeavingMember, LobbyData);
			}

			return MakeFulfilledPromise<void>().GetFuture();
		})
		// Step 9. Set member attributes.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			UE::Online::FLobbyClientDataPrepareClientChanges::Params PrepareParams;
			PrepareParams.LocalAccountId = Params.LocalAccountId;
			PrepareParams.ClientChanges.LocalName = Params.LocalName;
			PrepareParams.ClientChanges.MemberAttributes = { Params.UserAttributes, {} };

			auto PrepareResult = LobbyData->GetLobbyClientData()->PrepareClientChanges(MoveTemp(PrepareParams));
			if (PrepareResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] PrepareClientChanges Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *PrepareResult.GetErrorValue().GetLogString());

				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(PrepareResult.GetErrorValue()).GetFuture();
			}

			FLobbiesModifyLobbyDataImpl::Params ModifyLobbyDataParams;
			ModifyLobbyDataParams.LobbyData = LobbyData;
			ModifyLobbyDataParams.LocalAccountId = Params.LocalAccountId;
			ModifyLobbyDataParams.ServiceChanges = MoveTemp(PrepareResult.GetOkValue().ServiceChanges);

			return ModifyLobbyDataImpl(MoveTemp(ModifyLobbyDataParams));
		})
		// Step 10. Handle result.
		.Then([this, LeaveLobbyDuringJoin](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp, UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>&& Result)
		{
			if (Result.IsError())
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::JoinLobby] ModifyLobbyMemberDataImpl Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				return LeaveLobbyDuringJoin(InAsyncOp, Params.LocalAccountId, LobbyData, MoveTemp(Result.GetErrorValue()));
			}

			return MakeFulfilledPromise<void>().GetFuture();
		})
		// Step 11. Bookkeeping and notifications.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FJoinLobby>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			// An invitation that has been acted on is spent.
			if (const TSharedPtr<FLobbyInviteDataSteam> InviteData = GetActiveInvite(Params.LocalAccountId, Params.LobbyId))
			{
				RemoveActiveInvite(InviteData.ToSharedRef());
			}

			// Mark the lobby active.
			AddActiveLobby(Params.LocalAccountId, Params.bPresenceEnabled, LobbyData);

			// Joining changed the member count, which only the owner of the lobby can republish.
			LobbyData->GetLobbyDetails()->PublishOwnedMetadata();

			// Write the change through and tell whoever is listening.
			LobbyData->GetLobbyClientData()->CommitClientChanges({ &LobbyEvents });

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::JoinLobby] Succeeded: User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(UE::Online::FJoinLobby::Result { .Lobby = LobbyData->GetLobbyClientData()->GetPublicDataPtr() });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FLeaveLobby> FLobbiesSteam::LeaveLobby(UE::Online::FLeaveLobby::Params&& InParams)
	{
		auto Op = GetOp<UE::Online::FLeaveLobby>(MoveTemp(InParams));

		// Step 1: Check prerequisites
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FLeaveLobby>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (const auto Auth = Services.GetAuthInterface())
			{
				if (!Auth->IsLoggedIn(Params.LocalAccountId))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobby] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
					return;
				}

				if (!Params.LobbyId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobby] Failed: Lobby id is invalid."));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				const auto LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
				if (!LobbyData)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobby] Failed: Unable to find lobby data. LobbyId [%s]"), *ToLogString(Params.LobbyId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				InAsyncOp.Data.Set<TSharedRef<FLobbyDataSteam>>(LobbyDataKey, LobbyData.ToSharedRef());
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobby] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}
		})
		// Step 2: Leave the Lobby.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FLeaveLobby>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			UE::Online::FLobbyClientDataPrepareClientChanges::Params PrepareParams;
			PrepareParams.LocalAccountId = Params.LocalAccountId;
			PrepareParams.ClientChanges.LocalUserLeaveReason = UE::Online::ELobbyMemberLeaveReason::Left;

			auto PrepareResult = LobbyData->GetLobbyClientData()->PrepareClientChanges(MoveTemp(PrepareParams));
			if (PrepareResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobby] PrepareClientChanges Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *PrepareResult.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(PrepareResult.GetErrorValue()));
				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>>(FLobbiesLeaveLobbyImpl::Result{}).GetFuture();
			}

			FLobbiesLeaveLobbyImpl::Params LeaveParams;
			LeaveParams.LobbyData = LobbyData;
			LeaveParams.LocalAccountId = Params.LocalAccountId;

			return LeaveLobbyImpl(MoveTemp(LeaveParams));
		})
		// Step 3. Handle leave result.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FLeaveLobby>& InAsyncOp, UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>&& Result)
		{
			if (Result.IsError())
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobby] LeaveLobbyImpl Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());
			}
		})
		// Step 4. Bookkeeping and notifications.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FLeaveLobby>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			// This user is no longer in the lobby.
			// The data itself lives until the last reference to it goes.
			RemoveActiveLobby(Params.LocalAccountId, LobbyData);
			PendingKicks.Remove(LobbyData->GetLobbyIdHandle());

			// Write the change through and tell whoever is listening.
			LobbyData->GetLobbyClientData()->CommitClientChanges({ &LobbyEvents });

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::LeaveLobby] Succeeded: User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(UE::Online::FLeaveLobby::Result { });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FInviteLobbyMember> FLobbiesSteam::InviteLobbyMember(UE::Online::FInviteLobbyMember::Params&& InParams)
	{
		auto Op = GetOp<UE::Online::FInviteLobbyMember>(MoveTemp(InParams));

		// Step 1: Check prerequisites
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FInviteLobbyMember>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (const auto Auth = Services.GetAuthInterface())
			{
				if (!Auth->IsLoggedIn(Params.LocalAccountId))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::InviteLobbyMember] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
					return;
				}

				if (!Params.LobbyId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::InviteLobbyMember] Failed: Lobby id is invalid."));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				const auto LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
				if (!LobbyData)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::InviteLobbyMember] Failed: Unable to find lobby data. LobbyId [%s]"), *ToLogString(Params.LobbyId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				InAsyncOp.Data.Set<TSharedRef<FLobbyDataSteam>>(LobbyDataKey, LobbyData.ToSharedRef());
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::InviteLobbyMember] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}
		})
		// Step 2: Send invite.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FInviteLobbyMember>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			FLobbiesInviteLobbyMemberImpl::Params InviteParams;
			InviteParams.LobbyData = LobbyData;
			InviteParams.LocalAccountId = Params.LocalAccountId;
			InviteParams.TargetAccountId = Params.TargetAccountId;

			return InviteLobbyMemberImpl(MoveTemp(InviteParams));
		})
		// Step 3: Handle invite result.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FInviteLobbyMember>& InAsyncOp, UE::Online::TDefaultErrorResult<FLobbiesInviteLobbyMemberImpl>&& Result)
		{
			if (Result.IsError())
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::InviteLobbyMember] InviteLobbyMemberImpl Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(Result.GetErrorValue()));
			}
		})
		// Step 4: Notifications
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FInviteLobbyMember>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::InviteLobbyMember] Succeeded: User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(UE::Online::FInviteLobbyMember::Result { });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	void FLobbiesSteam::OnListenAddressChanged(const Steam::FSteamNetAddress& /*ListenAddress*/)
	{
		// Which lobby the address belongs on is a question for the moment it is asked, so the lobby is
		// looked up here rather than remembered.
		const auto LocalAccountId = GetLocalAccountId();
		if (!LocalAccountId.IsValid())
		{
			return;
		}

		// Every lobby this user is in, not only the one presence points at: a lobby made before the server
		// came up has no announcement of its own left to wait for, and presence is about what friends see
		// rather than about which lobby a server belongs to. A lobby somebody else owns is turned away by
		// the call itself, which is the only thing that decides.
		if (const auto Lobbies = ActiveLobbies.Find(LocalAccountId))
		{
			for (const auto& LobbyData : *Lobbies)
			{
				BindGameServerToOwnedLobby(LobbyData);
			}
		}
	}

	void FLobbiesSteam::BindGameServerToOwnedLobby(const TSharedPtr<FLobbyDataSteam>& LobbyData)
	{
		if (!LobbyData.IsValid())
		{
			return;
		}

		const auto& ListenAddress = Steam::FSocketSubsystemSteam::GetListenAddress();
		if (!ListenAddress.IsSet())
		{
			// This process hosts nothing, so there is no server to bind.
			return;
		}

		auto Matchmaking = Steam::GetSteamInterface<ISteamMatchmaking>();
		const auto LocalAccountId = GetLocalAccountId();

		if (Matchmaking == nullptr || !LocalAccountId.IsValid())
		{
			return;
		}

		// Only the owner of a lobby may bind a server to it; for everybody else the call is ignored.
		if (Matchmaking->GetLobbyOwner(LobbyData->GetLobbySteamId()) != GetSteamUserId(LocalAccountId))
		{
			return;
		}

		uint32 ServerIp { 0 };
		ListenAddress->GetIp(ServerIp);

		Steam::Wrappers::FSteamSetLobbyGameServer::Params BindParams
		{
			.LobbyId = LobbyData->GetLobbySteamId(),
			.GameServer =
			{
				.ServerIp = ServerIp,
				.ServerPort = static_cast<uint16>(ListenAddress->GetPort()),
				.ServerId = ListenAddress->GetSteamID()
			}
		};

		UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::BindGameServerToOwnedLobby] Binding the game server to the lobby: Lobby [%s], Server [%s]"),
			*ToLogString(LobbyData->GetLobbySteamId()), *ListenAddress->ToString(true));

		if (const auto BindResult = SteamCallSync<Steam::Wrappers::FSteamSetLobbyGameServer>(BindParams); BindResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::BindGameServerToOwnedLobby] Steam->SetLobbyGameServer Failed: Lobby [%s], Result [%s]"),
				*ToLogString(LobbyData->GetLobbySteamId()), *BindResult.GetErrorValue().GetLogString());
		}
	}

	TOptional<Steam::Wrappers::FSteamLobbyGameServer> FLobbiesSteam::GetLobbyGameServer(const UE::Online::FLobbyId LobbyId) const
	{
		const auto LobbyData = LobbyDataRegistry.IsValid() ? LobbyDataRegistry->Find(LobbyId) : nullptr;
		if (!LobbyData.IsValid())
		{
			return { };
		}

		const auto GameServerResult = SteamCallSync<Steam::Wrappers::FSteamGetLobbyGameServer>({ .LobbyId = LobbyData->GetLobbySteamId() });
		if (GameServerResult.IsError())
		{
			return { };
		}

		return GameServerResult.GetOkValue().GameServer;
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FDeclineLobbyInvitation> FLobbiesSteam::DeclineLobbyInvitation(UE::Online::FDeclineLobbyInvitation::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FDeclineLobbyInvitation>(MoveTemp(InParams));

		// Steam has no call behind declining: an invitation is a notification, and dropping it is
		// bookkeeping the invited client does for itself.
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FDeclineLobbyInvitation>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			const auto InviteData = GetActiveInvite(Params.LocalAccountId, Params.LobbyId);
			if (!InviteData)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::DeclineLobbyInvitation] Failed: No invitation to decline. User [%s], Lobby [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(Params.LobbyId));

				InAsyncOp.SetError(UE::Online::Errors::NotFound());
				return;
			}

			RemoveActiveInvite(InviteData.ToSharedRef());

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::DeclineLobbyInvitation] Succeeded: User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(Params.LobbyId));

			InAsyncOp.SetResult(UE::Online::FDeclineLobbyInvitation::Result { });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FKickLobbyMember> FLobbiesSteam::KickLobbyMember(UE::Online::FKickLobbyMember::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FKickLobbyMember>(MoveTemp(InParams));

		// Step 1: Check prerequisites
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FKickLobbyMember>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			const auto Auth = Services.GetAuthInterface();
			if (!Auth.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::KickLobbyMember] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
				return;
			}

			if (!Auth->IsLoggedIn(Params.LocalAccountId))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::KickLobbyMember] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
				InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
				return;
			}

			const auto LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
			if (!LobbyData)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::KickLobbyMember] Failed: Unable to find lobby data. LobbyId [%s]"), *ToLogString(Params.LobbyId));
				InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
				return;
			}

			if (Params.TargetAccountId == Params.LocalAccountId)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::KickLobbyMember] Failed: A user cannot remove themselves; leave the lobby instead. User [%s]"), *ToLogString(Params.LocalAccountId));
				InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
				return;
			}

			InAsyncOp.Data.Set<TSharedRef<FLobbyDataSteam>>(LobbyDataKey, LobbyData.ToSharedRef());
		})
		// Step 2: Ask the member to leave.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FKickLobbyMember>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			FLobbiesKickLobbyMemberImpl::Params KickParams;
			KickParams.LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);
			KickParams.LocalAccountId = Params.LocalAccountId;
			KickParams.TargetAccountId = Params.TargetAccountId;

			return KickLobbyMemberImpl(MoveTemp(KickParams));
		})
		// Step 3: Handle result.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FKickLobbyMember>& InAsyncOp, UE::Online::TDefaultErrorResult<FLobbiesKickLobbyMemberImpl>&& Result)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::KickLobbyMember] KickLobbyMemberImpl Failed: User [%s], Target [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(Params.TargetAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(Result.GetErrorValue()));
				return;
			}

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::KickLobbyMember] Succeeded: User [%s], Target [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(Params.TargetAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(UE::Online::FKickLobbyMember::Result { });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	TFuture<UE::Online::TDefaultErrorResult<FLobbiesKickLobbyMemberImpl>> FLobbiesSteam::KickLobbyMemberImpl(FLobbiesKickLobbyMemberImpl::Params&& Params)
	{
		using FKickResult = UE::Online::TDefaultErrorResult<FLobbiesKickLobbyMemberImpl>;

		if (!Params.LobbyData.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::KickLobbyMemberImpl] Failed: No lobby data provided."));
			return MakeFulfilledPromise<FKickResult>(UE::Online::Errors::InvalidParams()).GetFuture();
		}

		// Steam lets nobody remove anybody from a lobby, and lets nobody write another member's data
		// either. The owner therefore asks the member to leave over the lobby chat channel, and the
		// member acts on it; see OnLobbyChatMessage.
		if (Params.LobbyData->GetLobbyClientData()->GetPublicData().OwnerAccountId != Params.LocalAccountId)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::KickLobbyMemberImpl] Failed: User is not the lobby owner. User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()));

			return MakeFulfilledPromise<FKickResult>(UE::Online::Errors::AccessDenied()).GetFuture();
		}

		// The target comes from the game with nothing validating it on the way, so a member who left while
		// the menu was open would turn a mis-click into a check() failure.
		auto TargetSteamIdResult = ResolveSteamUser(Params.LocalAccountId, Params.TargetAccountId, TEXT("FLobbiesSteam::KickLobbyMemberImpl"));
		if (TargetSteamIdResult.IsError())
		{
			return MakeFulfilledPromise<FKickResult>(MoveTemp(TargetSteamIdResult.GetErrorValue())).GetFuture();
		}

		const CSteamID TargetSteamId = TargetSteamIdResult.GetOkValue();

		Steam::Wrappers::FSteamSendLobbyControlMessage::Params MessageParams;
		MessageParams.LobbyId = Params.LobbyData->GetLobbySteamId();
		MessageParams.Message.Command = Steam::ESteamLobbyControlCommand::Kick;
		MessageParams.Message.TargetSteamId = TargetSteamId.ConvertToUint64();

		Steam::TSteamResult<Steam::Wrappers::FSteamSendLobbyControlMessage> SendResult =
			SteamCallSync<Steam::Wrappers::FSteamSendLobbyControlMessage>(MessageParams);

		if (SendResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::KickLobbyMemberImpl] Steam->SendLobbyChatMsg Failed: User [%s], Lobby [%s], Result [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()), *SendResult.GetErrorValue().GetLogString());

			return MakeFulfilledPromise<FKickResult>(MoveTemp(SendResult.GetErrorValue())).GetFuture();
		}

		// Steam will report the departure as an ordinary leave, so it has to be remembered here for the
		// game to be told what really happened.
		PendingKicks.FindOrAdd(Params.LobbyData->GetLobbyIdHandle()).Add(TargetSteamId);

		return MakeFulfilledPromise<FKickResult>(FLobbiesKickLobbyMemberImpl::Result { }).GetFuture();
	}

	void FLobbiesSteam::HandleLocalUserKicked(const TSharedRef<FLobbyDataSteam>& LobbyData, const UE::Online::FAccountId LocalAccountId)
	{
		UE::Online::FLobbyClientDataPrepareClientChanges::Params PrepareParams;
		PrepareParams.LocalAccountId = LocalAccountId;
		PrepareParams.ClientChanges.LocalUserLeaveReason = UE::Online::ELobbyMemberLeaveReason::Kicked;

		UE::Online::TOnlineResult<UE::Online::FLobbyClientDataPrepareClientChanges> PrepareResult =
			LobbyData->GetLobbyClientData()->PrepareClientChanges(MoveTemp(PrepareParams));

		if (PrepareResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::HandleLocalUserKicked] PrepareClientChanges Failed: User [%s], Lobby [%s], Result [%s]"),
				*ToLogString(LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *PrepareResult.GetErrorValue().GetLogString());
			return;
		}

		FLobbiesLeaveLobbyImpl::Params LeaveParams;
		LeaveParams.LobbyData = LobbyData;
		LeaveParams.LocalAccountId = LocalAccountId;

		LeaveLobbyImpl(MoveTemp(LeaveParams))
		.Next([this, LobbyData, LocalAccountId](UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>&& Result)
		{
			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::HandleLocalUserKicked] LeaveLobbyImpl Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());
			}

			RemoveActiveLobby(LocalAccountId, LobbyData);
			LobbyData->GetLobbyClientData()->CommitClientChanges({ &LobbyEvents });

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::HandleLocalUserKicked] The lobby owner removed this user: User [%s], Lobby [%s]"),
				*ToLogString(LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));
		});
	}

	void FLobbiesSteam::RequestUiJoin(const CSteamID LobbySteamId, const UE::Online::FAccountId LocalAccountId, const UE::Online::EUILobbyJoinRequestedSource Source)
	{
		using FUiJoinResult = UE::Online::TResult<TSharedRef<const UE::Online::FLobby>, UE::Online::FOnlineError>;

		const auto BroadcastFailure = [this, LocalAccountId, Source](UE::Online::FOnlineError&& Error)
		{
			LobbyEvents.OnUILobbyJoinRequested.Broadcast(UE::Online::FUILobbyJoinRequested { .LocalAccountId = LocalAccountId, .Result = FUiJoinResult(MoveTemp(Error)), .JoinRequestedSource = Source });
		};

		UE::Online::TDefaultErrorResultInternal<TSharedRef<FLobbyDetailsSteam>> LobbyDetailsResult =
			FLobbyDetailsSteam::CreateFromLobbyId(LobbyPrerequisites.ToSharedRef(), LocalAccountId, LobbySteamId, ELobbyDetailsSource::UiEvent);

		if (LobbyDetailsResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::RequestUiJoin] FLobbyDetailsSteam::CreateFromLobbyId Failed: User [%s], Lobby [%s], Result [%s]"),
				*ToLogString(LocalAccountId), *ToLogString(LobbySteamId), *LobbyDetailsResult.GetErrorValue().GetLogString());

			BroadcastFailure(MoveTemp(LobbyDetailsResult.GetErrorValue()));
			return;
		}

		auto LobbyDataResult = LobbyDataRegistry->FindOrCreateFromLobbyDetails(LocalAccountId, LobbyDetailsResult.GetOkValue());
		if (LobbyDataResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::RequestUiJoin] FindOrCreateFromLobbyDetails Failed: User [%s], Lobby [%s], Result [%s]"),
				*ToLogString(LocalAccountId), *ToLogString(LobbySteamId), *LobbyDataResult.GetErrorValue().GetLogString());

			BroadcastFailure(MoveTemp(LobbyDataResult.GetErrorValue()));
			return;
		}

		UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::RequestUiJoin] Succeeded: User [%s], Lobby [%s]"),
			*ToLogString(LocalAccountId), *ToLogString(LobbySteamId));

		LobbyEvents.OnUILobbyJoinRequested.Broadcast(UE::Online::FUILobbyJoinRequested {
			.LocalAccountId = LocalAccountId,
			.Result = FUiJoinResult(LobbyDataResult.GetOkValue()->GetLobbyClientData()->GetPublicDataPtr()),
			.JoinRequestedSource = Source });
	}

	void FLobbiesSteam::OnLobbyChatMessage(LobbyChatMsg_t* Message)
	{
		if (!LobbyDataRegistry.IsValid() || Message == nullptr)
		{
			return;
		}

		const auto LobbyData = LobbyDataRegistry->Find(Message->m_ulSteamIDLobby);
		if (!LobbyData)
		{
			return;
		}

		Steam::TSteamResult<Steam::Wrappers::FSteamReadLobbyChatEntry> EntryResult =
			SteamCallSync<Steam::Wrappers::FSteamReadLobbyChatEntry>({ .LobbyId = CSteamID(Message->m_ulSteamIDLobby), .ChatEntryId = static_cast<int32>(Message->m_iChatID) });

		if (EntryResult.IsError() || !EntryResult.GetOkValue().ControlMessage.IsSet())
		{
			// Anything which is not a control message of ours is chat, and none of this component's business.
			return;
		}

		// The channel carries whatever any member cares to send, so a command is only obeyed when it
		// comes from the one member Steam considers to be in charge of the lobby.
		if (EntryResult.GetOkValue().SenderId != CSteamID(Message->m_ulSteamIDUser)
			|| FindAccountId(EntryResult.GetOkValue().SenderId) != LobbyData->GetLobbyClientData()->GetPublicData().OwnerAccountId)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::OnLobbyChatMessage] Ignoring a lobby command which did not come from the owner: Lobby [%s], Sender [%llu]"),
				*ToLogString(LobbyData->GetLobbySteamId()), Message->m_ulSteamIDUser);
			return;
		}

		const auto& ControlMessage = EntryResult.GetOkValue().ControlMessage.GetValue();
		const auto LocalAccountId = GetLocalAccountId();

		if (ControlMessage.Command == Steam::ESteamLobbyControlCommand::Kick
			&& LocalAccountId.IsValid()
			&& GetSteamUserIdChecked(LocalAccountId) == ControlMessage.TargetSteamId)
		{
			HandleLocalUserKicked(LobbyData.ToSharedRef(), LocalAccountId);
		}
	}

	void FLobbiesSteam::OnLobbyInvite(LobbyInvite_t* Message)
	{
		if (!LobbyDataRegistry.IsValid() || Message == nullptr)
		{
			return;
		}

		const auto LocalAccountId = GetLocalAccountId();
		if (!LocalAccountId.IsValid())
		{
			return;
		}

		auto Result = FLobbyInviteDataSteam::Create(LobbyPrerequisites.ToSharedRef(), LobbyDataRegistry.ToSharedRef(),
			LocalAccountId, Message->m_ulSteamIDLobby, Message->m_ulSteamIDUser);

		if (Result.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::OnLobbyInvite] FLobbyInviteDataSteam::Create Failed: User [%s], Result [%s]"),
				*ToLogString(LocalAccountId), *Result.GetErrorValue().GetLogString());
			return;
		}

		UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FLobbiesSteam::OnLobbyInvite] Succeeded: Invite [%s], Receiver [%s], Sender [%s]"),
			*Result.GetOkValue()->GetInviteId(), *ToLogString(Result.GetOkValue()->GetReceiver()), *ToLogString(Result.GetOkValue()->GetSender()));

		AddActiveInvite(Result.GetOkValue());
	}

	void FLobbiesSteam::OnGameLobbyJoinRequested(GameLobbyJoinRequested_t* Message)
	{
		if (!LobbyDataRegistry.IsValid() || Message == nullptr)
		{
			return;
		}

		const auto LocalAccountId = GetLocalAccountId();
		if (!LocalAccountId.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::OnGameLobbyJoinRequested] Ignored: No user is signed in."));
			return;
		}

		// The friend is only set when the join came from an invitation rather than from the friends list.
		RequestUiJoin(Message->m_steamIDLobby, LocalAccountId,
			Message->m_steamIDFriend.IsValid() ? UE::Online::EUILobbyJoinRequestedSource::FromInvitation : UE::Online::EUILobbyJoinRequestedSource::Unspecified);
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FPromoteLobbyMember> FLobbiesSteam::PromoteLobbyMember(UE::Online::FPromoteLobbyMember::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FPromoteLobbyMember>(MoveTemp(InParams));

		// Step 1: Check prerequisites
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FPromoteLobbyMember>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (const auto Auth = Services.GetAuthInterface())
			{
				if (!Auth->IsLoggedIn(Params.LocalAccountId))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::PromoteLobbyMember] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
					return;
				}

				if (!Params.LobbyId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::PromoteLobbyMember] Failed: Lobby id is invalid."));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				const auto LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
				if (!LobbyData)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::PromoteLobbyMember] Failed: Unable to find lobby data. LobbyId [%s]"), *ToLogString(Params.LobbyId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				InAsyncOp.Data.Set<TSharedRef<FLobbyDataSteam>>(LobbyDataKey, LobbyData.ToSharedRef());
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::PromoteLobbyMember] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}
		})
		//Step 2: Start operation.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FPromoteLobbyMember>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			UE::Online::FLobbyClientDataPrepareClientChanges::Params PrepareParams;
			PrepareParams.LocalAccountId = Params.LocalAccountId;
			PrepareParams.ClientChanges.OwnerAccountId = Params.TargetAccountId;

			auto PrepareResult = LobbyData->GetLobbyClientData()->PrepareClientChanges(MoveTemp(PrepareParams));

			if (PrepareResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::FPromoteLobbyMember] PrepareClientChanges Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *PrepareResult.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(PrepareResult.GetErrorValue()));

				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>>(FLobbiesPromoteLobbyMemberImpl::Result{}).GetFuture();
			}

			// PrepareClientChanges writes the new owner only when it differs from the current one, so
			// promoting whoever already owns the lobby comes back Ok with nothing set. Reading the optional
			// there would assert on a client, and only the owner can reach this at all.
			const auto& ServiceChanges = PrepareResult.GetOkValue().ServiceChanges;
			if (!ServiceChanges.OwnerAccountId.IsSet())
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::FPromoteLobbyMember] The member asked for already owns the lobby: User [%s], Lobby [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>>(
					FLobbiesPromoteLobbyMemberImpl::Result { }).GetFuture();
			}

			FLobbiesPromoteLobbyMemberImpl::Params PromoteParams;
			PromoteParams.LobbyData = LobbyData;
			PromoteParams.LocalAccountId = Params.LocalAccountId;
			PromoteParams.TargetAccountId = *ServiceChanges.OwnerAccountId;

			return PromoteLobbyMemberImpl(MoveTemp(PromoteParams));
		})
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FPromoteLobbyMember>& InAsyncOp, UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>&& Result)
		{
			if (Result.IsError())
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::FPromoteLobbyMember] PromoteLobbyMemberImpl Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(Result.GetErrorValue()));
			}
		})
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FPromoteLobbyMember>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			// Write the change through and tell whoever is listening.
			LobbyData->GetLobbyClientData()->CommitClientChanges({ &LobbyEvents });

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::FPromoteLobbyMember] Succeeded: User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(UE::Online::FPromoteLobbyMember::Result{});
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FModifyLobbyJoinPolicy> FLobbiesSteam::ModifyLobbyJoinPolicy(UE::Online::FModifyLobbyJoinPolicy::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FModifyLobbyJoinPolicy>(MoveTemp(InParams));

		// Step 1: Check prerequisites
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyJoinPolicy>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (const auto Auth = Services.GetAuthInterface())
			{
				if (!Auth->IsLoggedIn(Params.LocalAccountId))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyJoinPolicy] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
					return;
				}

				if (!Params.LobbyId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyJoinPolicy] Failed: Lobby id is invalid."));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				const auto LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
				if (!LobbyData)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyJoinPolicy] Failed: Unable to find lobby data. LobbyId [%s]"), *ToLogString(Params.LobbyId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				InAsyncOp.Data.Set<TSharedRef<FLobbyDataSteam>>(LobbyDataKey, LobbyData.ToSharedRef());
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyJoinPolicy] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}
		})
		// Step 2: Modify lobby join policy
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyJoinPolicy>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			UE::Online::FLobbyClientDataPrepareClientChanges::Params PrepareParams;
			PrepareParams.LocalAccountId = Params.LocalAccountId;
			PrepareParams.ClientChanges.JoinPolicy = Params.JoinPolicy;

			auto PrepareResult = LobbyData->GetLobbyClientData()->PrepareClientChanges(MoveTemp(PrepareParams));
			if (PrepareResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyJoinPolicy] PrepareClientChanges Failed: User[%s], Lobby[%s], Result[%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *PrepareResult.GetErrorValue().GetLogString());
				InAsyncOp.SetError(MoveTemp(PrepareResult.GetErrorValue()));

				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(FLobbiesModifyLobbyDataImpl::Result { }).GetFuture();
			}

			FLobbiesModifyLobbyDataImpl::Params ModifyLobbyDataParams;
			ModifyLobbyDataParams.LobbyData = LobbyData;
			ModifyLobbyDataParams.LocalAccountId = Params.LocalAccountId;
			ModifyLobbyDataParams.ServiceChanges = MoveTemp(PrepareResult.GetOkValue().ServiceChanges);

			return ModifyLobbyDataImpl(MoveTemp(ModifyLobbyDataParams));
		})
		// Step 3: Handle errors
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyJoinPolicy>& InAsyncOp, UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>&& Result){
			if (Result.IsError())
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyJoinPolicy] ModifyLobbyDataImpl Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(Result.GetErrorValue()));
			}
		})
		// Step 4: Finalize
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyJoinPolicy>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			// Write the change through and tell whoever is listening.
			LobbyData->GetLobbyClientData()->CommitClientChanges({ &LobbyEvents });

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::ModifyLobbyJoinPolicy] Succeeded: User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(UE::Online::FModifyLobbyJoinPolicy::Result { });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FModifyLobbyAttributes> FLobbiesSteam::ModifyLobbyAttributes(UE::Online::FModifyLobbyAttributes::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FModifyLobbyAttributes>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyAttributes>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (const auto Auth = Services.GetAuthInterface())
			{
				if (!Auth->IsLoggedIn(Params.LocalAccountId))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyAttributes] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
					return;
				}

				if (!Params.LobbyId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyAttributes] Failed: Lobby id is invalid."));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				const auto LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
				if (!LobbyData)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyAttributes] Failed: Unable to find lobby data. LobbyId [%s]"), *ToLogString(Params.LobbyId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				InAsyncOp.Data.Set<TSharedRef<FLobbyDataSteam>>(LobbyDataKey, LobbyData.ToSharedRef());
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyAttributes] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}
		})
		//Step 2: Modify Lobby Member attributes.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyAttributes>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			UE::Online::FLobbyClientDataPrepareClientChanges::Params PrepareParams;
			PrepareParams.LocalAccountId = Params.LocalAccountId;
			PrepareParams.ClientChanges.Attributes = { Params.UpdatedAttributes, Params.RemovedAttributes };

			auto PrepareResult = LobbyData->GetLobbyClientData()->PrepareClientChanges(MoveTemp(PrepareParams));
			if (PrepareResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyAttributes] PrepareClientChanges Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *PrepareResult.GetErrorValue().GetLogString());
				InAsyncOp.SetError(MoveTemp(PrepareResult.GetErrorValue()));

				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(FLobbiesModifyLobbyDataImpl::Result {}).GetFuture();
			}

			FLobbiesModifyLobbyDataImpl::Params ModifyLobbyDataParams;
			ModifyLobbyDataParams.LobbyData = LobbyData;
			ModifyLobbyDataParams.LocalAccountId = Params.LocalAccountId;
			ModifyLobbyDataParams.ServiceChanges = MoveTemp(PrepareResult.GetOkValue().ServiceChanges);

			return ModifyLobbyDataImpl(MoveTemp(ModifyLobbyDataParams));
		})
		// Step 3: Handle errors
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyAttributes>& InAsyncOp, UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>&& Result)
		{
			if (Result.IsError())
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyAttributes] ModifyLobbyMemberDataImpl Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(Result.GetErrorValue()));
			}
		})
		// Step 4: Finalize
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyAttributes>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			// Write the change through and tell whoever is listening.
			LobbyData->GetLobbyClientData()->CommitClientChanges({ &LobbyEvents });

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::ModifyLobbyAttributes] Succeeded: User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(UE::Online::FModifyLobbyAttributes::Result { });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FModifyLobbyMemberAttributes> FLobbiesSteam::ModifyLobbyMemberAttributes(UE::Online::FModifyLobbyMemberAttributes::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FModifyLobbyMemberAttributes>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyMemberAttributes>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (const auto Auth = Services.GetAuthInterface())
			{
				if (!Auth->IsLoggedIn(Params.LocalAccountId))
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyMemberAttributes] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
					return;
				}

				if (!Params.LobbyId.IsValid())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyMemberAttributes] Failed: Lobby id is invalid."));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				const auto LobbyData = LobbyDataRegistry->Find(Params.LobbyId);
				if (!LobbyData)
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyMemberAttributes] Failed: Unable to find lobby data. LobbyId [%s]"), *ToLogString(Params.LobbyId));
					InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
					return;
				}

				InAsyncOp.Data.Set<TSharedRef<FLobbyDataSteam>>(LobbyDataKey, LobbyData.ToSharedRef());
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyMemberAttributes] Failed: Authentication Interface is not found"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
			}
		})
		//Step 2: Modify Lobby Member attributes.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyMemberAttributes>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			UE::Online::FLobbyClientDataPrepareClientChanges::Params PrepareParams;
			PrepareParams.LocalAccountId = Params.LocalAccountId;
			PrepareParams.ClientChanges.MemberAttributes = { Params.UpdatedAttributes, Params.RemovedAttributes };

			auto PrepareResult = LobbyData->GetLobbyClientData()->PrepareClientChanges(MoveTemp(PrepareParams));
			if (PrepareResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyMemberAttributes] PrepareClientChanges Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *PrepareResult.GetErrorValue().GetLogString());
				InAsyncOp.SetError(MoveTemp(PrepareResult.GetErrorValue()));

				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(FLobbiesModifyLobbyDataImpl::Result {}).GetFuture();
			}

			FLobbiesModifyLobbyDataImpl::Params ModifyLobbyDataParams;
			ModifyLobbyDataParams.LobbyData = LobbyData;
			ModifyLobbyDataParams.LocalAccountId = Params.LocalAccountId;
			ModifyLobbyDataParams.ServiceChanges = MoveTemp(PrepareResult.GetOkValue().ServiceChanges);

			return ModifyLobbyDataImpl(MoveTemp(ModifyLobbyDataParams));
		})
		// Step 3: Handle errors
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyMemberAttributes>& InAsyncOp, UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>&& Result)
		{
			if (Result.IsError())
			{
				const auto& Params = InAsyncOp.GetParams();
				const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyMemberAttributes] ModifyLobbyMemberDataImpl Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(Result.GetErrorValue()));
			}
		})
		// Step 4: Finalize
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FModifyLobbyMemberAttributes>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const auto& LobbyData = GetOpDataChecked<TSharedRef<FLobbyDataSteam>>(InAsyncOp, LobbyDataKey);

			// Write the change through and tell whoever is listening.
			LobbyData->GetLobbyClientData()->CommitClientChanges({ &LobbyEvents });

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::ModifyLobbyMemberAttributes] Succeeded: User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(UE::Online::FModifyLobbyMemberAttributes::Result { });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FGetJoinedLobbies> FLobbiesSteam::GetJoinedLobbies(UE::Online::FGetJoinedLobbies::Params&& Params)
	{
		// Being in no lobby is an answer, and an empty list is how it is given: the table below only holds a
		// user who has joined something, so its silence says nothing about whether the user exists. That is
		// asked separately, and is the only thing this refuses.
		if (auto LocalUser = ResolveLocalSteamUser(Params.LocalAccountId, TEXT("FLobbiesSteam::GetJoinedLobbies")); LocalUser.IsError())
		{
			return UE::Online::TOnlineResult<UE::Online::FGetJoinedLobbies>(MoveTemp(LocalUser.GetErrorValue()));
		}

		UE::Online::FGetJoinedLobbies::Result Result;

		if (const TSet<TSharedRef<FLobbyDataSteam>>* Lobbies = ActiveLobbies.Find(Params.LocalAccountId))
		{
			Result.Lobbies.Reserve(Lobbies->Num());

			for (const auto& LobbyDataSteam : *Lobbies)
			{
				Result.Lobbies.Emplace(LobbyDataSteam->GetLobbyClientData()->GetPublicDataPtr());
			}
		}

		return UE::Online::TOnlineResult<UE::Online::FGetJoinedLobbies>(MoveTemp(Result));
	}

	UE::Online::TOnlineResult<UE::Online::FGetReceivedInvitations> FLobbiesSteam::GetReceivedInvitations(UE::Online::FGetReceivedInvitations::Params&& Params)
	{
		// Invitations are tracked, so the query is answered rather than refused; the result struct of the
		// interface carries no fields, so what was received is only ever reported through the events.
		UE_LOG(LogOnlineServicesSteam, VeryVerbose, TEXT("[FLobbiesSteam::GetReceivedInvitations] User [%s], Invitations [%d]"),
			*ToLogString(Params.LocalAccountId), ActiveInvites.Contains(Params.LocalAccountId) ? ActiveInvites[Params.LocalAccountId].Num() : 0);

		return UE::Online::TOnlineResult<UE::Online::FGetReceivedInvitations>(UE::Online::FGetReceivedInvitations::Result { });
	}

	void FLobbiesSteam::AddActiveInvite(const TSharedRef<FLobbyInviteDataSteam>& Invite)
	{
		auto& ActiveUserInvites = ActiveInvites.FindOrAdd(Invite->GetReceiver());

		if (const UE::Online::FLobbyId LobbyId = Invite->GetLobbyData()->GetLobbyIdHandle(); !ActiveUserInvites.Contains(LobbyId))
		{
			ActiveUserInvites.Add(LobbyId, Invite);
			LobbyEvents.OnLobbyInvitationAdded.Broadcast(
				UE::Online::FLobbyInvitationAdded{
					Invite->GetReceiver(),
					Invite->GetSender(),
					Invite->GetLobbyData()->GetLobbyClientData()->GetPublicDataPtr()
				});
		}
	}

	void FLobbiesSteam::RemoveActiveInvite(const TSharedRef<FLobbyInviteDataSteam>& Invite)
	{
		// Found rather than found-or-added: removing an invitation for a user who has none would otherwise
		// insert an empty map, and nothing ever prunes those.
		if (const auto ReceiverInvites = ActiveInvites.Find(Invite->GetReceiver()))
		{
			ReceiverInvites->Remove(Invite->GetLobbyData()->GetLobbyIdHandle());
		}

		LobbyEvents.OnLobbyInvitationRemoved.Broadcast(
			UE::Online::FLobbyInvitationRemoved{
				Invite->GetReceiver(),
				Invite->GetSender(),
				Invite->GetLobbyData()->GetLobbyClientData()->GetPublicDataPtr()
			});
	}

	TSharedPtr<FLobbyInviteDataSteam> FLobbiesSteam::GetActiveInvite(UE::Online::FAccountId TargetUser, UE::Online::FLobbyId TargetLobbyId)
	{
		if (const TMap<UE::Online::FLobbyId, TSharedRef<FLobbyInviteDataSteam>>* UserInvites = ActiveInvites.Find(TargetUser))
		{
			if (const TSharedRef<FLobbyInviteDataSteam>* Result = UserInvites->Find(TargetLobbyId))
			{
				return *Result;
			}
		}

		return nullptr;
	}

	TFuture<UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>> FLobbiesSteam::LeaveLobbyImpl(FLobbiesLeaveLobbyImpl::Params&& Params) const
	{
		if (const auto Auth = Services.GetAuthInterface())
		{
			if (!Auth->IsLoggedIn(Params.LocalAccountId))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobbyImpl] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>>(UE::Online::Errors::NotLoggedIn()).GetFuture();
			}

			if (!Params.LobbyData.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobbyImpl] Failed: No lobby data provided."));
				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>>(UE::Online::Errors::InvalidParams()).GetFuture();
			}

			auto LeaveResult = SteamCallSync<Steam::Wrappers::FSteamLeaveLobby>({ .LobbyId = Params.LobbyData->GetLobbySteamId() });
			if (LeaveResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobbyImpl] Steam->LeaveLobby Failed: User [%s], Lobby [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()), *LeaveResult.GetErrorValue().GetLogString());

				return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>>(MoveTemp(LeaveResult.GetErrorValue())).GetFuture();
			}

			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>>(FLobbiesLeaveLobbyImpl::Result { }).GetFuture();
		}

		UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::LeaveLobbyImpl] Failed: Authentication Interface is not found"));
		return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesLeaveLobbyImpl>>(UE::Online::Errors::MissingInterface()).GetFuture();
	}

	TFuture<UE::Online::TDefaultErrorResult<FLobbiesDestroyLobbyImpl>> FLobbiesSteam::DestroyLobbyImpl(FLobbiesDestroyLobbyImpl::Params&& Params) const
	{
		// Check prerequisites. Reached from the failure path of CreateLobby, which is after an await: the
		// authentication interface is not guaranteed to still be there, and it was dereferenced unguarded.
		if (auto LocalSteamUser = ResolveLocalSteamUser(Params.LocalAccountId, TEXT("FLobbiesSteam::DestroyLobbyImpl")); LocalSteamUser.IsError())
		{
			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesDestroyLobbyImpl>>(
				MoveTemp(LocalSteamUser.GetErrorValue())).GetFuture();
		}

		if (!Params.LobbySteamId.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::DestroyLobbyImpl] Failed: No lobby id provided."));
			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesDestroyLobbyImpl>>(UE::Online::Errors::InvalidParams()).GetFuture();
		}

		auto DestroyResult = SteamCallSync<Steam::Wrappers::FSteamDestroyLobby>({ .LobbyId = Params.LobbySteamId });
		if (DestroyResult.IsError())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::DestroyLobbyImpl] Steam->DestroyLobby Failed: User [%s], Lobby [%s], Result [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(Params.LobbySteamId), *DestroyResult.GetErrorValue().GetLogString());

			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesDestroyLobbyImpl>>(MoveTemp(DestroyResult.GetErrorValue())).GetFuture();
		}

		return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesDestroyLobbyImpl>>(FLobbiesDestroyLobbyImpl::Result { }).GetFuture();
	}

	TFuture<UE::Online::TDefaultErrorResult<FLobbiesInviteLobbyMemberImpl>> FLobbiesSteam::InviteLobbyMemberImpl(FLobbiesInviteLobbyMemberImpl::Params&& Params) const
	{
		// Check prerequisites. One call answers for the interface, the login and the target at once; the
		// target used to go through a checked lookup, so an id the game had gone stale on was fatal.
		auto TargetSteamIdResult = ResolveSteamUser(Params.LocalAccountId, Params.TargetAccountId, TEXT("FLobbiesSteam::InviteLobbyMemberImpl"));
		if (TargetSteamIdResult.IsError())
		{
			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesInviteLobbyMemberImpl>>(
				MoveTemp(TargetSteamIdResult.GetErrorValue())).GetFuture();
		}

		if (!Params.LobbyData.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::InviteLobbyMemberImpl] Failed: No lobby data provided."));
			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesInviteLobbyMemberImpl>>(UE::Online::Errors::InvalidParams()).GetFuture();
		}

		if (!LobbyPrerequisites->LobbyInterfaceHandle->InviteUserToLobby(Params.LobbyData->GetLobbySteamId(), TargetSteamIdResult.GetOkValue()))
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::InviteLobbyMemberImpl] Steam->InviteUserToLobby Failed: User [%s], TargetUser [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(Params.TargetAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()));

			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesInviteLobbyMemberImpl>>(UE::Online::Errors::NoConnection()).GetFuture();
		}

		UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::InviteLobbyMemberImpl] Succeeded: User [%s], TargetUser [%s], Lobby [%s]"),
			*ToLogString(Params.LocalAccountId), *ToLogString(Params.TargetAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()));

		return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesInviteLobbyMemberImpl>>(FLobbiesInviteLobbyMemberImpl::Result { }).GetFuture();
	}

	TFuture<UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>> FLobbiesSteam::PromoteLobbyMemberImpl(FLobbiesPromoteLobbyMemberImpl::Params&& Params) const
	{
		// Check prerequisites. One call answers for the interface, the login and the target at once; the
		// target used to go through a checked lookup, so an id the game had gone stale on was fatal.
		auto TargetSteamIdResult = ResolveSteamUser(Params.LocalAccountId, Params.TargetAccountId, TEXT("FLobbiesSteam::PromoteLobbyMemberImpl"));
		if (TargetSteamIdResult.IsError())
		{
			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>>(
				MoveTemp(TargetSteamIdResult.GetErrorValue())).GetFuture();
		}

		if (!Params.LobbyData.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::PromoteLobbyMemberImpl] Failed: No lobby data provided."));

			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>>(UE::Online::Errors::InvalidParams()).GetFuture();
		}

		if (Params.LobbyData->GetLobbyClientData()->GetPublicData().OwnerAccountId != Params.LocalAccountId)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::PromoteLobbyMemberImpl] Failed: User is not the lobby owner. User [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()));

			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>>(UE::Online::Errors::InvalidParams()).GetFuture();
		}

		// Start operation.
		if (!LobbyPrerequisites->LobbyInterfaceHandle->SetLobbyOwner(Params.LobbyData->GetLobbySteamId(), TargetSteamIdResult.GetOkValue()))
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::PromoteLobbyMemberImpl] Steam->SetLobbyOwner Failed: User [%s], TargetUser [%s], Lobby [%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(Params.TargetAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()));

			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>>(UE::Online::Errors::NoConnection()).GetFuture();
		}

		UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::PromoteLobbyMemberImpl] Succeeded: User [%s], TargetUser [%s], Lobby [%s]"),
			*ToLogString(Params.LocalAccountId), *ToLogString(Params.TargetAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()));

		return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesPromoteLobbyMemberImpl>>(FLobbiesPromoteLobbyMemberImpl::Result { }).GetFuture();
	}

	TFuture<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>> FLobbiesSteam::ModifyLobbyDataImpl(FLobbiesModifyLobbyDataImpl::Params&& Params) const
	{
		// Check prerequisites. Six operations reach this after an await, which is exactly the window where
		// the authentication interface can be gone; it was dereferenced unguarded.
		if (auto LocalSteamUser = ResolveLocalSteamUser(Params.LocalAccountId, TEXT("FLobbiesSteam::ModifyLobbyDataImpl")); LocalSteamUser.IsError())
		{
			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(
				MoveTemp(LocalSteamUser.GetErrorValue())).GetFuture();
		}

		if (!Params.LobbyData.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyDataImpl] Failed: No lobby data provided."));
			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(UE::Online::Errors::InvalidParams()).GetFuture();
		}

		const auto LobbyDetails = Params.LobbyData->GetUserLobbyDetails(Params.LocalAccountId);
		if (!LobbyDetails)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ModifyLobbyDataImpl] Failed: Unable to find lobby details for user. User[%s], Lobby[%s]"),
				*ToLogString(Params.LocalAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()));

			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(UE::Online::Errors::InvalidParams()).GetFuture();
		}

		// Start operation.
		auto UpdateResult = LobbyDetails->ApplyLobbyDataUpdateFromLocalChanges(Params.LocalAccountId, Params.ServiceChanges);
		if (UpdateResult != UE::Online::Errors::Success())
		{
			return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(MoveTemp(UpdateResult)).GetFuture();
		}

		UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::ModifyLobbyDataImpl] Succeeded: User [%s], Lobby [%s]"),
			*ToLogString(Params.LocalAccountId), *ToLogString(Params.LobbyData->GetLobbySteamId()));

		return MakeFulfilledPromise<UE::Online::TDefaultErrorResult<FLobbiesModifyLobbyDataImpl>>(FLobbiesModifyLobbyDataImpl::Result { }).GetFuture();
	}

	UE::Online::TOnlineAsyncOpHandle<FLobbiesProcessLobbyNotificationImpl> FLobbiesSteam::ProcessLobbyNotificationImplOp(FLobbiesProcessLobbyNotificationImpl::Params&& InParams)
	{
		const auto Op = GetOp<FLobbiesProcessLobbyNotificationImpl>(MoveTemp(InParams));

		// Step 1: Check prerequisites.
		Op->Then([this](UE::Online::TOnlineAsyncOp<FLobbiesProcessLobbyNotificationImpl>& InAsyncOp) {
			const auto& Params = InAsyncOp.GetParams();

			if (!Params.LobbyData.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ProcessLobbyNotificationImplOp] Failed: No lobby data provided."));
				InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
			}
		})
		// Step 2: Start operation.
		.Then([this](UE::Online::TOnlineAsyncOp<FLobbiesProcessLobbyNotificationImpl>& InAsyncOp)
		{
			const auto& [LobbyData, MutatedMembers, LeavingMembers] = InAsyncOp.GetParams();

			// A notification does not always name a user, so any usable details handle will do to
			// read the snapshot with.
			const auto LobbyDetails = LobbyData->GetActiveLobbyDetails();

			if (!LobbyDetails.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ProcessLobbyNotificationImplOp] Failed: Unable to find active lobby details to process notifications. Lobby[%s]"),
					*ToLogString(LobbyData->GetLobbySteamId()));

				InAsyncOp.SetError(UE::Online::Errors::InvalidState());

				return UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyServiceSnapshot>(UE::Online::Errors::InvalidState());
			}

			InAsyncOp.Data.Set<TSharedRef<FLobbyDetailsSteam>>(LobbyDetailsKey, LobbyDetails.ToSharedRef());

			// Take the snapshot; doing so is what resolves the account id of every member in it.
			return LobbyDetails->GetLobbySnapshot(true);
		})
		.Then([this](UE::Online::TOnlineAsyncOp<FLobbiesProcessLobbyNotificationImpl>& InAsyncOp, UE::Online::TDefaultErrorResultInternal<UE::Online::FLobbyServiceSnapshot>&& LobbySnapshotResult)
		{
			const auto& [LobbyData, MutatedMembers, LeavingMembers] = InAsyncOp.GetParams();
			const auto LobbyDetails = GetOpDataChecked<TSharedRef<FLobbyDetailsSteam>>(InAsyncOp, LobbyDetailsKey);

			if (LobbySnapshotResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ProcessLobbyNotificationImplOp] GetLobbySnapshot Failed. Lobby [%s], Result [%s]"),
					*ToLogString(LobbyData->GetLobbySteamId()), *LobbySnapshotResult.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(LobbySnapshotResult.GetErrorValue()));
				return;
			}

			// Get member snapshots.
			TMap<UE::Online::FAccountId, UE::Online::FLobbyMemberServiceSnapshot> LobbyMemberSnapshots;
			LobbyMemberSnapshots.Reserve(MutatedMembers.Num());

			for (CSteamID MutatedMember : MutatedMembers)
			{
				if (const UE::Online::FAccountId MutatedMemberAccountId = FindAccountId(MutatedMember); MutatedMemberAccountId.IsValid())
				{
					auto MemberSnapshotResult = LobbyDetails->GetLobbyMemberSnapshot(MutatedMemberAccountId);
					if (MemberSnapshotResult.IsError())
					{
						UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ProcessLobbyNotificationImplOp] GetLobbyMemberSnapshot Failed. User [%s], Lobby [%s], Result [%s]"),
							*ToLogString(MutatedMemberAccountId), *ToLogString(LobbyData->GetLobbySteamId()), *MemberSnapshotResult.GetErrorValue().GetLogString());

						InAsyncOp.SetError(MoveTemp(MemberSnapshotResult.GetErrorValue()));
						return;
					}

					LobbyMemberSnapshots.Add(MutatedMemberAccountId, MoveTemp(MemberSnapshotResult.GetOkValue()));
				}
			}

			// Translate leaving members from CSteamId to FAccountId.
			TMap<UE::Online::FAccountId, UE::Online::ELobbyMemberLeaveReason> LeavingMemberReason;
			LeavingMemberReason.Reserve(LeavingMembers.Num());

			for (const auto& LeavingMember : LeavingMembers)
			{
				if (const UE::Online::FAccountId LeavingMemberAccountId = FindAccountId(LeavingMember.Key); LeavingMemberAccountId.IsValid())
				{
					LeavingMemberReason.Add(LeavingMemberAccountId, LeavingMember.Value);
				}
			}

			// Turn the snapshot into the state the game sees.
			UE::Online::TOnlineResult<UE::Online::FLobbyClientDataPrepareServiceSnapshot> PrepareSnapshotResult =
				LobbyData->GetLobbyClientData()->PrepareServiceSnapshot({ .LobbySnapshot = MoveTemp(LobbySnapshotResult.GetOkValue()), .LobbyMemberSnapshots = MoveTemp(LobbyMemberSnapshots), .LeaveReasons = MoveTemp(LeavingMemberReason) });
			if (PrepareSnapshotResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::ProcessLobbyNotificationImplOp] PrepareServiceSnapshot Failed. Lobby [%s], Result [%s]"),
					*ToLogString(LobbyData->GetLobbySteamId()), *PrepareSnapshotResult.GetErrorValue().GetLogString());
				InAsyncOp.SetError(MoveTemp(PrepareSnapshotResult.GetErrorValue()));
			}
		})
		.Then([this](UE::Online::TOnlineAsyncOp<FLobbiesProcessLobbyNotificationImpl>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			// Fold the change in, then notify.
			auto [LeavingLocalMembers] = Params.LobbyData->GetLobbyClientData()->CommitServiceSnapshot({ &LobbyEvents });

			// Anyone who is no longer a member stops being one here.
			for (const auto LeavingMember : LeavingLocalMembers)
			{
				RemoveActiveLobby(LeavingMember, Params.LobbyData.ToSharedRef());
			}

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::ProcessLobbyNotificationImplOp] Succeeded: Lobby [%s]"),
				*ToLogString(Params.LobbyData->GetLobbySteamId()));

			InAsyncOp.SetResult(FLobbiesProcessLobbyNotificationImpl::Result{});
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	void FLobbiesSteam::ProcessLobbyNotification(const TSharedRef<FLobbyDataSteam>& LobbyData, const TCHAR* Context, FLobbiesProcessLobbyNotificationImpl::Params&& Params)
	{
		Params.LobbyData = LobbyData;

		ProcessLobbyNotificationImplOp(MoveTemp(Params))
		.OnComplete([LobbyData, Context](const UE::Online::TOnlineResult<FLobbiesProcessLobbyNotificationImpl>& Result)
		{
			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FLobbiesSteam::%s] ProcessLobbyNotificationImplOp Failed: Lobby [%s], Result [%s]"),
					Context, *ToLogString(LobbyData->GetLobbySteamId()), *Result.GetErrorValue().GetLogString());
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FLobbiesSteam::%s] Succeeded: Lobby [%s]"),
					Context, *ToLogString(LobbyData->GetLobbySteamId()));
			}
		});
	}

	void FLobbiesSteam::OnLobbyDataUpdate(LobbyDataUpdate_t* Message)
	{
		if (!LobbyDataRegistry.IsValid() || Message == nullptr || !Message->m_bSuccess)
		{
			return;
		}

		if (const TSharedPtr<FLobbyDataSteam> LobbyData = LobbyDataRegistry->Find(Message->m_ulSteamIDLobby))
		{
			FLobbiesProcessLobbyNotificationImpl::Params Params;

			// This is the only path a change to a lobby takes, the address of its game server included:
			// Steam mirrors that into the metadata and announces it here like any other change.
			//
			// Steam reports a change to the metadata of one member through the same callback, telling the
			// two apart by whether the subject of the change is the lobby itself.
			if (const CSteamID ChangedMember { Message->m_ulSteamIDMember }; ChangedMember != CSteamID(Message->m_ulSteamIDLobby))
			{
				Params.MutatedMembers.Add(ChangedMember);
			}

			ProcessLobbyNotification(LobbyData.ToSharedRef(), TEXT("OnLobbyDataUpdate"), MoveTemp(Params));
		}
	}

	void FLobbiesSteam::OnLobbyChatUpdate(LobbyChatUpdate_t* Message)
	{
		if (!LobbyDataRegistry.IsValid() || Message == nullptr)
		{
			return;
		}

		if (const TSharedPtr<FLobbyDataSteam> LobbyData = LobbyDataRegistry->Find(Message->m_ulSteamIDLobby))
		{
			FLobbiesProcessLobbyNotificationImpl::Params Params;

			const CSteamID ChangedMember { Message->m_ulSteamIDUserChanged };

			// The SDK documents this as a bitfield, and ships BChatMemberStateChangeRemoved because callers
			// have to test bits: a ban is also a kick, so 0x18 arrives as one value and a switch over the
			// whole word matched none of its cases and dropped the departure.
			const uint32 StateChange = Message->m_rgfChatMemberStateChange;

			if ((StateChange & k_EChatMemberStateChangeEntered) != 0)
			{
				Params.MutatedMembers.Add(ChangedMember);
			}

			if (BChatMemberStateChangeRemoved(StateChange))
			{
				// A member this host removed leaves of their own accord as far as Steam is concerned, so the
				// reason has to come from what the host asked for rather than from what Steam reports. The
				// record is consumed here, on an actual removal, rather than on any update at all.
				auto LobbyPendingKicks = PendingKicks.Find(LobbyData->GetLobbyIdHandle());
				const bool bWasKicked = LobbyPendingKicks != nullptr && LobbyPendingKicks->Remove(ChangedMember) > 0;

				auto LeaveReason = UE::Online::ELobbyMemberLeaveReason::Left;
				if (bWasKicked || (StateChange & (k_EChatMemberStateChangeKicked | k_EChatMemberStateChangeBanned)) != 0)
				{
					LeaveReason = UE::Online::ELobbyMemberLeaveReason::Kicked;
				}
				else if ((StateChange & k_EChatMemberStateChangeDisconnected) != 0)
				{
					LeaveReason = UE::Online::ELobbyMemberLeaveReason::Disconnected;
				}

				Params.LeavingMembers.Add(ChangedMember, LeaveReason);

				if (LobbyPendingKicks != nullptr && LobbyPendingKicks->IsEmpty())
				{
					PendingKicks.Remove(LobbyData->GetLobbyIdHandle());
				}
			}

			// The membership changed, so the count a lobby browser reads has to be republished. Only the
			// owner may do so; for every other member this does nothing.
			LobbyData->GetLobbyDetails()->PublishOwnedMetadata();

			ProcessLobbyNotification(LobbyData.ToSharedRef(), TEXT("OnLobbyChatUpdate"), MoveTemp(Params));
		}
	}
}
