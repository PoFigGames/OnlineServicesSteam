// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Steam/SteamCallTraits.h"
#include "Steam/SteamResult.h"
#include "SteamInterfaces.h"


namespace PoFigGames::Steam::Wrappers
{
	/**
	 * Whether a stats callback is about the game this process is running as.
	 *
	 * Steam announces stats and achievements to every listener in the process, tagged with the game they
	 * belong to, so a payload for another game is somebody else's answer rather than ours.
	 */
	inline bool IsCurrentGame(const uint64 GameId)
	{
		auto Interface = GetSteamInterface<ISteamUtils>();
		if (Interface == nullptr)
		{
			return false;
		}

		return CGameID(Interface->GetAppID()).ToUint64() == GameId;
	}

	/**
	 * @struct FSteamUserStats
	 *
	 * @brief Downloads a user's stats and achievements. Answered through the Steam CallResult system.
	 */
	struct FSteamUserStats
	{
		static constexpr TCHAR Name[] = TEXT("SteamUserStats");

		using SteamCallbackMsgType = UserStatsReceived_t;

		struct Params
		{
			CSteamID UserId { k_steamIDNil };
		};

		struct Result
		{
			CSteamID UserId { k_steamIDNil };
			uint64 GameId { 0 };
		};

		/** Issues the call and returns the handle used to track this request. */
		static SteamAPICall_t Invoke(const Params& In)
		{
			auto Interface = GetSteamInterface<ISteamUserStats>();
			if (Interface == nullptr)
			{
				return k_uAPICallInvalid;
			}

			return Interface->RequestUserStats(In.UserId);
		}

		/** Converts the response, mapping a failed download onto the native Steam error. */
		static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
		{
			if (Message.m_eResult != k_EResultOK)
			{
				return TSteamResultOf<Result>(Online::Errors::FromSteamResult(Message.m_eResult));
			}

			return TSteamResultOf<Result>(Result { .UserId = Message.m_steamIDUser, .GameId = Message.m_nGameID });
		}
	};

	/**
	 * @struct FSteamStoreStats
	 *
	 * @brief Commits the stats and achievements set locally to the Steam backend.
	 *
	 * Steam answers on a broadcast callback, plus one more callback for every achievement the commit
	 * unlocked.
	 */
	struct FSteamStoreStats
	{
		static constexpr TCHAR Name[] = TEXT("SteamStoreStats");

		using SteamCallbackMsgType = UserStatsStored_t;

		struct Params
		{
		};

		struct Result
		{
		};

		/** Sends everything set since the last commit. */
		static ESteamInvokeState Invoke(const Params& /*In*/, TSteamResultOf<Result>& OutResult)
		{
			auto Interface = GetSteamInterface<ISteamUserStats>();
			if (Interface == nullptr)
			{
				OutResult = TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				return ESteamInvokeState::Completed;
			}

			if (!Interface->StoreStats())
			{
				// Refused before anything was sent, which happens when the stats of the user were never
				// downloaded; there is no callback to wait for in that case.
				OutResult = TSteamResultOf<Result>(UE::Online::Errors::RequestFailure());
				return ESteamInvokeState::Completed;
			}

			return ESteamInvokeState::Pending;
		}

		/**
		 * Converts the response. A rejected commit means Steam sent the stored values back, so the caller
		 * has to re-read them rather than assume what it wrote.
		 */
		static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
		{
			if (Message.m_eResult != k_EResultOK)
			{
				return TSteamResultOf<Result>(Online::Errors::FromSteamResult(Message.m_eResult));
			}

			return TSteamResultOf<Result>(Result { });
		}

		/** Broadcast callbacks are delivered to every listener, so the payload has to be filtered. */
		static bool IsMatch(const Params& /*In*/, const SteamCallbackMsgType& Message)
		{
			return IsCurrentGame(Message.m_nGameID);
		}
	};

	/**
	 * @struct FSteamAchievementIcon
	 *
	 * @brief Fetches the icon of one achievement.
	 *
	 * Steam keeps a single icon per achievement, the one which matches its current state, and answers on a
	 * broadcast callback unless the image is already cached.
	 */
	struct FSteamAchievementIcon
	{
		static constexpr TCHAR Name[] = TEXT("SteamAchievementIcon");

		using SteamCallbackMsgType = UserAchievementIconFetched_t;

		struct Params
		{
			FString AchievementId { };
		};

		struct Result
		{
			FString AchievementId { };

			/** Whether the image is the unlocked or the locked version of the icon. */
			bool bAchieved { false };

			int32 ImageIndex { 0 };

			uint32 ImageWidth { 0 };
			uint32 ImageHeight { 0 };
		};

		/**
		 * Asks Steam for the icon handle.
		 * @return Completed when the image is already cached locally, Pending while Steam fetches it.
		 */
		static ESteamInvokeState Invoke(const Params& In, TSteamResultOf<Result>& OutResult)
		{
			auto Interface = GetSteamInterface<ISteamUserStats>();
			if (Interface == nullptr)
			{
				OutResult = TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				return ESteamInvokeState::Completed;
			}

			if (In.AchievementId.IsEmpty())
			{
				OutResult = TSteamResultOf<Result>(UE::Online::Errors::InvalidParams());
				return ESteamInvokeState::Completed;
			}

			const auto AchievementName = StringCast<ANSICHAR>(*In.AchievementId);

			// Zero means either that the image is on its way or that the achievement has no icon at all,
			// and the two are only told apart by the callback which follows.
			const int32 ImageIndex = Interface->GetAchievementIcon(AchievementName.Get());
			if (ImageIndex == 0)
			{
				return ESteamInvokeState::Pending;
			}

			bool bAchieved { false };
			Interface->GetAchievement(AchievementName.Get(), &bAchieved);

			OutResult = MakeIconResult(In.AchievementId, ImageIndex, bAchieved);
			return ESteamInvokeState::Completed;
		}

		/** Converts the callback payload into the icon result. */
		static TSteamResultOf<Result> MakeResult(const Params& In, const SteamCallbackMsgType& Message)
		{
			if (Message.m_nIconHandle == 0)
			{
				// The achievement has no icon of its own, which is an answer rather than a failure.
				return TSteamResultOf<Result>(UE::Online::Errors::NotFound());
			}

			return MakeIconResult(In.AchievementId, Message.m_nIconHandle, Message.m_bAchieved);
		}

		/** Broadcast callbacks are delivered to every listener, so the payload has to be filtered. */
		static bool IsMatch(const Params& In, const SteamCallbackMsgType& Message)
		{
			return IsCurrentGame(Message.m_nGameID.ToUint64())
				&& In.AchievementId == StringCast<TCHAR>(Message.m_rgchAchievementName).Get();
		}

	private:
		/** Reads the size of an image Steam has already cached. */
		static TSteamResultOf<Result> MakeIconResult(const FString& AchievementId, const int32 ImageIndex, const bool bAchieved)
		{
			auto Interface = GetSteamInterface<ISteamUtils>();
			if (Interface == nullptr)
			{
				return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
			}

			Result Icon { .AchievementId = AchievementId, .bAchieved = bAchieved, .ImageIndex = ImageIndex };
			if (!Interface->GetImageSize(Icon.ImageIndex, &Icon.ImageWidth, &Icon.ImageHeight))
			{
				return TSteamResultOf<Result>(UE::Online::Errors::InvalidResults());
			}

			return TSteamResultOf<Result>(MoveTemp(Icon));
		}
	};

	static_assert(CSteamCallResultOp<FSteamUserStats>);
	static_assert(CSteamCallbackOp<FSteamStoreStats>);
	static_assert(CSteamCallbackOp<FSteamAchievementIcon>);
}
