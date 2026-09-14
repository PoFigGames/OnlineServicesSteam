# Changelog

All notable changes to this plugin are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

While the version stays below 1.0 the public API is not stable: anything may change in a minor version,
and no deprecation is promised. 1.0 is what this becomes once the whole of it has been exercised against
live Steam on more than one machine — see [Not yet verified](#not-yet-verified) for what that still means.

## [0.9.0]

First tagged pre-release. Feature complete for what it sets out to cover, and complete enough to build
against, but not yet proven against live Steam.

### Added

- Play In Editor support: lobbies, invitations and travel to the host work from inside the editor,
  session after session, because `SteamworksCommon` owns the Steamworks API for the life of the process
  rather than for the life of a session. The Steam session itself ends only when the editor closes.
- OSSv2 services over the Steamworks SDK v1.65: `IAuth`, `ILobbies`, `IPresence`, `ISocial`,
  `IUserInfo`, `IAchievements`, `IStats`, `ILeaderboards`, `IUserFile`, `IPrivileges`.
- `OnlineSocketsSteam`: socket subsystem, net driver and net connection over
  `ISteamNetworkingSockets` or `ISteamNetworkingMessages`, selected by config, with `IpNetDriver` left
  as the fallback.
- One setting, `bUseRelay`, decides whether a match is spoken over the relay or over an address, and it
  answers for the host and the client alike. A LAN match is spoken over IP, from the flag in the URL.
- Net driver defaults in the plugin's own config section. The engine writes connection timeouts and
  rates under the IP driver's section, which `UNetDriverSteam` does not inherit, so a driver left to the
  engine's defaults would come up with no timeouts at all.
- `SteamworksCommon`: process-wide ownership of the Steamworks APIs, typed interface accessors, the
  platform config, Steam identities and addresses.
- Verified auth sessions closed on every path: on refusal, on timeout, and on a verdict that changes
  long after the request that asked for it, so a player is never locked out of rejoining by Steam
  answering `DuplicateRequest`.
- A dedicated server's anonymous login waits for Steam to confirm it rather than for the API to come up,
  and the listener is held open across it.
- Steam hardware detection (`FSteamDeviceInfo`), covering Steam Deck and the rest of Valve's hardware
  through `IsRunningOnSteamHardware` and `GetSteamHardwareDefaultConfig`.
- Steam forms of several operations, carrying choices the engine parameters have no room for: avatar
  size, overlay page, and whether a persona query wants avatars along with names.
- Avatar caching to disk in all three sizes Steam keeps.
- `ShowSteamScreenKeyboard` and `DismissSteamScreenKeyboard` in `SteamworksCommon`, over
  `ISteamUtils::ShowFloatingGamepadTextInput`. Steam sends the keys as ordinary keyboard input, so the
  widget holding focus reads them as it would from a real keyboard and nothing has to be collected
  afterwards. The keyboard has to be asked for: the engine starts text input once per window and never
  pairs a keyboard with the field that has focus.
- Lobby attributes mapped onto Steam keys through a service attribute pool, with member count, join
  policy, build id and teardown state published by the plugin because Steam does not report them to a
  non-member.
- Removing a member from a lobby, which Steam has no operation for, as a tagged fixed size message on
  the lobby chat channel that the addressee acts on.
- Nothing a peer sends can end the process: replicated identities, tickets, ports out of configuration
  and the size a Steam Cloud file claims to unpack to are all answered with an error rather than a
  check.

### Not yet verified

Nothing below is known to be broken. It is listed because it has never been run end to end against live
Steam with two machines, and until it has, none of it should be taken on trust.

- Joining a lobby and travelling to the host, in both transports.
- A dedicated server hosting over the relay, with the anonymous login completing both before and after
  the net driver is asked to listen.
- A LAN match, where a peer cannot prove who it is and the listener has to accept it anyway.
- Disconnecting cleanly in both directions, and what the other side sees.
- Recovery after a session on the connectionless transport is disrupted.
- The three lobby join policies, and what a lobby created as invitation-only publishes.

### Notes

- `ISessions` is not implemented: on Steam a session is a lobby, and matchmaking goes through
  `ILobbies`.
- `ITitleFile` is not implemented: Steam has no title-file storage.
- The Steamworks SDK is not distributed with the plugin; see the README.

[0.9.0]: https://github.com/PoFigGames/OnlineServicesSteam/releases/tag/v0.9.0
