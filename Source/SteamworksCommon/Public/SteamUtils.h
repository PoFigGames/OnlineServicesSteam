// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

THIRD_PARTY_INCLUDES_START
#include "steam/steamclientpublic.h"
THIRD_PARTY_INCLUDES_END

namespace PoFigGames::Steam
{
	/** The largest ticket Steam issues, which is also the most a server should ever be asked to read. */
	static constexpr int32 MaxAuthSessionTicketSize { 1024 };
}

STEAMWORKSCOMMON_API FORCEINLINE uint32 GetTypeHash(const CSteamID& SteamId)
{
	return GetTypeHash(SteamId.ConvertToUint64());
}

STEAMWORKSCOMMON_API FORCEINLINE FString ToLogString(const CSteamID& SteamId)
{
	return FString::Printf(TEXT("%llu"), SteamId.ConvertToUint64());
}

/**
 * Every EResult the Steamworks SDK defines, in a form fit for a log line.
 *
 * Answers in TCHAR rather than in the SDK's own UTF-8: every caller is either logging or building an
 * FString, and both want TCHAR, so returning UTF-8 here only moved a conversion to each of them.
 */
STEAMWORKSCOMMON_API FORCEINLINE const TCHAR* LexToString(const EResult SteamResult)
{
	switch (SteamResult)
	{
		case k_EResultNone:
			return TEXT("No result");
		case k_EResultOK:
			return TEXT("Success");
		case k_EResultFail:
			return TEXT("Generic failure");
		case k_EResultNoConnection:
			return TEXT("No network connection, or it failed");
		case k_EResultInvalidPassword:
			return TEXT("The password or ticket is not valid");
		case k_EResultLoggedInElsewhere:
			return TEXT("The same user is logged in somewhere else");
		case k_EResultInvalidProtocolVer:
			return TEXT("Wrong protocol version");
		case k_EResultInvalidParam:
			return TEXT("A parameter is not valid");
		case k_EResultFileNotFound:
			return TEXT("No such file");
		case k_EResultBusy:
			return TEXT("The call was made while the object was busy, and was not taken");
		case k_EResultInvalidState:
			return TEXT("The object was in a state this call cannot be made from");
		case k_EResultInvalidName:
			return TEXT("The name is not valid");
		case k_EResultInvalidEmail:
			return TEXT("The email address is not valid");
		case k_EResultDuplicateName:
			return TEXT("That name is already taken");
		case k_EResultAccessDenied:
			return TEXT("Access denied");
		case k_EResultTimeout:
			return TEXT("The operation ran out of time");
		case k_EResultBanned:
			return TEXT("Banned by VAC");
		case k_EResultAccountNotFound:
			return TEXT("No such account");
		case k_EResultInvalidSteamID:
			return TEXT("The SteamID is not valid");
		case k_EResultServiceUnavailable:
			return TEXT("The service is unavailable just now");
		case k_EResultNotLoggedOn:
			return TEXT("Nobody is logged on");
		case k_EResultPending:
			return TEXT("Still running, or waiting on somebody else");
		case k_EResultEncryptionFailure:
			return TEXT("Encryption or decryption failed");
		case k_EResultInsufficientPrivilege:
			return TEXT("Not enough privilege for this");
		case k_EResultLimitExceeded:
			return TEXT("A limit was exceeded");
		case k_EResultRevoked:
			return TEXT("Access has been revoked");
		case k_EResultExpired:
			return TEXT("It has expired");
		case k_EResultAlreadyRedeemed:
			return TEXT("Already redeemed, and cannot be redeemed twice");
		case k_EResultDuplicateRequest:
			return TEXT("The same request has already been acted on");
		case k_EResultAlreadyOwned:
			return TEXT("Everything asked for is already owned");
		case k_EResultIPNotFound:
			return TEXT("No such IP address");
		case k_EResultPersistFailed:
			return TEXT("The change could not be written to the store");
		case k_EResultLockingFailed:
			return TEXT("The lock for this operation could not be taken");
		case k_EResultLogonSessionReplaced:
			return TEXT("The logon session was replaced by another");
		case k_EResultConnectFailed:
			return TEXT("Could not connect");
		case k_EResultHandshakeFailed:
			return TEXT("The handshake failed");
		case k_EResultIOFailure:
			return TEXT("Input or output failed");
		case k_EResultRemoteDisconnect:
			return TEXT("The other end disconnected");
		case k_EResultShoppingCartNotFound:
			return TEXT("No such shopping cart");
		case k_EResultBlocked:
			return TEXT("The other user does not allow it");
		case k_EResultIgnored:
			return TEXT("The target is ignoring the sender");
		case k_EResultNoMatch:
			return TEXT("Nothing matched the request");
		case k_EResultAccountDisabled:
			return TEXT("The account is disabled");
		case k_EResultServiceReadOnly:
			return TEXT("The service is not accepting changes just now");
		case k_EResultAccountNotFeatured:
			return TEXT("The account does not have this feature");
		case k_EResultAdministratorOK:
			return TEXT("Allowed, but only because the caller is an administrator");
		case k_EResultContentVersion:
			return TEXT("Content version mismatch");
		case k_EResultTryAnotherCM:
			return TEXT("This coordinator cannot serve the request; try another");
		case k_EResultPasswordRequiredToKickSession:
			return TEXT("Already logged in elsewhere, and the cached credential was refused");
		case k_EResultAlreadyLoggedInElsewhere:
			return TEXT("Already logged in elsewhere; wait");
		case k_EResultSuspended:
			return TEXT("Suspended or paused");
		case k_EResultCancelled:
			return TEXT("Cancelled");
		case k_EResultDataCorruption:
			return TEXT("Cancelled because the data is malformed or unrecoverable");
		case k_EResultDiskFull:
			return TEXT("Cancelled for want of disk space");
		case k_EResultRemoteCallFailed:
			return TEXT("A remote or inter-process call failed");
		case k_EResultPasswordUnset:
			return TEXT("No password is set on the server, so none could be checked");
		case k_EResultExternalAccountUnlinked:
			return TEXT("The external account is not linked to a Steam account");
		case k_EResultPSNTicketInvalid:
			return TEXT("The PlayStation Network ticket is not valid");
		case k_EResultExternalAccountAlreadyLinked:
			return TEXT("The external account is already linked to another account");
		case k_EResultRemoteFileConflict:
			return TEXT("The local and remote files conflict, so the sync cannot continue");
		case k_EResultIllegalPassword:
			return TEXT("That password is not allowed");
		case k_EResultSameAsPreviousValue:
			return TEXT("The new value is the same as the old one");
		case k_EResultAccountLogonDenied:
			return TEXT("Login denied by two-factor authentication");
		case k_EResultCannotUseOldPassword:
			return TEXT("The old password cannot be reused");
		case k_EResultInvalidLoginAuthCode:
			return TEXT("The login auth code is not valid");
		case k_EResultAccountLogonDeniedNoMail:
			return TEXT("Login denied by two-factor authentication, and no mail was sent");
		case k_EResultHardwareNotCapableOfIPT:
			return TEXT("The hardware cannot do identity protection");
		case k_EResultIPTInitError:
			return TEXT("Identity protection failed to start");
		case k_EResultParentalControlRestricted:
			return TEXT("Refused by parental controls");
		case k_EResultFacebookQueryError:
			return TEXT("The Facebook query returned an error");
		case k_EResultExpiredLoginAuthCode:
			return TEXT("The login auth code has expired");
		case k_EResultIPLoginRestrictionFailed:
			return TEXT("Refused by an IP login restriction");
		case k_EResultAccountLockedDown:
			return TEXT("The account is locked");
		case k_EResultAccountLogonDeniedVerifiedEmailRequired:
			return TEXT("Login denied until the email address is verified");
		case k_EResultNoMatchingURL:
			return TEXT("No matching URL");
		case k_EResultBadResponse:
			return TEXT("The response could not be read");
		case k_EResultRequirePasswordReEntry:
			return TEXT("The password has to be entered again before this can go ahead");
		case k_EResultValueOutOfRange:
			return TEXT("The value is outside the range this accepts");
		case k_EResultUnexpectedError:
			return TEXT("Something happened that was not meant to be possible");
		case k_EResultDisabled:
			return TEXT("The service has been turned off");
		case k_EResultInvalidCEGSubmission:
			return TEXT("The files submitted to the custom executable generation server are not valid");
		case k_EResultRestrictedDevice:
			return TEXT("This device is not allowed to do that");
		case k_EResultRegionLocked:
			return TEXT("Not allowed in this region");
		case k_EResultRateLimitExceeded:
			return TEXT("Rate limited for now; try again later");
		case k_EResultAccountLoginDeniedNeedTwoFactor:
			return TEXT("Login needs a two-factor code");
		case k_EResultItemDeleted:
			return TEXT("It has been deleted");
		case k_EResultAccountLoginDeniedThrottle:
			return TEXT("Login failed, and further attempts are being throttled");
		case k_EResultTwoFactorCodeMismatch:
			return TEXT("The two-factor code does not match");
		case k_EResultTwoFactorActivationCodeMismatch:
			return TEXT("The two-factor activation code does not match");
		case k_EResultAccountAssociatedToMultiplePartners:
			return TEXT("The account is associated with more than one partner");
		case k_EResultNotModified:
			return TEXT("The data has not changed");
		case k_EResultNoMobileDevice:
			return TEXT("The account has no mobile device");
		case k_EResultTimeNotSynced:
			return TEXT("The time given is out of tolerance");
		case k_EResultSmsCodeFailed:
			return TEXT("The SMS code failed");
		case k_EResultAccountLimitExceeded:
			return TEXT("Too many accounts are using this resource");
		case k_EResultAccountActivityLimitExceeded:
			return TEXT("The account has been changed too often lately");
		case k_EResultPhoneActivityLimitExceeded:
			return TEXT("Too many changes to this phone number");
		case k_EResultRefundToWallet:
			return TEXT("The refund has to go to the wallet rather than the payment method");
		case k_EResultEmailSendFailure:
			return TEXT("The email could not be sent");
		case k_EResultNotSettled:
			return TEXT("Not until the payment has settled");
		case k_EResultNeedCaptcha:
			return TEXT("A captcha has to be solved first");
		case k_EResultGSLTDenied:
			return TEXT("A game server login token of this owner has been banned");
		case k_EResultGSOwnerDenied:
			return TEXT("The game server owner is refused for another reason");
		case k_EResultInvalidItemType:
			return TEXT("That kind of thing cannot be acted on");
		case k_EResultIPBanned:
			return TEXT("This IP address is banned from doing that");
		case k_EResultGSLTExpired:
			return TEXT("The game server login token expired from disuse, and can be reset");
		case k_EResultInsufficientFunds:
			return TEXT("Not enough wallet funds");
		case k_EResultTooManyPending:
			return TEXT("Too many of these are pending already");
		case k_EResultNoSiteLicensesFound:
			return TEXT("No site licences found");
		case k_EResultWGNetworkSendExceeded:
			return TEXT("The response was too large to send");
		case k_EResultAccountNotFriends:
			return TEXT("The two users are not mutually friends");
		case k_EResultLimitedUserAccount:
			return TEXT("The account is limited");
		case k_EResultCantRemoveItem:
			return TEXT("That item cannot be removed");
		case k_EResultAccountDeleted:
			return TEXT("The account has been deleted");
		case k_EResultExistingUserCancelledLicense:
			return TEXT("A licence for this exists, but was cancelled");
		case k_EResultCommunityCooldown:
			return TEXT("Refused by a community cooldown");
		case k_EResultNoLauncherSpecified:
			return TEXT("A launcher is needed to pick the realm, and none was named");
		case k_EResultMustAgreeToSSA:
			return TEXT("The subscriber agreement has to be accepted before logging in");
		case k_EResultLauncherMigrated:
			return TEXT("That launcher is no longer supported");
		case k_EResultSteamRealmMismatch:
			return TEXT("The user's realm does not match the resource's");
		case k_EResultInvalidSignature:
			return TEXT("The signature does not check out");
		case k_EResultParseFailure:
			return TEXT("The input could not be parsed");
		case k_EResultNoVerifiedPhone:
			return TEXT("The account has no verified phone number");
		case k_EResultInsufficientBattery:
			return TEXT("Not enough battery charge to do that");
		case k_EResultChargerRequired:
			return TEXT("A charger has to be plugged in for this");
		case k_EResultCachedCredentialInvalid:
			return TEXT("The cached credential is no longer valid; log in again");
		case k_EResultNotSupported:
			return TEXT("This API does not support that data");
		case k_EResultFamilySizeLimitExceeded:
			return TEXT("The family is already at its maximum size");
		case k_EResultOfflineAppCacheInvalid:
			return TEXT("The offline cache is not enough to log in with");
		case k_EResultTryLater:
			return TEXT("Try again later");
		default:
			return TEXT("Unrecognised Steam result");
	}
}
