// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/AuthSteam.h"

// Project
#include "Algo/AllOf.h"
#include "OnlineServicesSteamLogChannels.h"
#include "SteamCommon.h"
#include "SteamInterfaces.h"
#include "Online/OnlineIdSteam.h"
#include "Online/OnlineServicesSteam.h"
#include "Online/UserInfoUtils.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/SteamResult.h"
#include "Steam/Wrappers/SteamUserAuth.h"
#include "Steam/Wrappers/SteamUserInfo.h"

// Engine
#include "Online/AuthErrors.h"
#include "Online/OnlineUtilsCommon.h"


namespace UE::Online::Meta
{
	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FAuthRefreshAccountAttributesImpl::Params)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FAuthRefreshAccountAttributesImpl::Params, LocalAccountId),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FAuthRefreshAccountAttributesImpl::Params, bRefreshDisplayName),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FAuthRefreshAccountAttributesImpl::Params, bRefreshAvatar)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FAuthRefreshAccountAttributesImpl::Result)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FAuthSteamConfig)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FAuthSteamConfig, bVerifyAuthTicketsOnGameServer),
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FAuthSteamConfig, WebApiIdentity)
	END_ONLINE_STRUCT_META()
}

namespace PoFigGames::Online
{
	/** Key the pending display name is carried on inside the refresh operation. */
	static const FString DisplayNameKey { TEXT("DisplayNameSteam") };

	/**
	 * Whether a ticket handed in by a remote peer can be turned into bytes at all.
	 *
	 * Anything that reaches BeginVerifiedAuthSession came over the network, and HexToBytes trusts what it
	 * is given: it writes a byte past a buffer sized from an odd number of digits, and asserts on the
	 * first character that is not one. Neither is a failure mode a server should have.
	 */
	static bool IsWellFormedTicket(const FString& Ticket)
	{
		if (Ticket.IsEmpty() || Ticket.Len() % 2 != 0 || Ticket.Len() / 2 > Steam::MaxAuthSessionTicketSize)
		{
			return false;
		}

		return Algo::AllOf(Ticket, [](const TCHAR Character) { return FChar::IsHexDigit(Character); });
	}

	TSharedPtr<FAccountInfoSteam> FAccountInfoRegistrySteam::Find(const FPlatformUserId PlatformUserId) const
	{
		return StaticCastSharedPtr<FAccountInfoSteam>(Super::Find(PlatformUserId));
	}

	TSharedPtr<FAccountInfoSteam> FAccountInfoRegistrySteam::Find(const UE::Online::FAccountId AccountId) const
	{
		return StaticCastSharedPtr<FAccountInfoSteam>(Super::Find(AccountId));
	}

	TSharedPtr<FAccountInfoSteam> FAccountInfoRegistrySteam::Find(const CSteamID& SteamAccountId) const
	{
		FReadScopeLock Lock(IndexLock);
		if (const TSharedRef<FAccountInfoSteam>* FoundPtr = AuthDataBySteamID.Find(SteamAccountId))
		{
			return *FoundPtr;
		}

		return nullptr;
	}

	void FAccountInfoRegistrySteam::Register(const TSharedRef<FAccountInfoSteam>& AccountInfo)
	{
		FWriteScopeLock Lock(IndexLock);
		DoRegister(AccountInfo);
	}

	void FAccountInfoRegistrySteam::Unregister(const UE::Online::FAccountId AccountId)
	{
		if (const TSharedPtr<FAccountInfoSteam> AccountInfo = Find(AccountId))
		{
			FWriteScopeLock Lock(IndexLock);
			DoUnregister(AccountInfo.ToSharedRef());
		}
		else
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAccountInfoRegistrySteam::Unregister] Failed to find account [%s]."), *ToLogString(AccountId));
		}
	}

	void FAccountInfoRegistrySteam::DoRegister(const TSharedRef<UE::Online::FAccountInfo>& AccountInfo)
	{
		const auto AccountInfoSteam = StaticCastSharedRef<FAccountInfoSteam>(AccountInfo);

		Super::DoRegister(AccountInfo);

		if (AccountInfoSteam->SteamAccountId.IsValid())
		{
			AuthDataBySteamID.Add(AccountInfoSteam->SteamAccountId, AccountInfoSteam);
		}
	}

	void FAccountInfoRegistrySteam::DoUnregister(const TSharedRef<UE::Online::FAccountInfo>& AccountInfo)
	{
		const auto AccountInfoSteam = StaticCastSharedRef<FAccountInfoSteam>(AccountInfo);

		Super::DoUnregister(AccountInfo);

		if (AccountInfoSteam->SteamAccountId.IsValid())
		{
			AuthDataBySteamID.Remove(AccountInfoSteam->SteamAccountId);
		}
	}

	FAuthSteam::FAuthSteam(UE::Online::FOnlineServicesCommon& InServices)
		: TOnlineComponentSteam(InServices)
	{
	}

	void FAuthSteam::Initialize()
	{
		Super::Initialize();

		// Steam announces a persona change instead of answering a query, so the component listens for as long
		// as it is alive rather than polling.
		OnPersonaStateChangeCallback.Register(this, &FAuthSteam::OnPersonaStateChange);

		// A verified session can be withdrawn long after it was granted, and the request which granted it is
		// gone by then, so the component is what listens for the rest of its life.
		OnClientAuthSessionVerdictCallback.Register(this, &FAuthSteam::OnAuthSessionVerdict);
		OnServerAuthSessionVerdictCallback.Register(this, &FAuthSteam::OnAuthSessionVerdict);
	}

	void FAuthSteam::UpdateConfig()
	{
		Super::UpdateConfig();

		LoadConfig(Config);
	}

	void FAuthSteam::PreShutdown()
	{
		// The handlers reach for the account registry and the session map, so they have to stop firing before
		// anything is torn down.
		OnPersonaStateChangeCallback.Unregister();
		OnClientAuthSessionVerdictCallback.Unregister();
		OnServerAuthSessionVerdictCallback.Unregister();

		// Steam keeps tracking users and tickets until it is told otherwise, so nothing is left behind here.
		for (const auto& VerifiedSession : VerifiedAuthSessions)
		{
			SteamCallSync<Steam::Wrappers::FSteamEndAuthSession>({ .RemoteUserId = VerifiedSession.Value });
		}
		VerifiedAuthSessions.Empty();

		for (const auto& IssuedTicket : IssuedAuthTickets)
		{
			SteamCallSync<Steam::Wrappers::FSteamCancelAuthTicket>({ .TicketHandle = IssuedTicket.Value->TicketHandle });
		}
		IssuedAuthTickets.Empty();

		Super::PreShutdown();
	}

	UE::Online::FAccountId FAuthSteam::CreateAccountId(const CSteamID& SteamAccountId)
	{
		return FOnlineAccountIdRegistrySteam::GetRegistered(UE::Online::EOnlineServices::Steam).FindOrAddAccountId(SteamAccountId);
	}

	const UE::Online::FAccountInfoRegistry& FAuthSteam::GetAccountInfoRegistry() const
	{
		return AccountInfoRegistrySteam;
	}

	bool FAuthSteam::IsGameServerLogin(const UE::Online::FAuthLogin::Params& Params) const
	{
		if (Params.CredentialsType == LoginCredentialsType::AnonymousGameServer)
		{
			return true;
		}

		if (!Params.CredentialsType.IsNone() && Params.CredentialsType != UE::Online::LoginCredentialsType::Auto)
		{
			// Every other credentials type describes a user.
			return false;
		}

		// Nothing was asked for, so the role of this instance answers: a dedicated server runs no client API
		// and therefore has no user to log in.
		return GetOnlineServicesSteam().GetClientService() == nullptr;
	}

	/** The one bucket every game server login shares, so that two spellings of that identity meet in Compare. */
	static uint32 HashGameServerLogin()
	{
		return GetTypeHash(LoginCredentialsType::AnonymousGameServer);
	}

	/** Hashes the part of a login that FLoginJoinFuncs compares; the token is left out, as it is there. */
	static uint32 HashLoginParams(const UE::Online::FAuthLogin::Params& Params)
	{
		return HashCombine(GetTypeHash(Params.PlatformUserId),
			HashCombine(GetTypeHash(Params.CredentialsType), GetTypeHash(Params.CredentialsId)));
	}

	/**
	 * @struct FLoginJoinFuncs
	 *
	 * @brief What makes two logins one login, so that the second caller waits on the first.
	 *
	 * The game server is one identity per process, and the plugin does not log it on at all: it waits for
	 * the anonymous logon the API performs when it comes up. Two callers naming it are therefore waiting
	 * for the same confirmation, whatever credentials either of them offered, and the transport asks for
	 * the same identity a game does. Everything else has to name the same user and ask for the same
	 * credentials; the token is not compared, because a TVariant has no equality to compare it with.
	 */
	struct FLoginJoinFuncs
	{
		static bool NamesGameServer(const UE::Online::FAuthLogin::Params& Params)
		{
			return Params.CredentialsType == LoginCredentialsType::AnonymousGameServer
				|| Params.PlatformUserId == Steam::SteamGameServerPlatformId;
		}

		static bool Compare(const UE::Online::FAuthLogin::Params& First, const UE::Online::FAuthLogin::Params& Second)
		{
			if (NamesGameServer(First) || NamesGameServer(Second))
			{
				return NamesGameServer(First) && NamesGameServer(Second);
			}

			return First.PlatformUserId == Second.PlatformUserId
				&& First.CredentialsType == Second.CredentialsType
				&& First.CredentialsId == Second.CredentialsId;
		}

		static uint32 GetTypeHash(const UE::Online::FAuthLogin::Params& Params)
		{
			return NamesGameServer(Params) ? HashGameServerLogin() : HashLoginParams(Params);
		}
	};

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthLogin> FAuthSteam::Login(UE::Online::FAuthLogin::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FAuthLogin, FLoginJoinFuncs>(MoveTemp(InParams));

		if (Op->IsReady())
		{
			return Op->GetHandle();
		}

		// Step 1: Work out which identity is being logged in and prepare its account info.
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthLogin>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();
			const bool bGameServerLogin = IsGameServerLogin(Params);

			// The game server is one identity per process, so it always occupies the same platform user.
			const FPlatformUserId PlatformUserId = bGameServerLogin ? Steam::SteamGameServerPlatformId : Params.PlatformUserId;
			if (!PlatformUserId.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::Login] Failed: No platform user id provided."));
				InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
				return;
			}

			auto AccountInfoSteam = AccountInfoRegistrySteam.Find(PlatformUserId);
			if (AccountInfoSteam.IsValid() && UE::Online::IsOnlineStatus(AccountInfoSteam->LoginStatus))
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAuthSteam::Login] Failed: Already logged in as [%s]"), *ToLogString(AccountInfoSteam->AccountId));
				InAsyncOp.SetError(UE::Online::Errors::Auth::AlreadyLoggedIn());
				return;
			}

			if (!AccountInfoSteam.IsValid())
			{
				AccountInfoSteam = MakeShared<FAccountInfoSteam>();
				AccountInfoSteam->PlatformUserId = PlatformUserId;
				AccountInfoSteam->LoginStatus = UE::Online::ELoginStatus::NotLoggedIn;
			}

			AccountInfoSteam->bIsServer = bGameServerLogin;

			InAsyncOp.Data.Set<TSharedRef<FAccountInfoSteam>>(AccountInfoKey, AccountInfoSteam.ToSharedRef());
		})
		// Step 2: Wait for the game server to be logged on, if this login is about one.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthLogin>& InAsyncOp)
		{
			const auto& AccountInfoSteam = GetOpDataChecked<TSharedRef<FAccountInfoSteam>>(InAsyncOp, AccountInfoKey);

			// A client has nothing to wait for. A server does: the anonymous logon is asked for when the API
			// comes up and confirmed by a callback, so failing here on BLoggedOn made a server which logged
			// in at the ordinary moment fail for being early.
			if (!AccountInfoSteam->bIsServer)
			{
				return MakeFulfilledPromise<Steam::TSteamResult<Steam::Wrappers::FSteamGameServerLogOn>>(
					Steam::Wrappers::FSteamGameServerLogOn::Result { }).GetFuture();
			}

			return SteamListen<Steam::Wrappers::FSteamGameServerLogOn>({ });
		})
		// Step 3: Adopt the identity of the API this login belongs to.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthLogin>& InAsyncOp,
			Steam::TSteamResult<Steam::Wrappers::FSteamGameServerLogOn>&& LogOnResult)
		{
			const auto& AccountInfoSteam = GetOpDataChecked<TSharedRef<FAccountInfoSteam>>(InAsyncOp, AccountInfoKey);

			if (AccountInfoSteam->bIsServer)
			{
				if (LogOnResult.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::Login] Failed: The game server is not logged on to Steam: %s"),
						*LogOnResult.GetErrorValue().GetLogString());

					InAsyncOp.SetError(MoveTemp(LogOnResult.GetErrorValue()));
					return;
				}

				AccountInfoSteam->SteamAccountId = LogOnResult.GetOkValue().ServerUserId;
				return;
			}

			auto User = Steam::GetSteamInterface<ISteamUser>();
			if (User == nullptr)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::Login] Failed: This instance runs no Steam client API"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());
				return;
			}

			if (!User->BLoggedOn())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::Login] Failed: The Steam user is not logged on"));
				InAsyncOp.SetError(UE::Online::Errors::NotLoggedIn());
				return;
			}

			AccountInfoSteam->SteamAccountId = User->GetSteamID();
		})
		// Step 3: Ask Steam for the avatar of a user. A game server has no persona to fetch.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthLogin>& InAsyncOp) -> TFuture<Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>>
		{
			const auto& AccountInfoSteam = GetOpDataChecked<TSharedRef<FAccountInfoSteam>>(InAsyncOp, AccountInfoKey);

			if (AccountInfoSteam->bIsServer)
			{
				return MakeFulfilledPromise<Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>>(
					Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>(UE::Online::Errors::NotFound())).GetFuture();
			}

			return SteamListen<Steam::Wrappers::FSteamUserAvatar>({ AccountInfoSteam->SteamAccountId, DefaultAvatarSize });
		})
		// Step 4: Read the display name and the avatar image. Both come from the Steam API, so this step
		// stays on the game thread and touches no disk.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthLogin>& InAsyncOp, Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>&& AvatarResult) -> FSaveUserAvatar::Params
		{
			const auto& AccountInfoSteam = GetOpDataChecked<TSharedRef<FAccountInfoSteam>>(InAsyncOp, AccountInfoKey);

			if (AccountInfoSteam->bIsServer)
			{
				return FSaveUserAvatar::Params { };
			}

			if (ISteamFriends* Friends = Steam::GetSteamInterface<ISteamFriends>())
			{
				// Fetch display name
				FString DisplayName = StringCast<TCHAR>(Friends->GetPersonaName()).Get();
				AccountInfoSteam->Attributes.Emplace(UE::Online::AccountAttributeData::DisplayName, MoveTemp(DisplayName));
			}
			else
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::Login] Failed: Steam friends interface is not available"));
				InAsyncOp.SetError(UE::Online::Errors::MissingInterface());

				return FSaveUserAvatar::Params { };
			}

			// The avatar is cosmetic, so a user who has none, or whose avatar is still downloading when the
			// request times out, still logs in.
			if (AvatarResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAuthSteam::Login] No avatar available. User [%s], Result [%s]"),
					*ToLogString(AccountInfoSteam->SteamAccountId), *AvatarResult.GetErrorValue().GetLogString());

				return FSaveUserAvatar::Params { };
			}

			const auto& Avatar = AvatarResult.GetOkValue();

			auto ReadResult = FUserInfoUtils::ReadAvatarImage({
				.SteamId = Avatar.UserId.ConvertToUint64(),
				.AvatarSize = DefaultAvatarSize,
				.ImageIndex = Avatar.ImageIndex,
				.ImageWidth = Avatar.ImageWidth,
				.ImageHeight = Avatar.ImageHeight });

			if (ReadResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::Login] Failed to read the avatar image. User [%s], Result [%s]"),
					*ToLogString(AccountInfoSteam->SteamAccountId), *ReadResult.GetErrorValue().GetLogString());

				return FSaveUserAvatar::Params { };
			}

			return FSaveUserAvatar::Params { .Image = MoveTemp(ReadResult.GetOkValue().Image) };
		})
		// Step 5: Encoding a PNG and writing it out touch no Steam API, so a login does not wait on the disk.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthLogin>& /*InAsyncOp*/, FSaveUserAvatar::Params&& SaveParams)
		{
			if (SaveParams.Image.RawImage.IsEmpty())
			{
				return UE::Online::TDefaultErrorResult<FSaveUserAvatar>(UE::Online::Errors::NotFound());
			}

			return FUserInfoUtils::SaveAvatarImageToFile(MoveTemp(SaveParams));
		}, UE::Online::FOnlineAsyncExecutionPolicy::RunOnThreadPool())
		// Step 6: Bookkeeping and notifications, back on the game thread where the account info lives.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthLogin>& InAsyncOp, UE::Online::TDefaultErrorResult<FSaveUserAvatar>&& SaveResult)
		{
			const auto& AccountInfoSteam = GetOpDataChecked<TSharedRef<FAccountInfoSteam>>(InAsyncOp, AccountInfoKey);

			if (SaveResult.IsOk())
			{
				AccountInfoSteam->Attributes.Emplace(AccountAttributeData::AvatarUrl, SaveResult.GetOkValue().AvatarUrl);
			}
			else if (!AccountInfoSteam->bIsServer)
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAuthSteam::Login] No avatar cached. User [%s], Result [%s]"),
					*ToLogString(AccountInfoSteam->SteamAccountId), *SaveResult.GetErrorValue().GetLogString());
			}

			AccountInfoSteam->AccountId = CreateAccountId(AccountInfoSteam->SteamAccountId);
			AccountInfoSteam->LoginStatus = UE::Online::ELoginStatus::LoggedIn;

			AccountInfoRegistrySteam.Register(AccountInfoSteam);

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FAuthSteam::Login] Successfully logged in as [%s]%s"),
				*ToLogString(AccountInfoSteam->AccountId), AccountInfoSteam->bIsServer ? TEXT(" (game server)") : TEXT(""));

			OnAuthLoginStatusChangedEvent.Broadcast(UE::Online::FAuthLoginStatusChanged { .AccountInfo = AccountInfoSteam, .LoginStatus = AccountInfoSteam->LoginStatus });

			InAsyncOp.SetResult({ AccountInfoSteam });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthLogout> FAuthSteam::Logout(UE::Online::FAuthLogout::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FAuthLogout>(MoveTemp(InParams));

		// Step 1: Setup operation data.
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthLogout>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			if (!Params.LocalAccountId.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::Logout] Failed: No account id provided."));
				InAsyncOp.SetError(UE::Online::Errors::InvalidUser());
				return;
			}

			const auto AccountInfoSteam = AccountInfoRegistrySteam.Find(Params.LocalAccountId);
			if (!AccountInfoSteam.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::Logout] Failed: Associated steam account info not found."));
				InAsyncOp.SetError(UE::Online::Errors::NotFound());
				return;
			}

			if (!UE::Online::IsOnlineStatus(AccountInfoSteam->LoginStatus))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::Logout] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
				InAsyncOp.SetError(UE::Online::Errors::NotLoggedIn());
				return;
			}

			InAsyncOp.Data.Set<TSharedRef<FAccountInfoSteam>>(AccountInfoKey, AccountInfoSteam.ToSharedRef());
		})
		// Step 2: bookkeeping and notifications.
		//
		// Steam itself is not logged out of: the client API belongs to the Steam user running the process and
		// the game server API is logged on for as long as the server lives. What ends here is this game's use
		// of that identity, which is what bDestroyAuth would otherwise ask to forget.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthLogout>& InAsyncOp)
		{
			const auto& AccountInfoSteam = GetOpDataChecked<TSharedRef<FAccountInfoSteam>>(InAsyncOp, AccountInfoKey);

			// Steam keeps a ticket good and a session open until it is told otherwise, and both were asked
			// for as the identity that is going away: a ticket comes from the client API, a session is
			// opened by whichever API this process verifies through. Left behind, they outlive the login
			// they belong to, and the tickets stay usable by whoever was given one.
			if (AccountInfoSteam->bIsServer)
			{
				for (const auto& VerifiedSession : VerifiedAuthSessions)
				{
					SteamCallSync<Steam::Wrappers::FSteamEndAuthSession>({ .RemoteUserId = VerifiedSession.Value });
				}

				VerifiedAuthSessions.Empty();
			}
			else
			{
				for (const auto& IssuedTicket : IssuedAuthTickets)
				{
					SteamCallSync<Steam::Wrappers::FSteamCancelAuthTicket>({ .TicketHandle = IssuedTicket.Value->TicketHandle });
				}

				IssuedAuthTickets.Empty();
			}

			AccountInfoSteam->LoginStatus = UE::Online::ELoginStatus::NotLoggedIn;

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FAuthSteam::Logout] Successfully logged out [%s]"), *ToLogString(AccountInfoSteam->AccountId));

			OnAuthLoginStatusChangedEvent.Broadcast(UE::Online::FAuthLoginStatusChanged { .AccountInfo = AccountInfoSteam, .LoginStatus = AccountInfoSteam->LoginStatus });
			AccountInfoRegistrySteam.Unregister(AccountInfoSteam->AccountId);

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	TFuture<UE::Online::TDefaultErrorResultInternal<TSharedRef<Steam::FSteamAuthTicketData>>> FAuthSteam::IssueAuthTicket(const SteamNetworkingIdentity& Target)
	{
		using FIssuedTicketResult = UE::Online::TDefaultErrorResultInternal<TSharedRef<Steam::FSteamAuthTicketData>>;

		if (Config.bVerifyAuthTicketsOnGameServer)
		{
			// A web api ticket names its audience by a string from the configuration instead, so the target
			// only reaches the session ticket.
			return SteamListen<Steam::Wrappers::FSteamAuthSessionTicket>({ .Target = Target })
			.Next([](Steam::TSteamResult<Steam::Wrappers::FSteamAuthSessionTicket>&& Result)
			{
				if (Result.IsError())
				{
					return FIssuedTicketResult(MoveTemp(Result.GetErrorValue()));
				}

				return FIssuedTicketResult(Result.GetOkValue().Ticket.ToSharedRef());
			});
		}

		return SteamListen<Steam::Wrappers::FSteamAuthTicketForWebApi>({ Config.WebApiIdentity })
		.Next([](Steam::TSteamResult<Steam::Wrappers::FSteamAuthTicketForWebApi>&& Result)
		{
			if (Result.IsError())
			{
				return FIssuedTicketResult(MoveTemp(Result.GetErrorValue()));
			}

			return FIssuedTicketResult(Result.GetOkValue().Ticket.ToSharedRef());
		});
	}

	FName FAuthSteam::GetIssuedAuthTicketType() const
	{
		return Config.bVerifyAuthTicketsOnGameServer ? UE::Online::ExternalLoginType::SteamSessionTicket : ExternalLoginType::SteamWebApiTicket;
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthQueryVerifiedAuthTicket> FAuthSteam::RequestBoundAuthTicket(
		const UE::Online::IAuthPtr& Auth, UE::Online::FAuthQueryVerifiedAuthTicket::Params Params, const Steam::FSteamNetAddress& Peer)
	{
		const auto AuthSteam = StaticCastSharedPtr<FAuthSteam>(Auth);

		return AuthSteam->QueryVerifiedAuthTicket(MoveTemp(Params), Peer);
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthQueryVerifiedAuthTicket> FAuthSteam::QueryVerifiedAuthTicket(UE::Online::FAuthQueryVerifiedAuthTicket::Params&& InParams)
	{
		// Nobody named a host, so the ticket is bound to none and is good wherever it is shown.
		SteamNetworkingIdentity Anybody;
		Anybody.Clear();

		return QueryVerifiedAuthTicket(MoveTemp(InParams), Anybody);
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthQueryVerifiedAuthTicket> FAuthSteam::QueryVerifiedAuthTicket(
		UE::Online::FAuthQueryVerifiedAuthTicket::Params&& InParams, const SteamNetworkingIdentity& Target)
	{
		const auto Op = GetOp<UE::Online::FAuthQueryVerifiedAuthTicket>(MoveTemp(InParams));

		// Step 1: Only a logged in local user can prove who they are.
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthQueryVerifiedAuthTicket>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			const auto AccountInfoSteam = AccountInfoRegistrySteam.Find(Params.LocalAccountId);
			if (!AccountInfoSteam.IsValid() || !UE::Online::IsOnlineStatus(AccountInfoSteam->LoginStatus))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::QueryVerifiedAuthTicket] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
				InAsyncOp.SetError(UE::Online::Errors::NotLoggedIn());
			}
		})
		// Step 2: Ask Steam for a ticket of the kind the config asks for, for the host it is meant for.
		.Then([this, Target](UE::Online::TOnlineAsyncOp<UE::Online::FAuthQueryVerifiedAuthTicket>& /*InAsyncOp*/)
		{
			return IssueAuthTicket(Target);
		})
		// Step 3: Remember the ticket so that it can be withdrawn, and hand it to the caller.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthQueryVerifiedAuthTicket>& InAsyncOp,
			UE::Online::TDefaultErrorResultInternal<TSharedRef<Steam::FSteamAuthTicketData>>&& TicketResult)
		{
			const auto& Params = InAsyncOp.GetParams();

			if (TicketResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::QueryVerifiedAuthTicket] Failed: User [%s], Audience [%s], Result [%s]"),
					*ToLogString(Params.LocalAccountId), LexToString(Params.Audience), *TicketResult.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(TicketResult.GetErrorValue()));
				return;
			}

			const auto& Ticket = TicketResult.GetOkValue();

			const UE::Online::FVerifiedAuthTicketId TicketId { UE::Online::EOnlineServices::Steam, NextAuthTicketIndex++ };
			IssuedAuthTickets.Emplace(TicketId, Ticket);

			UE::Online::FAuthQueryVerifiedAuthTicket::Result Result;
			Result.VerifiedAuthTicketId = TicketId;
			Result.VerifiedAuthTicket.Type = GetIssuedAuthTicketType();
			Result.VerifiedAuthTicket.Data = BytesToHex(Ticket->TicketBytes.GetData(), Ticket->TicketBytes.Num());

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAuthSteam::QueryVerifiedAuthTicket] Succeeded: User [%s], Type [%s], Size [%d]"),
				*ToLogString(Params.LocalAccountId), *Result.VerifiedAuthTicket.Type.ToString(), Ticket->TicketBytes.Num());

			InAsyncOp.SetResult(MoveTemp(Result));
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthCancelVerifiedAuthTicket> FAuthSteam::CancelVerifiedAuthTicket(UE::Online::FAuthCancelVerifiedAuthTicket::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FAuthCancelVerifiedAuthTicket>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthCancelVerifiedAuthTicket>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			auto Ticket = IssuedAuthTickets.Find(Params.VerifiedAuthTicketId);
			if (Ticket == nullptr)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::CancelVerifiedAuthTicket] Failed: Unknown ticket [%s]"), *ToLogString(Params.VerifiedAuthTicketId));
				InAsyncOp.SetError(UE::Online::Errors::NotFound());
				return;
			}

			// Every host holding this ticket is told to drop the session.
			Steam::TSteamResult<Steam::Wrappers::FSteamCancelAuthTicket> CancelResult =
				SteamCallSync<Steam::Wrappers::FSteamCancelAuthTicket>({ (*Ticket)->TicketHandle });

			IssuedAuthTickets.Remove(Params.VerifiedAuthTicketId);

			if (CancelResult.IsError())
			{
				InAsyncOp.SetError(MoveTemp(CancelResult.GetErrorValue()));
				return;
			}

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthQueryExternalAuthToken> FAuthSteam::QueryExternalAuthToken(UE::Online::FAuthQueryExternalAuthToken::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FAuthQueryExternalAuthToken>(MoveTemp(InParams));

		// Step 1: Only a logged in local user has a token to give.
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthQueryExternalAuthToken>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			const auto AccountInfoSteam = AccountInfoRegistrySteam.Find(Params.LocalAccountId);
			if (!AccountInfoSteam.IsValid() || !UE::Online::IsOnlineStatus(AccountInfoSteam->LoginStatus))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::QueryExternalAuthToken] Failed: User is not logged in. User [%s]"), *ToLogString(Params.LocalAccountId));
				InAsyncOp.SetError(UE::Online::Errors::NotLoggedIn());
			}
		})
		// Step 2: A token for somebody else than a host is always the Web API one; the relying party names
		// the identity it is bound to, falling back to the configured one.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthQueryExternalAuthToken>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			return SteamListen<Steam::Wrappers::FSteamAuthTicketForWebApi>({ Params.RelyingParty.IsEmpty() ? Config.WebApiIdentity : Params.RelyingParty });
		})
		.Then(Steam::Unwrap<Steam::Wrappers::FSteamAuthTicketForWebApi>(TEXT("FAuthSteam::QueryExternalAuthToken"),
			[this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthQueryExternalAuthToken>& InAsyncOp, Steam::Wrappers::FSteamAuthTicketForWebApi::Result&& TicketResult)
		{
			const auto& Ticket = TicketResult.Ticket;

			UE::Online::FAuthQueryExternalAuthToken::Result Result;
			Result.ExternalAuthToken.Type = ExternalLoginType::SteamWebApiTicket;
			Result.ExternalAuthToken.Data = BytesToHex(Ticket->TicketBytes.GetData(), Ticket->TicketBytes.Num());

			InAsyncOp.SetResult(MoveTemp(Result));
		}))
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthBeginVerifiedAuthSession> FAuthSteam::BeginVerifiedAuthSession(UE::Online::FAuthBeginVerifiedAuthSession::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FAuthBeginVerifiedAuthSession>(MoveTemp(InParams));

		// Step 1: Check that this host is the one meant to verify tickets at all.
		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthBeginVerifiedAuthSession>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			if (!Config.bVerifyAuthTicketsOnGameServer)
			{
				// The tickets clients issue in this configuration are bound to a Web API identity and can only
				// be verified by the backend holding it, so accepting one here would verify nothing.
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::BeginVerifiedAuthSession] Failed: Auth tickets are configured to be verified by a backend"));
				InAsyncOp.SetError(UE::Online::Errors::NotConfigured());
				return;
			}

			if (Params.Ticket.Type != UE::Online::ExternalLoginType::SteamSessionTicket)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::BeginVerifiedAuthSession] Failed: Unexpected ticket type [%s]"), *Params.Ticket.Type.ToString());
				InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
				return;
			}

			if (!GetSteamUserId(Params.RemoteAccountId).IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::BeginVerifiedAuthSession] Failed: No associated steam id found. User [%s]"), *ToLogString(Params.RemoteAccountId));
				InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
				return;
			}

			if (!IsWellFormedTicket(Params.Ticket.Data))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::BeginVerifiedAuthSession] Failed: The ticket is not an even run of hexadecimal digits of at most %d bytes. Length [%d]"),
					Steam::MaxAuthSessionTicketSize, Params.Ticket.Data.Len());

				InAsyncOp.SetError(UE::Online::Errors::InvalidParams());
			}
		})
		// Step 2: Hand the ticket to Steam and wait for the verdict.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthBeginVerifiedAuthSession>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			TArray<uint8> TicketBytes;
			TicketBytes.SetNumUninitialized(Params.Ticket.Data.Len() / 2);
			HexToBytes(Params.Ticket.Data, TicketBytes.GetData());

			return SteamListen<Steam::Wrappers::FSteamBeginAuthSession>({ .RemoteUserId = GetSteamUserId(Params.RemoteAccountId), .TicketBytes = MoveTemp(TicketBytes) });
		})
		// Step 3: Track the user for as long as the session lasts.
		//
		// Written out rather than wrapped in Steam::Unwrap, because this step has something to undo when it
		// fails: a request Steam accepted and then never answered leaves a session open on its side, and
		// Steam answers the next BeginAuthSession for that user with DuplicateRequest until it is closed.
		// A refused verdict closes itself inside the wrapper, where it is known that a session was opened;
		// a request that timed out or was cancelled is only visible here.
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthBeginVerifiedAuthSession>& InAsyncOp,
			Steam::TSteamResult<Steam::Wrappers::FSteamBeginAuthSession>&& SessionResultOrError)
		{
			const auto& Params = InAsyncOp.GetParams();

			if (SessionResultOrError.IsError())
			{
				UE::Online::FOnlineError Error = MoveTemp(SessionResultOrError.GetErrorValue());

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::BeginVerifiedAuthSession] Failed: User [%s], Result [%s]"),
					*ToLogString(Params.RemoteAccountId), *Error.GetLogString());

				if (Error == UE::Online::Errors::ErrorCode::Common::Timeout || Error == UE::Online::Errors::ErrorCode::Common::Cancelled)
				{
					SteamCallSync<Steam::Wrappers::FSteamEndAuthSession>({ .RemoteUserId = GetSteamUserId(Params.RemoteAccountId) });
				}

				InAsyncOp.SetError(MoveTemp(Error));
				return;
			}

			auto SessionResult = MoveTemp(SessionResultOrError.GetOkValue());

			const UE::Online::FVerifiedAuthSessionId SessionId { UE::Online::EOnlineServices::Steam, NextAuthSessionIndex++ };
			VerifiedAuthSessions.Emplace(SessionId, SessionResult.RemoteUserId);

			UE_LOG(LogOnlineServicesSteam, Log, TEXT("[FAuthSteam::BeginVerifiedAuthSession] Verified [%s]%s"),
				*ToLogString(Params.RemoteAccountId),
				SessionResult.OwnerUserId.IsValid() && SessionResult.OwnerUserId != SessionResult.RemoteUserId ? TEXT(" (family sharing)") : TEXT(""));

			InAsyncOp.SetResult({ UE::Online::FVerifiedAuthSession { .SessionId = SessionId, .RemoteAccountId = Params.RemoteAccountId, .CreationTime = FPlatformTime::Seconds() } });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthEndVerifiedAuthSession> FAuthSteam::EndVerifiedAuthSession(UE::Online::FAuthEndVerifiedAuthSession::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FAuthEndVerifiedAuthSession>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FAuthEndVerifiedAuthSession>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			const auto RemoteUserId = VerifiedAuthSessions.Find(Params.SessionId);
			if (RemoteUserId == nullptr)
			{
				// Expected rather than wrong: a revoked verdict closes the session where it arrives, and the
				// game is told nothing, so it ends a session that is already gone on every ordinary logout.
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAuthSteam::EndVerifiedAuthSession] Failed: Unknown session [%s]"), *ToLogString(Params.SessionId));
				InAsyncOp.SetError(UE::Online::Errors::NotFound());
				return;
			}

			Steam::TSteamResult<Steam::Wrappers::FSteamEndAuthSession> EndResult =
				SteamCallSync<Steam::Wrappers::FSteamEndAuthSession>({ *RemoteUserId });

			VerifiedAuthSessions.Remove(Params.SessionId);

			if (EndResult.IsError())
			{
				InAsyncOp.SetError(MoveTemp(EndResult.GetErrorValue()));
				return;
			}

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	void FAuthSteam::OnPersonaStateChange(PersonaStateChange_t* Message)
	{
		if (Message == nullptr)
		{
			return;
		}

		const auto AccountInfoSteam = AccountInfoRegistrySteam.Find(Message->m_ulSteamID);
		if (!AccountInfoSteam.IsValid() || AccountInfoSteam->bIsServer || !UE::Online::IsOnlineStatus(AccountInfoSteam->LoginStatus))
		{
			// The change belongs to somebody else: friends and lobby members are the user info component's business.
			return;
		}

		const bool bRefreshDisplayName = (Message->m_nChangeFlags & k_EPersonaChangeName) != 0;
		const bool bRefreshAvatar = (Message->m_nChangeFlags & k_EPersonaChangeAvatar) != 0;

		if (!bRefreshDisplayName && !bRefreshAvatar)
		{
			return;
		}

		RefreshAccountAttributesImplOp({ .LocalAccountId = AccountInfoSteam->AccountId, .bRefreshDisplayName = bRefreshDisplayName, .bRefreshAvatar = bRefreshAvatar })
		.OnComplete([](const UE::Online::TOnlineResult<FAuthRefreshAccountAttributesImpl>& Result)
		{
			if (Result.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::OnPersonaStateChange] Failed to refresh the account attributes: %s"),
					*Result.GetErrorValue().GetLogString());
			}
		});
	}

	void FAuthSteam::OnAuthSessionVerdict(ValidateAuthTicketResponse_t* Message)
	{
		if (Message == nullptr || Message->m_eAuthSessionResponse == k_EAuthSessionResponseOK)
		{
			return;
		}

		// Only a session this component granted is its business. A refusal of a verification still in flight
		// belongs to the request which asked for it: that one is still listening, and the session does not
		// reach the map below until it has been granted.
		const auto RevokedSession = Algo::FindByPredicate(VerifiedAuthSessions,
			[Message](const TPair<UE::Online::FVerifiedAuthSessionId, CSteamID>& Session)
			{
				return Session.Value == Message->m_SteamID;
			});

		if (RevokedSession == nullptr)
		{
			return;
		}

		UE_LOG(LogOnlineServicesSteam, Warning,
			TEXT("[FAuthSteam::OnAuthSessionVerdict] The verification of [%s] was withdrawn: %s. The session is closed here; whoever is hosting decides what to do about the player"),
			*ToLogString(RevokedSession->Value),
			*Steam::Wrappers::FSteamBeginAuthSession::TranslateAuthSessionResponse(Message->m_eAuthSessionResponse).GetLogString());

		SteamCallSync<Steam::Wrappers::FSteamEndAuthSession>({ .RemoteUserId = RevokedSession->Value });
		VerifiedAuthSessions.Remove(RevokedSession->Key);
	}

	UE::Online::TOnlineAsyncOpHandle<FAuthRefreshAccountAttributesImpl> FAuthSteam::RefreshAccountAttributesImplOp(FAuthRefreshAccountAttributesImpl::Params&& InParams)
	{
		const auto Op = GetOp<FAuthRefreshAccountAttributesImpl>(MoveTemp(InParams));

		// Step 1: Read the new display name and, when the avatar changed, ask Steam for the new image.
		Op->Then([this](UE::Online::TOnlineAsyncOp<FAuthRefreshAccountAttributesImpl>& InAsyncOp) -> TFuture<Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>>
		{
			const auto& Params = InAsyncOp.GetParams();

			const auto AccountInfoSteam = AccountInfoRegistrySteam.Find(Params.LocalAccountId);
			if (!AccountInfoSteam.IsValid() || !UE::Online::IsOnlineStatus(AccountInfoSteam->LoginStatus))
			{
				// The user logged out while the change was on its way.
				InAsyncOp.SetError(UE::Online::Errors::NotLoggedIn());

				return MakeFulfilledPromise<Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>>(
					Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>(UE::Online::Errors::NotLoggedIn())).GetFuture();
			}

			InAsyncOp.Data.Set<TSharedRef<FAccountInfoSteam>>(AccountInfoKey, AccountInfoSteam.ToSharedRef());

			if (Params.bRefreshDisplayName)
			{
				if (ISteamFriends* Friends = Steam::GetSteamInterface<ISteamFriends>())
				{
					InAsyncOp.Data.Set<FString>(DisplayNameKey, StringCast<TCHAR>(Friends->GetPersonaName()).Get());
				}
			}

			if (!Params.bRefreshAvatar)
			{
				return MakeFulfilledPromise<Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>>(
					Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>(UE::Online::Errors::NotFound())).GetFuture();
			}

			return SteamListen<Steam::Wrappers::FSteamUserAvatar>({ AccountInfoSteam->SteamAccountId, DefaultAvatarSize });
		})
		// Step 2: Copy the new image out of the Steam cache, still on the game thread.
		.Then([this](UE::Online::TOnlineAsyncOp<FAuthRefreshAccountAttributesImpl>& InAsyncOp, Steam::TSteamResult<Steam::Wrappers::FSteamUserAvatar>&& AvatarResult) -> FSaveUserAvatar::Params
		{
			if (AvatarResult.IsError())
			{
				return FSaveUserAvatar::Params { };
			}

			const auto& Avatar = AvatarResult.GetOkValue();

			auto ReadResult = FUserInfoUtils::ReadAvatarImage({
				.SteamId = Avatar.UserId.ConvertToUint64(),
				.AvatarSize = DefaultAvatarSize,
				.ImageIndex = Avatar.ImageIndex,
				.ImageWidth = Avatar.ImageWidth,
				.ImageHeight = Avatar.ImageHeight });

			if (ReadResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FAuthSteam::RefreshAccountAttributes] Failed to read the avatar image: %s"),
					*ReadResult.GetErrorValue().GetLogString());

				return FSaveUserAvatar::Params { };
			}

			return FSaveUserAvatar::Params { .Image = MoveTemp(ReadResult.GetOkValue().Image) };
		})
		// Step 3: Write the new avatar off the game thread.
		.Then([this](UE::Online::TOnlineAsyncOp<FAuthRefreshAccountAttributesImpl>& /*InAsyncOp*/, FSaveUserAvatar::Params&& SaveParams)
		{
			if (SaveParams.Image.RawImage.IsEmpty())
			{
				return UE::Online::TDefaultErrorResult<FSaveUserAvatar>(UE::Online::Errors::NotFound());
			}

			return FUserInfoUtils::SaveAvatarImageToFile(MoveTemp(SaveParams));
		}, UE::Online::FOnlineAsyncExecutionPolicy::RunOnThreadPool())
		// Step 4: Publish what actually changed.
		.Then([this](UE::Online::TOnlineAsyncOp<FAuthRefreshAccountAttributesImpl>& InAsyncOp, UE::Online::TDefaultErrorResult<FSaveUserAvatar>&& SaveResult)
		{
			const auto& AccountInfoSteam = GetOpDataChecked<TSharedRef<FAccountInfoSteam>>(InAsyncOp, AccountInfoKey);

			UE::Online::FAuthAccountAttributesChanged AttributesChanged { AccountInfoSteam };

			const auto ApplyAttribute = [&AccountInfoSteam, &AttributesChanged](const UE::Online::FSchemaAttributeId& AttributeId, UE::Online::FSchemaVariant&& NewValue)
			{
				if (UE::Online::FSchemaVariant* ExistingValue = AccountInfoSteam->Attributes.Find(AttributeId))
				{
					if (*ExistingValue == NewValue)
					{
						return;
					}

					AttributesChanged.ChangedAttributes.Emplace(AttributeId, TPair<UE::Online::FSchemaVariant, UE::Online::FSchemaVariant>(*ExistingValue, NewValue));
					*ExistingValue = MoveTemp(NewValue);
				}
				else
				{
					AttributesChanged.AddedAttributes.Emplace(AttributeId, NewValue);
					AccountInfoSteam->Attributes.Emplace(AttributeId, MoveTemp(NewValue));
				}
			};

			if (const FString* DisplayName = InAsyncOp.Data.Get<FString>(DisplayNameKey))
			{
				ApplyAttribute(UE::Online::AccountAttributeData::DisplayName, UE::Online::FSchemaVariant(*DisplayName));
			}

			if (SaveResult.IsOk())
			{
				ApplyAttribute(AccountAttributeData::AvatarUrl, UE::Online::FSchemaVariant(SaveResult.GetOkValue().AvatarUrl));
			}

			if (!AttributesChanged.AddedAttributes.IsEmpty() || !AttributesChanged.ChangedAttributes.IsEmpty())
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FAuthSteam::RefreshAccountAttributes] Refreshed [%s]: %d added, %d changed"),
					*ToLogString(AccountInfoSteam->AccountId), AttributesChanged.AddedAttributes.Num(), AttributesChanged.ChangedAttributes.Num());

				OnAuthAccountAttributesChangedEvent.Broadcast(AttributesChanged);
			}

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}
}
