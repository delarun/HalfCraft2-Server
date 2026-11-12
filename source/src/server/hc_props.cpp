// server.dll: the half-life props minecraft crafts (see hc_props.h).

#include "cbase.h"
#include "player.h"
#include "vphysics_interface.h"

#include "tier0/valve_minmax_off.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>

#include "halfcraft_protocol.h"
#include "core/hc_log.h"
#include "core/hc_props.h"
#include "server/hc_props.h"
#include "server/hc_vitals.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		constexpr float PLACE_REACH = 96.0f;      // units from the eye a prop goes at most
		constexpr float PROBE_HALF = 8.0f;        // the first look's box: where the prop goes at all
		constexpr float NUDGE_SPEED = 60.0f;      // a hit's push on the prop it doesn't break (units/s, times its knockback)

		// minecraft's damage each prop took so far, by its handle
		std::unordered_map<unsigned long, float> g_taken;

		class PropsSystem final : public CAutoGameSystem
		{
		public:
			PropsSystem() : CAutoGameSystem("HalfCraftProps") {}

			// the models go into the precache with the map: a prop put later finds its model there
			void LevelInitPreEntity() override
			{
				for (const props::Kind& kind : props::KINDS) {
					CBaseEntity::PrecacheModel(std::string(kind.model).c_str());
				}
				g_taken.clear();
			}

			void LevelShutdownPostEntity() override { g_taken.clear(); }
		};

		PropsSystem g_props_system;

		const props::Kind* kind_of(CBaseEntity* entity)
		{
			if (!entity || Q_strncmp(entity->GetClassname(), "prop_physics", 12) || entity->GetModelName() == NULL_STRING) {
				return nullptr;
			}
			return props::by_model(STRING(entity->GetModelName()));
		}
	}

	bool is_minecraft_prop(CBaseEntity* entity)
	{
		return kind_of(entity) != nullptr;
	}

	void place_prop(CBasePlayer* player, std::uint32_t kind)
	{
		const props::Kind* entry = props::by_kind(kind);
		if (!player || !player->IsAlive() || !entry) {
			return;
		}
		Vector forward;
		AngleVectors(player->EyeAngles(), &forward);
		const Vector eye = player->EyePosition();
		trace_t      probe;
		UTIL_TraceHull(eye, eye + forward * PLACE_REACH, Vector(-PROBE_HALF, -PROBE_HALF, -PROBE_HALF), Vector(PROBE_HALF, PROBE_HALF, PROBE_HALF), MASK_SOLID,
			player, COLLISION_GROUP_NONE, &probe);
		if (probe.startsolid) {
			log_info("props: no room for %s in front of player %d", std::string(entry->model).c_str(), player->GetUserID());
			return;
		}
		CBaseEntity* prop = CreateEntityByName("prop_physics");
		if (!prop) {
			return;
		}
		const QAngle angles(0.0f, player->EyeAngles().y, 0.0f);
		prop->KeyValue("model", std::string(entry->model).c_str());
		prop->SetAbsOrigin(probe.endpos);
		prop->SetAbsAngles(angles);
		if (DispatchSpawn(prop) < 0) {
			UTIL_Remove(prop);
			return;
		}
		prop->Activate();
		// the prop's own size, out from the eye again: where all of it fits (its box as it stands, its origin wherever
		// the model has it)
		const Vector mins = prop->CollisionProp()->OBBMins(), maxs = prop->CollisionProp()->OBBMaxs();
		const float  half = std::max({ -mins.x, -mins.y, maxs.x, maxs.y }) * 0.75f;
		trace_t      fit;
		UTIL_TraceHull(eye, eye + forward * PLACE_REACH, Vector(-half, -half, mins.z), Vector(half, half, maxs.z), MASK_SOLID, player, COLLISION_GROUP_NONE, &fit);
		const Vector at = fit.startsolid ? probe.endpos : fit.endpos;
		prop->Teleport(&at, &angles, &vec3_origin);
		log_info("props: player %d put %s at (%.0f %.0f %.0f)", player->GetUserID(), std::string(entry->model).c_str(), at.x, at.y, at.z);
	}

	void harvest_hit(CBasePlayer* player, CBaseEntity* prop, float mc_damage, MapSlot slot)
	{
		const props::Kind* entry = kind_of(prop);
		if (!player || !entry || !(mc_damage > 0.0f)) {
			return;
		}
		float& taken = g_taken[prop->GetRefEHandle().ToInt()];
		taken += mc_damage;
		if (!props::harvested(*entry, taken)) {
			// a hit that doesn't break it pushes it a little, away from the player
			if (IPhysicsObject* physics = prop->VPhysicsGetObject()) {
				Vector away = prop->WorldSpaceCenter() - player->EyePosition();
				VectorNormalize(away);
				const Vector velocity = away * NUDGE_SPEED;
				physics->AddVelocity(&velocity, nullptr);
			}
			return;
		}
		g_taken.erase(prop->GetRefEHandle().ToInt());
		const Vector centre = prop->WorldSpaceCenter();
		const McVec  mc = source_to_mc(centre.Base(), slot);
		const float  at[3] = { static_cast<float>(mc.x), static_cast<float>(mc.y), static_cast<float>(mc.z) };
		int          bits[3];
		std::memcpy(bits, at, sizeof(bits));
		send_player_input(player, proto::kInPropDropped, static_cast<int>(entry->kind), bits[0], bits[1], bits[2]);
		log_info("props: player %d broke %s down into its item (%.1f of minecraft's damage)", player->GetUserID(), std::string(entry->model).c_str(), taken);
		UTIL_Remove(prop);
	}
}
