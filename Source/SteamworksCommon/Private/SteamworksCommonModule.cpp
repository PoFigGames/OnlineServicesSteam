// Copyright PoFig Games Studio. All Rights Reserved.


#include "SteamworksCommonModule.h"

#include "SteamServiceBase.h"
#include "SteamworksCommonLogChannels.h"

#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"


void FSteamworksCommonModule::StartupModule()
{
	// The libraries are linked before anything else runs, so a module which comes up later can call the
	// Steamworks APIs without checking whether they are there yet.
	LoadSteamModules();
}

void FSteamworksCommonModule::ShutdownModule()
{
	// The SDK expects its APIs to be shut down as the process goes away, and they have to be gone before the
	// libraries they live in are unlinked below.
	PoFigGames::Steam::ShutdownSteamServices();

	UnloadSteamModules();
}

bool FSteamworksCommonModule::AreSteamDllsLoaded() const
{
#if STEAM_CLIENT_LIBRARY_IS_DYNAMIC
	// Only the client library is answered for. The extra libraries a dedicated server can be asked to link
	// are a debugging aid, so failing to link them is logged and nothing more: saying the libraries are down
	// because of them would also say the Steamworks APIs may not be shut down, and then the API is never
	// closed and steam_appid.txt is never removed.
	return ClientLibrary != nullptr;
#else
	// A static link is always there.
	return true;
#endif
}

FString FSteamworksCommonModule::GetLibraryDirectory()
{
#if PLATFORM_WINDOWS
	return FPaths::ProjectDir() / TEXT("Binaries/ThirdParty/Steamworks") / STEAMWORKS_SDK_FOLDER / TEXT("Win64/");
#elif PLATFORM_MAC || PLATFORM_LINUX
	// SteamworksSDK.Build.cs stages the library at $(BinaryOutputDir) on these platforms, which is beside the
	// executable, not under Binaries/ThirdParty the way the Windows half of the same file does.
	return FString(FPlatformProcess::BaseDir());
#else
	return FString { };
#endif
}

void FSteamworksCommonModule::LoadSteamModules()
{
	if (AreSteamDllsLoaded())
	{
		return;
	}

	UE_LOG(LogSteamService, Display, TEXT("Linking the Steamworks libraries, SDK %s"), STEAMWORKS_SDK_VERSION);

#if PLATFORM_WINDOWS
	const auto LibraryDirectory = GetLibraryDirectory();
	const auto ClientLibraryFileName = TEXT("steam_api64.dll");

	FPlatformProcess::PushDllDirectory(*LibraryDirectory);
	ClientLibrary = FPlatformProcess::GetDllHandle(*(LibraryDirectory + ClientLibraryFileName));

	// The command line is read before it is asked whether this is a dedicated server, because in an editor
	// target that question is itself answered by reading the command line, and reading it before it is set
	// is fatal.
	if (FCommandLine::IsInitialized() && IsRunningDedicatedServer() && FParse::Param(FCommandLine::Get(), TEXT("force_steamclient_link")))
	{
		const auto ServerLibraryFileName = TEXT("steamclient64.dll");

		UE_LOG(LogSteamService, Log, TEXT("A dedicated server was asked to link the client libraries as well"));

		bServerNeedsClientLibrary = true;
		ServerLibrary = FPlatformProcess::GetDllHandle(*(LibraryDirectory + ServerLibraryFileName));

		if (ServerLibrary == nullptr)
		{
			UE_LOG(LogSteamService, Error, TEXT("Missing %s, tier0_s64.dll or vstdlib_s64.dll in %s. Copy them there from a Steam installation"),
				ServerLibraryFileName, *LibraryDirectory);
		}
	}

	FPlatformProcess::PopDllDirectory(*LibraryDirectory);

	// steam_api64.dll is delay loaded, so this handle is the only thing that binds it: without the notice
	// here, the first Steamworks call would fault instead of anything explaining why.
	if (ClientLibrary == nullptr)
	{
		UE_LOG(LogSteamService, Warning, TEXT("Could not link %s from %s, so nothing that needs Steam will work"), ClientLibraryFileName, *LibraryDirectory);
		return;
	}
#elif PLATFORM_MAC || (PLATFORM_LINUX && STEAM_CLIENT_LIBRARY_IS_DYNAMIC)
	#if PLATFORM_MAC
		const auto LibraryFileName = TEXT("libsteam_api.dylib");
	#else
		const auto LibraryFileName = TEXT("libsteam_api.so");
	#endif

	ClientLibrary = FPlatformProcess::GetDllHandle(LibraryFileName);

	if (ClientLibrary == nullptr)
	{
		// The system copy is the one Steam keeps current, so it is preferred; the copy staged beside the
		// executable is the fallback.
		UE_LOG(LogSteamService, Log, TEXT("No system copy of %s found, falling back to the staged one"), LibraryFileName);

		const auto LibraryDirectory = GetLibraryDirectory();
		ClientLibrary = FPlatformProcess::GetDllHandle(*(LibraryDirectory / LibraryFileName));
	}

	if (ClientLibrary == nullptr)
	{
		UE_LOG(LogSteamService, Warning, TEXT("Could not link %s, so nothing that needs Steam will work"), LibraryFileName);
		return;
	}

	UE_LOG(LogSteamService, Display, TEXT("Linked %s at %p"), LibraryFileName, ClientLibrary);
#endif

	UE_LOG(LogSteamService, Log, TEXT("Steamworks libraries are up"));
}

void FSteamworksCommonModule::UnloadSteamModules()
{
#if STEAM_LIBRARIES_ARE_DYNAMIC
	UE_LOG(LogSteamService, Log, TEXT("Releasing the Steamworks libraries"));

	if (ClientLibrary != nullptr)
	{
		FPlatformProcess::FreeDllHandle(ClientLibrary);
		ClientLibrary = nullptr;
	}

	if (ServerLibrary != nullptr)
	{
		FPlatformProcess::FreeDllHandle(ServerLibrary);
		ServerLibrary = nullptr;
	}

	bServerNeedsClientLibrary = false;
#endif
}

IMPLEMENT_MODULE(FSteamworksCommonModule, SteamworksCommon)
