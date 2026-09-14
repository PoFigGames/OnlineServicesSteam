// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/AuthCommon.h"
#include "Online/OnlineComponentSteam.h"
#include "Steam/Wrappers/SteamUserAuth.h"
#include "SteamInterfaces.h"
#include "SteamUtils.h"


namespace PoFigGames::Online {
	inline const FString ACCOUNT_INFO_KEY_NAME { TEXT("AccountInfoSteam") };

	namespace LoginCredentialsType
	{
		/**
		 * Anonymous login of the game server itself. There is no user behind it: the account is the Steam
		 * identity which the game server API logged on anonymously while it was initializing.
		 *
		 * Only a process running both APIs has to ask for this explicitly. On a dedicated server, where there
		 * is no client API to log a user into, a login which names no credentials resolves to it.
		 */
		const FName AnonymousGameServer = TEXT("SteamAnonymousGameServer");
	}

	namespace ExternalLoginType
	{
		/**
		 * Ticket bound to a Web API identity, verified by a backend through ISteamUserAuth/AuthenticateUserTicket.
		 * Told apart from UE::Online::ExternalLoginType::SteamSessionTicket, which a host verifies itself.
		 */
		const FName SteamWebApiTicket = TEXT("SteamWebApiTicket");
	}

	/**
	 * @struct FAuthSteamConfig
	 *
	 * @brief Loaded from the [OnlineServices.Steam.Auth] config section.
	 */
	struct FAuthSteamConfig
	{
		/**
		 * Who checks the ticket a client issues for a host.
		 *
		 * True, the default, issues a session ticket which the game server verifies itself through
		 * BeginVerifiedAuthSession. False issues a ticket bound to WebApiIdentity instead, which only a
		 * backend can verify; a game server asked to verify such a ticket refuses, because it cannot.
		 */
		bool bVerifyAuthTicketsOnGameServer { true };

		/**
		 * Identity a Web API ticket is bound to. The backend has to pass the same string to Steam, so an
		 * empty value means the ticket is bound to nothing and any backend can spend it.
		 */
		FString WebApiIdentity { };
	};

	namespace AccountAttributeData
	{
		/**
		 * Local file the user's avatar was cached to. UCommonUserSubsystem::GetLocalUserAvatarUrl looks this
		 * attribute up by the same name, so it cannot be renamed on one side alone.
		 */
		const UE::Online::FSchemaAttributeId AvatarUrl = TEXT("AvatarUrl");
	}

	/**
	 * @struct FAuthRefreshAccountAttributesImpl
	 *
	 * @brief Internal operation bringing the display name and the avatar of a logged in user back in sync after the
	 * user changed them on Steam.
	 */
	struct FAuthRefreshAccountAttributesImpl
	{
		static constexpr TCHAR Name[] = TEXT("RefreshAccountAttributesImpl");

		/** Input struct for FAuthSteam::RefreshAccountAttributesImplOp */
		struct Params
		{
			/** The local user whose persona changed. */
			UE::Online::FAccountId LocalAccountId { };

			bool bRefreshDisplayName { false };
			bool bRefreshAvatar { false };
		};

		/** Output struct for FAuthSteam::RefreshAccountAttributesImplOp */
		struct Result
		{
		};
	};

	/**
	 * @struct FAccountInfoSteam
	 *
	 * @brief Account info of one local Steam identity.
	 */
	struct FAccountInfoSteam final : UE::Online::FAccountInfo
	{
		/** The Steam identity behind this account: a user, or the game server itself. */
		CSteamID SteamAccountId { };

		/** True when this account is the anonymous identity of the game server rather than a user. */
		bool bIsServer { false };
	};

	/**
	 * @class FAccountInfoRegistrySteam
	 *
	 * @brief Local accounts, additionally indexed by the Steam identity behind them.
	 */
	class FAccountInfoRegistrySteam final : public UE::Online::FAccountInfoRegistry
	{
	public:
		using Super = FAccountInfoRegistry;

		using FAccountInfoRegistry::FAccountInfoRegistry;
		virtual ~FAccountInfoRegistrySteam() override = default;

		ONLINESERVICESSTEAM_API TSharedPtr<FAccountInfoSteam> Find(FPlatformUserId PlatformUserId) const;
		ONLINESERVICESSTEAM_API TSharedPtr<FAccountInfoSteam> Find(UE::Online::FAccountId AccountId) const;
		ONLINESERVICESSTEAM_API TSharedPtr<FAccountInfoSteam> Find(const CSteamID& SteamAccountId) const;

		void Register(const TSharedRef<FAccountInfoSteam>&UserAuthData);
		void Unregister(UE::Online::FAccountId AccountId);

	protected:
		virtual void DoRegister(const TSharedRef<UE::Online::FAccountInfo>& AccountInfo) override;
		virtual void DoUnregister(const TSharedRef<UE::Online::FAccountInfo>& AccountInfo) override;

	private:
		TMap<CSteamID, TSharedRef<FAccountInfoSteam>> AuthDataBySteamID { };
	};

	/**
	 * @class FAuthSteam
	 *
	 * @brief Steam authentication.
	 *
	 * Steam has no login of its own to perform: by the time this component runs, the client API has been
	 * initialized against the signed in Steam user and the game server API has logged on anonymously. Login
	 * therefore adopts the identity of whichever API the caller asks for, publishes the data a game needs of
	 * it (display name, cached avatar) and keeps that data in sync for as long as the user stays logged in.
	 */
	class FAuthSteam : public TOnlineComponentSteam<UE::Online::FAuthCommon>
	{
	public:
		using Super = FAuthCommon;

		ONLINESERVICESSTEAM_API explicit FAuthSteam(UE::Online::FOnlineServicesCommon& InServices);

		ONLINESERVICESSTEAM_API virtual void Initialize() override;
		ONLINESERVICESSTEAM_API virtual void UpdateConfig() override;
		ONLINESERVICESSTEAM_API virtual void PreShutdown() override;

		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthLogin>  Login(UE::Online::FAuthLogin::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthLogout> Logout(UE::Online::FAuthLogout::Params&& Params) override;

		/** Issues a ticket which proves this user's identity to a host, in the form the config asks for. */
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthQueryVerifiedAuthTicket> QueryVerifiedAuthTicket(UE::Online::FAuthQueryVerifiedAuthTicket::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthCancelVerifiedAuthTicket> CancelVerifiedAuthTicket(UE::Online::FAuthCancelVerifiedAuthTicket::Params&& Params) override;

		/** Issues a ticket for a backend, whichever way tickets for hosts are verified. */
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthQueryExternalAuthToken> QueryExternalAuthToken(UE::Online::FAuthQueryExternalAuthToken::Params&& Params) override;

		/** Verifies a ticket presented by a remote user. Only available where the host does its own checking. */
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthBeginVerifiedAuthSession> BeginVerifiedAuthSession(UE::Online::FAuthBeginVerifiedAuthSession::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthEndVerifiedAuthSession> EndVerifiedAuthSession(UE::Online::FAuthEndVerifiedAuthSession::Params&& Params) override;

	protected:
		FAccountInfoRegistrySteam AccountInfoRegistrySteam { };
		FAuthSteamConfig          Config { };

		/** Tickets this process issued, kept so that they can be withdrawn again. */
		TMap<UE::Online::FVerifiedAuthTicketId, TSharedRef<Steam::FSteamAuthTicketData>> IssuedAuthTickets { };

		/** Users this host is tracking after their ticket checked out. */
		TMap<UE::Online::FVerifiedAuthSessionId, CSteamID> VerifiedAuthSessions { };

		ONLINESERVICESSTEAM_API virtual const UE::Online::FAccountInfoRegistry& GetAccountInfoRegistry() const override;

		/**
		 * Whether a login asks for the anonymous game server identity rather than for a user.
		 * Named credentials decide; a login which names none is answered by the role of this instance.
		 */
		ONLINESERVICESSTEAM_API bool IsGameServerLogin(const UE::Online::FAuthLogin::Params& Params) const;

		/** Issues a ticket of the kind the config asks for. */
		ONLINESERVICESSTEAM_API TFuture<UE::Online::TDefaultErrorResultInternal<TSharedRef<Steam::FSteamAuthTicketData>>> IssueAuthTicket();

		/** Which kind of ticket QueryVerifiedAuthTicket hands out in the current configuration. */
		ONLINESERVICESSTEAM_API FName GetIssuedAuthTicketType() const;

		/** Re-reads the persona data of a logged in user and publishes what changed. */
		ONLINESERVICESSTEAM_API UE::Online::TOnlineAsyncOpHandle<FAuthRefreshAccountAttributesImpl> RefreshAccountAttributesImplOp(FAuthRefreshAccountAttributesImpl::Params&& Params);

		/** Steam announces every persona change to every listener, so only our own users are of interest. */
		ONLINESERVICESSTEAM_API void OnPersonaStateChange(PersonaStateChange_t* Message);

		/**
		 * A verdict on a ticket this process is already tracking.
		 *
		 * Steam sends one of these when it first looks at a ticket, and again whenever the answer changes
		 * afterwards: the ticket is cancelled by the client, the account is banned, the user leaves Steam.
		 * The request which asked for the verification stops listening once it has its first answer, so
		 * every later verdict arrives here and nowhere else.
		 */
		ONLINESERVICESSTEAM_API void OnAuthSessionVerdict(ValidateAuthTicketResponse_t* Message);

		static ONLINESERVICESSTEAM_API UE::Online::FAccountId CreateAccountId(const CSteamID& SteamAccountId);

	private:
		/** Handles of the two local id spaces above; both start at one, as every online id does. */
		uint32 NextAuthTicketIndex { 1 };
		uint32 NextAuthSessionIndex { 1 };

		/** Registered while the component is alive; see Initialize and PreShutdown. */
		CCallbackManual<FAuthSteam, PersonaStateChange_t> OnPersonaStateChangeCallback { };

		/**
		 * Both pipes are listened to, because which of them carries the verdict is decided by which API
		 * began the session, and that is decided by whether this process runs a game server at all.
		 */
		CCallbackManual<FAuthSteam, ValidateAuthTicketResponse_t> OnClientAuthSessionVerdictCallback { };
		CCallbackManual<FAuthSteam, ValidateAuthTicketResponse_t, true> OnServerAuthSessionVerdictCallback { };
	};
}
