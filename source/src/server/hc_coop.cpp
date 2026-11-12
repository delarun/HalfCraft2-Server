// server.dll: co-op's fixes to valve's game code for players who live on. singleplayer reloads the map when the
// player dies; with two slots (the engine's deathmatch 1, never coop 1) respawn() spawns the dead player again
// instead, a path half-life 2's own code never took. and whether the server has a level, which client.dll's hc_host
// waits on (hc_bridge.h).

#include "cbase.h"
#include "player.h"

#include "tier0/valve_minmax_off.h"

#include "core/hc_log.h"
#include "core/hc_module.h"
#include "shared/hc_hooks.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		bool g_server_in_level = false;  // HalfCraft_ServerInLevel

		// the engine's shutdown of a server runs the level's shutdown, then marks the server inactive, with no
		// console command between them: once the level is gone, maxplayers is taken
		class ServerLevelSystem final : public CAutoGameSystem
		{
		public:
			ServerLevelSystem() : CAutoGameSystem("HalfCraftServerLevel") {}

			void LevelInitPreEntity() override { g_server_in_level = true; }

			void LevelShutdownPostEntity() override { g_server_in_level = false; }
		};

		ServerLevelSystem g_server_level;
	}

	void server_player_respawned(CBasePlayer* player)
	{
		if (player == nullptr) {
			return;
		}
		// CBasePlayer::Event_Killed made the body non-solid and Spawn() keeps it so (hl2mp's CHL2MP_Player::Spawn
		// clears it, hl2's never had to): such a player walks through the others and touches no trigger
		player->RemoveSolidFlags(FSOLID_NOT_SOLID);
		log_info("respawn: player %d is solid again", player->entindex());
	}
}

// client.dll's hc_host: whether this process's server has a level (hc_bridge.h)
extern "C" HC_EXPORT int HalfCraft_ServerInLevel()
{
	return halfcraft::g_server_in_level ? 1 : 0;
}
