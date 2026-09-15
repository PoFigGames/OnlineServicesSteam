// Copyright PoFig Games Studio. All Rights Reserved.

#include "OnlineSocketsSteamModule.h"

#include "OnlineSocketsSteamLogChannels.h"
#include "SocketSubsystemSteam.h"

DEFINE_LOG_CATEGORY(LogOnlineSocketsSteam);

// Both out of line, because the member they destroy is only forward declared in the header.
FOnlineSocketsSteamModule::FOnlineSocketsSteamModule() = default;
FOnlineSocketsSteamModule::~FOnlineSocketsSteamModule() = default;

void FOnlineSocketsSteamModule::StartupModule()
{

}

void FOnlineSocketsSteamModule::ShutdownModule()
{
	// Taken down with the module rather than with whoever asked for it: the sockets it handed out are the
	// engine's to give back, and an online services instance going away is not the process going away.
	if (SocketSubsystem.IsValid())
	{
		SocketSubsystem->Shutdown();
		SocketSubsystem.Reset();
	}
}

void FOnlineSocketsSteamModule::SetBoundTicketRequest(FSteamBoundTicketRequest InRequest)
{
	BoundTicketRequest = MoveTemp(InRequest);
}

bool FOnlineSocketsSteamModule::EnsureSocketSubsystem(FString& OutError)
{
	if (SocketSubsystem.IsValid())
	{
		return true;
	}

	auto NewSubsystem = MakeUnique<PoFigGames::Steam::FSocketSubsystemSteam>();

	// Dropped rather than shut down when it refuses: Init registers itself only once nothing is left to
	// fail, so there is nothing registered to take back.
	if (!NewSubsystem->Init(OutError))
	{
		return false;
	}

	SocketSubsystem = MoveTemp(NewSubsystem);

	return true;
}

IMPLEMENT_MODULE(FOnlineSocketsSteamModule, OnlineSocketsSteam);

