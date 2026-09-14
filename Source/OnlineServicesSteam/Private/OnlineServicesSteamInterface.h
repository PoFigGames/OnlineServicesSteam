// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Net/OnlineEngineInterface.h"

#include "OnlineServicesSteamInterface.generated.h"


/**
 * @class UOnlineServicesSteamInterface
 *
 * @brief Implementation of UOnlineEngineInterface that uses Online Services (also known as Online Subsystem v2).
 *
 * The engine ships UOnlineServicesEngineInterfaceImpl for the same job, but it cannot be reused from here:
 * it lives in a private header of OnlineSubsystemUtils, so it can be neither included nor derived from, and
 * in engine versions before 5.8 it resolved Epic services instead of the configured default. This class
 * therefore mirrors it and resolves Steam explicitly.
 */
UCLASS(MinimalAPI, config = Engine)
class UOnlineServicesSteamInterface : public UOnlineEngineInterface
{
	GENERATED_BODY()

public:
	ONLINESERVICESSTEAM_API UOnlineServicesSteamInterface(const FObjectInitializer& ObjectInitializer);

	ONLINESERVICESSTEAM_API virtual bool IsLoaded(FName OnlineIdentifier) override;
	// Tells the online services instances of separate PIE worlds apart; without it every world shares one instance.
	ONLINESERVICESSTEAM_API virtual FName GetOnlineIdentifier(FWorldContext& WorldContext) override;
	ONLINESERVICESSTEAM_API virtual bool DoesInstanceExist(FName OnlineIdentifier) override;

	ONLINESERVICESSTEAM_API virtual void ShutdownOnlineSubsystem(FName OnlineIdentifier) override;
	ONLINESERVICESSTEAM_API virtual void DestroyOnlineSubsystem(FName OnlineIdentifier) override;
	ONLINESERVICESSTEAM_API virtual bool IsCompatibleUniqueNetId(const FUniqueNetIdWrapper& InUniqueNetId) const override;

	/**
	 * Utils
	 */
	ONLINESERVICESSTEAM_API virtual uint8 GetReplicationHashForSubsystem(FName InSubsystemName) const override;
	ONLINESERVICESSTEAM_API virtual FName GetSubsystemFromReplicationHash(uint8 InHash) const override;

	/**
	 * Identity
	 */
	ONLINESERVICESSTEAM_API virtual FUniqueNetIdWrapper CreateUniquePlayerIdWrapper(const FString& Str, FName Type) override;
	ONLINESERVICESSTEAM_API virtual FUniqueNetIdWrapper GetUniquePlayerIdWrapper(UWorld* World, int32 LocalUserNum, FName Type) override;

	ONLINESERVICESSTEAM_API virtual FString GetPlayerNickname(UWorld* World, const FUniqueNetIdWrapper& UniqueId) override;
	ONLINESERVICESSTEAM_API virtual bool GetPlayerPlatformNickname(UWorld* World, int32 LocalUserNum, FString& OutNickname) override;

	ONLINESERVICESSTEAM_API virtual bool AutoLogin(UWorld* World, int32 LocalUserNum, const FOnlineAutoLoginComplete& InCompletionDelegate) override;
	ONLINESERVICESSTEAM_API virtual bool IsLoggedIn(UWorld* World, int32 LocalUserNum) override;

	/**
	 * Session
	 */
	ONLINESERVICESSTEAM_API virtual void StartSession(UWorld* World, FName SessionName, FOnlineSessionStartComplete& InCompletionDelegate) override;
	ONLINESERVICESSTEAM_API virtual void EndSession(UWorld* World, FName SessionName, FOnlineSessionEndComplete& InCompletionDelegate) override;
	ONLINESERVICESSTEAM_API virtual bool DoesSessionExist(UWorld* World, FName SessionName) override;

	ONLINESERVICESSTEAM_API virtual bool GetSessionJoinability(UWorld* World, FName SessionName, FJoinabilitySettings& OutSettings) override;
	ONLINESERVICESSTEAM_API virtual void UpdateSessionJoinability(UWorld* World, FName SessionName, bool bPublicSearchable, bool bAllowInvites, bool bJoinViaPresence, bool bJoinViaPresenceFriendsOnly) override;

	ONLINESERVICESSTEAM_API virtual void RegisterPlayer(UWorld* World, FName SessionName, const FUniqueNetIdWrapper& UniqueId, bool bWasInvited) override;
	ONLINESERVICESSTEAM_API virtual void UnregisterPlayer(UWorld* World, FName SessionName, const FUniqueNetIdWrapper& UniqueId) override;
	ONLINESERVICESSTEAM_API virtual void UnregisterPlayers(UWorld* World, FName SessionName, const TArray<FUniqueNetIdWrapper>& Players) override;

	ONLINESERVICESSTEAM_API virtual bool GetResolvedConnectString(UWorld* World, FName SessionName, FString& URL) override;

	/**
	 * Voice
	 */
	ONLINESERVICESSTEAM_API virtual TSharedPtr<FVoicePacket> GetLocalPacket(UWorld* World, uint8 LocalUserNum) override;
	ONLINESERVICESSTEAM_API virtual TSharedPtr<FVoicePacket> SerializeRemotePacket(UWorld* World, const UNetConnection* const RemoteConnection, FArchive& Ar) override;

	ONLINESERVICESSTEAM_API virtual void StartNetworkedVoice(UWorld* World, uint8 LocalUserNum) override;
	ONLINESERVICESSTEAM_API virtual void StopNetworkedVoice(UWorld* World, uint8 LocalUserNum) override;
	ONLINESERVICESSTEAM_API virtual void ClearVoicePackets(UWorld* World) override;

	ONLINESERVICESSTEAM_API virtual bool MuteRemoteTalker(UWorld* World, uint8 LocalUserNum, const FUniqueNetIdWrapper& PlayerId, bool bIsSystemWide) override;
	ONLINESERVICESSTEAM_API virtual bool UnmuteRemoteTalker(UWorld* World, uint8 LocalUserNum, const FUniqueNetIdWrapper& PlayerId, bool bIsSystemWide) override;

	ONLINESERVICESSTEAM_API virtual int32 GetNumLocalTalkers(UWorld* World) override;

	/**
	 * External UI
	 */
	ONLINESERVICESSTEAM_API virtual void ShowLeaderboardUI(UWorld* World, const FString& CategoryName) override;
	ONLINESERVICESSTEAM_API virtual void ShowAchievementsUI(UWorld* World, int32 LocalUserNum) override;
	ONLINESERVICESSTEAM_API virtual void BindToExternalUIOpening(const FOnlineExternalUIChanged& Delegate) override;
	ONLINESERVICESSTEAM_API virtual void ShowWebURL(const FString& CurrentURL, const FShowWebUrlParams& ShowParams, const FOnlineShowWebUrlClosed& CompletionDelegate) override;
	ONLINESERVICESSTEAM_API virtual bool CloseWebURL() override;

	/**
	 * Debug
	 */
	ONLINESERVICESSTEAM_API virtual void DumpSessionState(UWorld* World) override;
	ONLINESERVICESSTEAM_API virtual void DumpPartyState(UWorld* World) override;
	ONLINESERVICESSTEAM_API virtual void DumpVoiceState(UWorld* World) override;
	ONLINESERVICESSTEAM_API virtual void DumpChatState(UWorld* World) override;

#if WITH_EDITOR
	/**
	 * PIE Utilities
	 */
	ONLINESERVICESSTEAM_API virtual bool SupportsOnlinePIE() override;
	ONLINESERVICESSTEAM_API virtual void SetShouldTryOnlinePIE(bool bShouldTry) override;
	ONLINESERVICESSTEAM_API virtual int32 GetNumPIELogins() override;
	ONLINESERVICESSTEAM_API virtual FString GetPIELoginCommandLineArgs(int32 Index) override;
	ONLINESERVICESSTEAM_API virtual void SetForceDedicated(FName OnlineIdentifier, bool bForce) override;
	ONLINESERVICESSTEAM_API virtual void LoginPIEInstance(FName OnlineIdentifier, int32 LocalUserNum, int32 PIELoginNum, FOnPIELoginComplete& CompletionDelegate) override;

private:
	bool bShouldTryOnlinePIE { true };
#endif
};
