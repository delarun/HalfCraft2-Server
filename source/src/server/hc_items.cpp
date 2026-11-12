// server.dll: minecraft's dropped items for half-life's gravity gun (see hc_items.h).

#include "cbase.h"
#include "player_pickup.h"
#include "takedamageinfo.h"

#include "tier0/valve_minmax_off.h"
#include <algorithm>
#include <cstdint>
#include <unordered_map>

#include "core/hc_log.h"
#include "server/hc_held_blocks.h"
#include "server/hc_items.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace
{
	constexpr float HALF = 6.0f;            // the stand-in's box: an item lies about a quarter block wide
	constexpr float PUNT_SPEED = 1000.0f;   // units a second an item the gravity gun punts flies at
	constexpr float WORLD_MARGIN = 64.0f;   // stand-ins this far inside source's world bounds (another map's slot is out)
}

//-----------------------------------------------------------------------------
// one of minecraft's items, where it lies: invisible, never networked, never saved. debris: players and props pass
// through it, the gravity gun's trace doesn't.
//-----------------------------------------------------------------------------
class CHalfCraftItem : public CBaseEntity, public CDefaultPlayerPickupVPhysics
{
public:
	DECLARE_CLASS(CHalfCraftItem, CBaseEntity);

	void Spawn(void) override
	{
		BaseClass::Spawn();
		SetSolid(SOLID_BBOX);
		SetMoveType(MOVETYPE_NONE);
		SetCollisionGroup(COLLISION_GROUP_DEBRIS);
		SetCollisionBounds(Vector(-HALF, -HALF, 0.0f), Vector(HALF, HALF, 2.0f * HALF));
		AddEffects(EF_NODRAW);
		m_takedamage = DAMAGE_EVENTS_ONLY;  // the gravity gun's punt reaches TraceAttack
	}

	int ObjectCaps(void) override { return BaseClass::ObjectCaps() | FCAP_DONT_SAVE; }

	/// the gravity gun can't lift the stand-in: it takes the item into a cube instead.
	CBaseEntity* OnFailedPhysGunPickup(Vector) override { return Take(vec3_origin); }

	void TraceAttack(const CTakeDamageInfo& info, const Vector& dir, trace_t* trace, CDmgAccumulator* accumulator) override
	{
		if (info.GetDamageType() & DMG_PHYSGUN) {
			Take(dir * PUNT_SPEED);
		}
		BaseClass::TraceAttack(info, dir, trace, accumulator);
	}

	CBaseEntity* Take(const Vector& velocity)
	{
		if (m_bTaken) {
			return nullptr;
		}
		CBaseEntity* cube = halfcraft::take_item(m_mcId, WorldSpaceCenter(), velocity);
		if (cube) {
			// gone from here at once: minecraft takes the item out, and the table drops it next
			m_bTaken = true;
			AddSolidFlags(FSOLID_NOT_SOLID);
		}
		return cube;
	}

	std::uint32_t m_mcId = 0;
	bool          m_bTaken = false;
};

LINK_ENTITY_TO_CLASS(halfcraft_item, CHalfCraftItem);

namespace halfcraft
{
	namespace
	{
		struct StandIn
		{
			EHANDLE       entity;
			std::uint32_t frame = 0;
		};

		std::unordered_map<std::uint32_t, StandIn> g_stand_ins;  // by minecraft's item id
		std::uint32_t                              g_frame = 0;

		bool inside_world(const float at[3])
		{
			for (int k = 0; k < 3; ++k) {
				if (at[k] < MIN_COORD_FLOAT + WORLD_MARGIN || at[k] > MAX_COORD_FLOAT - WORLD_MARGIN) {
					return false;
				}
			}
			return true;
		}

		void remove_all()
		{
			for (auto& [id, stand_in] : g_stand_ins) {
				if (CBaseEntity* entity = stand_in.entity.Get()) {
					UTIL_Remove(entity);
				}
			}
			g_stand_ins.clear();
		}
	}

	void Items::update(Link& link, MapSlot slot)
	{
		if (!link.mc_alive()) {
			remove_all();
			return;
		}
		if (!link.read_items(table_)) {
			return;  // minecraft was writing: last frame's stand-ins stay a frame longer
		}
		++g_frame;
		const std::uint32_t count = std::min(table_.count, proto::kMaxItems);
		for (std::uint32_t i = 0; i < count; ++i) {
			const proto::ItemRecord& record = table_.items[i];
			float                    feet[3];
			mc_to_source(record.x, record.y, record.z, slot, feet);
			if (!inside_world(feet)) {
				continue;
			}
			StandIn& stand_in = g_stand_ins[record.id];
			auto*    item = static_cast<CHalfCraftItem*>(stand_in.entity.Get());
			if (!item) {
				item = static_cast<CHalfCraftItem*>(CreateEntityByName("halfcraft_item"));
				if (!item) {
					g_stand_ins.erase(record.id);
					continue;
				}
				item->m_mcId = record.id;
				DispatchSpawn(item);
				stand_in.entity = item;
			}
			UTIL_SetOrigin(item, Vector(feet[0], feet[1], feet[2]));
			stand_in.frame = g_frame;
		}
		for (auto it = g_stand_ins.begin(); it != g_stand_ins.end();) {
			if (it->second.frame != g_frame) {
				if (CBaseEntity* entity = it->second.entity.Get()) {
					UTIL_Remove(entity);
				}
				it = g_stand_ins.erase(it);
			} else {
				++it;
			}
		}
		const std::size_t now = g_stand_ins.size();
		if (now != logged_ && (now % 10 == 0 || now < 5)) {
			log_info("%zu of minecraft's items lie where the gravity gun can take them", now);
		}
		logged_ = now;
	}

	void Items::reset()
	{
		g_stand_ins.clear();  // the level's entities go with it: only forget them
		logged_ = 0;
	}
}
