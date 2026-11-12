#pragma once

// digging half-life's ground: its displacements (the terrain's sand, grass and dirt) are a surface, not a solid like a
// brush, so minecraft gets two things more for them. the rock under them (ground_columns: what lies at most
// GROUND_DEPTH blocks under a surface facing up, which the void message hands minecraft as rock, solid until dug, its
// holes walled), and their triangles with the dug cells taken out (subtract_boxes_from_tri), so a hole in the ground
// opens. pure code, no engine and no link, tested by tools/debug/core_tests.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include "hc_collision_shapes.h"
#include "hc_dig_clip.h"

namespace halfcraft
{
	/// how far under the ground (blocks) minecraft finds rock: deeper holes come out under half-life's terrain
	inline constexpr float GROUND_DEPTH = 24.0f;
	/// and how far under the surface the rock starts (blocks): a sub-voxel of rock reaches 1/16 over its centre, and a
	/// player walking on the ground's triangles with their feet in rock fell through it (minecraft lets a box out of a
	/// shape it's in). more than 1/16, for slopes up to ~58 degrees: the rock's top stays under the feet
	inline constexpr float GROUND_MARGIN = 0.1f;

	namespace ground_detail
	{
		using Poly = std::vector<std::array<float, 3>>;

		/// the part of a convex polygon on one side of the axis plane coordinate `axis` == value: keep_below keeps
		/// what lies at or under it, else at or over it.
		inline void clip(const Poly& in, int axis, float value, bool keep_below, Poly& out)
		{
			out.clear();
			const std::size_t n = in.size();
			for (std::size_t i = 0; i < n; ++i) {
				const auto& a = in[i];
				const auto& b = in[(i + 1) % n];
				const float da = keep_below ? value - a[axis] : a[axis] - value;
				const float db = keep_below ? value - b[axis] : b[axis] - value;
				if (da >= 0.0f) {
					out.push_back(a);
				}
				if ((da >= 0.0f) != (db >= 0.0f)) {
					const float t = da / (da - db);
					out.push_back({ a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t });
				}
			}
		}

		inline float area2(const std::array<float, 3>& a, const std::array<float, 3>& b, const std::array<float, 3>& c)
		{
			const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
			const float x = u[1] * v[2] - u[2] * v[1], y = u[2] * v[0] - u[0] * v[2], z = u[0] * v[1] - u[1] * v[0];
			return std::sqrt(x * x + y * y + z * z);
		}

		inline bool overlaps(const Poly& poly, const DigBox& box)
		{
			for (int k = 0; k < 3; ++k) {
				float lo = poly[0][k], hi = poly[0][k];
				for (const auto& p : poly) {
					lo = std::min(lo, p[k]);
					hi = std::max(hi, p[k]);
				}
				if (hi <= box.lo[k] || lo >= box.hi[k]) {
					return false;
				}
			}
			return true;
		}
	}

	/// a triangle minus dug boxes: the triangles of what lies outside every box, appended to out (in the triangle's
	/// winding). a triangle that only touches a box's face keeps all of it, and slivers under DIG_SLIVER² go.
	inline void subtract_boxes_from_tri(const float v[9], const std::vector<DigBox>& boxes, std::vector<std::array<float, 9>>& out)
	{
		using ground_detail::Poly;
		std::vector<Poly> pieces{ Poly{ { v[0], v[1], v[2] }, { v[3], v[4], v[5] }, { v[6], v[7], v[8] } } };
		std::vector<Poly> next;
		Poly              outside, rest, keep;
		for (const DigBox& box : boxes) {
			next.clear();
			for (const Poly& piece : pieces) {
				if (!ground_detail::overlaps(piece, box)) {
					next.push_back(piece);
					continue;
				}
				// peel off what lies beyond each of the box's 6 faces; what's left is inside it, gone
				rest = piece;
				for (int axis = 0; axis < 3 && rest.size() >= 3; ++axis) {
					ground_detail::clip(rest, axis, box.lo[axis], true, outside);
					if (outside.size() >= 3) {
						next.push_back(outside);
					}
					ground_detail::clip(rest, axis, box.lo[axis], false, keep);
					rest.swap(keep);
					if (rest.size() < 3) {
						break;
					}
					ground_detail::clip(rest, axis, box.hi[axis], false, outside);
					if (outside.size() >= 3) {
						next.push_back(outside);
					}
					ground_detail::clip(rest, axis, box.hi[axis], true, keep);
					rest.swap(keep);
				}
			}
			pieces.swap(next);
		}
		for (const Poly& piece : pieces) {
			for (std::size_t i = 1; i + 1 < piece.size(); ++i) {
				if (ground_detail::area2(piece[0], piece[i], piece[i + 1]) < DIG_SLIVER * DIG_SLIVER) {
					continue;
				}
				out.push_back({ piece[0][0], piece[0][1], piece[0][2], piece[i][0], piece[i][1], piece[i][2], piece[i + 1][0], piece[i + 1][1], piece[i + 1][2] });
			}
		}
	}

	/// the ground over a region's columns, at its sub-voxels' resolution (8 to a block): where the line up each column
	/// crosses a ground triangle, and which way that one faces.
	class GroundColumns
	{
	public:
		/// @param origin - the region's minimum corner (minecraft blocks)
		/// @param tris - the ground's triangles near it (up to GROUND_DEPTH over it), solid side behind their normal
		GroundColumns(const double origin[3], const std::vector<ColPrimitives::Tri>& tris)
		{
			origin_[0] = origin[0];
			origin_[1] = origin[1];
			origin_[2] = origin[2];
			columns_.resize(static_cast<std::size_t>(REGION_VOXELS * REGION_VOXELS));
			const double top = origin[1] + COLLISION_REGION_SIZE + GROUND_DEPTH;
			for (const ColPrimitives::Tri& tri : tris) {
				const float* a = tri.v;
				const float* b = tri.v + 3;
				const float* c = tri.v + 6;
				if (std::max({ a[1], b[1], c[1] }) < origin[1] || std::min({ a[1], b[1], c[1] }) > top) {
					continue;
				}
				// its normal's y: positive faces up (the ground's top), and the line meets it where it isn't edge-on
				const float ux = b[0] - a[0], uz = b[2] - a[2], vx = c[0] - a[0], vz = c[2] - a[2];
				const float ny = uz * vx - ux * vz;
				if (std::fabs(ny) < 1e-9f) {
					continue;
				}
				const int x0 = std::max(0, static_cast<int>(std::floor((std::min({ a[0], b[0], c[0] }) - origin[0]) * 8.0)));
				const int x1 = std::min(REGION_VOXELS - 1, static_cast<int>(std::floor((std::max({ a[0], b[0], c[0] }) - origin[0]) * 8.0)));
				const int z0 = std::max(0, static_cast<int>(std::floor((std::min({ a[2], b[2], c[2] }) - origin[2]) * 8.0)));
				const int z1 = std::min(REGION_VOXELS - 1, static_cast<int>(std::floor((std::max({ a[2], b[2], c[2] }) - origin[2]) * 8.0)));
				for (int z = z0; z <= z1; ++z) {
					for (int x = x0; x <= x1; ++x) {
						const float px = static_cast<float>(origin[0] + (x + 0.5) / 8.0), pz = static_cast<float>(origin[2] + (z + 0.5) / 8.0);
						// barycentric in x/z
						const float w1 = ((px - a[0]) * vz - (pz - a[2]) * vx) / (ux * vz - uz * vx);
						const float w2 = ((pz - a[2]) * ux - (px - a[0]) * uz) / (ux * vz - uz * vx);
						if (w1 < 0.0f || w2 < 0.0f || w1 + w2 > 1.0f) {
							continue;
						}
						const float y = a[1] + w1 * (b[1] - a[1]) + w2 * (c[1] - a[1]);
						columns_[static_cast<std::size_t>(z * REGION_VOXELS + x)].push_back({ y, ny > 0.0f });
					}
				}
			}
			for (auto& column : columns_) {
				std::sort(column.begin(), column.end(), [](const Cross& l, const Cross& r) { return l.y < r.y; });
			}
		}

		/// whether the point (minecraft blocks, inside the region) lies under the ground: the first triangle the line up
		/// from it meets faces up, at least GROUND_MARGIN and at most GROUND_DEPTH over it.
		bool under(double x, double y, double z) const
		{
			const int cx = std::clamp(static_cast<int>(std::floor((x - origin_[0]) * 8.0)), 0, REGION_VOXELS - 1);
			const int cz = std::clamp(static_cast<int>(std::floor((z - origin_[2]) * 8.0)), 0, REGION_VOXELS - 1);
			for (const Cross& cross : columns_[static_cast<std::size_t>(cz * REGION_VOXELS + cx)]) {
				if (cross.y > y) {
					return cross.up && cross.y - y >= GROUND_MARGIN && cross.y - y <= GROUND_DEPTH;
				}
			}
			return false;
		}

		bool empty() const
		{
			for (const auto& column : columns_) {
				if (!column.empty()) {
					return false;
				}
			}
			return true;
		}

	private:
		struct Cross
		{
			float y;
			bool  up;
		};
		double                          origin_[3];
		std::vector<std::vector<Cross>> columns_;
	};
}
