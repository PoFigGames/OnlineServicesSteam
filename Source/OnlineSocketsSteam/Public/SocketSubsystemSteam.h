// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "SocketSubsystem.h"
#include "SocketSteam.h"
#include "SocketSteamMessages.h"
#include "SteamPlatformConfig.h"
#include "Online/OnlineServices.h"


class ISteamNetworkingSockets;
class UNetConnectionSteam;
class UNetDriverSteam;


namespace PoFigGames::Steam
{
	/**
	 * @class FSocketSubsystemSteam
	 *
	 * @brief The socket subsystem this plugin's transport is built on.
	 *
	 * Steam addresses a peer by identity rather than by host and port, so this subsystem hands out sockets
	 * which speak either to the Steam Datagram Relay or straight to an IP, keeps the bookkeeping tying a
	 * Steam connection handle back to the socket and driver which own it, and answers for the events Steam
	 * raises about connections and about the connectionless transport.
	 */
	class FSocketSubsystemSteam final : public ISocketSubsystem
	{
		friend class FSocketSteam;
		friend class FSocketSteamMessages;
		friend class ::UNetDriverSteam;
		friend class ::UNetConnectionSteam;

	public:
		ONLINESOCKETSSTEAM_API FSocketSubsystemSteam()
			: OnMessagesSessionRequestCallback(this, &FSocketSubsystemSteam::OnMessagesSessionRequest)
			, OnMessagesSessionFailedCallback(this, &FSocketSubsystemSteam::OnMessagesSessionFailed)
			, OnClientConnectionStatusChangedCallback(this, &FSocketSubsystemSteam::OnConnectionStatusChanged)
			, OnServerConnectionStatusChangedCallback(this, &FSocketSubsystemSteam::OnConnectionStatusChanged)
		{
		}
		ONLINESOCKETSSTEAM_API virtual ~FSocketSubsystemSteam() override = default;

		/**
		 * The Steam online services of this process, or null while they are not up.
		 *
		 * The subsystem talks to the services through the published interfaces alone, so that it neither
		 * depends on the module implementing them nor has to be handed anything by it.
		 */
		static ONLINESOCKETSSTEAM_API UE::Online::IOnlineServicesPtr GetOnlineServices();
		static ONLINESOCKETSSTEAM_API UE::Online::IAuthPtr GetAuthInterface();

		/** The account of a signed in local user, or an invalid id when there is none. */
		static ONLINESOCKETSSTEAM_API UE::Online::FAccountId GetLocalAccountId(FPlatformUserId PlatformUserId);

		/**
		 * The Steam identity behind a local user.
		 *
		 * An account id says nothing about the service behind it, but its replication form is exactly what
		 * the service needs to rebuild it elsewhere, which for Steam is the identity itself.
		 */
		static ONLINESOCKETSSTEAM_API CSteamID GetLocalSteamId(FPlatformUserId PlatformUserId);

		/** The settings the running Steamworks API was brought up with. */
		static ONLINESOCKETSSTEAM_API const FSteamPlatformConfig& GetSteamConfig();

		/** Which Steam networking API the sockets of this subsystem are built on. */
		static ONLINESOCKETSSTEAM_API ESteamTransport GetTransport();

		/**
		 * Announced when this process starts listening for players.
		 *
		 * Binding a server to a lobby is something only the owner of that lobby can do, and lobbies live
		 * in the module which implements the online services, which depends on this one. The socket layer
		 * therefore says where it listens and leaves the binding to whoever holds a lobby.
		 */
		DECLARE_MULTICAST_DELEGATE_OneParam(FOnListenAddressChanged, const FSteamNetAddress& /*ListenAddress*/);
		static ONLINESOCKETSSTEAM_API FOnListenAddressChanged& OnListenAddressChanged();

		/** Where this process listens for players, or nothing while it hosts no game. */
		static ONLINESOCKETSSTEAM_API const TOptional<FSteamNetAddress>& GetListenAddress();

	#pragma region ISocketSubsystem
		ONLINESOCKETSSTEAM_API virtual bool Init(FString& Error) override;
		ONLINESOCKETSSTEAM_API virtual void Shutdown() override;

		virtual FSocket* CreateSocket(const FName& SocketType, const FString& SocketDescription, const FName& ProtocolType) override;
		virtual void DestroySocket(FSocket* Socket) override;

		virtual FResolveInfoCached* CreateResolveInfoCached(TSharedPtr<FInternetAddr> Addr) const override { return nullptr; }

		virtual FAddressInfoResult GetAddressInfo(const TCHAR* HostName, const TCHAR* ServiceName = nullptr,
			EAddressInfoFlags QueryFlags = EAddressInfoFlags::Default,
			const FName ProtocolTypeName = NAME_None, ESocketType SocketType = SOCKTYPE_Unknown) override;
		virtual TSharedPtr<FInternetAddr> GetAddressFromString(const FString& IPAddress) override;

		virtual bool GetHostName(FString& HostName) override;

		virtual TSharedRef<FInternetAddr> CreateInternetAddr() override;
		virtual TSharedRef<FInternetAddr> CreateInternetAddr(const FName RequestedProtocol) override;

		virtual const TCHAR* GetSocketAPIName() const override { return TEXT("SocketsSteam"); }

		virtual ESocketErrors GetLastErrorCode() override { return TranslateErrorCode(LastSocketError); }
		virtual ESocketErrors TranslateErrorCode(int32 Code) override { return static_cast<ESocketErrors>(Code); }

		virtual bool GetLocalAdapterAddresses(TArray<TSharedPtr<FInternetAddr>>& OutAddresses) override;
		virtual TArray<TSharedRef<FInternetAddr>> GetLocalBindAddresses() override;

		virtual bool HasNetworkDevice() override { return true; }
		virtual bool IsSocketWaitSupported() const override { return false; }
		virtual bool RequiresChatDataBeSeparate() override { return false; }
		virtual bool RequiresEncryptedPackets() override { return false; }
		#pragma endregion

		/** Set last socket error for UE */
		FORCEINLINE void SetLastSocketError(const ESocketErrors Err) { LastSocketError = Err; }

		/** Return whether a Steam subsystem should be default (from config) */
		bool ShouldOverrideDefaultSubsystem() const;

		/** Returns if the application is using the SteamSocket relays */
		bool IsUsingRelayNetwork() const;

	private:
		/**
		 * @struct FSocketRecord
		 *
		 * @brief What is known about one socket this subsystem handed out.
		 *
		 * A record is marked rather than erased when its socket goes away, because Steam raises events about
		 * a connection after it has ended and those events still have to find something to act on.
		 */
		struct FSocketRecord
		{
			FSocketRecord(const TSharedPtr<FInternetAddr>& InAddr, const TSharedPtr<FSocketSteam>& InSocket,
				const TSharedPtr<FSocketSteam>& InParent = nullptr)
				: Addr(InAddr)
				, Socket(InSocket)
				, Parent(InParent)
			{
			}

			void MarkForDeletion();

			bool IsMarkedForDeletion() const { return bMarkedForDeletion; }

			bool operator==(const FSocketSteam* RHS) const
			{
				return Socket.Pin().Get() == RHS;
			}

			bool operator==(const FInternetAddr& InAddr) const;

			bool operator==(const FSocketRecord& RHS) const
			{
				return RHS.Addr == Addr && RHS.Socket.Pin() == Socket.Pin();
			}

			/** Whether this record still describes a socket which can be used. */
			bool IsValid() const
			{
				return Addr.IsValid() && Socket.IsValid() && !bMarkedForDeletion;
			}

			FString ToString() const;

			TSharedPtr<FInternetAddr> Addr { nullptr };

			/**
			 * The socket this entry describes and, for a socket a listener accepted, the listener it came
			 * from. Both are weak: the bookkeeping outlives neither, it only describes what is alive.
			 */
			TWeakPtr<FSocketSteam> Socket { nullptr };
			TWeakPtr<FSocketSteam> Parent { nullptr };

			TWeakObjectPtr<UNetDriverSteam> NetDriver { nullptr };

		private:
			bool bMarkedForDeletion { false };
		};

		/**
		 * A peer wants to talk to this process over the connectionless transport.
		 *
		 * There is no listen socket to answer for, so the socket which owns the channel takes the session
		 * in and hands out a socket for it the next time the driver accepts one.
		 */
		ONLINESOCKETSSTEAM_API void OnMessagesSessionRequest(SteamNetworkingMessagesSessionRequest_t* Message);

		/** A session broke; the socket speaking to that peer is of no further use. */
		ONLINESOCKETSSTEAM_API void OnMessagesSessionFailed(SteamNetworkingMessagesSessionFailed_t* Message);

		/**
		 * The socket reading the channel of the connectionless transport, if this process has one.
		 *
		 * On a host that is the socket which was listened on; on a client it is the socket which connected
		 * out, because nobody accepted it and so nobody reads the channel on its behalf. Asking for a listen
		 * socket alone left a client with no way to hear that its session had failed.
		 */
		ONLINESOCKETSSTEAM_API TSharedPtr<FSocketSteamMessages> FindMessagesChannelOwner() const;

		/**
		 * A connection changed state.
		 *
		 * Steam hands this event to every registration the process holds, so it is held once, here, rather
		 * than once per socket: registering per socket made one incoming connection run the accept path
		 * once for every socket alive, and all but the first failed noisily and overwrote the last error.
		 * Which socket an event belongs to is decided by the handle it carries, not by who was told.
		 */
		ONLINESOCKETSSTEAM_API void OnConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* Message);

		// Registered for the lifetime of the subsystem; they answer for the connectionless transport only.
		CCallback<FSocketSubsystemSteam, SteamNetworkingMessagesSessionRequest_t> OnMessagesSessionRequestCallback;
		CCallback<FSocketSubsystemSteam, SteamNetworkingMessagesSessionFailed_t> OnMessagesSessionFailedCallback;

		// Both pipes are listened to, because which of them delivers these on a client hosted listen server
		// is decided by how the process came up.
		CCallback<FSocketSubsystemSteam, SteamNetConnectionStatusChangedCallback_t> OnClientConnectionStatusChangedCallback;
		CCallback<FSocketSubsystemSteam, SteamNetConnectionStatusChangedCallback_t, true> OnServerConnectionStatusChangedCallback;

		ESocketErrors LastSocketError { SE_NO_ERROR };

		/**
		 * The sockets this subsystem handed out. The engine asks for a socket and gives it back by raw
		 * pointer, so the subsystem is the one holding it alive in between.
		 */
		TArray<TSharedRef<FSocketSteam>> OwnedSockets { };

		/** Active connection bookkeeping */
		using FSocketHandleInfoMap = TMap<uint32, FSocketRecord>;
		FSocketHandleInfoMap SocketInformationMap { };

		/** Records where this process listens and tells everybody who asked to be told. */
		static ONLINESOCKETSSTEAM_API void SetListenAddress(const FSteamNetAddress& ListenAddress);

		// Returns this machine's identity in the form of a FSteamNetAddress
		TSharedPtr<FInternetAddr> GetIdentityAddress();

		// Steam socket queriers
		ONLINESOCKETSSTEAM_API FSocketRecord* GetSocketInfo(uint32 InternalSocketHandle);
		ONLINESOCKETSSTEAM_API FSocketRecord* GetSocketInfo(const FInternetAddr& ForAddress);

		ONLINESOCKETSSTEAM_API void AddSocket(const FInternetAddr& ForAddr, const TSharedRef<FSocketSteam>& NewSocket,
			const TSharedPtr<FSocketSteam>& ParentSocket = nullptr);
		ONLINESOCKETSSTEAM_API void RemoveSocketsForListener(const FSocketSteam* ListenerSocket);
		ONLINESOCKETSSTEAM_API void QueueRemoval(uint32 SocketHandle);

		/**
		 * Drops the records whose socket is gone for good.
		 *
		 * Records are marked from inside Steam callbacks, which is the one place the map must not be
		 * rearranged: a callback is usually reached with a pointer into it still on the stack. Nothing
		 * erased them afterwards either, so the map grew for the life of the process and the lookup by
		 * address walked every connection ever made. Call this from a tick, never from a callback.
		 */
		ONLINESOCKETSSTEAM_API void SweepClosedSockets();
		ONLINESOCKETSSTEAM_API void LinkNetDriver(FSocket* Socket, UNetDriverSteam* NewNetDriver);

		/**
		 * Holds a listener open across the login it needs before it can listen on a Steam identity.
		 *
		 * @return Whether a login was started. False means nothing is going to happen and the caller has to
		 *         report that itself, rather than leaving the driver waiting for a completion which will
		 *         never come.
		 */
		ONLINESOCKETSSTEAM_API bool AddDelayedListener(FSocketSteam* ListenSocket, UNetDriverSteam* NewNetDriver);
	};
}
