#pragma once

// what the cells of a wall dig as: half-life's surface materials mapped onto the few minecraft blocks a wall breaks
// into (the spec's materials table, docs/superpowers/specs/2026-10-17-dig-through-walls-design.md), and the flags a
// world brush's face carries to say so (core/hc_collision_shapes.h: PRIM_DIGGABLE and the material in bits 8-15).
// pure code, no engine and no link.

#include <array>
#include <cstdint>
#include <vector>

#include "hc_collision_shapes.h"

namespace halfcraft
{
	/// the material a face digs as. the values are proto::DigMaterial's (this header can't take the protocol's: the code
	/// that includes both pins them with static_asserts)
	enum DigMaterialId : std::uint8_t
	{
		DIG_NONE,
		DIG_STONE,
		DIG_STONE_BRICKS,
		DIG_IRON,
		DIG_PLANKS,
		DIG_DIRT,
		DIG_GLASS,
		DIG_SAND,
	};

	/// what a half-life surface digs as.
	/// @param game_material - the char of the surface's material (surfacedata_t::game.material, the CHAR_TEX_* of
	///        game/shared/decals.h: 'C' concrete, 'T' tile, 'M' metal, 'G' grate, 'V' vent, 'P' computer, 'W' wood,
	///        'D' dirt, 'Y' glass; 'U' is cardboard, which decals.h has no name for)
	/// @return the block's material: concrete is stone, tile stone bricks, metal, grate, vent and computer iron, wood
	///         and cardboard planks, dirt and foliage dirt, sand sand, glass glass, and everything else (flesh, sand, slosh, no material at
	///         all) stone: a wall's far side is rock
	inline std::uint8_t dig_material(char game_material)
	{
		switch (game_material) {
		case 'T': return DIG_STONE_BRICKS;
		case 'M':
		case 'G':
		case 'V':
		case 'P': return DIG_IRON;
		case 'W':
		case 'U': return DIG_PLANKS;
		case 'D': return DIG_DIRT;
		case 'Y': return DIG_GLASS;
		case 'N': return DIG_SAND;
		case 'O': return DIG_DIRT;
		default: return DIG_STONE;
		}
	}

	/// what a whole brush digs as (its cut faces, and its sides without a material of their own): its sides' votes.
	/// @param sides - each side's material, DIG_NONE for a side that doesn't vote
	/// @return the material most sides vote for, on a tie the one voted for first; stone when none votes
	inline std::uint8_t brush_material(const std::vector<std::uint8_t>& sides)
	{
		std::array<int, 256> votes{};
		for (const std::uint8_t side : sides) {
			++votes[side];
		}
		std::uint8_t material = DIG_STONE;
		int          most = 0;
		for (const std::uint8_t side : sides) {
			if (side != DIG_NONE && votes[side] > most) {
				material = side;
				most = votes[side];
			}
		}
		return material;
	}

	/// the flags of a diggable face of that material: PRIM_DIGGABLE and the material in bits 8-15
	inline std::uint32_t dig_flags(std::uint8_t material)
	{
		return PRIM_DIGGABLE | (static_cast<std::uint32_t>(material) << PRIM_MATERIAL_SHIFT);
	}
}
