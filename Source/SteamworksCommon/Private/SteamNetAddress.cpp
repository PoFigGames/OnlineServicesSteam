// Copyright PoFig Games Studio. All Rights Reserved.

#include "SteamNetAddress.h"

#include "SteamInterfaces.h"
#include "SteamworksCommonLogChannels.h"


namespace PoFigGames::Steam
{
	// The shape of the raw form GetRawIp writes and SetRawIp reads back: one byte naming the type, then
	// either a SteamID most significant byte first or the bytes of an IPv6 address. These are signed
	// because every length they are compared against is, and a signed-to-unsigned comparison here would
	// be a warning on one compiler and a silent conversion on the other.
	constexpr int32 RawTypeByteCount { 1 };
	constexpr int32 RawSteamIdByteCount { static_cast<int32>(sizeof(uint64)) };
	constexpr int32 RawIpAddressByteCount { 16 };

	static_assert(RawIpAddressByteCount == static_cast<int32>(UE_ARRAY_COUNT(SteamNetworkingIPAddr::m_ipv6)), "An IPv6 address is sixteen bytes, and the SDK is expected to agree.");


	TArray<uint8> FSteamNetAddress::GetRawIp() const
	{
		// SteamNetworkingIdentity keeps its payload in a union with no byte view of its own, so the raw form
		// the engine asks for is assembled here. Byte zero carries the type, so that SetRawIp can tell a
		// SteamID from an IP address.
		TArray<uint8> RawBytes;

		if (Addr.m_eType == k_ESteamNetworkingIdentityType_SteamID)
		{
			const uint64 SteamIdValue = Addr.GetSteamID64();

			RawBytes.Reserve(RawTypeByteCount + RawSteamIdByteCount);
			RawBytes.Add(static_cast<uint8>(k_ESteamNetworkingIdentityType_SteamID));

			// Most significant byte first, which is the order the engine compares and prints raw addresses in.
			// Shifting the value rather than walking over its bytes is what makes that true on either endianness.
			for (int32 Shift = RawSteamIdByteCount * 8 - 8; Shift >= 0; Shift -= 8)
			{
				RawBytes.Add(static_cast<uint8>(SteamIdValue >> Shift));
			}
		}
		else if (const auto IpAddr = Addr.GetIPAddr(); Addr.m_eType == k_ESteamNetworkingIdentityType_IPAddress && IpAddr != nullptr)
		{
			RawBytes.Reserve(RawTypeByteCount + RawIpAddressByteCount);
			RawBytes.Add(static_cast<uint8>(k_ESteamNetworkingIdentityType_IPAddress));
			RawBytes.Append(IpAddr->m_ipv6, RawIpAddressByteCount);
		}

		return RawBytes;
	}

	void FSteamNetAddress::SetRawIp(const TArray<uint8>& RawAddr)
	{
		// Byte zero is the type written by GetRawIp, so anything shorter carries no address. Everything past
		// it is payload, and its length is not ours to trust: this is reached from identity replication, so
		// the array can be whatever arrived over the wire.
		if (RawAddr.Num() <= RawTypeByteCount)
		{
			return;
		}

		const auto Payload = MakeArrayView(RawAddr).RightChop(RawTypeByteCount);

		if (RawAddr[0] == k_ESteamNetworkingIdentityType_SteamID)
		{
			// A short payload is not a truncated id, it is not an id at all: filling the high bytes with zero
			// would name some other account rather than fail.
			if (Payload.Num() < RawSteamIdByteCount)
			{
				return;
			}

			// Most significant byte first, as GetRawIp wrote it.
			uint64 SteamIdValue = 0;
			for (const uint8 Byte : Payload.Left(RawSteamIdByteCount))
			{
				SteamIdValue = SteamIdValue << 8 | Byte;
			}

			Addr.Clear();
			Addr.SetSteamID64(SteamIdValue);
			ProtocolType = SteamRelayProtocol;
		}
		else if (RawAddr[0] == k_ESteamNetworkingIdentityType_IPAddress)
		{
			SteamNetworkingIPAddr IpAddress { };

			if (Payload.Num() < RawIpAddressByteCount)
			{
				return;
			}

			const auto Address = Payload.Left(RawIpAddressByteCount);
			FMemory::Memcpy(IpAddress.m_ipv6, Address.GetData(), Address.Num());

			Addr.Clear();
			Addr.SetIPAddr(IpAddress);
			ProtocolType = SteamIpProtocol;
		}
	}

	void FSteamNetAddress::SetIp(const TCHAR* InAddr, bool& bIsValid)
	{
		bIsValid = false;

		if (InAddr == nullptr)
		{
			return;
		}

		// FURL strips the scheme before handing us a host, but ToString writes it, and this is also reached
		// with whatever a caller happens to hold: both forms are accepted so that an address survives being
		// printed and read back.
		FString AddressText(InAddr);
		AddressText.RemoveFromStart(FString(SteamUrlPrefix) + SteamUrlSeparator, ESearchCase::IgnoreCase);

		if (AddressText.IsEmpty())
		{
			return;
		}

		TArray<FString> UrlParts;
		AddressText.ParseIntoArray(UrlParts, SteamUrlSeparator, false);

		// The relay form is a SteamID and at most a channel after it. Anything else is left to Valve's own
		// parser below, whole: splitting an IPv6 address on colons would shred it, since it is mostly colons.
		if (UrlParts.Num() <= 2 && UrlParts[0].IsNumeric())
		{
			// The channel is whatever the peer wrote, so it is range-checked rather than cast, and nothing is
			// adopted until the whole address has parsed.
			const auto NewChannel = UrlParts.IsValidIndex(1) ? FCString::Atoi(*UrlParts[1]) : 0;

			const uint64 NewId = FCString::Atoi64(*UrlParts[0]);

			if (const CSteamID SteamId(NewId); SteamId.IsValid() && IsValidChannel(NewChannel))
			{
				ProtocolType = SteamRelayProtocol;

				Addr.SetSteamID(SteamId);
				Channel = static_cast<uint8>(NewChannel);

				bIsValid = true;
			}

			return;
		}

		if (SteamNetworkingIPAddr ParsedAddress; GetSteamInterface<ISteamNetworkingUtils>() != nullptr && ParsedAddress.ParseString(StringCast<ANSICHAR>(*AddressText).Get()))
		{
			ProtocolType = SteamIpProtocol;
			Addr.SetIPAddr(ParsedAddress);

			bIsValid = true;
		}
	}

	void FSteamNetAddress::SetPort(int32 InPort)
	{
		if (ProtocolType == SteamRelayProtocol)
		{
			// On the relay the port is a channel, and a channel is one byte. The engine hands us the port from
			// the travel URL, which normally holds a real port number such as 7777, so a value that does not
			// fit is the ordinary case here rather than a programming error: the address keeps the channel it
			// already had and the connection goes out on that.
			if (!IsValidChannel(InPort))
			{
				UE_LOG(LogSteamService, Verbose, TEXT("Port %d is not a relay channel, keeping channel %u."), InPort, Channel);
				return;
			}

			Channel = static_cast<uint8>(InPort);
		}
		else if (ProtocolType == SteamIpProtocol)
		{
			if (!IsValidPort(InPort))
			{
				UE_LOG(LogSteamService, Verbose, TEXT("Port %d is out of range, keeping port %d."), InPort, GetPort());
				return;
			}

			// SteamNetworkingIPAddr exposes no setter for the port on its own, so the address is copied out,
			// given the new port, and put back.
			if (const auto IpAddr = Addr.GetIPAddr())
			{
				SteamNetworkingIPAddr RePortedAddress { *IpAddr };
				RePortedAddress.m_port = static_cast<uint16>(InPort);

				Addr.SetIPAddr(RePortedAddress);
			}
		}
	}

	int32 FSteamNetAddress::GetPort() const
	{
		if (ProtocolType == SteamRelayProtocol)
		{
			return Channel;
		}

		if (const auto IpAddr = Addr.GetIPAddr())
		{
			// Valve keeps the port in host byte order, unlike the address itself.
			return IpAddr->m_port;
		}

		return 0;
	}

	void FSteamNetAddress::SetAnyAddress()
	{
		SteamNetworkingIPAddr AnyAddress;
		AnyAddress.Clear();

		Addr.Clear();
		Addr.SetIPAddr(AnyAddress);

		ProtocolType = SteamIpProtocol;
	}

	FString FSteamNetAddress::ToString(bool bAppendPort) const
	{
		if (!IsValid())
		{
			return TEXT("Invalid");
		}

		if (const auto IpAddr = Addr.GetIPAddr())
		{
			// Valve's own printer lives behind ISteamNetworkingUtils, so an address can outlive the interface
			// that formats it: this is called from logging, which runs during shutdown as well. What is left
			// then are the header's own inline accessors, which reach IPv4 and nothing further.
			if (GetSteamInterface<ISteamNetworkingUtils>() != nullptr)
			{
				ANSICHAR Printed[SteamNetworkingIPAddr::k_cchMaxString];
				FMemory::Memzero(Printed);

				IpAddr->ToString(Printed, SteamNetworkingIPAddr::k_cchMaxString, bAppendPort);

				return FString::Printf(TEXT("%s%s%s"), SteamUrlPrefix, SteamUrlSeparator, ANSI_TO_TCHAR(Printed));
			}

			if (!IpAddr->IsIPv4())
			{
				return FString::Printf(TEXT("%s%s<IPv6>"), SteamUrlPrefix, SteamUrlSeparator);
			}

			const auto Ipv4 = IpAddr->GetIPv4();
			auto AddressResult = FString::Printf(TEXT("%s%s%u.%u.%u.%u"), SteamUrlPrefix, SteamUrlSeparator, Ipv4 >> 24 & 0xFFu, Ipv4 >> 16 & 0xFFu, Ipv4 >> 8 & 0xFFu, Ipv4 & 0xFFu);

			if (bAppendPort)
			{
				AddressResult += FString::Printf(TEXT("%s%u"), SteamUrlSeparator, static_cast<uint32>(IpAddr->m_port));
			}

			return AddressResult;
		}

		auto IdentityResult = FString::Printf(TEXT("%s%s%llu"), SteamUrlPrefix, SteamUrlSeparator, Addr.GetSteamID64());

		if (bAppendPort)
		{
			IdentityResult += FString::Printf(TEXT("%s%u"), SteamUrlSeparator, static_cast<uint32>(Channel));
		}

		return IdentityResult;
	}

	uint32 FSteamNetAddress::GetTypeHash() const
	{
		// Equality compares the identity and nothing else, so the hash reads the identity and nothing else:
		// mixing the channel in here would put two equal addresses in different buckets. The identity is a
		// plain value with no indirection, which is why hashing its bytes is safe and why this needs neither
		// the SDK nor a formatted string.
		if (Addr.IsInvalid())
		{
			return 0;
		}

		if (const auto SteamId = Addr.GetSteamID(); SteamId.IsValid())
		{
			return GetTypeHashHelper(SteamId.ConvertToUint64());
		}

		if (const auto IpAddr = Addr.GetIPAddr())
		{
			return HashCombineFast(FCrc::MemCrc32(IpAddr->m_ipv6, RawIpAddressByteCount), GetTypeHashHelper(IpAddr->m_port));
		}

		return GetTypeHashHelper(static_cast<int32>(Addr.m_eType));
	}
}
