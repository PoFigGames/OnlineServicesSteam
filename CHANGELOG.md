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
  answers for the host and the client alike: neither role reads it off its role, its login state or the
  address it was handed. A LAN match is spoken over IP, from the flag in the URL. Only the relay has been
  taken end to end; see [Not yet verified](#not-yet-verified) for the rest.
- Net driver defaults in the plugin's own config section. The engine writes connection timeouts and
  rates under the IP driver's section, which `UNetDriverSteam` does not inherit, so a driver left to the
  engine's defaults would come up with no timeouts at all. That section is a dynamic config layer,
  applied after a project's own files, so a project overrides it in `Config/PluginOverrideEngine.ini`.
  It also carries `MaxSecondsInReceive` and `NbPacketsBetweenReceiveTimeTest`, the pair `UIpNetDriver`
  has: set both to give the dispatch a budget for reading packets, leave them at zero — the default, and
  the engine's — and it reads until the sockets are empty.
- A received packet finds its connection through the map the engine already keeps for the purpose,
  rather than by searching the connection list. The search cost a scan per packet against the number of
  players in the match, which is what a full server would have spent its dispatch on.
- `SteamworksCommon`: process-wide ownership of the Steamworks APIs, typed interface accessors, the
  platform config, Steam identities and addresses. An API is brought up at most once per process: the
  SDK offers no supported way back up after a shutdown, so one that failed to come up is remembered as
  having failed and a later request is told it is not running rather than starting a second attempt.
- `steam_appid.txt` is written from `SteamAppId` as the API comes up and removed immediately afterwards,
  which is the only window Steam reads it in. Anything that ended the process in between used to leave
  it behind, and the Steam client will not relaunch a process that has one beside it. Shipping and test
  builds do not write it at all.
- Verified auth sessions closed on every path a request can end on: a refusal, a timeout, a cancellation,
  a verdict that changes long after the request that asked for it, and an answer that arrives after the
  connection it was asked for has gone. Steam keeps a session it opened until it is told otherwise and
  answers that player's next attempt with `DuplicateRequest`, so a path that forgets one locks a player
  out until the server restarts.
- A joining player proves who they are during the connection handshake, and nothing reaches the game
  until the services have vouched for them. The ticket is read under a length of its own rather than one
  the packet names, and over the relay it has to name the identity Steam signed for that connection;
  every other answer, an unanswered one included, closes the connection where it stands. The account that
  was proved is on the connection, for the game to hold its own login against.
- A dedicated server's anonymous login waits for Steam to confirm it rather than for the API to come up,
  and the listener is held open across it.
- Privileges answer about this game rather than about the client it runs under. A locked Family View
  permits the games on its list and Steam does not launch the others, so the lock alone restricts
  nobody: the age restriction is reported when Steam says this app is blocked, while ownership, VAC and
  the parental locks on friends and community are each asked about on their own. Checked against
  Steamworks SDK 1.65 on 2026-09-19.
- Steam hardware detection (`FSteamDeviceInfo`), covering Steam Deck and the rest of Valve's hardware
  through `IsRunningOnSteamHardware` and `GetSteamHardwareDefaultConfig`.
- Steam forms of several operations, carrying choices the engine parameters have no room for: avatar
  size, overlay page, and whether a persona query wants avatars along with names.
- Avatar caching to disk in all three sizes Steam keeps.
- Asking for a user's rich presence answers as soon as Steam can answer it. Steam never reports that a
  user published nothing, so a request could only ever end by timing out; keys the client already holds —
  which it does for anybody the local user can see — are the answer and are given straight away. A batch
  asks about everybody at once, so a party of players who published nothing costs one timeout between
  them rather than one each.
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
- The transport never brings the services up. Asking for them through a world creates an instance when
  there is none, so a packet handler asking who this process is signed in as could have created a second
  services instance from inside a connection; it asks by name, and only when that name is already loaded.
- Lengths and identities that arrive from elsewhere are answered with an error rather than a check:
  replicated identities, the two fields of a handshake ticket, ports out of configuration, and the size a
  Steam Cloud file claims to unpack to. Each is bounded before anything is reserved for it.

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
