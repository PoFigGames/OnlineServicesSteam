// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"


namespace PoFigGames::Steam
{
	/**
	 * @enum ESteamKeyboardLayout
	 *
	 * @brief Layout Steam gives the on-screen keyboard, which also decides what its enter key does.
	 */
	enum class ESteamKeyboardLayout : uint8
	{
		/** Enter dismisses the keyboard. */
		SingleLine,

		/** Enter is a newline, and the user closes the keyboard. */
		MultipleLines,

		Email,
		Numeric
	};

	/**
	 * Opens the Steam on-screen keyboard over the game and keeps it clear of the field being typed into.
	 *
	 * Steam sends the keys as ordinary keyboard input, so whichever widget holds focus reads them as it
	 * would from a real keyboard and nothing has to be collected afterwards.
	 *
	 * @param Layout Layout to open the keyboard on.
	 * @param TextFieldRect The field being typed into, in pixels from the origin of the game window.
	 * @return False when Steam refused, which is what a device with no such keyboard answers.
	 */
	STEAMWORKSCOMMON_API bool ShowSteamScreenKeyboard(ESteamKeyboardLayout Layout, const FIntRect& TextFieldRect);

	/** Closes the keyboard. False when Steam refused, which is also the answer when none was open. */
	STEAMWORKSCOMMON_API bool DismissSteamScreenKeyboard();
}
