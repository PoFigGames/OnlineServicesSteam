// Copyright PoFig Games Studio. All Rights Reserved.

#include "Steam/SteamCallDispatcher.h"

// Project
#include "OnlineServicesSteamLogChannels.h"

// Engine
#include "Online/OnlineErrorDefinitions.h"


namespace PoFigGames::Steam
{
	FSteamPendingRequest::FSteamPendingRequest(FSteamCallDispatcher& InDispatcher, const TCHAR* InOpName, const double InDeadlineSeconds)
		: Dispatcher(&InDispatcher)
		, OpName(InOpName)
		, DeadlineSeconds(InDeadlineSeconds)
	{
	}

	void FSteamPendingRequest::Release()
	{
		if (FSteamCallDispatcher* ReleasingDispatcher = Dispatcher)
		{
			// Cleared first: removing the request from the dispatcher may drop the last reference to it.
			Dispatcher = nullptr;
			ReleasingDispatcher->RemovePendingRequest(*this);
		}
	}

	FSteamCallDispatcher::FSteamCallDispatcher(const ESteamCallContext InContext)
		: Context(InContext)
	{
	}

	FSteamCallDispatcher::~FSteamCallDispatcher()
	{
		CancelAll(UE::Online::Errors::Cancelled());
	}

	void FSteamCallDispatcher::AddPendingRequest(const TSharedRef<FSteamPendingRequest>& Request)
	{
		check(IsInGameThread());

		PendingRequests.Add(Request);
	}

	void FSteamCallDispatcher::RemovePendingRequest(const FSteamPendingRequest& Request)
	{
		PendingRequests.RemoveAllSwap([&Request](const TSharedRef<FSteamPendingRequest>& PendingRequest)
		{
			return &PendingRequest.Get() == &Request;
		});
	}

	void FSteamCallDispatcher::Tick(const double NowSeconds)
	{
		if (PendingRequests.IsEmpty())
		{
			return;
		}

		// Collected first: failing a request runs the operation continuation, which may start new Steam calls.
		TArray<TSharedRef<FSteamPendingRequest>> ExpiredRequests;
		for (const auto& PendingRequest : PendingRequests)
		{
			if (PendingRequest->HasExpired(NowSeconds))
			{
				ExpiredRequests.Emplace(PendingRequest);
			}
		}

		for (const auto& ExpiredRequest : ExpiredRequests)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[Steam] %s Failed: timed out after %.1f seconds"),
				ExpiredRequest->GetOpName(), Config.RequestTimeoutSeconds);

			ExpiredRequest->FulfilWithError(UE::Online::Errors::Timeout());
		}
	}

	void FSteamCallDispatcher::CancelAll(const UE::Online::FOnlineError& Reason)
	{
		// Moved out first: completing a request removes it from the dispatcher.
		auto RequestsToCancel = MoveTemp(PendingRequests);
		PendingRequests.Reset();

		for (const auto& RequestToCancel : RequestsToCancel)
		{
			if (RequestToCancel->IsPending())
			{
				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[Steam] %s Cancelled: %s"),
					RequestToCancel->GetOpName(), *Reason.GetLogString());

				UE::Online::FOnlineError ErrorCopy { Reason };
				RequestToCancel->FulfilWithError(MoveTemp(ErrorCopy));
			}
		}
	}
}
