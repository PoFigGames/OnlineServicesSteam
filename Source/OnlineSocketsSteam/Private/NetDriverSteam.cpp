// Copyright PoFig Games Studio. All Rights Reserved.

#include "NetDriverSteam.h"

#include "OnlineSocketsSteamLogChannels.h"
#include "SocketSubsystemSteam.h"
#include "SocketSteam.h"
#include "NetConnectionSteam.h"
#include "SteamInterfaces.h"
#include "SteamServiceBase.h"

#include "Online/Auth.h"
#include "PacketHandler.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
#include "Misc/CommandLine.h"


UNetDriverSteam::UNetDriverSteam(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{

}

bool UNetDriverSteam::IsAvailable() const
{
	return ISocketSubsystem::Get(PoFigGames::Steam::STEAM_SUBSYSTEMNAME) != nullptr;
}

ISocketSubsystem* UNetDriverSteam::GetSocketSubsystem()
{
	return ISocketSubsystem::Get(PoFigGames::Steam::STEAM_SUBSYSTEMNAME);
}

bool UNetDriverSteam::IsNetResourceValid()
{
	return Socket != nullptr && (ServerConnection == nullptr || ServerConnection->GetConnectionState() == USOCK_Open);
}

void UNetDriverSteam::PublishServerDetails() const
{
	// Named apart from the World the driver already carries, which it would otherwise hide.
	const auto CurrentWorld = GetWorld();
	if (CurrentWorld == nullptr)
	{
		return;
	}

	PoFigGames::Steam::FSteamServerDetails ServerDetails { .MapName = CurrentWorld->GetMapName() };

	// Named on the command line, in the settings, or after the project, in that order: whoever launched
	// the server has the last word on what it is called.
	if (!FParse::Value(FCommandLine::Get(), TEXT("ServerName="), ServerDetails.ServerName))
	{
		const auto& SteamConfig = PoFigGames::Steam::FSocketSubsystemSteam::GetSteamConfig();
		ServerDetails.ServerName = SteamConfig.ServerName.IsEmpty() ? SteamConfig.ProductName : SteamConfig.ServerName;
	}

	// How many players the match holds is the business of the session, which is where the count lives.
	if (const auto GameMode = CurrentWorld->GetAuthGameMode(); GameMode != nullptr && GameMode->GameSession != nullptr)
	{
		ServerDetails.MaxPlayerCount = GameMode->GameSession->MaxPlayers;
	}

	PoFigGames::Steam::SetSteamServerDetails(ServerDetails);
}

bool UNetDriverSteam::ArePacketHandlersDisabled() const
{
#if !UE_BUILD_SHIPPING
	return FParse::Param(FCommandLine::Get(), TEXT("NoPacketHandler"));
#else
	return false;
#endif
}

ISteamNetworkingSockets* UNetDriverSteam::GetNetworkingSockets()
{
	// Whichever API this process brought up answers: a dedicated server has only the game server one.
	return PoFigGames::Steam::GetSteamInterface<ISteamNetworkingSockets>();
}

void UNetDriverSteam::ResetSocketInfo(const TSharedPtr<PoFigGames::Steam::FSocketSteam>& RemovedSocket)
{
	const auto SocketConnection =
		static_cast<UNetConnectionSteam*>(ServerConnection ? ToRawPtr(ServerConnection) : FindClientConnectionForHandle(RemovedSocket->SteamHandle));

	if (SocketConnection)
	{
		SocketConnection->ClearSocket();
	}

	if (Socket == RemovedSocket)
	{
		Socket = nullptr;
	}
}

void UNetDriverSteam::BeginReceiveBudget()
{
	// Both settings have to name a budget for there to be one, so that neither of them alone changes how
	// much the dispatch reads.
	ReceiveBailOutTime = MaxSecondsInReceive > 0.0 && NbPacketsBetweenReceiveTimeTest > 0
		? FPlatformTime::Seconds() + MaxSecondsInReceive
		: 0.0;

	PacketsUntilReceiveTimeTest = NbPacketsBetweenReceiveTimeTest;
}

bool UNetDriverSteam::IsReceiveBudgetSpent()
{
	if (ReceiveBailOutTime <= 0.0 || --PacketsUntilReceiveTimeTest > 0)
	{
		return false;
	}

	PacketsUntilReceiveTimeTest = NbPacketsBetweenReceiveTimeTest;

	if (FPlatformTime::Seconds() <= ReceiveBailOutTime)
	{
		return false;
	}

	UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Stopped reading packets after %f seconds; the rest waits for the next tick"), MaxSecondsInReceive);

	return true;
}

UNetConnection* UNetDriverSteam::FindClientConnectionForHandle(uint32 SocketHandle)
{
	for (auto& ClientConnection : ClientConnections)
	{
		const auto SteamConnection = StaticCastPtr<UNetConnectionSteam>(ClientConnection);
		if (SteamConnection && SteamConnection->GetRawSocket())
		{
			const auto& SteamSocket = SteamConnection->GetRawSocket();
			if (SteamSocket && SteamSocket->SteamHandle == SocketHandle)
			{
				return ClientConnection;
			}
		}
	}

	return nullptr;
}

void UNetDriverSteam::OnConnectionCreated(uint32 ListenParentHandle, uint32 SocketHandle)
{
	const auto SocketSubsystem = static_cast<PoFigGames::Steam::FSocketSubsystemSteam*>(GetSocketSubsystem());

	// Several drivers can listen at once — a beacon alongside the game — and each owns only what came
	// in on its own listener. Answering for somebody else's connection would destroy it when this
	// driver closes.
	if (Socket == nullptr || ListenParentHandle != Socket->SteamHandle || !SocketSubsystem)
	{
		return;
	}

	// Reached from a Steam callback a remote peer set off, so a missing interface refuses this connection
	// rather than ending the process: a server target may well ship with checks enabled.
	const auto NetworkingSockets = GetNetworkingSockets();
	if (NetworkingSockets == nullptr)
	{
		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("The Steam networking interface is gone, so connection %u cannot be answered"), SocketHandle);
		return;
	}

	// An unanswered connection stays pending on Steam's side rather than lapsing, so one which is not
	// wanted is refused at once instead of being accepted and closed.
	if (Notify && Notify->NotifyAcceptingConnection() == EAcceptConnection::Accept)
	{
		if (const auto AcceptedResult = NetworkingSockets->AcceptConnection(SocketHandle); AcceptedResult != k_EResultOK)
		{
			UE_LOG(LogOnlineSocketsSteam, Error, TEXT("Steam refused the incoming connection with error %d"), static_cast<int32>(AcceptedResult));
			SocketSubsystem->SetLastSocketError(SE_ECONNRESET);
			return;
		}

		const auto NewSocket = static_cast<PoFigGames::Steam::FSocketSteam*>(Socket->Accept(TEXT("AcceptedSocket")));
		if (NewSocket == nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, Error, TEXT("No socket could be made for connection %u, so it cannot be answered"), SocketHandle);
			NetworkingSockets->CloseConnection(SocketHandle, k_ESteamNetConnectionEnd_App_Generic, "No socket to answer with", false);

			return;
		}

		NewSocket->AdoptConnection(SocketHandle, Socket->PollGroupHandle);

		// Steam has neither an identity nor an address for a connection which died between the status event
		// and this call. Carrying the default address on would file the socket nowhere and hand the engine a
		// connection whose remote address is invalid, which is what the DDoS bookkeeping keys on.
		PoFigGames::Steam::FSteamNetAddress ConnectedAddr;
		if (!NewSocket->GetPeerAddress(ConnectedAddr))
		{
			UE_LOG(LogOnlineSocketsSteam, Error, TEXT("Steam reported no peer for connection %u, so it cannot be answered"), SocketHandle);

			SocketSubsystem->DestroySocket(NewSocket);
			NetworkingSockets->CloseConnection(SocketHandle, k_ESteamNetConnectionEnd_App_Generic, "No peer to answer", false);

			return;
		}

		const auto NewConnection = NewObject<UNetConnectionSteam>(GetTransientPackage(), NetConnectionClass);
		check(NewConnection);

		SocketSubsystem->AddSocket(ConnectedAddr, NewSocket->AsSteamShared(), Socket);
		SocketSubsystem->LinkNetDriver(NewSocket, this);

		auto RemoteConnectionState = USOCK_Pending;

		constexpr int32 LaneCount { 0 };
		SteamNetConnectionRealTimeStatus_t ConnectionStatus { };
		SteamNetConnectionRealTimeLaneStatus_t* LaneStatus { nullptr };

		// A route which is already open reports no further state change, so the connection would sit in
		// pending forever waiting for an event that has been and gone. Asking once here settles it. The
		// answer is an EResult, where every failure is non-zero: read as a bool it would say success and
		// the state below would be whatever the stack held.
		if (NetworkingSockets->GetConnectionRealTimeStatus(SocketHandle, &ConnectionStatus, LaneCount, LaneStatus) == k_EResultOK
			&& ConnectionStatus.m_eState == k_ESteamNetworkingConnectionState_Connected)
		{
			RemoteConnectionState = USOCK_Open;
		}

		NewConnection->InitRemoteConnection(this, NewSocket, World ? World->URL : FURL { }, ConnectedAddr, RemoteConnectionState);

		if (ArePacketHandlersDisabled())
		{
			// Steam numbers its own messages and the engine numbers its packets; the two never meet, so any
			// agreed starting point will do.
			NewConnection->InitSequence(4, 4);

			// Steam has already established who the peer is, so the stateless challenge is not run.
			if (NewConnection->Handler.IsValid())
			{
				NewConnection->Handler->BeginHandshaking();
			}
		}
		else if (ConnectionlessHandler.IsValid() && StatelessConnectComponent.IsValid())
		{
			NewConnection->FlagForHandshake();
		}

		Notify->NotifyAcceptedConnection(NewConnection);
		AddClientConnection(NewConnection);

		UE_LOG(LogOnlineSocketsSteam, Log, TEXT("New connection (%u) over listening socket accepted %s"), SocketHandle, *ConnectedAddr.ToString(true));
	}
	else
	{
		NetworkingSockets->CloseConnection(SocketHandle, k_ESteamNetConnectionEnd_App_Generic, "Not ours to answer", false);
		UE_LOG(LogOnlineSocketsSteam, Log, TEXT("Refused a connection that arrived on somebody else's listener"));
	}
}

void UNetDriverSteam::OnConnectionUpdated(uint32 SocketHandle, int32 NewState)
{
	// Only the connected state matters here; the relay flags this also reports are of no interest.
	if (NewState == k_ESteamNetworkingConnectionState_Connected)
	{
		if (UNetConnection* SocketConnection = ServerConnection ? ToRawPtr(ServerConnection) : FindClientConnectionForHandle(SocketHandle))
		{
			SocketConnection->SetConnectionState(USOCK_Open);
		}

		UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Connection %u is open"), SocketHandle);

		if (ServerConnection && ArePacketHandlersDisabled())
		{
			// Steam has already established who the peer is, so the stateless challenge is not run.
			// PendingNetGame asks for this too, but nothing can be sent until the connection is open.
			if (ServerConnection->Handler.IsValid())
			{
				ServerConnection->Handler->BeginHandshaking();
			}
		}
	}
}

void UNetDriverSteam::OnConnectionDisconnected(uint32 SocketHandle)
{
	if (UNetConnection* SocketConnection = ServerConnection ? ToRawPtr(ServerConnection) : FindClientConnectionForHandle(SocketHandle))
	{
		SocketConnection->SetConnectionState(USOCK_Closed);
	}

	UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Connection %u has gone away"), SocketHandle);
}

bool UNetDriverSteam::InitBase(bool bInitAsClient, FNetworkNotify* InNotify, const FURL& URL, bool bReuseAddressAndPort, FString& Error)
{
	if (!IsAvailable())
	{
		Error = TEXT("Steam is not carrying traffic in this build, so this driver cannot run");
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("%s"), *Error);

		return false;
	}

	if (!UNetDriver::InitBase(bInitAsClient, InNotify, URL, bReuseAddressAndPort, Error))
	{
		UE_CLOG(Error.IsEmpty(), LogOnlineSocketsSteam, Warning, TEXT("The base net driver refused to initialise"));
		return false;
	}

	FName SocketAddressType = NAME_None;

	// One option asks for LAN, and it means two things: the traffic goes to an address rather than through
	// the relay, and a peer that cannot prove who it is - nobody has a Steam identity on a closed network -
	// is accepted all the same. It used to also answer to bIsLanMatch, a flag of the older session layer,
	// which set the second of those and the plugin's own option did not.
	bPassthrough = URL.HasOption(TEXT("bPassthrough"));

	const auto SteamSubsystem = static_cast<PoFigGames::Steam::FSocketSubsystemSteam*>(GetSocketSubsystem());
	if (!SteamSubsystem)
	{
		Error = TEXT("There is no Steam socket subsystem to ask");
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("%s"), *Error);

		return false;
	}

	const bool bIsUsingSteamAddrs = SteamSubsystem->IsUsingRelayNetwork();

	// Hosting on a Steam identity needs a login first, and a dedicated server logs in anonymously well
	// after the driver is asked to listen. Until that happens the driver holds off rather than failing.
	// A listen server hosts on the identity of the player running it, so it asks about the client account.
	// Only a dedicated server has an identity of its own, and it is the game server account that FAuthSteam
	// registers its anonymous login under: asking about the client account there would never come back true,
	// and the driver would wait for a login nobody is going to perform.
	const FPlatformUserId& PlatformUserId = IsRunningDedicatedServer() && !bInitAsClient
		? PoFigGames::Steam::SteamGameServerPlatformId
		: PoFigGames::Steam::SteamClientPlatformId;
	const FString& SocketDescription = bInitAsClient ? PoFigGames::Steam::SteamClientSocketDescription : PoFigGames::Steam::SteamServerSocketDescription;

	// The certificate the relay network signs connections with is fetched once per session.
	if (const auto NetworkingSockets = GetNetworkingSockets())
	{
		NetworkingSockets->InitAuthentication();
	}

	// Whether this process may host on a Steam address at all is a question for the online services.
	const auto Auth = PoFigGames::Steam::FSocketSubsystemSteam::GetAuthInterface(this);
	const bool bIsLoggedIn = Auth.IsValid() && Auth->IsLoggedIn(PlatformUserId);

	// What a socket speaks is decided by configuration alone, and by the same answer on both sides of a
	// match. Neither role works it out from what it happens to be holding: the host used to read how far
	// its login had got, and the client the address it was handed.
	if (bPassthrough)
	{
		// A LAN match is spoken over IP, without the relay.
		SocketAddressType = PoFigGames::Steam::SteamIpProtocol;
	}
	else
	{
		SocketAddressType = PoFigGames::Steam::FSocketSubsystemSteam::GetSteamConfig().bUseRelay
			? PoFigGames::Steam::SteamRelayProtocol
			: PoFigGames::Steam::SteamIpProtocol;

		// A host configured the other way publishes a destination this driver cannot speak to, which is
		// worth saying here rather than leaving as a connection that never opens.
		if (bInitAsClient)
		{
			if (const auto SocketAddressHost = SteamSubsystem->GetAddressFromString(URL.Host);
				SocketAddressHost.IsValid() && SocketAddressHost->IsValid() && SocketAddressHost->GetProtocolType() != SocketAddressType)
			{
				UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Connecting over %s to a destination published as %s; the host is configured the other way."),
					*SocketAddressType.ToString(), *SocketAddressHost->GetProtocolType().ToString());
			}
		}
	}

	// Only a listener that has to come up on a Steam identity waits for anything. An address is an address
	// whether or not this process has signed in, so an IP listener is held up by nothing.
	bIsDelayedNetworkAccess = !bInitAsClient && bIsUsingSteamAddrs && !bIsLoggedIn
		&& SocketAddressType == PoFigGames::Steam::SteamRelayProtocol;

	// The subsystem is the one holding the socket alive; the driver takes a reference to the very same
	// object rather than wrapping the raw pointer in an owner of its own.
	const auto NewSocket = static_cast<PoFigGames::Steam::FSocketSteam*>(
		SteamSubsystem->CreateSocket(TEXT("SteamSocket"), SocketDescription, SocketAddressType));
	if (NewSocket == nullptr)
	{
		Error = FString::Printf(TEXT("Could not create a Steam socket for %s"), *GetDescription());
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("%s"), *Error);

		return false;
	}

	Socket = NewSocket->AsSteamShared();
	Socket->SetNoDelay(true);

	// A LAN peer cannot prove who it is, so the socket has to be told to accept it before the handshake
	// rather than after.
	if (bPassthrough)
	{
		Socket->bIsLANSocket = true;
	}

	// A server with nothing in the URL to go on leaves the choice to the subsystem, so the address type
	// is read back from the socket before a bind address is looked for.
	if (SocketAddressType.IsNone())
	{
		SocketAddressType = Socket->GetProtocol();
	}

	// Waiting for a login means there is no address to bind to yet, so an empty relay address stands in.
	if (!bIsDelayedNetworkAccess)
	{
		UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Looking for a binding address that matches protocol %s"), *SocketAddressType.ToString());

		for (const auto& BindAddress : SteamSubsystem->GetLocalBindAddresses())
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Looking at binding address %s"), *BindAddress->ToString(true));

			if (BindAddress->GetProtocolType() == SocketAddressType)
			{
				UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Binding here"));
				LocalAddr = BindAddress->Clone();

				break;
			}
		}

		if (!LocalAddr.IsValid())
		{
			Error = FString::Printf(TEXT("Nothing to bind to that matches protocol %s"), *SocketAddressType.ToString());
			UE_LOG(LogOnlineSocketsSteam, Error, TEXT("%s"), *Error);

			return false;
		}
	}
	else
	{
		LocalAddr = SteamSubsystem->CreateInternetAddr(SocketAddressType);
	}

	// The relay routes by identity and channel, so a port is only a port where the socket speaks to an
	// address. LAN is one such case; so is a match configured to go without the relay at all.
	if (SocketAddressType == PoFigGames::Steam::SteamIpProtocol)
	{
		LocalAddr->SetPort(URL.Port);
	}

	if (!ensure(Socket->Bind(*LocalAddr)))
	{
		Error = TEXT("The bind address is not one a socket can be made for");
		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("%s"), *Error);

		return false;
	}

	// Said once, because none of it is visible anywhere else and all of it decides whether a connection
	// survives being made. A zero timeout here means the driver read no configuration for its own class,
	// and every connection will be dropped as soon as it stops talking - which outside the editor is at
	// once. See the section this plugin carries in Config/Engine.ini.
	UE_LOG(LogOnlineSocketsSteam, Verbose,
		TEXT("%s is configured with connect timeout %.1fs, connection timeout %.1fs, keep alive %.2fs, client rate %d"),
		*GetDescription(), InitialConnectTimeout, ConnectionTimeout, KeepAliveTime, MaxInternetClientRate);

	return true;
}

bool UNetDriverSteam::InitConnect(FNetworkNotify* InNotify, const FURL& ConnectURL, FString& Error)
{
	if (!InitBase(true, InNotify, ConnectURL, false, Error))
	{
		UE_CLOG(Error.IsEmpty(), LogOnlineSocketsSteam, Warning, TEXT("InitConnect stopped at the shared set-up"));
		return false;
	}

	ServerConnection = NewObject<UNetConnectionSteam>(GetTransientPackage(), NetConnectionClass);
	ServerConnection->InitLocalConnection(this, Socket.Get(), ConnectURL, USOCK_Pending);

	CreateInitialClientChannels();

	if (ArePacketHandlersDisabled())
	{
		ServerConnection->InitSequence(4, 4);
	}

	const auto SteamSubsystem = static_cast<PoFigGames::Steam::FSocketSubsystemSteam*>(GetSocketSubsystem());
	check(SteamSubsystem);

	const TSharedPtr<FInternetAddr> SocketAddressHost = SteamSubsystem->GetAddressFromString(ConnectURL.Host);
	if (!SocketAddressHost.IsValid() || !SocketAddressHost->IsValid())
	{
		Error = FString::Printf(TEXT("%s does not name a Steam peer this build can reach"), *ConnectURL.Host);
		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("%s"), *Error);

		return false;
	}

	SocketAddressHost->SetPort(ConnectURL.Port);

	if (!Socket->Connect(*SocketAddressHost))
	{
		Error = FString::Printf(TEXT("Could not connect to %s, error code %d"), *SocketAddressHost->ToString(true),
			static_cast<int32>(SteamSubsystem->GetLastErrorCode()));
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("%s"), *Error);

		return false;
	}

	SteamSubsystem->LinkNetDriver(Socket.Get(), this);

	UE_LOG(LogOnlineSocketsSteam, Log, TEXT("Client bound on port %i at rate %i"), ConnectURL.Port, ServerConnection->CurrentNetSpeed);

	return true;
}

bool UNetDriverSteam::InitListen(FNetworkNotify* InNotify, FURL& ListenURL, bool bReuseAddressAndPort, FString& Error)
{
	if (!InitBase(false, InNotify, ListenURL, bReuseAddressAndPort, Error))
	{
		UE_CLOG(Error.IsEmpty(), LogOnlineSocketsSteam, Warning, TEXT("InitListen stopped at the shared set-up"));
		return false;
	}

	InitConnectionlessHandler();

	// Listen is what creates the socket on Steam's side; everything before it only described one.
	if (!bIsDelayedNetworkAccess && Socket->Listen(0) == false)
	{
		Error = TEXT("A listen socket could not be opened on the Steam network");
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("%s"), *Error);

		Socket->SetClosureReason(k_ESteamNetConnectionEnd_Misc_SteamConnectivity);

		return false;
	}

	const auto SteamSubsystem = static_cast<PoFigGames::Steam::FSocketSubsystemSteam*>(GetSocketSubsystem());
	check(SteamSubsystem);

	if (!bIsDelayedNetworkAccess)
	{
		PoFigGames::Steam::FSocketSubsystemSteam::SetListenAddress(Socket->BindAddress);

		PublishServerDetails();

		SteamSubsystem->LinkNetDriver(Socket.Get(), this);
	}
	else if (!SteamSubsystem->AddDelayedListener(Socket.Get(), this))
	{
		Error = TEXT("The listener could not be held open until a login completes");
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("%s"), *Error);

		return false;
	}

	UE_LOG(LogOnlineSocketsSteam, Log, TEXT("%s started listening on %d"), *GetDescription(), ListenURL.Port);
	UE_CLOG((!bIsDelayedNetworkAccess && !UE_BUILD_SHIPPING), LogOnlineSocketsSteam, Log, TEXT("Listening on handle %u"), Socket->SteamHandle);

	return true;
}

UNetConnection* UNetDriverSteam::FindClientConnectionForPeer(const TSharedRef<const FInternetAddr>& PeerAddress)
{
	// UNetDriver keeps this map for exactly this lookup and fills it in AddClientConnection and
	// RemoveClientConnection, so the driver does not track its connections a second time. A peer which has
	// just gone stays in it as an empty entry for RecentlyDisconnectedTrackingTime.
	const auto MappedConnection = MappedClientConnections.Find(PeerAddress);

	return MappedConnection != nullptr ? ToRawPtr(*MappedConnection) : nullptr;
}

void UNetDriverSteam::CreateMessagesConnection(const TSharedRef<PoFigGames::Steam::FSocketSteam>& AcceptedSocket)
{
	const auto SocketSubsystem = static_cast<PoFigGames::Steam::FSocketSubsystemSteam*>(GetSocketSubsystem());
	if (SocketSubsystem == nullptr)
	{
		return;
	}

	if (Notify == nullptr || Notify->NotifyAcceptingConnection() != EAcceptConnection::Accept)
	{
		UE_LOG(LogOnlineSocketsSteam, Log, TEXT("New message session rejected."));

		// Closing is not enough: only DestroySocket takes the socket out of the subsystem, and a peer which
		// is not allowed in can open sessions as fast as it likes.
		SocketSubsystem->DestroySocket(&AcceptedSocket.Get());

		return;
	}

	PoFigGames::Steam::FSteamNetAddress ConnectedAddr;
	if (!AcceptedSocket->GetPeerAddress(ConnectedAddr))
	{
		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("A message session was accepted from a peer with no address"));
		SocketSubsystem->DestroySocket(&AcceptedSocket.Get());

		return;
	}

	const auto NewConnection = NewObject<UNetConnectionSteam>(GetTransientPackage(), NetConnectionClass);
	check(NewConnection);

	SocketSubsystem->LinkNetDriver(&AcceptedSocket.Get(), this);

	// A session is either usable or gone; there is no state between the two to wait in.
	NewConnection->InitRemoteConnection(this, &AcceptedSocket.Get(), World ? World->URL : FURL { }, ConnectedAddr, USOCK_Open);

	if (ArePacketHandlersDisabled())
	{
		NewConnection->InitSequence(4, 4);

		if (NewConnection->Handler.IsValid())
		{
			NewConnection->Handler->BeginHandshaking();
		}
	}
	else if (ConnectionlessHandler.IsValid() && StatelessConnectComponent.IsValid())
	{
		NewConnection->FlagForHandshake();
	}

	Notify->NotifyAcceptedConnection(NewConnection);
	AddClientConnection(NewConnection);

	UE_LOG(LogOnlineSocketsSteam, Log, TEXT("New message session accepted from %s"), *ConnectedAddr.ToString(true));
}

void UNetDriverSteam::ReceiveMessagesOnSocket(const TSharedPtr<PoFigGames::Steam::FSocketSteam>& FromSocket, UNetConnection* ForConnection)
{
	if (!FromSocket.IsValid())
	{
		return;
	}

	int32 ReadCount = 0;
	SteamNetworkingMessage_t* Message { nullptr };

	// Built once and refilled for every message: the lookup below takes the address by shared reference.
	const auto MessageSender = MakeShared<PoFigGames::Steam::FSteamNetAddress>();

	while (FromSocket->RecvRaw(Message, 1, ReadCount) && ReadCount > 0 && Message != nullptr)
	{
		*MessageSender = PoFigGames::Steam::FSteamNetAddress(Message->m_identityPeer);

		// A message read off the socket which owns the channel belongs to a peer nobody speaks to yet.
		const auto Destination = ForConnection != nullptr ? ForConnection : FindClientConnectionForPeer(MessageSender);

		MessageSender->SetPort(Message->m_nChannel);

		if (Destination != nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, VeryVerbose, TEXT("Received message from %s with size %d"), *MessageSender->ToString(true), Message->GetSize());
			static_cast<UNetConnectionSteam*>(Destination)->HandleRecvMessage(Message->m_pData, Message->GetSize(), &MessageSender.Get());
		}
		else
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Dropped a message from %s: no connection speaks to it yet"), *MessageSender->ToString(true));
		}

		Message->Release();
		Message = nullptr;
		ReadCount = 0;

		if (IsReceiveBudgetSpent())
		{
			return;
		}
	}
}

void UNetDriverSteam::TickDispatchMessages()
{
	if (!Socket.IsValid())
	{
		return;
	}

	// Sessions the transport announced are turned into connections here: there is no notification to
	// answer, so the socket which owns the channel is asked what came in.
	if (ServerConnection == nullptr)
	{
		while (FSocket* AcceptedSocket = Socket->Accept(TEXT("AcceptedMessagesSocket")))
		{
			CreateMessagesConnection(static_cast<PoFigGames::Steam::FSocketSteam*>(AcceptedSocket)->AsSteamShared());
		}
	}

	if (ServerConnection != nullptr)
	{
		ReceiveMessagesOnSocket(Socket, ToRawPtr(ServerConnection));
		return;
	}

	// The listener holds whatever belongs to a peer with no connection yet; everybody else reads their own.
	ReceiveMessagesOnSocket(Socket, nullptr);

	for (auto& ClientConnection : ClientConnections)
	{
		if (const auto SteamConnection = static_cast<UNetConnectionSteam*>(ToRawPtr(ClientConnection)))
		{
			ReceiveMessagesOnSocket(SteamConnection->GetRawSocket(), ClientConnection);
		}
	}
}

void UNetDriverSteam::TickDispatch(float DeltaTime)
{
	UNetDriver::TickDispatch(DeltaTime);

	const bool bIsAServer = ServerConnection == nullptr;
	const auto SteamSubsystem = static_cast<PoFigGames::Steam::FSocketSubsystemSteam*>(GetSocketSubsystem());

	// Records are marked from inside Steam callbacks, where the map cannot be touched: something holding a
	// pointer into it is usually still on the stack. The tick is the safe point, and without it the map
	// only ever grows, taking the address lookup with it.
	if (SteamSubsystem != nullptr)
	{
		SteamSubsystem->SweepClosedSockets();
	}

	BeginReceiveBudget();

	// The connectionless transport names no connection on a message and announces no connection either,
	// so it is dispatched by identity rather than by handle.
	if (PoFigGames::Steam::FSocketSubsystemSteam::GetTransport() == PoFigGames::Steam::ESteamTransport::Messages)
	{
		if (!bIsDelayedNetworkAccess)
		{
			TickDispatchMessages();
		}

		return;
	}

	if (bIsDelayedNetworkAccess)
	{
		return;
	}

	// Built once and refilled for every packet: the lookup below takes the address by shared reference.
	const auto MessageSender = MakeShared<PoFigGames::Steam::FSteamNetAddress>();

	// The socket is tested every pass, because an API event handled between two reads can take it away.
	while (Socket != nullptr)
	{
		int32 BytesRead = 0;
		SteamNetworkingMessage_t* Message { nullptr };

		if (!Socket->RecvRaw(Message, 1, BytesRead) || BytesRead <= 0 || Message == nullptr)
		{
			if (BytesRead == 0)
			{
				UE_LOG(LogOnlineSocketsSteam, VeryVerbose, TEXT("Nothing more to read this tick"));
				break;
			}

			// A message with no sender means the handle is already gone; the API reports the disconnection
			// separately, and that is what cleans the connection up.
			UE_CLOG(SteamSubsystem != nullptr, LogOnlineSocketsSteam, Warning, TEXT("Reading a message failed with error %d"),
				SteamSubsystem->GetLastErrorCode());

			break;
		}

		*MessageSender = PoFigGames::Steam::FSteamNetAddress(Message->m_identityPeer);

		const auto ConnectionToHandleMessage = static_cast<UNetConnectionSteam*>(bIsAServer
			? FindClientConnectionForPeer(MessageSender)
			: ToRawPtr(ServerConnection));

		// A peer named by identity carries no port, so the channel is filled in from the socket.
		if (MessageSender->GetProtocolType() != PoFigGames::Steam::SteamIpProtocol)
		{
			MessageSender->SetPort(Message->m_nChannel);
		}

		if (ConnectionToHandleMessage != nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, VeryVerbose, TEXT("Received packet from %s with size %d"),
				*MessageSender->ToString(true), Message->GetSize());

			ConnectionToHandleMessage->HandleRecvMessage(Message->m_pData, Message->GetSize(), &MessageSender.Get());
		}
		else
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Could not find connection information for sender %s (handle: %u)"),
				*MessageSender->ToString(true), Message->m_conn);
		}

		Message->Release();

		if (IsReceiveBudgetSpent())
		{
			break;
		}
	}
}

void UNetDriverSteam::LowLevelSend(TSharedPtr<const FInternetAddr> Address, void* Data, int32 CountBits, FOutPacketTraits& Traits)
{
	if (Address.IsValid() && Address->IsValid() && Socket != nullptr)
	{
		auto OutgoingData = static_cast<uint8*>(Data);

		if (ConnectionlessHandler.IsValid())
		{
			const ProcessedPacket ModifiedData =
				ConnectionlessHandler->OutgoingConnectionless(Address, OutgoingData, CountBits, Traits);

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

		if (CountBits > 0)
		{
			const int32 SendCount = FMath::DivideAndRoundUp(CountBits, 8);

			if (int32 SentBytes = 0; !Socket->SendTo(OutgoingData, SendCount, SentBytes, *Address))
			{
				UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("LowLevelSend: Could not send %d data over socket to %s!"), SendCount, *Address->ToString(true));
			}
		}
	}
	else
	{
		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("LowLevelSend: no usable address or socket, nothing sent"));
	}
}

void UNetDriverSteam::LowLevelDestroy()
{
	UNetDriver::LowLevelDestroy();

	// Reached from FinishDestroy on the garbage collection purge, which on engine exit runs after the
	// subsystem has been unregistered: by then there is nothing to ask.
	const auto SteamSubsystem = static_cast<PoFigGames::Steam::FSocketSubsystemSteam*>(GetSocketSubsystem());
	if (SteamSubsystem != nullptr && Socket != nullptr && !HasAnyFlags(RF_ClassDefaultObject))
	{
		SteamSubsystem->QueueRemoval(Socket->SteamHandle);
		SteamSubsystem->DestroySocket(Socket.Get());
		Socket = nullptr;

		UE_LOG(LogOnlineSocketsSteam, Log, TEXT("%s shut down"), *GetDescription());
	}
}

void UNetDriverSteam::Shutdown()
{
	UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Net driver shutting down"));

	// The base class is what closes every channel and flushes the close bunch it produces, and on a client
	// the connection it flushes through is this very socket. Destroying the socket first sent every
	// farewell into an invalidated handle, so a clean disconnect reached the other side as a timeout.
	Super::Shutdown();

	if (const auto SteamSubsystem = static_cast<PoFigGames::Steam::FSocketSubsystemSteam*>(GetSocketSubsystem());
		SteamSubsystem != nullptr && Socket != nullptr)
	{
		// The driver is deliberately left on the record. DestroySocket reaches ResetSocketInfo only through
		// it, so clearing it here suppressed the very cleanup this path exists to do, and made the two
		// teardown paths disagree: LowLevelDestroy, which does not clear it, performed the cleanup.
		if (const auto SocketInfo = SteamSubsystem->GetSocketInfo(Socket->SteamHandle))
		{
			SocketInfo->MarkForDeletion();
		}

		SteamSubsystem->DestroySocket(Socket.Get());
		Socket = nullptr;
	}
}
