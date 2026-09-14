// Copyright PoFig Games Studio. All Rights Reserved.

using System.IO;
using UnrealBuildTool;

/**
 * Settings shared by every module of this plugin.
 *
 * The plugin is built inside whatever project takes it, so it cannot read that project's rules; it asks
 * for what it needs itself, and every one of its modules is then compiled the same way wherever it lands.
 */
public static class OnlineServicesSteamDefaults
{
    /**
     * Diagnostics this plugin holds itself to.
     *
     * Almost everything here describes a defect rather than a matter of taste, so it fails the build
     * instead of scrolling past in a log. A diagnostic stays a warning only where the plugin cannot act
     * on it: engine headers it merely instantiates also raise it, and failing somebody's build over
     * engine code is not this plugin's place. Each such case says so where it is set.
     *
     * The settings live on the module and not on the target, because a target in the shared build
     * environment carries no compiler settings of its own; asking here is what makes an editor build and
     * a packaged build agree, and it is also the only form available to a plugin, which has no target.
     *
     * Two of these bite on clang first, because MSVC keeps the matching diagnostic off: a local hiding a
     * member of a base class (C4458), and a switch with no default label that misses an enumerator
     * (C4062). Write for the stricter of the two rather than for whichever compiler is at hand.
     */
    public static void Apply(ModuleRules module)
    {
        var warnings = module.CppCompileWarningSettings;

        // ---- Reaching clang and MSVC, which is what this plugin is built with -------------------

        // Names. A local which hides a member reads as the wrong thing entirely. MSVC also reports a
        // local hiding a member of a base class (C4458) where clang reports nothing, so name locals
        // apart from inherited members even when the local build is happy.
        warnings.ShadowVariableWarningLevel = WarningLevel.Error;

        // Truncation is deliberately NOT raised here, for the same reason as UnsafeTypeCast below: both
        // knobs fire inside PerModuleInline.inl, where REPLACEMENT_OPERATOR_NEW_AND_DELETE and
        // UE_DEFINE_FMEMORY_WRAPPERS narrow size_t to uint32. That header is generated into every module
        // of this plugin and is not ours to edit, so raising either one fails the build on Windows and
        // Linux while passing on macOS. Narrow deliberately with IntCastChecked<T>(); see CONTRIBUTING.md.

        // Enums, of which the Steamworks SDK has a great many, silently becoming numbers or each other.
        warnings.EnumConversionWarningLevel = WarningLevel.Error;
        warnings.EnumEnumConversionWarningLevel = WarningLevel.Error;
        warnings.EnumFloatConversionWarningLevel = WarningLevel.Error;

        // A switch over an enum with no default label has to name every value. Not
        // SwitchUnhandledEnumeratorWarningLevel, which demands the same of a switch that does have a
        // default and is unusable against Steam's EResult and its 125 values. MSVC keeps its
        // counterpart (C4062) off, so this one bites on clang first.
        warnings.SwitchWarningLevel = WarningLevel.Error;

        // Comparisons and operators which cannot mean what they say.
        warnings.TautologicalCompareWarningLevel = WarningLevel.Error;
        warnings.BitwiseInsteadOfLogicalWarningLevel = WarningLevel.Error;

        // Memory and destruction: a memcpy over a non-trivial type, and deleting through a base with no
        // virtual destructor, both compile and then fail somewhere else.
        warnings.NonTrivialMemAccessWarningLevel = WarningLevel.Error;
        warnings.DeleteNonVirtualDtorWarningLevel = WarningLevel.Error;

        // InconsistentMissingOverride is likewise not raised: it fires on GameModeBase.h and GameSession.h,
        // which NetDriverSteam includes. It did find three missing overrides in this plugin before it was
        // turned back off, so run it by hand now and again rather than leaving it on.

        // The plugin branches on WITH_STEAM_SDK and STEAM_SDK_INSTALLED, and a name misspelled in an #if
        // quietly evaluates to zero.
        warnings.UndefinedIdentifierWarningLevel = WarningLevel.Error;

        // ---- Reaching clang-cl on Windows only ---------------------------------------------------
        //
        // The build system maps these onto the VC toolchain driving clang, and nothing else: they are
        // silent on plain clang and on MSVC. Set anyway, so that anyone who does build the plugin that
        // way gets the checks, but do not expect them to catch anything here.
        warnings.ReturnTypeWarningLevel = WarningLevel.Error;
        warnings.UninitializedWarningLevel = WarningLevel.Error;
        warnings.DanglingWarningLevel = WarningLevel.Error;
        warnings.UndefinedBoolConversionWarningLevel = WarningLevel.Error;
        warnings.ConstantLogicalOperandWarningLevel = WarningLevel.Error;
        warnings.LogicalOpParenthesesWarningLevel = WarningLevel.Error;
        warnings.NullArithmeticWarningLevel = WarningLevel.Error;
        warnings.NullPointerArithmeticWarningLevel = WarningLevel.Error;
        warnings.NullPointerSubtractionWarningLevel = WarningLevel.Error;
        warnings.BitfieldEnumConversion = WarningLevel.Error;
        warnings.InvalidTokenPasteWarningLevel = WarningLevel.Error;
        warnings.ExpansionToDefined = WarningLevel.Error;

        // ---- Deliberately not set ----------------------------------------------------------------
        //
        // UnsafeTypeCastWarningLevel would also catch a narrowing int32 to uint16, which is the one real
        // gap left above. On clang it is a single knob for both halves, and its floating point half
        // fires 68 times per translation unit inside OnlineAsyncOpQueue.h and OnlineAsyncOpCache.h,
        // engine headers this plugin only instantiates. Raising it fails the build on engine code and
        // lowering it to a warning buries every diagnostic that does mean something; wrapping the
        // include in PRAGMA_DISABLE_UNSAFE_TYPECAST_WARNINGS does not help, because those headers arrive
        // through the shared PCH. Narrow deliberately with IntCastChecked<T>(); see CONTRIBUTING.md.
        //
        // FormatWarningLevel, RangeLoopConstructWarningLevel, PragmaOnceOutsideHeaderWarningLevel,
        // LogicalNotParenthesesWarningLevel and SingleBitfieldConstantConversionWarningLevel are mapped
        // onto the Intel compiler alone, so setting them would promise a check that never runs.
    }
}

public class SteamworksCommon : ModuleRules
{
    public SteamworksCommon(ReadOnlyTargetRules target) : base(target)
    {
        OnlineServicesSteamDefaults.Apply(this);

        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(
            [
                "Core",
                "CoreOnline",
                "Sockets",
            ]
        );

        PrivateDependencyModuleNames.AddRange(
            [
                "CoreUObject",
                "Engine"
            ]
        );

        // The SDK is taken privately, so WITH_STEAM_SDK, which it publishes about itself, reaches this
        // module and stops there. What a module depending on this one asks instead is STEAM_SDK_INSTALLED:
        // Steam is part of the build it is being compiled into, without linking the SDK itself.
        AddEngineThirdPartyPrivateStaticDependencies(target, "SteamworksSDK");
        PublicDefinitions.Add("STEAM_SDK_INSTALLED");
    }
}