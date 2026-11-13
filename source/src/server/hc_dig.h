#pragma once

// server.dll: the cells minecraft dug out of half-life's walls, picked up from client.dll (HalfCraft_DugSince,
// shared/hc_bridge.h; none come without the shader dll). the world's brushes have them cut out of the collision
// streamed to minecraft (hc_world_collision.h), and the regions around a cell that changed go out again. include after
// the sdk headers and tier0/valve_minmax_off.h.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/hc_dig_clip.h"
#include "core/hc_dug_cells.h"
#include "core/hc_units.h"
#include "shared/hc_bridge.h"

namespace halfcraft
{
	class Link;

	class CollisionStreamer;

	/// the dug cells of the current map. main thread, all of it.
	class DigState
	{
	public:
		/// a new map: where it sits; everything is asked for again.
		void reset(MapSlot slot);
		/// once per frame: picks up what changed in client.dll, sends the regions around each cell that changed again,
		/// and outlines the cells for hc_debug_dig.
		/// @param streamer - nullptr: nothing to send again
		/// @param relayed - a dedicated server's: the dug sections the halfcraft server's minecraft wrote into the
		///                  server link's render ring this frame (kRenDug, take_relayed); nullptr: client.dll hands
		///                  them over (HalfCraft_DugSince)
		void update(CollisionStreamer* streamer, const std::vector<DugSection>* relayed = nullptr);
		/// @param x, y, z - a minecraft block
		bool dug(int x, int y, int z) const;
		/// the dug cells a minecraft box reaches into, merged into boxes (core/hc_dig_clip.h's merge_cells)
		/// @param out - replaced by them
		void boxes_in(const float lo[3], const float hi[3], std::vector<DigBox>& out) const;
		/// where the dug cells nearest the player are, for the pvs: the centres of the 64 nearest within 24 blocks,
		/// nearest first
		/// @param player - minecraft coords
		/// @param out - replaced by them, source units
		void pvs_points(const McVec& player, std::vector<Vector>& out) const;
		/// how many cells are dug in this map
		std::size_t count() const { return cells_.count(); }
		/// hc_debug_dig outlined the cells near the player in this frame's update: what else it outlines goes with them
		bool outlined() const { return outlined_; }

	private:
		void take_changes(CollisionStreamer* streamer);
		/// the sections in changes_ (copied of them) into the cells, and the regions around each changed cell out again
		void apply_changes(int copied, CollisionStreamer* streamer);
		void outline();

		DugSinceFn                  dug_since_ = nullptr;
		std::uint32_t               version_ = 0;
		MapSlot                     slot_;
		DugCells                    cells_;
		std::vector<DugSection>     changes_;
		std::vector<DugCells::Cell> changed_;
		float                       next_outline_ = 0.0f;  // gpGlobals->curtime
		bool                        outlined_ = false;
	};

	/// a dedicated server's: what the halfcraft server's minecraft wrote into the server link's render ring since the
	/// last frame, its dug sections (kRenDug) and its solid blocks (kRenSolids), as client.dll hands them over in a
	/// single player's game (HalfCraft_DugSince, HalfCraft_SolidsSince). the ring has one reader: this.
	void take_relayed(Link& link, std::vector<DugSection>& dug, std::vector<SolidSection>& solids);

	/// draws a minecraft box's 12 edges for a moment (hc_debug_dig), seen through walls.
	/// @param lo, hi - minecraft coords
	void outline_mc_box(const float lo[3], const float hi[3], MapSlot slot, int r, int g, int b);
}
