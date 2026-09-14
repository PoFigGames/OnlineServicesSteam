// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "SocketSteam.h"

THIRD_PARTY_INCLUDES_START
#include "steam/isteamnetworkingmessages.h"
THIRD_PARTY_INCLUDES_END


namespace PoFigGames::Steam
{
	/**
	 * @class FSocketSteamMessages
	 *
	 * @brief A socket over the connectionless Steam transport.
	 *
	 * ISteamNetworkingMessages has no listen socket and no connection handle: a message is addressed to
	 * an identity on a channel, and the session behind it is established the first time somebody sends.
	 * The engine, on the other hand, expects a socket to be listened on and connections to be accepted
	 * off it, so the roles are rebuilt here on top of what the transport does offer.
	 *
	 * A socket which was listened on owns the channel: it is the one which reads it, and it hands every
	 * message to the socket of the peer it came from. A socket which was accepted or connected speaks to
	 * exactly one peer and reads what the owner of the channel left for it.
	 */
	class FSocketSteamMessages final : public FSocketSteam
	{
	public:
		using Super = FSocketSteam;

		FSocketSteamMessages(ESocketType InType, const FString& InDescription, const FName& InProtocol)
			: FSocketSteam(InType, InDescription, InProtocol)
		{
		}

		ONLINESOCKETSSTEAM_API virtual ~FSocketSteamMessages() override;

		ONLINESOCKETSSTEAM_API virtual bool Close() override;
		ONLINESOCKETSSTEAM_API virtual bool Bind(const FInternetAddr& Addr) override;
		ONLINESOCKETSSTEAM_API virtual bool Connect(const FInternetAddr& Addr) override;
		ONLINESOCKETSSTEAM_API virtual bool Listen(int32 MaxBacklog) override;
		ONLINESOCKETSSTEAM_API virtual FSocket* Accept(const FString& InSocketDescription) override;

		ONLINESOCKETSSTEAM_API virtual bool Send(const uint8* Data, int32 Count, int32& BytesSent) override;
		ONLINESOCKETSSTEAM_API virtual bool SendTo(const uint8* Data, int32 Count, int32& BytesSent, const FInternetAddr& Dest) override;
		ONLINESOCKETSSTEAM_API virtual bool Recv(uint8* Data, int32 BufferSize, int32& BytesRead, ESocketReceiveFlags::Type Flags = ESocketReceiveFlags::None) override;
		ONLINESOCKETSSTEAM_API virtual bool HasPendingData(uint32& OutPendingDataSize) override;
		ONLINESOCKETSSTEAM_API virtual bool RecvRaw(SteamNetworkingMessage_t*& Data, int32 MaxMessages, int32& ReadCount, ESocketReceiveFlags::Type Flags = ESocketReceiveFlags::None) override;

		ONLINESOCKETSSTEAM_API virtual ESocketConnectionState GetConnectionState() override;
		ONLINESOCKETSSTEAM_API virtual bool GetPeerAddress(FInternetAddr& OutAddr) override;

		/** Takes in a session the remote peer asked for, and answers whether it was taken. */
		ONLINESOCKETSSTEAM_API bool AcceptSession(const SteamNetworkingIdentity& RemoteIdentity);

		/** Reports that a session with this peer has broken, so the socket speaking to it can be closed. */
		ONLINESOCKETSSTEAM_API void HandleSessionFailed(const SteamNetworkingIdentity& RemoteIdentity);

		/** The channel this socket reads and writes on; the platform port of its address. */
		int32 GetChannel() const { return Channel; }

		/** Whether this socket is the one reading its channel, rather than being served by a listener. */
		bool OwnsChannel() const { return bOwnsChannel; }

	private:
		/** Reads whatever is waiting on the channel and files each message under the peer it came from. */
		void PumpChannel();

		/** The next message waiting for this socket, taken out of the queue it was filed in. */
		FSteamNetworkingMessagePtr TakeNextMessage();

		/** The socket speaking to one peer, or null while nobody has been accepted for it. */
		TSharedPtr<FSocketSteamMessages> FindPeerSocket(const SteamNetworkingIdentity& RemoteIdentity) const;

		/** The socket which owns the channel: this one when it was listened on, otherwise its listener. */
		TSharedPtr<FSocketSteamMessages> GetChannelOwner() const;

		int32 Channel { 0 };

		/** Who this socket speaks to. Unset on the socket which owns the channel. */
		FSteamNetAddress PeerAddress { };

		/** The listener a socket was accepted from, which is the one reading the channel for it. */
		TWeakPtr<FSocketSteamMessages> Listener { nullptr };

		/**
		 * Whether this socket reads the channel itself rather than being served by a listener.
		 *
		 * True on a socket which was listened on and on one which connected out: a client was accepted by
		 * nobody, so there is no listener reading on its behalf. Asked instead of falling back to "read it
		 * myself when I have no listener", which would let a socket whose listener has been destroyed
		 * promote itself and swallow every other peer's traffic.
		 */
		bool bOwnsChannel { false };

		/** Peers a listener has taken in, and the socket handed out for each of them. */
		TMap<uint64, TWeakPtr<FSocketSteamMessages>> AcceptedPeers { };

		/** Peers which asked for a session and have not been accepted yet, oldest first. */
		TArray<SteamNetworkingIdentity> PendingSessions { };

		/** Messages read off the channel and waiting for the socket of their peer. */
		TArray<FSteamNetworkingMessagePtr> ReceivedMessages { };
	};
}
