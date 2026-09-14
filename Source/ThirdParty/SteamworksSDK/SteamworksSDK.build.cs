// Copyright PoFig Games Studio. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

public class SteamworksSDK : ModuleRules
{
    public SteamworksSDK(ReadOnlyTargetRules target) : base(target)
    {
        // The current SDK version number.
        const string SteamVersionNumber = "1.65";

        var SteamVersionInt = SteamVersionNumber.Replace(".", "");
        var SteamVersion = $"v{SteamVersionInt}";
        var SteamFolderName = $"Steam{SteamVersion}";

        Type = ModuleType.External;

        PublicDefinitions.Add("WITH_STEAM_SDK=1");
        PublicDefinitions.Add($"STEAMWORKS_SDK_VERSION=TEXT(\"{SteamVersionNumber}\")");
        PublicDefinitions.Add($"STEAMWORKS_SDK_VERSION_INT={SteamVersionInt}");
        PublicDefinitions.Add($"STEAMWORKS_SDK_FOLDER=TEXT(\"{SteamFolderName}\")");

        /* ----------  PATHS  --------------------------------------------------------- */
        var SdkBase = Path.Combine(PluginDirectory, "Source", target.UEThirdPartySourceDirectory, "SteamworksSDK", SteamFolderName);
        if (!Directory.Exists(SdkBase))
        {
            throw new BuildException($"Steamworks SDK {SteamVersionNumber} not found in {Path.GetFullPath(SdkBase)}");
        }

        PublicSystemIncludePaths.Add(Path.Combine(SdkBase, "public"));

        var TargetBaseDir = Path.Combine("$(ProjectDir)", "Binaries", "ThirdParty", "Steamworks", SteamFolderName);

        /* ----------  WINDOWS 64-bit ------------------------------------------------- */
        if (target.Platform == UnrealTargetPlatform.Win64)
        {
            var LibDir = Path.Combine(SdkBase, "redistributable_bin", "win64");
            const string Dll = "steam_api64.dll";
            const string Lib = "steam_api64.lib";

            PublicAdditionalLibraries.Add(Path.Combine(LibDir, Lib));
            PublicDelayLoadDLLs.Add(Dll);
            
            var TargetPlatformDir = Path.Combine(TargetBaseDir, "Win64");
            RuntimeDependencies.Add(Path.Combine(TargetPlatformDir, Dll), Path.Combine(LibDir, Dll));

            if (target.Type == TargetType.Server)
            {
                foreach (var extra in new[] { "steamclient64.dll", "tier0_s64.dll", "vstdlib_s64.dll" })
                {
                    var src = Path.Combine(LibDir, extra);
                    if (File.Exists(src))
                    {
                        RuntimeDependencies.Add(Path.Combine(TargetPlatformDir, extra), src);
                    }
                }
            }
        }
        /* ----------  LINUX 64-bit --------------------------------------------------- */
        else if (target.Platform == UnrealTargetPlatform.Linux)
        {
            var LibDir = Path.Combine(SdkBase, "redistributable_bin", "linux64");
            
            const string So = "libsteam_api.so";
            var FullSoPath = Path.Combine(LibDir, So);
            
            PublicAdditionalLibraries.Add(FullSoPath);
            PublicRuntimeLibraryPaths.Add(LibDir);
            PublicDelayLoadDLLs.Add(FullSoPath);
            
            RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", So), FullSoPath);
        }
        /* ----------  macOS (Universal) --------------------------------------------- */
        else if (target.Platform == UnrealTargetPlatform.Mac)
        {
            var LibDir = Path.Combine(SdkBase, "redistributable_bin", "osx");
            
            const string Dylib = "libsteam_api.dylib";
            var FullDylibPath = Path.Combine(LibDir, Dylib);
            
            PublicAdditionalLibraries.Add(FullDylibPath);
            
            RuntimeDependencies.Add(Path.Combine("$(BinaryOutputDir)", Dylib), FullDylibPath);
        }
	}
}
