// Copyright PoFig Games Studio. All Rights Reserved.

#include "AuthHandlerSteam.h"

#include "Engine/NetDriver.h"
#include "NetConnectionSteam.h"
#include "Net/Core/Connection/NetCloseResult.h"
#include "OnlineSocketsSteamModule.h"
#include "SocketSubsystemSteam.h"
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
	 * Steam's own ceiling on a ticket, in the hexadecimal form it travels as.
	 *
	 * This is what a reader will take, not what a sender can deliver: what fits in one packet is smaller,
	 * and is the connection's own budget rather than anything Steam decides.
	 */
	constexpr int32 LongestTicketText { Steam::MaxAuthSessionTicketSize * 2 };

	/** An account travels as the services put one on a wire, which for Steam is a SteamID and nothing else. */
	constexpr int32 AccountDataSize { sizeof(uint64) };

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

			// Both fields take their length from the packet, so both are bounded here rather than after the
			// fact. The stock array operator answers a lie with an ensure at sixteen megabytes and the
			// string operator with nothing at all: it allocates whatever the count said and then scans it.
			auto AccountSize = Ar.IsLoading() ? 0 : Account.Num();

			Ar << AccountSize;

			if (AccountSize != AccountDataSize)
			{
				Ar.SetError();

				return;
			}

			if (Ar.IsLoading())
			{
				Account.SetNumUninitialized(AccountDataSize);
			}

			Ar.Serialize(Account.GetData(), AccountDataSize);

			const auto PreviousMaxSize = Ar.GetMaxSerializeSize();

			// The count a string carries is the text plus its terminator.
			Ar.ArMaxSerializeSize = LongestTicketText + 1;

			Ar << Ticket;

			Ar.ArMaxSerializeSize = PreviousMaxSize;
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

	/** The account a Steam identity belongs to, asked for in the form identities travel in. */
	static UE::Online::FAccountId ToAccountId(const uint64 SteamId)
	{
		TArray<uint8> ReplicationData;
		ReplicationData.SetNumUninitialized(sizeof(SteamId));

		FMemory::Memcpy(ReplicationData.GetData(), &SteamId, sizeof(SteamId));

		return UE::Online::FOnlineIdRegistryRegistry::Get().ToAccountId(UE::Online::EOnlineServices::Steam, ReplicationData);
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
	const FName FAuthHandlerSteam::ComponentName { TEXT("AuthHandlerSteam") };

	FAuthHandlerSteam::FAuthHandlerSteam()
		: HandlerComponent(ComponentName)
	{
		HandlerComponent::SetActive(true);

		// Nothing travels on this connection until the exchange below has run. Which services to ask is
		// not known yet: that depends on the world the connection belongs to, and the connection is handed
		// over after the packet handler has built its components.
		bRequiresHandshake = true;
	}

	FAuthHandlerSteam::~FAuthHandlerSteam()
	{
		if (!Auth.IsValid())
		{
			return;
		}

		// The session Steam opened for this player closes with their connection; left open, the provider
		// answered the next one with DuplicateRequest when this was checked on 2026-09-13.
		if (Session.IsValid())
		{
			UE::Online::FAuthEndVerifiedAuthSession::Params SessionParams;
			SessionParams.SessionId = Session;

			Auth->EndVerifiedAuthSession(MoveTemp(SessionParams));
		}

		// A ticket stays good until it is cancelled, and this one was issued for this connection alone.
		// Cancelling tells whoever still holds it to drop the session, which by now is what leaving means.
		if (IssuedTicket.IsValid())
		{
			UE::Online::FAuthCancelVerifiedAuthTicket::Params TicketParams;
			TicketParams.LocalAccountId = Account;
			TicketParams.VerifiedAuthTicketId = IssuedTicket;

			Auth->CancelVerifiedAuthTicket(MoveTemp(TicketParams));
		}
	}

	void FAuthHandlerSteam::Initialize()
	{
		// Nothing is decided here. Which services this exchange belongs to depends on the connection, and
		// the packet handler builds and initialises its components before the connection is handed over, so
		// both that and the answer for a world with no services are settled in NotifyHandshakeBegin.
	}

	void FAuthHandlerSteam::NotifyHandshakeBegin()
	{
		// Asked for here rather than in the constructor: in the editor the services instance is named after
		// the play world, and the one this exchange belongs to is the one its connection's driver is in. By
		// now the connection has been handed over; in the constructor it had not.
		Auth = Steam::FSocketSubsystemSteam::GetAuthInterface(Connection.IsValid() ? ToRawPtr(Connection->Driver) : nullptr);
		bEnabled = Auth.IsValid();

		if (!bEnabled)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning,
				TEXT("[FAuthHandlerSteam] No Steam services in this world, so nobody is asked to prove who they are."));

			BecomeReady();

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
		const auto Local = Private::GetLocalAccount(Auth);

		if (!Local.IsValid())
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
		// answers on a callback of its own rather than at once. Asked for through the seam where the
		// services left one, so that the ticket names this host and no other can show it on.
		const auto& BoundRequest = FOnlineSocketsSteamModule::Get().GetBoundTicketRequest();

		auto TicketHandle = BoundRequest.IsBound()
			? BoundRequest.Execute(Auth, MoveTemp(Params), Peer)
			: Auth->QueryVerifiedAuthTicket(MoveTemp(Params));

		TicketHandle.OnComplete(
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
				IssuedTicket = Result.GetOkValue().VerifiedAuthTicketId;

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

		// What fits is the connection's packet size less what the components after this one reserve, and a
		// ticket is the only thing here whose length Steam decides. Over that budget the packet handler
		// drops the packet with an error of its own, and this end would then offer the same ticket again
		// every two seconds until the connection timed out.
		if (Packet.GetNumBits() > MaxOutgoingBits)
		{
			UE_LOG(LogOnlineSocketsSteam, Error,
				TEXT("[FAuthHandlerSteam] The ticket needs %" INT64_FMT " bits and %u fit in a packet; this connection cannot offer proof."),
				Packet.GetNumBits(), MaxOutgoingBits);

			if (const auto UnprovableConnection = Connection.Get())
			{
				UnprovableConnection->Close(ENetCloseResult::PendingConnectionFailure);
			}

			return;
		}

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

	void FAuthHandlerSteam::SetPeer(const Steam::FSteamNetAddress& InPeer)
	{
		Peer = InPeer;
	}

	void FAuthHandlerSteam::SetConnection(UNetConnectionSteam* InConnection)
	{
		Connection = InConnection;
	}

	void FAuthHandlerSteam::Refuse(const TCHAR* Reason)
	{
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[FAuthHandlerSteam] %s; the connection is refused."), Reason);

		SendVerdict(false);

		// Never Initialized(): a component that reports itself ready hands the connection straight on to
		// NMT_Hello, which is the thing a refusal has to prevent. Closing does not tear this component down
		// with it - that waits for the driver's own tick - so returning straight after is enough
		// (UNetConnection::Close, checked on 2026-09-15).
		if (const auto RefusedConnection = Connection.Get())
		{
			RefusedConnection->Close(ENetCloseResult::PreLoginFailure);
		}
	}

	void FAuthHandlerSteam::Refuse(FBitReader& Packet, const TCHAR* Reason)
	{
		Refuse(Reason);

		// Inside Incoming the packet is the shorter way out, and it covers a connection this component was
		// never told about: an error carrying no recovery closes it (UNetConnection::ReceivedRawPacket).
		Packet.SetError();
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
		// Nothing is resent while the services are being waited on: the ticket is with them, and asking for
		// it again would only start the wait over.
		if (!bEnabled || State == EState::Ready || State == EState::Idle || State == EState::WaitingForVerdict || !Handler)
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
				Refuse(Packet, TEXT("The offered ticket was not a ticket"));

				return;
			}

			if (!Auth.IsValid())
			{
				// The handler only enables itself where there is a provider, so this is one that went away
				// mid-handshake. Nobody can be checked, and letting them in unchecked is not the answer.
				Refuse(Packet, TEXT("This server has no online services left to check a ticket with"));

				return;
			}

			Account = UE::Online::FOnlineIdRegistryRegistry::Get().ToAccountId(UE::Online::EOnlineServices::Steam, Offered.Account);

			if (!Account.IsValid())
			{
				Refuse(Packet, TEXT("A connection offered an account this server cannot resolve"));

				return;
			}

			// The relay signs the identity it routes for, so over it a ticket for anybody else is not this
			// peer's to offer. A plain address authenticates nothing, and there is nothing to hold it to.
			if (Peer.GetProtocolType() == Steam::SteamRelayProtocol
				&& Account != Private::ToAccountId(Peer.GetSteamID().ConvertToUint64()))
			{
				Refuse(Packet, TEXT("The offered ticket names an account other than the peer Steam vouched for"));

				return;
			}

			UE::Online::FAuthBeginVerifiedAuthSession::Params Params;
			Params.RemoteAccountId = Account;
			Params.Ticket.Type = UE::Online::ExternalLoginType::SteamSessionTicket;
			Params.Ticket.Data = Offered.Ticket;

			// Nothing is answered until the services have. Steam judges the shape of the ticket at once and
			// the account behind it on a round trip of its own, and the connection waits for both rather
			// than being let through and taken back. Checked on 2026-09-13.
			Auth->BeginVerifiedAuthSession(MoveTemp(Params)).OnComplete(
				[this, Alive = TWeakPtr<uint8>(Liveness)]
				(const UE::Online::TOnlineResult<UE::Online::FAuthBeginVerifiedAuthSession>& Result)
				{
					if (!Alive.IsValid())
					{
						// The connection went before the answer did, and a session Steam opened for it is
						// now nobody's to close. Left open, it answers that player's next attempt with
						// DuplicateRequest for as long as this server lives.
						if (Result.IsOk() && Auth.IsValid())
						{
							UE::Online::FAuthEndVerifiedAuthSession::Params Params;
							Params.SessionId = Result.GetOkValue().Session.SessionId;

							Auth->EndVerifiedAuthSession(MoveTemp(Params));
						}

						return;
					}

					if (Result.IsOk())
					{
						Session = Result.GetOkValue().Session.SessionId;

						UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("[FAuthHandlerSteam] The services vouched for %s."), *ToLogString(Account));

						SendVerdict(true);
						BecomeReady();

						return;
					}

					// A timeout is among these. Nothing is known about the player either way by then, and a
					// player nothing is known about does not get in.
					Refuse(*FString::Printf(TEXT("%s could not be verified: %s"),
						*ToLogString(Account), *Result.GetErrorValue().GetLogString()));
				});

			State = EState::WaitingForVerdict;
		}
		else if (State == EState::SentTicket && Header.Type == Private::EAuthMessage::Verdict)
		{
			Private::FAuthVerdictMessage Verdict;
			Packet << Verdict;

			if (Packet.IsError())
			{
				return;
			}

			if (!Verdict.bAccepted)
			{
				// The server has already closed its end, so there is nothing left to finish the handshake
				// for; closing here too turns a minute of timing out into an answer.
				UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[FAuthHandlerSteam] The server would not take the ticket; the connection is refused."));

				Packet.SetError();

				return;
			}

			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("[FAuthHandlerSteam] The server took the ticket."));

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
			else if (State == EState::WaitingForTicket)
			{
				RequestResend();
			}

			// While the services are being waited on there is nothing to say yet, and saying it would only
			// fetch the same ticket again.
		}
	}
}

TSharedPtr<HandlerComponent> UAuthHandlerSteamFactory::CreateComponentInstance(FString& /*Options*/)
{
	return MakeShared<PoFigGames::Online::FAuthHandlerSteam>();
}
