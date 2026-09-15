// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Engine/NetConnection.h"
#include "Online/CoreOnline.h"

#include "NetConnectionSteam.generated.h"

namespace PoFigGames::Online
{
	class FAuthHandlerSteam;
}

namespace PoFigGames::Steam
{
	class FSocketSteam;
	class FSocketSubsystemSteam;
}

/**
 * @class UNetConnectionSteam
 *
 * @brief Represents a specialized network connection that interacts with the Steam networking system.
 * Inherits from UNetConnection and overrides certain methods to handle Steam-specific connections.
 */
UCLASS(MinimalAPI, Transient, config = "Engine")
class UNetConnectionSteam : public UNetConnection
{
	GENERATED_BODY()

	friend class UNetDriverSteam;

public:
	ONLINESOCKETSSTEAM_API UNetConnectionSteam(const FObjectInitializer& ObjectInitializer);

#pragma region UNetConnection
	ONLINESOCKETSSTEAM_API virtual void CleanUp() override;
	ONLINESOCKETSSTEAM_API virtual void InitBase(UNetDriver* InDriver, FSocket* InSocket, const FURL& InURL, EConnectionState InState, int32 InMaxPacket = 0, int32 InPacketOverhead = 0) override;
	ONLINESOCKETSSTEAM_API virtual void InitRemoteConnection(class UNetDriver* InDriver, class FSocket* InSocket, const FURL& InURL, const class FInternetAddr& InRemoteAddr, EConnectionState InState, int32 InMaxPacket = 0, int32 InPacketOverhead = 0) override;
	ONLINESOCKETSSTEAM_API virtual void InitLocalConnection(class UNetDriver* InDriver, class FSocket* InSocket, const FURL& InURL, EConnectionState InState, int32 InMaxPacket = 0, int32 InPacketOverhead = 0) override;
	ONLINESOCKETSSTEAM_API virtual void LowLevelSend(void* Data, int32 CountBits, FOutPacketTraits& Traits) override;

	ONLINESOCKETSSTEAM_API virtual FString LowLevelGetRemoteAddress(bool bAppendPort = false) override;
	ONLINESOCKETSSTEAM_API virtual FString LowLevelDescribe() override;
	ONLINESOCKETSSTEAM_API virtual void SetClientLoginState(EClientLoginState::Type NewState) override;
#pragma endregion UNetConnection

	/**
	 * The account the peer proved with a Steam ticket before this connection was let through.
	 *
	 * Empty where nobody proved anything: no handshake component, or a peer that offered no ticket on a
	 * server with no services to check it against. Over the relay the ticket is also refused unless it
	 * names the identity Steam signed for the connection, so there it is the peer and not a claim.
	 */
	ONLINESOCKETSSTEAM_API UE::Online::FAccountId GetVerifiedAccount() const;

private:
	/** The handshake component of this connection, or null where the project did not ask for one. */
	PoFigGames::Online::FAuthHandlerSteam* GetAuthHandler() const;

	/**
	 * The Steam socket subsystem behind this connection's driver, or null when there is none.
	 *
	 * Reaching it means going driver, subsystem, cast, and that was written out at every call site.
	 */
	PoFigGames::Steam::FSocketSubsystemSteam* GetSteamSubsystem();

	const TSharedPtr<PoFigGames::Steam::FSocketSteam>& GetRawSocket() const { return PeerSocket; }

	ONLINESOCKETSSTEAM_API void HandleRecvMessage(void* InData, int32 IncomingSize, const FInternetAddr* InFormattedAddress);

	void FlagForHandshake() { bAwaitingHandshake = true; }

	void ClearSocket() { PeerSocket.Reset(); }

	/** The socket this connection speaks through; the socket subsystem is the one that owns it. */
	TSharedPtr<PoFigGames::Steam::FSocketSteam> PeerSocket { nullptr };
	bool bAwaitingHandshake { false };
};
