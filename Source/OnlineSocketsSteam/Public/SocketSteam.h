// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Sockets.h"
#include "SteamNetAddress.h"


THIRD_PARTY_INCLUDES_START
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
THIRD_PARTY_INCLUDES_END


class UNetConnectionSteam;
class UNetDriverSteam;


namespace PoFigGames::Steam
{
	class FSocketSubsystemSteam;

	/**
	 * @struct FSteamNetworkingMessageDeleter
	 *
	 * @brief Hands a received message back to Steam.
	 *
	 * A message read off a connection is reference counted by the API rather than owned outright, so it
	 * is given back with Release rather than deleted.
	 */
	struct FSteamNetworkingMessageDeleter
	{
		void operator()(SteamNetworkingMessage_t* Message) const
		{
			if (Message != nullptr)
			{
				Message->Release();
			}
		}
	};

	/** A received message, held for exactly as long as it is needed. */
	using FSteamNetworkingMessagePtr = TUniquePtr<SteamNetworkingMessage_t, FSteamNetworkingMessageDeleter>;

	/**
	 * @class FSocketSteam
	 *
	 * @brief Represents a socket communication stream.
	 *
	 * Provides functionality for sending and receiving data
	 * over a socket connection using a stream-oriented approach.
	 */
	class FSocketSteam : public FSocket
	{
		friend class FSocketSubsystemSteam;
		friend class ::UNetDriverSteam;
		friend class ::UNetConnectionSteam;

	public:
		using Super = FSocket;

		FSocketSteam(ESocketType InType, const FString& InDescription, const FName& InProtocol);
		virtual ~FSocketSteam() override;

		/**
		 * Closes the socket
		 *
		 * @return Whether the socket closed cleanly.
		 */
		virtual bool Close() override;

		/**
		 * Binds a socket to a network byte-ordered address.
		 *
		 * @param Addr Address to take for this socket.
		 * @return Whether it worked.
		 */
		virtual bool Bind(const FInternetAddr& Addr) override;

		/**
		 * Connects a socket to a network byte-ordered address.
		 *
		 * @param Addr The peer to reach.
		 * @return Whether it worked.
		 */
		virtual bool Connect(const FInternetAddr& Addr) override;

		/**
		 * Takes over a connection the listener accepted and puts it in the same poll group, which is how
		 * the driver hears from it. Both belong to the socket, so both are set in one place.
		 */
		ONLINESOCKETSSTEAM_API void AdoptConnection(uint32 ConnectionHandle, uint32 PollGroup);

		/**
		 * Opens the socket for peers to connect to.
		 *
		 * @param MaxBacklog Ignored: Steam queues connections itself.
		 * @return Whether it worked.
		 */
		virtual bool Listen(int32 MaxBacklog) override;

		/**
		 * Takes the connection Steam is holding.
		 *
		 * @param InSocketDescription Debug description of socket,
		 * @return The socket for that connection, or nullptr.
		 */
		virtual FSocket* Accept(const FString& InSocketDescription) override;
		virtual FSocket* Accept(FInternetAddr& OutAddr, const FString& InSocketDescription) override { return nullptr; }

		/**
		 * Sends a buffer to a network byte-ordered address
		 *
		 * @param Data Bytes to send.
		 * @param Count How many.
		 * @param BytesSent Receives how many went.
		 * @param Dest the network byte ordered address to send to
		 */
		virtual bool SendTo(const uint8* Data, int32 Count, int32& BytesSent, const FInternetAddr& Dest) override;

		/**
		 * Sends over an open connection.
		 *
		 * @param Data Bytes to send.
		 * @param Count How many.
		 * @param BytesSent Receives how many went.
		 */
		virtual bool Send(const uint8* Data, int32 Count, int32& BytesSent) override;

		/**
		 * Reads from an open connection.
		 *
		 * Success does not mean anything arrived: BytesRead says how much did, and zero
		 * means there was nothing waiting.
		 *
		 *
		 * @param Data Buffer to fill.
		 * @param BufferSize How much it holds.
		 * @param BytesRead Receives how much arrived.
		 * @param Flags Whether this is a peek or a read.
		 * @return False once the connection is closed or broken.
		 */
		virtual bool Recv(uint8* Data, int32 BufferSize, int32& BytesRead, ESocketReceiveFlags::Type Flags = ESocketReceiveFlags::None) override;

		/**
		 * Whether anything is waiting to be read.
		 *
		 * @param OutPendingDataSize out parameter indicating how much data is on the pipe for a single recv call
		 *
		 * @return Whether a message is queued.
		 */
		virtual bool HasPendingData(uint32& OutPendingDataSize) override;

		/**
		 * What Steam reports about this connection.
		 *
		 * @return Connection state.
		 */
		virtual ESocketConnectionState GetConnectionState() override;

		/**
		 * The address this socket took.
		 *
		 * @param OutAddr Receives that address.
		 */
		virtual void GetAddress(FInternetAddr& OutAddr) override { OutAddr = static_cast<FInternetAddr&>(BindAddress); }

		/**
		 * The peer at the other end.
		 *
		 * @param OutAddr Receives the peer's address.
		 * @return Whether there was one to report.
		 */
		virtual bool GetPeerAddress(FInternetAddr& OutAddr) override;

		/**
		 * Asks Steam not to hold small messages back.
		 *
		 * @param bIsNoDelay Whether to send at once.
		 * @return Whether it worked.
		 */
		virtual bool SetNoDelay(bool bIsNoDelay = true) override;

		/**
		 * Whether a closed socket flushes what it still holds.
		 *
		 * @param bShouldLinger Whether to flush on close.
		 * @param Timeout Ignored: Steam decides how long.
		 * @return Whether the setting took.
		 */
		virtual bool SetLinger(bool bShouldLinger = true, int32 Timeout = 0) override;

		/**
		 * Sizes the send buffer.
		 *
		 * @param Size Size asked for.
		 * @param NewSize Receives the size actually given.
		 * @return Whether the setting took.
		 */
		virtual bool SetSendBufferSize(int32 Size, int32& NewSize) override;

		/**
		 * Sizes the receive buffer, which Steam ties to the send buffer.
		 *
		 * @param Size Size asked for.
		 * @param NewSize Receives the size actually given.
		 * @return Whether the setting took.
		 */
		virtual bool SetReceiveBufferSize(int32 Size, int32& NewSize) override;

		/**
		 * The channel this socket speaks on.
		 *
		 * @return Port number.
		 * @see GetSocketType
		 */
		virtual int32 GetPortNo() override
		{
			return BindAddress.GetPort();
		}

		/**
		 * This socket as the reference the subsystem holds it by.
		 *
		 * Every socket is created through FSocketSubsystemSteam::CreateSocket, which is what makes the
		 * reference exist in the first place; FSocket brings the sharing itself.
		 */
		TSharedRef<FSocketSteam> AsSteamShared() { return StaticCastSharedRef<FSocketSteam>(AsShared()); }

		void SetSendMode(int32 NewSendMode) { SendFlags = NewSendMode; }
		void SetClosureReason(ESteamNetConnectionEnd NewClosureReason) { ClosureReason = NewClosureReason; }

	protected:
		FSocketSubsystemSteam* SocketSubsystem { nullptr };

		HSteamNetConnection SteamHandle { k_HSteamNetConnection_Invalid };
		HSteamNetPollGroup PollGroupHandle { k_HSteamNetPollGroup_Invalid };

		FSteamNetAddress BindAddress { };

		int32 SendFlags { k_nSteamNetworkingSend_UnreliableNoNagle };

		bool bLingerOnClose { false };

		// A listen socket accepts and polls; a connection sends and receives. Neither can do the other's work.
		bool bIsListenSocket { false };

		// On a LAN there is no relay to validate against, so the peer is accepted without proving who it is.
		bool bIsLANSocket { false };

		// A message a peek left behind, waiting for the read that will hand it on.
		bool bHasPendingData { false };

		// Reason reported to the peer when this socket closes.
		ESteamNetConnectionEnd ClosureReason { k_ESteamNetConnectionEnd_App_Generic };

		// This is used for peeking and looking at pending data.
		// Data is already internally handled in a queue on the SteamAPI side, this will give us whatever is at the top.
		// Only set if called with HasPendingData or Recv with a Peek flag, and owned until it is handed on.
		FSteamNetworkingMessagePtr PendingData { nullptr };

		/**
		 * Hands out the next message the transport has for this socket, if any.
		 *
		 * The caller takes the message and with it the duty of giving it back to Steam.
		 */
		virtual bool RecvRaw(SteamNetworkingMessage_t*& Data, int32 MaxMessages, int32& ReadCount, ESocketReceiveFlags::Type Flags = ESocketReceiveFlags::None);

	#pragma region Unsupported
		virtual bool Shutdown(ESocketShutdownMode Mode) override;
		virtual bool Wait(ESocketWaitConditions::Type Condition, FTimespan WaitTime) override;
		virtual bool WaitForPendingConnection(bool& bHasPendingConnection, const FTimespan& WaitTime) override;
		virtual bool SetReuseAddr(bool bAllowReuse = true) override;
		virtual bool SetRecvErr(bool bUseErrorQueue = true) override;
		virtual bool SetNonBlocking(bool bIsNonBlocking = true) override;
		virtual bool SetBroadcast(bool bAllowBroadcast = true) override;
		virtual bool JoinMulticastGroup(const FInternetAddr& GroupAddress) override;
		virtual bool JoinMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress) override;
		virtual bool LeaveMulticastGroup(const FInternetAddr& GroupAddress) override;
		virtual bool LeaveMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress) override;
		virtual bool SetMulticastLoopback(bool bLoopback) override;
		virtual bool SetMulticastTtl(uint8 TimeToLive) override;
		virtual bool SetMulticastInterface(const FInternetAddr& InterfaceAddress) override;
	#pragma endregion Unsupported
	};
}