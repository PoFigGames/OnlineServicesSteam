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
			]
		);
		
		// The online services engine utils, which map a world onto its services instance, are only exposed
		// from 5.8 on; earlier engines are served by the fallback in UOnlineServicesSteamInterface.
		if (target.Version.MajorVersion > 5 || (target.Version.MajorVersion == 5 && target.Version.MinorVersion >= 8))
		{
			PrivateDependencyModuleNames.Add("OnlineSubsystemUtils");
		}

		AddEngineThirdPartyPrivateStaticDependencies(target, "SteamworksSDK");
    }
}
