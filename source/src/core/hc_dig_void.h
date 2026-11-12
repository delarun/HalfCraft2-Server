#pragma once

// the void behind a map's walls: where minecraft makes rock of what lies outside the map (the kColVoid message), per
// block and per sub-voxel of a collision region. and the rock under the ground (core/hc_dig_ground.h): half-life's
// terrain is a surface, so what lies a few blocks under it is rock for minecraft the same way, solid until dug. pure code, no engine and no link, so the streamer's worker thread runs it.
//
// the leaves outside the map are solid in the .bsp's tree (vbsp's FillOutside), as are a brush's insides: the world's
// brushes and the sky's are told apart by their flags (PRIM_DIGGABLE, PRIM_SKY), so a point is void when its leaf is
// solid and it lies inside none of the convexes that have one. a cell dug out of a structural brush is still solid in
// the tree, but once cut out of the convexes (core/hc_dig_clip.h) it lies inside none of them: it reads as void.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hc_bsp_tree.h"
#include "hc_collision_shapes.h"
#include "hc_dig_ground.h"
#include "hc_units.h"

namespace halfcraft
{
	// proto::VoidKind (protocol/schema/collision.toml), what a block of a region holds
	inline constexpr std::uint8_t VOID_NONE = 0;  // no sub-voxel is void
	inline constexpr std::uint8_t VOID_ALL = 1;   // every one is
	inline constexpr std::uint8_t VOID_SOME = 2;  // some are: the block has a layers entry

	/// the void in one region: for each block (bx + 8bz + 64by), how much of it is void
	struct RegionVoid
	{
		std::array<std::uint8_t, 512> kind{};
		/// for each VOID_SOME block, in the order of kind: its sub-voxels, bit z * 8 + x of layer y
		std::vector<std::array<std::uint64_t, 8>> layers;

		/// @return whether any block has void in it
		bool any() const
		{
			for (const std::uint8_t k : kind) {
				if (k != VOID_NONE) {
					return true;
				}
			}
			return false;
		}
	};

	namespace void_detail
	{
		/// n.p + d <= 0 for all the planes, bounds first
		inline bool inside(const ColPrimitives::Convex& cvx, const float p[3])
		{
			for (int k = 0; k < 3; ++k) {
				if (p[k] < cvx.lo[k] || p[k] > cvx.hi[k]) {
					return false;
				}
			}
			for (const auto& plane : cvx.planes) {
				if (plane[0] * p[0] + plane[1] * p[1] + plane[2] * p[2] + plane[3] > 0.0f) {
					return false;
				}
			}
			return true;
		}
	}

	/// the void in region (rx, ry, rz) of the map in slot. a block is looked at in 9 points, its centre and its corners
	/// 1/16 of a block inside: all 9 void is VOID_ALL, none VOID_NONE, and a mixture is VOID_SOME, whose 512 sub-voxel
	/// centres are looked at.
	/// @param prims - the world's brushes (the convexes with PRIM_DIGGABLE or PRIM_SKY count)
	/// @param tree - the map's bsp tree
	/// @param out - replaced
	inline void void_region(const ColPrimitives& prims, const BspTree& tree, MapSlot slot, int rx, int ry, int rz, RegionVoid& out)
	{
		constexpr int N = COLLISION_REGION_SIZE;
		static_assert(N == 8, "RegionVoid's kind and layers are for blocks of 8x8x8 sub-voxels, 8x8x8 of them in a region");
		out.kind.fill(VOID_NONE);
		out.layers.clear();

		// the region in minecraft blocks, and the convexes that could hold a point of it: gathered once, they're what a solid
		// point is tested against
		const double                              origin[3] = { static_cast<double>(rx) * N, static_cast<double>(ry) * N, static_cast<double>(rz) * N };
		std::vector<const ColPrimitives::Convex*> walls;
		for (const ColPrimitives::Convex& cvx : prims.convexes) {
			if ((cvx.flags & (PRIM_DIGGABLE | PRIM_SKY)) == 0) {
				continue;
			}
			bool apart = false;
			for (int k = 0; k < 3; ++k) {
				apart = apart || cvx.hi[k] < origin[k] || cvx.lo[k] > origin[k] + N;
			}
			if (!apart) {
				walls.push_back(&cvx);
			}
		}

		const GroundColumns ground(origin, prims.ground);
		const bool          has_ground = !ground.empty();
		const auto          is_void = [&](double x, double y, double z) {
			if (has_ground && ground.under(x, y, z)) {
				return true;
			}
			float source[3];
			mc_to_source(x, y, z, slot, source);
			if ((point_contents(tree, source) & BSP_CONTENTS_SOLID) == 0) {
				return false;
			}
			const float mc[3] = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(z) };
			for (const ColPrimitives::Convex* cvx : walls) {
				if (void_detail::inside(*cvx, mc)) {
					return false;
				}
			}
			return true;
		};

		constexpr double NEAR_EDGE = 1.0 / 16.0, FAR_EDGE = 15.0 / 16.0;
		std::size_t      block = 0;  // bx + 8bz + 64by, which the loops count up
		for (int by = 0; by < N; ++by) {
			for (int bz = 0; bz < N; ++bz) {
				for (int bx = 0; bx < N; ++bx) {
					const double x = origin[0] + bx, y = origin[1] + by, z = origin[2] + bz;
					// the 9 points: a mixture of void and not (seen_other) is as far as they need looking at
					bool seen_void = is_void(x + 0.5, y + 0.5, z + 0.5), seen_other = !seen_void;
					for (int corner = 0; corner < 8 && !(seen_void && seen_other); ++corner) {
						if (is_void(x + ((corner & 1) != 0 ? FAR_EDGE : NEAR_EDGE), y + ((corner & 2) != 0 ? FAR_EDGE : NEAR_EDGE), z + ((corner & 4) != 0 ? FAR_EDGE : NEAR_EDGE))) {
							seen_void = true;
						} else {
							seen_other = true;
						}
					}
					std::uint8_t& kind = out.kind[block++];
					if (!seen_other) {
						kind = VOID_ALL;
					} else if (!seen_void) {
						kind = VOID_NONE;
					} else {
						kind = VOID_SOME;
						std::array<std::uint64_t, 8> layer{};
						for (int sy = 0; sy < 8; ++sy) {
							for (int sz = 0; sz < 8; ++sz) {
								for (int sx = 0; sx < 8; ++sx) {
									if (is_void(x + (sx + 0.5) / 8.0, y + (sy + 0.5) / 8.0, z + (sz + 0.5) / 8.0)) {
										layer[static_cast<std::size_t>(sy)] |= 1ull << (sz * 8 + sx);
									}
								}
							}
						}
						out.layers.push_back(layer);
					}
				}
			}
		}
	}
}
