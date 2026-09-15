// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "HandlerComponentFactory.h"
#include "Online/CoreOnline.h"
#include "Online/OnlineServices.h"
#include "PacketHandler.h"
#include "SteamNetAddress.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

#include "AuthHandlerSteam.generated.h"


class UNetConnectionSteam;


namespace PoFigGames::Online
{
	/**
	 * @class FAuthHandlerSteam
	 *
	 * @brief Proves who a joining player is while the connection is still being made.
	 *
	 * The client hands over a Steam ticket as part of the connection handshake and the server puts it to
	 * Steam, which is the only moment a refusal is free. Steam answers the shape of the ticket at once and
	 * the account behind it on a backend round trip its documentation warns may take indefinitely, and the
	 * connection waits for both: nothing reaches the game until the services have said yes, and every other
	 * answer, silence included, closes it. Checked against the Steamworks user authentication documentation
	 * on 2026-09-13.
	 *
	 * A world with no services asks nobody to prove anything, which only works where neither end has them:
	 * a server that asks and a client that cannot answer wait for each other until the connection times out.
	 */
	class FAuthHandlerSteam : public HandlerComponent
	{
	public:
		/** The name this component is registered under, so that a connection can find it on its handler. */
		static ONLINESOCKETSSTEAM_API const FName ComponentName;

		ONLINESOCKETSSTEAM_API FAuthHandlerSteam();
		ONLINESOCKETSSTEAM_API virtual ~FAuthHandlerSteam() override;

		/**
		 * Tells this end who the transport says the other end is.
		 *
		 * Two things are held against it. A ticket is issued for this peer alone, so a host cannot show it
		 * on to another host as its own; and where the relay signed the identity, a ticket naming any other
		 * account is refused. A plain address signs nothing, so only the first of those applies there.
		 */
		ONLINESOCKETSSTEAM_API void SetPeer(const PoFigGames::Steam::FSteamNetAddress& InPeer);

		/** Tells this end which connection it belongs to, so that a refusal can close it. */
		ONLINESOCKETSSTEAM_API void SetConnection(UNetConnectionSteam* InConnection);

		/**
		 * The account the peer offered and the services vouched for, empty until they have.
		 *
		 * Over the relay it is also the identity Steam authenticated for the connection, because a ticket
		 * naming anybody else is refused; on a plain address there is nothing to hold it against.
		 */
		const UE::Online::FAccountId& GetVerifiedAccount() const { return Account; }

#pragma region HandlerComponent

		ONLINESOCKETSSTEAM_API virtual void Initialize() override;
		ONLINESOCKETSSTEAM_API virtual void NotifyHandshakeBegin() override;
		ONLINESOCKETSSTEAM_API virtual bool IsValid() const override;
		ONLINESOCKETSSTEAM_API virtual void Incoming(FBitReader& Packet) override;
		ONLINESOCKETSSTEAM_API virtual void Outgoing(FBitWriter& Packet, FOutPacketTraits& Traits) override;
		ONLINESOCKETSSTEAM_API virtual void Tick(float DeltaTime) override;
		ONLINESOCKETSSTEAM_API virtual int32 GetReservedPacketBits() const override;
		ONLINESOCKETSSTEAM_API virtual bool SupportsParallelConnectionOutgoing() const override;

#pragma endregion HandlerComponent

	private:
		/**
		 * @enum EState
		 *
		 * @brief Where this end of the exchange has got to.
		 */
		enum class EState : uint8
		{
			Idle,
			WaitingForTicket,
			WaitingForVerdict,
			SentTicket,
			Ready
		};

		void AskForTicket();
		void SendTicket();
		void SendVerdict(bool bAccepted);

		/** Tells the peer it is not welcome and closes the connection, rather than finishing the handshake. */
		void Refuse(const TCHAR* Reason);
		void Refuse(FBitReader& Packet, const TCHAR* Reason);

		void RequestResend();
		void SendPacket(FBitWriter& Packet);
		void BecomeReady();

		/** True while there is a provider here to ask; there is no switch, and see the note on the class. */
		bool bEnabled { true };

		/**
		 * The services this exchange speaks to, taken from the world its connection belongs to.
		 *
		 * Held rather than asked for again: the answers outlive the connection, and the late ones have to
		 * reach the same instance the request was made on.
		 */
		UE::Online::IAuthPtr Auth { nullptr };

		EState State { EState::Idle };

		/** When the last packet went out, so that a lost one is sent again rather than waited on forever. */
		double LastSent { 0.0 };

		/** The ticket this client offers, empty until the provider has issued one. */
		FString Ticket { };

		/** The account this connection speaks for: our own on a client, the peer's on a server. */
		UE::Online::FAccountId Account { };

		/** Who the transport says the peer is; empty on a connection nothing told. */
		PoFigGames::Steam::FSteamNetAddress Peer { };

		/** The connection this exchange belongs to, which is what a refusal has to close. */
		TWeakObjectPtr<UNetConnectionSteam> Connection { nullptr };

		/** The session Steam opened for a verified player, which has to be closed when they go. */
		UE::Online::FVerifiedAuthSessionId Session { };

		/** The ticket this end was issued, which is good until it is cancelled and was issued for one connection. */
		UE::Online::FVerifiedAuthTicketId IssuedTicket { };

		/** Says whether this handler is still alive to the asynchronous answers it is waiting on. */
		TSharedRef<uint8> Liveness { MakeShared<uint8>(0) };
	};
}


/**
 * @class UAuthHandlerSteamFactory
 *
 * @brief Makes the handler above, once named in the PacketHandlerComponents list of the project.
 */
UCLASS(MinimalAPI)
class UAuthHandlerSteamFactory : public UHandlerComponentFactory
{
	GENERATED_BODY()

public:
	ONLINESOCKETSSTEAM_API virtual TSharedPtr<HandlerComponent> CreateComponentInstance(FString& Options) override;
};
