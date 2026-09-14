// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "HandlerComponentFactory.h"
#include "Online/CoreOnline.h"
#include "PacketHandler.h"
#include "Templates/SharedPointer.h"

#include "AuthHandlerSteam.generated.h"


namespace PoFigGames::Online
{
	/**
	 * @class FAuthHandlerSteam
	 *
	 * @brief Proves who a joining player is while the connection is still being made.
	 *
	 * The client hands over a Steam ticket as part of the connection handshake and the server puts it to
	 * Steam, which is the only moment a refusal is free. Steam answers the shape of the ticket at once and
	 * the account behind it on a backend round trip its documentation warns may take indefinitely, so a
	 * player whose ticket was accepted for checking is let in and dropped later if the answer is no.
	 * Checked against the Steamworks user authentication documentation on 2026-09-13.
	 */
	class FAuthHandlerSteam : public HandlerComponent
	{
	public:
		ONLINESOCKETSSTEAM_API FAuthHandlerSteam();
		ONLINESOCKETSSTEAM_API virtual ~FAuthHandlerSteam() override;

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
			SentTicket,
			Ready
		};

		void AskForTicket();
		void SendTicket();
		void SendVerdict(bool bAccepted);
		void RequestResend();
		void SendPacket(FBitWriter& Packet);
		void BecomeReady();

		/** True while a provider is here to ask and the project has not turned this off. */
		bool bEnabled { true };

		EState State { EState::Idle };

		/** When the last packet went out, so that a lost one is sent again rather than waited on forever. */
		double LastSent { 0.0 };

		/** The ticket this client offers, empty until the provider has issued one. */
		FString Ticket { };

		/** The account this connection speaks for: our own on a client, the peer's on a server. */
		UE::Online::FAccountId Account { };

		/** The session Steam opened for a verified player, which has to be closed when they go. */
		UE::Online::FVerifiedAuthSessionId Session { };

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
