// Copyright PoFig Games Studio. All Rights Reserved.

#include "OnlineServicesSteamInterface.h"

// Project
#include "OnlineServicesSteamLogChannels.h"

// Engine
#include "Engine/Engine.h"
#include "Online/Auth.h"
#include "Online/ExternalUI.h"
#include "Online/OnlineAsyncOpHandle.h"
#include "Online/OnlineResult.h"
#include "Online/OnlineServices.h"
#include "Online/OnlineServicesEngineUtils.h"
#include "Online/OnlineServicesRegistry.h"


#include UE_INLINE_GENERATED_CPP_BY_NAME(OnlineServicesSteamInterface)


UOnlineServicesSteamInterface::UOnlineServicesSteamInterface(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UOnlineServicesSteamInterface::IsLoaded(FName OnlineIdentifier)
{
	constexpr UE::Online::EOnlineServices OnlineServicesType = UE::Online::EOnlineServices::Steam;
	return UE::Online::IsLoaded(OnlineServicesType, OnlineIdentifier);
}

FName UOnlineServicesSteamInterface::GetOnlineIdentifier(FWorldContext& WorldContext)
{
	if (UE::Online::IOnlineServicesEngineUtils* EngineUtils = UE::Online::GetServicesEngineUtils())
	{
		return EngineUtils->GetOnlineIdentifier(WorldContext);
	}

	return NAME_None;
}

bool UOnlineServicesSteamInterface::DoesInstanceExist(FName OnlineIdentifier)
{
	return UE::Online::IsLoaded(UE::Online::EOnlineServices::Steam, OnlineIdentifier);
}

void UOnlineServicesSteamInterface::ShutdownOnlineSubsystem(FName OnlineIdentifier)
{
	// The instances are destroyed rather than shut down, so that the shared references an
	// adapter holds to the older interfaces are released with them.
	UE::Online::DestroyAllServicesWithName(OnlineIdentifier);
}

void UOnlineServicesSteamInterface::DestroyOnlineSubsystem(FName OnlineIdentifier)
{
}

bool UOnlineServicesSteamInterface::IsCompatibleUniqueNetId(const FUniqueNetIdWrapper& InUniqueNetId) const
{
	return InUniqueNetId.IsV2();
}

/*
 * The three below belong to the identity replication of the older Online Subsystem, and nothing this
 * plugin writes ever asks for them - but what arrives is not this plugin's to choose. FUniqueNetIdRepl
 * reads its encoding flags out of the archive before it decides anything (Engine/Private/
 * OnlineReplStructs.cpp:275-303, read on 2026-09-13), so the remote side picks the branch: a flag word
 * without IsEncoded goes to NetSerializeLoadV1Unencoded, and an encoded one whose type hash is not the
 * v2 marker goes to NetSerializeLoadV1Encoded. Both call into these. An id travels in NMT_Login from a
 * client and in APlayerState::UniqueId from a server, so either end can send it.
 *
 * Answering with nothing is enough, because the engine already reads these answers as a refusal: a zero
 * hash leaves bValidTypeHash false and the whole branch is skipped, and NAME_None is tested before the
 * id is built (OnlineReplStructs.cpp:340-369). The warning is deliberate and not rate limited - in a
 * build where every id is a v2 id, reaching here at all means a peer that does not belong, and a flood
 * of these lines is the thing worth seeing.
 */
uint8 UOnlineServicesSteamInterface::GetReplicationHashForSubsystem(FName InSubsystemName) const
{
	UE_LOG(LogOnlineServicesSteam, Warning,
		TEXT("[UOnlineServicesSteamInterface] A peer asked for the replication hash of subsystem [%s]; only v2 identities are spoken here"),
		*InSubsystemName.ToString());

	return 0;
}

FName UOnlineServicesSteamInterface::GetSubsystemFromReplicationHash(uint8 InHash) const
{
	UE_LOG(LogOnlineServicesSteam, Warning,
		TEXT("[UOnlineServicesSteamInterface] A peer sent identity type hash %u; only v2 identities are spoken here"), InHash);

	return NAME_None;
}

FUniqueNetIdWrapper UOnlineServicesSteamInterface::CreateUniquePlayerIdWrapper(const FString& Str, FName Type)
{
	UE_LOG(LogOnlineServicesSteam, Warning,
		TEXT("[UOnlineServicesSteamInterface] A peer sent a version 1 identity of type [%s]; only v2 identities are spoken here"),
		*Type.ToString());

	return FUniqueNetIdWrapper { };
}

FUniqueNetIdWrapper UOnlineServicesSteamInterface::GetUniquePlayerIdWrapper(UWorld* World, int32 LocalUserNum, FName Type)
{
	const FName OnlineIdentifier = GetOnlineIdentifier(GEngine->GetWorldContextFromWorldChecked(World));
	constexpr UE::Online::EOnlineServices OnlineServicesType = UE::Online::EOnlineServices::Steam;

	if (const UE::Online::IOnlineServicesPtr OnlineServices = UE::Online::GetServices(OnlineServicesType, OnlineIdentifier))
	{
		if (const auto AuthPtr = OnlineServices->GetAuthInterface())
		{
			UE::Online::FAuthGetLocalOnlineUserByPlatformUserId::Params GetAccountParams = { FPlatformMisc::GetPlatformUserForUserIndex(LocalUserNum) };
			auto GetAccountResult = AuthPtr->GetLocalOnlineUserByPlatformUserId(MoveTemp(GetAccountParams));
			if (GetAccountResult.IsOk())
			{
				return FUniqueNetIdWrapper(GetAccountResult.GetOkValue().AccountInfo->AccountId);
			}
		}
	}
	return FUniqueNetIdWrapper { };
}

FString UOnlineServicesSteamInterface::GetPlayerNickname(UWorld* World, const FUniqueNetIdWrapper& UniqueId)
{
	// The only one of these that reads the identity it is given, so the only one that has to say no to an
	// identity it cannot read. GetV2 ensures on anything else.
	if (!UniqueId.IsValid() || !UniqueId.IsV2())
	{
		UE_LOG(LogOnlineServicesSteam, Warning,
			TEXT("[UOnlineServicesSteamInterface] A nickname was asked for an identity that is not a v2 one; only v2 identities are spoken here"));

		return FString { };
	}

	const FName OnlineIdentifier = GetOnlineIdentifier(GEngine->GetWorldContextFromWorldChecked(World));
	constexpr UE::Online::EOnlineServices OnlineServicesType = UE::Online::EOnlineServices::Steam;

	if (const UE::Online::IOnlineServicesPtr OnlineServices = UE::Online::GetServices(OnlineServicesType, OnlineIdentifier))
	{
		if (const auto AuthPtr = OnlineServices->GetAuthInterface())
		{
			UE::Online::FAuthGetLocalOnlineUserByOnlineAccountId::Params GetAccountParams = { UniqueId.GetV2() };
			auto GetAccountResult = AuthPtr->GetLocalOnlineUserByOnlineAccountId(MoveTemp(GetAccountParams));

			if (GetAccountResult.IsOk())
			{
				const auto DisplayName = GetAccountResult.GetOkValue().AccountInfo->Attributes.Find(UE::Online::AccountAttributeData::DisplayName);
				return DisplayName ? DisplayName->GetString() : FString { };
			}
		}
	}

	static FString InvalidName(TEXT("InvalidOSSUser"));
	return InvalidName;
}

bool UOnlineServicesSteamInterface::GetPlayerPlatformNickname(UWorld* World, int32 LocalUserNum, FString& OutNickname)
{
	const FName OnlineIdentifier = GetOnlineIdentifier(GEngine->GetWorldContextFromWorldChecked(World));
	constexpr UE::Online::EOnlineServices OnlineServicesType = UE::Online::EOnlineServices::Platform;

	if (const UE::Online::IOnlineServicesPtr OnlineServices = UE::Online::GetServices(OnlineServicesType, OnlineIdentifier))
	{
		if (const auto AuthPtr = OnlineServices->GetAuthInterface())
		{
			UE::Online::FAuthGetLocalOnlineUserByPlatformUserId::Params GetAccountParams = { FPlatformMisc::GetPlatformUserForUserIndex(LocalUserNum) };
			auto GetAccountResult = AuthPtr->GetLocalOnlineUserByPlatformUserId(MoveTemp(GetAccountParams));

			if (GetAccountResult.IsOk())
			{
				if (const UE::Online::FSchemaVariant* DisplayName = GetAccountResult.GetOkValue().AccountInfo->Attributes.Find(UE::Online::AccountAttributeData::DisplayName))
				{
					OutNickname = DisplayName->GetString();
				}

				return !OutNickname.IsEmpty();
			}
		}
	}

	return false;
}

bool UOnlineServicesSteamInterface::AutoLogin(UWorld* World, int32 LocalUserNum, const FOnlineAutoLoginComplete& InCompletionDelegate)
{
	const FName OnlineIdentifier = GetOnlineIdentifier(GEngine->GetWorldContextFromWorldChecked(World));
	constexpr UE::Online::EOnlineServices OnlineServicesType = UE::Online::EOnlineServices::Steam;

	if (const UE::Online::IOnlineServicesPtr OnlineServices = UE::Online::GetServices(OnlineServicesType, OnlineIdentifier))
	{
		if (const auto AuthPtr = OnlineServices->GetAuthInterface())
		{
			UE::Online::FAuthLogin::Params LoginParameters;
			LoginParameters.PlatformUserId = FPlatformMisc::GetPlatformUserForUserIndex(LocalUserNum);

			// Everything else is left at its default, so the service decides how to log in unattended the user
			auto LoginHandle = AuthPtr->Login(MoveTemp(LoginParameters));
			LoginHandle.OnComplete(this, [LocalUserNum, InCompletionDelegate](const UE::Online::TOnlineResult<UE::Online::FAuthLogin>& Result)
			{
				const FString ErrorCode = Result.IsError() ? Result.GetErrorValue().GetLogString() : FString { };
				InCompletionDelegate.ExecuteIfBound(LocalUserNum, Result.IsOk(), ErrorCode);
			});

			return true;
		}
	}

	// Not waiting for async login
	return false;
}

bool UOnlineServicesSteamInterface::IsLoggedIn(UWorld* World, int32 LocalUserNum)
{
	const FName OnlineIdentifier = GetOnlineIdentifier(GEngine->GetWorldContextFromWorldChecked(World));
	constexpr UE::Online::EOnlineServices OnlineServicesType = UE::Online::EOnlineServices::Steam;

	if (const UE::Online::IOnlineServicesPtr OnlineServices = UE::Online::GetServices(OnlineServicesType, OnlineIdentifier))
	{
		if (const auto AuthPtr = OnlineServices->GetAuthInterface())
		{
			UE::Online::FAuthGetLocalOnlineUserByPlatformUserId::Params GetAccountParams = { FPlatformMisc::GetPlatformUserForUserIndex(LocalUserNum) };
			auto GetAccountResult = AuthPtr->GetLocalOnlineUserByPlatformUserId(MoveTemp(GetAccountParams));

			if (GetAccountResult.IsOk())
			{
				return GetAccountResult.GetOkValue().AccountInfo->LoginStatus == UE::Online::ELoginStatus::LoggedIn;
			}
		}
	}

	return false;
}

/**
 * The engine asks about a session in the calls which follow, and this service has none: what the game
 * calls a session is a Steam lobby, and everything which needs one goes to the lobby interface of the
 * online services rather than through here. Answering from a lobby would put the same thing under two
 * names, which is why these say that there is no session rather than inventing one.
 */
void UOnlineServicesSteamInterface::StartSession(UWorld* World, FName SessionName, FOnlineSessionStartComplete& InCompletionDelegate)
{
	InCompletionDelegate.ExecuteIfBound(SessionName, false);
}

void UOnlineServicesSteamInterface::EndSession(UWorld* World, FName SessionName, FOnlineSessionEndComplete& InCompletionDelegate)
{
	InCompletionDelegate.ExecuteIfBound(SessionName, false);
}

bool UOnlineServicesSteamInterface::DoesSessionExist(UWorld* World, FName SessionName)
{
	return false;
}

bool UOnlineServicesSteamInterface::GetSessionJoinability(UWorld* World, FName SessionName, FJoinabilitySettings& OutSettings)
{
	return false;
}

void UOnlineServicesSteamInterface::UpdateSessionJoinability(UWorld* World, FName SessionName, bool bPublicSearchable, bool bAllowInvites, bool bJoinViaPresence, bool bJoinViaPresenceFriendsOnly)
{
}

void UOnlineServicesSteamInterface::RegisterPlayer(UWorld* World, FName SessionName, const FUniqueNetIdWrapper& UniqueId, bool bWasInvited)
{
}

void UOnlineServicesSteamInterface::UnregisterPlayer(UWorld* World, FName SessionName, const FUniqueNetIdWrapper& UniqueId)
{
}

void UOnlineServicesSteamInterface::UnregisterPlayers(UWorld* World, FName SessionName, const TArray<FUniqueNetIdWrapper>& Players)
{
}

bool UOnlineServicesSteamInterface::GetResolvedConnectString(UWorld* World, FName SessionName, FString& URL)
{
	return false;
}

TSharedPtr<FVoicePacket> UOnlineServicesSteamInterface::GetLocalPacket(UWorld* World, uint8 LocalUserNum)
{
	// Nothing to do until the game has voice.
	return nullptr;
}

TSharedPtr<FVoicePacket> UOnlineServicesSteamInterface::SerializeRemotePacket(UWorld* World, const UNetConnection* const RemoteConnection, FArchive& Ar)
{
	// Nothing to do until the game has voice.
	return nullptr;
}

void UOnlineServicesSteamInterface::StartNetworkedVoice(UWorld* World, uint8 LocalUserNum)
{
	// Nothing to do until the game has voice.
}

void UOnlineServicesSteamInterface::StopNetworkedVoice(UWorld* World, uint8 LocalUserNum)
{
	// Nothing to do until the game has voice.
}

void UOnlineServicesSteamInterface::ClearVoicePackets(UWorld* World)
{
	// Nothing to do until the game has voice.
}

bool UOnlineServicesSteamInterface::MuteRemoteTalker(UWorld* World, uint8 LocalUserNum, const FUniqueNetIdWrapper& PlayerId, bool bIsSystemWide)
{
	// Nothing to do until the game has voice.
	return false;
}

bool UOnlineServicesSteamInterface::UnmuteRemoteTalker(UWorld* World, uint8 LocalUserNum, const FUniqueNetIdWrapper& PlayerId, bool bIsSystemWide)
{
	// Nothing to do until the game has voice.
	return false;
}

int32 UOnlineServicesSteamInterface::GetNumLocalTalkers(UWorld* World)
{
	// Nothing to do until the game has voice.
	return 0;
}

void UOnlineServicesSteamInterface::ShowLeaderboardUI(UWorld* World, const FString& CategoryName)
{
	const FName OnlineIdentifier = GetOnlineIdentifier(GEngine->GetWorldContextFromWorldChecked(World));
	constexpr UE::Online::EOnlineServices OnlineServicesType = UE::Online::EOnlineServices::Steam;

	if (UE::Online::IOnlineServicesPtr OnlineServices = UE::Online::GetServices(OnlineServicesType, OnlineIdentifier))
	{
		auto ExternalUI = OnlineServices->GetExternalUIInterface();
		if (ExternalUI.IsValid())
		{
			// Waiting on wider overlay support.
		}
	}
}

void UOnlineServicesSteamInterface::ShowAchievementsUI(UWorld* World, int32 LocalUserNum)
{
	TArray<TSharedRef<UE::Online::IOnlineServices>> ServicesInstances;
	UE::Online::FOnlineServicesRegistry::Get().GetAllServicesInstances(ServicesInstances);
	for (const auto& OnlineServices : ServicesInstances)
	{
		auto ExternalUI = OnlineServices->GetExternalUIInterface();
		if (ExternalUI.IsValid())
		{
			// Waiting on wider overlay support.
		}
	}
}

void UOnlineServicesSteamInterface::ShowWebURL(const FString& CurrentURL, const FShowWebUrlParams& ShowParams, const FOnlineShowWebUrlClosed& CompletionDelegate)
{
	TArray<TSharedRef<UE::Online::IOnlineServices>> ServicesInstances;
	UE::Online::FOnlineServicesRegistry::Get().GetAllServicesInstances(ServicesInstances);
	for (const auto& OnlineServices : ServicesInstances)
	{
		auto ExternalUI = OnlineServices->GetExternalUIInterface();
		if (ExternalUI.IsValid())
		{
			// Waiting on wider overlay support.
		}
	}
}

bool UOnlineServicesSteamInterface::CloseWebURL()
{
	TArray<TSharedRef<UE::Online::IOnlineServices>> ServicesInstances;
	UE::Online::FOnlineServicesRegistry::Get().GetAllServicesInstances(ServicesInstances);
	for (const auto& OnlineServices : ServicesInstances)
	{
		auto ExternalUI = OnlineServices->GetExternalUIInterface();
		if (ExternalUI.IsValid())
		{
			// Waiting on wider overlay support.
		}
	}
	return false;
}

void UOnlineServicesSteamInterface::BindToExternalUIOpening(const FOnlineExternalUIChanged& Delegate)
{
	TArray<TSharedRef<UE::Online::IOnlineServices>> ServicesInstances;
	UE::Online::FOnlineServicesRegistry::Get().GetAllServicesInstances(ServicesInstances);
	for (const auto& ServiceInstance : ServicesInstances)
	{
		if (const UE::Online::IExternalUIPtr ExternalUI = ServiceInstance->GetExternalUIInterface())
		{
			auto _ = ExternalUI->OnExternalUIStatusChanged().Add([&Delegate](const UE::Online::FExternalUIStatusChanged& EventParams)
			{
				Delegate.ExecuteIfBound(EventParams.bIsOpening);
			});
		}
	}
}

void UOnlineServicesSteamInterface::DumpSessionState(UWorld* World)
{
}

void UOnlineServicesSteamInterface::DumpPartyState(UWorld* World)
{
	// A party is a lobby here, and the lobby interface is what prints its own state.
}

void UOnlineServicesSteamInterface::DumpVoiceState(UWorld* World)
{
	// Nothing to do until the game has voice.
}

void UOnlineServicesSteamInterface::DumpChatState(UWorld* World)
{
	// Nothing to do until the game has chat.
}

#if WITH_EDITOR
bool UOnlineServicesSteamInterface::SupportsOnlinePIE()
{
	check(UObjectInitialized());

	// No auth component means Steam is either misconfigured or turned off in this build.
	if (const UE::Online::IOnlineServicesPtr OnlineServices = UE::Online::GetServices())
	{
		if (UE::Online::IAuthPtr AuthPtr = OnlineServices->GetAuthInterface())
		{
			return bShouldTryOnlinePIE;
		}
	}

	return false;
}

void UOnlineServicesSteamInterface::SetShouldTryOnlinePIE(bool bShouldTry)
{
	bShouldTryOnlinePIE = bShouldTry;
}

int32 UOnlineServicesSteamInterface::GetNumPIELogins()
{
	return 1;
}

FString UOnlineServicesSteamInterface::GetPIELoginCommandLineArgs(int32 Index)
{
	return TEXT("");
}

void UOnlineServicesSteamInterface::SetForceDedicated(FName OnlineIdentifier, bool bForce)
{
	// Only ever reached with the default service id, so no other service is looked for.
	constexpr UE::Online::EOnlineServices OnlineServicesType = UE::Online::EOnlineServices::Default;
	if (UE::Online::IOnlineServicesPtr OnlineServices = UE::Online::GetServices(OnlineServicesType, OnlineIdentifier))
	{
		// There is no way to force the dedicated path through the services interface yet.
	}
}

void UOnlineServicesSteamInterface::LoginPIEInstance(FName OnlineIdentifier, int32 LocalUserNum, int32 PIELoginNum, FOnPIELoginComplete& CompletionDelegate)
{
	FString ErrorStr;
	if (SupportsOnlinePIE())
	{
		constexpr UE::Online::EOnlineServices OnlineServicesType = UE::Online::EOnlineServices::Default;
		if (const UE::Online::IOnlineServicesPtr OnlineServices = UE::Online::GetServices(OnlineServicesType, OnlineIdentifier))
		{
			if (const auto AuthPtr = OnlineServices->GetAuthInterface())
			{
				UE::Online::FAuthLogin::Params LoginParameters;
				LoginParameters.CredentialsType = TEXT("Steam");
				LoginParameters.PlatformUserId = FPlatformMisc::GetPlatformUserForUserIndex(LocalUserNum);

				AuthPtr->Login(MoveTemp(LoginParameters)).OnComplete(this, [LocalUserNum, CompletionDelegate](const UE::Online::TOnlineResult<UE::Online::FAuthLogin>& Result)
				{
					FString AuthLoginErrorStr;
					if (Result.IsError())
					{
						AuthLoginErrorStr = Result.GetErrorValue().GetLogString();
					}

					CompletionDelegate.ExecuteIfBound(LocalUserNum, Result.IsOk(), AuthLoginErrorStr);
				});

				return;
			}

			ErrorStr = TEXT("Cannot log in: this service has no auth component");
		}
		else
		{
			ErrorStr = TEXT("Cannot log in: no online service instance is present");
		}
	}
	else
	{
		ErrorStr = TEXT("A play-in-editor session cannot log in through this service");
	}

	if (!ErrorStr.IsEmpty())
	{
		CompletionDelegate.ExecuteIfBound(LocalUserNum, false, ErrorStr);
	}
}

#endif

