# Contributing

Thank you for looking. Issues and pull requests are both welcome; this file says what makes them easy to
act on and how the code is written.

## Reporting a problem

Steam problems are notoriously hard to reproduce from a description alone, so please include:

- Engine version and plugin version or commit.
- Platform, and whether you were in the editor, in PIE, or in a packaged build.
- Steamworks SDK version.
- The relevant `[OnlineServices.Steam*]` config, with your App ID redacted if you prefer.
- `LogOnlineServicesSteam`, `LogSteamService` and `LogOnlineSocketsSteam` output around the failure,
  at `Verbose` if you can:

  ```ini
  [Core.Log]
  LogOnlineServicesSteam=Verbose
  LogSteamService=Verbose
  LogOnlineSocketsSteam=Verbose
  ```

Please do not paste an auth ticket, a Web API key or a full session log without reading it first.

## Pull requests

- One change per pull request, with a description saying what the old behaviour was and what the new one
  is. Say how you tested it — Steam code that has never been run against a live client usually does not
  work.
- No new dependency on any particular game or project. The plugin builds inside whatever project takes
  it, so nothing in `Source/` may reference a host project's modules, target rules, tags or config.
- Do not commit the Steamworks SDK, generated binaries or intermediates; `.gitignore` already excludes
  them.
- Say whether the change alters the on-the-wire format of lobby attributes or of the transport, because
  that breaks compatibility with builds already in players' hands.

## Code style

The style is Unreal's, with a few settled choices. `.clang-format` and `.editorconfig` in the repository
root carry the mechanical part; the rest:

- Tabs, width 4. Allman braces. Line limit 150.
- Locals are declared with `auto`, `const auto`, `auto&` or `const auto&`. No `auto*` — a pointer is
  deduced by plain `auto`. Write an explicit type only where the initialiser does not name it.
- Members use brace initialisation with spaces: `bool bFlag { false };`, `FString Name { };`. An empty
  value is `FString { }`, never `FString()`.
- Every `class`, `struct`, `concept` and `enum class` carries a Doxygen block:

  ```cpp
  /**
   * @class FAuthSteam
   *
   * @brief Steam authentication.
   *
   * A longer description where one is warranted.
   */
  ```

  `@class` for classes, `@struct` for structs and concepts, `@enum` for enums. Single-line comments on
  members need no tags.
- Includes are sorted alphabetically.
- Every file starts with `// Copyright PoFig Games Studio. All Rights Reserved.`
- Identifiers, comments and log messages are in English.
- The plugin compiles with a strict set of diagnostics, listed and explained in
  `OnlineServicesSteamDefaults` in `Source/SteamworksCommon/SteamworksCommon.Build.cs`. Narrowing an
  64-bit value, shadowing a member, an enum becoming a number, missing an `override`, a switch with no
  default that misses an enumerator and more are errors. That file also says which compiler each check
  actually reaches, so read it before assuming a green build means much on one platform. Do not silence one by loosening the setting; fix the code, or say in the
  pull request why the diagnostic is wrong here.
- Two of those bite on clang before MSVC, because MSVC keeps the matching diagnostic off: a local hiding
  a member of a *base* class (C4458) and a switch with no default label (C4062). Write for the stricter
  compiler — a Windows-only build will not catch either.
- Narrow an integer with `IntCastChecked<T>()` rather than `static_cast`, unless the value is provably in
  range: a SteamID or a port losing its top bits connects you to the wrong peer instead of failing.

## Reaching the Steam APIs

Two rules keep the call sites short and the plumbing in one place:

- Ask for an interface through the typed accessor, `GetSteamInterface<ISteamFriends>()`, which is where
  the "client API, else server API, else nothing" cascade is written once.
- Inside an online component, use the mixin: `SteamCall<...>()`, `SteamListen<...>()`,
  `SteamCallSync<...>()`. If you find yourself reaching through two or three arrows to get at the
  dispatcher, add a helper instead of repeating the chain.

## Licence of contributions

By opening a pull request you agree that your contribution is licensed under the [MIT Licence](LICENSE),
on the same footing as the rest of this repository's own code — and that the contribution is yours to
license. Do not paste Unreal Engine source into a pull request: parts of this plugin already descend from
it (see [NOTICE.md](NOTICE.md)), and adding more is the one thing that cannot be fixed after the fact.
