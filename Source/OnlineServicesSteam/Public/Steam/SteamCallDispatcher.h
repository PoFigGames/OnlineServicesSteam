// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Async/Future.h"
#include "OnlineServicesSteamLogChannels.h"
#include "Steam/SteamCallTraits.h"
#include "Steam/SteamResult.h"

THIRD_PARTY_INCLUDES_START
#include "steam/steam_api_common.h"
THIRD_PARTY_INCLUDES_END


namespace PoFigGames::Steam
{
	class FSteamCallDispatcher;

	/**
	 * @enum ESteamCallContext
	 *
	 * @brief Which Steam pipe broadcast callbacks have to be registered on.
	 */
	enum class ESteamCallContext : uint8
	{
		Client,
		GameServer
	};

	/**
	 * @struct FSteamCallConfig
	 *
	 * @brief Loaded from the [OnlineServices] and [OnlineServices.Steam] config sections.
	 */
	struct FSteamCallConfig
	{
		/** How long a request may stay in flight before it is failed with Errors::Timeout(). */
		double RequestTimeoutSeconds { 4.0 };

		/**
		 * The same, for a request waiting on a Steam backend rather than on the client beside us.
		 *
		 * Valve gives no bound for these at all; this one exists so that a request cannot be pending for
		 * the life of the process, not because an answer is owed by then.
		 */
		double BackendRequestTimeoutSeconds { 30.0 };
	};

	/**
	 * @class FSteamPendingRequest
	 *
	 * @brief Base for a Steam request which is waiting for its answer.
	 *
	 * The dispatcher owns every pending request so that it can be timed out or cancelled, and the request
	 * guarantees that its promise is fulfilled exactly once no matter which of those paths completes it.
	 */
	class FSteamPendingRequest : public TSharedFromThis<FSteamPendingRequest>
	{
	public:
		virtual ~FSteamPendingRequest() = default;

		/** Completes the request with an error. Does nothing when the request has already completed. */
		virtual void FulfilWithError(UE::Online::FOnlineError&& Error) = 0;

		bool IsPending() const { return !bFulfilled; }
		bool HasExpired(const double NowSeconds) const { return !bFulfilled && NowSeconds >= DeadlineSeconds; }

		const TCHAR* GetOpName() const { return OpName; }

		/** How long this request was given, which differs between a local call and a backend round trip. */
		double GetTimeoutSeconds() const { return TimeoutSeconds; }

	protected:
		ONLINESERVICESSTEAM_API FSteamPendingRequest(FSteamCallDispatcher& InDispatcher, const TCHAR* InOpName, double InTimeoutSeconds);

		/** Hands the request back to the dispatcher. Safe to call from inside a Steam callback. */
		ONLINESERVICESSTEAM_API void Release();

		FSteamCallDispatcher* Dispatcher { nullptr };
		const TCHAR* OpName { nullptr };

		double TimeoutSeconds { 0.0 };
		double DeadlineSeconds { 0.0 };
		bool   bFulfilled { false };
	};

	/**
	 * @class FSteamCallDispatcher
	 *
	 * @brief Owns every in-flight Steam request of one online services instance.
	 *
	 * Guarantees that the future returned by a call is fulfilled exactly once: on the Steam callback, on
	 * timeout, or on cancellation when the service shuts down. Steam callbacks are dispatched from the game
	 * thread, so the dispatcher is game thread only by design.
	 */
	class FSteamCallDispatcher final
	{
	public:
		ONLINESERVICESSTEAM_API explicit FSteamCallDispatcher(ESteamCallContext InContext);
		ONLINESERVICESSTEAM_API ~FSteamCallDispatcher();

		FSteamCallDispatcher(const FSteamCallDispatcher&) = delete;
		FSteamCallDispatcher& operator=(const FSteamCallDispatcher&) = delete;

		/** Issues a Steam API call which answers through CCallResult. */
		template<typename OpType> requires CSteamCallResultOp<OpType>
		TFuture<TSteamResult<OpType>> Call(OpType::Params&& Params);

		/** Issues a request whose answer arrives on a broadcast callback, filtered by OpType::IsMatch. */
		template<typename OpType> requires CSteamCallbackOp<OpType>
		TFuture<TSteamResult<OpType>> Listen(OpType::Params&& Params);

		/** Runs a synchronous Steam API call. */
		template<typename OpType> requires CSteamSyncOp<OpType>
		TSteamResult<OpType> CallSync(const OpType::Params& Params) const
		{
			check(IsInGameThread());
			return OpType::Invoke(Params);
		}

		/** Fails every request whose deadline has passed. Called once per frame before Steam callbacks are pumped. */
		ONLINESERVICESSTEAM_API void Tick(double NowSeconds);

		/** Fails every pending request. Called before the Steam API is shut down. */
		ONLINESERVICESSTEAM_API void CancelAll(const UE::Online::FOnlineError& Reason);

		void SetConfig(const FSteamCallConfig& InConfig) { Config = InConfig; }
		const FSteamCallConfig& GetConfig() const { return Config; }

		ESteamCallContext GetContext() const { return Context; }
		int32 GetNumPendingRequests() const { return PendingRequests.Num(); }

	private:
		friend class FSteamPendingRequest;

		template<typename OpType, bool bGameServer> requires CSteamCallbackOp<OpType>
		TFuture<TSteamResult<OpType>> StartCallbackRequest(OpType::Params&& Params);

		ONLINESERVICESSTEAM_API void AddPendingRequest(const TSharedRef<FSteamPendingRequest>& Request);
		ONLINESERVICESSTEAM_API void RemovePendingRequest(const FSteamPendingRequest& Request);

		/**
		 * Which of Steam's two callback pipes an operation's answer will arrive on.
		 *
		 * The operation decides where it says so, because only it knows which API it called; everything
		 * else is answered on the pipe this instance was built for.
		 */
		template<typename OpType>
		bool UsesGameServerPipe() const
		{
			if constexpr (CSteamPipedOp<OpType>)
			{
				return OpType::UsesGameServerPipe();
			}
			else
			{
				return Context == ESteamCallContext::GameServer;
			}
		}

		template<typename OpType>
		double MakeTimeout() const
		{
			return CSteamBackendOp<OpType> ? Config.BackendRequestTimeoutSeconds : Config.RequestTimeoutSeconds;
		}

		ESteamCallContext Context { ESteamCallContext::Client };
		FSteamCallConfig  Config { };

		TArray<TSharedRef<FSteamPendingRequest>> PendingRequests { };
	};

	namespace Private
	{
		/**
		 * @class TSteamCallResultRequest
		 *
		 * @brief Pending request for a Steam API which answers through CCallResult.
		 */
		template<typename OpType> requires CSteamCallResultOp<OpType>
		class TSteamCallResultRequest final : public FSteamPendingRequest
		{
		public:
			TSteamCallResultRequest(FSteamCallDispatcher& InDispatcher, OpType::Params&& InParams, const double InTimeoutSeconds)
				: FSteamPendingRequest(InDispatcher, OpType::Name, InTimeoutSeconds)
				, Params(MoveTemp(InParams))
			{
			}

			virtual ~TSteamCallResultRequest() override
			{
				// Last line of defence: a promise must never be destroyed without a value.
				if (!bFulfilled)
				{
					bFulfilled = true;
					Promise.SetValue(TSteamResult<OpType>(UE::Online::Errors::Cancelled()));
				}
			}

			TFuture<TSteamResult<OpType>> GetFuture() { return Promise.GetFuture(); }

			/** Issues the call. Must be called after the request has been handed to the dispatcher. */
			void Start()
			{
				const SteamAPICall_t CallHandle = OpType::Invoke(Params);
				if (CallHandle == k_uAPICallInvalid)
				{
					// Steam did not accept the call, which is a failure rather than an empty success.
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[Steam] %s Failed: the call was not accepted by Steam"), OpType::Name);

					Fulfil(MakeSteamError<OpType>(UE::Online::Errors::MissingInterface()));
					return;
				}

				CallResult.Set(CallHandle, this, &TSteamCallResultRequest::OnCallResult);
			}

			virtual void FulfilWithError(UE::Online::FOnlineError&& Error) override
			{
				CallResult.Cancel();
				Fulfil(TSteamResult<OpType>(MoveTemp(Error)));
			}

		private:
			/** Called by Steam once the call has been answered. */
			void OnCallResult(OpType::SteamCallbackMsgType* Message, const bool bIOFailure)
			{
				if (bIOFailure || Message == nullptr)
				{
					// The payload is not valid when Steam reports an IO failure, so it must not be read.
					UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[Steam] %s Failed: IO failure"), OpType::Name);

					Fulfil(MakeSteamError<OpType>(k_EResultIOFailure));
					return;
				}

				Fulfil(OpType::MakeResult(Params, *Message));
			}

			/** The single completion path of the request. */
			void Fulfil(TSteamResult<OpType>&& Result)
			{
				if (bFulfilled)
				{
					return;
				}

				// Continuations run inside SetValue and may drop the last reference to this request.
				const auto KeepAlive = AsShared();

				bFulfilled = true;
				Release();

				Promise.SetValue(MoveTemp(Result));
			}

			OpType::Params Params { };
			TPromise<TSteamResult<OpType>> Promise { };

			CCallResult<TSteamCallResultRequest, typename OpType::SteamCallbackMsgType> CallResult { };
		};

		/**
		 * @class TSteamCallbackRequest
		 *
		 * @brief Pending request for a Steam API whose answer arrives on a broadcast callback.
		 */
		template<typename OpType, bool bGameServer> requires CSteamCallbackOp<OpType>
		class TSteamCallbackRequest final : public FSteamPendingRequest
		{
		public:
			TSteamCallbackRequest(FSteamCallDispatcher& InDispatcher, OpType::Params&& InParams, const double InTimeoutSeconds)
				: FSteamPendingRequest(InDispatcher, OpType::Name, InTimeoutSeconds)
				, Params(MoveTemp(InParams))
				, Callback(this, &TSteamCallbackRequest::OnSteamCallback)
			{
			}

			virtual ~TSteamCallbackRequest() override
			{
				// Last line of defence: a promise must never be destroyed without a value.
				if (!bFulfilled)
				{
					bFulfilled = true;
					Promise.SetValue(TSteamResult<OpType>(UE::Online::Errors::Cancelled()));
				}
			}

			TFuture<TSteamResult<OpType>> GetFuture() { return Promise.GetFuture(); }

			/** Starts the request. The callback is already registered, so an immediate answer is not lost. */
			void Start()
			{
				TSteamResult<OpType> ImmediateResult { };
				if (OpType::Invoke(Params, ImmediateResult) == ESteamInvokeState::Completed)
				{
					Fulfil(MoveTemp(ImmediateResult));
				}
			}

			virtual void FulfilWithError(UE::Online::FOnlineError&& Error) override
			{
				Fulfil(TSteamResult<OpType>(MoveTemp(Error)));
			}

		private:
			/** Called by Steam for every listener, so the payload has to be filtered first. */
			void OnSteamCallback(OpType::SteamCallbackMsgType* Message)
			{
				if (Message == nullptr || !OpType::IsMatch(Params, *Message))
				{
					return;
				}

				Fulfil(OpType::MakeResult(Params, *Message));
			}

			/** The single completion path of the request. */
			void Fulfil(TSteamResult<OpType>&& Result)
			{
				if (bFulfilled)
				{
					return;
				}

				// Continuations run inside SetValue and may drop the last reference to this request.
				const auto KeepAlive = AsShared();

				bFulfilled = true;
				Callback.Unregister();
				Release();

				Promise.SetValue(MoveTemp(Result));
			}

			OpType::Params Params { };
			TPromise<TSteamResult<OpType>> Promise { };

			CCallback<TSteamCallbackRequest, typename OpType::SteamCallbackMsgType, bGameServer> Callback;
		};
	}

	template<typename OpType> requires CSteamCallResultOp<OpType>
	TFuture<TSteamResult<OpType>> FSteamCallDispatcher::Call(typename OpType::Params&& Params)
	{
		check(IsInGameThread());

		const TSharedRef<Private::TSteamCallResultRequest<OpType>> Request =
			MakeShared<Private::TSteamCallResultRequest<OpType>>(*this, MoveTemp(Params), MakeTimeout<OpType>());

		auto Future = Request->GetFuture();

		AddPendingRequest(Request);
		Request->Start();

		return Future;
	}

	template<typename OpType> requires CSteamCallbackOp<OpType>
	TFuture<TSteamResult<OpType>> FSteamCallDispatcher::Listen(typename OpType::Params&& Params)
	{
		return UsesGameServerPipe<OpType>()
			? StartCallbackRequest<OpType, true>(MoveTemp(Params))
			: StartCallbackRequest<OpType, false>(MoveTemp(Params));
	}

	template<typename OpType, bool bGameServer> requires CSteamCallbackOp<OpType>
	TFuture<TSteamResult<OpType>> FSteamCallDispatcher::StartCallbackRequest(typename OpType::Params&& Params)
	{
		check(IsInGameThread());

		const TSharedRef<Private::TSteamCallbackRequest<OpType, bGameServer>> Request =
			MakeShared<Private::TSteamCallbackRequest<OpType, bGameServer>>(*this, MoveTemp(Params), MakeTimeout<OpType>());

		auto Future = Request->GetFuture();

		AddPendingRequest(Request);
		Request->Start();

		return Future;
	}
}
