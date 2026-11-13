#pragma once

// server.dll: half-life's objects as minecraft items (docs/server/PLAN.md). minecraft's hits on any object that isn't
// the map's own static geometry (a physics prop, a door, a dynamic prop: harvestable) add up instead of hurting it,
// and once they reach its toll (its mass's, a crafted kind's own: core/hc_props.h) it goes, its item to the player who
// struck (proto::kStrPropDropped). a prop's item puts the object back where its player looks (proto::kEvPlaceProp): a
// crafted kind's model, or the model its item carries (proto::kEvPropModel). half-life's own weapons break things as ever.

#include <cstdint>
#include <string>

#include "core/hc_units.h"

class CBaseEntity;
class CBasePlayer;

namespace halfcraft
{
	/// whether minecraft's hits break an entity down into an item: a physics prop, a door or a dynamic prop with a model,
	/// on its own (not parented: part of something bigger).
	bool is_minecraft_prop(CBaseEntity* entity);

	/// a player used a prop's item (proto::kEvPlaceProp): the object, in front of them where it fits.
	/// @param kind - proto::PropKind; 0: `wanted`
	/// @param prop_class - proto::PropClass, for a model
	void place_prop(CBasePlayer* player, std::uint32_t kind, std::uint32_t prop_class, const std::string& wanted);

	/// a minecraft hit by the player on such an object: it adds up, and one it breaks down goes, its item to them.
	/// @param mc_damage - minecraft's damage (after its own modifiers)
	void harvest_hit(CBasePlayer* player, CBaseEntity* prop, float mc_damage, MapSlot slot);

	/// a piece of a model's path a player's minecraft sends before its kEvPlaceProp (proto::kEvPropModel).
	void prop_model_piece(int user_id, std::uint32_t index, std::uint32_t length, const std::uint32_t words[6]);
	/// the path the pieces made, "" when they didn't make a whole one; it's taken (the next place needs its own).
	std::string take_prop_model(int user_id);
}
