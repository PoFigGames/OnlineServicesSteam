// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"


namespace PoFigGames::Steam
{
	/**
	 * @enum ESteamHardware
	 *
	 * @brief Steam hardware the game is running on, as Steam reports it.
	 *
	 * Meant for telemetry, support and diagnostics. Valve asks that a game not decide what it does from
	 * this: a build which turns features on for the devices it knows by name behaves wrongly on the ones
	 * released after it, which report a kind this build has never heard of. Decisions belong to the
	 * questions which describe the situation instead: the suggested configuration below, whether the game
	 * runs in Big Picture, in VR, or under Proton.
	 */
	enum class ESteamHardware : uint8
	{
		/** Steam answered, and this is not one of its devices. */
		None,
		SteamDeck,
		SteamMachine,
		SteamFrame,

		/** Steam could not be asked, or named a device this build predates. */
		Unknown
	};

	/**
	 * @enum ESteamDefaultConfig
	 *
	 * @brief Which of its tuned settings profiles Steam suggests the game start on for this device.
	 *
	 * This is the question to decide from. Steam answers with a device when the game has settings tuned
	 * for that device, and otherwise with one of the four general steps, which the game maps onto its own
	 * presets; a game with fewer presets maps several steps onto the same one. The answer for a device can
	 * be changed on the partner site afterwards, so a game released before a device existed can still be
	 * told what to do on it without being rebuilt.
	 */
	enum class ESteamDefaultConfig : uint8
	{
		/** Steam suggests nothing, so the usual heuristics of the game decide. */
		None,

		Low,
		Medium,
		High,
		Max,

		SteamDeck,
		SteamMachine,
		SteamFrame,

		/** Steam could not be asked, or named a profile this build predates. */
		Unknown
	};

	STEAMWORKSCOMMON_API const TCHAR* LexToString(ESteamHardware Hardware);
	STEAMWORKSCOMMON_API const TCHAR* LexToString(ESteamDefaultConfig DefaultConfig);

	/**
	 * @struct FSteamDeviceInfo
	 *
	 * @brief Everything Steam says about the device the game is running on.
	 *
	 * Read whenever it is asked for rather than kept: a user can move the game between Big Picture and the
	 * desktop, plug the device in or unplug it, while it runs.
	 */
	struct FSteamDeviceInfo
	{
		/** What the device is. For telemetry; see the note on ESteamHardware before deciding from it. */
		ESteamHardware Hardware { ESteamHardware::Unknown };

		/** What Steam suggests the game start on here. */
		ESteamDefaultConfig SuggestedConfig { ESteamDefaultConfig::Unknown };

		/** Steam and its overlay are in Big Picture, so the user is most likely holding a gamepad. */
		bool bBigPictureMode { false };

		/** Steam itself is running in VR. */
		bool bSteamRunningInVR { false };

		/** The game is running on Linux through the Proton compatibility layer. */
		bool bRunningUnderProton { false };

		/** Charge left, in percent, or nothing while the device is on mains power or has no battery. */
		TOptional<uint8> BatteryPercent { };
	};

	/**
	 * What Steam says about the device right now.
	 *
	 * Answers with everything unknown while the Steamworks API is not up, which is the case for a build
	 * running outside Steam and for the editor before the services start.
	 */
	STEAMWORKSCOMMON_API FSteamDeviceInfo GetSteamDeviceInfo();
}
