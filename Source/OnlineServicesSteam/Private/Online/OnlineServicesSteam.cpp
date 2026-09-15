// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/OnlineServicesSteam.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "OnlineSocketsSteamModule.h"
#include "SteamNetAddress.h"
#include "SteamPlatformConfig.h"
#include "Online/AchievementsSteam.h"
#include "Online/AuthSteam.h"
#include "Online/LeaderboardsSteam.h"
#include "Online/LobbiesSteam.h"
#include "Online/OnlineIdSteam.h"
#include "Online/PresenceSteam.h"
#include "Online/PrivilegesSteam.h"
#include "Online/SocialSteam.h"
#include "Online/StatsSteam.h"
#include "Online/UserFileSteam.h"
#include "Online/UserInfoSteam.h"
#include "Steam/SteamClientService.h"
#include "Steam/SteamServerService.h"


namespace PoFigGames::Online
{
	FOnlineServicesSteam::FOnlineServicesSteam(FName InInstanceName, FName InstanceConfigName)
		: FOnlineServicesCommon(GetServiceConfigNameStatic(), InInstanceName, InstanceConfigName)
	{
	}

	bool FOnlineServicesSteam::PreInit()
	{
		LoadConfig(SteamConfig);

		// A dedicated server only ever brings up the game server API; a client brings up the client API and,
		// when it hosts, the game server one as well. Which services this instance holds is the single answer
		// to "what role is this instance in": nothing below asks the environment about it a second time.
		// The services are process wide, so a second instance joins the APIs which are already running.
		if (IsRunningDedicatedServer())
		{
			ServerService = Steam::AcquireSteamService<Steam::FSteamServerService>(SteamConfig);
		}
		else
		{
			ClientService = Steam::AcquireSteamService<Steam::FSteamClientService>(SteamConfig);

			if (ClientService.IsValid() && SteamConfig.bInitServerOnClient)
			{
				ServerService = Steam::AcquireSteamService<Steam::FSteamServerService>(SteamConfig);

				if (!ServerService.IsValid())
				{
					// The client half is up, so the instance still works; it just cannot host.
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("Failed to initialize the Steam game server API, hosting is not available"));
				}
			}
		}

		if (!ClientService.IsValid() && !ServerService.IsValid())
		{
			UE_LOG(LogOnlineServicesSteam, Error, TEXT("Failed to initialize a Steamworks Service"));
			ReleaseSteamServices();

			return false;
		}

		// Steam sockets are optional: with them disabled the game keeps the engine's own socket subsystem.
		// When they are wanted, the transport module is asked for the one subsystem of this process rather
		// than made another: it registers under a name the engine keeps a single entry for.
		if (SteamConfig.bUseSteamTransport)
		{
			if (FString Error; !FOnlineSocketsSteamModule::Get().EnsureSocketSubsystem(Error))
			{
				UE_LOG(LogOnlineServicesSteam, Error, TEXT("Failed to initialize Steamworks Socket Subsystem: %s"), *Error);
				ReleaseSteamServices();

				return false;
			}
		}
		else
		{
			UE_LOG(LogOnlineServicesSteam, Log, TEXT("Steam sockets are disabled by config, the Steam socket subsystem is not registered"));
		}

		// Left for the handshake, which knows who the other end is and cannot ask for a ticket itself: the
		// transport module does not know this module and must not. Stateless, so one binding serves every
		// instance - the auth interface to ask is handed in by the caller.
		FOnlineSocketsSteamModule::Get().SetBoundTicketRequest(FSteamBoundTicketRequest::CreateStatic(&FAuthSteam::RequestBoundAuthTicket));

		AccountIdRegistry = static_cast<FOnlineAccountIdRegistrySteam*>(UE::Online::FOnlineIdRegistryRegistry::Get().GetAccountIdRegistry(GetServicesProvider()));

		LoadConfig(CallConfig);

		// Broadcast callbacks have to be registered on the pipe of the API this instance actually brought up.
		CallDispatcher = MakeUnique<Steam::FSteamCallDispatcher>(ClientService.IsValid()
			? Steam::ESteamCallContext::Client
			: Steam::ESteamCallContext::GameServer);
		CallDispatcher->SetConfig(CallConfig);

		return true;
	}

	void FOnlineServicesSteam::Destroy()
	{
		FOnlineServicesCommon::Destroy();

		// A request in flight answers a component, so the requests are given up first, while the components
		// they would answer are still there to hear it. Nothing may be waiting on a Steam callback once the
		// API below is shut down either.
		if (CallDispatcher)
		{
			CallDispatcher->CancelAll(UE::Online::Errors::Cancelled());
		}

		UnregisterComponents();

		CallDispatcher.Reset();

		// The socket subsystem is not taken down here: it belongs to the transport module and outlives any
		// one services instance, the way the Steamworks API itself does.
		ReleaseSteamServices();

		AccountIdRegistry = nullptr;
	}

	void FOnlineServicesSteam::ReleaseSteamServices()
	{
		// The APIs themselves stay up: they belong to the process and are shut down when the module unloads.
		ServerService.Reset();
		ClientService.Reset();
	}

	void FOnlineServicesSteam::RegisterComponents()
	{
		Components.Register<FAuthSteam>(*this);
		Components.Register<FLeaderboardsSteam>(*this);

		// Lobbies are Steam matchmaking, which only the client API offers: registering them on a dedicated
		// server would leave a component that cannot resolve its interface.
		if (ClientService.IsValid())
		{
			Components.Register<FLobbiesSteam>(*this);
		}

		Components.Register<FPresenceSteam>(*this);
		Components.Register<FPrivilegesSteam>(*this);
		Components.Register<FStatsSteam>(*this);
		Components.Register<FSocialSteam>(*this);
		Components.Register<FAchievementsSteam>(*this);
		Components.Register<FUserFileSteam>(*this);
		Components.Register<FUserInfoSteam>(*this);
	}

	void FOnlineServicesSteam::UpdateConfig()
	{
		FOnlineServicesCommon::UpdateConfig();

		LoadConfig(CallConfig);

		if (CallDispatcher)
		{
			CallDispatcher->SetConfig(CallConfig);
		}
	}

	void FOnlineServicesSteam::UnregisterComponents()
	{
		Components.Unregister<FUserInfoSteam>();
		Components.Unregister<FUserFileSteam>();
		Components.Unregister<FAchievementsSteam>();
		Components.Unregister<FStatsSteam>();
		Components.Unregister<FPrivilegesSteam>();
		Components.Unregister<FPresenceSteam>();
		Components.Unregister<FLobbiesSteam>();
		Components.Unregister<FLeaderboardsSteam>();
		Components.Unregister<FAuthSteam>();
		Components.Unregister<FSocialSteam>();
	}

	bool FOnlineServicesSteam::Tick(const float DeltaSeconds)
	{
		if (CallDispatcher)
		{
			CallDispatcher->Tick(FPlatformTime::Seconds());
		}

		// A shared service dispatches once per frame however many instances tick it.
		if (ClientService.IsValid())
		{
			ClientService->PumpDispatch();
		}

		if (ServerService.IsValid())
		{
			ServerService->PumpDispatch();
		}

		return FOnlineServicesCommon::Tick(DeltaSeconds);
	}

	UE::Online::TOnlineResult<UE::Online::FGetResolvedConnectString> FOnlineServicesSteam::GetResolvedConnectString(UE::Online::FGetResolvedConnectString::Params&& Params)
	{
		if (Params.LobbyId.IsValid())
		{
			const auto LobbiesSteam = GetLobbiesInterface();
			if (!LobbiesSteam.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FOnlineServicesSteam::GetResolvedConnectString] Failed: Lobbies interface is not available"));
				return UE::Online::TOnlineResult<UE::Online::FGetResolvedConnectString>(UE::Online::Errors::MissingInterface());
			}

			auto JoinedLobbiesResult = LobbiesSteam->GetJoinedLobbies({ Params.LocalAccountId });
			if (JoinedLobbiesResult.IsOk())
			{
				const auto& [Lobbies] = JoinedLobbiesResult.GetOkValue();
				for (const auto& Lobby : Lobbies)
				{
					if (Lobby->LobbyId == Params.LobbyId)
					{
						// The address is the one its host bound through SetLobbyGameServer, which is where
						// Steam keeps it; a lobby whose host has started no server yet has none.
						const auto GameServer = Get<FLobbiesSteam>() ? Get<FLobbiesSteam>()->GetLobbyGameServer(Params.LobbyId) : TOptional<Steam::Wrappers::FSteamLobbyGameServer> { };

						Steam::FSteamNetAddress ServerAddress;
						if (GameServer.IsSet() && GameServer->ServerId.IsValid())
						{
							ServerAddress.SetSteamID(GameServer->ServerId.ConvertToUint64());
						}
						else if (GameServer.IsSet() && GameServer->ServerIp > 0)
						{
							ServerAddress.SetNetworkingAddr(GameServer->ServerIp, GameServer->ServerPort);
						}
						else
						{
							UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FOnlineServicesSteam::GetResolvedConnectString] Failed: Not valid connection found. User [%s]"), *ToLogString(Params.LocalAccountId));
							return UE::Online::TOnlineResult<UE::Online::FGetResolvedConnectString>(UE::Online::Errors::NoConnection());
						}

						return UE::Online::TOnlineResult<UE::Online::FGetResolvedConnectString>({ ServerAddress.ToString(true) });
					}
				}

				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FOnlineServicesSteam::GetResolvedConnectString] Failed: Not matching lobby found. Lobby [%s] User [%s]"), *ToLogString(Params.LobbyId), *ToLogString(Params.LocalAccountId));
				return UE::Online::TOnlineResult<UE::Online::FGetResolvedConnectString>(UE::Online::Errors::NotFound());
			}

			return UE::Online::TOnlineResult<UE::Online::FGetResolvedConnectString>(JoinedLobbiesResult.GetErrorValue());
		}

		// A session id names nothing on Steam: what the game calls a session is a lobby, which the branch
		// above answers for.
		UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FOnlineServicesSteam::GetResolvedConnectString] Failed: Not valid lobby id found. User [%s]"), *ToLogString(Params.LocalAccountId));
		return UE::Online::TOnlineResult<UE::Online::FGetResolvedConnectString>(UE::Online::Errors::InvalidParams());
	}
}

namespace UE::Online::Meta
{
	BEGIN_ONLINE_STRUCT_META(PoFigGames::Steam::FSteamCallConfig)
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamCallConfig, RequestTimeoutSeconds),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamCallConfig, BackendRequestTimeoutSeconds)
	END_ONLINE_STRUCT_META()

	BEGIN_ONLINE_STRUCT_META(PoFigGames::Steam::FSteamPlatformConfig)
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, bUseSteamTransport),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, bOverrideDefaultSubsystem),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, bUseSymmetricConnect),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, Transport),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, bRelaunchInSteam),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, bInitServerOnClient),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, bUseRelay),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, bVACEnabled),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, bAdvertiseServer),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, ServerName),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, GameServerQueryPort),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, SteamAppId),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, ProductName),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, GameDirectory),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, GameVersion),
		ONLINE_STRUCT_FIELD(PoFigGames::Steam::FSteamPlatformConfig, GameDescription)
	END_ONLINE_STRUCT_META()
}
