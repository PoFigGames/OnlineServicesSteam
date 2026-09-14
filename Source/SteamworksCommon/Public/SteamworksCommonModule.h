// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

#ifndef STEAM_CLIENT_LIBRARY_IS_DYNAMIC
#define STEAM_CLIENT_LIBRARY_IS_DYNAMIC		(PLATFORM_WINDOWS || PLATFORM_MAC || (PLATFORM_LINUX && !IS_MONOLITHIC))
#endif

#ifndef STEAM_SERVER_LIBRARY_IS_DYNAMIC
#define STEAM_SERVER_LIBRARY_IS_DYNAMIC		(PLATFORM_WINDOWS || (PLATFORM_LINUX && !IS_MONOLITHIC) || PLATFORM_MAC)
#endif

#ifndef STEAM_LIBRARIES_ARE_DYNAMIC
#define STEAM_LIBRARIES_ARE_DYNAMIC				(STEAM_CLIENT_LIBRARY_IS_DYNAMIC || STEAM_SERVER_LIBRARY_IS_DYNAMIC)
#endif

/**
 * FSteamworksCommonModule
 */
class FSteamworksCommonModule : public IModuleInterface
{
public:
	FSteamworksCommonModule() = default;
	virtual ~FSteamworksCommonModule() override = default;
	
	virtual void StartupModule() override;
    virtual void ShutdownModule() override;

	/**
	 * The Steamworks libraries are linked once for the life of the process, so this module cannot be reloaded.
	 */ 
	STEAMWORKSCOMMON_API virtual bool SupportsDynamicReloading() override { return false; }

	/**
	 * Are the Steamworks Dlls loaded?
	 *
	 * @return Whether the libraries are available, which a static link always is.
	 */
	STEAMWORKSCOMMON_API bool AreSteamDllsLoaded() const;

	/**
	 * Where the Steamworks libraries are looked for.
	 *
	 * @return Directory the libraries are loaded from.
	 */
	STEAMWORKSCOMMON_API static FString GetLibraryDirectory();

	/**
	 * Whether a dedicated server also links the client libraries.
	 * Only Windows has a reason to.
	 *
	 * @return Whether this server links the client libraries.
	 */
	STEAMWORKSCOMMON_API bool IsLoadingServerClientDlls() const { return bServerNeedsClientLibrary; }

	/**
	 * Convenience accessor for this module.
	 * Do not reach for it while the process is tearing down; by then it may be gone.
	 *
	 * @return Returns a singleton instance, loading the module on demand if needed
	 */
	STEAMWORKSCOMMON_API static FSteamworksCommonModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FSteamworksCommonModule>(TEXT("SteamworksCommon"));
	}

	/**
	 * Whether Get is safe to call.
	 *
	 * @return Whether the module is up.
	 */
	STEAMWORKSCOMMON_API static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("SteamworksCommon"));
	}
	
private:
	/** Handle to the STEAM API dll */
	void* ClientLibrary { nullptr };

	/** Handle to the STEAM dedicated server support dlls */
	void* ServerLibrary { nullptr };

	/** If we force loaded the steam client dlls due to launch flags */
	bool bServerNeedsClientLibrary { false };

	/** Load the required modules for Steam */
	void LoadSteamModules();

	/** Unload the required modules for Steam */
	void UnloadSteamModules();
};
