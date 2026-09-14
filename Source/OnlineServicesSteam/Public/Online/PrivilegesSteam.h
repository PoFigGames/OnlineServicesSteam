// Copyright PoFig Games Studio. All Rights Reserved.

#pragma once

#include "Online/OnlineComponentSteam.h"
#include "Online/PrivilegesCommon.h"

namespace PoFigGames::Online {
	/**
	 * @class FPrivilegesSteam
	 *
	 * @brief Steam Privileges Online Component
	 */
	class FPrivilegesSteam : public TOnlineComponentSteam<UE::Online::FPrivilegesCommon>
	{
	public:
		using Super = FPrivilegesCommon;
		using TOnlineComponentSteam::TOnlineComponentSteam;

		virtual UE::Online::TOnlineAsyncOpHandle<UE::Online::FQueryUserPrivilege> QueryUserPrivilege(UE::Online::FQueryUserPrivilege::Params&& Params) override;
	};
}
