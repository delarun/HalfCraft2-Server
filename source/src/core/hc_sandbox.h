#pragma once

// the halfcraft server's sandbox (docs/server/PLAN.md): a map's entity lump with its story taken out before the
// entities spawn, as stripper:source does. half-life 2's maps tell their story through scenes, scripted sequences,
// ai goals, cameras and messages, and lock their vehicles until the story hands them over; a server that never
// restarts its map wants the place and its enemies, not the story. what goes:
//
//   - the story's own classes (STORY_CLASSES, ai_goal_*), the g-man, level changes (portals replace them);
//   - a vehicle's lock: its VehicleLocked key, and every output that locks a vehicle or turns it off;
//   - a prop's fading with distance (fademindist, fademaxdist; fadescale 0): the client works a prop's fade out once
//     a frame, in the first view that draws it, and on the coast that's the water's reflection, seen from under the
//     water, which faded the map's props out of the player's own view whenever water was in it.
//
// enemies, triggers and relays stay, and the map's enemies come back after they die (respawnable, respawn_due). pure: no sdk, no windows, tested by tools/debug/core_tests.cpp.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace halfcraft::sandbox
{
	/// one key and its value, in the order the lump has them (outputs repeat their key).
	using KeyValue = std::pair<std::string, std::string>;
	using Entity = std::vector<KeyValue>;

	/// the classes that carry the story and nothing else.
	inline const std::vector<std::string_view>& story_classes()
	{
		static const std::vector<std::string_view> classes{
			"logic_choreographed_scene",
			"logic_scene_list_manager",
			"scripted_sequence",
			"scripted_scene",
			"aiscripted_schedule",
			"ai_script_conditions",
			"point_viewcontrol",
			"env_message",
			"game_text",
			"logic_autosave",
			"npc_gman",
			"trigger_changelevel",
		};
		return classes;
	}

	/// the inputs that keep a vehicle from its player.
	inline bool locks_vehicle(std::string_view input)
	{
		auto is = [&](std::string_view word) {
			return input.size() == word.size() && std::equal(input.begin(), input.end(), word.begin(), [](char a, char b) {
				return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
			});
		};
		return is("Lock") || is("TurnOff");
	}

	inline std::string lower(std::string_view text)
	{
		std::string out(text);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	/// the value of a key (the first one), or "" without it.
	inline std::string_view value_of(const Entity& entity, std::string_view key)
	{
		for (const auto& [k, v] : entity) {
			if (lower(k) == lower(key)) {
				return v;
			}
		}
		return {};
	}

	/// the lump's entities: { "key" "value" ... } blocks. nullopt-free: what doesn't parse ends the list there.
	inline std::vector<Entity> parse(std::string_view lump)
	{
		std::vector<Entity> entities;
		std::size_t         at = 0;
		auto                quoted = [&](std::string& out) {
			at = lump.find_first_not_of(" \t\r\n", at);
			if (at == std::string_view::npos || lump[at] != '"') {
				return false;
			}
			const std::size_t end = lump.find('"', at + 1);
			if (end == std::string_view::npos) {
				return false;
			}
			out.assign(lump.substr(at + 1, end - at - 1));
			at = end + 1;
			return true;
		};
		while ((at = lump.find('{', at)) != std::string_view::npos) {
			++at;
			Entity      entity;
			std::string key, value;
			while (true) {
				const std::size_t next = lump.find_first_not_of(" \t\r\n", at);
				if (next == std::string_view::npos) {
					return entities;
				}
				if (lump[next] == '}') {
					at = next + 1;
					break;
				}
				if (!quoted(key) || !quoted(value)) {
					return entities;
				}
				entity.emplace_back(std::move(key), std::move(value));
			}
			entities.push_back(std::move(entity));
		}
		return entities;
	}

	/// the entities back as a lump.
	inline std::string serialize(const std::vector<Entity>& entities)
	{
		std::string out;
		for (const Entity& entity : entities) {
			out += "{\n";
			for (const auto& [key, value] : entity) {
				out += '"';
				out += key;
				out += "\" \"";
				out += value;
				out += "\"\n";
			}
			out += "}\n";
		}
		return out;
	}

	/// an output's target and input: "target,input,param,delay,times", its fields split by ',' or (newer
	/// compilers) by ESC.
	inline std::pair<std::string_view, std::string_view> output_target(std::string_view output)
	{
		const char        separator = output.find('\x1b') != std::string_view::npos ? '\x1b' : ',';
		const std::size_t first = output.find(separator);
		if (first == std::string_view::npos) {
			return { output, {} };
		}
		const std::size_t second = output.find(separator, first + 1);
		return { output.substr(0, first), output.substr(first + 1, second == std::string_view::npos ? std::string_view::npos : second - first - 1) };
	}

	struct Report
	{
		int removed = 0;          // entities taken out
		int unlocked = 0;         // vehicles whose lock key went
		int outputs_removed = 0;  // outputs that locked or turned off a vehicle
		int unfaded = 0;          // props that faded with distance and don't any more
	};

	/// the story taken out of a map's entities (the header says what).
	inline std::vector<Entity> apply(std::vector<Entity> entities, Report& report)
	{
		auto story = [](std::string_view classname) {
			const std::string name = lower(classname);
			const auto&       classes = story_classes();
			return name.rfind("ai_goal_", 0) == 0 || std::find(classes.begin(), classes.end(), name) != classes.end();
		};
		std::unordered_set<std::string> vehicles;
		for (const Entity& entity : entities) {
			const std::string classname = lower(value_of(entity, "classname"));
			const std::string name = lower(value_of(entity, "targetname"));
			if (classname.rfind("prop_vehicle_", 0) == 0 && classname != "prop_vehicle_crane" && !name.empty()) {
				vehicles.insert(name);
			}
		}
		std::vector<Entity> kept;
		kept.reserve(entities.size());
		for (Entity& entity : entities) {
			const std::string classname = lower(value_of(entity, "classname"));
			if (story(classname)) {
				++report.removed;
				continue;
			}
			const bool vehicle = classname.rfind("prop_vehicle_", 0) == 0 && classname != "prop_vehicle_crane";
			// a prop draws at any distance (the header says why)
			if (classname.rfind("prop_", 0) == 0 && classname.rfind("prop_vehicle_", 0) != 0) {
				const auto faded = std::remove_if(entity.begin(), entity.end(), [](const KeyValue& kv) {
					const std::string key = lower(kv.first);
					return key == "fademindist" || key == "fademaxdist" || key == "fadescale";
				});
				if (faded != entity.end()) {
					++report.unfaded;
					entity.erase(faded, entity.end());
				}
				entity.emplace_back("fadescale", "0");
			}
			entity.erase(std::remove_if(entity.begin(), entity.end(),
							 [&](const KeyValue& kv) {
								 const std::string key = lower(kv.first);
								 if (vehicle && key == "vehiclelocked") {
									 ++report.unlocked;
									 return true;
								 }
								 if (key.rfind("on", 0) != 0 && key != "playeron" && key != "playeroff" && key != "pressedattack") {
									 return false;  // not an output
								 }
								 const auto [target, input] = output_target(kv.second);
								 if (vehicles.count(lower(target)) && locks_vehicle(input)) {
									 ++report.outputs_removed;
									 return true;
								 }
								 return false;
							 }),
				entity.end());
			kept.push_back(std::move(entity));
		}
		return kept;
	}

	// ---- enemies coming back ----------------------------------------------------------------------------------------
	// the map's own enemies (not its bosses, its friends or an npc_maker's templates) spawn again where the map put
	// them, a while after they died and only while no player is near: the story's waves are gone with the story.

	/// npc spawnflags: an npc_maker's template, which never spawns by itself (ai_basenpc.h's SF_NPC_TEMPLATE).
	inline constexpr int SPAWNFLAG_TEMPLATE = 1 << 11;

	/// whether a map's npc comes back after it dies: an enemy of the campaigns' rank and file, not a template.
	inline bool respawnable(std::string_view classname, int spawnflags)
	{
		static const std::vector<std::string_view> enemies{
			"npc_combine_s",
			"npc_metropolice",
			"npc_antlion",
			"npc_antlion_worker",
			"npc_zombie",
			"npc_zombie_torso",
			"npc_zombine",
			"npc_fastzombie",
			"npc_fastzombie_torso",
			"npc_poisonzombie",
			"npc_headcrab",
			"npc_headcrab_fast",
			"npc_headcrab_black",
			"npc_headcrab_poison",
			"npc_rollermine",
			"npc_manhack",
			"npc_barnacle",
			"npc_cscanner",
		};
		const std::string name = lower(classname);
		return (spawnflags & SPAWNFLAG_TEMPLATE) == 0 && std::find(enemies.begin(), enemies.end(), name) != enemies.end();
	}

	/// an npc's spawnflags key as a number (0 without one or for what isn't one).
	inline int spawnflags_of(const Entity& entity)
	{
		const std::string_view text = value_of(entity, "spawnflags");
		int                    flags = 0;
		for (const char c : text) {
			if (c < '0' || c > '9') {
				return 0;
			}
			flags = flags * 10 + (c - '0');
		}
		return flags;
	}

	/// whether a dead enemy spawns again now.
	/// @param died_at - when it was found dead (seconds, the server's clock)
	/// @param nearest_player - how far the nearest living player is from its spawn spot; negative for no player
	inline bool respawn_due(double died_at, double now, double delay, double nearest_player, double keep_away)
	{
		return now - died_at >= delay && (nearest_player < 0.0 || nearest_player >= keep_away);
	}
}
