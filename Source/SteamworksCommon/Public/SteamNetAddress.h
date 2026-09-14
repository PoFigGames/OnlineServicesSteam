// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "IPAddress.h"
#include "SteamCommon.h"

THIRD_PARTY_INCLUDES_START
#include "steam/steamnetworkingtypes.h"
THIRD_PARTY_INCLUDES_END


namespace PoFigGames::Steam
{
	// An address reads as "STEAM:<identity>[:<channel>]", where the identity is either a SteamID or an
	// IP address in the form Valve's own parser accepts. The channel stands in for the port and only
	// means anything to the connectionless transport, which addresses a peer by identity and channel.
	inline constexpr const TCHAR* SteamUrlPrefix { TEXT("STEAM") };
	inline constexpr const TCHAR* SteamUrlSeparator { TEXT(":") };

	/**
	 * Whether a port the engine handed us can be carried as a relay channel.
	 *
	 * Ports arrive from travel URLs and from config, so they are peer data and routinely name a real port
	 * such as 7777, which no channel can hold. Callers answer that by keeping what they had, never by
	 * asserting: a checked cast here would turn a malformed URL into a shutdown.
	 */
	inline constexpr bool IsValidChannel(const int32 InPort)
	{
		return InPort >= 0 && InPort <= static_cast<int32>(MAX_uint8);
	}

	/**
	 * Whether a port the engine handed us is one an IP address can carry.
	 */
	inline constexpr bool IsValidPort(const int32 InPort)
	{
		return InPort >= 0 && InPort <= static_cast<int32>(MAX_uint16);
	}

	/**
	 * The protocol an address adopts when a caller asks for one.
	 *
	 * Callers outside the plugin speak of IPv4 and IPv6, and plenty name nothing at all. Every one of our
	 * addresses still reports one of our two protocols, because that name is the only thing that says an
	 * FInternetAddr is one of ours: comparison has nothing else to go on before it casts. An address which
	 * named nothing is a relay address, which is the transport this plugin exists for.
	 */
	inline FName ResolveProtocol(const FName RequestedProtocol)
	{
		if (RequestedProtocol == SteamIpProtocol || RequestedProtocol == FNetworkProtocolTypes::IPv4 || RequestedProtocol == FNetworkProtocolTypes::IPv6)
		{
			return SteamIpProtocol;
		}

		return SteamRelayProtocol;
	}


	/**
	 * @class FSteamNetAddress
	 *
	 * @brief An address the engine can carry which names a Steam peer rather than a host and a port.
	 *
	 * Steam addresses a peer by identity: either a SteamID, reached through the Steam Datagram Relay, or a
	 * plain IP address for a server which is spoken to directly. Both forms live in one
	 * SteamNetworkingIdentity, and which of the two this address holds is what ProtocolType reports.
	 */
	class FSteamNetAddress : public FInternetAddr
	{
		SteamNetworkingIdentity Addr { };
		uint8 Channel { 0 };
		FName ProtocolType { };

	public:
		FSteamNetAddress(const FName RequestedProtocol = NAME_None)
			: ProtocolType(ResolveProtocol(RequestedProtocol))
		{
			Addr.Clear();
		}

		FSteamNetAddress(const SteamNetworkingIdentity& NewAddress, const uint8 InChannel = 0u)
			: Addr(NewAddress)
			, Channel(InChannel)
			, ProtocolType(NewAddress.GetIPAddr() ? SteamIpProtocol : SteamRelayProtocol) 
		{
		}

		FSteamNetAddress(const SteamNetworkingIPAddr& IPAddr)
			: ProtocolType(SteamIpProtocol)
		{
			Addr.SetIPAddr(IPAddr);
		}

		explicit FSteamNetAddress(const uint64& InSteamID, const uint8 InChannel = 0u)
			: Channel(InChannel)
			, ProtocolType(SteamRelayProtocol)
		{
			Addr.SetSteamID64(InSteamID);
		}

		explicit FSteamNetAddress(const CSteamID& InSteamID, const uint8 InChannel = 0u)
			: Channel(InChannel)
			, ProtocolType(SteamRelayProtocol)
		{
			Addr.SetSteamID(InSteamID);
		}

		STEAMWORKSCOMMON_API virtual TArray<uint8> GetRawIp() const override;

		STEAMWORKSCOMMON_API virtual void SetRawIp(const TArray<uint8>& RawAddr) override;

		virtual void SetIp(uint32 InAddr) override
		{
			// The port is read first: GetPort answers according to ProtocolType, so assigning the protocol
			// ahead of it would make an address that used to be a relay peer report a port it never had.
			const auto PreviousPort = GetPort();

			ProtocolType = SteamIpProtocol;
			Addr.SetIPv4Addr(InAddr, IsValidPort(PreviousPort) ? static_cast<uint16>(PreviousPort) : 0);
		}

		/**
		 * The SteamID this address names.
		 * An address holding an IP has none, and returns an invalid id.
		 */ 
		CSteamID GetSteamID() const
		{
			return Addr.GetSteamID();
		}
	
		uint64 GetSteamID64() const
		{
			return GetSteamID().ConvertToUint64();
		}

		/**
		 * Takes a bare address, unlike SetIp, whose array carries the type byte as well
		 */
		void SetSteamID(const CSteamID& NewSteamID)
		{
			ProtocolType = SteamRelayProtocol;
			Addr.SetSteamID(NewSteamID);
		}
	
		void SetSteamIdentity(const SteamNetworkingIdentity& NewSteamIdentity)
		{
			ProtocolType = NewSteamIdentity.GetIPAddr() ? SteamIpProtocol : SteamRelayProtocol;
			Addr = NewSteamIdentity;
		}
	
		void SetNetworkingAddr(const uint32 IpAddress, const uint32 Port)
		{
			SteamNetworkingIPAddr NewAddr { };
			NewAddr.SetIPv4(IpAddress, static_cast<uint16>(FMath::Min<uint32>(Port, MAX_uint16)));
		
			SetNetworkingAddr(NewAddr);
		}
	
		void SetNetworkingAddr(const SteamNetworkingIPAddr& NewAddr)
		{
			ProtocolType = SteamIpProtocol;
			Addr.SetIPAddr(NewAddr);
		}

		/**
		 * Sets the ip address from a string ("A.B.C.D") or "STEAM:SteamID:Channel"
		 *
		 * @param InAddr Text form of the address to adopt.
		 * @param bIsValid
		 */
		STEAMWORKSCOMMON_API virtual void SetIp(const TCHAR* InAddr, bool& bIsValid) override;

		/**
		 * Reads the address back in host byte order.
		 *
		 * @param OutAddr Receives the address.
		 */
		virtual void GetIp(uint32& OutAddr) const override
		{
			OutAddr = Addr.GetIPv4();
		}

		/**
		 * Sets the port, given in host byte order.
		 *
		 * @param InPort Port to adopt.
		 */
		STEAMWORKSCOMMON_API virtual void SetPort(int32 InPort) override;

		/**
	 	 * The port, in host byte order.
		 */
		STEAMWORKSCOMMON_API virtual int32 GetPort() const override;

		/**
		 * Sets the channel, which is what stands in for a port on the relay.
		 */
		virtual void SetPlatformPort(int32 InPort) override
		{
			if (IsValidChannel(InPort))
			{
				Channel = static_cast<uint8>(InPort);
			}
		}
	
		/**
		 * The channel this address speaks on.
		 */
		virtual int32 GetPlatformPort() const override
		{
			return Channel;
		}
	
		/** 
		 * Sets the address to be any address 
		 */
		STEAMWORKSCOMMON_API virtual void SetAnyAddress() override;

		/** 
		 * Sets the address to broadcast
		 */
		virtual void SetBroadcastAddress() override
		{
			/** Not used */
		}

		/**
		 * Sets the address to loopback
		 */
		virtual void SetLoopbackAddress() override
		{
			Addr.SetLocalHost();
		}

		/**
		 * Converts this internet ip address to a string form
		 *
		 * @param bAppendPort Whether the port or channel is printed as well.
		 */
		STEAMWORKSCOMMON_API virtual FString ToString(bool bAppendPort) const override;

		/**
		 * Whether two addresses name the same peer.
		 *
		 * @param Other The other address.
		 */
		virtual FORCEINLINE bool operator==(const FInternetAddr& Other) const override
		{
			// The engine compares addresses without knowing what either side is, and an FInternetAddr from
			// another socket subsystem is not one of ours: the protocol is what says the cast is allowed.
			if (const auto OtherProtocol = Other.GetProtocolType(); OtherProtocol != SteamRelayProtocol && OtherProtocol != SteamIpProtocol)
			{
				return false;
			}

			return *this == static_cast<const FSteamNetAddress&>(Other);
		}

		/**
		 * Whether two addresses name the same peer.
		 *
		 * @param Other The other address.
		 */
		FORCEINLINE bool operator==(const FSteamNetAddress& Other) const
		{
			return Addr == Other.Addr;
		}

		STEAMWORKSCOMMON_API virtual uint32 GetTypeHash() const override;

		STEAMWORKSCOMMON_API virtual FName GetProtocolType() const override
		{
			return ProtocolType;
		}

		virtual bool IsValid() const override
		{
			return !Addr.IsInvalid();
		}

		operator const SteamNetworkingIPAddr() const
		{
			if (const SteamNetworkingIPAddr* IPAddr = Addr.GetIPAddr())
			{
				return *IPAddr;
			}
		
			SteamNetworkingIPAddr EmptyAddr;
			EmptyAddr.Clear();
		
			return EmptyAddr;
		}

		operator const SteamNetworkingIdentity() const
		{
			return Addr;
		}

		virtual TSharedRef<FInternetAddr> Clone() const override
		{
			// Copied rather than rebuilt from its parts: the identity constructor infers the protocol from the
			// address, so a relay address which has not been given an identity yet would come back an IP one.
			return MakeShared<FSteamNetAddress>(*this);
		}
	};
}
