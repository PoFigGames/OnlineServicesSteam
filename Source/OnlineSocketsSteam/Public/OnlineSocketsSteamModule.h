// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "Online/Auth.h"
#include "Online/OnlineServices.h"
#include "SteamNetAddress.h"
#include "Templates/UniquePtr.h"


namespace PoFigGames::Steam
{
	class FSocketSubsystemSteam;
}


/**
 * Asks for a ticket that names the host it is for, so that the host cannot show it on somewhere else.
 *
 * Only the online services can ask Steam for a ticket, and only the transport knows who the other end of a
 * connection is. The services leave this here for the handshake to call, which is the way round the module
 * dependencies allow: the services know about the transport and not the other way about.
 */
DECLARE_DELEGATE_RetVal_ThreeParams(
	UE::Online::TOnlineAsyncOpHandle<UE::Online::FAuthQueryVerifiedAuthTicket>,
	FSteamBoundTicketRequest,
	const UE::Online::IAuthPtr& /*Auth*/,
	UE::Online::FAuthQueryVerifiedAuthTicket::Params /*Params*/,
	const PoFigGames::Steam::FSteamNetAddress& /*Peer*/);


/**
 * @class FOnlineSocketsSteamModule
 *
 * @brief Brings the Steam socket subsystem up with the module and takes it down with it.
 */
class FOnlineSocketsSteamModule : public IModuleInterface
{
public:
	ONLINESOCKETSSTEAM_API FOnlineSocketsSteamModule();
	ONLINESOCKETSSTEAM_API virtual ~FOnlineSocketsSteamModule() override;

	virtual void StartupModule() override;
	ONLINESOCKETSSTEAM_API virtual void ShutdownModule() override;

	/**
	 * Brings the socket subsystem up if it is not up already, and answers whether it is.
	 *
	 * There is one for the whole process, because it registers itself under a name the engine keeps one
	 * entry for: a second one would take that entry from the first and leave every socket the first handed
	 * out pointing at bookkeeping nobody can reach. Asked for by whoever needs Steam to carry traffic,
	 * which is the online services instance reading its own configuration.
	 */
	ONLINESOCKETSSTEAM_API bool EnsureSocketSubsystem(FString& OutError);

	/** Left here by the services side once it is up; see FSteamBoundTicketRequest. */
	ONLINESOCKETSSTEAM_API void SetBoundTicketRequest(FSteamBoundTicketRequest InRequest);

	const FSteamBoundTicketRequest& GetBoundTicketRequest() const { return BoundTicketRequest; }

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

private:
	/** The one socket subsystem of this process, null until somebody asks for Steam to carry traffic. */
	TUniquePtr<PoFigGames::Steam::FSocketSubsystemSteam> SocketSubsystem { nullptr };

	FSteamBoundTicketRequest BoundTicketRequest { };
};
