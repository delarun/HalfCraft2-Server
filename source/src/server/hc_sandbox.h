#pragma once

// server.dll: the halfcraft server's sandbox (hc_sandbox.cpp).

namespace halfcraft
{
	/// minecraft's difficulty is peaceful or not (kEvDifficulty): on peaceful the npcs that hate the players are taken
	/// out, and the map's enemies don't come back until it's over (hc_peaceful).
	void sandbox_set_peaceful(bool peaceful);
}
