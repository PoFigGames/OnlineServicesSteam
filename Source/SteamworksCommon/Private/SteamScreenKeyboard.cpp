// Copyright PoFig Games Studio. All Rights Reserved.

#include "SteamScreenKeyboard.h"

// Project
#include "SteamInterfaces.h"
#include "SteamworksCommonLogChannels.h"


namespace PoFigGames::Steam
{
	namespace Private
	{
		static EFloatingGamepadTextInputMode TranslateKeyboardLayout(const ESteamKeyboardLayout Layout)
		{
			switch (Layout)
			{
				case ESteamKeyboardLayout::MultipleLines:
					return k_EFloatingGamepadTextInputModeModeMultipleLines;
				case ESteamKeyboardLayout::Email:
					return k_EFloatingGamepadTextInputModeModeEmail;
				case ESteamKeyboardLayout::Numeric:
					return k_EFloatingGamepadTextInputModeModeNumeric;
				case ESteamKeyboardLayout::SingleLine:
					break;
			}

			return k_EFloatingGamepadTextInputModeModeSingleLine;
		}
	}

	bool ShowSteamScreenKeyboard(const ESteamKeyboardLayout Layout, const FIntRect& TextFieldRect)
	{
		auto Interface = GetSteamInterface<ISteamUtils>();
		if (Interface == nullptr)
		{
			UE_LOG(LogSteamService, Verbose, TEXT("The Steam keyboard was asked for while the Steamworks API is not running."));

			return false;
		}

		const auto bShown = Interface->ShowFloatingGamepadTextInput(Private::TranslateKeyboardLayout(Layout),
			TextFieldRect.Min.X, TextFieldRect.Min.Y, TextFieldRect.Width(), TextFieldRect.Height());

		UE_CLOG(!bShown, LogSteamService, Verbose, TEXT("Steam refused the on-screen keyboard, which is what a device without one answers."));

		return bShown;
	}

	bool DismissSteamScreenKeyboard()
	{
		auto Interface = GetSteamInterface<ISteamUtils>();

		return Interface != nullptr && Interface->DismissFloatingGamepadTextInput();
	}
}
