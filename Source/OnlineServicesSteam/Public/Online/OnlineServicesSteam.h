// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineServicesCommon.h"
#include "OnlineIdSteam.h"
#include "SocketSubsystemSteam.h"
#include "Steam/SteamCallDispatcher.h"
#include "SteamServiceBase.h"

namespace PoFigGames::Steam
{
	class FSteamClientService;
	class FSteamServerService;
}

namespace PoFigGames::Online {
	using FAuthClientSteamPtr = TSharedPtr<class FAuthSteam>;
	using FAuthServerSteamPtr = TSharedPtr<class FAuthServerSteam>;

	/**
	 * @class FOnlineServicesSteam
	 *
	 * Main Steam online services class.
	 * Manages the initialization and lifecycle of the service interfaces (Client/Server) depending on the build type.
	 */
	class FOnlineServicesSteam : public UE::Online::FOnlineServicesCommon
	{
	public:
		using Super = FOnlineServicesCommon;
		using FOnlineServicesCommon::FOnlineServicesCommon;

		ONLINESERVICESSTEAM_API FOnlineServicesSteam(FName InInstanceName, FName InstanceConfigName);

		ONLINESERVICESSTEAM_API virtual void RegisterComponents() override;
		ONLINESERVICESSTEAM_API virtual void UpdateConfig() override;

		ONLINESERVICESSTEAM_API virtual bool PreInit();
		ONLINESERVICESSTEAM_API virtual void Destroy() override;

		ONLINESERVICESSTEAM_API virtual bool Tick(float DeltaSeconds) override;

		ONLINESERVICESSTEAM_API virtual UE::Online::EOnlineServices GetServicesProvider() const override { return UE::Online::EOnlineServices::Steam; }
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FGetResolvedConnectString> GetResolvedConnectString(UE::Online::FGetResolvedConnectString::Params&& Params) override;

		/** Owns every in-flight Steam request of this services instance. Valid between PreInit and Destroy. */
		Steam::FSteamCallDispatcher& GetCallDispatcher() const
		{
			check(CallDispatcher);
			return *CallDispatcher;
		}

		/** The client API service, or null when this instance runs on a dedicated server. */
		const TSharedPtr<Steam::FSteamClientService>& GetClientService() const { return ClientService; }

		/** The game server API service, or null when this instance does not host one. */
		const TSharedPtr<Steam::FSteamServerService>& GetServerService() const { return ServerService; }

		FOnlineAccountIdRegistrySteam& GetAccountIdRegistry() const { return *AccountIdRegistry; }
		FOnlineAccountIdRegistrySteam& GetAccountIdRegistry() { return *AccountIdRegistry; }

		const Steam::FSteamPlatformConfig& GetSteamConfig() const { return SteamConfig; }

		static const TCHAR* GetServiceConfigNameStatic() { return TEXT("Steam"); }

	protected:
		TUniquePtr<Steam::FSocketSubsystemSteam> SocketSubsystem { nullptr };
		TUniquePtr<Steam::FSteamCallDispatcher> CallDispatcher { nullptr };

		/**
		 * The Steamworks APIs this instance uses. The services are process wide singletons owned by the
		 * SteamworksCommon module, so these handles are for using an API, never for ending it.
		 */
		TSharedPtr<Steam::FSteamClientService> ClientService { nullptr };
		TSharedPtr<Steam::FSteamServerService> ServerService { nullptr };
		Steam::FSteamPlatformConfig SteamConfig { };
		Steam::FSteamCallConfig     CallConfig { };

		FOnlineAccountIdRegistrySteam* AccountIdRegistry { nullptr };

		ONLINESERVICESSTEAM_API virtual void UnregisterComponents();

		/** Releases the API handles this instance holds. */
		ONLINESERVICESSTEAM_API void ReleaseSteamServices();
	};

	using FOnlineServicesSteamPtr = TSharedPtr<FOnlineServicesSteam>;

}
