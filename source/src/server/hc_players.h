#pragma once

// server.dll: the players of a game. on a listen server the host, whose game the server runs in, is always the
// first slot (entity 1); guests take the slots after it. singleplayer has one slot, the host's.

class CBasePlayer;

namespace halfcraft
{
	/// the host's player: entity 1. nullptr on a dedicated server (nobody plays in it), and while slot 1 has no player
	/// entity (no game, or before the engine puts the host's client in the server). as connected_player(1) under these
	/// rules (see there).
	CBasePlayer* host_player();

	/// the player of a game for one (maxClients 1): what valve's UTIL_GetLocalPlayer gave before a dedicated server's
	/// stood in for it (util.cpp). nullptr in a game for more than one, where it means none in particular.
	CBasePlayer* singleplayer_player();

	/// the player is the host's (host_player()). false for nullptr.
	bool is_host(const CBasePlayer* player);

	/// how many player slots the server has (gpGlobals->maxClients): entities 1 to that are players.
	int player_slots();

	/// the player entity in a slot, while it has one. a client that leaves takes its entity with it: once the engine
	/// drops the client, UTIL_PlayerByIndex gives nullptr for its slot (in game, 3 s after a guest's disconnect the
	/// slot had no entity and hc_debug_players listed the host alone), whatever the sdk's ClientDisconnect says of
	/// edicts that stay. CBasePlayer::IsConnected() is
	/// checked too, but under this game's singleplayer rules it is always true: only CMultiplayRules'
	/// ClientDisconnected marks a player disconnected.
	/// @param index - its entity index, 1 to player_slots()
	/// @return nullptr for a slot without a player entity (no client in it, or none yet)
	CBasePlayer* connected_player(int index);

	/// fn(CBasePlayer&) for each player connected_player gives, in entity order from 1 to player_slots().
	template <class F>
	void for_each_player(F&& fn)
	{
		const int slots = player_slots();
		for (int index = 1; index <= slots; ++index) {
			if (CBasePlayer* player = connected_player(index)) {
				fn(*player);
			}
		}
	}
}
