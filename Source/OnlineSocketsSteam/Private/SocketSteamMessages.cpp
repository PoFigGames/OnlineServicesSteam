// Copyright PoFig Games Studio. All Rights Reserved.

#include "SocketSteamMessages.h"

#include "OnlineSocketsSteamLogChannels.h"
#include "SocketSubsystemSteam.h"
#include "SteamInterfaces.h"


namespace PoFigGames::Steam
{
	/** How many messages are taken off the channel in one go. */
	static constexpr int32 MessagesPerPump { 32 };

	FSocketSteamMessages::~FSocketSteamMessages()
	{
		FSocketSteamMessages::Close();
	}

	TSharedPtr<FSocketSteamMessages> FSocketSteamMessages::GetChannelOwner() const
	{
		if (bOwnsChannel)
		{
			return StaticCastSharedRef<FSocketSteamMessages>(const_cast<FSocketSteamMessages*>(this)->AsSteamShared());
		}

		return Listener.Pin();
	}

	TSharedPtr<FSocketSteamMessages> FSocketSteamMessages::FindPeerSocket(const SteamNetworkingIdentity& RemoteIdentity) const
	{
		const auto FoundSocket = AcceptedPeers.Find(RemoteIdentity.GetSteamID64());

		return FoundSocket != nullptr ? FoundSocket->Pin() : nullptr;
	}

	void FSocketSteamMessages::PumpChannel()
	{
		const auto MessagesInterface = GetSteamInterface<ISteamNetworkingMessages>();
		if (MessagesInterface == nullptr)
		{
			return;
		}

		SteamNetworkingMessage_t* ReadMessages[MessagesPerPump] { };
		const int32 ReadCount = MessagesInterface->ReceiveMessagesOnChannel(Channel, ReadMessages, MessagesPerPump);

		for (int32 MessageIndex = 0; MessageIndex < ReadCount; ++MessageIndex)
		{
			FSteamNetworkingMessagePtr Message { ReadMessages[MessageIndex] };

			// A message belongs to whoever is speaking to that peer; anything from a peer nobody has been
			// accepted for stays with the listener until Accept hands a socket out for it.
			if (const TSharedPtr<FSocketSteamMessages> PeerSocket = FindPeerSocket(Message->m_identityPeer))
			{
				PeerSocket->ReceivedMessages.Emplace(MoveTemp(Message));
			}
			else
			{
				ReceivedMessages.Emplace(MoveTemp(Message));
			}
		}
	}

	FSteamNetworkingMessagePtr FSocketSteamMessages::TakeNextMessage()
	{
		// Pumped only when there is nothing left to hand out. The driver drains with a loop around RecvRaw,
		// so pumping on every call cost one ReceiveMessagesOnChannel per packet rather than per batch.
		if (ReceivedMessages.IsEmpty())
		{
			if (const auto ChannelOwner = GetChannelOwner())
			{
				// Only the socket which owns the channel reads it; the others are served out of what it filed.
				ChannelOwner->PumpChannel();
			}
		}

		if (ReceivedMessages.IsEmpty())
		{
			return nullptr;
		}

		FSteamNetworkingMessagePtr Message = MoveTemp(ReceivedMessages[0]);
		ReceivedMessages.RemoveAt(0, EAllowShrinking::No);

		return Message;
	}

	bool FSocketSteamMessages::Bind(const FInternetAddr& Addr)
	{
		if (Addr.GetProtocolType() != SteamRelayProtocol && Addr.GetProtocolType() != SteamIpProtocol)
		{
			return false;
		}

		// The connectionless transport routes by channel rather than by port, so the platform port of the
		// address this socket was bound to is the channel it speaks on.
		BindAddress = static_cast<const FSteamNetAddress&>(Addr);
		Channel = BindAddress.GetPlatformPort();

		return true;
	}

	bool FSocketSteamMessages::Listen(int32 /*MaxBacklog*/)
	{
		// There is nothing to listen on: a session appears when a peer sends, and is announced through
		// SteamNetworkingMessagesSessionRequest_t. Owning the channel is all this socket has to do.
		bIsListenSocket = true;
		bOwnsChannel = true;

		// The subsystem files sockets under their Steam connection handle, and this transport has none, so
		// there is nothing to file here: a messages socket is found by the identity of its peer instead. The
		// same handle check makes LinkNetDriver and DestroySocket's lookup no-ops on these sockets, which is
		// why an accepted socket's connection is never told that its socket went away.
		UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Listening for messages on channel %d"), Channel);

		return true;
	}

	bool FSocketSteamMessages::Connect(const FInternetAddr& Addr)
	{
		if (!Addr.IsValid())
		{
			SocketSubsystem->SetLastSocketError(SE_EINVAL);
			return false;
		}

		// Nothing is sent yet: the session is established by the first message, and there is no answer to
		// wait for. Sending to a peer also accepts whatever it is sending back.
		PeerAddress = static_cast<const FSteamNetAddress&>(Addr);
		Channel = PeerAddress.GetPlatformPort();

		// Nobody accepted this socket, so there is no listener reading the channel on its behalf. Without
		// this a client could send and never receive, and no handshake would ever complete.
		bOwnsChannel = true;

		return true;
	}

	bool FSocketSteamMessages::AcceptSession(const SteamNetworkingIdentity& RemoteIdentity)
	{
		const auto MessagesInterface = GetSteamInterface<ISteamNetworkingMessages>();
		if (MessagesInterface == nullptr || !bIsListenSocket)
		{
			return false;
		}

		if (!MessagesInterface->AcceptSessionWithUser(RemoteIdentity))
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Steam refused the session with %llu"), RemoteIdentity.GetSteamID64());
			return false;
		}

		PendingSessions.AddUnique(RemoteIdentity);

		return true;
	}

	FSocket* FSocketSteamMessages::Accept(const FString& InSocketDescription)
	{
		if (!bIsListenSocket || PendingSessions.IsEmpty())
		{
			return nullptr;
		}

		const SteamNetworkingIdentity RemoteIdentity = PendingSessions[0];

		// The peer stays on the list until there is a socket for it. Steam has already accepted the session
		// and will not post another request for the same peer, so dropping the identity here would leave its
		// messages arriving in the listener's queue and being discarded for the life of the process.
		const auto AcceptedSocket = static_cast<FSocketSteamMessages*>(
			SocketSubsystem->CreateSocket(FName("SteamMessagesSocket"), InSocketDescription, SocketProtocol));

		if (AcceptedSocket == nullptr)
		{
			return nullptr;
		}

		PendingSessions.RemoveAt(0, EAllowShrinking::No);

		AcceptedSocket->Channel = Channel;
		AcceptedSocket->SendFlags = SendFlags;
		AcceptedSocket->bIsLANSocket = bIsLANSocket;

		// Adopted whole rather than rebuilt through the SteamID, which would flatten an IP form identity to
		// an all-zero one. SetSteamIdentity also takes the protocol from the identity itself.
		AcceptedSocket->PeerAddress.SetSteamIdentity(RemoteIdentity);
		AcceptedSocket->PeerAddress.SetPlatformPort(Channel);
		AcceptedSocket->Listener = StaticCastSharedRef<FSocketSteamMessages>(AsSteamShared());

		AcceptedPeers.Add(RemoteIdentity.GetSteamID64(), StaticCastSharedRef<FSocketSteamMessages>(AcceptedSocket->AsSteamShared()));

		// Whatever arrived before the socket existed was filed with the listener; hand it over so that no
		// message of this peer is lost between the session request and the accept.
		for (int32 MessageIndex = ReceivedMessages.Num() - 1; MessageIndex >= 0; --MessageIndex)
		{
			if (ReceivedMessages[MessageIndex]->m_identityPeer == RemoteIdentity)
			{
				AcceptedSocket->ReceivedMessages.Insert(MoveTemp(ReceivedMessages[MessageIndex]), 0);
				ReceivedMessages.RemoveAt(MessageIndex, EAllowShrinking::No);
			}
		}

		UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Accepted a message session with %llu on channel %d"), RemoteIdentity.GetSteamID64(), Channel);

		return AcceptedSocket;
	}

	void FSocketSteamMessages::HandleSessionFailed(const SteamNetworkingIdentity& RemoteIdentity)
	{
		PendingSessions.RemoveAll([&RemoteIdentity](const SteamNetworkingIdentity& Pending)
		{
			return Pending == RemoteIdentity;
		});

		if (const TSharedPtr<FSocketSteamMessages> PeerSocket = FindPeerSocket(RemoteIdentity))
		{
			PeerSocket->Close();
		}

		AcceptedPeers.Remove(RemoteIdentity.GetSteamID64());
	}

	bool FSocketSteamMessages::SendTo(const uint8* Data, int32 Count, int32& BytesSent, const FInternetAddr& Dest)
	{
		BytesSent = 0;

		const auto MessagesInterface = GetSteamInterface<ISteamNetworkingMessages>();
		if (MessagesInterface == nullptr)
		{
			SocketSubsystem->SetLastSocketError(SE_ENETDOWN);
			return false;
		}

		const auto& SteamDestination = static_cast<const FSteamNetAddress&>(Dest);
		const SteamNetworkingIdentity RemoteIdentity = SteamDestination;

		// Nothing here ever calls CloseSessionWithUser, so without this flag a session which breaks once can
		// never be sent on again: the SDK answers every later send with k_EResultNoConnection until the
		// session is closed or restarted. The engine carries its own reliability, so restarting the session
		// underneath it costs nothing. The flag is set on the call rather than folded into SendFlags, which
		// SetNoDelay and SetSendMode still compare against.
		const auto SendResult = MessagesInterface->SendMessageToUser(RemoteIdentity, Data, Count,
			SendFlags | k_nSteamNetworkingSend_AutoRestartBrokenSession, Channel);

		if (SendResult != k_EResultOK)
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Sending %d bytes to %s failed with %d"),
				Count, *Dest.ToString(true), static_cast<int32>(SendResult));

			switch (SendResult)
			{
				case k_EResultInvalidParam:
					SocketSubsystem->SetLastSocketError(SE_EINVAL);
					break;
				case k_EResultInvalidState:
					SocketSubsystem->SetLastSocketError(SE_EBADF);
					break;
				case k_EResultNoConnection:
					SocketSubsystem->SetLastSocketError(SE_ENOTCONN);
					break;
				case k_EResultIgnored:
					SocketSubsystem->SetLastSocketError(SE_SYSNOTREADY);
					break;
				case k_EResultLimitExceeded:
					SocketSubsystem->SetLastSocketError(SE_EPROCLIM);
					break;
				default:
					// Kept deliberately: an unknown failure still reads to existing callers as "retry later".
					SocketSubsystem->SetLastSocketError(SE_EWOULDBLOCK);
					break;
			}

			return false;
		}

		BytesSent = Count;
		SocketSubsystem->SetLastSocketError(SE_NO_ERROR);

		return true;
	}

	bool FSocketSteamMessages::Send(const uint8* Data, int32 Count, int32& BytesSent)
	{
		return SendTo(Data, Count, BytesSent, PeerAddress);
	}

	bool FSocketSteamMessages::Recv(uint8* Data, int32 BufferSize, int32& BytesRead, ESocketReceiveFlags::Type Flags)
	{
		BytesRead = 0;

		if (Flags == ESocketReceiveFlags::Peek)
		{
			uint32 PendingSize { 0 };
			if (!HasPendingData(PendingSize) || PendingSize > static_cast<uint32>(BufferSize))
			{
				return false;
			}

			FMemory::Memcpy(Data, PendingData->GetData(), PendingSize);
			BytesRead = static_cast<int32>(PendingSize);

			return true;
		}

		FSteamNetworkingMessagePtr Message = bHasPendingData ? MoveTemp(PendingData) : TakeNextMessage();
		bHasPendingData = false;

		if (!Message.IsValid())
		{
			SocketSubsystem->SetLastSocketError(SE_EWOULDBLOCK);
			return false;
		}

		if (static_cast<int32>(Message->GetSize()) > BufferSize)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("A message of %u bytes does not fit the %d byte buffer and was dropped"),
				Message->GetSize(), BufferSize);

			SocketSubsystem->SetLastSocketError(SE_EMSGSIZE);
			return false;
		}

		BytesRead = static_cast<int32>(Message->GetSize());
		FMemory::Memcpy(Data, Message->GetData(), BytesRead);

		SocketSubsystem->SetLastSocketError(SE_NO_ERROR);

		return true;
	}

	bool FSocketSteamMessages::HasPendingData(uint32& OutPendingDataSize)
	{
		OutPendingDataSize = 0;

		if (!bHasPendingData)
		{
			PendingData = TakeNextMessage();
			bHasPendingData = PendingData.IsValid();
		}

		if (!bHasPendingData)
		{
			return false;
		}

		OutPendingDataSize = PendingData->GetSize();

		return true;
	}

	bool FSocketSteamMessages::RecvRaw(SteamNetworkingMessage_t*& Data, int32 /*MaxMessages*/, int32& ReadCount, ESocketReceiveFlags::Type Flags)
	{
		// The transport reads whole messages, so one is handed out at a time whatever was asked for.
		//
		// The two flags differ in who owns what comes back: a real read hands the message over and with it
		// the duty of releasing it, while a peek keeps ownership here and lends out a bare pointer which the
		// caller must not release. Nothing peeks today; anything that starts to has to know this.
		FSteamNetworkingMessagePtr Message = bHasPendingData ? MoveTemp(PendingData) : TakeNextMessage();
		bHasPendingData = false;

		if (Flags == ESocketReceiveFlags::Peek)
		{
			// A peek leaves the message where it was, so the next read still finds it.
			PendingData = MoveTemp(Message);
			bHasPendingData = PendingData.IsValid();

			Data = PendingData.Get();
			ReadCount = bHasPendingData ? 1 : 0;

			return true;
		}

		Data = Message.Release();
		ReadCount = Data != nullptr ? 1 : 0;

		return true;
	}

	bool FSocketSteamMessages::GetPeerAddress(FInternetAddr& OutAddr)
	{
		if (!PeerAddress.IsValid())
		{
			return false;
		}

		OutAddr = static_cast<FInternetAddr&>(PeerAddress);

		return true;
	}

	ESocketConnectionState FSocketSteamMessages::GetConnectionState()
	{
		const auto MessagesInterface = GetSteamInterface<ISteamNetworkingMessages>();
		if (MessagesInterface == nullptr)
		{
			return SCS_NotConnected;
		}

		if (bIsListenSocket)
		{
			// The channel is open for as long as the socket which owns it is alive.
			return SCS_Connected;
		}

		const SteamNetworkingIdentity RemoteIdentity = PeerAddress;

		switch (MessagesInterface->GetSessionConnectionInfo(RemoteIdentity, nullptr, nullptr))
		{
			case k_ESteamNetworkingConnectionState_Connected:
				return SCS_Connected;
			case k_ESteamNetworkingConnectionState_Connecting:
			case k_ESteamNetworkingConnectionState_FindingRoute:
				// Sending is allowed while the session is still being established, so this is not a failure.
				return SCS_Connected;
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
				return SCS_ConnectionError;
			default:
				break;
		}

		return SCS_NotConnected;
	}

	bool FSocketSteamMessages::Close()
	{
		if (const auto MessagesInterface = GetSteamInterface<ISteamNetworkingMessages>())
		{
			if (PeerAddress.IsValid())
			{
				const SteamNetworkingIdentity RemoteIdentity = PeerAddress;

				// Only the channel of this socket is given up; a peer may still be spoken to on another one.
				MessagesInterface->CloseChannelWithUser(RemoteIdentity, Channel);
			}

			// A session taken in but never handed out has no socket of its own to close a channel with, so it
			// would sit open on Steam's side until it timed out, with the peer queueing messages at us the
			// whole time. Channel scoped, not user scoped: a beacon in this process may be speaking to the
			// same peer on another channel.
			for (const auto& PendingIdentity : PendingSessions)
			{
				MessagesInterface->CloseChannelWithUser(PendingIdentity, Channel);
			}
		}

		// Only HandleSessionFailed took an accepted socket off its listener, so a peer which left cleanly
		// left an expired entry that PumpChannel pinned on every pump and then filed that peer's traffic
		// into the listener's own queue.
		if (const auto OwningListener = Listener.Pin())
		{
			OwningListener->AcceptedPeers.Remove(PeerAddress.GetSteamID64());
		}

		PendingData.Reset();
		bHasPendingData = false;
		ReceivedMessages.Empty();
		PendingSessions.Empty();
		AcceptedPeers.Empty();

		return true;
	}
}
