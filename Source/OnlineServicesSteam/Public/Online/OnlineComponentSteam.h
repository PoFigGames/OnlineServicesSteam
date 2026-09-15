// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/Auth.h"
#include "Online/OnlineIdSteam.h"
#include "Online/OnlineServicesSteam.h"
#include "OnlineServicesSteamLogChannels.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/SteamCallTraits.h"
#include "Steam/SteamResult.h"
#include "SteamInterfaces.h"


namespace PoFigGames::Online
{
	/**
	 * @class TOnlineComponentSteam
	 *
	 * @brief Steam side of an online services component. It hands the component the call dispatcher of its own
	 * services instance, so a step of an operation reads as SteamCall<FSteamCreateLobby>(MoveTemp(Params))
	 * instead of reaching for the services object and its dispatcher first.
	 *
	 * Sits between a component and the F*Common class it implements, which leaves the Super chain the
	 * component registry walks untouched.
	 */
	template<typename ComponentType>
	class TOnlineComponentSteam : public ComponentType
	{
	public:
		explicit TOnlineComponentSteam(UE::Online::FOnlineServicesCommon& InServices)
			: ComponentType(InServices)
		{
		}

	protected:
		/** The Steam services this component belongs to. */
		FOnlineServicesSteam& GetOnlineServicesSteam() const
		{
			return static_cast<FOnlineServicesSteam&>(this->Services);
		}

		/** Owns every in-flight Steam request of this services instance. */
		Steam::FSteamCallDispatcher& GetCallDispatcher() const
		{
			return GetOnlineServicesSteam().GetCallDispatcher();
		}

		/** Issues a Steam API call which answers through CCallResult. */
		template<typename SteamOpType> requires Steam::CSteamCallResultOp<SteamOpType>
		TFuture<Steam::TSteamResult<SteamOpType>> SteamCall(typename SteamOpType::Params&& Params) const
		{
			return GetCallDispatcher().template Call<SteamOpType>(MoveTemp(Params));
		}

		/** Issues a request whose answer arrives on a broadcast callback, filtered by the wrapper. */
		template<typename SteamOpType> requires Steam::CSteamCallbackOp<SteamOpType>
		TFuture<Steam::TSteamResult<SteamOpType>> SteamListen(typename SteamOpType::Params&& Params) const
		{
			return GetCallDispatcher().template Listen<SteamOpType>(MoveTemp(Params));
		}

		/** Runs a synchronous Steam API call. */
		template<typename SteamOpType> requires Steam::CSteamSyncOp<SteamOpType>
		Steam::TSteamResult<SteamOpType> SteamCallSync(const typename SteamOpType::Params& Params) const
		{
			return GetCallDispatcher().template CallSync<SteamOpType>(Params);
		}

		/** The account of the signed in local user, or an invalid id while nobody is signed in. */
		UE::Online::FAccountId GetLocalAccountId() const
		{
			if (const auto SteamUser = Steam::GetSteamInterface<ISteamUser>())
			{
				return FindAccountId(const_cast<ISteamUser*>(SteamUser)->GetSteamID());
			}

			return UE::Online::FAccountId { };
		}

		/**
		 * Answers the questions every operation asks before reaching for Steam: whether there is anybody to
		 * ask on behalf of, and whether the user it is about is somebody Steam has an identity for.
		 *
		 * @param Context Named in the log, e.g. TEXT("FAchievementsSteam::UnlockAchievements").
		 */
		UE::Online::TDefaultErrorResultInternal<CSteamID> ResolveSteamUser(const UE::Online::FAccountId& LocalAccountId,
			const UE::Online::FAccountId& TargetAccountId, const TCHAR* Context) const
		{
			const auto Auth = this->Services.GetAuthInterface();
			if (!Auth.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] Failed: Authentication Interface is not found"), Context);
				return UE::Online::TDefaultErrorResultInternal<CSteamID>(UE::Online::Errors::MissingInterface());
			}

			if (!Auth->IsLoggedIn(LocalAccountId))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] Failed: User is not logged in. User [%s]"), Context, *ToLogString(LocalAccountId));
				return UE::Online::TDefaultErrorResultInternal<CSteamID>(UE::Online::Errors::NotLoggedIn());
			}

			const CSteamID TargetSteamId = GetSteamUserId(TargetAccountId);
			if (!TargetSteamId.IsValid())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] Failed: No associated steam id found. User [%s]"), Context, *ToLogString(TargetAccountId));
				return UE::Online::TDefaultErrorResultInternal<CSteamID>(UE::Online::Errors::NotFound());
			}

			return UE::Online::TDefaultErrorResultInternal<CSteamID>(TargetSteamId);
		}

		/** The same, for an operation which is about the local user itself. */
		UE::Online::TDefaultErrorResultInternal<CSteamID> ResolveLocalSteamUser(const UE::Online::FAccountId& LocalAccountId, const TCHAR* Context) const
		{
			return ResolveSteamUser(LocalAccountId, LocalAccountId, Context);
		}

		/**
		 * Whether the Steam overlay can show anything at all.
		 *
		 * Everything a game asks the overlay for is swallowed silently when it is switched off, so a caller
		 * which has nothing else to offer the user is told rather than left to wonder.
		 */
		UE::Online::FOnlineError CheckOverlayAvailable(const TCHAR* Context) const
		{
			if (const auto Utils = Steam::GetSteamInterface<ISteamUtils>(); Utils != nullptr && !Utils->IsOverlayEnabled())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] Failed: The Steam overlay is not enabled."), Context);
				return UE::Online::Errors::InvalidState();
			}

			return UE::Online::Errors::Success();
		}
	};
}
