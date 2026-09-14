// Copyright PoFig Games Studio. All Rights Reserved.

#include "SocketSteam.h"

#include "SocketSubsystemSteam.h"
#include "SteamInterfaces.h"

#include "NetDriverSteam.h"
#include "OnlineSocketsSteamLogChannels.h"


namespace PoFigGames::Steam
{
	FSocketSteam::FSocketSteam(ESocketType InType, const FString& InDescription, const FName& InProtocol)
		: Super(InType, InDescription, InProtocol)
	{
		SocketSubsystem = static_cast<FSocketSubsystemSteam*>(ISocketSubsystem::Get(STEAM_SUBSYSTEMNAME));
	}

	FSocketSteam::~FSocketSteam()
	{
		// A socket usually outlives the Steamworks API it was made against, so by the time the poll group is
		// handed back there may be nothing left to hand it to.
		if (const auto NetworkingSockets = UNetDriverSteam::GetNetworkingSockets();
			NetworkingSockets != nullptr && PollGroupHandle != k_HSteamNetPollGroup_Invalid)
		{
			NetworkingSockets->DestroyPollGroup(PollGroupHandle);
		}

		PollGroupHandle = k_HSteamNetPollGroup_Invalid;

		FSocketSteam::Close();
	}

	bool FSocketSteam::Close()
	{
		if (SteamHandle == k_HSteamListenSocket_Invalid || SteamHandle == k_HSteamNetConnection_Invalid)
		{
			UE_LOG(LogOnlineSocketsSteam, VeryVerbose, TEXT("Already closed, nothing left to do"));
			return true;
		}

		auto NetworkingSockets = UNetDriverSteam::GetNetworkingSockets();

		if (NetworkingSockets == nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, VeryVerbose, TEXT("No networking interface, so the socket cannot be closed through Steam"));
			return false;
		}

		// Which branch runs is decided by what this socket is, not by whether the call succeeded: the two
		// handle spaces are different, so a failed CloseListenSocket falling through would close whatever
		// connection happened to share the number.
		if (bIsListenSocket)
		{
			// Steam tears down every connection a listen socket accepted, so they are dropped here first.
			if (!NetworkingSockets->CloseListenSocket(SteamHandle))
			{
				UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Steam refused to close listen socket %u"), SteamHandle);
				return false;
			}

			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Listen socket %u closed through Steam"), SteamHandle);

			SocketSubsystem->RemoveSocketsForListener(this);
			SocketSubsystem->QueueRemoval(SteamHandle);
			SteamHandle = k_HSteamListenSocket_Invalid;

			return true;
		}

		if (NetworkingSockets->CloseConnection(SteamHandle, ClosureReason, "Connection ended", bLingerOnClose))
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Socket %u closed through Steam, reason %d"), SteamHandle, ClosureReason);
			SocketSubsystem->QueueRemoval(SteamHandle);
			SteamHandle = k_HSteamNetConnection_Invalid;

			return true;
		}

		return false;
	}

	bool FSocketSteam::Bind(const FInternetAddr& Addr)
	{
		if (Addr.GetProtocolType() == SteamIpProtocol ||
			Addr.GetProtocolType() == SteamRelayProtocol)
		{
			// Steam has no bind step: the address is only handed over when the socket starts listening, so it
			// is kept until then.
			BindAddress = static_cast<const FSteamNetAddress&>(Addr);
			return true;
		}

		return false;
	}

	/**
	 * The options a peer to peer connection is opened with.
	 *
	 * Symmetric connect is what Valve offers for connecting two players without either of them being
	 * the one who listens: both sides call connect, and Steam pairs the two attempts into one
	 * connection instead of refusing the second.
	 */
	static TArray<SteamNetworkingConfigValue_t, TInlineAllocator<2>> MakePeerToPeerOptions()
	{
		TArray<SteamNetworkingConfigValue_t, TInlineAllocator<2>> Options;

		if (FSocketSubsystemSteam::GetSteamConfig().bUseSymmetricConnect)
		{
			Options.AddDefaulted_GetRef().SetInt32(k_ESteamNetworkingConfig_SymmetricConnect, 1);
		}

		return Options;
	}

	bool FSocketSteam::Connect(const FInternetAddr& Addr)
	{
		auto NetworkingSockets = UNetDriverSteam::GetNetworkingSockets();

		if (NetworkingSockets == nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("No networking interface, so nothing can be connected"));
			return false;
		}

		if (!Addr.IsValid())
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("The destination address is not usable"));
			SocketSubsystem->SetLastSocketError(SE_EINVAL);
			return false;
		}

		const auto& PeerAddress = static_cast<const FSteamNetAddress&>(Addr);
		if (SocketProtocol == SteamIpProtocol)
		{
			SteamNetworkingConfigValue_t LocalNetworkOption;
			LocalNetworkOption.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, static_cast<int32>(bIsLANSocket));

			SteamHandle = NetworkingSockets->ConnectByIPAddress(PeerAddress, 1, &LocalNetworkOption);
		}
		else
		{
			const auto Options = MakePeerToPeerOptions();
			SteamHandle = NetworkingSockets->ConnectP2P(PeerAddress, PeerAddress.GetPort(), Options.Num(), Options.GetData());
		}

		if (SteamHandle != k_HSteamNetConnection_Invalid)
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Connection to %s initiated"), *Addr.ToString(false));
			SocketSubsystem->AddSocket(Addr, AsSteamShared());

			return true;
		}

		return false;
	}

	bool FSocketSteam::Listen(int32 /*MaxBacklog*/)
	{
		auto NetworkingSockets = UNetDriverSteam::GetNetworkingSockets();

		if (NetworkingSockets == nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("No networking interface, so nothing can listen"));
			return false;
		}

		if (SocketProtocol == SteamIpProtocol)
		{
			// The option a listener needs is the one Connect already uses. Its localhost-only sibling applies,
			// as the SDK says, only to connections to and from localhost, so a LAN peer on another machine
			// would still be rejected for failing to authenticate.
			SteamNetworkingConfigValue_t LocalNetworkOption;
			LocalNetworkOption.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, static_cast<int32>(bIsLANSocket));

			SteamHandle = NetworkingSockets->CreateListenSocketIP(BindAddress, 1, &LocalNetworkOption);
		}
		else
		{
			const auto Options = MakePeerToPeerOptions();
			SteamHandle = NetworkingSockets->CreateListenSocketP2P(BindAddress.GetPlatformPort(), Options.Num(), Options.GetData());
		}

		if (SteamHandle != k_HSteamListenSocket_Invalid)
		{
			bIsListenSocket = true;
			SocketSubsystem->AddSocket(BindAddress, AsSteamShared());

			// The group is what every accepted connection is read through, so a listener without one can never
			// be read from at all. Each connection joins it in AdoptConnection: a poll group is set per
			// connection, and a listen socket handle is not one.
			PollGroupHandle = NetworkingSockets->CreatePollGroup();

			if (PollGroupHandle == k_HSteamNetPollGroup_Invalid)
			{
				UE_LOG(LogOnlineSocketsSteam, Error, TEXT("Listen socket %u has no poll group, so it could not be opened"), SteamHandle);
				Close();

				return false;
			}

			return true;
		}

		return false;
	}

	void FSocketSteam::AdoptConnection(const uint32 ConnectionHandle, const uint32 PollGroup)
	{
		SteamHandle = ConnectionHandle;

		if (const auto NetworkingSockets = UNetDriverSteam::GetNetworkingSockets())
		{
			NetworkingSockets->SetConnectionPollGroup(ConnectionHandle, PollGroup);
		}
	}

	FSocket* FSocketSteam::Accept(const FString& InSocketDescription)
	{
		const auto NewSocket = static_cast<FSocketSteam*>(SocketSubsystem->CreateSocket(FName("SteamClientSocket"), InSocketDescription, SocketProtocol));
		if (NewSocket == nullptr)
		{
			return nullptr;
		}

		NewSocket->SendFlags = SendFlags;
		NewSocket->bLingerOnClose = bLingerOnClose;
		NewSocket->bIsLANSocket = bIsLANSocket;

		// An accepted P2P socket keeps the channel of the socket which accepted it.
		if (SocketProtocol == SteamRelayProtocol)
		{
			NewSocket->BindAddress.SetPlatformPort(BindAddress.GetPlatformPort());
		}

		return NewSocket;
	}

	bool FSocketSteam::SendTo(const uint8* Data, int32 Count, int32& BytesSent, const FInternetAddr& Destination)
	{
		BytesSent = 0;

		FSteamNetAddress ConnectedPeer;
		if (GetPeerAddress(ConnectedPeer) && Destination == ConnectedPeer)
		{
			return Send(Data, Count, BytesSent);
		}

		if (SocketSubsystem == nullptr)
		{
			return false;
		}

		// Only the socket itself is asked for. The record also remembers which listener accepted it, and that
		// says nothing about whether it can be sent on: a client's own connection was accepted by nobody, and
		// asking for it would silently drop every send a client makes this way.
		if (const auto SocketInfo = SocketSubsystem->GetSocketInfo(Destination))
		{
			if (const auto DestinationSocket = SocketInfo->Socket.Pin())
			{
				return DestinationSocket->Send(Data, Count, BytesSent);
			}
		}

		return false;
	}

	bool FSocketSteam::Send(const uint8* Data, int32 Count, int32& BytesSent)
	{
		BytesSent = 0;

		// Steam drops anything sent before the connection reports itself established.
		if (SteamHandle != k_HSteamNetConnection_Invalid && GetConnectionState() == SCS_Connected)
		{
			switch (UNetDriverSteam::GetNetworkingSockets()->SendMessageToConnection(SteamHandle, Data, Count, SendFlags, nullptr))
			{
				case k_EResultOK:
					SocketSubsystem->SetLastSocketError(SE_NO_ERROR);
					BytesSent = Count;
					return true;
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
					SocketSubsystem->SetLastSocketError(SE_EFAULT);
					break;
			}

			BytesSent = -1;
		}

		return false;
	}

	bool FSocketSteam::Recv(uint8* Data, int32 BufferSize, int32& BytesRead, ESocketReceiveFlags::Type Flags)
	{
		BytesRead = -1;

		SteamNetworkingMessage_t* Message { nullptr };
		int32 ReadCount { 0 };

		if (!RecvRaw(Message, 1, ReadCount, Flags))
		{
			return false;
		}

		// A peek leaves the message with the socket rather than handing it over, so it is read back out of
		// where the peek put it. Only a real read owns what it was given, and only a real read gives it back.
		const bool bIsPeeking = Flags == ESocketReceiveFlags::Peek;
		const auto ReadMessage = bIsPeeking ? PendingData.Get() : Message;

		if (ReadCount < 1 || ReadMessage == nullptr)
		{
			BytesRead = 0;
			return true;
		}

		if (BufferSize < 0 || ReadMessage->GetSize() > static_cast<uint32>(BufferSize))
		{
			if (!bIsPeeking)
			{
				ReadMessage->Release();
			}

			SocketSubsystem->SetLastSocketError(SE_EMSGSIZE);
			return false;
		}

		BytesRead = static_cast<int32>(ReadMessage->GetSize());
		FMemory::Memcpy(Data, ReadMessage->GetData(), BytesRead);

		if (!bIsPeeking)
		{
			ReadMessage->Release();
		}

		return true;
	}

	bool FSocketSteam::HasPendingData(uint32& OutPendingDataSize)
	{
		if (bHasPendingData)
		{
			if (PendingData.IsValid())
			{
				OutPendingDataSize = PendingData->GetSize();
				return true;
			}

			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("A peeked message was announced but is not there"));
			bHasPendingData = false;
		}

		int32 ReadCount;
		OutPendingDataSize = 0;
		SteamNetworkingMessage_t* PeekedMessage = nullptr;

		if (RecvRaw(PeekedMessage, 1, ReadCount, ESocketReceiveFlags::Peek))
		{
			if (ReadCount >= 1 && PendingData.IsValid())
			{
				OutPendingDataSize = PendingData->GetSize();
				return true;
			}
		}

		return false;
	}

	ESocketConnectionState FSocketSteam::GetConnectionState()
	{
		auto NetworkingSockets = UNetDriverSteam::GetNetworkingSockets();
		if (SteamHandle == k_HSteamNetConnection_Invalid || NetworkingSockets == nullptr)
		{
			return SCS_NotConnected;
		}

		// The status comes back as an EResult, where only k_EResultNone is zero: read as a bool, every failure
		// would read as success and the status below would then be whatever the stack happened to hold.
		constexpr int32 LaneCount { 0 };

		SteamNetConnectionRealTimeStatus_t RealTimeStatus { };
		SteamNetConnectionRealTimeLaneStatus_t* LaneStatus { nullptr };

		if (NetworkingSockets->GetConnectionRealTimeStatus(SteamHandle, &RealTimeStatus, LaneCount, LaneStatus) == k_EResultOK)
		{
			switch (RealTimeStatus.m_eState)
			{
				case k_ESteamNetworkingConnectionState_Connected:
					return SCS_Connected;
				case k_ESteamNetworkingConnectionState_None:
				case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
					return SCS_ConnectionError;
				default:
					return SCS_NotConnected;
			}
		}

		return SCS_NotConnected;
	}

	bool FSocketSteam::GetPeerAddress(FInternetAddr& OutAddr)
	{
		auto NetworkingSockets = UNetDriverSteam::GetNetworkingSockets();
		auto& PeerAddress = static_cast<FSteamNetAddress&>(OutAddr);
		SteamNetConnectionInfo_t ConnectionInfo;

		if (NetworkingSockets && NetworkingSockets->GetConnectionInfo(SteamHandle, &ConnectionInfo))
		{
			if (ConnectionInfo.m_identityRemote.IsInvalid())
			{
				// Neither half of the remote identity says whether it is set, so an all-zero IPv6 address is the
				// only sign that this connection has no peer to report.
				if (ConnectionInfo.m_addrRemote.IsIPv6AllZeros())
				{
					return false;
				}

				PeerAddress = ConnectionInfo.m_addrRemote;
			}
			else
			{
				PeerAddress = ConnectionInfo.m_identityRemote;

				// A peer named by identity carries no port, so the channel this socket listens on stands in for it.
				PeerAddress.SetPlatformPort(BindAddress.GetPlatformPort());
			}

			return true;
		}

		return false;
	}

	bool FSocketSteam::SetNoDelay(bool bIsNoDelay)
	{
		// Steam carries these as a bitmask on the send rather than as modes to switch between, and no delay
		// implies no Nagle. The SDK calls no delay invalid on a reliable message, so a reliable socket only
		// loses Nagle. Clearing goes no further than the bit this call owns: Nagle is a setting of its own,
		// and turning it back on here would undo something the caller never asked about.
		if (bIsNoDelay)
		{
			SendFlags |= k_nSteamNetworkingSend_NoNagle;

			if ((SendFlags & k_nSteamNetworkingSend_Reliable) == 0)
			{
				SendFlags |= k_nSteamNetworkingSend_NoDelay;
			}
		}
		else
		{
			SendFlags &= ~k_nSteamNetworkingSend_NoDelay;
		}

		return true;
	}

	bool FSocketSteam::SetLinger(bool bShouldLinger, int32 /*Timeout*/)
	{
		bLingerOnClose = bShouldLinger;
		return true;
	}

	bool FSocketSteam::SetSendBufferSize(int32 Size, int32& NewSize)
	{
		NewSize = -1;

		const auto NetworkingUtils = GetSteamInterface<ISteamNetworkingUtils>();
		if (NetworkingUtils == nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Buffer sizes need the networking utilities interface"));
			return false;
		}

		const auto bSuccess = NetworkingUtils->SetConnectionConfigValueInt32(SteamHandle, k_ESteamNetworkingConfig_SendBufferSize, Size);

		// The size is read back either way, so the caller learns what it actually got. The read can fail on
		// its own — the driver sizes the socket while the handle is still invalid — and says so by leaving
		// the answer at -1 rather than by leaving it undisturbed.
		auto ConfigValueSize = sizeof(int32);
		const auto ReadResult = NetworkingUtils->GetConfigValue(k_ESteamNetworkingConfig_SendBufferSize,
			k_ESteamNetworkingConfig_Connection, SteamHandle, nullptr, &NewSize, &ConfigValueSize);

		if (ReadResult != k_ESteamNetworkingGetConfigValue_OK && ReadResult != k_ESteamNetworkingGetConfigValue_OKInherited)
		{
			NewSize = -1;
		}

		return bSuccess;
	}

	bool FSocketSteam::SetReceiveBufferSize(int32 Size, int32& NewSize)
	{
		// Steam sizes both directions with one setting.
		return SetSendBufferSize(Size, NewSize);
	}

	bool FSocketSteam::RecvRaw(SteamNetworkingMessage_t*& Data, int32 MaxMessages, int32& ReadCount, ESocketReceiveFlags::Type Flags)
	{
		auto NetworkingSockets = UNetDriverSteam::GetNetworkingSockets();

		if (NetworkingSockets == nullptr)
		{
			SocketSubsystem->SetLastSocketError(SE_SYSNOTREADY);
			return false;
		}

		if (SteamHandle == k_HSteamNetConnection_Invalid || (bIsListenSocket && SteamHandle == k_HSteamListenSocket_Invalid))
		{
			SocketSubsystem->SetLastSocketError(SE_EINVAL);
			return false;
		}

		// One message pointer is handed over below, so at most one may be written back: Steam fills an array
		// of the length it is given, and asking for more than one would have it write past the pointer.
		if (MaxMessages > 1)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("This socket returns one message at most; %d were asked for"), MaxMessages);
			MaxMessages = 1;
		}

		const bool bIsPeeking = Flags == ESocketReceiveFlags::Peek;
		if (bIsPeeking)
		{
			if (bHasPendingData)
			{
				ReadCount = 1;
				return true;
			}
		}
		else if (bHasPendingData)
		{
			// The message peeked at earlier is handed on, and with it the duty of giving it back to Steam.
			Data = PendingData.Release();
			ReadCount = 1;

			bHasPendingData = false;
			return true;
		}

		SteamNetworkingMessage_t* ReceivedMessage { nullptr };

		ReadCount = bIsListenSocket ? NetworkingSockets->ReceiveMessagesOnPollGroup(PollGroupHandle, &ReceivedMessage, MaxMessages) :
			NetworkingSockets->ReceiveMessagesOnConnection(SteamHandle, &ReceivedMessage, MaxMessages);

		if (ReadCount >= 1)
		{
			if (bIsPeeking)
			{
				// A peeked message stays here until a real read asks for it.
				PendingData.Reset(ReceivedMessage);
				bHasPendingData = true;
			}
			else
			{
				Data = ReceivedMessage;
			}

			SocketSubsystem->SetLastSocketError(SE_NO_ERROR);
			return true;
		}

		if (ReadCount == 0)
		{
			bHasPendingData = false;
			PendingData.Reset();
			return true;
		}

		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("Nothing can be read from an invalid handle (listen socket: %d)"), bIsListenSocket);
		ReadCount = -1;

		SocketSubsystem->SetLastSocketError(SE_EFAULT);
		return false;
	}

	#pragma region Unsupported
	bool FSocketSteam::Shutdown(ESocketShutdownMode Mode)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::Wait(ESocketWaitConditions::Type Condition, FTimespan WaitTime)
	{
		// Unsupported: Steam does not report writability.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::WaitForPendingConnection(bool& bHasPendingConnection, const FTimespan& WaitTime)
	{
		// Steam reports a waiting connection through its callback rather than on demand.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::SetReuseAddr(bool bAllowReuse)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::SetRecvErr(bool bUseErrorQueue)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::SetNonBlocking(bool bIsNonBlocking)
	{
		// Unsupported: nothing in this API blocks.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::SetBroadcast(bool bAllowBroadcast)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::JoinMulticastGroup(const FInternetAddr& GroupAddress)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::JoinMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::LeaveMulticastGroup(const FInternetAddr& GroupAddress)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::LeaveMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::SetMulticastLoopback(bool bLoopback)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::SetMulticastTtl(uint8 TimeToLive)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}

	bool FSocketSteam::SetMulticastInterface(const FInternetAddr& InterfaceAddress)
	{
		// Unsupported.
		SocketSubsystem->SetLastSocketError(SE_EOPNOTSUPP);
		return false;
	}
	#pragma endregion
}
