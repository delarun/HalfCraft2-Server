// server.dll: the players of a game (see hc_players.h), and hc_debug_players, which logs each one.

#include "cbase.h"
#include "player.h"

#include "tier0/valve_minmax_off.h"

#include "core/hc_log.h"
#include "server/hc_players.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	CBasePlayer* host_player()
	{
		return engine->IsDedicatedServer() ? nullptr : UTIL_PlayerByIndex(1);
	}

	CBasePlayer* singleplayer_player()
	{
		return gpGlobals->maxClients > 1 ? nullptr : UTIL_PlayerByIndex(1);
	}

	bool is_host(const CBasePlayer* player)
	{
		return player != nullptr && player == host_player();
	}

	int player_slots()
	{
		return gpGlobals->maxClients;
	}

	CBasePlayer* connected_player(int index)
	{
		CBasePlayer* player = UTIL_PlayerByIndex(index);
		return player && player->IsConnected() ? player : nullptr;
	}
}

CON_COMMAND(hc_debug_players, "halfcraft: log each player: index, name, host, origin, health, armor, suit, alive, solid")
{
	if (!UTIL_IsCommandIssuedByServerAdmin()) {
		return;
	}
	int count = 0;
	halfcraft::for_each_player([&count](CBasePlayer&) { ++count; });
	// the engine's cvars, as the console shows them (gpGlobals has them as bools, from the map's start)
	const ConVarRef deathmatch_var("deathmatch");
	const ConVarRef coop_var("coop");
	halfcraft::log_info("players: %d of %d slots, deathmatch %d, coop %d", count, halfcraft::player_slots(), deathmatch_var.IsValid() ? deathmatch_var.GetInt() : -1,
		coop_var.IsValid() ? coop_var.GetInt() : -1);
	halfcraft::for_each_player([](CBasePlayer& player) {
		const Vector& origin = player.GetAbsOrigin();
		const bool    solid = player.IsSolid() && !player.IsSolidFlagSet(FSOLID_NOT_SOLID);
		halfcraft::log_info("player %d \"%s\"%s: origin %.1f %.1f %.1f, health %d, armor %d, suit %d, alive %d, solid %d", player.entindex(), player.GetPlayerName(),
			halfcraft::is_host(&player) ? " host" : "", origin.x, origin.y, origin.z, player.GetHealth(), player.ArmorValue(), player.IsSuitEquipped() ? 1 : 0,
			player.IsAlive() ? 1 : 0, solid ? 1 : 0);
	});
}
