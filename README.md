# Online Services Steam

An implementation of Unreal Engine's **Online Services (OSSv2)** interface on top of the **Steamworks
SDK**, together with a Steam networking transport for the engine's net driver.

The engine ships an OSSv2 backend for EOS and for Null, and leaves Steam on the older OnlineSubsystem
(OSSv1). This plugin fills that gap: a game written against `UE::Online::IAuth`, `ILobbies`,
`IAchievements` and the rest of the OSSv2 interfaces runs on Steam without knowing that it does, and its
traffic goes through the Steam Datagram Relay instead of raw UDP.

And it runs **in Play In Editor** — lobbies, invitations, travel to the host, session after session,
without restarting the editor. See [Play In Editor](#play-in-editor) for why that is unusual, and for
the one thing it costs.

> **Status: 0.9, pre-release.** Feature complete and built daily against a game in development, but not
> yet proven against live Steam on more than one machine. While the version is below 1.0 the API is not
> stable and nothing is deprecated before it changes. [CHANGELOG.md](CHANGELOG.md) lists what has and
> has not been verified.

---

## Contents

- [Requirements](#requirements)
- [Modules](#modules)
- [What is implemented](#what-is-implemented)
- [Play In Editor](#play-in-editor)
- [Installation](#installation)
- [Steamworks SDK](#steamworks-sdk)
- [Configuration](#configuration)
- [Networking](#networking)
- [Proving who is joining](#proving-who-is-joining)
- [Dedicated servers](#dedicated-servers)
- [Known limitations](#known-limitations)
- [Contributing](#contributing)
- [Licence](#licence)

---

## Requirements

| | |
|---|---|
| Unreal Engine | 5.8 (built and run against 5.8.3) |
| Steamworks SDK | v1.65, downloaded separately — see [Steamworks SDK](#steamworks-sdk) |
| Platforms | Win64, Mac, Linux |
| Engine plugins | `OnlineServices`, `OnlineSubsystemUtils` (both ship with the engine) |
| Steam account | A Steamworks partner account and an App ID |

The mapping from a world to its online services instance comes from `OnlineSubsystemUtils`. In an editor
build a Play In Editor world gets an instance named after its world context; every other world, and every
world outside the editor, gets the unnamed one. The transport asks through this mapping so that it reaches
the same instance the game signed in to, and never creates one of its own.

No other engine version has been built, so treat anything below 5.8 as untried. What the plugin needs from
that module is older than its own history: `GetServices(const UWorld*, EOnlineServices)` is there in the
`5.4.0-release` tag, and `GetServicesInstanceName(const UWorld*)`, which this plugin calls, from
`5.5.0-release` onward — checked against the engine's release tags on 2026-09-16. There is nothing to
configure and no version to special-case.

## Modules

| Module | Loading phase | Role |
|---|---|---|
| `SteamworksCommon` | `EarliestPossible` | Brings the Steamworks APIs up and owns them for the life of the process. Typed interface accessors, the platform config, Steam identities and addresses, Steam hardware detection, the on-screen keyboard. Depends on nothing else in the plugin. |
| `OnlineSocketsSteam` | `PostConfigInit` | `ISocketSubsystem`, `UNetDriver` and `UNetConnection` over `ISteamNetworkingSockets` or `ISteamNetworkingMessages`. |
| `OnlineServicesSteam` | `PostConfigInit` | The OSSv2 services themselves, the Steam callback and call-result dispatcher, and typed wrappers over the Steam interfaces. |

The dependency direction is one way: `OnlineServicesSteam` → `OnlineSocketsSteam` → `SteamworksCommon`.
A project that only wants Steam identities or Steam hardware detection can depend on `SteamworksCommon`
alone.

## What is implemented

| OSSv2 interface | Steam interface behind it | Notes |
|---|---|---|
| `IAuth` | `ISteamUser`, `ISteamGameServer` | Login, auth tickets, `BeginAuthSession` on the host or Web API tickets for a backend. |
| `ILobbies` | `ISteamMatchmaking` | Create, search, join, invite, attributes, member attributes, lobby chat, game server binding. |
| `IPresence` | `ISteamFriends` | Rich presence, presence of friends. |
| `ISocial` | `ISteamFriends` | Friends list, relationships, blocking, the friends overlay. |
| `IUserInfo` | `ISteamFriends`, `ISteamUtils` | Persona data and avatars, cached to disk in all three Steam sizes. |
| `IAchievements` | `ISteamUserStats` | Query, unlock, progress. |
| `IStats` | `ISteamUserStats` | Read and write user stats, including on a game server. |
| `ILeaderboards` | `ISteamUserStats` | Find or create a board, read ranges and around a user, write scores. |
| `IUserFile` | `ISteamRemoteStorage` | Cloud saves. |
| `IPrivileges` | `ISteamUser`, `ISteamApps` | Whether a user may play online, chat, use UGC. |

`ISessions` is deliberately **not** implemented — see [Known limitations](#known-limitations).

Beyond the engine interfaces, several operations have a Steam form that carries choices the engine
parameters have no room for (which avatar size to fetch, which overlay page to open, whether a persona
query wants avatars too). Both forms share one implementation, so the Steam form costs nothing to keep.

## Play In Editor

**This works in PIE.** Sign in, create a lobby, search, invite, join, travel to the host, play, come
back to the editor and do it again — without leaving the editor once. Two editors on two machines find
each other the same way two packaged builds would.

That is not the usual state of affairs for a Steam integration, and the reason it is unusual is worth
stating, because it is also the reason for the one caveat below. The Steamworks API is a **process-wide
singleton**: `SteamAPI_Init` may be called once for the life of a process, and `SteamAPI_Shutdown`
cannot be undone. An integration that ties the API's lifetime to a session — initialising when a session
starts and shutting down when it ends — works exactly once in the editor and then has to be restarted,
which is why so many of them are packaged-build-only.

This plugin does not do that. `SteamworksCommon` owns the API for the whole process: it is brought up
once, in a module that loads at `EarliestPossible`, and it is taken down only as the process exits.
Everything above it — services, lobbies, sockets, the net driver — is created and destroyed freely
around an API that stays up. So a PIE session ending takes down the lobby and the connections, and
leaves Steam itself running, ready for the next one.

> **The caveat.** Because the API is never shut down while the editor lives, neither is your Steam
> session: the Steam client goes on showing you as in-game, and rich presence stays as the last thing
> the plugin published. Both clear when you close the editor. If you need Steam to see you as out of
> the game, close the editor — restarting PIE will not do it.

Two things that follow from the same design, and are worth knowing before you file a bug:

- **One Steam identity per machine, not per process.** A machine runs one Steam client, signed in as
  one account, and every process of your app on it gets that account: two PIE clients, two editors, an
  editor beside a packaged build — all of them are the same user as far as Steam is concerned. So
  anything that needs two accounts — a lobby with two members in it, an invitation, a friend request,
  a client joining a listen server — needs a second machine signed in as somebody else.

  The one exception is a **dedicated server**, which logs on anonymously through
  `SteamGameServer_InitEx` and so has a Steam identity of its own. A dedicated server and a client on
  the same machine really are two identities, which makes a good deal of the host side testable
  locally: hosting over the relay, a client connecting to it, travel, the handshake, and disconnecting.
- **`-CustomConfig` matters.** A plain PIE session uses the engine's default URL protocol and will not
  travel to a Steam host. Launch the editor with the config that sets `[URL] Protocol` to this
  plugin's, the way a packaged build does.

## Installation

1. Clone into your project's `Plugins` directory:

   ```bash
   git clone https://github.com/PoFigGames/OnlineServicesSteam.git Plugins/OnlineServicesSteam
   ```

2. Install the Steamworks SDK — see the next section. **The plugin will not build without it.**

3. Enable the plugin, either in the editor's plugin browser or in your `.uproject`:

   ```json
   {
     "Name": "OnlineServicesSteam",
     "Enabled": true
   }
   ```

4. Configure it — see [Configuration](#configuration).

5. Regenerate project files and build.

The plugin's modules compile with a deliberately strict set of diagnostics — a truncated 64-bit value, a
shadowed member, an enum silently becoming a number, a missing `override`, a switch with no default that
misses an enumerator and more are errors rather than warnings. They are set in
`OnlineServicesSteamDefaults` in `SteamworksCommon.Build.cs`, apply only to this plugin's own modules,
and are explained there — including which of them the build system routes to which compiler, and the one
check that is deliberately left off.

## Steamworks SDK

**The SDK is not part of this repository.** Valve distributes it under the Steamworks SDK Access
Agreement, which does not grant the right to redistribute it publicly, so you have to fetch your own
copy — it is free, and any Steamworks account can download it.

1. Download `steamworks_sdk_165.zip` from
   [partner.steamgames.com/downloads](https://partner.steamgames.com/downloads/list).
2. Unpack it so that the `sdk` folder's contents land here:

   ```
   Source/ThirdParty/SteamworksSDK/Steamv165/
   ├── public/
   │   └── steam/          ← headers
   └── redistributable_bin/
       ├── win64/          ← steam_api64.lib, steam_api64.dll
       ├── osx/            ← libsteam_api.dylib
       └── linux64/        ← libsteam_api.so
   ```

3. Build. If the folder is missing, `SteamworksSDK.build.cs` stops the build with the path it expected,
   rather than failing later with an unresolved symbol.

To move to another SDK version, change `SteamVersionNumber` at the top of
`Source/ThirdParty/SteamworksSDK/SteamworksSDK.build.cs` and unpack into the matching `Steamv<version>`
folder. The version is published to C++ as `STEAM_SDK_VER`, `STEAM_SDK_VER_INT` and `STEAM_SDK_VER_PATH`.

Two definitions describe the SDK to the rest of the build, and they are not interchangeable:

- `WITH_STEAM_SDK` — set by the SDK module itself and visible only to modules that link it. Use it inside
  this plugin.
- `STEAM_SDK_INSTALLED` — published by `SteamworksCommon`. This is what **your game** asks: Steam is part
  of this build, without your module linking the SDK.

```cpp
#if defined(STEAM_SDK_INSTALLED)
    const auto DeviceInfo = PoFigGames::Steam::GetSteamDeviceInfo();
#endif
```

## Configuration

### The minimum

`Config/DefaultEngine.ini` of your project:

```ini
[OnlineServices]
DefaultServices=Steam

[OnlineServices.Steam]
SteamAppId=480

[URL]
Protocol=STEAM

[/Script/Engine.Engine]
!NetDriverDefinitions=ClearArray
+NetDriverDefinitions=(DefName="GameNetDriver",DriverClassName="/Script/OnlineSocketsSteam.NetDriverSteam",DriverClassNameFallback="/Script/OnlineSubsystemUtils.IpNetDriver")
+NetDriverDefinitions=(DefName="BeaconNetDriver",DriverClassName="/Script/OnlineSocketsSteam.NetDriverSteam",DriverClassNameFallback="/Script/OnlineSubsystemUtils.IpNetDriver")
+NetDriverDefinitions=(DefName="DemoNetDriver",DriverClassName="/Script/Engine.DemoNetDriver",DriverClassNameFallback="/Script/Engine.DemoNetDriver")
+IrisNetDriverConfigs=(NetDriverName="NetDriverSteam",bCanUseIris=true)

[/Script/OnlineSocketsSteam.NetDriverSteam]
NetConnectionClassName="/Script/OnlineSocketsSteam.NetConnectionSteam"

[PacketHandlerComponents]
+Components=OnlineSocketsSteam.AuthHandlerSteamFactory

[/Script/Engine.OnlineEngineInterface]
ClassName=/Script/OnlineServicesSteam.OnlineServicesSteamInterface

[/Script/OnlineSubsystemUtils.OnlineEngineInterfaceImpl]
!CompatibleUniqueNetIdTypes=ClearArray
+CompatibleUniqueNetIdTypes=Steam
```

Steam only reports an App ID to a process it launched itself, so a build that was not started through
the client needs a `steam_appid.txt` beside the executable. The plugin writes that file from `SteamAppId`
as the API comes up and removes it immediately afterwards, which is the only window Steam reads it in;
nothing is left behind for a later run to trip over. Shipping and test builds do not write it at all —
they are started through Steam.

### `[OnlineServices.Steam]`

Every field of `FSteamPlatformConfig` (`SteamworksCommon/Public/SteamPlatformConfig.h`) is read from this
section; the header documents each one. The ones worth knowing about:

| Key | Default | Meaning |
|---|---|---|
| `SteamAppId` | `0` | The App ID the APIs come up for. Required. |
| `bUseSteamTransport` | `True` | Whether Steam carries game traffic at all. |
| `bOverrideDefaultSubsystem` | `True` | Whether Steam becomes the socket subsystem the engine reaches for. |
| `Transport` | `Sockets` | `Sockets` (`ISteamNetworkingSockets`) or `Messages` (`ISteamNetworkingMessages`). |
| `bUseRelay` | `True` | Whether a match is spoken over the Steam Datagram Relay or over an address. The same answer for the host and every client. |
| `bUseSymmetricConnect` | `False` | Peers connect without either being the listener. `Sockets` only. |
| `bInitServerOnClient` | `False` | Whether a client also brings the game server API up. A listen server does not need it to host; see [Dedicated servers](#dedicated-servers). |
| `bRelaunchInSteam` | `False` | Whether a game started outside Steam relaunches through the client. Off while developing. |
| `bVACEnabled` | `True` | Whether the game server announces itself as VAC secured. |
| `bAdvertiseServer` | `True` | Whether the game server heartbeats to the master server and appears in the browser. |
| `GameServerQueryPort` | `27015` | Port the master server queries the game server on. |
| `RequestTimeoutSeconds` | `4.0` | How long a call into Steam may stay unanswered before it is failed. |
| `BackendRequestTimeoutSeconds` | `30.0` | The same, for the calls that wait on a Steam backend rather than on the client beside you: verifying a ticket, and a game server's anonymous logon. |
| `ServerName`, `ProductName`, `GameDirectory`, `GameVersion`, `GameDescription` | — | What the server browser shows and how Steam tells one game's servers from another's. |

### `[/Script/OnlineSocketsSteam.NetDriverSteam]`

`UNetDriverSteam` derives from `UNetDriver`, not from `UIpNetDriver`, so none of the numbers the engine
writes under the IP driver's own section reach it. The plugin's own `Config/Engine.ini` therefore ships
the connection timeouts, keep-alive, rates and relevancy values the IP driver uses; without them a
packaged build would come up with every timeout at zero.

That file is a *dynamic* config layer, applied after a project's static ones, so a project overrides
these in `Config/PluginOverrideEngine.ini` rather than in its own `DefaultEngine.ini`.

Two keys are off by default and are yours to set:

| Key | Default | Meaning |
|---|---|---|
| `MaxSecondsInReceive` | `0.0` | Longest one dispatch may spend reading packets. |
| `NbPacketsBetweenReceiveTimeTest` | `0` | How many packets it reads between two checks of that. |

Both have to name a number for there to be a budget at all; left at zero — which is how the engine ships
the same pair on `UIpNetDriver` — the dispatch reads until the sockets are empty.

### `[OnlineServices.Steam.Auth]`

| Key | Default | Meaning |
|---|---|---|
| `bVerifyAuthTicketsOnGameServer` | `True` | The game server verifies tickets itself through `BeginAuthSession`. Set to `False` to issue Web API tickets that only a backend can verify. |
| `WebApiIdentity` | — | Identity a Web API ticket is bound to; your backend has to pass the same string. |

### Lobby attributes

OSSv2 addresses lobby attributes by schema, and Steam addresses them by string key, so the two are
matched up through a pool of service attributes declared in the plugin's `Config/Engine.ini`, section
`[OnlineServices.Steam.Lobbies]`. The pool is deliberately large (100 lobby slots, 100 member slots);
binding is first-fit by type and `MaxSize`, so **order matters** — a game attribute is bound to the first
free service slot that can hold it.

Four keys are written by the plugin rather than by the game, because Steam does not report them to
somebody who has not joined the lobby: `__memberCount`, `__joinPolicy`, `__buildId` and
`__beingDestroyed`. Declare them in your game's schema to read them back like any other attribute:

```ini
[OnlineServices.Lobbies]
+SchemaCategoryAttributeDescriptors=(SchemaId="LobbyBase", CategoryId="Lobby", AttributeIds=("SchemaCompatibilityId", "Name", "Map", "__memberCount", "__joinPolicy", "__buildId", "__beingDestroyed"))
+SchemaAttributeDescriptors=(Id="__memberCount", Type="Int64", Flags=("Private"), MaxSize=100)
+SchemaAttributeDescriptors=(Id="__joinPolicy", Type="Int64", Flags=("Private"), MaxSize=100)
+SchemaAttributeDescriptors=(Id="__buildId", Type="Int64", Flags=("Private"), MaxSize=100)
+SchemaAttributeDescriptors=(Id="__beingDestroyed", Type="Bool", Flags=("Private"))
```

Writing over them from the game has no lasting effect: the host republishes them whenever the lobby
changes.

The address of a game server bound to a lobby (`__gameserverIP`, `__gameserverPort`,
`__gameserverSteamID`) is filled in by Steam itself during `SetLobbyGameServer`. Do not declare those as
attributes and do not write them — the plugin calls `SetLobbyGameServer` and `GetLobbyGameServer`, and
keeps no mirror of the value.

## Networking

`UNetDriverSteam` replaces `UIpNetDriver` and speaks one of two Steam APIs, chosen by `Transport`:

- **`Sockets`** (`ISteamNetworkingSockets`) — listen sockets, connections and poll groups, with a reason
  reported for every connection that fails or closes. Recommended, and what Valve recommends.
- **`Messages`** (`ISteamNetworkingMessages`) — no listen socket and no connection handle; messages are
  addressed to an identity on a channel and sessions are established implicitly. Closer to UDP.

Either way, traffic goes through the Steam Datagram Relay: peers never learn one another's IP address,
and NAT traversal is Valve's problem rather than yours. `NetDriverDefinitions` above keeps
`IpNetDriver` as the fallback, so a build with Steam unavailable still runs on plain sockets.

Travel URLs use the `STEAM` protocol and an identity in place of an address:
`STEAM:<identity>[:<channel>]`, where the identity is a SteamID over the relay or an IP address without
it, and the channel stands in for the port and only means anything to the `Messages` transport.

## Proving who is joining

`AuthHandlerSteamFactory` above adds a step to the connection handshake, after the engine's own stateless
challenge and before the engine has a login to refuse. The joining player asks Steam for a ticket and
offers it; the server puts it to Steam and waits. Nothing reaches the game until Steam has vouched for the
account: a refusal, an error, or no answer at all closes the connection where it stands. Over the relay
the ticket also has to name the identity Steam signed for that connection, and a client that then logs in
as somebody else is closed too.

The account that was proved is on the connection, so a game that wants to hold its own login against it
can ask `UNetConnectionSteam::GetVerifiedAccount()`.

Both ends need the component. A world with no Steam services asks nobody to prove anything — it says so
in the log and lets the connection through — and that only works when neither end has them: a server that
asks and a client that cannot answer wait for each other until the connection times out. "No services"
means the game has not brought them up in that world; the handshake never creates them itself.

## Dedicated servers

A dedicated server brings up the game server API only, and therefore registers every component except
lobbies — Steam matchmaking belongs to the client API.

`bInitServerOnClient=True` brings the game server API up on a client as well. A listen server does not
need it to host: over the relay it listens on the identity of the player running it. What it buys is
hosting *before* that player has signed in — the listener is held open across the login — and verifying
a joining player's ticket through the game server API rather than the client one.

The socket layer announces the server's listen address, and the services layer binds it to the lobby;
a game does not have to do either by hand.

## Known limitations

- **No `ISessions`.** Steam has no session concept separate from lobbies: what a game calls a session is
  a lobby, and matchmaking goes through `ILobbies`. An `ISessions` component would have been a second
  name for the same thing, so it was removed rather than kept as a facade.
- **No `ITitleFile`.** Steam has no title-file storage. Ship such data with the build, or serve it from
  your own backend.
- **Lobby member attributes are read by key.** Steam offers no iterator over a member's attributes
  (`GetLobbyMemberData` takes a key), so the plugin asks for exactly the keys the schema declares. An
  attribute that is not in the schema cannot be read back.
- **Member count and join policy are metadata.** Steam does not report either to a non-member, so the
  host publishes them; they are as fresh as the host's last update.
- **A PIE session does not end your Steam session.** The Steamworks API is a process singleton and is
  only shut down as the process exits, which is what makes repeated PIE sessions work at all; the price
  is that Steam goes on showing you as in-game until the editor closes. See
  [Play In Editor](#play-in-editor).
- **An API that fails to come up is not tried again.** The SDK documents `SteamAPI_Shutdown` as
  something to call at process exit and offers no supported way back up, so a failure is remembered for
  the life of the process: a later request for that API is told it is not running rather than being
  allowed to start a second attempt. Restart the process.
- **Steam Deck detection is telemetry-shaped.** `IsRunningOnSteamDeck` was removed in SDK 1.65;
  `FSteamDeviceInfo` exposes `IsRunningOnSteamHardware` (telemetry) and `GetSteamHardwareDefaultConfig`
  (what you should actually branch on).

## Contributing

Bug reports, questions and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for the
code style and what a good report looks like.

## Licence

The parts of this plugin authored by PoFig Games Studio are released under the
[MIT Licence](LICENSE).

The licence reaches only what is ours to license. The Steamworks SDK is not included here and stays
Valve's; Unreal Engine is licensed separately by Epic Games, and using this plugin requires your own
Engine licence. [NOTICE.md](NOTICE.md) says what this plugin is built against, what each of those
requires of you, and which Engine plugins its structure follows. Read it before you fork this.

Unreal Engine is a trademark of Epic Games, Inc. Steam and Steamworks are trademarks of Valve
Corporation. This project is not affiliated with, endorsed by, or sponsored by either.
