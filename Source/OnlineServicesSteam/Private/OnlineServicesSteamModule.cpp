// Copyright PoFig Games Studio. All Rights Reserved.

#include "OnlineServicesSteamModule.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "Online/OnlineIdSteam.h"
#include "Online/OnlineServicesRegistry.h"
#include "Online/OnlineServicesSteam.h"


#ifndef STEAM_SDK_INSTALLED
#error Steam SDK not located! Expected to be found in <PluginsFolder>/OnlineServicesSteam/Source/ThirdParty/SteamworksSDK/{SteamVersion}
#endif

DEFINE_LOG_CATEGORY(LogOnlineServicesSteam);


/**
 * @class FOnlineServicesFactorySteam
 *
 * @brief Builds an FOnlineServicesSteam for the registry.
 */
class FOnlineServicesFactorySteam final : public UE::Online::IOnlineServicesFactory
{
public:
	FOnlineServicesFactorySteam() = default;
	virtual ~FOnlineServicesFactorySteam() override = default;

	virtual TSharedPtr<UE::Online::IOnlineServices> Create(FName InInstanceName, FName InstanceConfigName) override
	{
		if (auto Service = MakeShared<PoFigGames::Online::FOnlineServicesSteam>(InInstanceName, InstanceConfigName); Service->PreInit())
		{
			return Service;
		}

		return nullptr;
	}
};

int FOnlineServicesSteamModule::GetRegistryPriority()
{
	return 0;
}

void FOnlineServicesSteamModule::StartupModule()
{
	FModuleManager& ModuleManager = FModuleManager::Get();

	// Making sure we load the modules at this point will avoid errors while cooking
	static const FName SteamworksCommonModuleName = TEXT("SteamworksCommon");
	if (!ModuleManager.IsModuleLoaded(SteamworksCommonModuleName))
	{
		ModuleManager.LoadModuleChecked(SteamworksCommonModuleName);
	}

	static const FName OnlineServicesInterfaceModuleName = TEXT("OnlineServicesInterface");
	if (!ModuleManager.IsModuleLoaded(OnlineServicesInterfaceModuleName))
	{
		ModuleManager.LoadModuleChecked(OnlineServicesInterfaceModuleName);
	}

	UE::Online::FOnlineServicesRegistry::Get().RegisterServicesFactory(UE::Online::EOnlineServices::Steam, MakeUnique<FOnlineServicesFactorySteam>());

	UE::Online::FOnlineIdRegistryRegistry& OnlineIdRegistryRegistry = UE::Online::FOnlineIdRegistryRegistry::Get();

	static PoFigGames::Online::FOnlineAccountIdRegistrySteam AccountIdRegistry(UE::Online::EOnlineServices::Steam);
	OnlineIdRegistryRegistry.RegisterAccountIdRegistry(UE::Online::EOnlineServices::Steam, &AccountIdRegistry, GetRegistryPriority());
}

void FOnlineServicesSteamModule::ShutdownModule()
{
	UE::Online::FOnlineIdRegistryRegistry& OnlineIdRegistryRegistry = UE::Online::FOnlineIdRegistryRegistry::Get();

	OnlineIdRegistryRegistry.UnregisterAccountIdRegistry(UE::Online::EOnlineServices::Steam);

	UE::Online::FOnlineServicesRegistry::Get().UnregisterServicesFactory(UE::Online::EOnlineServices::Steam);
}

IMPLEMENT_MODULE(FOnlineServicesSteamModule, OnlineServicesSteam);
