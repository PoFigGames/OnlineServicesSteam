// Copyright PoFig Games Studio. All Rights Reserved.

#include "SteamDeviceInfo.h"

// Project
#include "SteamInterfaces.h"


namespace PoFigGames::Steam
{
	namespace Private
	{
		/** Steam answers this when the device is plugged in rather than running off a battery. */
		static constexpr uint8 BatteryOnMainsPower { 255 };

		static ESteamHardware TranslateHardware(const ESteamHardwareType HardwareType)
		{
			switch (HardwareType)
			{
				case k_ESteamHardwareTypeNone:
					return ESteamHardware::None;
				case k_ESteamHardwareTypeSteamDeck:
					return ESteamHardware::SteamDeck;
				case k_ESteamHardwareTypeSteamMachine:
					return ESteamHardware::SteamMachine;
				case k_ESteamHardwareTypeSteamFrame:
					return ESteamHardware::SteamFrame;
				default:
					break;
			}

			// A device released after this build, which is exactly why nothing should be decided from here.
			return ESteamHardware::Unknown;
		}

		static ESteamDefaultConfig TranslateDefaultConfig(const ESteamHardwareDefaultConfig DefaultConfig)
		{
			switch (DefaultConfig)
			{
				case k_ESteamHardwareDefaultConfigNone:
					return ESteamDefaultConfig::None;
				case k_ESteamHardwareDefaultConfigLow:
					return ESteamDefaultConfig::Low;
				case k_ESteamHardwareDefaultConfigMedium:
					return ESteamDefaultConfig::Medium;
				case k_ESteamHardwareDefaultConfigHigh:
					return ESteamDefaultConfig::High;
				case k_ESteamHardwareDefaultConfigMax:
					return ESteamDefaultConfig::Max;
				case k_ESteamHardwareDefaultConfigSteamDeck:
					return ESteamDefaultConfig::SteamDeck;
				case k_ESteamHardwareDefaultConfigSteamMachine:
					return ESteamDefaultConfig::SteamMachine;
				case k_ESteamHardwareDefaultConfigSteamFrame:
					return ESteamDefaultConfig::SteamFrame;
				default:
					break;
			}

			return ESteamDefaultConfig::Unknown;
		}
	}

	const TCHAR* LexToString(const ESteamHardware Hardware)
	{
		switch (Hardware)
		{
			case ESteamHardware::None:
				return TEXT("None");
			case ESteamHardware::SteamDeck:
				return TEXT("SteamDeck");
			case ESteamHardware::SteamMachine:
				return TEXT("SteamMachine");
			case ESteamHardware::SteamFrame:
				return TEXT("SteamFrame");
			case ESteamHardware::Unknown:
				break;
		}

		return TEXT("Unknown");
	}

	const TCHAR* LexToString(const ESteamDefaultConfig DefaultConfig)
	{
		switch (DefaultConfig)
		{
			case ESteamDefaultConfig::None:
				return TEXT("None");
			case ESteamDefaultConfig::Low:
				return TEXT("Low");
			case ESteamDefaultConfig::Medium:
				return TEXT("Medium");
			case ESteamDefaultConfig::High:
				return TEXT("High");
			case ESteamDefaultConfig::Max:
				return TEXT("Max");
			case ESteamDefaultConfig::SteamDeck:
				return TEXT("SteamDeck");
			case ESteamDefaultConfig::SteamMachine:
				return TEXT("SteamMachine");
			case ESteamDefaultConfig::SteamFrame:
				return TEXT("SteamFrame");
			case ESteamDefaultConfig::Unknown:
				break;
		}

		return TEXT("Unknown");
	}

	FSteamDeviceInfo GetSteamDeviceInfo()
	{
		auto Interface = GetSteamInterface<ISteamUtils>();
		if (Interface == nullptr)
		{
			// Nothing is known outside Steam, which is a state of its own rather than a desktop machine.
			return FSteamDeviceInfo { };
		}

		FSteamDeviceInfo DeviceInfo
		{
			.Hardware = Private::TranslateHardware(Interface->IsRunningOnSteamHardware()),
			.SuggestedConfig = Private::TranslateDefaultConfig(Interface->GetSteamHardwareDefaultConfig()),
			.bBigPictureMode = Interface->IsSteamInBigPictureMode(),
			.bSteamRunningInVR = Interface->IsSteamRunningInVR(),
			.bRunningUnderProton = Interface->IsRunningUnderProton()
		};

		if (const uint8 BatteryPower = Interface->GetCurrentBatteryPower(); BatteryPower != Private::BatteryOnMainsPower)
		{
			DeviceInfo.BatteryPercent.Emplace(BatteryPower);
		}

		return DeviceInfo;
	}
}
