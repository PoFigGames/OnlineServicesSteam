// Copyright PoFig Games Studio. All Rights Reserved.

#include "AuthHandlerSteam.h"

#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Online/Auth.h"
#include "Online/CoreOnline.h"
#include "Online/OnlineAsyncOpHandle.h"
#include "Online/OnlineResult.h"
#include "Online/OnlineServices.h"
#include "OnlineSocketsSteamLogChannels.h"
#include "SteamUtils.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AuthHandlerSteam)

namespace PoFigGames::Online::Private
{
	/**
	 * @enum EAuthMessage
	 *
	 * @brief What one end of the exchange is saying.
	 */
	enum class EAuthMessage : uint8
	{
		None = 0,
		Ticket,
		Verdict,
		ResendTicket,
		ResendVerdict,
		Max
	};

	/**
	 * @struct FAuthHeader
	 *
	 * @brief The header every message carries, and all a resend request needs.
	 */
	struct FAuthHeader
	{
		EAuthMessage Type { EAuthMessage::None };

		virtual ~FAuthHeader() = default;

		virtual void Serialize(FArchive& Ar)
		{
			Ar << Type;

			if (Ar.IsLoading() && Type >= EAuthMessage::Max)
			{
				// A type nobody wrote is a packet nobody should read on.
				Ar.SetError();
			}
		}

		friend FArchive& operator<<(FArchive& Ar, FAuthHeader& Message)
		{
			Message.Serialize(Ar);

			return Ar;
		}
	};

	/**
	 * @struct FAuthTicketMessage
	 *
	 * @brief A client saying which account it plays as and offering the proof.
	 */
	struct FAuthTicketMessage : FAuthHeader
	{
		FAuthTicketMessage() { Type = EAuthMessage::Ticket; }

		/** The account as the services themselves put one on a wire, rather than in any spelling of ours. */
		TArray<uint8> Account { };
		FString Ticket { };

		virtual void Serialize(FArchive& Ar) override
		{
			FAuthHeader::Serialize(Ar);

			Ar << Account << Ticket;
		}

		friend FArchive& operator<<(FArchive& Ar, FAuthTicketMessage& Message)
		{
			Message.Serialize(Ar);

			return Ar;
		}
	};

	/**
	 * @struct FAuthVerdictMessage
	 *
	 * @brief A server saying whether the proof was even accepted for checking.
	 */
	struct FAuthVerdictMessage : FAuthHeader
	{
		FAuthVerdictMessage() { Type = EAuthMessage::Verdict; }

		bool bAccepted { false };

		virtual void Serialize(FArchive& Ar) override
		{
			FAuthHeader::Serialize(Ar);

			Ar << bAccepted;
		}

		friend FArchive& operator<<(FArchive& Ar, FAuthVerdictMessage& Message)
		{
			Message.Serialize(Ar);

			return Ar;
		}
	};

	/** A packet left unanswered this long is sent again. */
	constexpr double ResendAfterSeconds { 2.0 };

	/** Steam's own ceiling on a ticket, in the hexadecimal form it travels as. */
	constexpr int32 LongestTicketText { Steam::MaxAuthSessionTicketSize * 2 };
}

namespace PoFigGames::Online::Private
{
	/**
	 * @class FRefusedPlayers
	 *
	 * @brief Players the services would not vouch for, kept until a connection for them can be closed.
	 *
	 * A verdict arrives after the player is already in the game, and nothing at the handshake knows what a
	 * player is. The sweep below finds them by the account their player state carries and hands them to
	 * the game session, which is what closes a connection politely.
	 */
	class FRefusedPlayers
	{
	public:
		static FRefusedPlayers& Get()
		{
			static FRefusedPlayers Instance;

			return Instance;
		}

		void Refuse(const FString& AccountId)
		{
			if (AccountId.IsEmpty())
			{
				return;
			}

			Refused.AddUnique(AccountId);

			if (!SweepHandle.IsValid())
			{
				SweepHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FRefusedPlayers::Sweep), 0.0f);
			}
		}

	private:
		/** Drops everyone on the list who is playing in this world, and forgets them. */
		void KickFrom(const UWorld& World, AGameSession& Session)
		{
			for (auto Iterator = World.GetPlayerControllerIterator(); Iterator; ++Iterator)
			{
				const auto Controller = Iterator->Get();
				const auto PlayerState = Controller ? Controller->PlayerState : nullptr;

				if (PlayerState && Refused.Remove(PlayerState->GetUniqueId().ToString()) > 0)
				{
					UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[FAuthHandlerSteam] Dropping %s: the services would not vouch for them."),
						*PlayerState->GetUniqueId().ToString());

					Session.KickPlayer(Controller,
						NSLOCTEXT("OnlineSocketsSteam", "AuthRefused", "You could not be verified with the online service."));
				}
			}
		}

		bool Sweep(float /*DeltaTime*/)
		{
			if (!GEngine)
			{
				return true;
			}

			for (const auto& Context : GEngine->GetWorldContexts())
			{
				const auto World = Context.World();
				const auto GameMode = World ? World->GetAuthGameMode() : nullptr;

				if (const auto Session = GameMode ? GameMode->GameSession : nullptr)
				{
					KickFrom(*World, *Session);
				}
			}

			// A player whose connection has not reached the game yet is swept again next tick. The sweep
			// stops only when there is nobody left to look for.
			if (Refused.IsEmpty())
			{
				SweepHandle.Reset();

				return false;
			}

			return true;
		}

		TArray<FString> Refused;
		FTSTicker::FDelegateHandle SweepHandle;
	};

	/** The Steam services, which exist once per process outside the editor. */
	static UE::Online::IAuthPtr GetAuth()
	{
		const auto Services = UE::Online::GetServices(UE::Online::EOnlineServices::Steam);

		return Services.IsValid() ? Services->GetAuthInterface() : nullptr;
	}

	/** The account of whoever is signed in here, which on a client is the player offering a ticket. */
	static UE::Online::FAccountId GetLocalAccount(const UE::Online::IAuthPtr& Auth)
	{
		if (!Auth.IsValid())
		{
			return UE::Online::FAccountId { };
		}

		const auto Users = Auth->GetAllLocalOnlineUsers(UE::Online::FAuthGetAllLocalOnlineUsers::Params { });

		if (Users.IsError() || Users.GetOkValue().AccountInfo.IsEmpty())
		{
			return UE::Online::FAccountId { };
		}

		return Users.GetOkValue().AccountInfo[0]->AccountId;
	}
}

namespace PoFigGames::Online
{
	FAuthHandlerSteam::FAuthHandlerSteam()
	{
		HandlerComponent::SetActive(true);

		// Nothing travels on this connection until the exchange below has run.
		bRequiresHandshake = true;

		bEnabled = Private::GetAuth().IsValid();

		UE_CLOG(!bEnabled, LogOnlineSocketsSteam, Warning,
			TEXT("[FAuthHandlerSteam] No Steam services here, so nobody is asked to prove who they are."));
	}

	FAuthHandlerSteam::~FAuthHandlerSteam()
	{
		if (!Session.IsValid())
		{
			return;
		}

		// The session Steam opened for this player closes with their connection; left open, the provider
		// answered the next one with DuplicateRequest when this was checked on 2026-09-13.
		if (const auto Auth = Private::GetAuth())
		{
			UE::Online::FAuthEndVerifiedAuthSession::Params Params;
			Params.SessionId = Session;

			Auth->EndVerifiedAuthSession(MoveTemp(Params));
		}
	}

	void FAuthHandlerSteam::Initialize()
	{
		if (!bEnabled)
		{
			// Still has to say it is done, or every connection waits on a handshake that will not happen.
			if (Handler)
			{
				BecomeReady();
			}
			else
			{
				SetActive(false);
			}
		}
	}

	void FAuthHandlerSteam::NotifyHandshakeBegin()
	{
		if (!bEnabled)
		{
			return;
		}

		LastSent = FPlatformTime::Seconds();

		if (Handler->Mode == UE::Handler::Mode::Client)
		{
			AskForTicket();
		}
		else
		{
			State = EState::WaitingForTicket;
		}
	}

	bool FAuthHandlerSteam::IsValid() const
	{
		return bEnabled;
	}

	int32 FAuthHandlerSteam::GetReservedPacketBits() const
	{
		// One bit on every packet, saying whether it belongs to this exchange.
		return 1;
	}

	bool FAuthHandlerSteam::SupportsParallelConnectionOutgoing() const
	{
		return true;
	}

	void FAuthHandlerSteam::AskForTicket()
	{
		const auto Auth = Private::GetAuth();
		const auto Local = Private::GetLocalAccount(Auth);

		if (!Auth.IsValid() || !Local.IsValid())
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[FAuthHandlerSteam] Nobody is signed in here, so this connection offers no proof."));

			BecomeReady();

			return;
		}

		Account = Local;

		UE::Online::FAuthQueryVerifiedAuthTicket::Params Params;
		Params.LocalAccountId = Local;
		Params.Audience = UE::Online::ERemoteAuthTicketAudience::DedicatedServer;

		// Issued rather than reused: the provider only vouches for a ticket it has just handed out, and it
		// answers on a callback of its own rather than at once.
		Auth->QueryVerifiedAuthTicket(MoveTemp(Params)).OnComplete(
			[this, Alive = TWeakPtr<uint8>(Liveness)](const UE::Online::TOnlineResult<UE::Online::FAuthQueryVerifiedAuthTicket>& Result)
			{
				if (!Alive.IsValid())
				{
					return;
				}

				if (Result.IsError())
				{
					UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[FAuthHandlerSteam] No ticket was issued: %s"),
						*Result.GetErrorValue().GetLogString());

					BecomeReady();

					return;
				}

				Ticket = Result.GetOkValue().VerifiedAuthTicket.Data;

				SendTicket();
			});
	}

	void FAuthHandlerSteam::SendTicket()
	{
		Private::FAuthTicketMessage Message;
		Message.Account = UE::Online::FOnlineIdRegistryRegistry::Get().ToReplicationData(Account);
		Message.Ticket = Ticket;

		FBitWriter Packet((sizeof(Private::FAuthTicketMessage) + Private::LongestTicketText) * 8 + 1);
		Packet.WriteBit(1);
		Packet << Message;

		SendPacket(Packet);

		State = EState::SentTicket;

		UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("[FAuthHandlerSteam] Offered a ticket for %s."), *ToLogString(Account));
	}

	void FAuthHandlerSteam::SendVerdict(const bool bAccepted)
	{
		Private::FAuthVerdictMessage Message;
		Message.bAccepted = bAccepted;

		FBitWriter Packet(sizeof(Private::FAuthVerdictMessage) * 8 + 1, true);
		Packet.WriteBit(1);
		Packet << Message;

		SendPacket(Packet);
	}

	void FAuthHandlerSteam::RequestResend()
	{
		Private::FAuthHeader Message;
		Message.Type = Handler->Mode == UE::Handler::Mode::Server ? Private::EAuthMessage::ResendTicket : Private::EAuthMessage::ResendVerdict;

		FBitWriter Packet(sizeof(Private::FAuthHeader) * 8 + 1, true);
		Packet.WriteBit(1);
		Packet << Message;

		SendPacket(Packet);
	}

	void FAuthHandlerSteam::SendPacket(FBitWriter& Packet)
	{
		FOutPacketTraits Traits;

		Handler->SendHandlerPacket(this, Packet, Traits);

		LastSent = FPlatformTime::Seconds();
	}

	void FAuthHandlerSteam::BecomeReady()
	{
		if (State != EState::Ready)
		{
			State = EState::Ready;

			Initialized();
		}
	}

	void FAuthHandlerSteam::Outgoing(FBitWriter& Packet, FOutPacketTraits& /*Traits*/)
	{
		FBitWriter Marked(Packet.GetNumBits() + 1, true);

		// Everything else on this connection is marked as not ours, so the reader can tell them apart.
		Marked.WriteBit(0);
		Marked.SerializeBits(Packet.GetData(), Packet.GetNumBits());

		Packet = MoveTemp(Marked);
	}

	void FAuthHandlerSteam::Tick(float /*DeltaTime*/)
	{
		if (!bEnabled || State == EState::Ready || State == EState::Idle || !Handler)
		{
			return;
		}

		if (LastSent != 0.0 && FPlatformTime::Seconds() - LastSent > Private::ResendAfterSeconds)
		{
			RequestResend();
		}
	}
}

namespace PoFigGames::Online
{
	void FAuthHandlerSteam::Incoming(FBitReader& Packet)
	{
		const auto bForUs = !!Packet.ReadBit() && !Packet.IsError();

		if (!bEnabled || !bForUs)
		{
			return;
		}

		// Read the header, then rewind so the body can be read whole.
		FBitReaderMark Mark(Packet);
		Private::FAuthHeader Header;

		Packet << Header;

		if (Packet.IsError())
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[FAuthHandlerSteam] A packet of this exchange could not be read."));

			return;
		}

		Mark.Pop(Packet);

		if (State == EState::WaitingForTicket && Header.Type == Private::EAuthMessage::Ticket)
		{
			Private::FAuthTicketMessage Offered;
			Packet << Offered;

			if (Packet.IsError() || Offered.Ticket.Len() > Private::LongestTicketText)
			{
				UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[FAuthHandlerSteam] The offered ticket was not a ticket; the connection is refused."));

				Packet.SetError();
				SendVerdict(false);
				BecomeReady();

				return;
			}

			Account = UE::Online::FOnlineIdRegistryRegistry::Get().ToAccountId(UE::Online::EOnlineServices::Steam, Offered.Account);

			const auto Auth = Private::GetAuth();

			if (!Account.IsValid() || !Auth.IsValid())
			{
				UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[FAuthHandlerSteam] A connection offered no account this server can check."));

				SendVerdict(false);
				BecomeReady();

				return;
			}

			UE::Online::FAuthBeginVerifiedAuthSession::Params Params;
			Params.RemoteAccountId = Account;
			Params.Ticket.Type = UE::Online::ExternalLoginType::SteamSessionTicket;
			Params.Ticket.Data = Offered.Ticket;

			// Steam judges the shape of the ticket now and the account behind it on a round trip of its own,
			// which its documentation warns may take indefinitely and asks callers not to wait on; a session
			// it later calls invalid must end. Checked on 2026-09-13.
			const auto bRefusedAtOnce = MakeShared<bool>(false);

			Auth->BeginVerifiedAuthSession(MoveTemp(Params)).OnComplete(
				[this, bRefusedAtOnce, Alive = TWeakPtr<uint8>(Liveness)]
				(const UE::Online::TOnlineResult<UE::Online::FAuthBeginVerifiedAuthSession>& Result)
				{
					if (!Alive.IsValid())
					{
						return;
					}

					if (Result.IsOk())
					{
						Session = Result.GetOkValue().Session.SessionId;

						return;
					}

					UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[FAuthHandlerSteam] %s was refused: %s"),
						*ToLogString(Account), *Result.GetErrorValue().GetLogString());

					if (State == EState::Ready)
					{
						Private::FRefusedPlayers::Get().Refuse(ToString(Account));
					}
					else
					{
						*bRefusedAtOnce = true;
					}
				});

			SendVerdict(!*bRefusedAtOnce);
			BecomeReady();
		}
		else if (State == EState::SentTicket && Header.Type == Private::EAuthMessage::Verdict)
		{
			Private::FAuthVerdictMessage Verdict;
			Packet << Verdict;

			UE_CLOG(!Packet.IsError(), LogOnlineSocketsSteam, Verbose, TEXT("[FAuthHandlerSteam] The server took the ticket: %d"), Verdict.bAccepted);

			// Ready either way: a refusal is the server's to act on, and this end has to finish the
			// handshake for the refusal to reach it.
			BecomeReady();
		}
		else if (State == EState::SentTicket && Header.Type == Private::EAuthMessage::ResendTicket)
		{
			SendTicket();
		}
		else if (Header.Type == Private::EAuthMessage::ResendVerdict && Handler->Mode == UE::Handler::Mode::Server)
		{
			if (State == EState::Ready)
			{
				SendVerdict(true);
			}
			else
			{
				RequestResend();
			}
		}
	}
}

TSharedPtr<HandlerComponent> UAuthHandlerSteamFactory::CreateComponentInstance(FString& /*Options*/)
{
	return MakeShared<PoFigGames::Online::FAuthHandlerSteam>();
}
