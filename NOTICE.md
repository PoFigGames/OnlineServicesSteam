# Notices

## Steamworks SDK

This plugin builds against Valve's Steamworks SDK. **The SDK is not part of this repository** and is not
covered by the plugin's [licence](LICENSE): Valve distributes it under the Steamworks SDK Access
Agreement and the Steam Subscriber Agreement, which do not grant a right to redistribute it. Download
your own copy from <https://partner.steamgames.com/downloads/list> — see the "Steamworks SDK" section of
[README.md](README.md) for where it goes.

Only the build rules under `Source/ThirdParty/SteamworksSDK/` live in this repository. They expect the
SDK to be unpacked beside them, and refuse to build with a clear message when it is not.

- Steam, Steamworks, Steam Deck and the Steam logo are trademarks of Valve Corporation.
- <https://partner.steamgames.com/documentation/sdk_access_agreement>
- <https://store.steampowered.com/subscriber_agreement/>

## Unreal Engine

This plugin is written for Unreal Engine and is of no use without it. Unreal Engine is licensed
separately by Epic Games under the Unreal Engine End User Licence Agreement; **using or redistributing
this plugin requires your own Engine licence.** The Engine itself is not included here.

The plugin is built against Engine headers and implements Engine interfaces — `ISocketSubsystem`,
`FSocket`, `UNetDriver`, `UNetConnection`, `FOnlineServicesCommon` and the Online Services v2 component
interfaces — so the shape of those classes is fixed by the Engine rather than chosen here.

Where a design question had already been answered inside the Engine, the answer was read rather than
reinvented, and it is worth saying plainly what was read and what it did and did not give us.

**The Online Services components took their bearings from `OnlineServicesNull` and
`OnlineServicesEOSGS`.** Null is the Engine's reference implementation of Online Services v2 and shows
what a component is expected to look like; EOSGS shows how a real backend sequences an operation against
an asynchronous platform. Neither is a Steam implementation, and the Epic Online Services are a different
platform from Steam in the way that matters most here: they answer different questions, through different
calls, with a different model of identity, of a lobby and of a session. What was taken is the shape of a
component and the order of the steps in an operation. What does the work is written for Steamworks.

**The transport started from the Engine's `SteamSockets` and `SteamShared` plugins and was rewritten for
Online Services v2.** Those are Online Subsystem v1 code: they answer to `IOnlineSubsystem`, and the
identity, authentication and lobby state they assume come from there. This plugin has no Online Subsystem
v1 underneath it, so the socket subsystem, the net driver, the net connection and the address type were
reworked to take their identities and their configuration from the Online Services v2 interfaces instead,
and a second transport over `ISteamNetworkingMessages` was added beside the original one. The Steamworks
calls in the middle are the same calls, because there is only one Steamworks SDK.

## The plugin icon

`Resources/Icon128.png` and `Resources/Icon32.png` are the studio's own mark and are covered by the
plugin's [licence](LICENSE). No part of Valve's or Epic Games' branding appears in them.

## Affiliation

This project is not affiliated with, endorsed by, or sponsored by Epic Games or Valve Corporation.
