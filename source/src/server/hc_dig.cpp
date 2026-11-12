// server.dll: the cells minecraft dug out of half-life's walls (see hc_dig.h).

#include "cbase.h"
#include "debugoverlay_shared.h"
#include "player.h"

#include "tier0/valve_minmax_off.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "core/hc_collision.h"
#include "core/hc_link.h"
#include "core/hc_log.h"
#include "core/hc_module.h"
#include "core/hc_units.h"
#include "server/hc_dig.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		constexpr int         SECTION_BLOCKS = 16;
		constexpr double      PVS_BLOCKS = 24.0;  // dug cells this close to the player go into its pvs
		constexpr std::size_t PVS_POINTS = 64;    // and at most this many of them, the nearest
		constexpr double      OUTLINE_BLOCKS = 16.0;
		constexpr std::size_t OUTLINE_CELLS = 512;
		constexpr float       OUTLINE_SECONDS = 0.2f;   // hc_debug_dig draws again this often
		constexpr float       OUTLINE_LINGER = 0.25f;   // and its lines stay a little longer, so they don't flicker
		constexpr float       AROUND_CELL = 0.5f;       // blocks around a changed cell whose regions go out again

		ConVar hc_debug_dig("hc_debug_dig", "0", 0,
			"halfcraft: outline the cells dug out of half-life's walls near the player (orange) and the pieces the world's brushes were cut into around them (blue)");
	}

	void DigState::reset(MapSlot slot)
	{
		slot_ = slot;
		version_ = 0;  // ask for everything again
		cells_.clear();
		next_outline_ = 0.0f;  // the clock starts over with the map
		outlined_ = false;
	}

	void DigState::update(CollisionStreamer* streamer, Link* server_link)
	{
		if (server_link) {
			take_messages(*server_link, streamer);
		} else {
			take_changes(streamer);
		}
		outline();
	}

	void DigState::take_messages(Link& link, CollisionStreamer* streamer)
	{
		changes_.clear();
		link.drain_render(
			[&](std::uint32_t type, const std::uint8_t* payload, std::uint32_t bytes) {
				if (type != proto::kRenDug || bytes < sizeof(proto::RenDug)) {
					return;
				}
				proto::RenDug header;
				std::memcpy(&header, payload, sizeof(header));
				DugSection section{};
				section.sx = header.sx;
				section.sy = header.sy;
				section.sz = header.sz;
				if (header.count > 0 && sizeof(header) + sizeof(section.bits) <= bytes) {
					section.count = header.count;
					std::memcpy(section.bits, payload + sizeof(header), sizeof(section.bits));
				}
				changes_.push_back(section);
			},
			1u << 20);
		if (!changes_.empty()) {
			apply_changes(static_cast<int>(changes_.size()), streamer);
		}
	}

	void DigState::take_changes(CollisionStreamer* streamer)
	{
		if (!dug_since_) {
			dug_since_ = reinterpret_cast<DugSinceFn>(find_export("client.dll", HC_DUG_EXPORT));
			if (!dug_since_) {
				return;
			}
		}
		std::uint32_t now = version_;
		const int     changed = dug_since_(version_, nullptr, 0, &now);
		if (changed <= 0) {
			version_ = now;
			return;
		}
		changes_.resize(static_cast<std::size_t>(changed));
		const int copied = dug_since_(version_, changes_.data(), changed, &now);
		version_ = now;
		apply_changes(std::min(copied, changed), streamer);
	}

	void DigState::apply_changes(int copied, CollisionStreamer* streamer)
	{
		changed_.clear();
		const float centre = static_cast<float>(slot_.x_blocks());
		for (int i = 0; i < copied; ++i) {
			const DugSection& section = changes_[static_cast<std::size_t>(i)];
			// a section another map's slot has (minecraft keeps them all) has nothing dug in this one
			const bool this_map = std::fabs(static_cast<float>(section.sx * SECTION_BLOCKS) - centre) <= KEEP_SLOT_BLOCKS;
			cells_.set(section.sx, section.sy, section.sz, section.count > 0 && this_map ? section.bits : nullptr, changed_);
		}
		// the regions a changed cell's collision lies in, its triangles' half block of slack included: a few around
		// each cell rather than the section's box, which spans as many as invalidate takes at once
		if (streamer) {
			for (const DugCells::Cell& cell : changed_) {
				const float lo[3] = { cell[0] - AROUND_CELL, cell[1] - AROUND_CELL, cell[2] - AROUND_CELL };
				const float hi[3] = { cell[0] + 1.0f + AROUND_CELL, cell[1] + 1.0f + AROUND_CELL, cell[2] + 1.0f + AROUND_CELL };
				streamer->invalidate(lo, hi);
			}
		}
		log_info("dig: %d section(s) changed, %zu cell(s) dug in this map", copied, cells_.count());
	}

	void DigState::outline()
	{
		outlined_ = false;
		if (!hc_debug_dig.GetBool() || gpGlobals->curtime < next_outline_) {
			return;
		}
		CBasePlayer* player = UTIL_GetLocalPlayer();
		if (!player) {
			return;
		}
		next_outline_ = gpGlobals->curtime + OUTLINE_SECONDS;
		const Vector&               origin = player->GetAbsOrigin();
		const float                 feet[3] = { origin.x, origin.y, origin.z };
		std::vector<DugCells::Cell> cells;
		cells_.nearest(source_to_mc(feet, slot_), OUTLINE_BLOCKS, OUTLINE_CELLS, cells);
		for (const DugCells::Cell& cell : cells) {
			const float lo[3] = { static_cast<float>(cell[0]), static_cast<float>(cell[1]), static_cast<float>(cell[2]) };
			const float hi[3] = { lo[0] + 1.0f, lo[1] + 1.0f, lo[2] + 1.0f };
			outline_mc_box(lo, hi, slot_, 255, 80, 0);
		}
		outlined_ = true;
	}

	bool DigState::dug(int x, int y, int z) const
	{
		return cells_.dug(x, y, z);
	}

	void DigState::boxes_in(const float lo[3], const float hi[3], std::vector<DigBox>& out) const
	{
		std::vector<DugCells::Cell> cells;
		cells_.cells_in(lo, hi, cells);
		merge_cells(std::move(cells), out);
	}

	void DigState::pvs_points(const McVec& player, std::vector<Vector>& out) const
	{
		out.clear();
		std::vector<DugCells::Cell> cells;
		cells_.nearest(player, PVS_BLOCKS, PVS_POINTS, cells);
		for (const DugCells::Cell& cell : cells) {
			float centre[3];
			mc_to_source(cell[0] + 0.5, cell[1] + 0.5, cell[2] + 0.5, slot_, centre);
			out.push_back(Vector(centre[0], centre[1], centre[2]));
		}
	}

	void outline_mc_box(const float lo[3], const float hi[3], MapSlot slot, int r, int g, int b)
	{
		// corner i has the box's hi along each axis whose bit is set in i (x 1, y 2, z 4)
		Vector corners[8];
		for (int i = 0; i < 8; ++i) {
			float source[3];
			mc_to_source((i & 1) ? hi[0] : lo[0], (i & 2) ? hi[1] : lo[1], (i & 4) ? hi[2] : lo[2], slot, source);
			corners[i].Init(source[0], source[1], source[2]);
		}
		// an edge joins two corners one bit apart
		for (int i = 0; i < 8; ++i) {
			for (int bit = 1; bit < 8; bit <<= 1) {
				if (!(i & bit)) {
					NDebugOverlay::Line(corners[i], corners[i | bit], r, g, b, true, OUTLINE_LINGER);
				}
			}
		}
	}
}
