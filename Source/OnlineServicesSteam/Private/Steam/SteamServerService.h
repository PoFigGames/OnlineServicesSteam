// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "SteamInterfaces.h"
#include "SteamServiceBase.h"


namespace PoFigGames::Steam
{
	/**
	 * @class FSteamServerService
	 *
	 * @brief Steam Service handler for the Server Instance.
	 * Owns the game server API lifetime and the interfaces resolved from it. Based on FSteamServerInstanceHandler.
	 */
	class FSteamServerService final : public FSteamServiceBase
	{
	public:
		/** Which of the process wide APIs this service owns. */
		static constexpr ESteamApi Api { ESteamApi::GameServer };

		FSteamServerService() = default;

		/** The API is shut down here rather than in the base, which must not call a virtual while destructing. */
		ONLINESERVICESSTEAM_API virtual ~FSteamServerService() override;

		int32 GetQueryPort() const { return QueryPort; }

		/** Valid only while the service is initialized. */
		const FSteamServerInterfaces& GetInterfaces() const { return Interfaces; }

	protected:
		FSteamServerInterfaces Interfaces { };

		int32 QueryPort { -1 };

		ONLINESERVICESSTEAM_API virtual bool InternalInit() override;
		ONLINESERVICESSTEAM_API virtual void InternalShutdown() override;
		ONLINESERVICESSTEAM_API virtual void InternalPumpDispatch() const override;
	};
}
