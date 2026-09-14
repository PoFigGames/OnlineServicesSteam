// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Steam/SteamResult.h"

THIRD_PARTY_INCLUDES_START
#include "steam/steam_api_common.h"
THIRD_PARTY_INCLUDES_END

#include <type_traits>


namespace PoFigGames::Steam
{
	/**
	 * @enum ESteamInvokeState
	 *
	 * Outcome of starting a request whose answer arrives on a broadcast Steam callback.
	 */
	enum class ESteamInvokeState : uint8
	{
		/** The request was sent to Steam; the answer will arrive on the callback. */
		Pending,
		/** The data was already available locally, the out result is filled in and no callback is expected. */
		Completed
	};

	/**
	 * @struct CSteamOp
	 *
	 * Common part of every Steam API wrapper: the parameters it takes, the result it produces and a name
	 * used for logging and diagnostics.
	 */
	template<typename OpType>
	concept CSteamOp = requires
	{
		typename OpType::Params;
		typename OpType::Result;
		{ OpType::Name };
	};

	/**
	 * @struct CSteamCallResultOp
	 *
	 * Wrapper for a Steam API which answers through CCallResult, that is one response per call.
	 * Invoke issues the call and returns its handle; MakeResult converts the response payload, mapping
	 * any failure onto an FOnlineError.
	 */
	template<typename OpType>
	concept CSteamCallResultOp = CSteamOp<OpType>
		&& requires (const typename OpType::Params& Params, const typename OpType::SteamCallbackMsgType& Message)
	{
		typename OpType::SteamCallbackMsgType;
		requires std::is_same_v<decltype(OpType::Invoke(Params)), SteamAPICall_t>;
		requires std::is_same_v<decltype(OpType::MakeResult(Params, Message)), TSteamResult<OpType>>;
	};

	/**
	 * @struct CSteamCallbackOp
	 *
	 * Wrapper for a Steam API whose answer arrives on a broadcast callback shared by every listener.
	 * Invoke starts the request and reports whether an answer is still expected; IsMatch tells the
	 * request's own answer apart from everybody else's.
	 */
	template<typename OpType>
	concept CSteamCallbackOp = CSteamOp<OpType>
		&& requires (const typename OpType::Params& Params, const typename OpType::SteamCallbackMsgType& Message, TSteamResult<OpType>& OutResult)
	{
		typename OpType::SteamCallbackMsgType;
		requires std::is_same_v<decltype(OpType::Invoke(Params, OutResult)), ESteamInvokeState>;
		requires std::is_same_v<decltype(OpType::IsMatch(Params, Message)), bool>;
		requires std::is_same_v<decltype(OpType::MakeResult(Params, Message)), TSteamResult<OpType>>;
	};

	/**
	 * @struct CSteamSyncOp
	 *
	 * Wrapper for a Steam API which answers immediately. Kept in the same shape as the asynchronous ones
	 * so that failures are reported the same way everywhere.
	 */
	template<typename OpType>
	concept CSteamSyncOp = CSteamOp<OpType>
		&& requires (const typename OpType::Params& Params)
	{
		requires std::is_same_v<decltype(OpType::Invoke(Params)), TSteamResult<OpType>>;
	};
}
