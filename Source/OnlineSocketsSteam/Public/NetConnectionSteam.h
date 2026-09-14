// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Engine/NetConnection.h"

#include "NetConnectionSteam.generated.h"

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
#pragma endregion

private:
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
