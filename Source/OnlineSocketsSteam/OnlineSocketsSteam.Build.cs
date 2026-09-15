// Copyright PoFig Games Studio. All Rights Reserved.

using UnrealBuildTool;

public class OnlineSocketsSteam : ModuleRules
{
    public OnlineSocketsSteam(ReadOnlyTargetRules target) : base(target)
    {
        OnlineServicesSteamDefaults.Apply(this);

	    Type = ModuleType.CPlusPlus;

		PublicDependencyModuleNames.AddRange(
			[
				"Core",
				"Engine",
				"NetCore",
				"Sockets",
				"SteamworksCommon",
				"OnlineServicesInterface",
				"CoreOnline",
			]
		);

		PrivateDependencyModuleNames.AddRange(
			[
				"CoreOnline",
				"CoreUObject",
				"PacketHandler",
				"OnlineSubsystemUtils",
			]
		);

		AddEngineThirdPartyPrivateStaticDependencies(target, "SteamworksSDK");
	}
}
