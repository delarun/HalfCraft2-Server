// server.dll: the halfcraft server's sandbox (core/hc_sandbox.h): with more than one slot the map's entities lose
// their story before they spawn, and the map's enemies spawn again where the map put them a while after they die,
// while no player is near. singleplayer plays the story as it always did.

#include "cbase.h"
#include "ai_basenpc.h"
#include "player.h"

#include "tier0/valve_minmax_off.h"

#include <cstdio>
#include <string>
#include <vector>

#include "core/hc_log.h"
#include "core/hc_sandbox.h"
#include "server/hc_players.h"
#include "shared/hc_hooks.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		ConVar hc_sandbox("hc_sandbox", "1", FCVAR_GAMEDLL,
			"with more than one slot, maps load without their story (scenes, scripted sequences, ai goals, cameras, messages, level changes) and with "
			"their vehicles unlocked: from the next map load");

		ConVar hc_respawn("hc_respawn", "1", FCVAR_GAMEDLL, "in the sandbox, the map's enemies spawn again where the map put them after they die");
		ConVar hc_respawn_delay("hc_respawn_delay", "180", FCVAR_GAMEDLL, "seconds a dead enemy of the map stays dead at least", true, 5.0f, false, 0.0f);
		ConVar hc_respawn_distance("hc_respawn_distance", "1500", FCVAR_GAMEDLL,
			"a dead enemy of the map spawns again only while no player is closer to its spot than this (units)", true, 0.0f, false, 0.0f);

		std::string                  g_entities;  // the lump the engine parses: alive until the next level's
		std::vector<sandbox::Entity> g_kept;  // the same, as entities: the enemies' keys come back from here
		bool                         g_applied = false;  // this level's lump went through the sandbox

		/// one of the map's enemies: its keys, and the entity they made, while it lives.
		struct Enemy
		{
			const sandbox::Entity* keys;
			Vector                 spot;
			EHANDLE                entity;
			double                 died_at = -1.0;  // when it was found dead; -1 while it lives
		};

		Vector origin_of(const sandbox::Entity& entity)
		{
			Vector     origin(0.0f, 0.0f, 0.0f);
			const auto text = std::string(sandbox::value_of(entity, "origin"));
			std::sscanf(text.c_str(), "%f %f %f", &origin.x, &origin.y, &origin.z);
			return origin;
		}

		/// how far the nearest living player is from a spot; -1 with none.
		float nearest_player(const Vector& spot)
		{
			float nearest = -1.0f;
			for_each_player([&](CBasePlayer& player) {
				if (player.IsAlive()) {
					const float distance = (player.GetAbsOrigin() - spot).Length();
					nearest = nearest < 0.0f ? distance : std::min(nearest, distance);
				}
			});
			return nearest;
		}

		class RespawnSystem final : public CAutoGameSystemPerFrame
		{
		public:
			RespawnSystem() : CAutoGameSystemPerFrame("HalfCraftRespawn") {}

			void LevelInitPostEntity() override
			{
				enemies_.clear();
				next_check_ = 0.0;
				if (!g_applied) {
					return;
				}
				// the map's npcs as they spawned, by hammer id: only these come back (a template never spawned)
				for (const sandbox::Entity& keys : g_kept) {
					if (!sandbox::respawnable(sandbox::value_of(keys, "classname"), sandbox::spawnflags_of(keys))) {
						continue;
					}
					const int id = atoi(std::string(sandbox::value_of(keys, "hammerid")).c_str());
					for (CBaseEntity* entity = gEntList.FirstEnt(); entity && id > 0; entity = gEntList.NextEnt(entity)) {
						if (entity->m_iHammerID == id && entity->IsNPC()) {
							enemies_.push_back({ &keys, origin_of(keys), entity });
							break;
						}
					}
				}
				log_info("sandbox: %zu enemies of the map come back %.0f s after they die", enemies_.size(), hc_respawn_delay.GetFloat());
			}

			void LevelShutdownPostEntity() override { enemies_.clear(); }

			void FrameUpdatePostEntityThink() override
			{
				const double now = gpGlobals->curtime;
				if (enemies_.empty() || !hc_respawn.GetBool() || now < next_check_) {
					return;
				}
				next_check_ = now + 1.0;
				for (Enemy& enemy : enemies_) {
					CBaseEntity* entity = enemy.entity.Get();
					if (entity && entity->IsAlive()) {
						continue;
					}
					if (enemy.died_at < 0.0) {
						enemy.died_at = now;
						continue;
					}
					if (sandbox::respawn_due(enemy.died_at, now, hc_respawn_delay.GetFloat(), nearest_player(enemy.spot), hc_respawn_distance.GetFloat())) {
						respawn(enemy);
					}
				}
			}

			/// hc_debug_respawn kill: every one of them that lives goes, as if it died (sv_cheats)
			void remove_all()
			{
				int removed = 0;
				for (Enemy& enemy : enemies_) {
					if (CBaseEntity* entity = enemy.entity.Get(); entity && entity->IsAlive()) {
						UTIL_Remove(entity);
						++removed;
					}
				}
				Msg("hc_debug_respawn: %d removed\n", removed);
			}

			/// hc_debug_respawn
			void report() const
			{
				const double now = gpGlobals->curtime;
				Msg("%zu enemies of the map come back (hc_respawn %d, delay %.0f s, away %.0f units):\n", enemies_.size(), hc_respawn.GetInt(),
					hc_respawn_delay.GetFloat(), hc_respawn_distance.GetFloat());
				for (const Enemy& enemy : enemies_) {
					const CBaseEntity* entity = enemy.entity.Get();
					const std::string  classname(sandbox::value_of(*enemy.keys, "classname"));
					const std::string  name(sandbox::value_of(*enemy.keys, "targetname"));
					if (entity && const_cast<CBaseEntity*>(entity)->IsAlive()) {
						Msg("  %s %s: alive (entity %d)\n", classname.c_str(), name.c_str(), entity->entindex());
					} else {
						Msg("  %s %s: dead %.0f s, nearest player %.0f units\n", classname.c_str(), name.c_str(), enemy.died_at < 0.0 ? 0.0 : now - enemy.died_at,
							nearest_player(enemy.spot));
					}
				}
			}

		private:
			void respawn(Enemy& enemy)
			{
				const std::string classname(sandbox::value_of(*enemy.keys, "classname"));
				CBaseEntity*      entity = CreateEntityByName(classname.c_str());
				if (entity == nullptr) {
					enemy.died_at = gpGlobals->curtime;  // try again after another delay
					return;
				}
				for (const auto& [key, value] : *enemy.keys) {
					if (sandbox::lower(key) != "classname") {
						entity->KeyValue(key.c_str(), value.c_str());
					}
				}
				DispatchSpawn(entity);
				entity->Activate();
				enemy.entity = entity;
				enemy.died_at = -1.0;
				log_info("sandbox: %s came back at (%.0f %.0f %.0f)", classname.c_str(), enemy.spot.x, enemy.spot.y, enemy.spot.z);
			}

			std::vector<Enemy> enemies_;
			double             next_check_ = 0.0;
		};

		RespawnSystem g_respawn;

		CON_COMMAND(hc_debug_respawn, "halfcraft: the sandbox's enemies that come back, and how each one is; hc_debug_respawn kill takes them all out (sv_cheats)")
		{
			if (args.ArgC() > 1 && V_stricmp(args[1], "kill") == 0) {
				if (!sv_cheats->GetBool()) {
					Msg("hc_debug_respawn kill needs sv_cheats 1\n");
					return;
				}
				g_respawn.remove_all();
			}
			g_respawn.report();
		}
	}

	const char* server_map_entities(const char* map, const char* entities)
	{
		g_applied = false;
		g_kept.clear();
		if (entities == nullptr || gpGlobals->maxClients <= 1 || !hc_sandbox.GetBool()) {
			return entities;
		}
		sandbox::Report report;
		g_kept = sandbox::apply(sandbox::parse(entities), report);
		g_entities = sandbox::serialize(g_kept);
		g_applied = true;
		log_info("sandbox: %s without its story: %d entities out, %d vehicle(s) unlocked, %d output(s) that lock a vehicle out, %d prop(s) unfaded",
			map ? map : "?", report.removed, report.unlocked, report.outputs_removed, report.unfaded);
		return g_entities.c_str();
	}
}
