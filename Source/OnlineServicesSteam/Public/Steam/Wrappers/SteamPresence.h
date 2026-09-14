// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Steam/SteamCallTraits.h"
#include "Steam/SteamResult.h"
#include "SteamInterfaces.h"


namespace PoFigGames::Steam::Wrappers
{
	/**
	 * @struct FSteamFriendRichPresence
	 *
	 * Asks Steam for the rich presence of a user, which it only keeps for people the local user can see:
	 * friends, and everybody in the same lobby or on the same server.
	 *
	 * Steam answers on a broadcast callback shared by every listener, and says nothing at all about a user
	 * who has published no rich presence, so a caller which needs an answer either way should treat the
	 * request timing out as that answer rather than as a failure.
	 */
	struct FSteamFriendRichPresence
	{
		static constexpr TCHAR Name[] = TEXT("SteamFriendRichPresence");

		using SteamCallbackMsgType = FriendRichPresenceUpdate_t;

		struct Params
		{
			CSteamID UserId { k_steamIDNil };
		};

		struct Result
		{
			CSteamID UserId { k_steamIDNil };
		};

		/** Asks for the rich presence; Steam answers on the callback rather than returning anything. */
		static ESteamInvokeState Invoke(const Params& In, TSteamResultOf<Result>& OutResult)
		{
			auto Interface = GetSteamInterface<ISteamFriends>();
			if (Interface == nullptr)
			{
				OutResult = TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				return ESteamInvokeState::Completed;
			}

			if (!In.UserId.IsValid())
			{
				OutResult = TSteamResultOf<Result>(UE::Online::Errors::InvalidParams());
				return ESteamInvokeState::Completed;
			}

			Interface->RequestFriendRichPresence(In.UserId);
			return ESteamInvokeState::Pending;
		}

		/** Converts the callback payload; the keys themselves are read from the Steam cache afterwards. */
		static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
		{
			return TSteamResultOf<Result>(Result { .UserId = Message.m_steamIDFriend });
		}

		/** Broadcast callbacks are delivered to every listener, so the payload has to be filtered. */
		static bool IsMatch(const Params& In, const SteamCallbackMsgType& Message)
		{
			return Message.m_steamIDFriend == In.UserId;
		}
	};

	static_assert(CSteamCallbackOp<FSteamFriendRichPresence>);
}
