// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/UserFileSteam.h"

// Project
#include "OnlineServicesSteamLogChannels.h"
#include "SteamInterfaces.h"
#include "Online/OnlineServicesSteam.h"
#include "Steam/SteamCallDispatcher.h"
#include "Steam/SteamResult.h"
#include "Steam/Wrappers/SteamRemoteStorage.h"

// Engine
#include "Misc/Compression.h"
#include "Online/OnlineErrorDefinitions.h"


namespace UE::Online::Meta {
	BEGIN_ONLINE_STRUCT_META(PoFigGames::Online::FUserFileSteamConfig)
		ONLINE_STRUCT_FIELD(PoFigGames::Online::FUserFileSteamConfig, bCompressBeforeUpload)
	END_ONLINE_STRUCT_META()
}

namespace PoFigGames::Online
{
	namespace Private
	{
		/**
		 * Marks a file this component compressed, so that what is read back is decided by the file rather
		 * than by whatever the configuration happens to say at the time.
		 */
		static constexpr uint8 CompressedFileMagic[] { 'R', 'G', 'L', 'Z' };

		/** The uncompressed size travels with the file, because the decompressor has to be told it up front. */
		static constexpr int32 CompressedFileHeaderSize { sizeof(CompressedFileMagic) + sizeof(uint32) };

		/** Deflate cannot grow data by more than this, so a header claiming more than it is not one we wrote. */
		static constexpr int64 MaxDeflateExpansion { 1032 };

		/** Whoever wrote this file compressed it. */
		static bool IsCompressed(const TArray<uint8>& FileContents)
		{
			return FileContents.Num() >= CompressedFileHeaderSize
				&& FMemory::Memcmp(FileContents.GetData(), CompressedFileMagic, sizeof(CompressedFileMagic)) == 0;
		}

		/** Packs the contents, answering with them unchanged when packing them saves nothing. */
		static TArray<uint8> Compress(const TArray<uint8>& FileContents)
		{
			int32 CompressedSize = FCompression::CompressMemoryBound(NAME_Zlib, FileContents.Num());

			TArray<uint8> CompressedContents;
			CompressedContents.SetNumUninitialized(CompressedFileHeaderSize + CompressedSize);

			if (!FCompression::CompressMemory(NAME_Zlib, CompressedContents.GetData() + CompressedFileHeaderSize, CompressedSize,
				FileContents.GetData(), FileContents.Num()))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserFileSteam::WriteFile] Failed to compress, writing the file as it is."));
				return FileContents;
			}

			if (CompressedFileHeaderSize + CompressedSize >= FileContents.Num())
			{
				// Packing it made it bigger, which happens to data which is already packed.
				return FileContents;
			}

			const uint32 UncompressedSize = FileContents.Num();

			FMemory::Memcpy(CompressedContents.GetData(), CompressedFileMagic, sizeof(CompressedFileMagic));
			FMemory::Memcpy(CompressedContents.GetData() + sizeof(CompressedFileMagic), &UncompressedSize, sizeof(UncompressedSize));

			CompressedContents.SetNum(CompressedFileHeaderSize + CompressedSize, EAllowShrinking::No);

			return CompressedContents;
		}

		/** Unpacks what Compress produced; anything else is answered as it is. Unset means the file is not readable. */
		static TOptional<TArray<uint8>> Decompress(const TArray<uint8>& FileContents)
		{
			if (!IsCompressed(FileContents))
			{
				return FileContents;
			}

			uint32 HeaderSize { 0 };
			FMemory::Memcpy(&HeaderSize, FileContents.GetData() + sizeof(CompressedFileMagic), sizeof(HeaderSize));

			// The header is a claim made by the file, and the cloud hands back whatever is on disk, so the size
			// is checked against what deflate can produce before anything is reserved for it.
			const int64 UncompressedSize = HeaderSize;
			const int32 PayloadSize = FileContents.Num() - CompressedFileHeaderSize;
			if (UncompressedSize <= 0 || UncompressedSize > MAX_int32 || UncompressedSize > PayloadSize * MaxDeflateExpansion)
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserFileSteam::ReadFile] The file says it unpacks to %" INT64_FMT " bytes out of %d, which it cannot."),
					UncompressedSize, PayloadSize);
				return { };
			}

			TArray<uint8> UncompressedContents;
			UncompressedContents.SetNumUninitialized(static_cast<int32>(UncompressedSize));

			if (!FCompression::UncompressMemory(NAME_Zlib, UncompressedContents.GetData(), UncompressedContents.Num(),
				FileContents.GetData() + CompressedFileHeaderSize, PayloadSize))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserFileSteam::ReadFile] Failed to decompress the file."));
				return { };
			}

			return UncompressedContents;
		}
	}

	void FUserFileSteam::UpdateConfig()
	{
		Super::UpdateConfig();

		LoadConfig(Config);
	}

	UE::Online::FOnlineError FUserFileSteam::CheckLocalUser(const UE::Online::FAccountId& LocalAccountId, const TCHAR* Context) const
	{
		if (auto LocalUser = ResolveLocalSteamUser(LocalAccountId, Context); LocalUser.IsError())
		{
			return MoveTemp(LocalUser.GetErrorValue());
		}

		const auto RemoteStorage = Steam::GetSteamInterface<ISteamRemoteStorage>();
		if (RemoteStorage == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] Failed: Steam remote storage interface is not available"), Context);
			return UE::Online::Errors::MissingInterface();
		}

		// The cloud is switched off per account and per game, and either switch leaves the files where
		// they are without letting anybody reach them.
		if (!RemoteStorage->IsCloudEnabledForAccount() || !RemoteStorage->IsCloudEnabledForApp())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] Failed: Steam Cloud is turned off for this account or for this game."), Context);
			return UE::Online::Errors::NotConfigured();
		}

		return UE::Online::Errors::Success();
	}

	UE::Online::FOnlineError FUserFileSteam::CheckFilename(const FString& Filename, const TCHAR* Context) const
	{
		if (Filename.IsEmpty())
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[%s] Failed: No file named."), Context);
			return UE::Online::Errors::InvalidParams();
		}

		return UE::Online::Errors::Success();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileEnumerateFiles> FUserFileSteam::EnumerateFiles(UE::Online::FUserFileEnumerateFiles::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FUserFileEnumerateFiles>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUserFileEnumerateFiles>& InAsyncOp)
			{
				const auto& Params = InAsyncOp.GetParams();

				if (auto Error = CheckLocalUser(Params.LocalAccountId, TEXT("FUserFileSteam::EnumerateFiles")); Error != UE::Online::Errors::Success())
				{
					return InAsyncOp.SetError(MoveTemp(Error));
				}

				const auto RemoteStorage = Steam::GetSteamInterface<ISteamRemoteStorage>();

				// The Steam client keeps a copy of the cloud on disk and synchronises it on its own, so
				// what it already holds is the answer and there is nothing to wait for.
				const int32 FileCount = RemoteStorage->GetFileCount();

				TArray<FString> Filenames;
				Filenames.Reserve(FileCount);

				for (int32 FileIndex = 0; FileIndex < FileCount; ++FileIndex)
				{
					int32 FileSizeInBytes { 0 };

					const auto Filename = RemoteStorage->GetFileNameAndSize(FileIndex, &FileSizeInBytes);
					if (Filename == nullptr || *Filename == '\0')
					{
						continue;
					}

					Filenames.Emplace(StringCast<TCHAR>(Filename).Get());
				}

				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FUserFileSteam::EnumerateFiles] Succeeded: User [%s], Files [%d]"),
					*ToLogString(Params.LocalAccountId), Filenames.Num());

				EnumeratedFiles.Emplace(Params.LocalAccountId, MoveTemp(Filenames));

				InAsyncOp.SetResult({ });
			})
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineResult<UE::Online::FUserFileGetEnumeratedFiles> FUserFileSteam::GetEnumeratedFiles(UE::Online::FUserFileGetEnumeratedFiles::Params&& InParams)
	{
		if (auto Error = CheckLocalUser(InParams.LocalAccountId, TEXT("FUserFileSteam::GetEnumeratedFiles")); Error != UE::Online::Errors::Success())
		{
			return UE::Online::TOnlineResult<UE::Online::FUserFileGetEnumeratedFiles>(MoveTemp(Error));
		}

		const auto Filenames = EnumeratedFiles.Find(InParams.LocalAccountId);
		if (Filenames == nullptr)
		{
			UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserFileSteam::GetEnumeratedFiles] Failed: Call EnumerateFiles first."));
			return UE::Online::TOnlineResult<UE::Online::FUserFileGetEnumeratedFiles>(UE::Online::Errors::InvalidState());
		}

		return UE::Online::TOnlineResult<UE::Online::FUserFileGetEnumeratedFiles>({ *Filenames });
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileReadFile> FUserFileSteam::ReadFile(UE::Online::FUserFileReadFile::Params&& InParams)
	{
		const auto Op = GetJoinableOp<UE::Online::FUserFileReadFile>(MoveTemp(InParams));
		if (!Op->IsReady())
		{
			Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUserFileReadFile>& InAsyncOp)
			{
				using FReadResult = Steam::TSteamResult<Steam::Wrappers::FSteamReadUserFile>;

				const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
				{
					InAsyncOp.SetError(MoveTemp(Error));
					return MakeFulfilledPromise<FReadResult>(FReadResult(UE::Online::Errors::Cancelled())).GetFuture();
				};

				const auto& Params = InAsyncOp.GetParams();

				if (auto Error = CheckLocalUser(Params.LocalAccountId, TEXT("FUserFileSteam::ReadFile")); Error != UE::Online::Errors::Success())
				{
					return FailWith(MoveTemp(Error));
				}

				if (auto Error = CheckFilename(Params.Filename, TEXT("FUserFileSteam::ReadFile")); Error != UE::Online::Errors::Success())
				{
					return FailWith(MoveTemp(Error));
				}

				return SteamCall<Steam::Wrappers::FSteamReadUserFile>({ .Filename = Params.Filename });
			})
			.Then(Steam::Unwrap<Steam::Wrappers::FSteamReadUserFile>(TEXT("FUserFileSteam::ReadFile"),
				[this](UE::Online::TOnlineAsyncOp<UE::Online::FUserFileReadFile>& InAsyncOp, Steam::Wrappers::FSteamReadUserFile::Result&& Result)
			{
				// What was written packed is unpacked again; the file says which it is.
				auto UnpackedContents = Private::Decompress(Result.FileContents);
				if (!UnpackedContents.IsSet())
				{
					InAsyncOp.SetError(UE::Online::Errors::InvalidResults());
					return;
				}

				auto FileContents = MakeShared<UE::Online::FUserFileContents>(MoveTemp(UnpackedContents.GetValue()));

				UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FUserFileSteam::ReadFile] Succeeded: File [%s], Bytes [%d]"),
					*InAsyncOp.GetParams().Filename, FileContents->Num());

				InAsyncOp.SetResult({ .FileContents = MoveTemp(FileContents) });
			}))
			.Enqueue(GetSerialQueue());
		}

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileWriteFile> FUserFileSteam::WriteFile(UE::Online::FUserFileWriteFile::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FUserFileWriteFile>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUserFileWriteFile>& InAsyncOp)
		{
			using FWriteResult = Steam::TSteamResult<Steam::Wrappers::FSteamWriteUserFile>;

			const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
			{
				InAsyncOp.SetError(MoveTemp(Error));
				return MakeFulfilledPromise<FWriteResult>(FWriteResult(UE::Online::Errors::Cancelled())).GetFuture();
			};

			const auto& Params = InAsyncOp.GetParams();

			if (auto Error = CheckLocalUser(Params.LocalAccountId, TEXT("FUserFileSteam::WriteFile")); Error != UE::Online::Errors::Success())
			{
				return FailWith(MoveTemp(Error));
			}

			if (auto Error = CheckFilename(Params.Filename, TEXT("FUserFileSteam::WriteFile")); Error != UE::Online::Errors::Success())
			{
				return FailWith(MoveTemp(Error));
			}

			return SteamCall<Steam::Wrappers::FSteamWriteUserFile>({
				.Filename = Params.Filename,
				.FileContents = Config.bCompressBeforeUpload ? Private::Compress(Params.FileContents) : Params.FileContents });
		})
		.Then(Steam::Unwrap<Steam::Wrappers::FSteamWriteUserFile>(TEXT("FUserFileSteam::WriteFile"),
			[this](UE::Online::TOnlineAsyncOp<UE::Online::FUserFileWriteFile>& InAsyncOp, Steam::Wrappers::FSteamWriteUserFile::Result&& /*Result*/)
		{
			const auto& Params = InAsyncOp.GetParams();

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FUserFileSteam::WriteFile] Succeeded: File [%s], Bytes [%d]"),
				*Params.Filename, Params.FileContents.Num());

			// A file which was not there before is now, so a list read earlier would be missing it.
			if (const auto Filenames = EnumeratedFiles.Find(Params.LocalAccountId))
			{
				Filenames->AddUnique(Params.Filename);
			}

			InAsyncOp.SetResult({ });
		}))
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileCopyFile> FUserFileSteam::CopyFile(UE::Online::FUserFileCopyFile::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FUserFileCopyFile>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUserFileCopyFile>& InAsyncOp)
		{
			using FReadResult = Steam::TSteamResult<Steam::Wrappers::FSteamReadUserFile>;

			const auto FailWith = [&InAsyncOp](UE::Online::FOnlineError&& Error)
			{
				InAsyncOp.SetError(MoveTemp(Error));
				return MakeFulfilledPromise<FReadResult>(FReadResult(UE::Online::Errors::Cancelled())).GetFuture();
			};

			const auto& Params = InAsyncOp.GetParams();

			if (auto Error = CheckLocalUser(Params.LocalAccountId, TEXT("FUserFileSteam::CopyFile")); Error != UE::Online::Errors::Success())
			{
				return FailWith(MoveTemp(Error));
			}

			if (auto Error = CheckFilename(Params.SourceFilename, TEXT("FUserFileSteam::CopyFile")); Error != UE::Online::Errors::Success())
			{
				return FailWith(MoveTemp(Error));
			}

			if (auto Error = CheckFilename(Params.TargetFilename, TEXT("FUserFileSteam::CopyFile")); Error != UE::Online::Errors::Success())
			{
				return FailWith(MoveTemp(Error));
			}

			// Steam has no notion of copying a file, so the contents make the trip: they are read out and
			// written back under the other name, exactly as they were stored.
			return SteamCall<Steam::Wrappers::FSteamReadUserFile>({ .Filename = Params.SourceFilename });
		})
		.Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUserFileCopyFile>& InAsyncOp, Steam::TSteamResult<Steam::Wrappers::FSteamReadUserFile>&& ReadResult)
		{
			using FWriteResult = Steam::TSteamResult<Steam::Wrappers::FSteamWriteUserFile>;

			const auto& Params = InAsyncOp.GetParams();

			if (ReadResult.IsError())
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserFileSteam::CopyFile] Failed to read the source. File [%s], Result [%s]"),
					*Params.SourceFilename, *ReadResult.GetErrorValue().GetLogString());

				InAsyncOp.SetError(MoveTemp(ReadResult.GetErrorValue()));
				return MakeFulfilledPromise<FWriteResult>(FWriteResult(UE::Online::Errors::Cancelled())).GetFuture();
			}

			return SteamCall<Steam::Wrappers::FSteamWriteUserFile>({
				.Filename = Params.TargetFilename, .FileContents = MoveTemp(ReadResult.GetOkValue().FileContents) });
		})
		.Then(Steam::Unwrap<Steam::Wrappers::FSteamWriteUserFile>(TEXT("FUserFileSteam::CopyFile"),
			[this](UE::Online::TOnlineAsyncOp<UE::Online::FUserFileCopyFile>& InAsyncOp, Steam::Wrappers::FSteamWriteUserFile::Result&& /*Result*/)
		{
			const auto& Params = InAsyncOp.GetParams();

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FUserFileSteam::CopyFile] Succeeded: Source [%s], Target [%s]"),
				*Params.SourceFilename, *Params.TargetFilename);

			if (const auto Filenames = EnumeratedFiles.Find(Params.LocalAccountId))
			{
				Filenames->AddUnique(Params.TargetFilename);
			}

			InAsyncOp.SetResult({ });
		}))
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}

	UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileDeleteFile> FUserFileSteam::DeleteFile(UE::Online::FUserFileDeleteFile::Params&& InParams)
	{
		const auto Op = GetOp<UE::Online::FUserFileDeleteFile>(MoveTemp(InParams));

		Op->Then([this](UE::Online::TOnlineAsyncOp<UE::Online::FUserFileDeleteFile>& InAsyncOp)
		{
			const auto& Params = InAsyncOp.GetParams();

			if (auto Error = CheckLocalUser(Params.LocalAccountId, TEXT("FUserFileSteam::DeleteFile")); Error != UE::Online::Errors::Success())
			{
				return InAsyncOp.SetError(MoveTemp(Error));
			}

			if (auto Error = CheckFilename(Params.Filename, TEXT("FUserFileSteam::DeleteFile")); Error != UE::Online::Errors::Success())
			{
				return InAsyncOp.SetError(MoveTemp(Error));
			}

			const auto RemoteStorage = Steam::GetSteamInterface<ISteamRemoteStorage>();

			// FileDelete takes the file out of the cloud as well, which is what deleting means here;
			// FileForget, the other one, only stops it being synchronised and is not that.
			if (!RemoteStorage->FileDelete(StringCast<ANSICHAR>(*Params.Filename).Get()))
			{
				UE_LOG(LogOnlineServicesSteam, Warning, TEXT("[FUserFileSteam::DeleteFile] Steam->FileDelete Failed: File [%s]"), *Params.Filename);
				return InAsyncOp.SetError(UE::Online::Errors::NotFound());
			}

			UE_LOG(LogOnlineServicesSteam, Verbose, TEXT("[FUserFileSteam::DeleteFile] Succeeded: File [%s]"), *Params.Filename);

			if (const auto Filenames = EnumeratedFiles.Find(Params.LocalAccountId))
			{
				Filenames->Remove(Params.Filename);
			}

			InAsyncOp.SetResult({ });
		})
		.Enqueue(GetSerialQueue());

		return Op->GetHandle();
	}
}
