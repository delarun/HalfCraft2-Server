#pragma once

// server.dll: minecraft owns the player's health whenever its player is in its world (also while
// source has the player on a ladder or a ride): every hit goes to minecraft, so source's own health
// would sit at full and its health kits, wall chargers, suit batteries and medics would never help.
// minecraft's health and absorption are mirrored onto half-life's player instead, so they see it
// and work as usual; whatever they add goes to minecraft (health heals, suit armour becomes
// absorption).

#include <unordered_map>
#include <utility>

#include "core/hc_link.h"

class CBasePlayer;

namespace halfcraft
{
	class Vitals
	{
	public:
		/// once per frame.
		void update(Link& link, CBasePlayer* player);
		/// a dedicated server's, once per frame: each player's health is their minecraft's on the halfcraft server
		/// (the link's PlayerVitals, by source userid)
		void update_players(Link& link);

		/// a level loaded: source's player comes with the health it had (a save, a transition), which
		/// isn't healing to pass on.
		void reset()
		{
			health_.reset();
			armor_.reset();
		}

	private:
		struct Mirror
		{
			int   set = -1;         // what source's player was given last (-1: nothing yet)
			float pending = 0.0f;   // healed in half-life, not yet seen in minecraft (source points)
			float seen = -1.0f;     // minecraft's value as last read (minecraft points)
			float waited = 0.0f;    // seconds the pending amount has gone unanswered

			/// forwards what half-life added since the last frame and returns the value to give source's
			/// player now.
			int  update(CBasePlayer* player, int current, float minecraft, int kind, float frametime);
			void reset() { *this = Mirror{}; }
		};

		Mirror                                             health_;
		Mirror                                             armor_;
		std::unordered_map<int, std::pair<Mirror, Mirror>> players_;  // a dedicated server's, by userid: health, armor
	};

	/// whether minecraft owns the player's health this frame (half-life's hits go to it).
	bool minecraft_owns_health();
	/// whether minecraft owns this player's health: single player's minecraft_owns_health(), or on a dedicated server
	/// whether the halfcraft server's minecraft has this player in its world, alive
	bool minecraft_owns_health(CBasePlayer* player);

	/// a dedicated server's link, for what goes to the halfcraft server's minecraft (hc_server.cpp sets it); nullptr in
	/// single player and on a listen server
	void  set_dedicated_link(Link* link);
	Link* dedicated_link();

	/// an input message for a player's minecraft: in single player through client.dll's input ring (HalfCraft_PushInput);
	/// on a dedicated server into its link, after a kInFor naming the player
	void send_player_input(CBasePlayer* player, proto::InputType type, int code, int a = 0, int b = 0, int c = 0);
}
