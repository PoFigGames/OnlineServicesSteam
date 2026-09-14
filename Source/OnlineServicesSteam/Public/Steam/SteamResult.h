// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include <type_traits>

#include "OnlineServicesSteamLogChannels.h"
#include "Online/OnlineAsyncOp.h"
#include "Online/OnlineErrorSteam.h"


namespace PoFigGames::Steam
{
	/**
	 * @class TSteamResultOf
	 *
	 * Result of a Steam API wrapper: either the wrapper's own result type or an FOnlineError.
	 * Spelled in terms of the result type so that it can be used inside the wrapper declaration itself.
	 */
	template<typename ResultType>
	using TSteamResultOf = UE::Online::TDefaultErrorResultInternal<ResultType>;

	/**
	 * @class TSteamResult
	 *
	 * Result of a Steam API wrapper, spelled in terms of the wrapper. Every wrapper reports failure through
	 * this type, so the native EResult is never lost on the way to the operation step that decides what to do.
	 */
	template<typename OpType>
	using TSteamResult = TSteamResultOf<typename OpType::Result>;

	/** Builds a failure result from a Steam EResult, preserving the native code in the STEAM error category. */
	template<typename OpType>
	TSteamResult<OpType> MakeSteamError(const EResult SteamResult)
	{
		return TSteamResult<OpType>(Online::Errors::FromSteamResult(SteamResult));
	}

	/** Builds a failure result from an online error. */
	template<typename OpType>
	TSteamResult<OpType> MakeSteamError(UE::Online::FOnlineError&& Error)
	{
		return TSteamResult<OpType>(MoveTemp(Error));
	}

	namespace Private
	{
		/** Extracts the operation type from a continuation of the form void(TOnlineAsyncOp<OpType>&, ValueType). */
		template<typename CallableType>
		struct TSteamContinuationTraits : TSteamContinuationTraits<decltype(&std::remove_reference_t<CallableType>::operator())>
		{
		};

		template<typename ReturnType, typename ObjectType, typename OpType, typename ValueType>
		struct TSteamContinuationTraits<ReturnType (ObjectType::*)(UE::Online::TOnlineAsyncOp<OpType>&, ValueType)>
		{
			using AsyncOpType = OpType;
		};

		template<typename ReturnType, typename ObjectType, typename OpType, typename ValueType>
		struct TSteamContinuationTraits<ReturnType (ObjectType::*)(UE::Online::TOnlineAsyncOp<OpType>&, ValueType) const>
		{
			using AsyncOpType = OpType;
		};

		/**
		 * @struct TSteamUnwrapContinuation
		 *
		 * Operation step which turns a failed Steam call into an operation error and forwards a successful
		 * value on to the wrapped step. Written as a functor rather than a lambda because TOnlineAsyncOp::Then
		 * deduces the step signature from operator().
		 */
		template<typename SteamOpType, typename OpType, typename CallableType>
		struct TSteamUnwrapContinuation
		{
			void operator()(UE::Online::TOnlineAsyncOp<OpType>& InAsyncOp, TSteamResult<SteamOpType>&& InResult) const
			{
				if (InResult.IsError())
				{
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] %s Failed: %s"), Context, SteamOpType::Name, *InResult.GetErrorValue().GetLogString());

					InAsyncOp.SetError(MoveTemp(InResult.GetErrorValue()));
					return;
				}

				Callable(InAsyncOp, MoveTemp(InResult.GetOkValue()));
			}

			const TCHAR* Context { nullptr };
			CallableType Callable;
		};
	}

	/**
	 * Wraps an operation step so that a failed Steam call is logged and turned into an operation error,
	 * while the step itself only ever sees a successful value. Use it for steps whose only reaction to a
	 * failure is to fail the operation; steps which have to undo work on failure should handle the result
	 * themselves.
	 *
	 * @param Context Logged as the source of the failure, e.g. TEXT("FLobbiesSteam::CreateLobby").
	 * @param Callable Step of the form void(TOnlineAsyncOp<OpType>&, SteamOpType::Result&&).
	 */
	template<typename SteamOpType, typename CallableType>
	auto Unwrap(const TCHAR* Context, CallableType&& Callable)
	{
		using FAsyncOpType = typename Private::TSteamContinuationTraits<CallableType>::AsyncOpType;
		return Private::TSteamUnwrapContinuation<SteamOpType, FAsyncOpType, std::decay_t<CallableType>>
		{
			Context,
			Forward<CallableType>(Callable)
		};
	}
}
