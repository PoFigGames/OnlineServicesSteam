// Copyright PoFig Games Studio. All Rights Reserved.

#include "NetConnectionSteam.h"

#include "AuthHandlerSteam.h"
#include "Net/Core/Connection/NetCloseResult.h"
#include "NetDriverSteam.h"
#include "OnlineSocketsSteamLogChannels.h"
#include "PacketHandler.h"
#include "SocketSteam.h"
#include "SocketSubsystemSteam.h"
#include "Net/DataChannel.h"
#include "PacketHandlers/StatelessConnectHandlerComponent.h"


#include UE_INLINE_GENERATED_CPP_BY_NAME(NetConnectionSteam)


static constexpr int32 DefaultMaxPacketSize = 1024;


UNetConnectionSteam::UNetConnectionSteam(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

PoFigGames::Steam::FSocketSubsystemSteam* UNetConnectionSteam::GetSteamSubsystem()
{
	const auto ConnectionDriver = GetDriver();

	return ConnectionDriver != nullptr
		? static_cast<PoFigGames::Steam::FSocketSubsystemSteam*>(ConnectionDriver->GetSocketSubsystem())
		: nullptr;
}

void UNetConnectionSteam::InitLocalConnection(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL, EConnectionState InState, int32 InMaxPacket, int32 InPacketOverhead)
{
	InitBase(InDriver, InSocket, InURL, InState, InMaxPacket, InPacketOverhead);

	if (InDriver == nullptr || InDriver->GetSocketSubsystem() == nullptr)
	{
		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("InitLocalConnection: the driver has no socket subsystem to ask"));
		Close();

		return;
	}

	RemoteAddr = InDriver->GetSocketSubsystem()->GetAddressFromString(InURL.Host);
	if (!RemoteAddr.IsValid() || !RemoteAddr->IsValid())
	{
		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("The connection URL does not parse as a Steam address"));
		Close();

		return;
	}

	RemoteAddr->SetPort(InURL.Port);

	InitSendBuffer();
}

void UNetConnectionSteam::LowLevelSend(void* Data, int32 CountBits, FOutPacketTraits& Traits)
{
	const auto SteamSubsystem = GetSteamSubsystem();
	if (SteamSubsystem == nullptr)
	{
		return;
	}

	if (!PeerSocket.IsValid())
	{
		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("LowLevelSend: this connection has no socket to send over"));
		SteamSubsystem->SetLastSocketError(SE_EPROCLIM);

		return;
	}

	auto OutgoingData = static_cast<const uint8*>(Data);

	if (Handler.IsValid() && !Handler->GetRawSend())
	{
		const auto ModifiedData = Handler->Outgoing(static_cast<uint8*>(Data), CountBits, Traits);

		if (!ModifiedData.bError)
		{
			OutgoingData = ModifiedData.Data;
			CountBits = ModifiedData.CountBits;
		}
		else
		{
			CountBits = 0;
		}
	}

	const auto SendCount = FMath::DivideAndRoundUp(CountBits, 8);

	if (SendCount > k_cbMaxSteamNetworkingSocketsMessageSizeSend)
	{
		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("LowLevelSend: %d bytes asked for, and Steam accepts at most %d"),
			SendCount, k_cbMaxSteamNetworkingSocketsMessageSizeSend);

		SteamSubsystem->SetLastSocketError(SE_EMSGSIZE);
		return;
	}

	if (SendCount > 0)
	{
		if (int32 BytesSent = 0; !PeerSocket->Send(OutgoingData, SendCount, BytesSent))
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("LowLevelSend: %d bytes refused with error %d"),
				SendCount, static_cast<int32>(SteamSubsystem->GetLastErrorCode()));
		}
	}
}

FString UNetConnectionSteam::LowLevelGetRemoteAddress(bool bAppendPort)
{
	// Both InitLocalConnection and InitRemoteConnection set this, so the socket is only asked when
	// the connection is described before either has run.
	if (RemoteAddr.IsValid() && RemoteAddr->IsValid())
	{
		return RemoteAddr->ToString(bAppendPort);
	}

	PoFigGames::Steam::FSteamNetAddress PeerAddress;
	if (PeerSocket.IsValid())
	{
		PeerSocket->GetPeerAddress(PeerAddress);
		if (PeerAddress.IsValid())
		{
			return PeerAddress.ToString(bAppendPort);
		}
	}

	return TEXT("Invalid");
}

FString UNetConnectionSteam::LowLevelDescribe()
{
	// Asked through the accessor above rather than off RemoteAddr, which InitLocalConnection leaves unset
	// when the URL will not parse; this is reached from NMT_DebugText and from the net console commands.
	return FString::Printf(TEXT("Addr=%s"), *LowLevelGetRemoteAddress(true));
}

void UNetConnectionSteam::HandleRecvMessage(void* InData, int32 IncomingSize, const FInternetAddr* InFormattedAddress)
{
	auto IncomingData = static_cast<uint8*>(InData);

	// Nothing upstream filters an empty message: both receive loops count messages, never bytes. An empty
	// one reaching ReceivedRawPacket has PacketHandler read Data[-1] and let a stray byte drive the bit
	// count negative, and no valid Unreal packet is empty in the first place.
	if (IncomingData == nullptr || InFormattedAddress == nullptr || IncomingSize <= 0)
	{
		return;
	}

	// Steam will carry half a megabyte in one message, and this connection parses at most MaxPacket. The
	// engine's own UDP path cannot meet an oversize packet because it reads into a fixed buffer; here the
	// size is the peer's to choose, so it is checked instead.
	if (IncomingSize > MaxPacket)
	{
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Dropped a %d byte message from %s; this connection carries at most %d"),
			IncomingSize, *InFormattedAddress->ToString(true), MaxPacket);

		return;
	}

	if (bAwaitingHandshake)
	{
		const auto SteamDriver = static_cast<UNetDriverSteam*>(Driver);
		if (SteamDriver != nullptr && !SteamDriver->ArePacketHandlersDisabled()
			&& SteamDriver->ConnectionlessHandler.IsValid() && SteamDriver->StatelessConnectComponent.IsValid())
		{
			bool bHandshakeRestarted = false;

			// Both consumers take a const TSharedPtr<const FInternetAddr>&, and Clone answers with a
			// TSharedRef, so deducing the type here would build a throwaway TSharedPtr for each of them.
			const TSharedPtr<const FInternetAddr> SenderAddress = InFormattedAddress->Clone();

			// Pinned before the unwrap rather than after it: the component was checked one line above, and
			// nothing should run between the check and taking the reference.
			const auto HandshakeComponent = SteamDriver->StatelessConnectComponent.Pin();
			const auto UnwrappedPacket = SteamDriver->ConnectionlessHandler->IncomingConnectionless(SenderAddress, IncomingData, IncomingSize);

			if (!UnwrappedPacket.bError && HandshakeComponent->HasPassedChallenge(SenderAddress, bHandshakeRestarted) && !bHandshakeRestarted)
			{
				// Both ends start counting from the sequence the challenge agreed on.
				int32 ServerPacketSequence = 0;
				int32 ClientPacketSequence = 0;

				HandshakeComponent->GetChallengeSequence(ServerPacketSequence, ClientPacketSequence);

				InitSequence(ClientPacketSequence, ServerPacketSequence);

				if (Handler.IsValid())
				{
					Handler->BeginHandshaking();
				}

				UE_LOG(LogOnlineSocketsSteam, Log, TEXT("The connectionless handshake is done"));
				bAwaitingHandshake = false;

				// From here the connection answers for the handshake rather than the driver.
				if (StatelessConnectComponent.IsValid())
				{
					StatelessConnectComponent.Pin()->SetDriver(SteamDriver);
				}

				// The driver reuses the component for the next client, which needs a challenge of its own.
				HandshakeComponent->ResetChallengeData();

				IncomingSize = FMath::DivideAndRoundUp(UnwrappedPacket.CountBits, 8);
				if (IncomingSize > 0)
				{
					IncomingData = UnwrappedPacket.Data;
				}
				else
				{
					return;
				}
			}
		}

		// Still waiting means this packet was not the answer: it failed to unwrap, or it did not carry the
		// cookie, or the challenge was restarted. None of those may go on to the connection - a peer that
		// has not answered the challenge has not shown it can receive at the address it claims, and
		// ReceivedRawPacket is where a packet starts costing something.
		if (bAwaitingHandshake)
		{
			return;
		}
	}

	UNetConnection::ReceivedRawPacket(IncomingData, IncomingSize);
}

void UNetConnectionSteam::InitRemoteConnection(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL, const FInternetAddr& InRemoteAddr, EConnectionState InState, int32 InMaxPacket, int32 InPacketOverhead)
{
	InitBase(InDriver, InSocket, InURL, InState, InMaxPacket, InPacketOverhead);

	RemoteAddr = InRemoteAddr.Clone();

	InitSendBuffer();

	SetClientLoginState(EClientLoginState::LoggingIn);
	SetExpectedClientLoginMsgType(NMT_Hello);
}

void UNetConnectionSteam::CleanUp()
{
	if (const auto SteamSubsystem = GetSteamSubsystem(); SteamSubsystem != nullptr && PeerSocket.IsValid())
	{
		SteamSubsystem->QueueRemoval(PeerSocket->SteamHandle);

		// A connection a listener accepted was handed a socket of its own, and nothing else ever gives it
		// back: marking the record is not enough, because the subsystem holds the socket alive until it is
		// destroyed and a record whose socket is still alive is never swept. The one connection which does
		// not own its socket is a client's, which speaks through the driver's own.
		if (Driver == nullptr || Driver->ServerConnection != this)
		{
			// DestroySocket clears PeerSocket on the way through ResetSocketInfo, so the socket is held here
			// for as long as the call needs it.
			const auto OwnedSocket = PeerSocket;
			SteamSubsystem->DestroySocket(OwnedSocket.Get());
		}
	}

	UNetConnection::CleanUp();
}

void UNetConnectionSteam::InitBase(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL, EConnectionState InState, int32 InMaxPacket, int32 InPacketOverhead)
{
	// Steam frames the packet itself, so the overhead is zero; UNetConnection asserts on zero, hence one.
	UNetConnection::InitBase(InDriver, InSocket, InURL, InState, InMaxPacket ? InMaxPacket : DefaultMaxPacketSize, InPacketOverhead ? InPacketOverhead : 1);

	PeerSocket = InSocket != nullptr
		? TSharedPtr<PoFigGames::Steam::FSocketSteam>(static_cast<PoFigGames::Steam::FSocketSteam*>(InSocket)->AsSteamShared())
		: nullptr;

	// The handshake has no way to the connection of its own, and who the other end is the connection's to
	// know.
	if (const auto AuthHandler = GetAuthHandler())
	{
		if (PoFigGames::Steam::FSteamNetAddress PeerAddress; PeerSocket.IsValid() && PeerSocket->GetPeerAddress(PeerAddress))
		{
			AuthHandler->SetPeer(PeerAddress);
		}

		AuthHandler->SetConnection(this);
	}
}

PoFigGames::Online::FAuthHandlerSteam* UNetConnectionSteam::GetAuthHandler() const
{
	const auto Component = Handler.IsValid() ? Handler->GetComponentByName(PoFigGames::Online::FAuthHandlerSteam::ComponentName) : nullptr;

	return static_cast<PoFigGames::Online::FAuthHandlerSteam*>(Component.Get());
}

void UNetConnectionSteam::SetClientLoginState(const EClientLoginState::Type NewState)
{
	Super::SetClientLoginState(NewState);

	// Only a connection the server accepted logs in, and only its own handshake proved anything.
	if (NewState != EClientLoginState::Welcomed || Driver == nullptr || Driver->ServerConnection == this)
	{
		return;
	}

	// NMT_Login carries an identity of the client's own choosing, and the handshake has already proved
	// one. The two have to be the same player, or everything the server keys on the player state - the
	// statistics, the bans, who a later refusal belongs to - is about somebody else. Welcomed is reached
	// once the game has taken the login and before any actor exists for it.
	const auto VerifiedAccount = GetVerifiedAccount();

	if (VerifiedAccount.IsValid() && (!PlayerId.IsV2() || PlayerId.GetV2Unsafe() != VerifiedAccount))
	{
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("[UNetConnectionSteam] %s logged in as %s; the connection is refused."),
			*ToLogString(VerifiedAccount), *PlayerId.ToDebugString());

		Close(ENetCloseResult::PreLoginFailure);
	}
}

UE::Online::FAccountId UNetConnectionSteam::GetVerifiedAccount() const
{
	const auto AuthHandler = GetAuthHandler();

	return AuthHandler != nullptr ? AuthHandler->GetVerifiedAccount() : UE::Online::FAccountId { };
}

