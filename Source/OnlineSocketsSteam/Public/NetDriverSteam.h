// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Engine/NetDriver.h"

#include "NetDriverSteam.generated.h"


namespace PoFigGames::Steam {
	class FSocketSubsystemSteam;
	class FSocketSteam;
}

class ISteamNetworkingSockets;
class FNetworkNotify;


/**
 * @class UNetDriverSteam
 *
 * @brief A Steam-specific network driver that inherits from UNetDriver.
 * This class provides functionality for managing network connections
 * using Steam networking features.
 */
UCLASS(MinimalAPI, Transient, Config=Engine)
class UNetDriverSteam : public UNetDriver
{
	GENERATED_BODY()

	friend class PoFigGames::Steam::FSocketSubsystemSteam;
	friend class PoFigGames::Steam::FSocketSteam;

public:
	UNetDriverSteam(const FObjectInitializer& ObjectInitializer);

#pragma region NetDriver
	ONLINESOCKETSSTEAM_API virtual void Shutdown() override;
	ONLINESOCKETSSTEAM_API virtual bool IsAvailable() const override;
	ONLINESOCKETSSTEAM_API virtual bool InitBase(bool bInitAsClient, FNetworkNotify* InNotify, const FURL& URL, bool bReuseAddressAndPort, FString& Error) override;
	ONLINESOCKETSSTEAM_API virtual bool InitConnect(FNetworkNotify* InNotify, const FURL& ConnectURL, FString& Error) override;
	ONLINESOCKETSSTEAM_API virtual bool InitListen(FNetworkNotify* InNotify, FURL& ListenURL, bool bReuseAddressAndPort, FString& Error) override;
	ONLINESOCKETSSTEAM_API virtual void TickDispatch(float DeltaTime) override;
	ONLINESOCKETSSTEAM_API virtual void LowLevelSend(TSharedPtr<const FInternetAddr> Address, void* Data, int32 CountBits, FOutPacketTraits& Traits) override;
	ONLINESOCKETSSTEAM_API virtual void LowLevelDestroy() override;
	ONLINESOCKETSSTEAM_API virtual ISocketSubsystem* GetSocketSubsystem() override;
	ONLINESOCKETSSTEAM_API virtual bool IsNetResourceValid() override;
#pragma endregion

	ONLINESOCKETSSTEAM_API bool ArePacketHandlersDisabled() const;

	/**
	 * Tells the Steam master server what this server is running, once it has started listening.
	 *
	 * The match is what the driver can see: which map is loaded and how many players the session holds.
	 * Everything Unreal has no notion of, a password and a bot count, is reported as absent until whoever
	 * owns the session says otherwise.
	 */
	ONLINESOCKETSSTEAM_API void PublishServerDetails() const;

	static ONLINESOCKETSSTEAM_API ISteamNetworkingSockets* GetNetworkingSockets();

protected:
	TSharedPtr<PoFigGames::Steam::FSocketSteam> Socket { nullptr };

	bool bIsDelayedNetworkAccess { false };
	bool bPassthrough { false };

	ONLINESOCKETSSTEAM_API void ResetSocketInfo(const TSharedPtr<PoFigGames::Steam::FSocketSteam>& RemovedSocket);

	ONLINESOCKETSSTEAM_API UNetConnection* FindClientConnectionForHandle(uint32 SocketHandle);

	/** The connection speaking to one Steam identity, which is all a connectionless message names. */
	ONLINESOCKETSSTEAM_API UNetConnection* FindClientConnectionForIdentity(uint64 SteamId);

	/**
	 * Takes in whatever the connectionless transport has to offer this frame.
	 *
	 * There is no connection to be notified of, so sessions are accepted by asking the socket which owns
	 * the channel for them, and every connection is then read from the socket of its own peer.
	 */
	ONLINESOCKETSSTEAM_API void TickDispatchMessages();

	/** Builds the connection for a peer whose session was just accepted. */
	ONLINESOCKETSSTEAM_API void CreateMessagesConnection(const TSharedRef<PoFigGames::Steam::FSocketSteam>& AcceptedSocket);

	/** Hands everything waiting on one socket to the connection which speaks through it. */
	ONLINESOCKETSSTEAM_API void ReceiveMessagesOnSocket(const TSharedPtr<PoFigGames::Steam::FSocketSteam>& FromSocket, UNetConnection* ForConnection);

	ONLINESOCKETSSTEAM_API void OnConnectionCreated(uint32 ListenParentHandle, uint32 SocketHandle);
	ONLINESOCKETSSTEAM_API void OnConnectionUpdated(uint32 SocketHandle, int32 NewState);
	ONLINESOCKETSSTEAM_API void OnConnectionDisconnected(uint32 SocketHandle);
};
