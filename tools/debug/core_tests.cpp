// core_tests: plain checks over the pure c++ core (source/src/core's headers: no sdk, no engine, no link), the code
// server.dll and client.dll share with no engine under it. built and run by tools/debug/core_tests.py (make core-test).
//
// CHECK(condition) counts a check and, when it fails, says which one and goes on: one run shows everything wrong.
// each test is a function named for what it holds; run_tests calls them all and returns the exit code.

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/hc_bsp_tree.h"
#include "core/hc_campaign_models.h"
#include "core/hc_collision_shapes.h"
#include "core/hc_coop.h"
#include "core/hc_dig_material.h"
#include "core/hc_dig_clip.h"
#include "core/hc_dig_mask.h"
#include "core/hc_dig_void.h"
#include "core/hc_dug_cells.h"
#include "core/hc_link_names.h"
#include "core/hc_sandbox.h"

using namespace halfcraft;

namespace
{
	int checks = 0, failed = 0;

	bool record(bool ok, const char* expression, const char* file, int line)
	{
		++checks;
		if (!ok) {
			++failed;
			std::printf("FAILED %s:%d: %s\n", file, line, expression);
		}
		return ok;
	}
}

#define CHECK(condition) record(static_cast<bool>(condition), #condition, __FILE__, __LINE__)

namespace
{
	using Cvx = ColPrimitives::Convex;
	using Poly = std::vector<std::array<float, 3>>;

	constexpr std::uint32_t D = PRIM_DIGGABLE;

	/// the dig material k in a primitive's flags
	constexpr std::uint32_t M(std::uint32_t k)
	{
		return k << PRIM_MATERIAL_SHIFT;
	}

	bool near(float a, float b, float tolerance = 1e-4f)
	{
		return std::fabs(a - b) <= tolerance;
	}

	/// the cube [lo, hi]^3 as a convex: its planes are +x, -x, +y, -y, +z, -z
	Cvx cube(float lo, float hi)
	{
		Cvx cvx;
		cvx.planes.push_back({ 1, 0, 0, -hi });
		cvx.planes.push_back({ -1, 0, 0, lo });
		cvx.planes.push_back({ 0, 1, 0, -hi });
		cvx.planes.push_back({ 0, -1, 0, lo });
		cvx.planes.push_back({ 0, 0, 1, -hi });
		cvx.planes.push_back({ 0, 0, -1, lo });
		for (int k = 0; k < 3; ++k) {
			cvx.lo[k] = lo;
			cvx.hi[k] = hi;
		}
		return cvx;
	}

	/// a triangle's normal, as long as twice its area
	std::array<float, 3> triangle_normal(const float* v)
	{
		const float e1[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] }, e2[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
		return { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
	}

	/// a convex polygon's normal, as long as twice its area: its fan from the first corner, summed
	std::array<float, 3> polygon_normal(const Poly& poly)
	{
		std::array<float, 3> sum = { 0, 0, 0 };
		for (std::size_t i = 1; i + 1 < poly.size(); ++i) {
			const float tri[9] = { poly[0][0], poly[0][1], poly[0][2], poly[i][0], poly[i][1], poly[i][2], poly[i + 1][0], poly[i + 1][1], poly[i + 1][2] };
			const auto  n = triangle_normal(tri);
			for (int k = 0; k < 3; ++k) {
				sum[k] += n[k];
			}
		}
		return sum;
	}

	float length(const std::array<float, 3>& v)
	{
		return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	}

	/// which of a cube's faces (the order of cube()'s planes) a normal looks out of: the axis it's longest along, then its sign
	std::size_t cube_face_of(const std::array<float, 3>& n)
	{
		std::size_t axis = 0;
		for (std::size_t k = 1; k < 3; ++k) {
			if (std::fabs(n[k]) > std::fabs(n[axis])) {
				axis = k;
			}
		}
		return axis * 2 + (n[axis] < 0.0f ? 1 : 0);
	}

	using Cell = std::array<int, 3>;

	bool is_box(const DigBox& box, float x0, float y0, float z0, float x1, float y1, float z1)
	{
		return box.lo[0] == x0 && box.lo[1] == y0 && box.lo[2] == z0 && box.hi[0] == x1 && box.hi[1] == y1 && box.hi[2] == z1;
	}

	bool same_convex(const Cvx& a, const Cvx& b)
	{
		return a.planes == b.planes && std::equal(a.lo, a.lo + 3, b.lo) && std::equal(a.hi, a.hi + 3, b.hi) && a.flags == b.flags && a.plane_flags == b.plane_flags;
	}

	/// the voxels of region (0, 0, 0) that convexes hold
	RegionVoxels voxels_of(const std::vector<Cvx>& convexes)
	{
		ColPrimitives prims;
		for (const Cvx& cvx : convexes) {
			prims.convexes.push_back(cvx);
		}
		RegionVoxels voxels;
		voxelize_region(prims, 0, 0, 0, voxels);
		return voxels;
	}

	int voxel_count(const RegionVoxels& voxels)
	{
		int count = 0;
		for (const std::uint64_t row : voxels) {
			count += static_cast<int>(std::bitset<64>(row).count());
		}
		return count;
	}

	/// a brush's voxels (region 0, 0, 0) without the blocks of cells, which lie in the region: a block is 8 voxels an edge
	RegionVoxels voxels_without(const Cvx& brush, const std::vector<Cell>& cells)
	{
		RegionVoxels voxels = voxels_of({ brush });
		for (const Cell& cell : cells) {
			for (int dy = 0; dy < 8; ++dy) {
				for (int dz = 0; dz < 8; ++dz) {
					voxels[(cell[1] * 8 + dy) * REGION_VOXELS + cell[2] * 8 + dz] &= ~(0xFFull << (cell[0] * 8));
				}
			}
		}
		return voxels;
	}

	/// the cells cut out of the brush (merge_cells, subtract_boxes) leave the brush's voxels less the cells' blocks
	bool cut_matches(const Cvx& brush, const std::vector<Cell>& cells)
	{
		std::vector<DigBox> boxes;
		merge_cells(cells, boxes);
		std::vector<Cvx> pieces;
		subtract_boxes(brush, boxes, 0, pieces);
		return voxels_of(pieces) == voxels_without(brush, cells);
	}

	/// how many of the brush's voxels the cells' blocks hold
	int voxels_in(const Cvx& brush, const std::vector<Cell>& cells)
	{
		return voxel_count(voxels_of({ brush })) - voxel_count(voxels_without(brush, cells));
	}

	/// how many of a piece's planes have those flags
	std::size_t planes_flagged(const Cvx& piece, std::uint32_t flags)
	{
		return static_cast<std::size_t>(std::count(piece.plane_flags.begin(), piece.plane_flags.end(), flags));
	}

	/// every face of a convex lies within its bounds: its planes make the box its bounds say
	bool fits_bounds(const Cvx& cvx)
	{
		Poly poly;
		for (std::size_t i = 0; i < cvx.planes.size(); ++i) {
			if (!convex_face(cvx, i, poly)) {
				continue;
			}
			for (const auto& corner : poly) {
				for (int k = 0; k < 3; ++k) {
					if (corner[k] < cvx.lo[k] - 1e-4f || corner[k] > cvx.hi[k] + 1e-4f) {
						return false;
					}
				}
			}
		}
		return true;
	}

	float bounds_volume(const Cvx& cvx)
	{
		return (cvx.hi[0] - cvx.lo[0]) * (cvx.hi[1] - cvx.lo[1]) * (cvx.hi[2] - cvx.lo[2]);
	}

	void test_convex_face_gives_a_cube_its_squares()
	{
		Cvx  cvx = cube(0, 1);
		Poly poly;
		CHECK(convex_face(cvx, 0, poly));
		CHECK(poly.size() == 4);
		for (const auto& corner : poly) {
			CHECK(near(corner[0], 1.0f, 1e-5f));
			CHECK(near(corner[1], 0.0f, 1e-5f) || near(corner[1], 1.0f, 1e-5f));
			CHECK(near(corner[2], 0.0f, 1e-5f) || near(corner[2], 1.0f, 1e-5f));
		}
		// counter-clockwise seen from outside: its normal points out of the cube, along +x
		const auto n = polygon_normal(poly);
		CHECK(near(length(n) / 2.0f, 1.0f));
		CHECK(n[0] > 0.0f);

		// a plane outside the others' box has no face, and neither has one with no normal
		cvx.planes.push_back({ 1, 0, 0, -2 });
		cvx.planes.push_back({ 0, 0, 0, 0 });
		CHECK(!convex_face(cvx, 6, poly));
		CHECK(!convex_face(cvx, 7, poly));
	}

	void test_face_flags_take_the_planes_material()
	{
		Cvx cvx = cube(0, 1);
		cvx.flags = D | M(1);
		CHECK(face_flags(cvx, 0) == cvx.flags);
		cvx.plane_flags = { D | M(6), 0, 0, 0, 0, 0 };
		CHECK(face_flags(cvx, 0) == (D | M(6)));
		CHECK(face_flags(cvx, 1) == cvx.flags);
		cvx.flags = PRIM_SKY;
		CHECK(face_flags(cvx, 0) == (PRIM_SKY | D | M(6)));
	}

	void test_triangulate_gives_each_face_its_flags()
	{
		ColPrimitives prims;
		prims.tris.push_back({ { 0, 0, 0, 1, 0, 0, 0, 0, 1 }, 1 });
		Cvx cvx = cube(0, 1);
		cvx.flags = PRIM_SKY;
		for (std::uint32_t i = 0; i < 6; ++i) {
			cvx.plane_flags.push_back(D | M(i + 1));
		}
		prims.convexes.push_back(cvx);
		std::vector<ColPrimitives::Tri> tris;
		triangulate(prims, tris);
		CHECK(tris.size() == 13);
		if (tris.size() != 13) {
			return;
		}
		CHECK(tris[0].flags == 1);
		int on_face[6] = {};
		for (std::size_t t = 1; t < tris.size(); ++t) {
			const std::size_t face = cube_face_of(triangle_normal(tris[t].v));
			++on_face[face];
			CHECK(tris[t].flags == (PRIM_SKY | D | M(static_cast<std::uint32_t>(face) + 1)));
		}
		for (const int count : on_face) {
			CHECK(count == 2);
		}
	}

	void test_dig_material_maps_surface_materials()
	{
		CHECK(dig_material('C') == DIG_STONE);
		CHECK(dig_material('T') == DIG_STONE_BRICKS);
		CHECK(dig_material('M') == DIG_IRON);
		CHECK(dig_material('G') == DIG_IRON);
		CHECK(dig_material('V') == DIG_IRON);
		CHECK(dig_material('P') == DIG_IRON);
		CHECK(dig_material('W') == DIG_PLANKS);
		CHECK(dig_material('U') == DIG_PLANKS);
		CHECK(dig_material('D') == DIG_DIRT);
		CHECK(dig_material('Y') == DIG_GLASS);
		CHECK(dig_material('F') == DIG_STONE);
		CHECK(dig_material('S') == DIG_STONE);
		CHECK(dig_material('N') == DIG_STONE);
		CHECK(dig_material('Z') == DIG_STONE);
		CHECK(dig_material('\0') == DIG_STONE);
		CHECK(dig_flags(DIG_STONE) == 0x104u);
		CHECK(dig_flags(DIG_GLASS) == 0x604u);
	}

	void test_brush_material()
	{
		CHECK(brush_material({}) == DIG_STONE);
		CHECK(brush_material({ DIG_NONE, DIG_NONE }) == DIG_STONE);
		CHECK(brush_material({ DIG_NONE, DIG_DIRT }) == DIG_DIRT);
		CHECK(brush_material({ DIG_PLANKS, DIG_NONE, DIG_STONE, DIG_PLANKS }) == DIG_PLANKS);
		CHECK(brush_material({ DIG_GLASS, DIG_IRON, DIG_IRON, DIG_GLASS }) == DIG_GLASS);  // a tie: the first
		CHECK(brush_material({ DIG_NONE, DIG_IRON, DIG_GLASS, DIG_GLASS, DIG_IRON }) == DIG_IRON);
	}

	void test_merge_cells_makes_boxes()
	{
		std::vector<DigBox> boxes;
		merge_cells({ { 0, 0, 0 }, { 1, 0, 0 } }, boxes);
		CHECK(boxes.size() == 1 && is_box(boxes[0], 0, 0, 0, 2, 1, 1));

		// an L: the row along x first, then what's left
		merge_cells({ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 0, 0 } }, boxes);
		CHECK(boxes.size() == 2);
		if (boxes.size() == 2) {
			CHECK(is_box(boxes[0], 0, 0, 0, 2, 1, 1));
			CHECK(is_box(boxes[1], 0, 0, 1, 1, 1, 2));
		}

		// a cube of cells, one of them twice
		std::vector<Cell> cube_cells;
		for (int y = 0; y < 2; ++y) {
			for (int z = 0; z < 2; ++z) {
				for (int x = 0; x < 2; ++x) {
					cube_cells.push_back({ x, y, z });
				}
			}
		}
		cube_cells.push_back({ 1, 0, 1 });
		merge_cells(cube_cells, boxes);
		CHECK(boxes.size() == 1 && is_box(boxes[0], 0, 0, 0, 2, 2, 2));

		merge_cells({ { -3, 5, -1 } }, boxes);
		CHECK(boxes.size() == 1 && is_box(boxes[0], -3, 5, -1, -2, 6, 0));

		merge_cells({}, boxes);  // replaces what was there
		CHECK(boxes.empty());
	}

	void test_subtract_boxes_cuts_the_cells_voxels()
	{
		CHECK(cut_matches(cube(2, 4), { { 2, 2, 2 } }));
		CHECK(cut_matches(cube(2, 4), { { 3, 3, 3 } }));
		CHECK(cut_matches(cube(2, 4), { { 2, 2, 2 }, { 3, 2, 2 }, { 2, 2, 3 } }));
		CHECK(cut_matches(cube(2, 4), { { 4, 2, 2 } }));
		CHECK(cut_matches(cube(2.25f, 3.75f), { { 3, 3, 3 } }));
		CHECK(cut_matches(cube(2.25f, 3.75f), { { 2, 3, 2 } }));
		// and they cut something: a cell inside takes its voxels (8 an edge), one that only touches takes none
		CHECK(voxels_in(cube(2, 4), { { 2, 2, 2 } }) == 512);
		CHECK(voxels_in(cube(2, 4), { { 2, 2, 2 }, { 3, 2, 2 }, { 2, 2, 3 } }) == 3 * 512);
		CHECK(voxels_in(cube(2, 4), { { 4, 2, 2 } }) == 0);
		CHECK(voxels_in(cube(2.25f, 3.75f), { { 3, 3, 3 } }) == 6 * 6 * 6);
	}

	void test_subtract_boxes_flags_the_cut_faces()
	{
		const std::uint32_t stone = dig_flags(DIG_STONE), bricks = dig_flags(DIG_STONE_BRICKS), planks = dig_flags(DIG_PLANKS);
		Cvx                 brush = cube(2, 4);
		brush.flags = stone;
		brush.plane_flags.assign(6, bricks);
		std::vector<DigBox> boxes;
		merge_cells({ { 2, 2, 2 } }, boxes);
		std::vector<Cvx> pieces;
		subtract_boxes(brush, boxes, planks, pieces);

		// what the cube has left outside the cell: x > 3, then y > 3 at x 2..3, then z > 3 at x and y 2..3 (the layers
		// below the cell are nothing: the cube starts where the cell does)
		CHECK(pieces.size() == 3);
		if (pieces.size() != 3) {
			return;
		}
		const std::size_t plane_counts[3] = { 7, 8, 9 };
		ColPrimitives     prims;
		for (std::size_t k = 0; k < 3; ++k) {
			CHECK(pieces[k].planes.size() == plane_counts[k]);
			CHECK(pieces[k].flags == stone);
			CHECK(pieces[k].plane_flags.size() == pieces[k].planes.size());
			CHECK(planes_flagged(pieces[k], bricks) == 6);  // the cube's own faces keep theirs...
			CHECK(planes_flagged(pieces[k], planks) == plane_counts[k] - 6);  // ...and the cell's new ones are the cut's
			prims.convexes.push_back(pieces[k]);
		}

		// their faces, by axis, where they lie (2, 3 or 4) and which way they face: the cube's face at 2 faces -axis and
		// what's left of it is 3 square blocks, at 4 it's 4 facing +axis, and the cut at 3 is planks
		std::vector<ColPrimitives::Tri> tris;
		triangulate(prims, tris);
		float area[3][3][2] = {};
		for (const auto& tri : tris) {
			const auto  n = triangle_normal(tri.v);
			const float twice_area = length(n);
			if (twice_area < 1e-5f) {
				continue;  // a corner of a face twice: no triangle
			}
			std::size_t axis = 0;
			for (std::size_t k = 1; k < 3; ++k) {
				if (std::fabs(n[k]) > std::fabs(n[axis])) {
					axis = k;
				}
			}
			const int slot = static_cast<int>(std::lround(tri.v[axis])) - 2;
			if (!CHECK(slot >= 0 && slot <= 2 && near(tri.v[axis], static_cast<float>(slot + 2)))) {
				continue;
			}
			area[axis][slot][n[axis] > 0.0f ? 1 : 0] += twice_area / 2.0f;
			CHECK(tri.flags == (slot == 1 ? planks : bricks));
		}
		for (std::size_t axis = 0; axis < 3; ++axis) {
			CHECK(near(area[axis][0][0], 3.0f));
			CHECK(near(area[axis][0][1], 0.0f));
			CHECK(near(area[axis][2][1], 4.0f));
			CHECK(near(area[axis][2][0], 0.0f));
			CHECK(area[axis][1][0] + area[axis][1][1] > 0.0f);
		}
	}

	void test_subtract_boxes_flags_the_new_planes_of_an_unflagged_brush()
	{
		Cvx brush = cube(2, 4);
		brush.flags = PRIM_SKY;
		std::vector<DigBox> boxes;
		merge_cells({ { 2, 2, 2 } }, boxes);
		std::vector<Cvx> pieces;
		subtract_boxes(brush, boxes, 7u, pieces);
		CHECK(pieces.size() == 3);
		for (const Cvx& piece : pieces) {
			CHECK(piece.flags == PRIM_SKY);
			CHECK(piece.plane_flags.size() == piece.planes.size());
			CHECK(planes_flagged(piece, 0) == 6);
			CHECK(planes_flagged(piece, 7u) == piece.planes.size() - 6);
		}
	}

	void test_subtract_boxes_leaves_what_it_doesnt_cut()
	{
		Cvx brush = cube(2, 4);
		brush.flags = PRIM_SKY;
		std::vector<Cvx> out;
		out.push_back(cube(0, 1));
		// one box far off and one that only touches the cube (at x = 4)
		const std::vector<DigBox> away = { { { 10, 10, 10 }, { 11, 11, 11 } }, { { 4, 2, 2 }, { 5, 3, 3 } } };
		subtract_boxes(brush, away, 7u, out);
		CHECK(out.size() == 2);
		if (out.size() == 2) {
			CHECK(same_convex(out[1], brush));
		}
		// one around the whole cube leaves nothing
		out.clear();
		subtract_boxes(brush, { { { 1, 1, 1 }, { 5, 5, 5 } } }, 7u, out);
		CHECK(out.empty());
	}

	void test_subtract_boxes_keeps_each_piece_within_its_bounds()
	{
		// cubes, so every piece is a box: its volume is its bounds' and the pieces add up to what the cells leave
		struct Case
		{
			Cvx               brush;
			std::vector<Cell> cells;
			float             left;  // the volume left of the brush
		};
		const Case cases[] = {
			{ cube(2, 4), { { 2, 2, 2 } }, 7.0f },
			{ cube(1, 5), { { 2, 2, 2 } }, 63.0f },
			{ cube(1, 5), { { 2, 2, 2 }, { 3, 2, 2 }, { 2, 2, 3 } }, 61.0f },
			{ cube(2.25f, 3.75f), { { 3, 3, 3 } }, 2.953125f },
		};
		for (const Case& c : cases) {
			std::vector<DigBox> boxes;
			merge_cells(c.cells, boxes);
			std::vector<Cvx> pieces;
			subtract_boxes(c.brush, boxes, 0, pieces);
			float volume = 0.0f;
			for (const Cvx& piece : pieces) {
				CHECK(fits_bounds(piece));
				volume += bounds_volume(piece);
			}
			CHECK(near(volume, c.left));
		}
	}

	void test_subtract_boxes_drops_slivers()
	{
		const std::uint32_t cut = dig_flags(DIG_PLANKS);
		const std::size_t   plane_counts[3] = { 7, 8, 9 };
		// a cell starting a hair inside the cube's face (less than DIG_SLIVER): no layer of the cube below it, and the plane
		// the cell makes there is the cube's own
		std::vector<Cvx> pieces;
		subtract_boxes(cube(2, 4), { { { 2.0005f, 2, 2 }, { 3, 3, 3 } } }, cut, pieces);
		CHECK(pieces.size() == 3);
		for (std::size_t k = 0; k < pieces.size() && k < 3; ++k) {
			CHECK(pieces[k].planes.size() == plane_counts[k]);
		}
		// and ending a hair inside the far face
		pieces.clear();
		subtract_boxes(cube(2, 4), { { { 3, 3, 3 }, { 3.9995f, 4, 4 } } }, cut, pieces);
		CHECK(pieces.size() == 3);
		// a cell further in leaves its layer
		pieces.clear();
		subtract_boxes(cube(2, 4), { { { 2.01f, 2, 2 }, { 3, 3, 3 } } }, cut, pieces);
		CHECK(pieces.size() == 4);
	}

	/// the box (x0, y0, z0)-(x1, y1, z1) as a convex: its planes are +x, -x, +y, -y, +z, -z
	Cvx box_convex(float x0, float y0, float z0, float x1, float y1, float z1, std::uint32_t flags)
	{
		Cvx cvx;
		cvx.planes.push_back({ 1, 0, 0, -x1 });
		cvx.planes.push_back({ -1, 0, 0, x0 });
		cvx.planes.push_back({ 0, 1, 0, -y1 });
		cvx.planes.push_back({ 0, -1, 0, y0 });
		cvx.planes.push_back({ 0, 0, 1, -z1 });
		cvx.planes.push_back({ 0, 0, -1, z0 });
		cvx.lo[0] = x0, cvx.lo[1] = y0, cvx.lo[2] = z0;
		cvx.hi[0] = x1, cvx.hi[1] = y1, cvx.hi[2] = z1;
		cvx.flags = flags;
		return cvx;
	}

	/// a tree of one plane (n.p = dist): empty in front of it, solid behind it
	BspTree one_plane(float nx, float ny, float nz, float dist)
	{
		BspTree tree;
		tree.planes.push_back({ nx, ny, nz, dist });
		tree.nodes.push_back({ 0, -1, -2 });
		tree.leaf_contents = { 0, BSP_CONTENTS_SOLID };
		return tree;
	}

	/// the contents of the tree at source point (0, 0, z)
	int contents_at(const BspTree& tree, float z)
	{
		const float p[3] = { 0.0f, 0.0f, z };
		return point_contents(tree, p);
	}

	void test_bsp_tree_gives_a_points_contents()
	{
		const BspTree t = one_plane(0, 0, 1, 106);
		CHECK(contents_at(t, 120) == 0);
		CHECK(contents_at(t, 80) == BSP_CONTENTS_SOLID);
		CHECK(contents_at(t, 106) == 0);  // on the plane: in front of it
		CHECK(contents_at(BspTree{}, 80) == 0);

		// a plane that isn't along an axis takes the dot product: 0.6x + 0.8z = 100 here
		const BspTree slanted = one_plane(0.6f, 0, 0.8f, 100);
		const float   front[3] = { 100, 0, 100 }, back[3] = { 20, 0, 50 };
		CHECK(point_contents(slanted, front) == 0);
		CHECK(point_contents(slanted, back) == BSP_CONTENTS_SOLID);

		// two nodes: the first's back is solid, its front goes on to a plane of x
		BspTree two = t;
		two.planes.push_back({ 1, 0, 0, 0 });
		two.nodes[0] = { 0, 1, -2 };
		two.nodes.push_back({ 1, -1, -2 });
		const float east[3] = { 5, 0, 120 }, west[3] = { -5, 0, 120 }, under[3] = { 5, 0, 80 };
		CHECK(point_contents(two, east) == 0);
		CHECK(point_contents(two, west) == BSP_CONTENTS_SOLID);
		CHECK(point_contents(two, under) == BSP_CONTENTS_SOLID);

		// a head node that is a leaf
		BspTree leaf = t;
		leaf.headnode = -2;
		CHECK(contents_at(leaf, 120) == BSP_CONTENTS_SOLID);

		// a damaged tree reads as empty, and never loops
		BspTree broken = t;
		broken.nodes[0][2] = -5;  // a leaf it doesn't have
		CHECK(contents_at(broken, 80) == 0);
		broken = t;
		broken.nodes[0][1] = 3;  // a node it doesn't have
		CHECK(contents_at(broken, 120) == 0);
		broken = t;
		broken.nodes[0][0] = 7;  // a plane it doesn't have
		CHECK(contents_at(broken, 80) == 0);
		broken = t;
		broken.nodes[0] = { 0, 0, 0 };  // a loop
		CHECK(contents_at(broken, 80) == 0);
		broken = t;
		broken.headnode = 4;
		CHECK(contents_at(broken, 80) == 0);
		broken.headnode = -9;
		CHECK(contents_at(broken, 80) == 0);
	}

	/// v's bytes at `at`
	template <typename T>
	void put(std::vector<std::uint8_t>& bytes, std::size_t at, T v)
	{
		std::memcpy(bytes.data() + at, &v, sizeof(v));
	}

	void test_read_bsp_tree()
	{
		// the .bsp's lumps of one node on the plane z = 0: empty in front of it (leaf 0), solid behind it (leaf 1)
		std::vector<std::uint8_t> planes(BSP_PLANE_BYTES), nodes(BSP_NODE_BYTES), leaves(2 * BSP_LEAF_BYTES_V1), leaves_v0(2 * BSP_LEAF_BYTES_V0);
		put(planes, 8, 1.0f);  // dplane_t's normal.z
		put(nodes, 4, -1);     // dnode_t's children: in front, behind
		put(nodes, 8, -2);
		put(leaves, BSP_LEAF_BYTES_V1, BSP_CONTENTS_SOLID);  // dleaf_t's contents
		put(leaves_v0, BSP_LEAF_BYTES_V0, BSP_CONTENTS_SOLID);
		put(leaves_v0, 32, 7);  // leaf 0's ambient light, where version 1's leaf 1 would start

		const float above[3] = { 0, 0, 10 }, below[3] = { 0, 0, -10 };
		BspTree     tree;
		CHECK(read_bsp_tree(planes, nodes, leaves, 1, 0, tree));
		CHECK(tree.planes.size() == 1 && tree.nodes.size() == 1 && tree.leaf_contents.size() == 2 && tree.headnode == 0);
		CHECK(point_contents(tree, above) == 0);
		CHECK(point_contents(tree, below) == BSP_CONTENTS_SOLID);
		CHECK(read_bsp_tree(planes, nodes, leaves_v0, 0, 0, tree));
		CHECK(point_contents(tree, below) == BSP_CONTENTS_SOLID);

		// what it refuses leaves out empty, whatever it held
		const auto refused = [&tree](const std::vector<std::uint8_t>& p, const std::vector<std::uint8_t>& n, const std::vector<std::uint8_t>& l, int version, int headnode) {
			BspTree out = tree;
			out.headnode = 5;
			return !read_bsp_tree(p, n, l, version, headnode, out) && out.planes.empty() && out.nodes.empty() && out.leaf_contents.empty() && out.headnode == 0;
		};
		CHECK(refused(planes, nodes, leaves, 2, 0));                                             // a leaf version it doesn't know
		CHECK(refused(planes, nodes, std::vector<std::uint8_t>(BSP_LEAF_BYTES_V1 + 1), 1, 0));  // a lump that isn't whole leaves
		CHECK(refused(planes, nodes, leaves, 1, 1));                                             // a head node it doesn't have
		std::vector<std::uint8_t> bad = nodes;
		put(bad, 0, 1);  // a plane it doesn't have
		CHECK(refused(planes, bad, leaves, 1, 0));
		bad = nodes;
		put(bad, 8, -3);  // leaf 2, which it doesn't have
		CHECK(refused(planes, bad, leaves, 1, 0));
		bad = nodes;
		put(bad, 8, 0);  // a child that doesn't come after its node (here itself: a loop)
		CHECK(refused(planes, bad, leaves, 1, 0));
	}

	void test_void_region_finds_the_rock_behind_the_walls()
	{
		const MapSlot slot{ 1, 16.0f };
		// the region (128, 0, 0) is source x 0..320, z 16..336 (block bx + 8bz + 64by); a dug wall makes its box diggable
		ColPrimitives prims;
		prims.convexes.push_back(box_convex(1024, 0, 0, 1024.25f, 1, 1, PRIM_DIGGABLE));  // block 0, sub-voxels x 0 and 1
		const float         x0[3] = { 1027, 1029, 1030 };  // blocks 35, 37 and 38
		const std::uint32_t flags[3] = { PRIM_DIGGABLE, PRIM_SKY, 0 };
		for (int k = 0; k < 3; ++k) {
			prims.convexes.push_back(box_convex(x0[k], 0, 4, x0[k] + 1, 1, 5, flags[k]));
		}

		// solid below minecraft y 2.25
		RegionVoid out;
		void_region(prims, one_plane(0, 0, 1, 106), slot, 128, 0, 0, out);
		CHECK(out.any());
		for (int block = 0; block < 512; ++block) {
			std::uint8_t want = VOID_ALL;
			if (block == 0 || (block >= 128 && block < 192)) {
				want = VOID_SOME;
			} else if (block == 35 || block == 37 || block >= 192) {
				want = VOID_NONE;
			}
			CHECK(out.kind[static_cast<std::size_t>(block)] == want);
		}
		if (CHECK(out.layers.size() == 65u)) {
			// block 0 is void but for its diggable sub-voxels x 0 and 1; block 128 is void in its two lowest layers
			for (const std::uint64_t row : out.layers[0]) {
				CHECK(row == 0xFCFCFCFCFCFCFCFCull);
			}
			CHECK(out.layers[1][0] == ~0ull && out.layers[1][1] == ~0ull);
			for (int y = 2; y < 8; ++y) {
				CHECK(out.layers[1][y] == 0);
			}
		}

		// solid west of source x 100 (minecraft x 1026.5), no brushes
		void_region(ColPrimitives{}, one_plane(1, 0, 0, 100), slot, 128, 0, 0, out);
		CHECK(out.kind[0] == VOID_ALL && out.kind[1] == VOID_ALL);
		CHECK(out.kind[2] == VOID_SOME);
		CHECK(out.kind[3] == VOID_NONE);
		if (CHECK(out.layers.size() == 64u)) {
			for (const std::uint64_t row : out.layers[0]) {
				CHECK(row == 0x0F0F0F0F0F0F0F0Full);
			}
		}

		// no solid leaf anywhere: out is reset
		void_region(prims, BspTree{}, slot, 128, 0, 0, out);
		CHECK(!out.any() && out.layers.empty());
	}

	void test_void_region_tests_a_convex_by_its_planes()
	{
		// the half of block 0 with x + z <= 1025 (a wedge, its bounds the whole block) is the wall's: the rest is void
		Cvx wedge = box_convex(1024, 0, 0, 1025, 1, 1, PRIM_DIGGABLE);
		wedge.planes.push_back({ 1, 0, 1, -1025 });
		ColPrimitives prims;
		prims.convexes.push_back(wedge);
		RegionVoid out;
		void_region(prims, one_plane(0, 0, 1, 106), MapSlot{ 1, 16.0f }, 128, 0, 0, out);
		CHECK(out.kind[0] == VOID_SOME);
		if (CHECK(out.layers.size() > 0u)) {
			std::uint64_t want = 0;  // sub-voxel (x, z) is void where its centre has (x + z + 1) / 8 > 1
			for (int z = 0; z < 8; ++z) {
				for (int x = 0; x < 8; ++x) {
					if (x + z >= 8) {
						want |= 1ull << (z * 8 + x);
					}
				}
			}
			for (const std::uint64_t row : out.layers[0]) {
				CHECK(row == want);
			}
		}
	}

	void test_dig_mask_layout_places_the_cells_in_the_texture()
	{
		const DigMaskLayout a = DigMaskLayout::around(0.5, 70.2, -3.0);
		CHECK(a.ox == -64 && a.oy == 32 && a.oz == -80);  // its centre is (0, 64, -16)
		CHECK(!a.needs_recentre(0.5, 70.2, -3.0));

		// recentred past 32 blocks along x or z, or 16 along y, from the centre
		CHECK(!a.needs_recentre(32.0, 64, -16) && !a.needs_recentre(-32.0, 64, -16));
		CHECK(a.needs_recentre(32.5, 64, -16) && a.needs_recentre(-32.5, 64, -16));
		CHECK(!a.needs_recentre(0, 64, 16.0) && a.needs_recentre(0, 64, 16.5));
		CHECK(!a.needs_recentre(0, 64, -48.0) && a.needs_recentre(0, 64, -48.5));
		CHECK(!a.needs_recentre(0, 80.0, -16) && a.needs_recentre(0, 80.5, -16));
		CHECK(!a.needs_recentre(0, 48.0, -16) && a.needs_recentre(0, 47.5, -16));

		// a cell's texel: 64 layers of 128 x 128 in tiles of 8 x 8, the first layer's corner cell at (0, 0)
		int u = -1, v = -1;
		CHECK(a.texel(-64, 32, -80, u, v) && u == 0 && v == 0);
		CHECK(a.texel(-59, 41, -73, u, v) && u == 133 && v == 135);
		CHECK(a.texel(63, 95, 47, u, v) && u == 1023 && v == 1023);
		CHECK(!a.texel(-65, 32, -80, u, v) && !a.texel(64, 32, -80, u, v));
		CHECK(!a.texel(-64, 31, -80, u, v) && !a.texel(-64, 96, -80, u, v));
		CHECK(!a.texel(-64, 32, -81, u, v) && !a.texel(-64, 32, 48, u, v));

		// the shader's constants: a map in slot 1 (minecraft x 1024) with its grid at source z 16
		const MapSlot       slot{ 1, 16.0f };
		const DigMaskLayout b = DigMaskLayout::around(1030.5, 70.2, -3.0);
		CHECK(b.ox == 960 && b.oy == 32 && b.oz == -80);
		float origin[4], layout[4];
		b.shader_constants(slot, origin, layout);
		CHECK(origin[0] == -2560.0f && origin[1] == 3200.0f && origin[2] == 1296.0f);
		CHECK(std::fabs(origin[3] - 0.025f) < 1e-7f);
		CHECK(layout[0] == 1.0f / 1024.0f && layout[1] == 128.0f && layout[2] == 8.0f && layout[3] == 1.0f);

		// the shader's arithmetic on a point of block (965, 41, -73) (the constants, then the distance over 40, floored) finds the cell that texel() puts at that block
		float s[3];
		mc_to_source(965.5, 41.5, -72.5, slot, s);
		const int i = static_cast<int>(std::floor((s[0] - origin[0]) * origin[3]));
		const int j = static_cast<int>(std::floor((origin[1] - s[1]) * origin[3]));
		const int k = static_cast<int>(std::floor((s[2] - origin[2]) * origin[3]));
		CHECK(i == 5 && j == 7 && k == 9);
		CHECK(b.texel(965, 41, -73, u, v) && u == (k % 8) * 128 + i && v == (k / 8) * 128 + j);
	}

	void test_dig_mask_shader_texel()
	{
		// a map in slot 3 (minecraft x 3072) with its grid at source z -152.5
		const DigMaskLayout layout = DigMaskLayout::around(3200.5, 70.2, -40.7);
		CHECK(layout.ox == 3136 && layout.oy == 32 && layout.oz == -112);
		const MapSlot slot{ 3, -152.5f };
		float         o[4], l[4];
		layout.shader_constants(slot, o, l);

		// the centre of block (x, y, z)'s cell, source units
		const auto centre = [&slot](int x, int y, int z, float p[3]) { mc_to_source(x + .5, y + .5, z + .5, slot, p); };
		float      p[3];
		int        u = -1, v = -1;
		centre(3141, 41, -105, p);
		CHECK(shader_texel(o, l, p, u, v) && u == 133 && v == 135);  // i 5, j 7, k 9
		centre(3263, 95, 15, p);
		CHECK(shader_texel(o, l, p, u, v) && u == 1023 && v == 1023);

		// past each of the layout's edges: false, u and v left alone
		u = v = -1;
		centre(3135, 32, -112, p);
		CHECK(!shader_texel(o, l, p, u, v));
		centre(3136, 96, -112, p);
		CHECK(!shader_texel(o, l, p, u, v));
		centre(3136, 32, 16, p);
		CHECK(!shader_texel(o, l, p, u, v));
		CHECK(u == -1 && v == -1);

		// every 3rd cell in and around the layout: the shader finds the texel texel() puts the cell at
		int wrong = 0, inside = 0;
		for (int x = 3134; x <= 3266; x += 3) {
			for (int y = 30; y <= 97; y += 3) {
				for (int z = -114; z <= 18; z += 3) {
					int tu = -1, tv = -1;
					u = v = -1;
					centre(x, y, z, p);
					const bool want = layout.texel(x, y, z, tu, tv);
					const bool got = shader_texel(o, l, p, u, v);
					wrong += got != want || (got && (u != tu || v != tv)) ? 1 : 0;
					inside += want ? 1 : 0;
				}
			}
		}
		CHECK(wrong == 0);
		CHECK(inside > 0);
	}

	void test_dig_mask_write_section()
	{
		const DigMaskLayout       layout = DigMaskLayout::around(3200.5, 70.2, -40.7);
		std::vector<std::uint8_t> rgba(1024 * 1024 * 4, 0);
		std::uint8_t              bits[512] = {};
		const int                 bit = 3 + 16 * 5 + 256 * 9;  // block (3, 9, 5) of the section
		bits[bit / 8] = static_cast<std::uint8_t>(1u << (bit % 8));
		const auto red = [&rgba](int u, int v) { return rgba[(static_cast<std::size_t>(v) * 1024 + u) * 4]; };

		// section (196, 2, -7): blocks from (3136, 32, -112), the layout's corner
		CHECK(write_section(layout, 196, 2, -7, bits, rgba.data()));
		CHECK(red(131, 133) == 255);  // cell (3139, 41, -107)
		const std::size_t texel = (133 * 1024 + 131) * 4;
		CHECK(rgba[texel + 1] == 0 && rgba[texel + 2] == 0 && rgba[texel + 3] == 0);
		CHECK(mask_texel_dug(rgba[texel], rgba[texel + 2]));
		CHECK(red(132, 133) == 0 && red(131, 134) == 0 && red(3, 5) == 0);
		CHECK(!mask_texel_dug(red(132, 133), rgba[(133 * 1024 + 132) * 4 + 2]));

		// no mask: nothing dug in the section
		CHECK(write_section(layout, 196, 2, -7, nullptr, rgba.data()));
		CHECK(red(131, 133) == 0);

		// a section outside the layout writes nothing
		CHECK(!write_section(layout, 195, 2, -7, bits, rgba.data()));
		CHECK(std::all_of(rgba.begin(), rgba.end(), [](std::uint8_t byte) { return byte == 0; }));
	}

	void test_dig_mask_texel_dug()
	{
		CHECK(mask_texel_dug(255, 0));  // write_section's dug cell
		CHECK(!mask_texel_dug(0, 0));   // not dug, and the black texture
		// what the material system may bind instead of the mask cuts nothing: its error texture's pink, white, grey
		CHECK(!mask_texel_dug(255, 255));
		CHECK(!mask_texel_dug(128, 128));
		CHECK(!mask_texel_dug(191, 0) && mask_texel_dug(192, 63) && !mask_texel_dug(192, 64));  // the shader's 0.75 and 0.25
	}

	void test_dig_mask_has_section()
	{
		// blocks (3136, 32, -112) to (3263, 95, 15): sections 196-203 along x, 2-5 along y, -7-0 along z
		const DigMaskLayout layout = DigMaskLayout::around(3200.5, 70.2, -40.7);
		CHECK(layout.has_section(196, 2, -7) && layout.has_section(203, 5, 0) && layout.has_section(200, 4, -3));
		CHECK(!layout.has_section(195, 2, -7) && !layout.has_section(204, 2, -7));
		CHECK(!layout.has_section(196, 1, -7) && !layout.has_section(196, 6, -7));
		CHECK(!layout.has_section(196, 2, -8) && !layout.has_section(196, 2, 1));

		// the sections in and around the layout: write_section writes the ones it has, and only those
		std::vector<std::uint8_t> rgba(1024 * 1024 * 4, 0);
		int                       wrong = 0, inside = 0;
		for (int sx = 194; sx <= 205; ++sx) {
			for (int sy = 0; sy <= 7; ++sy) {
				for (int sz = -9; sz <= 2; ++sz) {
					const bool has = layout.has_section(sx, sy, sz);
					wrong += write_section(layout, sx, sy, sz, nullptr, rgba.data()) != has ? 1 : 0;
					inside += has ? 1 : 0;
				}
			}
		}
		CHECK(wrong == 0);
		CHECK(inside == 8 * 4 * 8);
	}

	void test_dig_mask_needs_rebuild()
	{
		// the first build always lays the image out: it makes the texture
		CHECK(mask_needs_rebuild(false, false, false));
		CHECK(mask_needs_rebuild(false, false, true));
		// no cell dug in the image and none in the new layout: the layout alone moves (no clear, no upload)
		CHECK(!mask_needs_rebuild(true, false, false));
		// a section with cells dug in the new layout is written in
		CHECK(mask_needs_rebuild(true, false, true));
		// cells dug in the image go, even when the new layout has none
		CHECK(mask_needs_rebuild(true, true, false));
		CHECK(mask_needs_rebuild(true, true, true));
	}

	void test_dig_mask_upload_before_clip()
	{
		// the first cell dug in the mask: the whole image goes up before the shader takes it
		CHECK(mask_needs_upload_before_clip(true, false, false));
		// a recentre has just uploaded it whole: once is enough
		CHECK(!mask_needs_upload_before_clip(true, false, true));
		// the shader clipped by it the frame before: the sections that changed are enough
		CHECK(!mask_needs_upload_before_clip(true, true, false));
		CHECK(!mask_needs_upload_before_clip(true, true, true));
		// no cell dug in the mask: the shader doesn't take it, nothing to upload
		CHECK(!mask_needs_upload_before_clip(false, false, false));
		CHECK(!mask_needs_upload_before_clip(false, true, false));
	}

	void test_dug_cells()
	{
		// section (-1, 0, 2) (blocks from -16, 0, 32): bit 0 is block (0, 0, 0) of it, bit 561 (byte 70, bit 1) block (1, 2, 3)
		std::uint8_t bits[512] = {};
		bits[0] = 1u;
		bits[70] = 1u << 1;
		DugCells          cells;
		std::vector<Cell> changed;
		cells.set(-1, 0, 2, bits, changed);
		CHECK(changed.size() == 2);
		CHECK(cells.count() == 2);
		CHECK(cells.dug(-16, 0, 32) && cells.dug(-15, 2, 35));
		CHECK(!cells.dug(-16, 0, 33) && !cells.dug(0, 0, 32));

		// the same mask changes nothing; a cell filled again is the one change
		changed.clear();
		cells.set(-1, 0, 2, bits, changed);
		CHECK(changed.empty());
		bits[70] = 0;
		cells.set(-1, 0, 2, bits, changed);
		CHECK(changed == (std::vector<Cell>{ { -15, 2, 35 } }));
		CHECK(cells.count() == 1);

		// a box only touching a cell doesn't have it; one inside it does
		std::vector<Cell> in;
		const float       touch_lo[3] = { -15, 0, 32 }, touch_hi[3] = { -14, 1, 33 };
		cells.cells_in(touch_lo, touch_hi, in);
		CHECK(in.empty());
		const float inside_lo[3] = { -15.5f, .5f, 32.5f }, inside_hi[3] = { -15.4f, .6f, 32.6f };
		cells.cells_in(inside_lo, inside_hi, in);
		CHECK(in == (std::vector<Cell>{ { -16, 0, 32 } }));

		// no mask: the section has nothing dug
		changed.clear();
		cells.set(-1, 0, 2, nullptr, changed);
		CHECK(changed.size() == 1);
		CHECK(cells.count() == 0);

		// cells around (0.5, 0.5, 0.5): (1, 0, 0) and (5, 0, 0) and (0, 0, 10) in section (0, 0, 0), (32, 0, 0) in (2, 0, 0)
		std::uint8_t near_bits[512] = {}, far_bits[512] = {};
		near_bits[0] = 1u << 5 | 1u << 1;
		near_bits[20] = 1u;
		far_bits[0] = 1u;
		cells.set(0, 0, 0, near_bits, changed);
		cells.set(2, 0, 0, far_bits, changed);
		std::vector<Cell> nearest;
		cells.nearest({ .5, .5, .5 }, 24.0, 2, nearest);
		CHECK(nearest == (std::vector<Cell>{ { 1, 0, 0 }, { 5, 0, 0 } }));
		cells.nearest({ .5, .5, .5 }, 24.0, 64, nearest);
		CHECK(nearest.size() == 3 && nearest[2] == (Cell{ 0, 0, 10 }));

		// a source box is dug when every cell its volume reaches into is (slot 1, grid at source z 16: cell (1025, 0, -1)
		// is source x 40..80, y 0..40, z 16..56)
		const MapSlot slot{ 1, 16.0f };
		std::uint8_t  column[512] = {};
		column[(1 + 16 * 15) / 8] = 1u << ((1 + 16 * 15) % 8);              // block (1, 0, 15) of section (64, 0, -1)
		column[(1 + 16 * 15 + 256) / 8] = 1u << ((1 + 16 * 15 + 256) % 8);  // and the one above it
		cells.set(64, 0, -1, column, changed);
		const auto  dug_cell = [&cells](int x, int y, int z) { return cells.dug(x, y, z); };
		const float in_lo[3] = { 44, 4, 17 }, in_hi[3] = { 76, 36, 96 };
		CHECK(source_box_dug(in_lo, in_hi, slot, dug_cell));
		const float edge_lo[3] = { 40, 0, 16 }, edge_hi[3] = { 80, 40, 96 };  // exactly the two cells
		CHECK(source_box_dug(edge_lo, edge_hi, slot, dug_cell));
		const float over_lo[3] = { 44, 4, 17 }, over_hi[3] = { 81, 36, 56 };  // a unit into cell (1026, 0, -1)
		CHECK(!source_box_dug(over_lo, over_hi, slot, dug_cell));
		const float up_lo[3] = { 44, 4, 17 }, up_hi[3] = { 76, 36, 97 };  // a unit into cell (1025, 2, -1)
		CHECK(!source_box_dug(up_lo, up_hi, slot, dug_cell));
	}

	void test_dug_pending()
	{
		using Key = std::array<int, 3>;
		DugPending<Key>  pending;
		std::vector<Key> emptied;
		const auto       empty = [&emptied](const Key& key) { emptied.push_back(key); };
		CHECK(pending.expire(100.0, empty) == 0 && emptied.empty());  // nothing pending: nothing to empty, whenever

		// a kRenClearAll at 10 s finds two sections with cells dug
		pending.start(10.0);
		pending.add({ 1, 2, 3 });
		pending.add({ 4, 5, 6 });
		CHECK(pending.size() == 2 && pending.pending({ 1, 2, 3 }) && pending.pending({ 4, 5, 6 }) && !pending.pending({ 7, 8, 9 }));
		CHECK(pending.expire(11.999, empty) == 0 && emptied.empty() && pending.size() == 2);  // within the grace
		// one comes again (its kRenDug): no longer pending, once
		CHECK(pending.settle({ 1, 2, 3 }));
		CHECK(!pending.settle({ 1, 2, 3 }));
		CHECK(!pending.settle({ 7, 8, 9 }));  // never was
		CHECK(!pending.pending({ 1, 2, 3 }) && pending.size() == 1);
		// the grace ends (10 + 2 s): the other is emptied, and only once
		CHECK(pending.expire(12.0, empty) == 1);
		CHECK(emptied.size() == 1 && emptied[0] == (Key{ 4, 5, 6 }));
		CHECK(pending.size() == 0 && pending.expire(20.0, empty) == 0 && emptied.size() == 1);

		// every section comes again: nothing emptied when the grace ends
		pending.start(21.0);
		pending.add({ 1, 1, 1 });
		CHECK(pending.settle({ 1, 1, 1 }) && pending.size() == 0);
		CHECK(pending.expire(30.0, empty) == 0 && emptied.size() == 1);

		// another kRenClearAll during the grace starts it over, with the sections it finds dug (the caller adds them)
		pending.start(30.0);
		pending.add({ 1, 1, 1 });
		pending.add({ 2, 2, 2 });
		pending.start(31.5);
		pending.add({ 2, 2, 2 });
		CHECK(!pending.pending({ 1, 1, 1 }) && pending.pending({ 2, 2, 2 }));
		CHECK(pending.expire(33.0, empty) == 0);  // 30 + 2 s is past, 31.5 + 2 s isn't
		CHECK(pending.expire(33.5, empty) == 1 && emptied.size() == 2 && emptied[1] == (Key{ 2, 2, 2 }));

		// another map ends the grace at once
		pending.start(40.0);
		pending.add({ 3, 3, 3 });
		CHECK(pending.end(empty) == 1 && emptied.size() == 3 && emptied[2] == (Key{ 3, 3, 3 }) && pending.size() == 0);
		CHECK(pending.end(empty) == 0 && emptied.size() == 3);

		// shutdown drops them, emptying none
		pending.start(50.0);
		pending.add({ 5, 5, 5 });
		pending.drop();
		CHECK(pending.size() == 0 && pending.expire(60.0, empty) == 0 && emptied.size() == 3);
	}

	void test_triangle_clip()
	{
		const float                     lo[3] = { 0, 0, 0 }, hi[3] = { 1, 1, 1 };
		std::vector<ColPrimitives::Tri> out;

		// a big triangle over the box's middle: what's left is the square of it in the box, wound and flagged as it was
		clip_triangle({ { -4, .5f, -4, -4, .5f, 8, 8, .5f, -4 }, 0x305u }, lo, hi, out);
		CHECK(!out.empty());
		float area = 0.0f;
		for (const auto& tri : out) {
			CHECK(tri.flags == 0x305u);
			for (int k = 0; k < 9; ++k) {
				CHECK(tri.v[k] >= -1e-5f && tri.v[k] <= 1.0f + 1e-5f);
			}
			const auto n = triangle_normal(tri.v);
			CHECK(n[1] > 1e-6f);
			area += length(n) / 2.0f;
		}
		CHECK(near(area, 1.0f, 1e-4f));

		// one inside the box is itself; one outside it is nothing
		const ColPrimitives::Tri inside{ { .25f, .25f, .25f, .25f, .25f, .75f, .75f, .25f, .25f }, 0 };
		out.clear();
		clip_triangle(inside, lo, hi, out);
		CHECK(out.size() == 1 && std::equal(inside.v, inside.v + 9, out[0].v) && out[0].flags == 0);
		out.clear();
		clip_triangle({ { 2, .5f, 0, 2, .5f, 1, 3, .5f, 0 }, 0 }, lo, hi, out);
		CHECK(out.empty());
	}
}

// ---- campaign models ------------------------------------------------------------------------------------------------

namespace
{
	void put32(std::vector<std::uint8_t>& data, std::size_t at, std::uint32_t value)
	{
		for (std::size_t i = 0; i < 4; ++i) {
			data[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
		}
	}

	/// the bytes of a .mdl with this checksum, includes and anim block, its header laid out as studiomdl does
	std::vector<std::uint8_t> studio_model(std::uint32_t checksum, const std::vector<std::string>& includes = {}, const std::string& anim_block = "",
		int version = 48)
	{
		std::vector<std::uint8_t> data(408, 0);
		std::memcpy(data.data(), "IDST", 4);
		put32(data, 4, static_cast<std::uint32_t>(version));
		put32(data, 8, checksum);
		const std::size_t groups = data.size();
		data.resize(groups + 8 * includes.size(), 0);
		put32(data, 336, static_cast<std::uint32_t>(includes.size()));
		put32(data, 340, static_cast<std::uint32_t>(groups));
		for (std::size_t i = 0; i < includes.size(); ++i) {
			const std::size_t group = groups + 8 * i;
			put32(data, group + 4, static_cast<std::uint32_t>(data.size() - group));
			data.insert(data.end(), includes[i].begin(), includes[i].end());
			data.push_back(0);
		}
		if (!anim_block.empty()) {
			put32(data, 348, static_cast<std::uint32_t>(data.size()));
			put32(data, 352, 2);
			data.insert(data.end(), anim_block.begin(), anim_block.end());
			data.push_back(0);
		}
		put32(data, 76, static_cast<std::uint32_t>(data.size()));
		return data;
	}

	/// a companion file carrying a checksum at an offset
	std::vector<std::uint8_t> companion_file(std::size_t offset, std::uint32_t checksum)
	{
		std::vector<std::uint8_t> data(64, 0x5a);
		put32(data, offset, checksum);
		return data;
	}

	/// the game's files under their path ids, for the planner
	class FakeModelFiles final : public ModelFileSource
	{
	public:
		void add(const char* path_id, const std::string& name, std::vector<std::uint8_t> data) { files_[{ path_id, name }] = std::move(data); }

		void remove(const char* path_id, const std::string& name) { files_.erase({ path_id, name }); }

		/// a file the file system says is bigger than what a read gives
		void cut_short(const char* path_id, const std::string& name) { short_.insert({ path_id, name }); }

		bool exists(const std::string& name, const char* path_id) const override { return files_.count({ path_id, name }) != 0; }

		long size(const std::string& name, const char* path_id) const override
		{
			const auto found = files_.find({ path_id, name });
			return found == files_.end() ? -1 : static_cast<long>(found->second.size()) + (short_.count({ path_id, name }) ? 1 : 0);
		}

		bool read(const std::string& name, const char* path_id, std::vector<std::uint8_t>& out, std::size_t max_bytes) const override
		{
			const auto found = files_.find({ path_id, name });
			if (found == files_.end()) {
				return false;
			}
			const std::size_t count = max_bytes && max_bytes < found->second.size() ? max_bytes : found->second.size();
			out.assign(found->second.begin(), found->second.begin() + static_cast<std::ptrdiff_t>(count));
			return true;
		}

	private:
		std::map<std::pair<std::string, std::string>, std::vector<std::uint8_t>> files_;
		std::set<std::pair<std::string, std::string>>                            short_;
	};

	/// a .mdl with the companions a character has (a .vvd and three .vtx), all with its checksum
	void add_character(FakeModelFiles& files, const char* path_id, const std::string& name, std::uint32_t checksum,
		const std::vector<std::string>& includes = {}, const std::string& anim_block = "")
	{
		const std::string stem = name.substr(0, name.size() - 4);
		files.add(path_id, name, studio_model(checksum, includes, anim_block));
		files.add(path_id, stem + ".vvd", companion_file(8, checksum));
		for (const char* vtx : { ".dx90.vtx", ".dx80.vtx", ".sw.vtx" }) {
			files.add(path_id, stem + vtx, companion_file(16, checksum));
		}
	}

	bool planned(const ModelSetPlan& plan, const std::string& name, const std::string& source, std::uint32_t checksum)
	{
		return std::any_of(plan.models.begin(), plan.models.end(),
			[&](const PlannedModel& model) { return model.name == name && model.source == source && model.checksum == checksum; });
	}

	bool has_file(const ModelSetPlan& plan, const std::string& name)
	{
		return std::any_of(plan.files.begin(), plan.files.end(), [&](const PlannedFile& file) { return file.name == name; });
	}

	bool skipped(const ModelSetPlan& plan, const std::string& root, const std::string& why)
	{
		return std::any_of(plan.skipped.begin(), plan.skipped.end(),
			[&](const std::string& line) { return line.rfind(root + "'s closure", 0) == 0 && line.find(why) != std::string::npos; });
	}

	void test_read_studio_header()
	{
		const auto data = studio_model(0x4314b420, { "models/Alyx_Animations.mdl", "models\\humans\\female_shared.mdl" }, "models/Alyx_gestures.ani");
		const auto header = read_studio_header(data.data(), data.size());
		CHECK(header && header->checksum == 0x4314b420);
		CHECK(header && header->includes == std::vector<std::string>({ "models/alyx_animations.mdl", "models/humans/female_shared.mdl" }));
		CHECK(header && header->anim_block == "models/alyx_gestures.ani");
		const auto bare = studio_model(1, {}, "", 44);
		const auto bare_header = read_studio_header(bare.data(), bare.size());
		CHECK(bare_header && bare_header->includes.empty() && bare_header->anim_block.empty());

		CHECK(!read_studio_header(nullptr, 0));
		CHECK(!read_studio_header(data.data(), 407));  // shorter than the header
		auto not_studio = data;
		not_studio[0] = 'X';
		CHECK(!read_studio_header(not_studio.data(), not_studio.size()));
		const auto newer = studio_model(1, {}, "", 50);
		CHECK(!read_studio_header(newer.data(), newer.size()));
		auto cut = data;  // a file cut short isn't the length its header says
		cut.pop_back();
		CHECK(!read_studio_header(cut.data(), cut.size()));
		auto far = data;
		put32(far, 408 + 4, 1u << 20);  // the first include's name past the end
		CHECK(!read_studio_header(far.data(), far.size()));
		const auto outside = studio_model(1, { "materials/x.mdl" });
		CHECK(!read_studio_header(outside.data(), outside.size()));
		const auto up = studio_model(1, { "models/../../x.mdl" });
		CHECK(!read_studio_header(up.data(), up.size()));
		const auto wrong_block = studio_model(1, {}, "models/x.vvd");
		CHECK(!read_studio_header(wrong_block.data(), wrong_block.size()));
		auto many = studio_model(1);
		put32(many, 336, 100000);
		CHECK(!read_studio_header(many.data(), many.size()));
	}

	void test_companion_checksum()
	{
		CHECK(companion_checksum({ ".vvd", 8 }, companion_file(8, 0x8c31975c)) == 0x8c31975cu);
		CHECK(companion_checksum({ ".dx90.vtx", 16 }, companion_file(16, 7)) == 7u);
		CHECK(companion_checksum({ ".phy", 12 }, companion_file(12, 9)) == 9u);
		CHECK(!companion_checksum({ ".vvd", 8 }, std::vector<std::uint8_t>(11, 0)));
	}

	void test_model_set_of()
	{
		CHECK(model_set_of("d2_lostcoast") == ModelSet::kLostCoast);
		CHECK(model_set_of("maps/D2_LostCoast.bsp") == ModelSet::kLostCoast);
		CHECK(model_set_of("d2_lostcoast_2") == ModelSet::kHalfLife2);
		CHECK(model_set_of("d1_trainstation_05") == ModelSet::kHalfLife2);
		CHECK(model_set_of("ep1_citadel_00") == ModelSet::kEpisodeOne);
		CHECK(model_set_of("maps\\ep2_outland_01.bsp") == ModelSet::kEpisodeTwo);
		CHECK(model_set_of(nullptr) == ModelSet::kHalfLife2);
		CHECK(model_set_sources(ModelSet::kEpisodeTwo).empty());
		CHECK(model_set_sources(ModelSet::kLostCoast).size() == 2 && std::strcmp(model_set_sources(ModelSet::kLostCoast)[0], "hc_lc") == 0);
	}

	void test_plan_half_life_2()
	{
		FakeModelFiles files;
		add_character(files, "hc_hl2", "models/alyx.mdl", 1, { "models/alyx_animations.mdl", "models/humans/female_shared.mdl" });
		files.add("hc_hl2", "models/alyx_animations.mdl", studio_model(2, {}, "models/Alyx_Animations.ani"));
		files.add("hc_hl2", "models/alyx_animations.ani", std::vector<std::uint8_t>(100, 1));
		files.add("hc_hl2", "models/humans/female_shared.mdl", studio_model(3));
		add_character(files, "hc_ep2", "models/alyx.mdl", 11, { "models/alyx_animations.mdl", "models/humans/female_shared.mdl" });
		files.add("hc_ep2", "models/alyx_animations.mdl", studio_model(12, {}, "models/alyx_animations.ani"));
		files.add("hc_ep2", "models/alyx_animations.ani", std::vector<std::uint8_t>(90, 2));
		// a rig without animation blocks whose episode two version has some: that .ani goes with no half-life 2 file
		add_character(files, "hc_hl2", "models/blackout.mdl", 5);
		add_character(files, "hc_ep2", "models/blackout.mdl", 6, {}, "models/blackout.ani");
		files.add("hc_ep2", "models/blackout.ani", std::vector<std::uint8_t>(10, 3));
		// the same version in both: the merged mount has it already
		add_character(files, "hc_hl2", "models/dog.mdl", 7);
		add_character(files, "hc_ep1", "models/dog.mdl", 7);

		const auto plan = plan_model_set(ModelSet::kHalfLife2, files);
		CHECK(plan.skipped.empty() && plan.roots == 3);
		CHECK(plan.models.size() == 3);
		CHECK(planned(plan, "models/alyx.mdl", "hc_hl2", 1) && planned(plan, "models/alyx_animations.mdl", "hc_hl2", 2) &&
			planned(plan, "models/blackout.mdl", "hc_hl2", 5));
		CHECK(plan.files.size() == 12);  // alyx's .mdl, .vvd and 3 .vtx; alyx_animations' .mdl and .ani; blackout's 5
		CHECK(has_file(plan, "models/alyx.dx80.vtx") && has_file(plan, "models/alyx_animations.ani") && !has_file(plan, "models/blackout.ani"));
		CHECK(!has_file(plan, "models/humans/female_shared.mdl") && !has_file(plan, "models/dog.mdl"));
		CHECK(std::all_of(plan.files.begin(), plan.files.end(), [](const PlannedFile& file) { return std::strcmp(file.source, "hc_hl2") == 0; }));
		const auto ani = std::find_if(plan.files.begin(), plan.files.end(), [](const PlannedFile& file) { return file.name == "models/alyx_animations.ani"; });
		CHECK(ani != plan.files.end() && ani->data == std::vector<std::uint8_t>(100, 1));
		CHECK(plan.bytes() > 0);

		const auto ep2 = plan_model_set(ModelSet::kEpisodeTwo, files);
		CHECK(ep2.models.empty() && ep2.files.empty() && ep2.skipped.empty());

		// a set whose own content has none of the characters (gameinfo.txt doesn't mount it) says so
		const FakeModelFiles nothing;
		const auto           none = plan_model_set(ModelSet::kHalfLife2, nothing);
		CHECK(none.models.empty() && none.roots == 0 && none.skipped.size() == 1 && none.skipped[0].find("none of the characters is in hc_hl2") == 0);
		CHECK(plan_model_set(ModelSet::kEpisodeTwo, nothing).skipped.empty());
	}

	void test_plan_episode_one_and_lost_coast()
	{
		FakeModelFiles files;
		add_character(files, "hc_ep1", "models/alyx.mdl", 21);
		add_character(files, "hc_ep2", "models/alyx.mdl", 22);
		add_character(files, "hc_hl2", "models/alyx.mdl", 20);
		add_character(files, "hc_hl2", "models/advisor.mdl", 31);  // episode one has none: its own is half-life 2's
		add_character(files, "hc_ep2", "models/advisor.mdl", 32);
		add_character(files, "hc_hl2", "models/lamarr.mdl", 41);   // nobody's over it
		add_character(files, "hc_ep1", "models/stalker.mdl", 51);  // nor over episode one's
		add_character(files, "hc_hl2", "models/stalker.mdl", 50);
		// lost coast's soldiers' animations, over half-life 2's
		add_character(files, "hc_hl2", "models/combine_soldier.mdl", 63, { "models/combine_soldier_anims.mdl" });
		files.add("hc_hl2", "models/combine_soldier_anims.mdl", studio_model(62, {}, "models/combine_soldier_anims.ani"));
		files.add("hc_hl2", "models/combine_soldier_anims.ani", std::vector<std::uint8_t>(30, 4));
		files.add("hc_lc", "models/combine_soldier_anims.mdl", studio_model(61, {}, "models/combine_soldier_anims.ani"));
		files.add("hc_lc", "models/combine_soldier_anims.ani", std::vector<std::uint8_t>(40, 5));

		const auto ep1 = plan_model_set(ModelSet::kEpisodeOne, files);
		CHECK(ep1.skipped.empty() && ep1.models.size() == 2);
		CHECK(planned(ep1, "models/advisor.mdl", "hc_hl2", 31) && planned(ep1, "models/alyx.mdl", "hc_ep1", 21));

		const auto hl2 = plan_model_set(ModelSet::kHalfLife2, files);
		CHECK(hl2.skipped.empty() && hl2.models.size() == 3);  // advisor, alyx and stalker: the episodes' are over them
		CHECK(planned(hl2, "models/stalker.mdl", "hc_hl2", 50) && !has_file(hl2, "models/combine_soldier_anims.mdl"));

		const auto lost_coast = plan_model_set(ModelSet::kLostCoast, files);
		CHECK(lost_coast.skipped.empty() && lost_coast.models.size() == 4);
		CHECK(planned(lost_coast, "models/combine_soldier_anims.mdl", "hc_lc", 61) && !has_file(lost_coast, "models/combine_soldier.mdl"));
		const auto ani = std::find_if(lost_coast.files.begin(), lost_coast.files.end(),
			[](const PlannedFile& file) { return file.name == "models/combine_soldier_anims.ani"; });
		CHECK(ani != lost_coast.files.end() && std::strcmp(ani->source, "hc_lc") == 0 && ani->data.size() == 40);
	}

	void test_plan_leaves_out_broken_closures()
	{
		const auto with = [](const std::function<void(FakeModelFiles&)>& change) {
			FakeModelFiles files;
			add_character(files, "hc_hl2", "models/alyx.mdl", 1, { "models/alyx_gestures.mdl" });
			files.add("hc_hl2", "models/alyx_gestures.mdl", studio_model(2, {}, "models/alyx_gestures.ani"));
			files.add("hc_hl2", "models/alyx_gestures.ani", std::vector<std::uint8_t>(20, 1));
			add_character(files, "hc_ep2", "models/alyx.mdl", 11, { "models/alyx_gestures.mdl" });
			files.add("hc_ep2", "models/alyx_gestures.mdl", studio_model(12));
			add_character(files, "hc_hl2", "models/lamarr.mdl", 3);
			add_character(files, "hc_ep2", "models/lamarr.mdl", 13);
			change(files);
			return plan_model_set(ModelSet::kHalfLife2, files);
		};
		const auto whole = with([](FakeModelFiles&) {});
		CHECK(whole.skipped.empty() && whole.models.size() == 3);

		const auto mismatch = with([](FakeModelFiles& files) { files.add("hc_hl2", "models/alyx.vvd", companion_file(8, 99)); });
		CHECK(skipped(mismatch, "models/alyx.mdl", "models/alyx.vvd's checksum 00000063 isn't the .mdl's 00000001"));
		CHECK(!has_file(mismatch, "models/alyx.mdl") && !has_file(mismatch, "models/alyx_gestures.mdl") && planned(mismatch, "models/lamarr.mdl", "hc_hl2", 3));

		const auto no_block = with([](FakeModelFiles& files) { files.remove("hc_hl2", "models/alyx_gestures.ani"); });
		CHECK(skipped(no_block, "models/alyx.mdl", "models/alyx_gestures.ani isn't in hc_hl2") && no_block.models.size() == 1);

		// leftovers are found by <stem>.ani: a copy streaming from another name couldn't be taken out again
		const auto elsewhere = with([](FakeModelFiles& files) { files.add("hc_hl2", "models/alyx_gestures.mdl", studio_model(2, {}, "models/elsewhere.ani")); });
		CHECK(skipped(elsewhere, "models/alyx.mdl", "models/alyx_gestures.mdl streams its animations from models/elsewhere.ani, not models/alyx_gestures.ani"));
		CHECK(elsewhere.models.size() == 1);

		const auto leak = with([](FakeModelFiles& files) { files.add("hc_ep2", "models/alyx.phy", companion_file(12, 11)); });
		CHECK(skipped(leak, "models/alyx.mdl", "models/alyx.phy isn't in hc_hl2 but hc_ep2 has one"));
		// lost coast's pak comes after half-life 2's on GAME but isn't half-life 2's own content: it slips in all the same
		const auto later = with([](FakeModelFiles& files) { files.add("hc_lc", "models/alyx.phy", companion_file(12, 1)); });
		CHECK(skipped(later, "models/alyx.mdl", "models/alyx.phy isn't in hc_hl2 but hc_lc has one"));

		const auto cut = with([](FakeModelFiles& files) { files.cut_short("hc_hl2", "models/alyx_gestures.ani"); });
		CHECK(skipped(cut, "models/alyx.mdl", "couldn't be read whole") && !has_file(cut, "models/alyx_gestures.ani"));

		const auto broken = with([](FakeModelFiles& files) {
			auto data = studio_model(1);
			data.pop_back();
			files.add("hc_hl2", "models/alyx.mdl", data);
		});
		CHECK(skipped(broken, "models/alyx.mdl", "isn't a studio model this can read"));

		// a model the lists don't know would be copied (tools/game/campaign_models.py says to add it)
		const auto unlisted = with([](FakeModelFiles& files) {
			add_character(files, "hc_hl2", "models/gman.mdl", 4, { "models/breen_anims.mdl" });
			files.add("hc_hl2", "models/breen_anims.mdl", studio_model(5));
			files.add("hc_ep2", "models/breen_anims.mdl", studio_model(6));
		});
		CHECK(skipped(unlisted, "models/gman.mdl", "models/breen_anims.mdl would be copied but isn't in CAMPAIGN_MODELS_SWAPPED"));
		CHECK(unlisted.models.size() == 3);

		// a model another closure has whole stays when one with it falls apart, and the skipped line says so
		const auto shared = with([](FakeModelFiles& files) {
			add_character(files, "hc_hl2", "models/kleiner.mdl", 7, { "models/kleiner_gestures.mdl" });
			add_character(files, "hc_hl2", "models/kleiner_monitor.mdl", 8, { "models/kleiner_gestures.mdl", "models/kleiner_lost.mdl" });
			files.add("hc_hl2", "models/kleiner_gestures.mdl", studio_model(9));
			files.add("hc_ep2", "models/kleiner_gestures.mdl", studio_model(10));
		});
		CHECK(skipped(shared, "models/kleiner_monitor.mdl", "models/kleiner_lost.mdl isn't in the campaign's own content"));
		CHECK(skipped(shared, "models/kleiner_monitor.mdl", "it still gets models/kleiner_gestures.mdl of this set through other characters"));
		CHECK(planned(shared, "models/kleiner_gestures.mdl", "hc_hl2", 9) && shared.skipped.size() == 1);
		CHECK(!skipped(mismatch, "models/alyx.mdl", "still gets"));
	}

	void test_campaign_model_names()
	{
		const auto files = campaign_model_files();
		CHECK(files.size() == std::size(CAMPAIGN_MODELS_SWAPPED) * (2 + std::size(MODEL_COMPANIONS)));
		CHECK(std::count(files.begin(), files.end(), "models/alyx_gestures.ani") == 1 && std::count(files.begin(), files.end(), "models/zombie/classic.sw.vtx") == 1);
		const auto flushed = campaign_models_to_flush();
		CHECK(flushed.size() == std::size(CAMPAIGN_MODELS_SWAPPED) + std::size(CAMPAIGN_MODEL_DEPENDENTS));
		CHECK(std::count(flushed.begin(), flushed.end(), "models/gman_high.mdl") == 1 && std::count(flushed.begin(), flushed.end(), "models/alyx.mdl") == 1);
		for (const char* character : CAMPAIGN_CHARACTERS) {
			CHECK(std::string(character).rfind("models/", 0) == 0);
		}

		CHECK(model_of_file("models/Alyx_Gestures.ani") == std::string("models/alyx_gestures.mdl"));
		CHECK(model_of_file("models\\zombie\\classic.dx90.vtx") == std::string("models/zombie/classic.mdl"));
		CHECK(model_of_file("models/gman.mdl") == std::string("models/gman.mdl"));
		CHECK(!model_of_file("halfcraft-campaign-models.txt"));
		CHECK(models_of_files({ "models/alyx.vvd", "models/dog.ani", "models/alyx.mdl", "notes.txt" }) == std::vector<std::string>({ "models/alyx.mdl", "models/dog.mdl" }));
		CHECK(normal_model_name("Models\\Humans\\Male_Shared.MDL") == "models/humans/male_shared.mdl");
		CHECK(checksum_text(0x4314b420) == "4314b420" && checksum_text(7) == "00000007");
	}

	void test_campaign_model_state()
	{
		CampaignModelState state;
		state.list_hash = campaign_model_list_hash();
		state.set = ModelSet::kLostCoast;
		state.files.push_back({ "models/alyx.mdl", 480536, "hc_hl2", 480536 });
		state.files.push_back({ "models/combine_soldier_anims.ani", 1234, "hc_lc", 1234 });
		state.models.push_back({ "models/alyx.mdl", "hc_hl2", 0x4314b420 });
		const std::string text = format_campaign_model_state(state);
		const auto        read = parse_campaign_model_state(text);
		CHECK(read && read->list_hash == state.list_hash && read->set == ModelSet::kLostCoast);
		CHECK(read && read->files.size() == 2 && read->files[1].name == "models/combine_soldier_anims.ani" && read->files[1].source == "hc_lc" &&
			read->files[1].size == 1234 && read->files[1].source_size == 1234);
		CHECK(read && read->models.size() == 1 && read->models[0].checksum == 0x4314b420u && read->models[0].source == "hc_hl2");

		std::string crlf;
		for (const char c : text) {
			crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
		}
		CHECK(parse_campaign_model_state(crlf).has_value());
		CampaignModelState none;
		none.list_hash = 1;
		const auto empty = parse_campaign_model_state(format_campaign_model_state(none));
		CHECK(empty && empty->set == ModelSet::kEpisodeTwo && empty->files.empty());

		const std::string head = std::string(CAMPAIGN_MODEL_STATE_HEADER) + "\n";
		CHECK(!parse_campaign_model_state(""));
		CHECK(!parse_campaign_model_state("halfcraft campaign models 1\nlist 00000001\nset hl2\n"));  // an older format
		CHECK(!parse_campaign_model_state(head + "list 00000001\n"));                                 // no set
		CHECK(!parse_campaign_model_state(head + "list 00000001\nset ep3\n"));                        // no such set
		CHECK(!parse_campaign_model_state(head + "list 00000001\nset hl2\nfile x 1\n"));               // a line cut short
		CHECK(!parse_campaign_model_state(head + "list 0000000g\nset hl2\n"));
		CHECK(!parse_campaign_model_state(head + "list 00000001\nset hl2\nfile models/a.mdl -5 hc_hl2 3\n"));
		CHECK(!parse_campaign_model_state(head + "list 00000001\nset hl2\nmodel models/a.mdl 00000001\n"));  // no source
		CHECK(campaign_model_list_hash() != 0);
	}

	void test_stale_campaign_models()
	{
		FakeModelFiles files;
		add_character(files, "hc_hl2", "models/alyx.mdl", 1);
		add_character(files, "hc_ep2", "models/alyx.mdl", 11);
		CampaignModelState state;
		state.set = ModelSet::kHalfLife2;
		state.files.push_back({ "models/alyx.mdl", files.size("models/alyx.mdl", "hc_hl2"), "hc_hl2", files.size("models/alyx.mdl", "hc_hl2") });
		state.models.push_back({ "models/alyx.mdl", "hc_hl2", 1 });
		CHECK(!stale_campaign_models(state, files));

		auto bigger = files;
		bigger.add("hc_hl2", "models/alyx.mdl", studio_model(1, { "models/alyx_more.mdl" }));
		CHECK(stale_campaign_models(state, bigger) == std::string("models/alyx.mdl under hc_hl2 isn't the size it was"));
		auto same_size = files;  // valve's update kept the size and changed the checksum
		same_size.add("hc_hl2", "models/alyx.mdl", studio_model(2));
		CHECK(stale_campaign_models(state, same_size) == std::string("models/alyx.mdl under hc_hl2 isn't the version copied"));
		auto caught_up = files;  // episode two's is half-life 2's now: nothing to copy any more
		caught_up.add("hc_ep2", "models/alyx.mdl", studio_model(1));
		CHECK(stale_campaign_models(state, caught_up) == std::string("models/alyx.mdl is the merged mount's version now"));
	}

	void test_campaign_model_swap()
	{
		ModelSetPlan plan;
		plan.files.push_back({ "models/alyx.mdl", "hc_hl2", std::vector<std::uint8_t>(100, 1) });
		plan.files.push_back({ "models/alyx.vvd", "hc_hl2", std::vector<std::uint8_t>(50, 2) });
		plan.files.push_back({ "models/combine_soldier_anims.mdl", "hc_lc", std::vector<std::uint8_t>(70, 3) });
		CampaignModelState old;
		old.set = ModelSet::kHalfLife2;
		old.files.push_back({ "models/alyx.mdl", 100, "hc_hl2", 100 });
		old.files.push_back({ "models/alyx.vvd", 50, "hc_hl2", 50 });
		old.files.push_back({ "models/gman.mdl", 30, "hc_hl2", 30 });             // not wanted: goes
		old.files.push_back({ "models/from_older_lists.mdl", 9, "hc_hl2", 9 });  // of an older list: goes too
		std::map<std::string, std::vector<std::uint8_t>> folder{
			{ "models/alyx.mdl", std::vector<std::uint8_t>(100, 1) },  // the plan's bytes: stays
			{ "models/alyx.vvd", std::vector<std::uint8_t>(50, 9) },   // the plan's size, other bytes (a valve update): comes again
			{ "models/gman.mdl", std::vector<std::uint8_t>(30, 1) },
			{ "models/from_older_lists.mdl", std::vector<std::uint8_t>(9, 1) },
			{ "models/dog.ani", std::vector<std::uint8_t>(5, 1) },  // a stray
		};
		int                       reads = 0;
		const CampaignModelFolder view{
			[&folder](const std::string& name) {
				const auto found = folder.find(name);
				return found == folder.end() ? -1L : static_cast<long>(found->second.size());
			},
			[&folder, &reads](const std::string& name, const std::vector<std::uint8_t>& data) {
				++reads;
				const auto found = folder.find(name);
				return found != folder.end() && found->second == data;
			},
		};
		const auto swap = plan_campaign_model_swap(old, plan, view);
		CHECK(swap.keep == std::vector<std::string>({ "models/alyx.mdl" }));
		CHECK(swap.write == std::vector<std::size_t>({ 1, 2 }));
		CHECK(reads == 2);  // only the files of the plan's size are read
		auto removed = swap.remove;
		std::sort(removed.begin(), removed.end());
		CHECK(removed == std::vector<std::string>({ "models/alyx.vvd", "models/dog.ani", "models/from_older_lists.mdl", "models/gman.mdl" }));

		// no state at all: whatever of the lists is there goes but what's byte for byte the plan's
		const auto unknown = plan_campaign_model_swap(std::nullopt, plan, view);
		CHECK(unknown.keep == std::vector<std::string>({ "models/alyx.mdl" }) && unknown.remove.size() == 3 && unknown.write.size() == 2);
		// another campaign's file of the same size (alyx.mdl is 480,536 bytes in all three) goes
		folder["models/alyx.mdl"] = std::vector<std::uint8_t>(100, 7);
		CHECK(plan_campaign_model_swap(old, plan, view).keep.empty());
	}

	void test_campaign_model_restore()
	{
		ModelSetPlan plan;  // episode one's alyx
		plan.files.push_back({ "models/alyx.mdl", "hc_ep1", std::vector<std::uint8_t>(100, 21) });
		plan.files.push_back({ "models/alyx.vvd", "hc_ep1", std::vector<std::uint8_t>(50, 21) });
		plan.files.push_back({ "models/alyx_gestures.mdl", "hc_ep1", std::vector<std::uint8_t>(70, 21) });
		plan.files.push_back({ "models/alyx_gestures.ani", "hc_ep1", std::vector<std::uint8_t>(40, 21) });
		CampaignModelState old;  // half-life 2's
		old.set = ModelSet::kHalfLife2;
		old.files.push_back({ "models/alyx.mdl", 100, "hc_hl2", 100 });
		old.files.push_back({ "models/alyx.vvd", 50, "hc_hl2", 50 });
		old.files.push_back({ "models/alyx_gestures.mdl", 60, "hc_hl2", 60 });
		old.files.push_back({ "models/alyx_gestures.ani", 30, "hc_hl2", 30 });
		old.files.push_back({ "models/dog.mdl", 10, "hc_hl2", 10 });
		// what a swap that stopped halfway left: half-life 2's alyx_gestures.ani (held open), episode one's alyx.vvd
		// (written, then held), and a stray nobody knows
		std::map<std::string, std::vector<std::uint8_t>> folder{
			{ "models/alyx_gestures.ani", std::vector<std::uint8_t>(30, 2) },
			{ "models/alyx.vvd", std::vector<std::uint8_t>(50, 21) },
			{ "models/kleiner.ani", std::vector<std::uint8_t>(3, 3) },
		};
		const CampaignModelFolder view{
			[&folder](const std::string& name) {
				const auto found = folder.find(name);
				return found == folder.end() ? -1L : static_cast<long>(found->second.size());
			},
			[&folder](const std::string& name, const std::vector<std::uint8_t>& data) {
				const auto found = folder.find(name);
				return found != folder.end() && found->second == data;
			},
		};
		const std::vector<std::string> leftovers{ "models/alyx.vvd", "models/alyx_gestures.ani", "models/kleiner.ani" };
		const auto                     restore = plan_campaign_model_restore(leftovers, old, plan, view);
		const auto                     find = [&restore](const std::string& name) {
			return std::find_if(restore.begin(), restore.end(), [&name](const CampaignModelRestore& file) { return file.name == name; });
		};
		CHECK(restore.size() == 2);
		// alyx's leftover is episode one's: the plan's .mdl comes back
		CHECK(find("models/alyx.mdl") != restore.end() && find("models/alyx.mdl")->plan_file == std::optional<std::size_t>(0));
		// alyx_gestures' leftover is half-life 2's: its .mdl comes back from hc_hl2
		CHECK(find("models/alyx_gestures.mdl") != restore.end() && !find("models/alyx_gestures.mdl")->plan_file &&
			find("models/alyx_gestures.mdl")->source == "hc_hl2" && find("models/alyx_gestures.mdl")->size == 60);
		// a stray of a model neither knows stays as it is; dog, with nothing left, needs nothing
		CHECK(find("models/kleiner.mdl") == restore.end() && find("models/dog.mdl") == restore.end());
		CHECK(plan_campaign_model_restore({}, old, plan, view).empty());
	}

	void test_valid_link_id()
	{
		CHECK(valid_link_id("b"));
		CHECK(valid_link_id("AZaz09_-"));
		CHECK(valid_link_id(std::string(MAX_LINK_ID, 'x')));
		CHECK(!valid_link_id(""));
		CHECK(!valid_link_id(std::string(MAX_LINK_ID + 1, 'x')));
		// a mapping name's own characters, a space, a line break, utf-8's bytes and a nul aren't an id's
		for (const char* bad : { "b c", "a.b", "a/b", "a\\b", "a:b", "*", "b\n", "\xC3\xA9", "\xEF\xBC\xA2" }) {
			CHECK(!valid_link_id(bad));
		}
		CHECK(!valid_link_id(std::string_view("a\0b", 3)));
	}

	void test_ipv4_of()
	{
		using halfcraft::ipv4_of;
		CHECK(ipv4_of("192.168.10.254:27015") == std::optional<std::uint32_t>(0xC0A80AFEu));
		CHECK(ipv4_of("10.0.0.1") == std::optional<std::uint32_t>(0x0A000001u));
		CHECK(!ipv4_of("loopback") && !ipv4_of("") && !ipv4_of("1.2.3") && !ipv4_of("1.2.3.256:1") && !ipv4_of("1.2.3.4x"));
		CHECK(!ipv4_of("1234.1.1.1") && !ipv4_of("[::1]:27015"));
	}

	void test_sandbox()
	{
		using namespace halfcraft::sandbox;
		// d2_coast_01's jeep, its crane sequence and a scene, as the map's lump has them (outputs with ESC)
		const std::string lump =
			"{\n\"classname\" \"prop_vehicle_jeep\"\n\"targetname\" \"jeep\"\n\"VehicleLocked\" \"1\"\n"
			"\"PlayerOn\" \"jeep_escape_relay\x1bTrigger\x1b\x1b" "0\x1b-1\"\n}\n"
			"{\n\"classname\" \"logic_auto\"\n\"OnMapSpawn\" \"jeep\x1bTurnOff\x1b\x1b" "0\x1b" "1\"\n"
			"\"OnMapSpawn\" \"cranedriver,SetRelationship,!player D_NU 99,0,1\"\n}\n"
			"{\n\"classname\" \"logic_relay\"\n\"OnTrigger\" \"JEEP,lock,,0,1\"\n\"OnTrigger\" \"car_hold,Kill,,6,1\"\n}\n"
			"{\n\"classname\" \"logic_choreographed_scene\"\n\"targetname\" \"scene_1\"\n}\n"
			"{\n\"classname\" \"ai_goal_lead\"\n}\n"
			"{\n\"classname\" \"trigger_changelevel\"\n\"map\" \"d2_coast_03\"\n}\n"
			"{\n\"classname\" \"prop_vehicle_crane\"\n\"targetname\" \"crane\"\n\"VehicleLocked\" \"1\"\n}\n"
			"{\n\"classname\" \"npc_combine_s\"\n\"origin\" \"1 2 3\"\n}\n";
		const auto entities = parse(lump);
		CHECK(entities.size() == 8);
		CHECK(entities.size() == 8 && value_of(entities[0], "vehiclelocked") == "1");
		CHECK(output_target("jeep\x1bTurnOff\x1b\x1b" "0\x1b" "1") == std::make_pair(std::string_view("jeep"), std::string_view("TurnOff")));
		CHECK(output_target("a,b") == std::make_pair(std::string_view("a"), std::string_view("b")));
		Report     report;
		const auto kept = apply(entities, report);
		// the scene, the ai goal and the level change go; the npc, the relay, the auto and both vehicles stay
		CHECK(report.removed == 3 && kept.size() == 5);
		// the jeep's lock key goes, the crane's stays (it's the story's machine, not a ride)
		CHECK(report.unlocked == 1 && kept.size() == 5 && value_of(kept[0], "VehicleLocked").empty() && value_of(kept[3], "VehicleLocked") == "1");
		// the outputs that lock or turn off the jeep go (any case), the others stay
		CHECK(report.outputs_removed == 2);
		CHECK(kept.size() == 5 && kept[1].size() == 2 && kept[1][1].second.find("cranedriver") == 0);
		CHECK(kept.size() == 5 && kept[2].size() == 2 && kept[2][1].second == "car_hold,Kill,,6,1");
		CHECK(kept.size() == 5 && kept[0].size() == 3 && kept[0][2].first == "PlayerOn");
		// what serialize writes parses back to the same entities
		CHECK(parse(serialize(kept)) == kept);
		// the map's soldiers and antlions come back, not their bosses, friends or an npc_maker's templates
		CHECK(respawnable("npc_combine_s", 0) && respawnable("NPC_Antlion", 4) && respawnable("npc_rollermine", 0));
		CHECK(!respawnable("npc_combine_s", 2048 | 4) && !respawnable("npc_combinegunship", 0) && !respawnable("npc_citizen", 0));
		CHECK(!respawnable("npc_vortigaunt", 0) && !respawnable("npc_bullseye", 0) && !respawnable("npc_template_maker", 0));
		CHECK(spawnflags_of({ { "spawnflags", "2052" } }) == 2052 && spawnflags_of({}) == 0 && spawnflags_of({ { "spawnflags", "x" } }) == 0);
		// a dead one comes back once the delay is over and no player is near its spot
		CHECK(!respawn_due(100.0, 200.0, 180.0, -1.0, 1500.0) && respawn_due(100.0, 280.0, 180.0, -1.0, 1500.0));
		CHECK(!respawn_due(100.0, 280.0, 180.0, 400.0, 1500.0) && respawn_due(100.0, 280.0, 180.0, 1500.0, 1500.0));
		// a lump cut short ends the list there
		CHECK(parse("{\n\"a\" \"b\"\n}\n{\n\"c\" \"d").size() == 1);
	}

	void test_link_names()
	{
		// no id: the names halfcraft always had
		const auto plain = link_names("");
		CHECK(plain && plain->id.empty());
		CHECK(plain && plain->mapping == proto::kMappingName && plain->mapping == L"Local\\HalfCraft_v1");
		CHECK(plain && plain->frame_event == L"Local\\HalfCraft_v1_frame");
		CHECK(plain && plain->minecraft_mutex == L"Local\\HalfCraft_v1_minecraft");
		// an id: the default mapping's name, a dot and the id, then the event and the mutex after it
		const auto b = link_names("b");
		CHECK(b && b->id == "b" && b->mapping == L"Local\\HalfCraft_v1.b");
		CHECK(b && b->frame_event == L"Local\\HalfCraft_v1.b_frame" && b->minecraft_mutex == L"Local\\HalfCraft_v1.b_minecraft");
		// linux: the same names after the namespace, as posix shared memory names
		CHECK(plain && posix_name(plain->mapping) == "/HalfCraft_v1");
		CHECK(b && posix_name(b->mapping) == "/HalfCraft_v1.b" && posix_name(b->minecraft_mutex) == "/HalfCraft_v1.b_minecraft");
		CHECK(posix_name(L"HalfCraft_v1") == "/HalfCraft_v1");
		const std::string longest_id = "Pair_2-of-3_ABCDEFGHIJKLMNOPQRST";
		const auto        longest = link_names(longest_id);
		CHECK(longest_id.size() == MAX_LINK_ID && longest && longest->mapping == L"Local\\HalfCraft_v1.Pair_2-of-3_ABCDEFGHIJKLMNOPQRST");
		// not an id: no names at all, never the default link's
		CHECK(!link_names("b c"));
		CHECK(!link_names(std::string(MAX_LINK_ID + 1, 'a')));
		CHECK(!link_names("\xC3\xA9"));
	}

	void test_choose_link_id()
	{
		const auto from = [](const LinkChoice& choice) { return std::string(choice.from ? choice.from : "none"); };
		// -hc_link wins over HALFCRAFT_LINK, which wins over the default link
		CHECK(choose_link_id("a", "b").id == "a" && from(choose_link_id("a", "b")) == "-hc_link");
		CHECK(choose_link_id(nullptr, "b").id == "b" && from(choose_link_id(nullptr, "b")) == "HALFCRAFT_LINK");
		CHECK(choose_link_id(nullptr, nullptr).id.empty() && from(choose_link_id(nullptr, nullptr)) == "none");
		// an empty HALFCRAFT_LINK is unset; an empty -hc_link is still given
		CHECK(from(choose_link_id(nullptr, "")) == "none");
		CHECK(choose_link_id("", "b").id.empty() && from(choose_link_id("", "b")) == "-hc_link");

		// what a choice names: an id given that isn't one names nothing, not the default link
		const auto none = chosen_link_names(choose_link_id(nullptr, nullptr));
		CHECK(none && none->mapping == proto::kMappingName);
		const auto empty = chosen_link_names(choose_link_id(nullptr, ""));
		CHECK(empty && empty->id.empty());
		const auto b = chosen_link_names(choose_link_id(nullptr, "b"));
		CHECK(b && b->mapping == L"Local\\HalfCraft_v1.b");
		CHECK(!chosen_link_names(choose_link_id("", "b")));
		CHECK(!chosen_link_names(choose_link_id("b c", "b")));
		CHECK(!chosen_link_names(choose_link_id(nullptr, "b c")));
		// a good -hc_link over a bad HALFCRAFT_LINK (client.dll then sets the variable to it)
		const auto over_bad = chosen_link_names(choose_link_id("a", "b c"));
		CHECK(over_bad && over_bad->id == "a");
	}

	void test_is_map_name_takes_map_names_only()
	{
		CHECK(is_map_name("d1_trainstation_02"));
		CHECK(!is_map_name(""));
		CHECK(!is_map_name("d1 trainstation"));
		CHECK(!is_map_name("x;quit"));
		CHECK(is_map_name(std::string(64, 'a')));
		CHECK(!is_map_name(std::string(65, 'a')));
	}

	void test_host_commands_set_up_a_two_slot_lan_server()
	{
		// the wait lets the game there is shut down before maxplayers, which the engine refuses while a server runs
		CHECK(host_commands("d1_trainstation_02", false) == "disconnect; wait; maxplayers 2; sv_lan 1; coop 0; map d1_trainstation_02");
		CHECK(host_commands("d1_trainstation_02", true) == "disconnect; wait; maxplayers 2; sv_lan 0; coop 0; map d1_trainstation_02");
		CHECK(host_commands("x;quit", false).empty());
	}

	void test_parse_host_args_takes_a_map_and_vpn()
	{
		using Args = std::vector<std::string_view>;
		const auto is = [](const std::optional<HostRequest>& request, std::string_view map, bool vpn) {
			return request && request->map == map && request->vpn == vpn;
		};
		constexpr std::string_view here = "d1_trainstation_02";
		// a map, with vpn or without, on the menu or in a game
		CHECK(is(parse_host_args(Args{ "d1_canals_01" }, here), "d1_canals_01", false));
		CHECK(is(parse_host_args(Args{ "d1_canals_01" }, ""), "d1_canals_01", false));
		CHECK(is(parse_host_args(Args{ "d1_canals_01", "vpn" }, ""), "d1_canals_01", true));
		CHECK(is(parse_host_args(Args{ "d1_canals_01", "VPN" }, ""), "d1_canals_01", true));
		// no map: the current one; a lone vpn is the flag
		CHECK(is(parse_host_args(Args{}, here), here, false));
		CHECK(is(parse_host_args(Args{ "vpn" }, here), here, true));
		CHECK(is(parse_host_args(Args{ "Vpn" }, here), here, true));
		// no map at the menu: the usage line
		CHECK(!parse_host_args(Args{}, ""));
		CHECK(!parse_host_args(Args{ "vpn" }, ""));
		// a second argument that isn't vpn, one too many, vpn first: the usage line, never a guess at sv_lan
		CHECK(!parse_host_args(Args{ "d1_canals_01", "vpm" }, here));
		CHECK(!parse_host_args(Args{ "d1_canals_01", "vpn", "x" }, here));
		CHECK(!parse_host_args(Args{ "vpn", "d1_canals_01" }, here));
		CHECK(!parse_host_args(Args{ "vpn", "vpn" }, here));
		// a name that isn't a map name, typed or the current level's; an empty one isn't "no map"
		CHECK(!parse_host_args(Args{ "bad-name" }, here));
		CHECK(!parse_host_args(Args{ "x;quit" }, here));
		CHECK(!parse_host_args(Args{ "" }, here));
		CHECK(!parse_host_args(Args{}, "custom-map"));
		CHECK(!parse_host_args(Args{ "vpn" }, "custom-map"));
	}

	void test_map_file_and_host_slots()
	{
		CHECK(map_file("d1_trainstation_02") == "maps/d1_trainstation_02.bsp");
		CHECK(HOST_SLOTS == 2);
		CHECK(!hosted_level_has_room(1));
		CHECK(hosted_level_has_room(2));
		CHECK(hosted_level_has_room(32));
	}
}

int run_tests()
{
	test_convex_face_gives_a_cube_its_squares();
	test_face_flags_take_the_planes_material();
	test_triangulate_gives_each_face_its_flags();
	test_dig_material_maps_surface_materials();
	test_brush_material();
	test_merge_cells_makes_boxes();
	test_subtract_boxes_cuts_the_cells_voxels();
	test_subtract_boxes_flags_the_cut_faces();
	test_subtract_boxes_flags_the_new_planes_of_an_unflagged_brush();
	test_subtract_boxes_leaves_what_it_doesnt_cut();
	test_subtract_boxes_keeps_each_piece_within_its_bounds();
	test_subtract_boxes_drops_slivers();
	test_bsp_tree_gives_a_points_contents();
	test_read_bsp_tree();
	test_void_region_finds_the_rock_behind_the_walls();
	test_void_region_tests_a_convex_by_its_planes();
	test_dig_mask_layout_places_the_cells_in_the_texture();
	test_dig_mask_shader_texel();
	test_dig_mask_write_section();
	test_dig_mask_texel_dug();
	test_dig_mask_has_section();
	test_dig_mask_needs_rebuild();
	test_dig_mask_upload_before_clip();
	test_dug_cells();
	test_dug_pending();
	test_triangle_clip();
	test_read_studio_header();
	test_companion_checksum();
	test_model_set_of();
	test_plan_half_life_2();
	test_plan_episode_one_and_lost_coast();
	test_plan_leaves_out_broken_closures();
	test_campaign_model_names();
	test_campaign_model_state();
	test_stale_campaign_models();
	test_campaign_model_swap();
	test_campaign_model_restore();
	test_valid_link_id();
	test_link_names();
	test_sandbox();
	test_ipv4_of();
	test_choose_link_id();
	test_is_map_name_takes_map_names_only();
	test_host_commands_set_up_a_two_slot_lan_server();
	test_parse_host_args_takes_a_map_and_vpn();
	test_map_file_and_host_slots();
	if (failed == 0) {
		std::printf("core_tests: %d checks passed\n", checks);
		return 0;
	}
	std::printf("core_tests: %d of %d checks failed\n", failed, checks);
	return 1;
}

int main()
{
	// a check that throws (out of memory) says so instead of crashing
	try {
		return run_tests();
	} catch (const std::exception& error) {
		std::fprintf(stderr, "core_tests: %s\n", error.what());
		return 2;
	}
}
