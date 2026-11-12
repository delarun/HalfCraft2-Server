#pragma once

// server.dll: what half-life 2's player collides with, as minecraft geometry. include after the sdk
// headers and tier0/valve_minmax_off.h.
//
//   world brushes      enginetrace brush planes        -> convexes (exact, player clips included; the
//                                                         sky's flagged PRIM_SKY, from the map's .bsp;
//                                                         the ones minecraft may dig PRIM_DIGGABLE, each
//                                                         side with its dig material, traced once per brush;
//                                                         all but the sky's with the cells minecraft dug
//                                                         cut out, server/hc_dig.h)
//   displacements      enginetrace displacement collide -> triangles
//   static props       their .phy hulls                -> convexes
//   solid entities     doors, lifts, props, crates     -> convexes, re-sent while they move: what source's
//                                                         player movement hits of them (a model's first
//                                                         solid, a brush model's brushes of the player's
//                                                         contents, a bone follower's own solid; a model
//                                                         without one, nothing)
//   the map's void     the .bsp's tree (models[0])     -> what's behind its walls, for the streamer's
//                                                         worker (void_query, core/hc_dig_void.h)
//
// npcs and players are left out: npcs become minecraft actors, not walls.

#include <memory>
#include <unordered_map>
#include <vector>

#include "core/hc_collision.h"
#include "core/hc_dig_clip.h"
#include "core/hc_dig_material.h"

class CBaseEntity;
class CPhysCollide;
struct model_t;

namespace halfcraft
{
	class DigState;

	class WorldCollision final : public CollisionSource
	{
	public:
		/// a new map: where it sits in the minecraft world, and nothing cached from the last one.
		/// @param map_name - its name ("d1_canals_01"), to read its sky brushes and bsp tree from; nullptr: none
		void reset(MapSlot slot, const char* map_name);

		/// the cells minecraft dug, which gather cuts out of the world's brushes from now on (every map's: reset keeps it).
		/// @param dig - nullptr: none
		void set_dig(const DigState* dig) { dig_ = dig; }

		void gather(const float lo[3], const float hi[3], ColPrimitives& out) override;
		/// the map's bsp tree (none when it didn't read) and slot
		VoidQuery void_query() const override;

		/// hc_debug_voxels: voxelizes the regions around a point again and checks each voxel against
		/// source's own collision (box traces, also from above and below: a displacement is one-sided),
		/// logging the voxels nothing in source is near.
		/// @param centre - minecraft coords
		/// @param radius - regions around it horizontally (one above and below)
		void check_voxels(const McVec& centre, int radius);

		/// solid entities near the player that moved since the last frame: the regions they left
		/// and entered go to minecraft again.
		/// @param player - the player's feet, minecraft coords
		void track_movers(const McVec& player, CollisionStreamer& streamer);

		/// hc_debug_dig_material: logs the world brush a ray hits, its side's surface and game material, and the dig
		/// materials minecraft gets for that side and the brush.
		/// @param eye, forward - the ray, source coords
		void log_dig_material(const Vector& eye, const Vector& forward);

		/// hc_debug_dig: the pieces the world's brushes were cut into around dug cells, as last gathered, that come within
		/// radius of a point along each axis.
		/// @param p - minecraft coords
		/// @param out - replaced by their bounds, minecraft coords
		void cut_pieces_near(const McVec& p, double radius, std::vector<DigBox>& out) const;

	private:
		using Hull = std::vector<Vector>;  // a convex piece as triangles (3 corners each), model space

		/// what a diggable world brush digs as
		struct BrushDig
		{
			std::vector<std::uint8_t> sides;                 // by GetBrushInfo's plane: its side's material (never DIG_NONE)
			std::uint8_t              material = DIG_STONE;  // the brush's own (brush_material): what the faces a hole cuts in it dig as
		};

		/// a model's vcollide by solid (cached): of a brush model's, only the pieces of the brushes the player collides with
		/// @return empty: no vcollide
		const std::vector<std::vector<Hull>>& model_hulls(const model_t* model);
		/// a solid's convex pieces, appended
		/// @param brush - a brush model's (vbsp's): only the pieces of the brushes the player collides with
		static void solid_hulls(const CPhysCollide* solid, bool brush, std::vector<Hull>& out);
		void        add_hulls(const std::vector<Hull>& hulls, const matrix3x4_t& to_world, std::uint32_t flags, ColPrimitives& out) const;
		void        add_box(const Vector& mins, const Vector& maxs, const matrix3x4_t& to_world, ColPrimitives& out) const;
		void        add_brushes(const Vector& mins, const Vector& maxs, ColPrimitives& out, std::vector<DigBox>& cut);
		void        add_displacements(const Vector& mins, const Vector& maxs, ColPrimitives& out);
		/// a displacement triangle's dig material (add_displacements), traced the first time it's asked for on this map:
		/// a blended ground's (sand and grass) is the material its corners' alpha picks, which only a trace gets
		/// (the displacements' collision model has the first material alone)
		/// @param verts - its corners, source coords
		/// @param fallback - its collision model's material: when the trace misses
		std::uint8_t ground_material(const Vector verts[3], std::uint8_t fallback);
		void         add_static_props(const Vector& mins, const Vector& maxs, ColPrimitives& out);
		void         add_entities(const Vector& mins, const Vector& maxs, ColPrimitives& out);
		void         add_entity(CBaseEntity* entity, ColPrimitives& out);
		/// an entity's vphysics objects' solids, each where vphysics has it (what its TestCollision traces)
		void add_vphysics(CBaseEntity* entity, ColPrimitives& out);

		/// a diggable world brush's materials (add_brushes), traced the first time it's asked for on this map: sides
		/// that don't trace cleanly take the brush's
		/// @param planes - its GetBrushInfo planes
		const BrushDig& brush_dig(int brush, const CUtlVector<Vector4D>& planes);
		/// whether minecraft may dig a world brush: solid, window or grate, and not the sky
		bool diggable(int brush, int contents) const;
		/// one of the map's sky brushes (read_sky_brushes)
		bool sky_brush(int brush) const;

		/// minecraft box -> source box
		void to_source_box(const float lo[3], const float hi[3], Vector& mins, Vector& maxs) const;
		/// source box -> minecraft box
		void to_mc_box(const Vector& mins, const Vector& maxs, float lo[3], float hi[3]) const;

		struct Mover
		{
			Vector origin;
			QAngle angles;
			Vector mins, maxs;  // world bounds when last seen
			int    frame;
		};

		MapSlot                                                            slot_;
		std::vector<bool>                                                  sky_brushes_;  // by brush index
		std::unordered_map<int, BrushDig>                                  brush_dig_;    // by brush index: the diggable brushes asked for so far
		std::unordered_map<std::uint64_t, std::uint8_t>                    ground_dig_;   // by triangle (its centre, quantized): ground_material's so far
		std::shared_ptr<const BspTree>                                     bsp_;          // the map's tree, which jobs share with the streamer's worker
		std::unordered_map<const model_t*, std::vector<std::vector<Hull>>> hulls_;  // by model: model_hulls'
		std::unordered_map<int, Mover>                                     movers_;  // by entity serial-qualified index
		int                                                                frame_ = 0;
		const DigState*                                                    dig_ = nullptr;
		// by region (region_key): the bounds of the pieces brushes were cut into there, as last gathered (hc_debug_dig)
		std::unordered_map<std::uint64_t, std::vector<DigBox>> cut_pieces_;
	};
}
