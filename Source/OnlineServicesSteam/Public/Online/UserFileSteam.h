// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineComponentSteam.h"
#include "Online/UserFileCommon.h"


namespace PoFigGames::Online
{
	/**
	 * @struct FUserFileSteamConfig
	 *
	 * @brief Loaded from the [OnlineServices.Steam.UserFile] config section.
	 */
	struct FUserFileSteamConfig
	{
		/**
		 * Whether a file is compressed on its way into the cloud.
		 *
		 * The storage of a user is a shared quota rather than a per file limit, so a game which keeps
		 * large saves there is better off compressing them. What is read back is decided by the file
		 * itself and not by this setting, so turning it on or off leaves the files already written
		 * readable either way.
		 */
		bool bCompressBeforeUpload { false };
	};

	/**
	 * @class FUserFileSteam
	 *
	 * @brief Steam User File Online Component
	 *
	 * The files of a user live in Steam Cloud, which the client keeps a copy of on disk and synchronises
	 * on its own. Listing them is therefore a walk of what the client already has, while reading and
	 * writing are asked of Steam and answered when it is done; the calls which would do the same without
	 * asking block the game until the file has been handed over, which is why they are not used.
	 *
	 * Steam has no notion of copying a file, so a copy is a read followed by a write, and no notion of a
	 * file belonging to anybody but the user running the game: the cloud storage of somebody else is not
	 * reachable from here at all.
	 */
	class FUserFileSteam : public TOnlineComponentSteam<UE::Online::FUserFileCommon>
	{
	public:
		using Super = FUserFileCommon;
		using TOnlineComponentSteam::TOnlineComponentSteam;

	#pragma region TOnlineComponent
		ONLINESERVICESSTEAM_API virtual void UpdateConfig() override;
	#pragma endregion TOnlineComponent

	#pragma region IUserFile
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileEnumerateFiles> EnumerateFiles(UE::Online::FUserFileEnumerateFiles::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineResult<UE::Online::FUserFileGetEnumeratedFiles>    GetEnumeratedFiles(UE::Online::FUserFileGetEnumeratedFiles::Params&& Params) override;

		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileReadFile>   ReadFile(UE::Online::FUserFileReadFile::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileWriteFile>  WriteFile(UE::Online::FUserFileWriteFile::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileCopyFile>   CopyFile(UE::Online::FUserFileCopyFile::Params&& Params) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FUserFileDeleteFile> DeleteFile(UE::Online::FUserFileDeleteFile::Params&& Params) override;
	#pragma endregion IUserFile

	protected:
		FUserFileSteamConfig Config { };

		/** What the last walk of the cloud storage found, by the local user it belongs to. */
		TMap<UE::Online::FAccountId, TArray<FString>> EnumeratedFiles { };

	private:
		/**
		 * Answers the questions every operation asks: is the user signed in, is this the user running the
		 * game, and is the cloud storage available to them at all.
		 */
		ONLINESERVICESSTEAM_API UE::Online::FOnlineError CheckLocalUser(const UE::Online::FAccountId& LocalAccountId, const TCHAR* Context) const;

		/** Rejects a name Steam would refuse, before anything is asked of it. */
		ONLINESERVICESSTEAM_API UE::Online::FOnlineError CheckFilename(const FString& Filename, const TCHAR* Context) const;
	};
}
