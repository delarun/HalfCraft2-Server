// server.dll: co-op's fixes to valve's game code for players who live on. singleplayer reloads the map when the
// player dies; with two slots (the engine's deathmatch 1, never coop 1) respawn() spawns the dead player again
// instead, a path half-life 2's own code never took. a player who comes into a game for more than one (a dedicated
// server's, a listen server's guest) or spawns again in it gets the suit and the server's loadout: the maps hand them
// out once, as the level starts. and whether the server has a level, which client.dll's hc_host waits on
// (hc_bridge.h).

#include "cbase.h"
#include "basecombatweapon.h"
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

		ConVar hc_spawn_suit("hc_spawn_suit", "1", FCVAR_GAMEDLL, "a player who spawns in a game for more than one wears the suit (its hud with it)");
		ConVar hc_spawn_loadout("hc_spawn_loadout", "weapon_crowbar weapon_physcannon weapon_pistol weapon_smg1", FCVAR_GAMEDLL,
			"the weapons a player who spawns in a game for more than one gets, by class name, space separated (\"\" for none)");

		/// how many rounds a spawned player has at least beside each weapon's clip: two clips' worth
		constexpr int SPAWN_CLIPS = 2;
	}

	void server_player_spawned(CBasePlayer* player)
	{
		if (player == nullptr || gpGlobals->maxClients <= 1) {
			return;  // singleplayer: the map and the saves hand everything out
		}
		const bool suit = hc_spawn_suit.GetBool() && !player->IsSuitEquipped();
		if (suit) {
			player->EquipSuit(false);
		}
		CUtlStringList names;
		V_SplitString(hc_spawn_loadout.GetString(), " ", names);
		int given = 0;
		for (int i = 0; i < names.Count(); ++i) {
			if (names[i][0] != '\0' && !player->Weapon_OwnsThisType(names[i])) {
				player->GiveNamedItem(names[i]);
				++given;
			}
		}
		for (int i = 0; i < player->WeaponCount(); ++i) {
			CBaseCombatWeapon* weapon = player->GetWeapon(i);
			if (weapon == nullptr || !weapon->UsesPrimaryAmmo() || weapon->GetMaxClip1() <= 0) {
				continue;
			}
			const int type = weapon->GetPrimaryAmmoType();
			const int want = weapon->GetMaxClip1() * SPAWN_CLIPS;
			if (const int have = player->GetAmmoCount(type); have < want) {
				player->GiveAmmo(want - have, type, true);
			}
		}
		log_info("spawn: player %d%s, %d weapon(s) of the loadout given", player->entindex(), suit ? " suited up" : "", given);
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
		server_player_spawned(player);
	}
}

// client.dll's hc_host: whether this process's server has a level (hc_bridge.h)
extern "C" HC_EXPORT int HalfCraft_ServerInLevel()
{
	return halfcraft::g_server_in_level ? 1 : 0;
}
