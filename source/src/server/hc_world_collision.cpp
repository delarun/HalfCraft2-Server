// server.dll: half-life 2's collision, as minecraft geometry (see hc_world_collision.h).

#include "cbase.h"
#include "engine/IEngineTrace.h"
#include "engine/IStaticPropMgr.h"
#include "engine/ivmodelinfo.h"
#include "vphysics_interface.h"
#include "physics_shared.h"
#include "gamerules.h"
#include "bspfile.h"
#include "filesystem.h"
#include "world.h"
#include "model_types.h"
#include "player.h"
#include "studio.h"

#include "tier0/valve_minmax_off.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>

#include "core/hc_dig_ground.h"
#include "core/hc_dig_material.h"
#include "core/hc_log.h"
#include "server/hc_players.h"
#include "server/hc_block_solids.h"
#include "server/hc_dig.h"
#include "server/hc_world_collision.h"
#include "shared/hc_bsp.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		constexpr int   MAX_ENTITIES = 1024;
		constexpr float MOVED_UNITS = 0.5f;
		constexpr float MOVED_DEGREES = 0.5f;
		// what source's player movement collides with (MASK_PLAYERSOLID without monsters: no brush is one)
		constexpr int PLAYER_SOLID_BRUSHES = CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_PLAYERCLIP | CONTENTS_WINDOW | CONTENTS_GRATE;
		constexpr int DIGGABLE_BRUSHES = CONTENTS_SOLID | CONTENTS_WINDOW | CONTENTS_GRATE;  // what minecraft may dig (not player clips)
		// a brush side's material is what a trace through its middle, from this far out to this far in, hits, unless
		// that surface draws nothing
		constexpr int   UNSEEN_SURFACES = SURF_NODRAW | SURF_SKY | SURF_SKY2D;
		constexpr float SIDE_PROBE_UNITS = 1.0f;
		// a hit's plane facing this close to a side's way is that side
		constexpr float SAME_NORMAL = 0.99f;
		// a brush side whose normal points down at least this much is seen from under the brush
		constexpr float FACING_DOWN = -0.7f;

		/// the map's sky brushes, by brush index: the ones whose sides facing down are sky (toolsskybox),
		/// so what's under them is out in the open. a roof with sky only on top stays a roof.
		std::vector<bool> read_sky_brushes(const char* map_name)
		{
			const std::string         path = "maps/" + map_base_name(map_name) + ".bsp";
			bsp::BspFile              file(path.c_str());
			std::vector<dbrush_t>     brushes;
			std::vector<dbrushside_t> sides;
			std::vector<dplane_t>     planes;
			std::vector<texinfo_t>    texinfos;
			if (!file.open() || !file.lump(LUMP_BRUSHES, brushes) || !file.lump(LUMP_BRUSHSIDES, sides) || !file.lump(LUMP_PLANES, planes) ||
				!file.lump(LUMP_TEXINFO, texinfos)) {
				log_warning("sky: can't read %s: half-life's sky counts as a roof there", path.c_str());
				return {};
			}
			std::vector<bool> sky(brushes.size(), false);
			int               count = 0;
			for (std::size_t b = 0; b < brushes.size(); ++b) {
				const dbrush_t& brush = brushes[b];
				bool            seen_from_under = false, all_sky = true;
				for (int s = brush.firstside; s < brush.firstside + brush.numsides && all_sky; ++s) {
					if (s < 0 || static_cast<std::size_t>(s) >= sides.size()) {
						all_sky = false;
						break;
					}
					const dbrushside_t& side = sides[s];
					if (side.bevel || side.planenum >= planes.size() || planes[side.planenum].normal.z > FACING_DOWN) {
						continue;
					}
					seen_from_under = true;
					all_sky = side.texinfo >= 0 && static_cast<std::size_t>(side.texinfo) < texinfos.size() &&
						(texinfos[side.texinfo].flags & (SURF_SKY | SURF_SKY2D));
				}
				if (seen_from_under && all_sky) {
					sky[b] = true;
					++count;
				}
			}
			// enginetrace numbers brushes as the file does; if it doesn't, the flags would land on other
			// brushes: better none
			CUtlVector<Vector4D> brush_planes;
			for (std::size_t b = 0; b < brushes.size(); ++b) {
				int contents = 0;
				brush_planes.RemoveAll();
				if (!enginetrace->GetBrushInfo(static_cast<int>(b), &brush_planes, &contents) || contents != brushes[b].contents) {
					log_warning("sky: brush %zu of %s isn't the engine's (contents %d, the engine's %d): half-life's sky counts as a roof there",
						b, path.c_str(), brushes[b].contents, contents);
					return {};
				}
			}
			log_info("sky: %d of %s's %zu brushes are its sky", count, path.c_str(), brushes.size());
			return sky;
		}

		/// a brush side's dig material: what a trace through its centre hits, the surfaceprop's game material (what bullets
		/// and footsteps get there).
		/// @param centre - a point on the side, source coords
		/// @param normal - the side's, out of the brush
		/// @return DIG_NONE: no clean hit (the trace starts in a brush, misses, hits another plane or a surface that draws
		///         nothing, or the surface has no surfaceprop)
		std::uint8_t side_material(const Vector& centre, const Vector& normal)
		{
			Ray_t ray;
			ray.Init(centre + normal * SIDE_PROBE_UNITS, centre - normal * SIDE_PROBE_UNITS);
			CTraceFilterWorldOnly filter;
			trace_t               tr;
			enginetrace->TraceRay(ray, DIGGABLE_BRUSHES, &filter, &tr);
			if (tr.startsolid || tr.fraction >= 1.0f || DotProduct(tr.plane.normal, normal) < SAME_NORMAL || (tr.surface.flags & UNSEEN_SURFACES) != 0 ||
				tr.surface.surfaceProps < 0) {
				return DIG_NONE;
			}
			const surfacedata_t* data = physprops->GetSurfaceData(tr.surface.surfaceProps);
			return data ? dig_material(static_cast<char>(data->game.material)) : DIG_NONE;
		}

		// read_bsp_tree's records are the file's (bspfile.h)
		static_assert(sizeof(dplane_t) == BSP_PLANE_BYTES && sizeof(dnode_t) == BSP_NODE_BYTES);
		static_assert(sizeof(dleaf_t) == BSP_LEAF_BYTES_V1 && sizeof(dleaf_version_0_t) == BSP_LEAF_BYTES_V0);

		/// the map's bsp tree, what the streamer's worker finds the void behind its walls with (core/hc_dig_void.h).
		/// logged with how often the engine's own contents agree with it (only the log's: the void is the tree's).
		/// @return nullptr: the file's tree didn't read
		std::shared_ptr<const BspTree> read_void_tree(const char* map_name)
		{
			const std::string         path = "maps/" + map_base_name(map_name) + ".bsp";
			bsp::BspFile              file(path.c_str());
			std::vector<std::uint8_t> planes, nodes, leaves;
			std::vector<dmodel_t>     models;
			int                       version = 0, leaf_version = 0;
			auto                      tree = std::make_shared<BspTree>();
			if (!file.open() || !file.lump_bytes(LUMP_PLANES, planes, version) || !file.lump_bytes(LUMP_NODES, nodes, version) ||
				!file.lump_bytes(LUMP_LEAFS, leaves, leaf_version) || !file.lump(LUMP_MODELS, models) || models.empty() ||
				!read_bsp_tree(planes, nodes, leaves, leaf_version, models[0].headnode, *tree)) {
				log_warning("void: can't read %s's tree: nothing behind its walls is rock there", path.c_str());
				return nullptr;
			}
			std::size_t solid = 0;
			for (const int contents : tree->leaf_contents) {
				solid += (contents & BSP_CONTENTS_SOLID) != 0 ? 1 : 0;
			}

			// the centres of an 8x8x8 grid over the world and 64 units around it: solid in the tree, and in the engine,
			// asked about the world's collideable alone, or, when that gives no solid anywhere, for its point contents
			// (the world and what's in it)
			constexpr int   GRID = 8, POINTS = GRID * GRID * GRID;
			constexpr float MARGIN = 64.0f;
			const dmodel_t& world_model = models[0];
			const auto      point = [&](int i) {
				const int cell[3] = { i % GRID, i / GRID % GRID, i / (GRID * GRID) };
				Vector    p;
				for (int k = 0; k < 3; ++k) {
					const float lo = world_model.mins[k] - MARGIN, hi = world_model.maxs[k] + MARGIN;
					p[k] = lo + (static_cast<float>(cell[k]) + 0.5f) * (hi - lo) / GRID;
				}
				return p;
			};
			CWorld*                  world = GetWorldEntity();
			ICollideable*            collideable = world ? world->GetCollideable() : nullptr;
			std::array<bool, POINTS> tree_solid{}, engine_solid{};
			bool                     any_solid = false;
			for (int i = 0; i < POINTS; ++i) {
				const Vector p = point(i);
				const float  at[3] = { p.x, p.y, p.z };
				tree_solid[i] = (point_contents(*tree, at) & BSP_CONTENTS_SOLID) != 0;
				engine_solid[i] = collideable && (enginetrace->GetPointContents_Collideable(collideable, p) & CONTENTS_SOLID) != 0;
				any_solid = any_solid || engine_solid[i];
			}
			const char* call = "world collideable";
			if (!any_solid) {
				call = "point contents";
				for (int i = 0; i < POINTS; ++i) {
					engine_solid[i] = (enginetrace->GetPointContents(point(i)) & CONTENTS_SOLID) != 0;
				}
			}
			int agree = 0;
			for (int i = 0; i < POINTS; ++i) {
				agree += tree_solid[i] == engine_solid[i] ? 1 : 0;
			}
			log_info("void: %s's tree: %zu nodes, %zu leaves (%zu solid); the engine (%s) agrees at %d of %d points", path.c_str(), tree->nodes.size(),
				tree->leaf_contents.size(), solid, call, agree, POINTS);
			return tree;
		}

		float slot_offset(MapSlot slot)
		{
			return static_cast<float>(slot.x_blocks());
		}

		void to_mc(const Vector& source, MapSlot slot, float out[3])
		{
			const float p[3] = { source.x, source.y, source.z };
			const auto  mc = source_to_mc(p, slot);
			out[0] = static_cast<float>(mc.x);
			out[1] = static_cast<float>(mc.y);
			out[2] = static_cast<float>(mc.z);
		}

		/// a convex piece given as triangles (minecraft space) -> its planes and bounds.
		bool hull_to_convex(const std::vector<std::array<float, 3>>& corners, std::uint32_t flags, ColPrimitives::Convex& out)
		{
			if (corners.size() < 12) {
				return false;  // fewer than four triangles has no volume
			}
			float centre[3] = { 0.0f, 0.0f, 0.0f };
			out.lo[0] = out.lo[1] = out.lo[2] = FLT_MAX;
			out.hi[0] = out.hi[1] = out.hi[2] = -FLT_MAX;
			for (const auto& c : corners) {
				for (int k = 0; k < 3; ++k) {
					centre[k] += c[k];
					out.lo[k] = std::min(out.lo[k], c[k]);
					out.hi[k] = std::max(out.hi[k], c[k]);
				}
			}
			for (float& k : centre) {
				k /= static_cast<float>(corners.size());
			}
			out.planes.clear();
			for (std::size_t i = 0; i + 2 < corners.size(); i += 3) {
				const auto& a = corners[i];
				const auto& b = corners[i + 1];
				const auto& c = corners[i + 2];
				const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
				const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
				float       n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
				const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (len < 1e-7f) {
					continue;
				}
				for (float& k : n) {
					k /= len;
				}
				float d = -(n[0] * a[0] + n[1] * a[1] + n[2] * a[2]);
				if (n[0] * centre[0] + n[1] * centre[1] + n[2] * centre[2] + d > 0.0f) {
					for (float& k : n) {
						k = -k;
					}
					d = -d;
				}
				const bool duplicate = std::any_of(out.planes.begin(), out.planes.end(), [&](const std::array<float, 4>& p) {
					return p[0] * n[0] + p[1] * n[1] + p[2] * n[2] > 0.9999f && std::fabs(p[3] - d) < 1e-4f;
				});
				if (!duplicate) {
					out.planes.push_back({ n[0], n[1], n[2], d });
				}
			}
			out.flags = flags;
			return out.planes.size() >= 4;
		}

		bool moved(const Vector& a, const Vector& b, float tolerance)
		{
			return std::fabs(a.x - b.x) > tolerance || std::fabs(a.y - b.y) > tolerance || std::fabs(a.z - b.z) > tolerance;
		}

		bool turned(const QAngle& a, const QAngle& b)
		{
			return std::fabs(a.x - b.x) > MOVED_DEGREES || std::fabs(a.y - b.y) > MOVED_DEGREES || std::fabs(a.z - b.z) > MOVED_DEGREES;
		}

		/// solid things the player collides with that minecraft should see as walls: what source's player movement
		/// traces (MASK_PLAYERSOLID, CTraceFilterSimple with the player and COLLISION_GROUP_PLAYER_MOVEMENT) hit.
		/// FSOLID_NOT_STANDABLE stays a wall: the player's movement stands on it all the same.
		bool is_wall(CBaseEntity* entity)
		{
			if (!entity || entity->IsPlayer() || entity->MyNPCPointer() || entity->IsEFlagSet(EFL_KILLME)) {
				return false;
			}
			if (FClassnameIs(entity, BLOCKS_CLASSNAME)) {
				return false;  // minecraft's own blocks: it has them already
			}
			if (!entity->IsSolid() || entity->IsSolidFlagSet(FSOLID_NOT_SOLID)) {
				return false;
			}
			// what the player carries floats in front of them: minecraft would walk into it
			if (IPhysicsObject* body = entity->VPhysicsGetObject(); body && (body->GetGameFlags() & FVPHYSICS_PLAYER_HELD)) {
				return false;
			}
			// PassServerEntityFilter: the player goes through what it owns (its grenades, its rockets) and its owner
			if (CBasePlayer* player = singleplayer_player(); player && (entity->GetOwnerEntity() == player || player->GetOwnerEntity() == entity)) {
				return false;
			}
			// enginetrace skips a studio model whose contents the mask hasn't
			if (const model_t* model = entity->GetModel(); model && modelinfo->GetModelType(model) == mod_studio) {
				const studiohdr_t* studio = modelinfo->GetStudiomodel(model);
				if (studio && !(studio->contents & MASK_PLAYERSOLID)) {
					return false;
				}
			}
			// the entity's own say (debris, a choreo vehicle that ignores the player), then the game rules' (passable doors)
			return entity->ShouldCollide(COLLISION_GROUP_PLAYER_MOVEMENT, MASK_PLAYERSOLID) &&
				g_pGameRules->ShouldCollide(COLLISION_GROUP_PLAYER_MOVEMENT, entity->GetCollisionGroup());
		}
	}

	namespace
	{
		/// what gather() makes geometry of: the world, static props and walls (is_wall)
		class GatheredFilter final : public CTraceFilter
		{
		public:
			bool ShouldHitEntity(IHandleEntity* handle, int) override
			{
				if (staticpropmgr->IsStaticProp(handle)) {
					return true;
				}
				CBaseEntity* entity = EntityFromEntityHandle(handle);
				return entity && (entity->IsWorld() || is_wall(entity));
			}
		};
	}

	void WorldCollision::check_voxels(const McVec& centre, int radius)
	{
		constexpr int   R = COLLISION_REGION_SIZE;
		constexpr int   G = REGION_VOXELS;
		constexpr float REACH = 5.0f;  // half a voxel and the voxelizer's margin around a primitive (units)
		struct Block
		{
			int  x, y, z;
			int  phantoms;
			bool full_layer;  // a whole 8x8 layer of it has nothing of source's near it
		};
		const int          px = static_cast<int>(std::floor(centre.x / R)), py = static_cast<int>(std::floor(centre.y / R));
		const int          pz = static_cast<int>(std::floor(centre.z / R));
		GatheredFilter     filter;
		std::vector<Block> blocks;
		long long          voxels = 0, phantoms = 0;
		long long          in_holes = 0;  // voxels in dug cells: source still has its walls there
		const bool         holes = dig_ && dig_->count() > 0;
		for (int rx = px - radius; rx <= px + radius; ++rx) {
			for (int ry = py - 1; ry <= py + 1; ++ry) {
				for (int rz = pz - radius; rz <= pz + radius; ++rz) {
					const float   lo[3] = { float(rx * R) - 0.5f, float(ry * R) - 0.5f, float(rz * R) - 0.5f };
					const float   hi[3] = { lo[0] + R + 1.0f, lo[1] + R + 1.0f, lo[2] + R + 1.0f };
					ColPrimitives prims;
					gather(lo, hi, prims);
					RegionVoxels solid;
					voxelize_region(prims, rx, ry, rz, solid);
					int counts[R][R][R] = {};
					int layers[R][R][R][8] = {};
					for (int y = 0; y < G; ++y) {
						for (int z = 0; z < G; ++z) {
							const std::uint64_t row = solid[y * G + z];
							for (int x = 0; x < G && row >> x; ++x) {
								if (!(row >> x & 1)) {
									continue;
								}
								if (holes && dig_->dug(rx * R + x / 8, ry * R + y / 8, rz * R + z / 8)) {
									++in_holes;
									continue;
								}
								++voxels;
								float src[3];
								mc_to_source(rx * R + (x + 0.5) / 8.0, ry * R + (y + 0.5) / 8.0, rz * R + (z + 0.5) / 8.0, slot_, src);
								const Vector p(src[0], src[1], src[2]);
								Ray_t        ray;
								ray.Init(p - Vector(0, 0, 0.5f), p + Vector(0, 0, 0.5f), Vector(-REACH, -REACH, -REACH), Vector(REACH, REACH, REACH));
								trace_t tr;
								enginetrace->TraceRay(ray, PLAYER_SOLID_BRUSHES, &filter, &tr);
								// a displacement is one-sided to a box already in it: come at it from above and from below too
								bool near = tr.startsolid || tr.fraction < 1.0f;
								for (float dir = -1.0f; dir <= 1.0f && !near; dir += 2.0f) {
									Ray_t sweep;
									sweep.Init(p - Vector(0, 0, dir * 2.0f * REACH), p + Vector(0, 0, dir * 2.0f * REACH), Vector(-REACH, -REACH, -1.0f), Vector(REACH, REACH, 1.0f));
									enginetrace->TraceRay(sweep, PLAYER_SOLID_BRUSHES, &filter, &tr);
									near = tr.startsolid || (tr.fraction < 1.0f && std::fabs(tr.endpos.z - p.z) <= REACH + 1.0f);
								}
								if (!near) {
									++phantoms;
									++counts[x / 8][y / 8][z / 8];
									++layers[x / 8][y / 8][z / 8][y % 8];
								}
							}
						}
					}
					for (int bx = 0; bx < R; ++bx) {
						for (int by = 0; by < R; ++by) {
							for (int bz = 0; bz < R; ++bz) {
								if (counts[bx][by][bz]) {
									bool full = false;
									for (int l = 0; l < 8; ++l) {
										full |= layers[bx][by][bz][l] == 64;
									}
									blocks.push_back({ rx * R + bx, ry * R + by, rz * R + bz, counts[bx][by][bz], full });
								}
							}
						}
					}
				}
			}
		}
		std::sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.phantoms > b.phantoms; });
		log_info("hc_debug_voxels: %lld voxels, %lld with nothing of source's near them, in %zu blocks", voxels, phantoms, blocks.size());
		if (holes) {
			log_info("hc_debug_voxels: %lld voxels inside dug cells (brushes leave none; props and displacements aren't cut)", in_holes);
		}
		for (std::size_t i = 0; i < blocks.size() && i < 20; ++i) {
			const Block& b = blocks[i];
			float        src[3];
			mc_to_source(b.x + 0.5, b.y + 0.5, b.z + 0.5, slot_, src);
			log_info("hc_debug_voxels: block %d %d %d (source %.0f %.0f %.0f): %d phantom voxels%s", b.x, b.y, b.z, src[0], src[1], src[2], b.phantoms,
				b.full_layer ? ", whole 8x8 layers of it" : "");
		}
	}

	void WorldCollision::reset(MapSlot slot, const char* map_name)
	{
		slot_ = slot;
		hulls_.clear();
		movers_.clear();
		sky_brushes_ = map_name ? read_sky_brushes(map_name) : std::vector<bool>();
		brush_dig_.clear();
		ground_dig_.clear();
		bsp_ = map_name ? read_void_tree(map_name) : nullptr;
		cut_pieces_.clear();
	}

	VoidQuery WorldCollision::void_query() const
	{
		return { bsp_, slot_ };
	}

	void WorldCollision::to_source_box(const float lo[3], const float hi[3], Vector& mins, Vector& maxs) const
	{
		const float units = static_cast<float>(UNITS_PER_BLOCK);
		const float offset = slot_offset(slot_);
		mins.Init((lo[0] - offset) * units, -hi[2] * units, lo[1] * units + slot_.grid_z);
		maxs.Init((hi[0] - offset) * units, -lo[2] * units, hi[1] * units + slot_.grid_z);
	}

	void WorldCollision::to_mc_box(const Vector& mins, const Vector& maxs, float lo[3], float hi[3]) const
	{
		const float units = static_cast<float>(UNITS_PER_BLOCK);
		const float offset = slot_offset(slot_);
		lo[0] = mins.x / units + offset;
		hi[0] = maxs.x / units + offset;
		lo[1] = (mins.z - slot_.grid_z) / units;
		hi[1] = (maxs.z - slot_.grid_z) / units;
		lo[2] = -maxs.y / units;
		hi[2] = -mins.y / units;
	}

	void WorldCollision::gather(const float lo[3], const float hi[3], ColPrimitives& out)
	{
		Vector mins, maxs;
		to_source_box(lo, hi, mins, maxs);
		// the region it's for (the streamer gathers half a block around one): its cut pieces are these from now on
		int region[3];
		for (int k = 0; k < 3; ++k) {
			region[k] = static_cast<int>(std::floor((lo[k] + 0.5f) / COLLISION_REGION_SIZE));
		}
		const std::uint64_t key = region_key(region[0], region[1], region[2]);
		std::vector<DigBox> cut;
		add_brushes(mins, maxs, out, cut);
		if (cut.empty()) {
			cut_pieces_.erase(key);
		} else {
			cut_pieces_[key] = std::move(cut);
		}
		add_displacements(mins, maxs, out);
		add_static_props(mins, maxs, out);
		add_entities(mins, maxs, out);
	}

	/// @param cut - the bounds of the pieces brushes with dug cells in them were cut into, appended
	void WorldCollision::add_brushes(const Vector& mins, const Vector& maxs, ColPrimitives& out, std::vector<DigBox>& cut)
	{
		CUtlVector<int> brushes;
		enginetrace->GetBrushesInAABB(mins, maxs, &brushes, PLAYER_SOLID_BRUSHES);
		CUtlVector<Vector4D>               planes;
		std::vector<std::array<float, 4>>  brush;
		const float                        box_mins[3] = { mins.x, mins.y, mins.z }, box_maxs[3] = { maxs.x, maxs.y, maxs.z };
		const bool                         digging = dig_ && dig_->count() > 0;
		std::vector<DigBox>                boxes;
		std::vector<ColPrimitives::Convex> pieces;
		for (int i = 0; i < brushes.Count(); ++i) {
			planes.RemoveAll();
			int contents = 0;
			if (!enginetrace->GetBrushInfo(brushes[i], &planes, &contents) || !(contents & PLAYER_SOLID_BRUSHES) || planes.Count() < 4) {
				continue;
			}
			brush.clear();
			for (int p = 0; p < planes.Count(); ++p) {
				brush.push_back({ planes[p].x, planes[p].y, planes[p].z, planes[p].w });
			}
			ColPrimitives::Convex cvx;
			brush_to_convex(brush.data(), brush.size(), box_mins, box_maxs, slot_, cvx);
			if (sky_brush(brushes[i])) {
				cvx.flags |= PRIM_SKY;
			} else if (diggable(brushes[i], contents)) {
				// each face digs as its side does
				const BrushDig& dig = brush_dig(brushes[i], planes);
				cvx.flags |= PRIM_DIGGABLE;
				for (std::size_t p = 0; p < cvx.planes.size(); ++p) {
					cvx.plane_flags.push_back(dig_flags(dig.sides.size() == cvx.planes.size() ? dig.sides[p] : dig.material));
				}
			}
			// the cells minecraft dug in it (all but the sky's: player clips and moveable brushes too) come out of it
			boxes.clear();
			if (digging && !(cvx.flags & PRIM_SKY)) {
				dig_->boxes_in(cvx.lo, cvx.hi, boxes);
			}
			if (boxes.empty()) {
				out.convexes.push_back(std::move(cvx));
				continue;
			}
			// the faces of the hole's walls dig as the brush does; a clip's or a moveable brush's don't dig
			const std::uint32_t cut_flags = diggable(brushes[i], contents) ? dig_flags(brush_dig(brushes[i], planes).material) : 0u;
			pieces.clear();
			subtract_boxes(cvx, boxes, cut_flags, pieces);
			for (auto& piece : pieces) {
				cut.push_back({ { piece.lo[0], piece.lo[1], piece.lo[2] }, { piece.hi[0], piece.hi[1], piece.hi[2] } });
				out.convexes.push_back(std::move(piece));
			}
		}
	}

	void WorldCollision::cut_pieces_near(const McVec& p, double radius, std::vector<DigBox>& out) const
	{
		out.clear();
		const double at[3] = { p.x, p.y, p.z };
		for (const auto& region : cut_pieces_) {
			for (const DigBox& piece : region.second) {
				bool near = true;
				for (int k = 0; k < 3 && near; ++k) {
					near = piece.lo[k] <= at[k] + radius && piece.hi[k] >= at[k] - radius;
				}
				if (near) {
					out.push_back(piece);
				}
			}
		}
	}

	bool WorldCollision::sky_brush(int brush) const
	{
		return brush >= 0 && static_cast<std::size_t>(brush) < sky_brushes_.size() && sky_brushes_[brush];
	}

	bool WorldCollision::diggable(int brush, int contents) const
	{
		return (contents & DIGGABLE_BRUSHES) != 0 && !sky_brush(brush);
	}

	const WorldCollision::BrushDig& WorldCollision::brush_dig(int brush, const CUtlVector<Vector4D>& planes)
	{
		const auto it = brush_dig_.find(brush);
		if (it != brush_dig_.end()) {
			return it->second;
		}
		// the whole brush, whatever box it was gathered in: its sides' faces whole
		std::vector<std::array<float, 4>> brush_planes;
		brush_planes.reserve(static_cast<std::size_t>(planes.Count()));
		for (int p = 0; p < planes.Count(); ++p) {
			brush_planes.push_back({ planes[p].x, planes[p].y, planes[p].z, planes[p].w });
		}
		const float           world_mins[3] = { MIN_COORD_FLOAT, MIN_COORD_FLOAT, MIN_COORD_FLOAT };
		const float           world_maxs[3] = { MAX_COORD_FLOAT, MAX_COORD_FLOAT, MAX_COORD_FLOAT };
		ColPrimitives::Convex probe;
		brush_to_convex(brush_planes.data(), brush_planes.size(), world_mins, world_maxs, MapSlot{}, probe);

		// each side votes with what a trace through the middle of its face hits
		std::vector<std::uint8_t>         votes;
		std::vector<std::array<float, 3>> poly;
		for (int p = 0; p < planes.Count(); ++p) {
			std::uint8_t vote = DIG_NONE;
			if (convex_face(probe, static_cast<std::size_t>(p), poly)) {
				double middle[3] = { 0.0, 0.0, 0.0 };
				for (const auto& corner : poly) {
					for (int k = 0; k < 3; ++k) {
						middle[k] += corner[k];
					}
				}
				const double corners = static_cast<double>(poly.size());
				float        src[3];
				mc_to_source(middle[0] / corners, middle[1] / corners, middle[2] / corners, MapSlot{}, src);
				vote = side_material(Vector(src[0], src[1], src[2]), planes[p].AsVector3D());
			}
			votes.push_back(vote);
		}
		BrushDig& dig = brush_dig_[brush];
		dig.material = brush_material(votes);
		for (const std::uint8_t vote : votes) {
			dig.sides.push_back(vote != DIG_NONE ? vote : dig.material);
		}
		return dig;
	}

	void WorldCollision::log_dig_material(const Vector& eye, const Vector& forward)
	{
		constexpr float REACH_UNITS = 8192.0f;
		constexpr float INSIDE_UNITS = 0.5f;     // the hit point moved this far into the brush it's on
		constexpr float ON_PLANE_UNITS = 0.01f;  // how far out of a brush's plane that point may be and still be in it
		Ray_t           ray;
		ray.Init(eye, eye + forward * REACH_UNITS);
		CTraceFilterWorldOnly filter;
		trace_t               tr;
		enginetrace->TraceRay(ray, DIGGABLE_BRUSHES, &filter, &tr);

		// the brush hit: the first around the point a little inside it that has that point inside all its planes
		int                  brush = -1, contents = 0;
		CUtlVector<Vector4D> planes;
		if (tr.fraction < 1.0f) {
			const Vector    p = tr.endpos - tr.plane.normal * INSIDE_UNITS;
			const Vector    h(0.25f, 0.25f, 0.25f);
			CUtlVector<int> list;
			enginetrace->GetBrushesInAABB(p - h, p + h, &list, DIGGABLE_BRUSHES);
			for (int i = 0; i < list.Count() && brush < 0; ++i) {
				planes.RemoveAll();
				int  c = 0;
				bool inside = enginetrace->GetBrushInfo(list[i], &planes, &c) && (c & DIGGABLE_BRUSHES) != 0;
				for (int k = 0; k < planes.Count() && inside; ++k) {
					inside = DotProduct(planes[k].AsVector3D(), p) - planes[k].w <= ON_PLANE_UNITS;
				}
				if (inside) {
					brush = list[i];
					contents = c;
				}
			}
		}
		if (brush < 0) {
			log_info("hc_debug_dig_material: no world brush under the crosshair");
			return;
		}
		if (!diggable(brush, contents)) {
			log_info("hc_debug_dig_material: brush %d isn't diggable (contents 0x%x%s)", brush, static_cast<unsigned>(contents), sky_brush(brush) ? ", sky" : "");
			return;
		}

		// the side hit: of the planes facing the way the hit's surface does, the one the hit lies closest to
		int   side = -1;
		float closest = FLT_MAX;
		for (int k = 0; k < planes.Count(); ++k) {
			const Vector& n = planes[k].AsVector3D();
			const float   off = std::fabs(DotProduct(n, tr.endpos) - planes[k].w);
			if (DotProduct(n, tr.plane.normal) >= SAME_NORMAL && off < closest) {
				side = k;
				closest = off;
			}
		}
		const BrushDig&      dig = brush_dig(brush, planes);
		const int            props = tr.surface.surfaceProps;
		const surfacedata_t* data = props >= 0 ? physprops->GetSurfaceData(props) : nullptr;
		const char*          prop = props >= 0 ? physprops->GetPropName(props) : nullptr;
		// a game material of 0 would end the line there
		const char game_material = data && data->game.material != 0 ? static_cast<char>(data->game.material) : '?';
		const int  side_dig = side >= 0 && static_cast<std::size_t>(side) < dig.sides.size() ? dig.sides[side] : 0;
		log_info("hc_debug_dig_material: brush %d side %d of %d: %s (%s, '%c') -> side %d, brush %d", brush, side, planes.Count(),
			tr.surface.name ? tr.surface.name : "?", prop ? prop : "?", game_material, side_dig, dig.material);
	}

	std::uint8_t WorldCollision::ground_material(const Vector verts[3], std::uint8_t fallback)
	{
		const Vector centre = (verts[0] + verts[1] + verts[2]) / 3.0f;
		// a quarter of a unit apart: two triangles' centres never are that close
		const auto          q = [](float v) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(v * 4.0f)) & 0x1FFFFF); };
		const std::uint64_t key = q(centre.x) | (q(centre.y) << 21) | (q(centre.z) << 42);
		const auto          found = ground_dig_.find(key);
		if (found != ground_dig_.end()) {
			return found->second;
		}
		Vector       normal = CrossProduct(verts[1] - verts[0], verts[2] - verts[0]);
		std::uint8_t material = fallback;
		if (VectorNormalize(normal) > 0.0f) {
			// through its middle, from either side: whichever way its winding faces
			for (const float way : { 1.0f, -1.0f }) {
				Ray_t ray;
				ray.Init(centre + normal * (way * SIDE_PROBE_UNITS), centre - normal * (way * SIDE_PROBE_UNITS));
				CTraceFilterWorldOnly filter;
				trace_t               tr;
				enginetrace->TraceRay(ray, MASK_SOLID_BRUSHONLY, &filter, &tr);
				if (tr.startsolid || tr.fraction >= 1.0f || !(tr.dispFlags & DISPSURF_FLAG_SURFACE) || tr.surface.surfaceProps < 0) {
					continue;
				}
				const surfacedata_t* data = physprops->GetSurfaceData(tr.surface.surfaceProps);
				if (data && data->game.material != 0) {
					material = dig_material(static_cast<char>(data->game.material));
				}
				break;
			}
		}
		ground_dig_.emplace(key, material);
		return material;
	}

	void WorldCollision::add_displacements(const Vector& mins, const Vector& maxs, ColPrimitives& out)
	{
		// the ground over the box too, as far as the rock under it reaches into the box (core/hc_dig_ground.h)
		const Vector  above(maxs.x, maxs.y, maxs.z + GROUND_DEPTH * static_cast<float>(UNITS_PER_BLOCK));
		CPhysCollide* collide = enginetrace->GetCollidableFromDisplacementsInAABB(mins, above);
		if (!collide) {
			return;
		}
		float lo[3], hi[3];
		to_mc_box(mins, maxs, lo, hi);
		const bool                        digging = dig_ && dig_->count() > 0;
		std::vector<DigBox>               boxes;
		std::vector<std::array<float, 9>> pieces;
		ICollisionQuery*                  query = physcollision->CreateQueryModel(collide);
		for (int c = 0; c < query->ConvexCount(); ++c) {
			for (int t = 0; t < query->TriangleCount(c); ++t) {
				Vector verts[3];
				query->GetTriangleVerts(c, t, verts);
				ColPrimitives::Tri tri;
				float              tlo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, thi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
				for (int v = 0; v < 3; ++v) {
					to_mc(verts[v], slot_, tri.v + v * 3);
					for (int k = 0; k < 3; ++k) {
						tlo[k] = std::min(tlo[k], tri.v[v * 3 + k]);
						thi[k] = std::max(thi[k], tri.v[v * 3 + k]);
					}
				}
				// the ground digs as its surface does: dirt, sand, grass (dirt), a rock face's concrete (stone)
				const surfacedata_t* data = physprops->GetSurfaceData(query->GetTriangleMaterialIndex(c, t));
				tri.flags = dig_flags(ground_material(verts, data && data->game.material != 0 ? dig_material(static_cast<char>(data->game.material)) : DIG_DIRT));
				out.ground.push_back(tri);
				bool inside = true;
				for (int k = 0; k < 3; ++k) {
					inside = inside && thi[k] >= lo[k] && tlo[k] <= hi[k];
				}
				if (!inside) {
					continue;  // over the box: only the rock under it reaches in
				}
				// the cells dug through it come out of it, so a hole in the ground opens
				boxes.clear();
				if (digging) {
					dig_->boxes_in(tlo, thi, boxes);
				}
				if (boxes.empty()) {
					out.tris.push_back(tri);
					continue;
				}
				pieces.clear();
				subtract_boxes_from_tri(tri.v, boxes, pieces);
				for (const auto& piece : pieces) {
					ColPrimitives::Tri cut;
					std::copy(piece.begin(), piece.end(), cut.v);
					cut.flags = tri.flags;
					out.tris.push_back(cut);
				}
			}
		}
		physcollision->DestroyQueryModel(query);
		physcollision->DestroyCollide(collide);
	}

	void WorldCollision::add_static_props(const Vector& mins, const Vector& maxs, ColPrimitives& out)
	{
		CUtlVector<ICollideable*> props;
		staticpropmgr->GetAllStaticPropsInAABB(mins, maxs, &props);
		for (int i = 0; i < props.Count(); ++i) {
			ICollideable* prop = props[i];
			switch (prop->GetSolid()) {
			case SOLID_VPHYSICS: {
				const auto& solids = model_hulls(prop->GetCollisionModel());
				if (!solids.empty()) {
					for (const auto& hulls : solids) {
						add_hulls(hulls, prop->CollisionToWorldTransform(), 0, out);
					}
					break;
				}
				[[fallthrough]];
			}
			case SOLID_BBOX:
			case SOLID_OBB:
			case SOLID_OBB_YAW:
				add_box(prop->OBBMins(), prop->OBBMaxs(), prop->CollisionToWorldTransform(), out);
				break;
			default:
				break;
			}
		}
	}

	void WorldCollision::add_entities(const Vector& mins, const Vector& maxs, ColPrimitives& out)
	{
		CBaseEntity* list[MAX_ENTITIES];
		const int    count = UTIL_EntitiesInBox(list, MAX_ENTITIES, mins, maxs, 0);
		for (int i = 0; i < count; ++i) {
			if (is_wall(list[i])) {
				add_entity(list[i], out);
			}
		}
	}

	void WorldCollision::add_entity(CBaseEntity* entity, ColPrimitives& out)
	{
		// enginetrace hands these the player's swept box (TestCollision): the sdk's that the player can walk into trace
		// their vphysics objects (a bone follower its own solid of the model, a prop door, a ragdoll each piece); the
		// ones that trace nothing for it have none (a bullet shield, a commentary node)
		if (entity->GetSolid() == SOLID_CUSTOM || entity->IsSolidFlagSet(FSOLID_CUSTOMBOXTEST)) {
			add_vphysics(entity, out);
			return;
		}
		switch (entity->GetSolid()) {
		case SOLID_BSP:
		case SOLID_VPHYSICS: {
			// enginetrace traces the model's first solid (a brush model's only one) where the collision property puts it,
			// only a brush model's brushes the mask has (model_hulls). a model without one has nothing to hit
			const auto& solids = model_hulls(entity->GetModel());
			if (!solids.empty()) {
				add_hulls(solids[0], entity->CollisionProp()->CollisionToWorldTransform(), 0, out);
			}
			return;
		}
		case SOLID_BBOX: {
			matrix3x4_t translation;
			AngleMatrix(vec3_angle, entity->GetAbsOrigin(), translation);
			add_box(entity->WorldAlignMins(), entity->WorldAlignMaxs(), translation, out);
			return;
		}
		default:
			break;
		}
		add_box(entity->CollisionProp()->OBBMins(), entity->CollisionProp()->OBBMaxs(), entity->CollisionProp()->CollisionToWorldTransform(), out);
	}

	void WorldCollision::add_vphysics(CBaseEntity* entity, ColPrimitives& out)
	{
		IPhysicsObject* objects[VPHYSICS_MAX_OBJECT_LIST_COUNT];
		// a ragdoll counts all its pieces, also the ones that didn't fit
		const int         count = std::min(entity->VPhysicsGetObjectList(objects, VPHYSICS_MAX_OBJECT_LIST_COUNT), VPHYSICS_MAX_OBJECT_LIST_COUNT);
		const model_t*    model = entity->GetModel();
		const vcollide_t* collide = model ? modelinfo->GetVCollide(model) : nullptr;
		const auto&       solids = model_hulls(model);
		std::vector<Hull> own;
		for (int i = 0; i < count; ++i) {
			const CPhysCollide* solid = objects[i] ? objects[i]->GetCollide() : nullptr;
			if (!solid) {
				continue;
			}
			// each where vphysics has it (a bone follower and a prop door follow their entity there)
			matrix3x4_t to_world;
			objects[i]->GetPositionMatrix(&to_world);
			// the model's solid it was made from, cached with the model; else its own (a scaled one's, a block prop's)
			const std::vector<Hull>* hulls = nullptr;
			for (int s = 0; collide && s < collide->solidCount && static_cast<std::size_t>(s) < solids.size() && !hulls; ++s) {
				hulls = collide->solids[s] == solid ? &solids[static_cast<std::size_t>(s)] : nullptr;
			}
			if (!hulls) {
				own.clear();
				solid_hulls(solid, false, own);
				hulls = &own;
			}
			add_hulls(*hulls, to_world, 0, out);
		}
	}

	const std::vector<std::vector<WorldCollision::Hull>>& WorldCollision::model_hulls(const model_t* model)
	{
		const auto it = hulls_.find(model);
		if (it != hulls_.end()) {
			return it->second;
		}
		auto&       solids = hulls_[model];
		vcollide_t* collide = model ? modelinfo->GetVCollide(model) : nullptr;
		if (!collide) {
			return solids;
		}
		const bool brush = modelinfo->GetModelType(model) == mod_brush;
		for (int s = 0; s < collide->solidCount; ++s) {
			solids.push_back({});
			solid_hulls(collide->solids[s], brush, solids.back());
		}
		return solids;
	}

	void WorldCollision::solid_hulls(const CPhysCollide* solid, bool brush, std::vector<Hull>& out)
	{
		ICollisionQuery*     query = physcollision->CreateQueryModel(const_cast<CPhysCollide*>(solid));  // it only reads it
		CUtlVector<Vector4D> planes;
		for (int c = 0; c < query->ConvexCount(); ++c) {
			// vbsp tags a brush model's pieces with their brushes (CPlaneList::AddBrushes), and enginetrace hits only
			// the pieces whose brush's contents the mask has: a monster clip isn't the player's, nor is water
			int contents = 0;
			planes.RemoveAll();
			if (brush && enginetrace->GetBrushInfo(static_cast<int>(query->GetGameData(c)), &planes, &contents) && !(contents & PLAYER_SOLID_BRUSHES)) {
				continue;
			}
			Hull hull;
			for (int t = 0; t < query->TriangleCount(c); ++t) {
				Vector verts[3];
				query->GetTriangleVerts(c, t, verts);
				for (const Vector& v : verts) {
					hull.push_back(v);
				}
			}
			if (!hull.empty()) {
				out.push_back(std::move(hull));
			}
		}
		physcollision->DestroyQueryModel(query);
	}

	void WorldCollision::add_hulls(const std::vector<Hull>& hulls, const matrix3x4_t& to_world, std::uint32_t flags, ColPrimitives& out) const
	{
		std::vector<std::array<float, 3>> corners;
		for (const auto& hull : hulls) {
			corners.clear();
			for (const auto& v : hull) {
				Vector world;
				VectorTransform(v, to_world, world);
				std::array<float, 3> mc;
				to_mc(world, slot_, mc.data());
				corners.push_back(mc);
			}
			ColPrimitives::Convex cvx;
			if (hull_to_convex(corners, flags, cvx)) {
				out.convexes.push_back(std::move(cvx));
			}
		}
	}

	void WorldCollision::add_box(const Vector& mins, const Vector& maxs, const matrix3x4_t& to_world, ColPrimitives& out) const
	{
		// the box as a hull of 12 triangles
		const Vector c[8] = {
			Vector(mins.x, mins.y, mins.z),
			Vector(maxs.x, mins.y, mins.z),
			Vector(maxs.x, maxs.y, mins.z),
			Vector(mins.x, maxs.y, mins.z),
			Vector(mins.x, mins.y, maxs.z),
			Vector(maxs.x, mins.y, maxs.z),
			Vector(maxs.x, maxs.y, maxs.z),
			Vector(mins.x, maxs.y, maxs.z),
		};
		static constexpr int FACES[6][4] = { { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
		Hull                 hull;
		for (const auto& f : FACES) {
			hull.insert(hull.end(), { c[f[0]], c[f[1]], c[f[2]], c[f[0]], c[f[2]], c[f[3]] });
		}
		add_hulls({ hull }, to_world, 0, out);
	}

	void WorldCollision::track_movers(const McVec& player, CollisionStreamer& streamer)
	{
		++frame_;
		// as far as collision is streamed (see hc_collision.cpp), and a region more
		constexpr float REACH_BLOCKS = (CollisionStreamer::RADIUS + 1) * CollisionStreamer::REGION_SIZE;
		const float     lo[3] = { static_cast<float>(player.x) - REACH_BLOCKS, static_cast<float>(player.y) - REACH_BLOCKS, static_cast<float>(player.z) - REACH_BLOCKS };
		const float     hi[3] = { static_cast<float>(player.x) + REACH_BLOCKS, static_cast<float>(player.y) + REACH_BLOCKS, static_cast<float>(player.z) + REACH_BLOCKS };
		Vector          mins, maxs;
		to_source_box(lo, hi, mins, maxs);

		CBaseEntity* list[MAX_ENTITIES];
		const int    count = UTIL_EntitiesInBox(list, MAX_ENTITIES, mins, maxs, 0);
		for (int i = 0; i < count; ++i) {
			CBaseEntity* entity = list[i];
			if (!is_wall(entity)) {
				continue;
			}
			Vector emins, emaxs;
			entity->CollisionProp()->WorldSpaceAABB(&emins, &emaxs);
			const int key = entity->GetRefEHandle().ToInt();
			auto      it = movers_.find(key);
			if (it == movers_.end()) {
				movers_[key] = { entity->GetAbsOrigin(), entity->GetAbsAngles(), emins, emaxs, frame_ };
				continue;
			}
			Mover& mover = it->second;
			if (moved(mover.origin, entity->GetAbsOrigin(), MOVED_UNITS) || turned(mover.angles, entity->GetAbsAngles())) {
				float old_lo[3], old_hi[3], new_lo[3], new_hi[3];
				to_mc_box(mover.mins, mover.maxs, old_lo, old_hi);
				to_mc_box(emins, emaxs, new_lo, new_hi);
				streamer.invalidate(old_lo, old_hi);
				streamer.invalidate(new_lo, new_hi);
				mover.origin = entity->GetAbsOrigin();
				mover.angles = entity->GetAbsAngles();
				mover.mins = emins;
				mover.maxs = emaxs;
			}
			mover.frame = frame_;
		}
		// whatever is gone (broken, removed, out of reach) leaves a hole where it was
		for (auto it = movers_.begin(); it != movers_.end();) {
			if (it->second.frame == frame_) {
				++it;
				continue;
			}
			float old_lo[3], old_hi[3];
			to_mc_box(it->second.mins, it->second.maxs, old_lo, old_hi);
			streamer.invalidate(old_lo, old_hi);
			it = movers_.erase(it);
		}
	}
}
