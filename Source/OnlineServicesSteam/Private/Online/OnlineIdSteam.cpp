// Copyright PoFig Games Studio. All Rights Reserved.

#include "Online/OnlineIdSteam.h"

namespace PoFigGames::Online
{
	const UE::Online::FAccountId FOnlineAccountIdRegistrySteam::InvalidSteamAccountId { UE::Online::EOnlineServices::Steam, 0 };

	FOnlineAccountIdRegistrySteam::FOnlineAccountIdRegistrySteam(const UE::Online::EOnlineServices Services)
		: Registry(Services)
	{

	}

	UE::Online::FAccountId FOnlineAccountIdRegistrySteam::FindOrAddAccountId(const CSteamID& SteamAccountId)
	{
		// Checked rather than ensured: an id that names nobody arrives from a peer as a matter of course -
		// eight zero bytes in a replicated identity are all it takes - and that is an answer to give, not a
		// mistake to report. An ensure here wrote a callstack to the log for every one of them.
		if (!SteamAccountId.IsValid())
		{
			return Registry.GetInvalidHandle();
		}

		return Registry.FindOrAddHandle(SteamAccountId);
	}


	UE::Online::FAccountId FOnlineAccountIdRegistrySteam::FindAccountId(const CSteamID& SteamAccountId) const
	{
		return Registry.FindHandle(SteamAccountId);
	}

	CSteamID FOnlineAccountIdRegistrySteam::GetSteamId(const UE::Online::FAccountId& AccountId) const
	{
		return Registry.FindIdValue(AccountId);
	}

	FString FOnlineAccountIdRegistrySteam::ToString(const UE::Online::FAccountId& AccountId) const
	{
		FString Result;
		if (Registry.ValidateOnlineId(AccountId))
		{
			const CSteamID SteamAccountId = Registry.FindIdValue(AccountId);
			Result = LexToString(SteamAccountId.ConvertToUint64());
		}
		else
		{
			check(!AccountId.IsValid()); // Check we haven't been passed a valid handle for a different EOnlineServices.
			Result = TEXT("Invalid");
		}

		return Result;

	}

	FString FOnlineAccountIdRegistrySteam::ToLogString(const UE::Online::FAccountId& AccountId) const
	{
		return ToString(AccountId);
	}

	TArray<uint8> FOnlineAccountIdRegistrySteam::ToReplicationData(const UE::Online::FAccountId& AccountId) const
	{
		TArray<uint8> ReplicationData;
		if (Registry.ValidateOnlineId(AccountId))
		{
			if (const CSteamID SteamAccountId = Registry.FindIdValue(AccountId); ensure(SteamAccountId.IsValid()))
			{
				ReplicationData.SetNumUninitialized(sizeof(uint64));

				const uint64 SteamId = SteamAccountId.ConvertToUint64();
				FMemory::Memcpy(ReplicationData.GetData(), &SteamId, sizeof(uint64));
			}
		}

		return ReplicationData;
	}

	UE::Online::FAccountId FOnlineAccountIdRegistrySteam::FromReplicationData(const TArray<uint8>& ReplicationData)
	{
		if (ReplicationData.Num() == sizeof(uint64))
		{
			uint64 SteamId;
			FMemory::Memcpy(&SteamId, ReplicationData.GetData(), sizeof(uint64));

			return FindOrAddAccountId(SteamId);
		}

		return Registry.GetInvalidHandle();
	}

	UE::Online::FAccountId FOnlineAccountIdRegistrySteam::FromStringData(const FString& StringData)
	{
		// The other way round from ToString, which writes the SteamID in decimal. Anything that is not one -
		// a name, another provider's spelling, the word ToString writes for a handle this registry does not
		// own - parses to nothing a SteamID can be, and is answered with the invalid handle.
		uint64 SteamId { 0 };
		LexFromString(SteamId, *StringData);

		return FindOrAddAccountId(CSteamID { SteamId });
	}

	FOnlineAccountIdRegistrySteam& FOnlineAccountIdRegistrySteam::GetRegistered(const UE::Online::EOnlineServices Services)
	{
		check(Services == UE::Online::EOnlineServices::Steam);

		const auto Registry = UE::Online::FOnlineIdRegistryRegistry::Get().GetAccountIdRegistry(Services);
		check(Registry);

		return *static_cast<FOnlineAccountIdRegistrySteam*>(Registry);
	}

	CSteamID GetSteamUserId(const UE::Online::FAccountId& AccountId)
	{
		return FOnlineAccountIdRegistrySteam::GetRegistered(UE::Online::EOnlineServices::Steam).GetSteamId(AccountId);
	}

	CSteamID GetSteamUserIdChecked(const UE::Online::FAccountId& AccountId)
	{
		const CSteamID SteamUserId = GetSteamUserId(AccountId);
		check(SteamUserId.IsValid());
		return SteamUserId;
	}

	UE::Online::FAccountId FindAccountId(const CSteamID& SteamUserId, UE::Online::EOnlineServices Services)
	{
		return FOnlineAccountIdRegistrySteam::GetRegistered(Services).FindAccountId(SteamUserId);
	}

	UE::Online::FAccountId FindAccountIdChecked(const CSteamID& SteamUserId, UE::Online::EOnlineServices Services)
	{
		const UE::Online::FAccountId AccountId = FindAccountId(SteamUserId, Services);
		check(AccountId.IsValid());
		return AccountId;
	}

	UE::Online::FAccountId FindOrAddAccountId(const CSteamID& SteamUserId, const UE::Online::EOnlineServices Services)
	{
		// A Steam id which names nobody is answered rather than registered: an account id handed out for it
		// could never be turned back into a user.
		if (!SteamUserId.IsValid())
		{
			return FOnlineAccountIdRegistrySteam::InvalidSteamAccountId;
		}

		return FOnlineAccountIdRegistrySteam::GetRegistered(Services).FindOrAddAccountId(SteamUserId);
	}

	TArray<UE::Online::FAccountId> FindOrAddAccountIds(const TArrayView<const CSteamID> SteamUserIds, const UE::Online::EOnlineServices Services)
	{
		TArray<UE::Online::FAccountId> AccountIds;
		AccountIds.Reserve(SteamUserIds.Num());

		auto& AccountIdRegistry = FOnlineAccountIdRegistrySteam::GetRegistered(Services);
		for (const auto& SteamUserId : SteamUserIds)
		{
			AccountIds.Emplace(SteamUserId.IsValid()
				? AccountIdRegistry.FindOrAddAccountId(SteamUserId)
				: FOnlineAccountIdRegistrySteam::InvalidSteamAccountId);
		}

		return AccountIds;
	}
}
