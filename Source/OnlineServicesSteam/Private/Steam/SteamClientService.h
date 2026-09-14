// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "SteamInterfaces.h"
#include "SteamServiceBase.h"


namespace PoFigGames::Steam
{
	/**
	 * @class FSteamClientService
	 *
	 * @brief Steam Service handler for the Client Instance.
	 * Owns the client API lifetime and the interfaces resolved from it. Based on FSteamClientInstanceHandler.
	 */
	class FSteamClientService final : public FSteamServiceBase
	{
	public:
		/** Which of the process wide APIs this service owns. */
		static constexpr ESteamApi Api { ESteamApi::Client };

		FSteamClientService() = default;

		/** The API is shut down here rather than in the base, which must not call a virtual while destructing. */
		ONLINESERVICESSTEAM_API virtual ~FSteamClientService() override;

		/** Valid only while the service is initialized. */
		const FSteamClientInterfaces& GetInterfaces() const { return Interfaces; }

	protected:
		FSteamClientInterfaces Interfaces { };

		ONLINESERVICESSTEAM_API virtual bool InternalInit() override;
		ONLINESERVICESSTEAM_API virtual void InternalShutdown() override;
		ONLINESERVICESSTEAM_API virtual void InternalPumpDispatch() const override;
	};
}
