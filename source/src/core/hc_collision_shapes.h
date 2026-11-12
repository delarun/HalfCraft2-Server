#pragma once

// the geometry half of the collision stream (core/hc_collision.h): solid shapes in minecraft space (half-life's
// brushes turned into them), convex hulls turned into triangles (and into their faces), and a region's
// 8x8x8-per-block occupancy. pure code, no engine and no link, so tools/debug/collision_check.cpp checks a map's
// .bsp offline with exactly what server.dll runs.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "hc_units.h"

namespace halfcraft
{
	inline constexpr int COLLISION_REGION_SIZE = 8;  // blocks per region edge (HostCollision.REGION_SIZE)
	inline constexpr int REGION_VOXELS = COLLISION_REGION_SIZE * 8;  // voxels per region edge (64)
	/// a primitive's flags (proto::ColTriFlags: its triangles keep them): one of the map's sky brushes
	inline constexpr std::uint32_t PRIM_SKY = 1u << 1;
	/// a face of a world brush that can be dug
	inline constexpr std::uint32_t PRIM_DIGGABLE = 1u << 2;  // proto::kTriDiggable
	/// bits 8-15 of a primitive's flags: the face's dig material (core/hc_dig_material.h, proto::DigMaterial)
	inline constexpr int           PRIM_MATERIAL_SHIFT = 8;
	inline constexpr std::uint32_t PRIM_MATERIAL_MASK = 0xFFu << PRIM_MATERIAL_SHIFT;

	/// solid geometry in minecraft space.
	struct ColPrimitives
	{
		struct Tri
		{
			float         v[9];       // three corners; solid side behind the winding's normal
			std::uint32_t flags = 0;  // proto::ColTriFlags
		};
		struct Convex
		{
			std::vector<std::array<float, 4>> planes;  // n.p + d <= 0 inside
			float                             lo[3], hi[3];
			std::uint32_t                     flags = 0;
			std::vector<std::uint32_t>        plane_flags;  // empty, or one for each plane: its face's own flags (see face_flags)
		};
		std::vector<Tri>    tris;
		std::vector<Convex> convexes;
		/// the ground's triangles (displacements) around, up to GROUND_DEPTH over the box too (core/hc_dig_ground.h): what
		/// lies under them is rock for minecraft. not sent as triangles (tris has those in the box)
		std::vector<Tri> ground;
	};

	/// a region's occupancy: bit x of rows[y * REGION_VOXELS + z] is voxel (x, y, z), each 1/8 block, counted
	/// from the region's minimum corner.
	using RegionVoxels = std::vector<std::uint64_t>;

	namespace voxel_detail
	{
		inline constexpr float STEEP_MIN = 0.1f;    // |n.y| below this is a wall: keep it fine-grained
		inline constexpr float STEEP_MAX = 0.643f;  // |n.y| below this (steeper than ~50 deg) gets block-coarsened
		inline constexpr float PRIM_MARGIN = 0.5f;  // voxels; lets thin convex shapes still register

		inline void sub(const float* a, const float* b, float* o)
		{
			o[0] = a[0] - b[0], o[1] = a[1] - b[1], o[2] = a[2] - b[2];
		}
		inline void cross(const float* a, const float* b, float* o)
		{
			o[0] = a[1] * b[2] - a[2] * b[1];
			o[1] = a[2] * b[0] - a[0] * b[2];
			o[2] = a[0] * b[1] - a[1] * b[0];
		}
		inline float dot(const float* a, const float* b)
		{
			return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
		}

		// ---- triangle / box overlap (akenine-moller SAT), voxel units ------------------------------
		inline bool axis_test(const float* v0, const float* v1, const float* v2, const float* axis, float h)
		{
			const float p0 = dot(v0, axis), p1 = dot(v1, axis), p2 = dot(v2, axis);
			const float mn = std::min({ p0, p1, p2 }), mx = std::max({ p0, p1, p2 });
			const float r = h * (std::fabs(axis[0]) + std::fabs(axis[1]) + std::fabs(axis[2]));
			return !(mn > r) && !(mx < -r);
		}

		// box centred at c with half-size h (all axes). triangle corners ta/tb/tc, face normal n.
		inline bool tri_box_overlap(const float* c, float h, const float* ta, const float* tb, const float* tc, const float* n)
		{
			float v0[3], v1[3], v2[3];
			sub(ta, c, v0);
			sub(tb, c, v1);
			sub(tc, c, v2);
			for (int i = 0; i < 3; ++i) {
				const float mn = std::min({ v0[i], v1[i], v2[i] }), mx = std::max({ v0[i], v1[i], v2[i] });
				if (mn > h || mx < -h) {
					return false;
				}
			}
			const float d = dot(n, v0);
			const float r = h * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]));
			if (std::fabs(d) > r) {
				return false;
			}
			float e[3][3];
			sub(v1, v0, e[0]);
			sub(v2, v1, e[1]);
			sub(v0, v2, e[2]);
			static constexpr float AXES[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
			for (auto& edge : e) {
				for (auto& unit : AXES) {
					float axis[3];
					cross(edge, unit, axis);
					if (!axis_test(v0, v1, v2, axis, h)) {
						return false;
					}
				}
			}
			return true;
		}

		/// the face a convex's plane makes: a square big enough to span the convex, on the plane, cut down by all the
		/// other planes. wound counter-clockwise seen from outside.
		/// @param poly - its corners (empty when there's no face)
		/// @param clipped - scratch for the cutting: a caller that goes over many faces allocates it once
		/// @return false when the plane has no face: it's out of range or has no normal, or the others leave it less than a triangle
		inline bool face_polygon(const ColPrimitives::Convex& cvx, std::size_t plane, std::vector<std::array<float, 3>>& poly, std::vector<std::array<float, 3>>& clipped)
		{
			poly.clear();
			if (plane >= cvx.planes.size()) {
				return false;
			}
			const float ex = cvx.hi[0] - cvx.lo[0], ey = cvx.hi[1] - cvx.lo[1], ez = cvx.hi[2] - cvx.lo[2];
			const float diag = std::sqrt(ex * ex + ey * ey + ez * ez) + 1.0f;
			const float mid[3] = { (cvx.lo[0] + cvx.hi[0]) * 0.5f, (cvx.lo[1] + cvx.hi[1]) * 0.5f, (cvx.lo[2] + cvx.hi[2]) * 0.5f };
			const auto& pl = cvx.planes[plane];
			const float n[3] = { pl[0], pl[1], pl[2] };
			const float nl = std::sqrt(dot(n, n));
			if (nl < 1e-6f) {
				return false;
			}
			const float dist = (dot(n, mid) + pl[3]) / (nl * nl);
			const float o[3] = { mid[0] - n[0] * dist, mid[1] - n[1] * dist, mid[2] - n[2] * dist };
			const float ref[3] = { std::fabs(n[1]) < 0.9f * nl ? 0.0f : 1.0f, std::fabs(n[1]) < 0.9f * nl ? 1.0f : 0.0f, 0.0f };
			float       t1[3], t2[3];
			cross(ref, n, t1);
			const float lt = std::sqrt(dot(t1, t1));
			for (float& k : t1) {
				k /= lt;
			}
			cross(n, t1, t2);
			const float l2 = std::sqrt(dot(t2, t2));
			for (float& k : t2) {
				k /= l2;
			}
			const float sgn1[4] = { -1, 1, 1, -1 }, sgn2[4] = { -1, -1, 1, 1 };
			for (int q = 0; q < 4; ++q) {
				const float s1 = sgn1[q] * diag, s2 = sgn2[q] * diag;
				poly.push_back({ o[0] + t1[0] * s1 + t2[0] * s2, o[1] + t1[1] * s1 + t2[1] * s2, o[2] + t1[2] * s1 + t2[2] * s2 });
			}
			for (std::size_t j = 0; j < cvx.planes.size() && poly.size() >= 3; ++j) {
				if (j == plane) {
					continue;
				}
				const auto& cp = cvx.planes[j];
				clipped.clear();
				for (std::size_t v = 0; v < poly.size(); ++v) {
					const auto& a = poly[v];
					const auto& b = poly[(v + 1) % poly.size()];
					const float da = cp[0] * a[0] + cp[1] * a[1] + cp[2] * a[2] + cp[3];
					const float db = cp[0] * b[0] + cp[1] * b[1] + cp[2] * b[2] + cp[3];
					if (da <= 0.0f) {
						clipped.push_back(a);
					}
					if ((da <= 0.0f) != (db <= 0.0f)) {
						const float t = da / (da - db);
						clipped.push_back({ a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t });
					}
				}
				poly.swap(clipped);
			}
			if (poly.size() < 3) {
				poly.clear();
				return false;
			}
			return true;
		}
	}

	/// a half-life brush as a minecraft convex. its bounds are the source box it was gathered in, cut down by its
	/// planes along the axes (vbsp gives every brush those).
	/// @param planes - inside where n.p <= dist: {nx, ny, nz, dist}, source units
	/// @param mins, maxs - the source box it was gathered in
	inline void brush_to_convex(const std::array<float, 4>* planes, std::size_t count, const float mins[3], const float maxs[3], MapSlot slot,
		ColPrimitives::Convex& out)
	{
		const float units = static_cast<float>(UNITS_PER_BLOCK);
		const float offset = static_cast<float>(slot.x_blocks());
		float       bmins[3] = { mins[0], mins[1], mins[2] }, bmaxs[3] = { maxs[0], maxs[1], maxs[2] };
		out.planes.clear();
		out.plane_flags.clear();
		for (std::size_t p = 0; p < count; ++p) {
			const auto& pl = planes[p];
			// source: inside where n.p <= dist. minecraft: n' = (nx, nz, -ny), inside where
			// n'.p' - (nx * slot offset + (dist - nz * grid z) / units) <= 0.
			out.planes.push_back({ pl[0], pl[2], -pl[1], -(pl[0] * offset + (pl[3] - pl[2] * slot.grid_z) / units) });
			// only a plane exactly along an axis bounds the brush along it (vbsp snaps those). a bevel a hair
			// off one (d1_trainstation_01 has them) gave its distance as the bound, which it isn't at all: the
			// box turned inside out and the brush got no voxels
			for (int axis = 0; axis < 3; ++axis) {
				if (pl[axis] == 1.0f) {
					bmaxs[axis] = std::min(bmaxs[axis], pl[3]);
				} else if (pl[axis] == -1.0f) {
					bmins[axis] = std::max(bmins[axis], -pl[3]);
				}
			}
		}
		out.lo[0] = bmins[0] / units + offset;
		out.hi[0] = bmaxs[0] / units + offset;
		out.lo[1] = (bmins[2] - slot.grid_z) / units;
		out.hi[1] = (bmaxs[2] - slot.grid_z) / units;
		out.lo[2] = -bmaxs[1] / units;
		out.hi[2] = -bmins[1] / units;
		out.flags = 0;
	}

	/// the flags of a convex's face on a plane: the convex's own and the plane's (core/hc_dig_material.h's dig_flags: a
	/// face cut out of a hole's wall has a material of its own). a plane with a material replaces the convex's, whose
	/// bits 8-15 would otherwise mix into a third one (stone | planks is dirt); a plane without one (sky, diggable alone)
	/// adds its bits to the convex's.
	inline std::uint32_t face_flags(const ColPrimitives::Convex& cvx, std::size_t plane)
	{
		if (plane >= cvx.plane_flags.size()) {
			return cvx.flags;
		}
		const std::uint32_t own = cvx.plane_flags[plane];
		return ((own & PRIM_MATERIAL_MASK) != 0 ? cvx.flags & ~PRIM_MATERIAL_MASK : cvx.flags) | own;
	}

	/// the face a convex's plane makes, as the corners of a polygon wound counter-clockwise seen from outside.
	/// @param poly - replaced by the corners (empty when false)
	/// @return false when the plane has no face (see voxel_detail::face_polygon)
	inline bool convex_face(const ColPrimitives::Convex& cvx, std::size_t plane, std::vector<std::array<float, 3>>& poly)
	{
		std::vector<std::array<float, 3>> scratch;
		return voxel_detail::face_polygon(cvx, plane, poly, scratch);
	}

	/// convex hulls (planes) -> outward-wound triangles: clip a big square on each plane by all the
	/// others. appended to out after the source's own triangles, each face's with its face_flags.
	inline void triangulate(const ColPrimitives& src, std::vector<ColPrimitives::Tri>& out)
	{
		using namespace voxel_detail;
		// one at a time: a range insert's strong guarantee needs exception handling, which the sdk builds without (c4530)
		for (const auto& tri : src.tris) {
			out.push_back(tri);
		}
		std::vector<std::array<float, 3>> poly, clipped;
		for (const auto& cvx : src.convexes) {
			for (std::size_t i = 0; i < cvx.planes.size(); ++i) {
				if (!face_polygon(cvx, i, poly, clipped)) {
					continue;
				}
				const auto&         pl = cvx.planes[i];
				const float         n[3] = { pl[0], pl[1], pl[2] };
				const std::uint32_t flags = face_flags(cvx, i);
				for (std::size_t v = 1; v + 1 < poly.size(); ++v) {
					ColPrimitives::Tri t{ { poly[0][0], poly[0][1], poly[0][2], poly[v][0], poly[v][1], poly[v][2], poly[v + 1][0], poly[v + 1][1], poly[v + 1][2] }, flags };
					float              e1[3], e2[3], tn[3];
					sub(t.v + 3, t.v, e1);
					sub(t.v + 6, t.v, e2);
					cross(e1, e2, tn);
					if (dot(tn, n) < 0.0f) {
						std::swap_ranges(t.v + 3, t.v + 6, t.v + 6);
					}
					out.push_back(t);
				}
			}
		}
	}

	/// the part of a triangle inside the box [lo, hi], appended to out as triangles with its winding and flags: its
	/// polygon cut by the box's six planes in turn (sutherland-hodgman: a corner on a plane is inside, an edge gets a new
	/// corner only where it crosses one, as SkyCraft's Clip::Split), fanned from the first corner without the slivers
	/// (|e1 x e2| < 1e-6). nothing when it misses the box.
	inline void clip_triangle(const ColPrimitives::Tri& tri, const float lo[3], const float hi[3], std::vector<ColPrimitives::Tri>& out)
	{
		using namespace voxel_detail;
		using Corner = std::array<float, 3>;
		// each plane adds a corner at most to a convex polygon: a triangle ends with 9. the rest is room for rounding
		constexpr std::size_t           MAX_CORNERS = 32;
		std::array<Corner, MAX_CORNERS> poly{}, next{};
		std::size_t                     count = 3;
		for (std::size_t c = 0; c < 3; ++c) {
			poly[c] = { tri.v[c * 3], tri.v[c * 3 + 1], tri.v[c * 3 + 2] };
		}
		for (std::size_t plane = 0; plane < 6 && count >= 3; ++plane) {
			const std::size_t axis = plane / 2;
			const bool        upper = plane % 2 == 1;
			const float       bound = upper ? hi[axis] : lo[axis];
			// how far out of the box a corner lies past this plane (on it: 0, inside)
			const auto  outside = [axis, upper, bound](const Corner& c) { return upper ? c[axis] - bound : bound - c[axis]; };
			std::size_t kept = 0;
			for (std::size_t i = 0; i < count && kept < MAX_CORNERS; ++i) {
				const Corner& a = poly[i];
				const Corner& b = poly[(i + 1) % count];
				const float   da = outside(a), db = outside(b);
				if (da <= 0.0f) {
					next[kept++] = a;
				}
				if (((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) && kept < MAX_CORNERS) {
					const float t = da / (da - db);
					Corner      cut = { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t };
					cut[axis] = bound;  // on the plane exactly, whatever the rounding
					next[kept++] = cut;
				}
			}
			poly.swap(next);
			count = kept;
		}
		for (std::size_t i = 1; i + 1 < count; ++i) {
			ColPrimitives::Tri piece{ { poly[0][0], poly[0][1], poly[0][2], poly[i][0], poly[i][1], poly[i][2], poly[i + 1][0], poly[i + 1][1], poly[i + 1][2] },
				tri.flags };
			float              e1[3], e2[3], n[3];
			sub(piece.v + 3, piece.v, e1);
			sub(piece.v + 6, piece.v, e2);
			cross(e1, e2, n);
			if (std::sqrt(dot(n, n)) >= 1e-6f) {
				out.push_back(piece);
			}
		}
	}

	/// the occupancy of region (rx, ry, rz), from everything gathered for it (with the half block of
	/// slack the streamer gathers around a region).
	///
	/// a voxel is solid where geometry reaches into it; a surface only touching one of its faces doesn't
	/// fill it. #8's grid shift (core/hc_grid.h) puts a map's main floor exactly on a voxel boundary: a
	/// brush there used to fill the voxel above its top too (mobs and items stood 5 units over the floor),
	/// and a flat displacement there filled nothing at all, so they fell through it (d1_canals_01's gravel
	/// by the tracks, z = 256 with the grid at 16).
	///
	/// @param sky - when given, the voxels of the map's sky brushes (PRIM_SKY) among them
	inline void voxelize_region(const ColPrimitives& prims, int rx, int ry, int rz, RegionVoxels& solid, RegionVoxels* sky = nullptr)
	{
		using namespace voxel_detail;
		constexpr int G = REGION_VOXELS;
		solid.assign(G * G, 0);
		if (sky) {
			sky->assign(G * G, 0);
		}
		RegionVoxels steep(G * G, 0);
		auto         set = [&](RegionVoxels& grid, int x, int y, int z) { grid[y * G + z] |= 1ull << x; };

		const float ox = float(rx * COLLISION_REGION_SIZE), oy = float(ry * COLLISION_REGION_SIZE), oz = float(rz * COLLISION_REGION_SIZE);
		auto        to_voxel = [&](const float* mc, float* out) {
			out[0] = (mc[0] - ox) * 8.0f;
			out[1] = (mc[1] - oy) * 8.0f;
			out[2] = (mc[2] - oz) * 8.0f;
		};
		auto clamp_lo = [&](float v) { return std::clamp(static_cast<int>(std::floor(v)), 0, G - 1); };
		auto clamp_hi = [&](float v) { return std::clamp(static_cast<int>(std::ceil(v)) - 1, 0, G - 1); };

		// triangles: plane-guided SAT test so big triangles cost O(area) instead of O(volume).
		for (const auto& tri : prims.tris) {
			float a[3], b[3], c[3];
			to_voxel(tri.v, a);
			to_voxel(tri.v + 3, b);
			to_voxel(tri.v + 6, c);
			float lo[3], hi[3];
			for (int i = 0; i < 3; ++i) {
				lo[i] = std::min({ a[i], b[i], c[i] });
				hi[i] = std::max({ a[i], b[i], c[i] });
			}
			if (hi[0] < 0 || hi[1] < 0 || hi[2] < 0 || lo[0] > G || lo[1] > G || lo[2] > G) {
				continue;
			}
			float e1[3], e2[3], n[3];
			sub(b, a, e1);
			sub(c, a, e2);
			cross(e1, e2, n);
			const float len = std::sqrt(dot(n, n));
			if (len < 1e-9f) {
				continue;
			}
			n[0] /= len, n[1] /= len, n[2] /= len;
			const float ny = std::fabs(n[1]);
			const bool  flat = ny >= STEEP_MAX || ny < STEEP_MIN;
			auto&       grid = flat ? solid : steep;

			int dom = 0;
			if (std::fabs(n[1]) > std::fabs(n[dom]))
				dom = 1;
			if (std::fabs(n[2]) > std::fabs(n[dom]))
				dom = 2;
			const int   u = (dom + 1) % 3, v = (dom + 2) % 3;
			const float d = dot(n, a);
			const float r = 0.5f * (std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]));
			const int   iu0 = clamp_lo(lo[u]), iu1 = clamp_hi(hi[u]), iv0 = clamp_lo(lo[v]), iv1 = clamp_hi(hi[v]);
			// the layers along the normal's axis it reaches into. lying exactly on a layer boundary it
			// reaches into neither: a floor fills the layer under it, its solid side; a wall both, as its
			// solid side isn't known (vphysics hands each triangle over in both windings)
			int id0 = static_cast<int>(std::floor(lo[dom])), id1 = static_cast<int>(std::ceil(hi[dom])) - 1;
			if (id1 < id0) {
				id0 = id1;
				if (dom != 1) {
					++id1;
				}
			}
			id0 = std::max(id0, 0);
			id1 = std::min(id1, G - 1);
			for (int iu = iu0; iu <= iu1; ++iu) {
				for (int iv = iv0; iv <= iv1; ++iv) {
					const float cu = iu + 0.5f, cv = iv + 0.5f;
					const float s0 = (d - r - n[u] * cu - n[v] * cv) / n[dom];
					const float s1 = (d + r - n[u] * cu - n[v] * cv) / n[dom];
					const int   a0 = std::max(id0, static_cast<int>(std::floor(std::min(s0, s1) - 0.5f)));
					const int   a1 = std::min(id1, static_cast<int>(std::ceil(std::max(s0, s1) - 0.5f)));
					for (int id = a0; id <= a1; ++id) {
						float cen[3];
						cen[dom] = id + 0.5f;
						cen[u] = cu;
						cen[v] = cv;
						if (tri_box_overlap(cen, 0.5f, a, b, c, n)) {
							int p[3];
							p[dom] = id, p[u] = iu, p[v] = iv;
							set(grid, p[0], p[1], p[2]);
						}
					}
				}
			}
		}

		// convex primitives: voxel-centre containment with a small margin. a face right on a voxel's side
		// leaves the voxel out (see above)
		constexpr float m = PRIM_MARGIN / 8.0f;
		for (const auto& cvx : prims.convexes) {
			float lo[3], hi[3];
			to_voxel(cvx.lo, lo);
			to_voxel(cvx.hi, hi);
			if (hi[0] < 0 || hi[1] < 0 || hi[2] < 0 || lo[0] > G || lo[1] > G || lo[2] > G) {
				continue;
			}
			for (int y = clamp_lo(lo[1] - 1); y <= clamp_hi(hi[1] + 1); ++y) {
				for (int z = clamp_lo(lo[2] - 1); z <= clamp_hi(hi[2] + 1); ++z) {
					for (int x = clamp_lo(lo[0] - 1); x <= clamp_hi(hi[0] + 1); ++x) {
						const float p[3] = { ox + (x + 0.5f) / 8.0f, oy + (y + 0.5f) / 8.0f, oz + (z + 0.5f) / 8.0f };
						bool        inside = true;
						for (const auto& pl : cvx.planes) {
							if (pl[0] * p[0] + pl[1] * p[1] + pl[2] * p[2] + pl[3] >= m) {
								inside = false;
								break;
							}
						}
						if (inside) {
							set(solid, x, y, z);
							if (sky && (cvx.flags & PRIM_SKY)) {
								set(*sky, x, y, z);
							}
						}
					}
				}
			}
		}

		// steep (50-84 degree) surfaces: snap to whole-block footprints so the risers between
		// neighbouring columns exceed minecraft's 0.6 step height. minecraft's own step-up and jump
		// rules then decide what is climbable, like a cliff made of blocks.
		for (int by = 0; by < COLLISION_REGION_SIZE; ++by) {
			for (int bz = 0; bz < COLLISION_REGION_SIZE; ++bz) {
				for (int bx = 0; bx < COLLISION_REGION_SIZE; ++bx) {
					const std::uint64_t xmask = 0xFFull << (bx * 8);
					int                 min_y = 99, max_y = -1;
					for (int y = by * 8; y < by * 8 + 8; ++y) {
						for (int z = bz * 8; z < bz * 8 + 8; ++z) {
							if (steep[y * G + z] & xmask) {
								min_y = std::min(min_y, y);
								max_y = std::max(max_y, y);
							}
						}
					}
					for (int y = min_y; y <= max_y; ++y) {
						for (int z = bz * 8; z < bz * 8 + 8; ++z) {
							solid[y * G + z] |= xmask;
						}
					}
				}
			}
		}
	}
}
