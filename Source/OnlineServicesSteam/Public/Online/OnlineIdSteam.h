// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineIdCommon.h"
#include "SteamUtils.h"

namespace PoFigGames::Online
{
	/**
	 * @class IOnlineAccountIdRegistrySteam
	 *
	 * @brief Interface for an account ID registry specialized for handling Steam IDs.
	 */
	class IOnlineAccountIdRegistrySteam : public UE::Online::IOnlineAccountIdRegistry
	{
	public:
		virtual UE::Online::FAccountId FindOrAddAccountId(const CSteamID& SteamAccountId) = 0;
		virtual UE::Online::FAccountId FindAccountId(const CSteamID& SteamAccountId) const = 0;
		virtual CSteamID               GetSteamId(const UE::Online::FAccountId& AccountId) const = 0;
	};

	/**
	 * @class FOnlineAccountIdRegistrySteam
	 *
	 * @brief Account id registry specifically for Steam ids which are segmented.
	 */
	class FOnlineAccountIdRegistrySteam : public IOnlineAccountIdRegistrySteam
	{
	public:
		static const UE::Online::FAccountId InvalidSteamAccountId;
		
		ONLINESERVICESSTEAM_API FOnlineAccountIdRegistrySteam(UE::Online::EOnlineServices Services);
		virtual ~FOnlineAccountIdRegistrySteam() override = default;

	#pragma region IOnlineAccountIdRegistrySteam		
		ONLINESERVICESSTEAM_API virtual UE::Online::FAccountId FindOrAddAccountId(const CSteamID& SteamAccountId) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::FAccountId FindAccountId(const CSteamID& SteamAccountId) const override;
		ONLINESERVICESSTEAM_API virtual CSteamID               GetSteamId(const UE::Online::FAccountId& AccountId) const override;
	#pragma endregion

	#pragma region IOnlineAccountIdRegistry	
		ONLINESERVICESSTEAM_API virtual FString                ToString(const UE::Online::FAccountId& AccountId) const override;
		ONLINESERVICESSTEAM_API virtual FString                ToLogString(const UE::Online::FAccountId& AccountId) const override;
		ONLINESERVICESSTEAM_API virtual TArray<uint8>          ToReplicationData(const UE::Online::FAccountId& AccountId) const override;
		ONLINESERVICESSTEAM_API virtual UE::Online::FAccountId FromReplicationData(const TArray<uint8>& ReplicationData) override;
		ONLINESERVICESSTEAM_API virtual UE::Online::FAccountId FromStringData(const FString& StringData) override;		
	#pragma endregion 

		static ONLINESERVICESSTEAM_API FOnlineAccountIdRegistrySteam& GetRegistered(UE::Online::EOnlineServices Services);
		
	private:
		UE::Online::TOnlineBasicAccountIdRegistry<CSteamID> Registry;
	};
	
	ONLINESERVICESSTEAM_API CSteamID GetSteamUserId(const UE::Online::FAccountId& AccountId);
	ONLINESERVICESSTEAM_API CSteamID GetSteamUserIdChecked(const UE::Online::FAccountId& AccountId);
	
	ONLINESERVICESSTEAM_API UE::Online::FAccountId FindAccountId(const CSteamID& SteamUserId, UE::Online::EOnlineServices Services = UE::Online::EOnlineServices::Steam);
	ONLINESERVICESSTEAM_API UE::Online::FAccountId FindAccountIdChecked(const CSteamID& SteamUserId, UE::Online::EOnlineServices Services = UE::Online::EOnlineServices::Steam);

	/**
	 * Account id of a Steam user, registering the identity the first time it is seen.
	 *
	 * Turning a Steam id into an account id is a lookup in a local registry: nothing is asked of Steam, so
	 * the answer is there before the call returns. Callers which only want to know whether an identity is
	 * already known use FindAccountId instead.
	 */
	ONLINESERVICESSTEAM_API UE::Online::FAccountId FindOrAddAccountId(const CSteamID& SteamUserId, UE::Online::EOnlineServices Services = UE::Online::EOnlineServices::Steam);

	/**
	 * The same for a list of identities, answering in the order it was asked. A Steam id which names
	 * nobody keeps its place in the answer as an invalid account id rather than being dropped.
	 */
	ONLINESERVICESSTEAM_API TArray<UE::Online::FAccountId> FindOrAddAccountIds(TArrayView<const CSteamID> SteamUserIds,
		UE::Online::EOnlineServices Services = UE::Online::EOnlineServices::Steam);
}
