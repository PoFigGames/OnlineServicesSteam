// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Steam/SteamCallTraits.h"
#include "Steam/SteamResult.h"
#include "SteamInterfaces.h"


namespace PoFigGames::Steam::Wrappers
{
	/**
	 * @struct FSteamWriteUserFile
	 *
	 * @brief Writes one file into the cloud storage of the local user.
	 *
	 * The synchronous call of the same name blocks the game until the file has been handed over, so the
	 * asynchronous one is used instead: Steam takes a copy of the data and answers when it is done.
	 */
	struct FSteamWriteUserFile
	{
		static constexpr TCHAR Name[] = TEXT("SteamWriteUserFile");

		using SteamCallbackMsgType = RemoteStorageFileWriteAsyncComplete_t;

		struct Params
		{
			FString Filename { };
			TArray<uint8> FileContents { };
		};

		struct Result
		{
		};

		/** Issues the call and returns the handle used to track this request. */
		static SteamAPICall_t Invoke(const Params& In)
		{
			auto Interface = GetSteamInterface<ISteamRemoteStorage>();
			if (Interface == nullptr || In.Filename.IsEmpty())
			{
				return k_uAPICallInvalid;
			}

			// A file larger than what Steam accepts is refused here rather than half written.
			if (In.FileContents.Num() > static_cast<int32>(k_unMaxCloudFileChunkSize))
			{
				return k_uAPICallInvalid;
			}

			return Interface->FileWriteAsync(StringCast<ANSICHAR>(*In.Filename).Get(), In.FileContents.GetData(), In.FileContents.Num());
		}

		/** Converts the response, mapping a refusal onto the native Steam error. */
		static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
		{
			if (Message.m_eResult != k_EResultOK)
			{
				return TSteamResultOf<Result>(Online::Errors::FromSteamResult(Message.m_eResult));
			}

			return TSteamResultOf<Result>(Result { });
		}
	};

	/**
	 * @struct FSteamReadUserFile
	 *
	 * @brief Reads one file out of the cloud storage of the local user.
	 *
	 * Steam hands the bytes over only once the read is acknowledged, and forgets them as soon as they have
	 * been collected, so they are taken as the answer is converted rather than left for later.
	 */
	struct FSteamReadUserFile
	{
		static constexpr TCHAR Name[] = TEXT("SteamReadUserFile");

		using SteamCallbackMsgType = RemoteStorageFileReadAsyncComplete_t;

		struct Params
		{
			FString Filename { };
		};

		struct Result
		{
			TArray<uint8> FileContents { };
		};

		/** Issues the call and returns the handle used to track this request. */
		static SteamAPICall_t Invoke(const Params& In)
		{
			auto Interface = GetSteamInterface<ISteamRemoteStorage>();
			if (Interface == nullptr || In.Filename.IsEmpty())
			{
				return k_uAPICallInvalid;
			}

			const auto Filename = StringCast<ANSICHAR>(*In.Filename);
			if (!Interface->FileExists(Filename.Get()))
			{
				return k_uAPICallInvalid;
			}

			// The whole file is asked for; Steam reads a chunk at a time only for files past its limit.
			return Interface->FileReadAsync(Filename.Get(), 0, Interface->GetFileSize(Filename.Get()));
		}

		/** Collects the bytes Steam has been holding since the read completed. */
		static TSteamResultOf<Result> MakeResult(const Params& /*In*/, const SteamCallbackMsgType& Message)
		{
			if (Message.m_eResult != k_EResultOK)
			{
				return TSteamResultOf<Result>(Online::Errors::FromSteamResult(Message.m_eResult));
			}

			auto Interface = GetSteamInterface<ISteamRemoteStorage>();
			if (Interface == nullptr)
			{
				return TSteamResultOf<Result>(UE::Online::Errors::MissingInterface());
			}

			Result ReadFile { };
			ReadFile.FileContents.SetNumUninitialized(Message.m_cubRead);

			if (!Interface->FileReadAsyncComplete(Message.m_hFileReadAsync, ReadFile.FileContents.GetData(), Message.m_cubRead))
			{
				return TSteamResultOf<Result>(UE::Online::Errors::InvalidResults());
			}

			return TSteamResultOf<Result>(MoveTemp(ReadFile));
		}
	};

	static_assert(CSteamCallResultOp<FSteamWriteUserFile>);
	static_assert(CSteamCallResultOp<FSteamReadUserFile>);
}
