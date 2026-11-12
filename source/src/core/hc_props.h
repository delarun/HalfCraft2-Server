#pragma once

// the half-life props minecraft crafts (docs/server/PLAN.md): each proto::PropKind's model, and how much of minecraft's
// hitting (its damage, after its own modifiers) breaks one down into its item again. a prop of the kind's model, the
// map's own too, comes back as the item; any other is half-life's to break. pure: no sdk, no windows, tested by
// tools/debug/core_tests.cpp.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace halfcraft::props
{
	struct Kind
	{
		std::uint32_t    kind;     // proto::PropKind
		std::string_view model;    // "models/..."
		float            harvest;  // minecraft damage that breaks it down: a fist's 1 a hit, an iron sword's 6
	};

	inline constexpr std::array<Kind, 9> KINDS{ {
		{ 1, "models/props_junk/wood_crate001a.mdl", 6.0f },
		{ 2, "models/props_c17/oildrum001.mdl", 10.0f },
		{ 3, "models/props_c17/oildrum001_explosive.mdl", 10.0f },
		{ 4, "models/props_junk/cinderblock01a.mdl", 8.0f },
		{ 5, "models/props_c17/furniturechair001a.mdl", 5.0f },
		{ 6, "models/props_c17/furnituretable001a.mdl", 8.0f },
		{ 7, "models/props_vehicles/carparts_tire01a.mdl", 6.0f },
		{ 8, "models/props_interiors/radiator01a.mdl", 10.0f },
		{ 9, "models/props_c17/concrete_barrier001a.mdl", 20.0f },
	} };

	/// a kind by its proto::PropKind, nullptr for none.
	inline const Kind* by_kind(std::uint32_t kind)
	{
		for (const Kind& entry : KINDS) {
			if (entry.kind == kind) {
				return &entry;
			}
		}
		return nullptr;
	}

	/// a kind by its model as an entity names it (any case, '\\' or '/'), nullptr for a model no kind has.
	inline const Kind* by_model(std::string_view model)
	{
		auto same = [](char a, char b) {
			auto fold = [](char c) { return c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
			return fold(a) == fold(b);
		};
		for (const Kind& entry : KINDS) {
			if (model.size() == entry.model.size() && std::equal(model.begin(), model.end(), entry.model.begin(), same)) {
				return &entry;
			}
		}
		return nullptr;
	}

	/// whether hits that add up to taken (minecraft damage) break a kind down.
	inline bool harvested(const Kind& kind, float taken)
	{
		return taken >= kind.harvest;
	}
}
