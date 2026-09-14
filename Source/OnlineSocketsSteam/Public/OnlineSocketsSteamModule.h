// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"
#include "Modules/ModuleInterface.h"


/**
 * @class FOnlineSocketsSteamModule
 *
 * @brief Brings the Steam socket subsystem up with the module and takes it down with it.
 */
class FOnlineSocketsSteamModule : public IModuleInterface
{
public:
	FOnlineSocketsSteamModule() = default;
	virtual ~FOnlineSocketsSteamModule() override = default;

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	virtual bool SupportsDynamicReloading() override
	{
		return false;
	}

	static FOnlineSocketsSteamModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FOnlineSocketsSteamModule>("OnlineSocketsSteam");
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("OnlineSocketsSteam");
	}
};
