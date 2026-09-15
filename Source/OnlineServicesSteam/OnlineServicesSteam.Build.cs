// Copyright PoFig Games Studio. All Rights Reserved.

using UnrealBuildTool;

public class OnlineServicesSteam : ModuleRules
{
	public OnlineServicesSteam(ReadOnlyTargetRules target) : base(target)
    {
        OnlineServicesSteamDefaults.Apply(this);

	    PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

	    PublicDependencyModuleNames.AddRange(
			[
				"OnlineServicesInterface",
				"OnlineServicesCommonEngineUtils",
				"OnlineServicesCommon",
				"OnlineSocketsSteam",
				"SteamworksCommon",
			]
		);

		PrivateDependencyModuleNames.AddRange(
			[
				"Core",
				"CoreOnline",
				"ApplicationCore",
				"OnlineBase",
				"Sockets",
				"Engine",
				"CoreUObject",
				"Json",
				"ImageWrapper",
				"OnlineSubsystemUtils",
			]
		);

		AddEngineThirdPartyPrivateStaticDependencies(target, "SteamworksSDK");
    }
}
