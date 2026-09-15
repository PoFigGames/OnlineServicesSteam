// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"


/**
 * @class FOnlineServicesSteamModule
 *
 * @brief The OSSv2 Steam services module. Based on SteamSharedModule.
 */
class FOnlineServicesSteamModule final : public IModuleInterface
{
public:
	FOnlineServicesSteamModule() = default;
	virtual ~FOnlineServicesSteamModule() override = default;

	ONLINESERVICESSTEAM_API virtual void StartupModule() override;
	ONLINESERVICESSTEAM_API virtual void ShutdownModule() override;

	/**
	 * The Steamworks libraries are linked once for the life of the process, so this module cannot be reloaded.
	 */
	virtual bool SupportsDynamicReloading() override { return false; }

	ONLINESERVICESSTEAM_API static int GetRegistryPriority();

	/**
	 * Convenience accessor for this module.
	 * Do not reach for it while the process is tearing down; by then it may be gone.
	 *
	 * @return Returns a singleton instance, loading the module on demand if needed
	 */
	ONLINESERVICESSTEAM_API static FOnlineServicesSteamModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FOnlineServicesSteamModule>(TEXT("OnlineServicesSteam"));
	}

	/**
	 * Whether Get is safe to call.
	 *
	 * @return Whether the module is up.
	 */
	ONLINESERVICESSTEAM_API static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("OnlineServicesSteam"));
	}
};


