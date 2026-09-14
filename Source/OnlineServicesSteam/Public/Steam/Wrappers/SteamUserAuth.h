// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Steam/SteamCallTraits.h"
#include "Steam/SteamResult.h"
#include "SteamInterfaces.h"


namespace PoFigGames::Steam
{
	/** Largest session ticket Steam hands out; the web API one has its own, larger, limit. */

	/**
	 * @struct FSteamAuthTicketData
	 *
	 * @brief One issued auth ticket: the bytes to send and the handle Steam tracks them under, which is what
	 * cancelling the ticket needs later.
	 */
	struct FSteamAuthTicketData
	{
		HAuthTicket TicketHandle { k_HAuthTicketInvalid };
		TArray<uint8> TicketBytes { };
	};

	namespace Wrappers
	{
		/**
		 * @struct FSteamAuthSessionTicket
		 *
		 * @brief Ticket for a remote host which verifies it itself through BeginAuthSession. Steam hands the bytes
		 * over immediately but validates them with its backend first, so the ticket may only be sent once the
		 * response callback has arrived.
		 */
		struct FSteamAuthSessionTicket
		{
			static constexpr TCHAR Name[] = TEXT("SteamAuthSessionTicket");

			using SteamCallbackMsgType = GetAuthSessionTicketResponse_t;

			struct Params
			{
				/** Filled in by Invoke and handed on by MakeResult: the request owns the ticket it issued. */
				TSharedRef<FSteamAuthTicketData> Ticket { MakeShared<FSteamAuthTicketData>() };
			};

			struct Result
			{
				TSharedPtr<FSteamAuthTicketData> Ticket { nullptr };
			};

			/** Asks Steam for a ticket. The answer is always awaited: an unvalidated ticket is refused by the host. */
			static ESteamInvokeState Invoke(const Params& In, TSteamResultOf<Result>& OutResult)
			{
				auto Interface = GetSteamInterface<ISteamUser>();
				if (Interface == nullptr)
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
					return ESteamInvokeState::Completed;
				}

				In.Ticket->TicketBytes.SetNumUninitialized(MaxAuthSessionTicketSize);

				uint32 TicketSize { 0 };

				// The remote identity is not known where the ticket is issued, so it is bound to none. A host
				// which needs a bound ticket has to be named by whoever asks for it.
				In.Ticket->TicketHandle = Interface->GetAuthSessionTicket(In.Ticket->TicketBytes.GetData(), In.Ticket->TicketBytes.Num(), &TicketSize, nullptr);

				if (In.Ticket->TicketHandle == k_HAuthTicketInvalid)
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::InvalidResults());
					return ESteamInvokeState::Completed;
				}

				In.Ticket->TicketBytes.SetNum(TicketSize);

				return ESteamInvokeState::Pending;
			}

			static bool IsMatch(const Params& In, const SteamCallbackMsgType& Message)
			{
				return In.Ticket->TicketHandle == Message.m_hAuthTicket;
			}

			static TSteamResultOf<Result> MakeResult(const Params& In, const SteamCallbackMsgType& Message)
			{
				if (Message.m_eResult != k_EResultOK)
				{
					return TSteamResultOf<Result>(Online::Errors::FromSteamResult(Message.m_eResult));
				}

				return TSteamResultOf<Result>(Result { .Ticket = In.Ticket });
			}
		};

		/**
		 * @struct FSteamAuthTicketForWebApi
		 *
		 * @brief Ticket a backend verifies through ISteamUserAuth/AuthenticateUserTicket. The bytes arrive with the
		 * response rather than from the call itself.
		 */
		struct FSteamAuthTicketForWebApi
		{
			static constexpr TCHAR Name[] = TEXT("SteamAuthTicketForWebApi");

			using SteamCallbackMsgType = GetTicketForWebApiResponse_t;

			struct Params
			{
				/** Identity the ticket is bound to; the backend has to present the same string. */
				FString Identity { };

				/** Filled in by MakeResult: the request owns the ticket it issued. */
				TSharedRef<FSteamAuthTicketData> Ticket { MakeShared<FSteamAuthTicketData>() };
			};

			struct Result
			{
				TSharedPtr<FSteamAuthTicketData> Ticket { nullptr };
			};

			static ESteamInvokeState Invoke(const Params& In, TSteamResultOf<Result>& OutResult)
			{
				auto Interface = GetSteamInterface<ISteamUser>();
				if (Interface == nullptr)
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
					return ESteamInvokeState::Completed;
				}

				In.Ticket->TicketHandle = Interface->GetAuthTicketForWebApi(In.Identity.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*In.Identity));

				if (In.Ticket->TicketHandle == k_HAuthTicketInvalid)
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::InvalidResults());
					return ESteamInvokeState::Completed;
				}

				return ESteamInvokeState::Pending;
			}

			static bool IsMatch(const Params& In, const SteamCallbackMsgType& Message)
			{
				return In.Ticket->TicketHandle == Message.m_hAuthTicket;
			}

			static TSteamResultOf<Result> MakeResult(const Params& In, const SteamCallbackMsgType& Message)
			{
				if (Message.m_eResult != k_EResultOK)
				{
					return TSteamResultOf<Result>(Online::Errors::FromSteamResult(Message.m_eResult));
				}

				In.Ticket->TicketBytes.Append(Message.m_rgubTicket, Message.m_cubTicket);

				return TSteamResultOf<Result>(Result { .Ticket = In.Ticket });
			}
		};

		/**
		 * @struct FSteamGameServerLogOn
		 *
		 * @brief Waits until Steam confirms the anonymous logon of this process's game server.
		 *
		 * LogOnAnonymous is asked for when the game server API comes up and answers with a callback, so a
		 * login which runs straight afterwards finds BLoggedOn false through no fault of its own. Only the
		 * confirmation is listened for: a refusal arrives as SteamServerConnectFailure_t, which is a
		 * different message, so a server Steam will not take becomes the request timing out.
		 */
		struct FSteamGameServerLogOn
		{
			static constexpr TCHAR Name[] = TEXT("SteamGameServerLogOn");

			using SteamCallbackMsgType = SteamServersConnected_t;

			struct Params
			{
			};

			struct Result
			{
				CSteamID ServerUserId { k_steamIDNil };
			};

			static ESteamInvokeState Invoke(const Params& /*In*/, TSteamResultOf<Result>& OutResult)
			{
				ISteamGameServer* GameServer = GetSteamInterface<ISteamGameServer>();
				if (GameServer == nullptr)
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
					return ESteamInvokeState::Completed;
				}

				if (GameServer->BLoggedOn())
				{
					OutResult = TSteamResultOf<Result>(Result { .ServerUserId = GameServer->GetSteamID() });
					return ESteamInvokeState::Completed;
				}

				return ESteamInvokeState::Pending;
			}

			/** The message carries nothing to tell one server from another, and a process has one. */
			static bool IsMatch(const Params& /*In*/, const SteamCallbackMsgType& /*Message*/)
			{
				return true;
			}

			static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& /*Message*/)
			{
				ISteamGameServer* GameServer = GetSteamInterface<ISteamGameServer>();
				if (GameServer == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				return TSteamResultOf<Result>(Result { .ServerUserId = GameServer->GetSteamID() });
			}
		};

		/**
		 * @struct FSteamEndAuthSession
		 *
		 * @brief Stops tracking a user whose ticket was verified.
		 */
		struct FSteamEndAuthSession
		{
			static constexpr TCHAR Name[] = TEXT("SteamEndAuthSession");

			struct Params
			{
				CSteamID RemoteUserId { k_steamIDNil };
			};

			struct Result
			{
			};

			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				if (ISteamGameServer* GameServer = GetSteamInterface<ISteamGameServer>())
				{
					GameServer->EndAuthSession(In.RemoteUserId);
					return TSteamResultOf<Result>(Result { });
				}

				if (ISteamUser* User = GetSteamInterface<ISteamUser>())
				{
					User->EndAuthSession(In.RemoteUserId);
					return TSteamResultOf<Result>(Result { });
				}

				return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
			}
		};

		/**
		 * @struct FSteamBeginAuthSession
		 *
		 * @brief Verifies a ticket presented by a remote user. The call itself only accepts the ticket for checking;
		 * whether the user may play arrives on the response callback.
		 */
		struct FSteamBeginAuthSession
		{
			static constexpr TCHAR Name[] = TEXT("SteamBeginAuthSession");

			using SteamCallbackMsgType = ValidateAuthTicketResponse_t;

			struct Params
			{
				/** The user the ticket claims to belong to. */
				CSteamID RemoteUserId { k_steamIDNil };

				TArray<uint8> TicketBytes { };
			};

			struct Result
			{
				CSteamID RemoteUserId { k_steamIDNil };

				/** Set when the ticket belongs to an account borrowing the game through family sharing. */
				CSteamID OwnerUserId { k_steamIDNil };
			};

			/** A dedicated server verifies through the game server API; a listen server through the client one. */
			static ESteamInvokeState Invoke(const Params& In, TSteamResultOf<Result>& OutResult)
			{
				if (!In.RemoteUserId.IsValid() || In.TicketBytes.IsEmpty())
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::InvalidParams());
					return ESteamInvokeState::Completed;
				}

				EBeginAuthSessionResult BeginResult { k_EBeginAuthSessionResultInvalidTicket };

				if (ISteamGameServer* GameServer = GetSteamInterface<ISteamGameServer>())
				{
					BeginResult = GameServer->BeginAuthSession(In.TicketBytes.GetData(), In.TicketBytes.Num(), In.RemoteUserId);
				}
				else if (ISteamUser* User = GetSteamInterface<ISteamUser>())
				{
					BeginResult = User->BeginAuthSession(In.TicketBytes.GetData(), In.TicketBytes.Num(), In.RemoteUserId);
				}
				else
				{
					OutResult = TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
					return ESteamInvokeState::Completed;
				}

				if (BeginResult != k_EBeginAuthSessionResultOK)
				{
					OutResult = TSteamResultOf<Result>(TranslateBeginAuthSessionResult(BeginResult));
					return ESteamInvokeState::Completed;
				}

				return ESteamInvokeState::Pending;
			}

			static bool IsMatch(const Params& In, const SteamCallbackMsgType& Message)
			{
				return In.RemoteUserId == Message.m_SteamID;
			}

			static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
			{
				if (Message.m_eAuthSessionResponse != k_EAuthSessionResponseOK)
				{
					// Reaching here means BeginAuthSession answered OK and Steam opened a session before it
					// looked at the ticket, so a refusal still leaves that session standing. Steam keeps it
					// until it is told otherwise and answers the next BeginAuthSession for the same user with
					// DuplicateRequest, which would lock that player out of this server for as long as the
					// process lives. The refusal closes what the request opened.
					FSteamEndAuthSession::Invoke({ .RemoteUserId = Message.m_SteamID });

					return TSteamResultOf<Result>(TranslateAuthSessionResponse(Message.m_eAuthSessionResponse));
				}

				return TSteamResultOf<Result>(Result { .RemoteUserId = Message.m_SteamID, .OwnerUserId = Message.m_OwnerSteamID });
			}

		private:
			/** Why Steam refused to even start checking the ticket. */
			static UE::Online::FOnlineError TranslateBeginAuthSessionResult(const EBeginAuthSessionResult BeginResult)
			{
				switch (BeginResult)
				{
					case k_EBeginAuthSessionResultInvalidTicket:
					case k_EBeginAuthSessionResultInvalidVersion:
						return UE::Online::Errors::InvalidCreds();
					case k_EBeginAuthSessionResultDuplicateRequest:
						return UE::Online::Errors::AlreadyPending();
					case k_EBeginAuthSessionResultExpiredTicket:
						return UE::Online::Errors::InvalidCreds();
					case k_EBeginAuthSessionResultGameMismatch:
						return UE::Online::Errors::IncompatibleVersion();
					default:
						return UE::Online::Errors::Unknown();
				}
			}

		public:
			/**
			 * Why the user may not play, once Steam had a look at the ticket.
			 *
			 * Reachable from outside, because a verdict can arrive long after the request which asked for it
			 * has gone: Steam sends another one whenever the answer changes.
			 */
			static UE::Online::FOnlineError TranslateAuthSessionResponse(const EAuthSessionResponse AuthSessionResponse)
			{
				switch (AuthSessionResponse)
				{
					case k_EAuthSessionResponseUserNotConnectedToSteam:
						return UE::Online::Errors::NoConnection();
					case k_EAuthSessionResponseNoLicenseOrExpired:
					case k_EAuthSessionResponseVACBanned:
					case k_EAuthSessionResponsePublisherIssuedBan:
					case k_EAuthSessionResponseVACCheckTimedOut:
						return UE::Online::Errors::AccessDenied();
					case k_EAuthSessionResponseLoggedInElseWhere:
					case k_EAuthSessionResponseAuthTicketCanceled:
						return UE::Online::Errors::Cancelled();
					case k_EAuthSessionResponseAuthTicketInvalidAlreadyUsed:
					case k_EAuthSessionResponseAuthTicketInvalid:
						return UE::Online::Errors::InvalidCreds();
					default:
						return UE::Online::Errors::Unknown();
				}
			}
		};

		/**
		 * @struct FSteamCancelAuthTicket
		 *
		 * @brief Withdraws a ticket this process issued, which makes every host holding it drop the session.
		 */
		struct FSteamCancelAuthTicket
		{
			static constexpr TCHAR Name[] = TEXT("SteamCancelAuthTicket");

			struct Params
			{
				HAuthTicket TicketHandle { k_HAuthTicketInvalid };
			};

			struct Result
			{
			};

			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamUser>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				Interface->CancelAuthTicket(In.TicketHandle);

				return TSteamResultOf<Result>(Result { });
			}
		};

		static_assert(CSteamCallbackOp<FSteamAuthSessionTicket>);
		static_assert(CSteamCallbackOp<FSteamAuthTicketForWebApi>);
		static_assert(CSteamCallbackOp<FSteamBeginAuthSession>);
		static_assert(CSteamSyncOp<FSteamEndAuthSession>);
		static_assert(CSteamSyncOp<FSteamCancelAuthTicket>);
	}
}
