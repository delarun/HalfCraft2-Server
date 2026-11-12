#pragma once

// server.dll: the half-life props minecraft crafts (core/hc_props.h). a player's prop item puts its prop where they
// look (proto::kEvPlaceProp), and minecraft's hits on a prop of those models, the map's own too, add up until it breaks
// down into its item again (proto::kInPropDropped) instead of hurting it. half-life's own weapons break it as ever.

#include <cstdint>

#include "core/hc_units.h"

class CBaseEntity;
class CBasePlayer;

namespace halfcraft
{
	/// whether an entity is a physics prop of a model minecraft crafts.
	bool is_minecraft_prop(CBaseEntity* entity);

	/// a player used a prop's item (proto::kEvPlaceProp): the prop, in front of them where it fits.
	/// @param kind - proto::PropKind
	void place_prop(CBasePlayer* player, std::uint32_t kind);

	/// a minecraft hit by the player on such a prop: it adds up, and a prop it breaks down goes, its item to the player's
	/// minecraft where it was.
	/// @param mc_damage - minecraft's damage (after its own modifiers)
	void harvest_hit(CBasePlayer* player, CBaseEntity* prop, float mc_damage, MapSlot slot);
}
