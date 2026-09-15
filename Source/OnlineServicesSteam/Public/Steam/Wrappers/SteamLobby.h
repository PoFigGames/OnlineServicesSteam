// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Steam/SteamCallTraits.h"
#include "Steam/SteamResult.h"
#include "SteamInterfaces.h"
#include "SteamLobbyKeys.h"


namespace PoFigGames::Steam
{
	/**
	 * @struct FSteamLobbyAttributeData
	 *
	 * @brief One Steam lobby attribute as Steam stores it: a key and a value, both plain strings.
	 *
	 * Values are written exactly as the game means them, without any envelope of our own, so that the
	 * Steam matchmaking filters (AddRequestLobbyListStringFilter and AddRequestLobbyListNumericalFilter)
	 * can compare them server side. The type of a value is not carried alongside it: it is looked up in
	 * the lobby schema, which is the only place that knows it anyway.
	 */
	struct FSteamLobbyAttributeData
	{
		/** Name of the lobby attribute; Steam stores at most k_nMaxLobbyKeyLength characters of it. */
		FString Key { };

		/** Value of the lobby attribute; Steam stores at most k_cubChatMetadataMax bytes of it. */
		FString Value { };
	};

	/**
	 * The build of the game this client is running, as Steam knows it.
	 *
	 * Steam answers with zero for a build it did not install itself, which is every build run from the
	 * editor or from a local folder. That makes the value usable as the identity of a build without any
	 * help from the build configuration: a development build never matches a published one, two
	 * published builds of different versions never match each other, and each beta branch has a build
	 * of its own. Lobbies of a build a client cannot play with are therefore never shown to it.
	 *
	 * @return The build id, or zero when the game was not installed by Steam or the client API is absent.
	 */
	inline int32 GetSteamAppBuildId()
	{
		auto AppsInterface = GetSteamInterface<ISteamApps>();

		return AppsInterface != nullptr ? AppsInterface->GetAppBuildId() : 0;
	}

	/**
	 * @enum ESteamLobbyControlCommand
	 *
	 * @brief What a control message sent over the lobby chat channel asks the members to do.
	 */
	enum class ESteamLobbyControlCommand : uint8
	{
		/** The member named by the message is no longer welcome and is expected to leave. */
		Kick = 1
	};

	/**
	 * @struct FSteamLobbyControlMessage
	 *
	 * @brief A message the owner of a lobby sends to its members over the Steam lobby chat channel.
	 *
	 * Steam has no way for the owner of a lobby to remove somebody from it, and no way to write the
	 * metadata of another member either, so the only way to reach one member is to tell everybody and
	 * let the addressee act on it. The tag makes a message of this plugin's own recognisable, because
	 * the channel also carries whatever chat the game itself sends.
	 */
	struct FSteamLobbyControlMessage
	{
		/** Identifies a message written by this plugin rather than an ordinary chat line. */
		static constexpr ANSICHAR MessageTag[4] { 'S', 'T', 'C', 'M' };

		ANSICHAR Tag[4] { MessageTag[0], MessageTag[1], MessageTag[2], MessageTag[3] };

		ESteamLobbyControlCommand Command { ESteamLobbyControlCommand::Kick };

		/** The struct goes out over the wire whole, so the bytes the compiler needs here are named and zeroed. */
		uint8 Reserved[3] { };

		/** The member the command is addressed to. */
		uint64 TargetSteamId { 0 };

		/** Whether a payload read off the chat channel is a control message of this plugin. */
		bool IsValid(const int32 PayloadSize) const
		{
			return PayloadSize == sizeof(FSteamLobbyControlMessage) && FMemory::Memcmp(Tag, MessageTag, sizeof(MessageTag)) == 0;
		}
	};

	static_assert(sizeof(FSteamLobbyControlMessage) == 16, "Every byte sent over the lobby chat channel has to be a named one.");

	namespace Wrappers
	{
		/** Maps the response of a lobby join attempt onto an online error. */
		inline UE::Online::FOnlineError TranslateChatRoomEnterResponse(const EChatRoomEnterResponse EnterResponse)
		{
			switch (EnterResponse)
			{
				case k_EChatRoomEnterResponseSuccess:
					return UE::Online::Errors::Success();
				case k_EChatRoomEnterResponseDoesntExist:
					return UE::Online::Errors::NotFound();
				case k_EChatRoomEnterResponseFull:
					return UE::Online::Errors::SessionFull();
				case k_EChatRoomEnterResponseRatelimitExceeded:
					return UE::Online::Errors::TooManyRequests();
				case k_EChatRoomEnterResponseNotAllowed:
				case k_EChatRoomEnterResponseBanned:
				case k_EChatRoomEnterResponseLimited:
				case k_EChatRoomEnterResponseCommunityBan:
				case k_EChatRoomEnterResponseMemberBlockedYou:
				case k_EChatRoomEnterResponseYouBlockedMember:
					return UE::Online::Errors::AccessDenied();
				case k_EChatRoomEnterResponseClanDisabled:
					return UE::Online::Errors::InvalidState();
				default:
					return UE::Online::Errors::Unknown();
			}
		}

		/**
		 * @struct FSteamCreateLobby
		 *
		 * @brief Creates a matchmaking lobby. Answered through the Steam CallResult system.
		 */
		struct FSteamCreateLobby
		{
			static constexpr TCHAR Name[] = TEXT("SteamCreateLobby");

			using SteamCallbackMsgType = LobbyCreated_t;

			struct Params
			{
				ELobbyType LobbyType { k_ELobbyTypePublic };
				int32 MaxMembers { 3 };
			};

			struct Result
			{
				CSteamID LobbyId { k_steamIDNil };
			};

			/** Issues the call and returns the handle used to track this request. */
			static SteamAPICall_t Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return k_uAPICallInvalid;
				}

				return Interface->CreateLobby(In.LobbyType, In.MaxMembers);
			}

			/** Converts the response, mapping a failed creation onto the native Steam error. */
			static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
			{
				if (Message.m_eResult != k_EResultOK)
				{
					return TSteamResultOf<Result>(Online::Errors::FromSteamResult(Message.m_eResult));
				}

				return TSteamResultOf<Result>(Result { .LobbyId = Message.m_ulSteamIDLobby });
			}
		};

		/**
		 * @struct FSteamJoinLobby
		 *
		 * @brief Joins an existing matchmaking lobby. Answered through the Steam CallResult system.
		 */
		struct FSteamJoinLobby
		{
			static constexpr TCHAR Name[] = TEXT("SteamJoinLobby");

			using SteamCallbackMsgType = LobbyEnter_t;

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
			};

			struct Result
			{
				CSteamID LobbyId { k_steamIDNil };
				uint32 ChatPermissions { 0 };
			};

			/** Issues the call and returns the handle used to track this request. */
			static SteamAPICall_t Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return k_uAPICallInvalid;
				}

				return Interface->JoinLobby(In.LobbyId);
			}

			/** Converts the response, mapping a locked lobby and every refused entry onto an error. */
			static TSteamResultOf<Result> MakeResult(const Params& In, const SteamCallbackMsgType& Message)
			{
				if (Message.m_bLocked)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::SessionJoinDenied());
				}

				const auto EnterResponse = static_cast<EChatRoomEnterResponse>(Message.m_EChatRoomEnterResponse);
				if (EnterResponse != k_EChatRoomEnterResponseSuccess)
				{
					return TSteamResultOf<Result>(TranslateChatRoomEnterResponse(EnterResponse));
				}

				if (const CSteamID JoinedLobbyId { Message.m_ulSteamIDLobby }; JoinedLobbyId != In.LobbyId)
				{
					// Steam answered for a different lobby than the one that was requested.
					return TSteamResultOf<Result>(UE::Online::Errors::InvalidResults());
				}

				return TSteamResultOf<Result>(Result { .LobbyId = Message.m_ulSteamIDLobby, .ChatPermissions = Message.m_rgfChatPermissions });
			}
		};

		/**
		 * @struct FSteamRequestLobbyList
		 *
		 * @brief Requests the list of lobbies matching the filters set on the matchmaking interface.
		 */
		struct FSteamRequestLobbyList
		{
			static constexpr TCHAR Name[] = TEXT("SteamRequestLobbyList");

			using SteamCallbackMsgType = LobbyMatchList_t;

			struct Params
			{
			};

			struct Result
			{
				uint32 LobbyCount { 0 };
			};

			/** Issues the call and returns the handle used to track this request. */
			static SteamAPICall_t Invoke(const Params& /*In*/)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return k_uAPICallInvalid;
				}

				return Interface->RequestLobbyList();
			}

			/** Converts the response. Steam reports an empty list rather than a failure. */
			static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
			{
				return TSteamResultOf<Result>(Result { .LobbyCount = Message.m_nLobbiesMatching });
			}
		};

		/**
		 * @struct FSteamGetLobbySnapshot
		 *
		 * @brief Reads the current metadata and member list of a lobby out of the local Steam cache.
		 */
		struct FSteamGetLobbySnapshot
		{
			static constexpr TCHAR Name[] = TEXT("SteamGetLobbySnapshot");

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
				bool bFetchMembers { true };
			};

			struct Result
			{
				CSteamID OwnerId { k_steamIDNil };
				int32 MaxMembers { 0 };

				TArray<CSteamID> Members { };

				/**
				 * Everything the lobby carries, including the address of its game server: Steam mirrors
				 * what SetLobbyGameServer was given into the metadata under keys of its own, so there is
				 * nothing extra to ask it for.
				 */
				TArray<FSteamLobbyAttributeData> Attributes { };
			};

			/** Collects everything Steam knows about the lobby right now. */
			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				Result Snapshot { };
				Snapshot.OwnerId = Interface->GetLobbyOwner(In.LobbyId);
				Snapshot.MaxMembers = Interface->GetLobbyMemberLimit(In.LobbyId);

				// Steam only lets a member of the lobby look at the member list; for every other lobby the
				// caller has to fall back on the member count the owner published as metadata.
				if (In.bFetchMembers)
				{
					const int32 MemberCount = Interface->GetNumLobbyMembers(In.LobbyId);
					Snapshot.Members.Reserve(MemberCount);

					for (int32 Index = 0; Index < MemberCount; ++Index)
					{
						if (const CSteamID MemberId = Interface->GetLobbyMemberByIndex(In.LobbyId, Index); MemberId.IsValid())
						{
							Snapshot.Members.Emplace(MemberId);
						}
					}
				}

				const int32 AttributeCount = Interface->GetLobbyDataCount(In.LobbyId);
				Snapshot.Attributes.Reserve(AttributeCount);

				ANSICHAR KeyBuffer[k_nMaxLobbyKeyLength + 1];
				TArray<ANSICHAR> ValueBuffer;
				ValueBuffer.SetNumUninitialized(k_cubChatMetadataMax);

				for (int32 Index = 0; Index < AttributeCount; ++Index)
				{
					if (Interface->GetLobbyDataByIndex(In.LobbyId, Index, KeyBuffer, sizeof(KeyBuffer), ValueBuffer.GetData(), ValueBuffer.Num()))
					{
						Snapshot.Attributes.Emplace(FSteamLobbyAttributeData { .Key = UTF8_TO_TCHAR(KeyBuffer), .Value = UTF8_TO_TCHAR(ValueBuffer.GetData()) });
					}
				}

				return TSteamResultOf<Result>(MoveTemp(Snapshot));
			}
		};

		/**
		 * @struct FSteamGetLobbyMemberData
		 *
		 * @brief Reads the requested attributes of one lobby member out of the local Steam cache.
		 */
		struct FSteamGetLobbyMemberData
		{
			static constexpr TCHAR Name[] = TEXT("SteamGetLobbyMemberData");

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
				CSteamID MemberId { k_steamIDNil };

				/** Keys to read from the Steam cache. Steam offers no way to enumerate them. */
				TArray<FString> Keys { };
			};

			struct Result
			{
				CSteamID LobbyId { k_steamIDNil };
				CSteamID MemberId { k_steamIDNil };

				TArray<FSteamLobbyAttributeData> Attributes { };
			};

			/** Reads the attributes for the member. */
			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				Result MemberData { };
				MemberData.LobbyId = In.LobbyId;
				MemberData.MemberId = In.MemberId;
				MemberData.Attributes.Reserve(In.Keys.Num());

				for (const FString& Key : In.Keys)
				{
					// An unset attribute reads back as an empty string, which is not the same as a set one.
					if (const ANSICHAR* Value = Interface->GetLobbyMemberData(In.LobbyId, In.MemberId, TCHAR_TO_UTF8(*Key));
						Value != nullptr && *Value != '\0')
					{
						MemberData.Attributes.Emplace(FSteamLobbyAttributeData { .Key = Key, .Value = UTF8_TO_TCHAR(Value) });
					}
				}

				return TSteamResultOf<Result>(MoveTemp(MemberData));
			}
		};

		/**
		 * @struct FSteamLobbyGameServer
		 *
		 * @brief Address of the game server bound to a lobby.
		 */
		struct FSteamLobbyGameServer
		{
			uint32   ServerIp { 0 };
			uint16   ServerPort { 0 };
			CSteamID ServerId { k_steamIDNil };
		};

		/**
		 * @struct FSteamSetLobbyGameServer
		 *
		 * @brief Binds a game server to a lobby, which only its owner may do.
		 *
		 * Steam mirrors the address into the metadata of the lobby under reserved keys of its own and
		 * tells every member that the lobby now has a server, so there is nothing to publish by hand.
		 */
		struct FSteamSetLobbyGameServer
		{
			static constexpr TCHAR Name[] = TEXT("SteamSetLobbyGameServer");

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
				FSteamLobbyGameServer GameServer { };
			};

			struct Result
			{
			};

			/** Binds the address. Steam answers nothing, so a refusal is only visible on the next read. */
			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				if (!In.LobbyId.IsValid())
				{
					return TSteamResultOf<Result>(UE::Online::Errors::InvalidParams());
				}

				const auto& [ServerIp, ServerPort, ServerId] = In.GameServer;
				Interface->SetLobbyGameServer(In.LobbyId, ServerIp, ServerPort, ServerId);

				return TSteamResultOf<Result>(Result { });
			}
		};

		/**
		 * @struct FSteamGetLobbyGameServer
		 *
		 * @brief Reads back the game server bound to a lobby, if it has one yet.
		 */
		struct FSteamGetLobbyGameServer
		{
			static constexpr TCHAR Name[] = TEXT("SteamGetLobbyGameServer");

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
			};

			struct Result
			{
				FSteamLobbyGameServer GameServer { };
			};

			/** Answers NotFound while the lobby has no server, which is the state it starts in. */
			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				Result GameServerResult { };

				auto& [ServerIp, ServerPort, ServerId] = GameServerResult.GameServer;
				if (!Interface->GetLobbyGameServer(In.LobbyId, &ServerIp, &ServerPort, &ServerId))
				{
					return TSteamResultOf<Result>(UE::Online::Errors::NotFound());
				}

				return TSteamResultOf<Result>(MoveTemp(GameServerResult));
			}
		};

		/**
		 * @struct FSteamUpdateLobbyData
		 *
		 * @brief Applies a batch of metadata changes to a lobby.
		 */
		struct FSteamUpdateLobbyData
		{
			static constexpr TCHAR Name[] = TEXT("SteamUpdateLobbyData");

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
				TOptional<ELobbyType> NewLobbyType { };

				TArray<FSteamLobbyAttributeData> Attributes { };
				TArray<FString> RemovedAttributes { };

				TArray<FSteamLobbyAttributeData> MemberAttributes { };
				TArray<FString> RemovedMemberAttributes { };
			};

			struct Result
			{
				CSteamID LobbyId { k_steamIDNil };
			};

			/** Applies every change, stopping at the first one Steam refuses. */
			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				if (In.NewLobbyType.IsSet() && !Interface->SetLobbyType(In.LobbyId, In.NewLobbyType.GetValue()))
				{
					return TSteamResultOf<Result>(UE::Online::Errors::InvalidState());
				}

				for (const auto& Attribute : In.Attributes)
				{
					if (!Interface->SetLobbyData(In.LobbyId, TCHAR_TO_UTF8(*Attribute.Key), TCHAR_TO_UTF8(*Attribute.Value)))
					{
						return TSteamResultOf<Result>(UE::Online::Errors::InvalidState());
					}
				}

				for (const FString& Key : In.RemovedAttributes)
				{
					// Steam answers false for a key that was not there to begin with, which is not a failure.
					Interface->DeleteLobbyData(In.LobbyId, TCHAR_TO_UTF8(*Key));
				}

				for (const auto& Attribute : In.MemberAttributes)
				{
					Interface->SetLobbyMemberData(In.LobbyId, TCHAR_TO_UTF8(*Attribute.Key), TCHAR_TO_UTF8(*Attribute.Value));
				}

				for (const FString& Key : In.RemovedMemberAttributes)
				{
					Interface->SetLobbyMemberData(In.LobbyId, TCHAR_TO_UTF8(*Key), "");
				}

				return TSteamResultOf<Result>(Result { .LobbyId = In.LobbyId });
			}
		};

		/**
		 * @struct FSteamSendLobbyControlMessage
		 *
		 * @brief Broadcasts one control message to every member of a lobby.
		 */
		struct FSteamSendLobbyControlMessage
		{
			static constexpr TCHAR Name[] = TEXT("SteamSendLobbyControlMessage");

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
				FSteamLobbyControlMessage Message { };
			};

			struct Result
			{
			};

			/** Sends the message. Steam refuses only when the local user is not in the lobby. */
			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				if (!Interface->SendLobbyChatMsg(In.LobbyId, &In.Message, sizeof(In.Message)))
				{
					return TSteamResultOf<Result>(UE::Online::Errors::InvalidState());
				}

				return TSteamResultOf<Result>(Result { });
			}
		};

		/**
		 * @struct FSteamReadLobbyChatEntry
		 *
		 * @brief Reads one entry off the lobby chat channel, as announced by LobbyChatMsg_t.
		 */
		struct FSteamReadLobbyChatEntry
		{
			static constexpr TCHAR Name[] = TEXT("SteamReadLobbyChatEntry");

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
				int32    ChatEntryId { 0 };
			};

			struct Result
			{
				CSteamID SenderId { k_steamIDNil };

				/** Set when the entry is a control message written by this game. */
				TOptional<FSteamLobbyControlMessage> ControlMessage { };
			};

			/** Reads the entry and reports whether it was one of ours. */
			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				Result Entry { };
				FSteamLobbyControlMessage Payload { };
				EChatEntryType EntryType { k_EChatEntryTypeInvalid };

				const int32 PayloadSize = Interface->GetLobbyChatEntry(In.LobbyId, In.ChatEntryId, &Entry.SenderId, &Payload, sizeof(Payload), &EntryType);
				if (PayloadSize <= 0)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::NotFound());
				}

				if (Payload.IsValid(PayloadSize))
				{
					Entry.ControlMessage.Emplace(Payload);
				}

				return TSteamResultOf<Result>(MoveTemp(Entry));
			}
		};

		/**
		 * @struct FSteamLeaveLobby
		 *
		 * @brief Leaves a lobby. Steam answers immediately and does not report a result.
		 */
		struct FSteamLeaveLobby
		{
			static constexpr TCHAR Name[] = TEXT("SteamLeaveLobby");

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
			};

			struct Result
			{
			};

			/** Leaves the lobby. */
			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				Interface->LeaveLobby(In.LobbyId);

				return TSteamResultOf<Result>(Result { });
			}
		};

		/**
		 * @struct FSteamDestroyLobby
		 *
		 * @brief Takes a lobby down.
		 *
		 * Steam has no dedicated destroy call, so the lobby is made private, flagged as being destroyed
		 * for the remaining members and then left.
		 */
		struct FSteamDestroyLobby
		{
			static constexpr TCHAR Name[] = TEXT("SteamDestroyLobby");

			struct Params
			{
				CSteamID LobbyId { k_steamIDNil };
			};

			struct Result
			{
			};

			/** Flags the lobby as being destroyed and leaves it. */
			static TSteamResultOf<Result> Invoke(const Params& In)
			{
				auto Interface = GetSteamInterface<ISteamMatchmaking>();
				if (Interface == nullptr)
				{
					return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
				}

				Interface->SetLobbyType(In.LobbyId, k_ELobbyTypePrivate);

				// Written the way the schema writes a boolean, otherwise the members still reading the
				// lobby snapshot cannot translate the value back.
				Interface->SetLobbyData(In.LobbyId, TCHAR_TO_UTF8(LobbyKeys::BeingDestroyed), TCHAR_TO_UTF8(LobbyKeys::True));
				Interface->LeaveLobby(In.LobbyId);

				return TSteamResultOf<Result>(Result { });
			}
		};

		static_assert(CSteamCallResultOp<FSteamCreateLobby>);
		static_assert(CSteamCallResultOp<FSteamJoinLobby>);
		static_assert(CSteamCallResultOp<FSteamRequestLobbyList>);
		static_assert(CSteamSyncOp<FSteamGetLobbySnapshot>);
		static_assert(CSteamSyncOp<FSteamGetLobbyMemberData>);
		static_assert(CSteamSyncOp<FSteamSetLobbyGameServer>);
		static_assert(CSteamSyncOp<FSteamGetLobbyGameServer>);
		static_assert(CSteamSyncOp<FSteamUpdateLobbyData>);
		static_assert(CSteamSyncOp<FSteamSendLobbyControlMessage>);
		static_assert(CSteamSyncOp<FSteamReadLobbyChatEntry>);
		static_assert(CSteamSyncOp<FSteamLeaveLobby>);
		static_assert(CSteamSyncOp<FSteamDestroyLobby>);
	}
}
