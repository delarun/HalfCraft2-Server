// server.dll: half-life's objects as minecraft items (see hc_props.h).

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
#include "shared/hc_bridge.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		constexpr float       PLACE_REACH = 96.0f;  // units from the eye a prop goes at most
		constexpr float       PROBE_HALF = 8.0f;    // the first look's box: where the prop goes at all
		constexpr float       NUDGE_SPEED = 60.0f;  // a hit's push on the prop it doesn't break (units/s)
		constexpr float       DOOR_TOLL = 12.0f;    // minecraft damage that takes a door off its hinges
		constexpr float       DYNAMIC_TOLL = 15.0f;
		constexpr float       KG_PER_DAMAGE = 8.0f;  // a physics prop's toll: its mass over this, 3 to 30
		constexpr std::size_t MAX_MODEL = 200;

		// minecraft's damage each object took so far, by its handle
		std::unordered_map<unsigned long, float> g_taken;
		// the model each player's minecraft is sending (kEvPropModel), by userid: what came, and how long it is
		struct Pieces
		{
			std::string   path;
			std::uint32_t length = 0;
		};
		std::unordered_map<int, Pieces> g_pieces;

		class PropsSystem final : public CAutoGameSystem
		{
		public:
			PropsSystem() : CAutoGameSystem("HalfCraftProps") {}

			// the crafted kinds' models go into the precache with the map: a prop put later finds its model there
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

		std::uint32_t class_of(CBaseEntity* entity)
		{
			const char* name = entity->GetClassname();
			if (!Q_strncmp(name, "prop_door", 9)) {
				return proto::kPropClassDoor;
			}
			if (!Q_strncmp(name, "prop_dynamic", 12)) {
				return proto::kPropClassDynamic;
			}
			return proto::kPropClassPhysics;
		}

		/// what breaking it down takes (minecraft damage).
		float toll_of(CBaseEntity* entity, const props::Kind* kind)
		{
			if (kind) {
				return kind->harvest;
			}
			switch (class_of(entity)) {
			case proto::kPropClassDoor:
				return DOOR_TOLL;
			case proto::kPropClassDynamic:
				return DYNAMIC_TOLL;
			default:
				break;
			}
			const IPhysicsObject* physics = entity->VPhysicsGetObject();
			return std::clamp(physics ? physics->GetMass() / KG_PER_DAMAGE : 5.0f, 3.0f, 30.0f);
		}

		/// a string to a player's minecraft on a StringChannel: the server link's (for their userid), or client.dll's.
		void send_string(CBasePlayer* player, proto::StringChannel channel, const std::string& text)
		{
			if (Link* link = dedicated_link()) {
				link->push_input(proto::kInFor, 0, player->GetUserID());
				link->push_string(channel, text);
				return;
			}
			const PushInputFn push = client_push_input();
			if (!push) {
				return;
			}
			const std::size_t pieces = std::max<std::size_t>(1, (text.size() + proto::kStringPieceBytes - 1) / proto::kStringPieceBytes);
			for (std::size_t piece = 0; piece < pieces; ++piece) {
				const std::size_t offset = piece * proto::kStringPieceBytes;
				const std::size_t bytes = std::min<std::size_t>(proto::kStringPieceBytes, text.size() - offset);
				int               words[3]{};
				std::memcpy(words, text.data() + offset, bytes);
				const int code = static_cast<int>(channel | (bytes << proto::kStringBytesShift) | (piece + 1 == pieces ? proto::kStringEnd : 0));
				push(proto::kInString, code, words[0], words[1], words[2]);
			}
		}

		/// a model path fit to spawn: models/..., .mdl, no way out of the game's folders.
		bool sane_model(const std::string& model)
		{
			return model.size() > 11 && model.size() < MAX_MODEL && !Q_strnicmp(model.c_str(), "models/", 7) && !Q_stricmp(model.c_str() + model.size() - 4, ".mdl") &&
				model.find("..") == std::string::npos && model.find(':') == std::string::npos;
		}
	}

	bool is_minecraft_prop(CBaseEntity* entity)
	{
		if (!entity || entity->GetModelName() == NULL_STRING || entity->GetMoveParent() || entity->IsEffectActive(EF_NODRAW)) {
			return false;
		}
		const char* name = entity->GetClassname();
		return !Q_strncmp(name, "prop_physics", 12) || !Q_strcmp(name, "prop_door_rotating") || !Q_strncmp(name, "prop_dynamic", 12);
	}

	void place_prop(CBasePlayer* player, std::uint32_t kind, std::uint32_t prop_class, const std::string& wanted)
	{
		const props::Kind* entry = kind != 0 ? props::by_kind(kind) : nullptr;
		const std::string  model = entry ? std::string(entry->model) : wanted;
		if (!player || !player->IsAlive() || (kind != 0 && !entry) || !sane_model(model)) {
			return;
		}
		if (modelinfo->GetModelIndex(model.c_str()) < 0) {
			CBaseEntity::PrecacheModel(model.c_str());
		}
		Vector forward;
		AngleVectors(player->EyeAngles(), &forward);
		const Vector eye = player->EyePosition();
		trace_t      probe;
		UTIL_TraceHull(eye, eye + forward * PLACE_REACH, Vector(-PROBE_HALF, -PROBE_HALF, -PROBE_HALF), Vector(PROBE_HALF, PROBE_HALF, PROBE_HALF), MASK_SOLID,
			player, COLLISION_GROUP_NONE, &probe);
		if (probe.startsolid) {
			log_info("props: no room for %s in front of player %d", model.c_str(), player->GetUserID());
			return;
		}
		const std::uint32_t cls = entry ? proto::kPropClassPhysics : prop_class;
		const char*         classname = cls == proto::kPropClassDoor ? "prop_door_rotating" : cls == proto::kPropClassDynamic ? "prop_dynamic"
																															  : "prop_physics";
		const QAngle        angles(0.0f, player->EyeAngles().y + (cls == proto::kPropClassDoor ? 90.0f : 0.0f), 0.0f);
		// the model's box as it will stand (turned by its yaw), around its origin: a door's is its hinge, mid-height
		Vector box_min(-PROBE_HALF, -PROBE_HALF, -PROBE_HALF), box_max(PROBE_HALF, PROBE_HALF, PROBE_HALF);
		if (const model_t* model_info = modelinfo->GetModel(modelinfo->GetModelIndex(model.c_str()))) {
			Vector mins, maxs;
			modelinfo->GetModelRenderBounds(model_info, mins, maxs);
			matrix3x4_t turn;
			AngleMatrix(angles, turn);
			TransformAABB(turn, mins, maxs, box_min, box_max);
		}
		// out from the eye where all of it fits (its box's middle on the line of sight), then down onto the floor
		const Vector middle = (box_min + box_max) * 0.5f;
		trace_t      fit;
		UTIL_TraceHull(eye, eye + forward * PLACE_REACH, box_min - middle, box_max - middle, MASK_SOLID, player, COLLISION_GROUP_NONE, &fit);
		Vector at = (fit.startsolid ? probe.endpos : fit.endpos) - middle;
		if (cls != proto::kPropClassPhysics) {
			// a door and a dynamic prop stand where they're put, their bottom a unit over the floor (a door touching it
			// finds its swing blocked)
			trace_t down;
			UTIL_TraceHull(at, at - Vector(0.0f, 0.0f, 512.0f), box_min, box_max, MASK_SOLID, player, COLLISION_GROUP_NONE, &down);
			if (!down.startsolid) {
				at = down.endpos + Vector(0.0f, 0.0f, 1.0f);
			}
		}
		CBaseEntity* prop = CreateEntityByName(classname);
		if (!prop) {
			return;
		}
		prop->KeyValue("model", model.c_str());
		if (cls == proto::kPropClassDoor) {
			prop->KeyValue("hardware", "1");
			prop->KeyValue("distance", "90");
			prop->KeyValue("speed", "100");
			prop->KeyValue("returndelay", "-1");
		} else if (cls == proto::kPropClassDynamic) {
			prop->KeyValue("solid", "6");  // its model's collision
		}
		prop->SetAbsOrigin(at);
		prop->SetAbsAngles(angles);
		if (DispatchSpawn(prop) < 0) {
			UTIL_Remove(prop);
			return;
		}
		prop->Activate();
		if (cls == proto::kPropClassPhysics) {
			if (!prop->VPhysicsGetObject()) {
				// a model with no physics of its own can't be a physics prop: it stands instead
				UTIL_Remove(prop);
				place_prop(player, 0, proto::kPropClassDynamic, model);
				return;
			}
		}
		log_info("props: player %d put %s (%s) at (%.0f %.0f %.0f)", player->GetUserID(), model.c_str(), classname, at.x, at.y, at.z);
	}

	void harvest_hit(CBasePlayer* player, CBaseEntity* prop, float mc_damage, MapSlot slot)
	{
		if (!player || !is_minecraft_prop(prop) || !(mc_damage > 0.0f)) {
			return;
		}
		const std::string  model = STRING(prop->GetModelName());
		const props::Kind* entry = props::by_model(model);
		float&             taken = g_taken[prop->GetRefEHandle().ToInt()];
		taken += mc_damage;
		const float toll = toll_of(prop, entry);
		if (taken < toll) {
			// a hit that doesn't break it pushes it a little, away from the player
			if (IPhysicsObject* physics = prop->VPhysicsGetObject(); physics && physics->IsMoveable()) {
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
		char         text[MAX_MODEL + 96];
		Q_snprintf(text, sizeof(text), "%u %u %.2f %.2f %.2f %s", entry ? entry->kind : 0u, class_of(prop), mc.x, mc.y, mc.z, model.c_str());
		send_string(player, proto::kStrPropDropped, text);
		log_info("props: player %d broke %s (%s) down into its item (%.1f of minecraft's damage)", player->GetUserID(), model.c_str(), prop->GetClassname(), taken);
		UTIL_Remove(prop);
	}

	void prop_model_piece(int user_id, std::uint32_t index, std::uint32_t length, const std::uint32_t words[6])
	{
		Pieces& pieces = g_pieces[user_id];
		if (index == 0) {
			pieces = Pieces{ {}, length };
		}
		if (length > MAX_MODEL || length != pieces.length || pieces.path.size() != index * 24u || pieces.path.size() >= length) {
			pieces = Pieces{};  // out of order, or too long: the place that follows has no model
			return;
		}
		char bytes[24];
		std::memcpy(bytes, words, sizeof(bytes));
		pieces.path.append(bytes, std::min<std::size_t>(24, length - pieces.path.size()));
	}

	std::string take_prop_model(int user_id)
	{
		const auto it = g_pieces.find(user_id);
		if (it == g_pieces.end()) {
			return {};
		}
		std::string path = it->second.path.size() == it->second.length ? it->second.path : std::string();
		g_pieces.erase(it);
		return path;
	}
}
