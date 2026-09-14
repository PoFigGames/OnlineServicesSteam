// Copyright PoFig Games Studio. All Rights Reserved.

#include "SocketSubsystemSteam.h"

#include "NetDriverSteam.h"
#include "OnlineSocketsSteamLogChannels.h"
#include "SocketSteam.h"
#include "SocketSteamMessages.h"
#include "SteamInterfaces.h"
#include "SteamPlatformConfig.h"
#include "SteamServiceBase.h"

#include "Engine/GameEngine.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "Online/Auth.h"
#include "Online/OnlineAsyncOpHandle.h"
#include "Online/OnlineResult.h"
#include "Online/OnlineServices.h"
#include "SocketSubsystemModule.h"

THIRD_PARTY_INCLUDES_START
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
THIRD_PARTY_INCLUDES_END


DEFINE_LOG_CATEGORY_STATIC(LogSteamAPI, Log, All);

namespace PoFigGames::Steam
{
	namespace Private
	{
		/** Where this process listens, kept for whoever asks after the fact rather than being told. */
		static TOptional<FSteamNetAddress>& GetListenAddressStorage()
		{
			static TOptional<FSteamNetAddress> ListenAddress { };
			return ListenAddress;
		}
	}

	/** Hook Steam calls with its own diagnostics, forwarded to this module's log channel. */
	void LogSteamNetworkingMessage(ESteamNetworkingSocketsDebugOutputType InType, const char* InMessage)
	{
	#if !NO_LOGGING
		auto Verboseness = ELogVerbosity::VeryVerbose;
		switch (InType)
		{
			default:
			case k_ESteamNetworkingSocketsDebugOutputType_None:
				break;
			// Steam calls ordinary connectivity trouble an error, and Fatal is the one verbosity nothing in
			// the engine writes without dying: a line of third party text must not end the run, nor read to
			// a log scraper as though it had.
			case k_ESteamNetworkingSocketsDebugOutputType_Bug:
			case k_ESteamNetworkingSocketsDebugOutputType_Error:
				Verboseness = ELogVerbosity::Error;
				break;
			case k_ESteamNetworkingSocketsDebugOutputType_Important:
			case k_ESteamNetworkingSocketsDebugOutputType_Warning:
				Verboseness = ELogVerbosity::Warning;
				break;
			case k_ESteamNetworkingSocketsDebugOutputType_Everything:
			case k_ESteamNetworkingSocketsDebugOutputType_Verbose:
				Verboseness = ELogVerbosity::Verbose;
				break;
			case k_ESteamNetworkingSocketsDebugOutputType_Msg:
			case k_ESteamNetworkingSocketsDebugOutputType_Debug:
				Verboseness = ELogVerbosity::Display;
				break;
		}

		GLog->Log(LogSteamAPI.GetCategoryName(), Verboseness, StringCast<TCHAR>(InMessage).Get());
	#endif
	}

	bool FSocketSubsystemSteam::Init(FString& Error)
	{
		const auto NetworkingUtils = GetSteamInterface<ISteamNetworkingUtils>();
		if (NetworkingUtils == nullptr)
		{
			Error = TEXT("Steam networking utils are not available");
			return false;
		}

		#if UE_BUILD_SHIPPING
		// Steam's own logging is verbose enough to matter in a shipping build, so it is turned down there.
		const auto DebugLevel = k_ESteamNetworkingSocketsDebugOutputType_Important;
		#else
		const auto DebugLevel = k_ESteamNetworkingSocketsDebugOutputType_Msg;
		#endif

		NetworkingUtils->SetDebugOutputFunction(DebugLevel, LogSteamNetworkingMessage);

		// Registered once nothing is left to fail: a caller which sees this fail drops the subsystem without
		// shutting it down, and a registration left behind would name an object which is already gone.
		auto& SocketsModule = FModuleManager::LoadModuleChecked<FSocketSubsystemModule>("Sockets");
		SocketsModule.RegisterSocketSubsystem(STEAM_SUBSYSTEMNAME, this, ShouldOverrideDefaultSubsystem());

		return true;
	}

	void FSocketSubsystemSteam::Shutdown()
	{
		UE_LOG(LogOnlineSocketsSteam, Log, TEXT("Cleaning up"));

		// Steam keeps the function pointer until it is handed another one, and this module can be gone long
		// before the process is.
		if (const auto NetworkingUtils = GetSteamInterface<ISteamNetworkingUtils>())
		{
			NetworkingUtils->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
		}

		// Where this process listened is read elsewhere as "this process is hosting something". It outlives
		// the subsystem, and would otherwise point the next lobby at a server which is gone.
		Private::GetListenAddressStorage().Reset();

		if (const auto SocketsModule = FModuleManager::GetModulePtr<FSocketSubsystemModule>("Sockets"))
		{
			SocketsModule->UnregisterSocketSubsystem(STEAM_SUBSYSTEMNAME);

			// Unregistering takes the entry out of the map but leaves the default name pointing here, so
			// every later request with no name would ask for a subsystem which is no longer registered and
			// be answered with null. Handing the default to the platform is what it was before this took it.
			if (ShouldOverrideDefaultSubsystem())
			{
				if (const auto PlatformSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
				{
					SocketsModule->RegisterSocketSubsystem(PLATFORM_SOCKETSUBSYSTEM, PlatformSubsystem, true);
				}
			}
		}

		// A socket outliving this object would reach back into its bookkeeping from ~FSocketSteam, and by
		// then the map is gone: members are destroyed in reverse declaration order, and the map is declared
		// after the array which holds the sockets alive.
		for (const auto& OwnedSocket : OwnedSockets)
		{
			OwnedSocket->Close();
		}

		OwnedSockets.Empty();
		SocketInformationMap.Empty();
	}

	TSharedPtr<FSocketSteamMessages> FSocketSubsystemSteam::FindMessagesChannelOwner() const
	{
		for (const auto& OwnedSocket : OwnedSockets)
		{
			if (const auto MessagesSocket = StaticCastSharedRef<FSocketSteamMessages>(OwnedSocket); MessagesSocket->OwnsChannel())
			{
				return MessagesSocket;
			}
		}

		return nullptr;
	}

	void FSocketSubsystemSteam::OnMessagesSessionRequest(SteamNetworkingMessagesSessionRequest_t* Message)
	{
		if (Message == nullptr || GetTransport() != ESteamTransport::Messages)
		{
			return;
		}

		// A session is only ever taken in by a socket which was listened on; AcceptSession refuses on any
		// other, which is what keeps a client from answering an unsolicited peer.
		const auto ChannelOwner = FindMessagesChannelOwner();
		if (!ChannelOwner.IsValid() || !ChannelOwner->AcceptSession(Message->m_identityRemote))
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Ignoring a message session from %llu: nothing here is listening for one."),
				Message->m_identityRemote.GetSteamID64());

			return;
		}

		UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Took in a message session from %llu"), Message->m_identityRemote.GetSteamID64());
	}

	void FSocketSubsystemSteam::OnMessagesSessionFailed(SteamNetworkingMessagesSessionFailed_t* Message)
	{
		if (Message == nullptr || GetTransport() != ESteamTransport::Messages)
		{
			return;
		}

		UE_LOG(LogOnlineSocketsSteam, Log, TEXT("The message session with %llu failed: %hs"),
			Message->m_info.m_identityRemote.GetSteamID64(), Message->m_info.m_szEndDebug);

		if (const auto ChannelOwner = FindMessagesChannelOwner())
		{
			ChannelOwner->HandleSessionFailed(Message->m_info.m_identityRemote);
		}
	}

	FSocket* FSocketSubsystemSteam::CreateSocket(const FName& /*SocketType*/, const FString& SocketDescription, const FName& ProtocolType)
	{
		// Steam speaks over the relay and over a plain IP address, and the two are different socket
		// protocols rather than a transport and its fallback. The relay is what this subsystem exists for,
		// so it is what a caller naming no protocol gets; an IP socket is only ever made for a caller which
		// asks for one, which is a LAN match or a destination that is already an address.
		FName ProtocolTypeToUse = ProtocolType.IsNone() ? SteamRelayProtocol : ProtocolType;

		if (ProtocolTypeToUse != SteamRelayProtocol && ProtocolTypeToUse != SteamIpProtocol)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Cannot create a socket for protocol %s: the Steam network offers %s and %s."),
				*ProtocolTypeToUse.ToString(), *SteamRelayProtocol.Resolve().ToString(), *SteamIpProtocol.Resolve().ToString());

			return nullptr;
		}

		// The engine takes the socket by raw pointer and gives it back through DestroySocket, so the
		// subsystem holds the only reference which keeps it alive in between.
		const TSharedRef<FSocketSteam> NewSocket = GetTransport() == ESteamTransport::Messages
			? StaticCastSharedRef<FSocketSteam>(MakeShared<FSocketSteamMessages>(SOCKTYPE_Datagram, SocketDescription, ProtocolTypeToUse))
			: MakeShared<FSocketSteam>(SOCKTYPE_Datagram, SocketDescription, ProtocolTypeToUse);

		OwnedSockets.Add(NewSocket);

		return &NewSocket.Get();
	}

	void FSocketSubsystemSteam::DestroySocket(FSocket* Socket)
	{
		if (Socket == nullptr)
		{
			return;
		}

		const auto SteamSocket = static_cast<FSocketSteam*>(Socket);
		UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Destroying socket %u"), SteamSocket->SteamHandle);

		if (const auto SocketInfo = GetSocketInfo(SteamSocket->SteamHandle))
		{
			if (const auto SteamDriver = SocketInfo->NetDriver.Get())
			{
				SteamDriver->ResetSocketInfo(SteamSocket->AsSteamShared());
			}

			SocketInfo->Socket.Reset();
			SocketInfo->MarkForDeletion(); // Mark us for deletion so we're removed from the map
		}

		// The address this process published is what a lobby points players at, so it goes away with the
		// socket which was listening on it. Another listener of this process, a beacon beside the game, is
		// told apart by the channel it took.
		if (SteamSocket->bIsListenSocket)
		{
			if (auto& ListenAddress = Private::GetListenAddressStorage();
				ListenAddress.IsSet() && *ListenAddress == SteamSocket->BindAddress
				&& ListenAddress->GetPlatformPort() == SteamSocket->BindAddress.GetPlatformPort())
			{
				ListenAddress.Reset();
			}
		}

		SteamSocket->Close();

		// Letting go of the last reference is what actually destroys it, so anybody still holding one
		// keeps working until they are done.
		OwnedSockets.RemoveAll([SteamSocket](const TSharedRef<FSocketSteam>& OwnedSocket)
		{
			return &OwnedSocket.Get() == SteamSocket;
		});
	}

	FAddressInfoResult FSocketSubsystemSteam::GetAddressInfo(const TCHAR* HostName, const TCHAR* ServiceName, EAddressInfoFlags QueryFlags,
		const FName ProtocolTypeName, ESocketType /*SocketType*/)
	{
		// Steam resolves nothing. A host name is either already an address this subsystem understands or it
		// is not an address at all, so the query is answered from what can be parsed and from what this
		// process is itself reachable on; the flags asking for a lookup have nowhere to go.
		FAddressInfoResult ResultData(HostName, ServiceName);

		const bool bAnyProtocol = ProtocolTypeName.IsNone();
		const bool bKnownProtocol = bAnyProtocol
			|| ProtocolTypeName == SteamRelayProtocol
			|| ProtocolTypeName == SteamIpProtocol;

		// A service name here is a port number and nothing else: there is no service database to consult.
		// IsNumeric also accepts a leading sign and a decimal point, so the value itself is checked below.
		const bool bServiceNamesAPort = ServiceName == nullptr || FCString::IsNumeric(ServiceName);

		if ((HostName == nullptr && ServiceName == nullptr) || !bServiceNamesAPort || !bKnownProtocol)
		{
			ResultData.ReturnCode = SE_EINVAL;
			return ResultData;
		}

		// With no host named, or when the caller is looking for somewhere to bind, the answer is what this
		// process can be reached on rather than anything belonging to a peer.
		TArray<TSharedPtr<FInternetAddr>> Candidates;
		if (HostName == nullptr || EnumHasAnyFlags(QueryFlags, EAddressInfoFlags::BindableAddress))
		{
			GetLocalAdapterAddresses(Candidates);
		}
		else if (const auto ParsedAddress = GetAddressFromString(HostName); ParsedAddress.IsValid())
		{
			Candidates.Add(ParsedAddress);
		}

		if (Candidates.IsEmpty())
		{
			ResultData.ReturnCode = SE_NO_DATA;
			return ResultData;
		}

		const int32 Port = ServiceName != nullptr ? FCString::Atoi(ServiceName) : INDEX_NONE;
		if (ServiceName != nullptr && (Port < 0 || Port > MAX_uint16))
		{
			ResultData.ReturnCode = SE_EINVAL;
			return ResultData;
		}

		for (const auto& Candidate : Candidates)
		{
			if (!bAnyProtocol && Candidate->GetProtocolType() != ProtocolTypeName)
			{
				continue;
			}

			// A relay address carries a channel rather than a port, and a channel is one byte wide, where an
			// IP address takes the whole range. A candidate which cannot hold the service name asked for is
			// left out rather than offered with a port it quietly did not take.
			if (const int32 WidestPort = Candidate->GetProtocolType() == SteamRelayProtocol ? MAX_uint8 : MAX_uint16; Port > WidestPort)
			{
				continue;
			}

			// Each candidate was allocated for this call, so giving it the port affects nobody else.
			if (Port >= 0)
			{
				Candidate->SetPort(Port);
			}

			// Datagram is what CreateSocket makes, and the only thing this subsystem can hand back.
			ResultData.Results.Emplace(Candidate.ToSharedRef(), 0, Candidate->GetProtocolType(), SOCKTYPE_Datagram);
		}

		// Being asked for a protocol none of the candidates speaks is a different failure from having
		// nothing to offer at all, and callers act on the two differently.
		ResultData.ReturnCode = !ResultData.Results.IsEmpty()
			? SE_NO_ERROR
			: (bAnyProtocol ? SE_EFAULT : SE_ADDRFAMILY);

		return ResultData;
	}

	TSharedPtr<FInternetAddr> FSocketSubsystemSteam::GetAddressFromString(const FString& IPAddress)
	{
		auto NewAddr = StaticCastSharedRef<FSteamNetAddress>(CreateInternetAddr());

		if (IPAddress.IsEmpty())
		{
			// An empty string names no peer, and there is no obvious default between this process's own
			// identity and a wildcard, so the wildcard is used: it binds, and it connects to nothing by
			// accident.
			NewAddr->SetAnyAddress();

			return NewAddr;
		}

		bool bIsAddrValid = false;
		NewAddr->SetIp(*IPAddress, bIsAddrValid);

		if (!bIsAddrValid)
		{
			return nullptr;
		}

		return NewAddr;
	}

	bool FSocketSubsystemSteam::GetHostName(FString& /*HostName*/)
	{
		// There is no name resolution to perform: Steam addresses are identities, not host names.
		UE_LOG(LogOnlineSocketsSteam, Error, TEXT("There is no host name to report: a Steam address is an identity"));
		return false;
	}

	TSharedRef<FInternetAddr> FSocketSubsystemSteam::CreateInternetAddr()
	{
		return MakeShared<FSteamNetAddress>();
	}

	TSharedRef<FInternetAddr> FSocketSubsystemSteam::CreateInternetAddr(const FName RequestedProtocol)
	{
		return MakeShared<FSteamNetAddress>(RequestedProtocol);
	}

	bool FSocketSubsystemSteam::GetLocalAdapterAddresses(TArray<TSharedPtr<FInternetAddr>>& OutAddresses)
	{
		// Over the relay this process is reachable at its own Steam identity.
		if (IsUsingRelayNetwork())
		{
			auto IdentityAddress = GetIdentityAddress();
			if (IdentityAddress.IsValid())
			{
				OutAddresses.Add(IdentityAddress);
			}
		}

		// The wildcard is always offered, because it is what a listen socket binds to.
		const auto AnyAddress = CreateInternetAddr(SteamIpProtocol);
		AnyAddress->SetAnyAddress();

		OutAddresses.Add(AnyAddress);

		return true;
	}

	TArray<TSharedRef<FInternetAddr>> FSocketSubsystemSteam::GetLocalBindAddresses()
	{
		TArray<TSharedRef<FInternetAddr>> OutAddresses;
		TArray<TSharedPtr<FInternetAddr>> AdapterAddresses;
		GetLocalAdapterAddresses(AdapterAddresses);

		auto MultihomeAddress = CreateInternetAddr(SteamIpProtocol);
		if (GetMultihomeAddress(MultihomeAddress))
		{
			OutAddresses.Add(MultihomeAddress);
		}

		for (const auto& AdapterAddress : AdapterAddresses)
		{
			OutAddresses.Add(AdapterAddress.ToSharedRef());
		}

		return OutAddresses;
	}

	UE::Online::IOnlineServicesPtr FSocketSubsystemSteam::GetOnlineServices()
	{
		return UE::Online::GetServices(UE::Online::EOnlineServices::Steam);
	}

	UE::Online::IAuthPtr FSocketSubsystemSteam::GetAuthInterface()
	{
		const UE::Online::IOnlineServicesPtr OnlineServices = GetOnlineServices();
		return OnlineServices.IsValid() ? OnlineServices->GetAuthInterface() : nullptr;
	}

	UE::Online::FAccountId FSocketSubsystemSteam::GetLocalAccountId(const FPlatformUserId PlatformUserId)
	{
		const UE::Online::IAuthPtr Auth = GetAuthInterface();
		if (!Auth.IsValid())
		{
			return UE::Online::FAccountId { };
		}

		const UE::Online::TOnlineResult<UE::Online::FAuthGetLocalOnlineUserByPlatformUserId> AccountResult =
			Auth->GetLocalOnlineUserByPlatformUserId({ .PlatformUserId = PlatformUserId });

		if (AccountResult.IsError())
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("No account for platform user [%s]: %s"),
				*UE::Online::ToLogString(PlatformUserId), *AccountResult.GetErrorValue().GetLogString());

			return UE::Online::FAccountId { };
		}

		return AccountResult.GetOkValue().AccountInfo->AccountId;
	}

	CSteamID FSocketSubsystemSteam::GetLocalSteamId(const FPlatformUserId PlatformUserId)
	{
		const UE::Online::FAccountId AccountId = GetLocalAccountId(PlatformUserId);
		if (!AccountId.IsValid())
		{
			return k_steamIDNil;
		}

		const TArray<uint8> ReplicationData = UE::Online::FOnlineIdRegistryRegistry::Get().ToReplicationData(AccountId);
		if (ReplicationData.Num() != sizeof(uint64))
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("The account of platform user [%s] does not carry a Steam identity."),
				*UE::Online::ToLogString(PlatformUserId));

			return k_steamIDNil;
		}

		uint64 SteamId { 0 };
		FMemory::Memcpy(&SteamId, ReplicationData.GetData(), sizeof(SteamId));

		return CSteamID(SteamId);
	}

	const FSteamPlatformConfig& FSocketSubsystemSteam::GetSteamConfig()
	{
		// The subsystem is only ever registered while an API is up, so the defaults below are a fallback
		// for the moment the process is tearing that API down.
		static const FSteamPlatformConfig DefaultConfig { };

		const FSteamPlatformConfig* RunningConfig = GetRunningSteamConfig();

		return RunningConfig != nullptr ? *RunningConfig : DefaultConfig;
	}

	FSocketSubsystemSteam::FOnListenAddressChanged& FSocketSubsystemSteam::OnListenAddressChanged()
	{
		static FOnListenAddressChanged ListenAddressChanged { };
		return ListenAddressChanged;
	}

	const TOptional<FSteamNetAddress>& FSocketSubsystemSteam::GetListenAddress()
	{
		return Private::GetListenAddressStorage();
	}

	void FSocketSubsystemSteam::SetListenAddress(const FSteamNetAddress& ListenAddress)
	{
		Private::GetListenAddressStorage().Emplace(ListenAddress);

		UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Listening for players on %s"), *ListenAddress.ToString(true));

		OnListenAddressChanged().Broadcast(ListenAddress);
	}

	bool FSocketSubsystemSteam::ShouldOverrideDefaultSubsystem() const
	{
		return GetSteamConfig().bOverrideDefaultSubsystem;
	}

	bool FSocketSubsystemSteam::IsUsingRelayNetwork() const
	{
		// Addressing a peer by its Steam identity rather than by an IP is what the relay network is for,
		// and both transports do it the same way.
		const FSteamPlatformConfig& SteamConfig = GetSteamConfig();
		return SteamConfig.bUseSteamTransport && SteamConfig.bOverrideDefaultSubsystem;
	}

	ESteamTransport FSocketSubsystemSteam::GetTransport()
	{
		return GetSteamConfig().Transport;
	}

	TSharedPtr<FInternetAddr> FSocketSubsystemSteam::GetIdentityAddress()
	{
		auto PeerAddress = StaticCastSharedRef<FSteamNetAddress>(CreateInternetAddr());

		SteamNetworkingIdentity SteamIdentity { };
		if (const auto NetworkingSockets = UNetDriverSteam::GetNetworkingSockets();
			NetworkingSockets != nullptr && NetworkingSockets->GetIdentity(&SteamIdentity))
		{
			PeerAddress->SetSteamIdentity(SteamIdentity);
			return PeerAddress;
		}

		if (const CSteamID CurrentUser = GetLocalSteamId(SteamGameServerPlatformId); CurrentUser.IsValid())
		{
			PeerAddress->SetSteamID(CurrentUser);
			return PeerAddress;
		}

		UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("The local user has no usable Steam identity"));
		return nullptr;
	}

	FSocketSubsystemSteam::FSocketRecord* FSocketSubsystemSteam::GetSocketInfo(uint32 InternalSocketHandle)
	{
		if (InternalSocketHandle == k_HSteamNetConnection_Invalid)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Nothing is filed under an invalid socket handle"));
			return nullptr;
		}

		return SocketInformationMap.Find(InternalSocketHandle);
	}

	FSocketSubsystemSteam::FSocketRecord* FSocketSubsystemSteam::GetSocketInfo(const FInternetAddr& ForAddress)
	{
		if (!ForAddress.IsValid())
		{
			return nullptr;
		}

		// Looking a socket up by address walks the map and can match more than one socket, so the handle is
		// the way in wherever the caller has one. This exists for the places which do not.
		for (FSocketHandleInfoMap::TIterator It(SocketInformationMap); It; ++It)
		{
			// A record is marked rather than erased, so an address lookup which ignored the mark would keep
			// handing out sockets which are already gone. The lookup by handle deliberately does not: both
			// DestroySocket and the driver shutdown have to find a record a disconnection already marked.
			if (It.Value().IsMarkedForDeletion())
			{
				continue;
			}

			if (It.Value() == ForAddress)
			{
				return &It.Value();
			}
		}

		return nullptr;
	}

	void FSocketSubsystemSteam::AddSocket(const FInternetAddr& ForAddr, const TSharedRef<FSocketSteam>& NewSocket,
		const TSharedPtr<FSocketSteam>& ParentSocket)
	{
		if (!ForAddr.IsValid())
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Refusing to file a socket with no handle"));
			return;
		}

		if (NewSocket->SteamHandle == k_HSteamListenSocket_Invalid || NewSocket->SteamHandle == k_HSteamNetConnection_Invalid)
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Dropped a socket that had no handle to file it under"));
			return;
		}

		// A record which is only marked still occupies the handle, and Steam reuses handles: without this a
		// recycled one would stay filed under the socket which is already gone, and the new socket would be
		// invisible to every lookup.
		if (const auto ExistingSocketInfo = SocketInformationMap.Find(NewSocket->SteamHandle);
			ExistingSocketInfo != nullptr && !ExistingSocketInfo->IsMarkedForDeletion())
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("A socket with the address %s already exists, will not add another!"), *ForAddr.ToString(true));
			return;
		}

		UE_LOG(LogOnlineSocketsSteam, Log, TEXT("Now tracking socket %u for addr %s, has parent? %d"),
			NewSocket->SteamHandle, *ForAddr.ToString(true), ParentSocket != nullptr);

		SocketInformationMap.Emplace(NewSocket->SteamHandle, FSocketRecord(ForAddr.Clone(), NewSocket, ParentSocket));
	}

	void FSocketSubsystemSteam::RemoveSocketsForListener(const FSocketSteam* ListenerSocket)
	{
		if (ListenerSocket == nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Refusing to close the children of a listener with no handle"));
			return;
		}

		// Steam closes every connection a listen socket accepted, but frees nothing and tells nobody, so
		// the children are found and taken down here.
		UE_LOG(LogOnlineSocketsSteam, Log, TEXT("Closing every connection listener %u accepted"), ListenerSocket->SteamHandle);

		for (FSocketHandleInfoMap::TIterator It(SocketInformationMap); It; ++It)
		{
			if (FSocketRecord& SocketInfo = It.Value(); SocketInfo.Parent.Pin().Get() == ListenerSocket)
			{
				const auto AcceptedSocket = SocketInfo.Socket.Pin();
				UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Dropped socket %u"),
					AcceptedSocket.IsValid() ? AcceptedSocket->SteamHandle : k_HSteamNetConnection_Invalid);

				// Marked rather than deleted, so that the disconnection events Steam is about to raise find nothing left to act on.
				SocketInfo.Parent.Reset();

				SocketInfo.NetDriver.Reset();
				SocketInfo.MarkForDeletion();
			}
		}
	}

	void FSocketSubsystemSteam::QueueRemoval(uint32 RemoveHandle)
	{
		if (const auto SocketInfo = GetSocketInfo(RemoveHandle))
		{
			const FString Address = SocketInfo->Addr.IsValid() ? SocketInfo->Addr->ToString(true) : TEXT("INVALID");

			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Marked socket %u with address %s for removal (pending)"), RemoveHandle, *Address);
			SocketInfo->MarkForDeletion();
		}
	}

	void FSocketSubsystemSteam::SweepClosedSockets()
	{
		// Only a record whose socket object itself has gone is dropped. A record which is merely marked can
		// still be looked up by handle: RemoveSocketsForListener marks every child of a listener while those
		// children are still alive, and DestroySocket looks its own record up after marking it.
		for (FSocketHandleInfoMap::TIterator It(SocketInformationMap); It; ++It)
		{
			if (It.Value().IsMarkedForDeletion() && !It.Value().Socket.IsValid())
			{
				UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Dropped the record of socket %u"), It.Key());
				It.RemoveCurrent();
			}
		}
	}

	void FSocketSubsystemSteam::LinkNetDriver(FSocket* Socket, UNetDriverSteam* NewNetDriver)
	{
		if (Socket == nullptr || NewNetDriver == nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Refusing to file a driver against a socket with no handle"));
			return;
		}

		auto SteamSocket = static_cast<FSocketSteam*>(Socket);

		if (SteamSocket->SteamHandle == k_HSteamListenSocket_Invalid || SteamSocket->SteamHandle == k_HSteamNetConnection_Invalid)
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Dropped the driver of a socket that had no handle"));
			return;
		}

		if (const auto SocketInfo = GetSocketInfo(SteamSocket->SteamHandle))
		{
			SocketInfo->NetDriver = NewNetDriver;
		}
	}

	bool FSocketSubsystemSteam::AddDelayedListener(FSocketSteam* ListenSocket, UNetDriverSteam* NetDriver)
	{
		if (ListenSocket == nullptr || NetDriver == nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, Warning, TEXT("Cannot hold a listener open without both a socket and a driver"));
			return false;
		}

		const auto Auth = GetAuthInterface();

		// Which account has to appear before this process can host: a dedicated server waits for the game
		// server it logs on anonymously, and a client hosting a listen server waits for the player. The
		// client case is the one bInitServerOnClient answers for; a dedicated server never runs a client API
		// and used to be refused here by a flag that has nothing to say about it.
		const bool bIsDedicatedServer = IsRunningDedicatedServer();
		const FPlatformUserId PlatformUserId = bIsDedicatedServer ? SteamGameServerPlatformId : SteamClientPlatformId;

		if (!Auth.IsValid() || (!bIsDedicatedServer && !GetSteamConfig().bInitServerOnClient))
		{
			UE_LOG(LogOnlineSocketsSteam, Error, TEXT("Nothing in this process can log in to host, so the listener cannot be held open"));
			return false;
		}

		// A login takes as long as it takes, and both the socket and the driver are free to go away while it
		// does: hosting can be given up, and travel destroys the driver outright. Only "this" is held raw,
		// and deliberately: FOnlineServicesSteam::Destroy cancels the operation queue, which fires the
		// completions, before it shuts this subsystem down.
		const TWeakPtr<FSocketSteam> WeakListenSocket = ListenSocket->AsSteamShared();
		const TWeakObjectPtr<UNetDriverSteam> WeakNetDriver { NetDriver };

		using FLoginResult = UE::Online::TOnlineResult<UE::Online::FAuthLogin>;

		Auth->Login({ .PlatformUserId = PlatformUserId }).OnComplete(
			[this, WeakListenSocket, WeakNetDriver](const FLoginResult& Result)
		{
			const auto DelayedSocket = WeakListenSocket.Pin();
			const auto DelayedDriver = WeakNetDriver.Get();

			if (!DelayedSocket.IsValid() || DelayedDriver == nullptr)
			{
				UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("The login finished after whoever was waiting for it had gone"));
				return;
			}

			if (Result.IsError())
			{
				UE_LOG(LogOnlineSocketsSteam, Error, TEXT("Hosting needs a signed in user and the login failed: %s"), *Result.GetErrorValue().GetLogString());
				DelayedDriver->Shutdown();

				return;
			}

			// The identity comes from the login just performed, and it can still come back with nothing.
			const auto ListenerAddress = StaticCastSharedPtr<FSteamNetAddress>(GetIdentityAddress());
			if (!ListenerAddress.IsValid())
			{
				UE_LOG(LogOnlineSocketsSteam, Error, TEXT("The login left no Steam identity to listen on"));
				DelayedDriver->Shutdown();

				return;
			}

			ListenerAddress->SetPlatformPort(DelayedSocket->BindAddress.GetPlatformPort());

			DelayedSocket->BindAddress = *ListenerAddress;
			DelayedDriver->LocalAddr = ListenerAddress;

			if (!DelayedSocket->Listen(0))
			{
				UE_LOG(LogOnlineSocketsSteam, Error, TEXT("The identity was there but the socket would not listen on it"));
				DelayedDriver->Shutdown();

				return;
			}

			LinkNetDriver(DelayedSocket.Get(), DelayedDriver);

			// The driver was ticking slowly while it waited for the login; it is needed at full rate now.
			DelayedDriver->bIsDelayedNetworkAccess = false;

			SetListenAddress(*ListenerAddress);

			DelayedDriver->PublishServerDetails();

			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Processed delayed listener: %s"), *DelayedDriver->GetDescription());
		});

		return true;
	}

	static FString LexToString(const ESteamNetworkingConnectionState ConnectionState)
	{
		switch (ConnectionState)
		{
			default:
			case k_ESteamNetworkingConnectionState_None:
				return TEXT("None");
			case k_ESteamNetworkingConnectionState_Connecting:
				return TEXT("Connecting");
			case k_ESteamNetworkingConnectionState_FindingRoute:
				return TEXT("Routing");
			case k_ESteamNetworkingConnectionState_Connected:
				return TEXT("Connected");
			case k_ESteamNetworkingConnectionState_ClosedByPeer:
				return TEXT("Peer closed it");
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
				return TEXT("Problem on this end");
		}
	}


	void FSocketSubsystemSteam::OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* Message)
	{
		if (Message == nullptr || UNetDriverSteam::GetNetworkingSockets() == nullptr)
		{
			return;
		}

		if (Message->m_eOldState == k_ESteamNetworkingConnectionState_None &&
			Message->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting &&
			Message->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid)
		{
			if (const auto SocketInfo = GetSocketInfo(Message->m_info.m_hListenSocket))
			{
				if (const auto NetDriver = SocketInfo->NetDriver.Get())
				{
					NetDriver->OnConnectionCreated(Message->m_info.m_hListenSocket, Message->m_hConn);
				}
			}
		}
		else if ((Message->m_eOldState == k_ESteamNetworkingConnectionState_Connecting
			|| Message->m_eOldState == k_ESteamNetworkingConnectionState_FindingRoute)
			&& Message->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected)
		{
			if (const auto SocketInfo = GetSocketInfo(Message->m_hConn))
			{
				if (const auto NetDriver = SocketInfo->NetDriver.Get())
				{
					NetDriver->OnConnectionUpdated(Message->m_hConn, static_cast<int32>(Message->m_info.m_eState));
				}
			}
		}
		else if ((Message->m_eOldState == k_ESteamNetworkingConnectionState_FindingRoute
			|| Message->m_eOldState == k_ESteamNetworkingConnectionState_Connecting
			|| Message->m_eOldState == k_ESteamNetworkingConnectionState_Connected)
			&& (Message->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer
				|| Message->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally))
		{
			const auto SocketInfo = GetSocketInfo(Message->m_hConn);

			if (const auto ChangedSocket = SocketInfo != nullptr ? SocketInfo->Socket.Pin() : nullptr;
				ChangedSocket.IsValid() && !ChangedSocket->bIsListenSocket)
			{
				UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Connection %u has disconnected. Old state: %s Reason: %s"), Message->m_hConn,
					*LexToString(Message->m_eOldState), *LexToString(Message->m_info.m_eState));

				if (const auto NetDriver = SocketInfo->NetDriver.Get())
				{
					NetDriver->OnConnectionDisconnected(Message->m_hConn);
				}

				// A connection which went away because its listen socket was destroyed is already accounted for,
				// so only a genuine disconnection is reported onwards. The two are told apart by the parent
				// handle: a server clears it on every socket it owns, and a client never had one to begin with.
				if (SocketInfo->Parent.IsValid() || Message->m_info.m_hListenSocket == k_HSteamListenSocket_Invalid)
				{
					// The tick does the deletion, so that nothing is freed inside a Steam callback.
					SocketInfo->MarkForDeletion();
				}
			}
		}
	}

	void FSocketSubsystemSteam::FSocketRecord::MarkForDeletion()
	{
		bMarkedForDeletion = true;

		const auto MarkedDriver = NetDriver.Get();

		if (const auto MarkedSocket = Socket.Pin(); MarkedSocket.IsValid() && MarkedDriver != nullptr)
		{
			UE_LOG(LogOnlineSocketsSteam, Verbose, TEXT("Handle %u marked for deletion. Has parent? %d NetDriver definition: %s"),
				MarkedSocket->SteamHandle, Parent.IsValid(), *MarkedDriver->GetDescription());
		}
	}

	bool FSocketSubsystemSteam::FSocketRecord::operator==(const FInternetAddr& InAddr) const
	{
		const auto& PeerAddress = static_cast<const FSteamNetAddress&>(InAddr);
		return *Addr == PeerAddress;
	}

	FString FSocketSubsystemSteam::FSocketRecord::ToString() const
	{
		const auto DescribedSocket = Socket.Pin();

		return FString::Printf(TEXT("SocketInfo: Addr [%s], Socket [%u], Status [%d], Listener [%d], HasNetDriver [%d], MarkedForDeletion [%d]"),
			Addr.IsValid() ? *Addr->ToString(true) : TEXT("INVALID"), DescribedSocket.IsValid() ? DescribedSocket->SteamHandle : 0,
			DescribedSocket.IsValid() ? static_cast<int32>(DescribedSocket->GetConnectionState()) : -1,
			Parent.IsValid(),
			NetDriver.IsValid(), bMarkedForDeletion);
	}
}
