#pragma once

// what client.dll and server.dll hand each other directly. both live in the same process: client.dll
// exports these functions and server.dll looks them up by name.
//   solids - minecraft's solid blocks, read off the render ring by the client; the server turns them
//            into collision for npcs, physics and bullets
//   dug    - the cells minecraft dug out of half-life's walls, read off the render ring likewise; the
//            server cuts them out of the world brushes in the collision it streams to minecraft. only
//            with the shader dll (shared/hc_dig_abi.h): without it half-life draws its walls whole,
//            and they stay solid for minecraft too
//   hurts  - half-life's hits on the player, caught by the server; the client puts them on the input
//            ring it owns
//   heals  - half-life's health kits, chargers and suit batteries on the player, seen by the server;
//            likewise
//   inputs - anything else the server has for minecraft's input ring (save checkpoints)
//   holding - whether the player carries a prop with use (server knows); the client sends the mouse
//            buttons to source then, to throw or drop it
//   hazards - minecraft's fire, lava and magma blocks (they come with its block lights); the server
//            burns npcs that stand in them
//   arrows - minecraft's arrows that stuck in an npc (an event the server reads); the client draws
//            them on the npc's bones
//   landings - one of minecraft's jumps source took next to where minecraft put its player, or
//            refused (the player movement, hc_movement.cpp); the client sends minecraft's player there
//   new games - the menu's chapter cfgs run the client's hc_new_game before their map; the server
//            asks as the map loads, which tells a new game from a `map` typed into the console
//   in level - whether the client still has a level's entities; the server changes the characters'
//            model files only when it hasn't (hc_campaign_models.cpp)
//   server in level - the one the other way, a server.dll export client.dll looks up: whether the
//            server has a level; the client's hc_host waits for it to go before maxplayers (hc_coop.cpp)
//   speakers - who says a close caption (the server knows); minecraft's chat names them

#include <cstdint>

#include "core/hc_module.h"

namespace halfcraft
{
	/// one 16x16x16 section's solid blocks (plain layout: it crosses a dll boundary).
	struct SolidSection
	{
		std::int32_t  sx, sy, sz;  // section coords (minecraft)
		std::uint32_t count;          // solid blocks; 0: none (the section's collision is gone)
		std::uint8_t  bits[512];      // bit (x + 16 z + 256 y), minecraft axes
		std::uint8_t  takeable[512];  // the solid blocks the gravity gun may tear out (proto::kRenSolids), same bits
	};

	/// sections whose solids changed after version since.
	/// @param out - up to max of them
	/// @param now - the current version (pass it back as since next time)
	/// @return how many changed (may exceed max: call again with room for all)
	using SolidsSinceFn = int (*)(std::uint32_t since, SolidSection* out, int max, std::uint32_t* now);

	inline constexpr char HC_SOLIDS_EXPORT[] = "HalfCraft_SolidsSince";

	/// one 16x16x16 section's cells dug out of half-life's walls (plain layout: it crosses a dll boundary).
	struct DugSection
	{
		std::int32_t  sx, sy, sz;  // section coords (minecraft)
		std::uint32_t count;       // dug cells; 0: none (its holes are gone)
		std::uint8_t  bits[512];   // bit (x + 16 z + 256 y), minecraft axes
	};

	/// sections whose dug cells changed after version since (as SolidsSinceFn).
	/// @param out - up to max of them
	/// @param now - the current version (pass it back as since next time)
	/// @return how many changed (may exceed max: call again with room for all)
	using DugSinceFn = int (*)(std::uint32_t since, DugSection* out, int max, std::uint32_t* now);

	inline constexpr char HC_DUG_EXPORT[] = "HalfCraft_DugSince";

	/// server.dll -> client.dll: half-life hit the player; goes to minecraft as proto::kInHurt
	/// (client.dll owns the input ring).
	/// @param kind - proto::HurtKind
	/// @param damage - half-life's damage (minecraft divides it by 5: 100 hp -> 20)
	/// @param attacker - the attacker's actor id (proto::ActorRecord::id), 0 for none
	using PushHurtFn = void (*)(int kind, float damage, std::uint32_t attacker, std::uint32_t flags);

	inline constexpr char HC_PUSH_HURT_EXPORT[] = "HalfCraft_PushHurt";

	/// server.dll -> client.dll: half-life healed the player; goes to minecraft as proto::kInHeal.
	/// @param kind - proto::HealKind
	/// @param amount - half-life points (minecraft divides them by 5)
	using PushHealFn = void (*)(int kind, float amount);

	inline constexpr char HC_PUSH_HEAL_EXPORT[] = "HalfCraft_PushHeal";

	/// server.dll -> client.dll: an event for minecraft's input ring (proto::InputEvent).
	using PushInputFn = void (*)(int type, int code, int a, int b, int c);

	inline constexpr char HC_PUSH_INPUT_EXPORT[] = "HalfCraft_PushInput";

	/// a dedicated server's server.so: where client_push_input's events go instead, its server link's input ring
	/// (hc_vitals' set_dedicated_link). per module, as each module has its own copy of this function's static.
	inline PushInputFn& dedicated_push_input()
	{
		static PushInputFn push_input = nullptr;
		return push_input;
	}

	/// server.dll's way to client.dll's PushInputFn, looked up until client.dll has it.
	/// @return nullptr while it doesn't: an event then goes nowhere
	inline PushInputFn client_push_input()
	{
		if (const PushInputFn dedicated = dedicated_push_input()) {
			return dedicated;
		}
		static PushInputFn push_input = nullptr;
		if (!push_input) {
			push_input = reinterpret_cast<PushInputFn>(find_export("client.dll", HC_PUSH_INPUT_EXPORT));
		}
		return push_input;
	}

	/// server.dll -> client.dll, every frame: the player carries a prop (half-life's use pickup).
	using SetHoldingFn = void (*)(int holding);

	inline constexpr char HC_SET_HOLDING_EXPORT[] = "HalfCraft_SetHolding";

	/// what hurts at a minecraft block.
	/// @return proto::BlockHazard (kHazardNone for nothing)
	using HazardAtFn = int (*)(int x, int y, int z);

	inline constexpr char HC_HAZARD_AT_EXPORT[] = "HalfCraft_HazardAt";

	/// server.dll -> client.dll: a minecraft arrow stuck in an npc (proto::kEvArrowStuck).
	/// @param entindex - the npc's entity index (the same on both sides)
	/// @param hit - where it hit, source units
	/// @param direction - its flight direction, source axes, unit length
	using StickArrowFn = void (*)(int entindex, const float hit[3], const float direction[3]);

	inline constexpr char HC_STICK_ARROW_EXPORT[] = "HalfCraft_StickArrow";

	/// server.dll -> client.dll: minecraft's screenshot key (proto::kEvScreenshot, an event the server
	/// reads). the client reads the next finished frame back and answers on proto::kStrScreenshot.
	/// @param request - minecraft's request number (proto::McEvent::actorId)
	using RequestScreenshotFn = void (*)(std::uint32_t request);

	inline constexpr char HC_REQUEST_SCREENSHOT_EXPORT[] = "HalfCraft_RequestScreenshot";

	/// server.dll -> client.dll: source took one of minecraft's jumps next to where minecraft put its
	/// player, which didn't fit there (a pearl against a ledge or a ceiling), or refused it. single
	/// player has no client prediction to see it: the client resyncs minecraft's player once source's
	/// is there.
	/// @param feet - where source put the player, or kept it, source units
	/// @param refused - nonzero: source refused the jump
	using JumpLandedFn = void (*)(const float feet[3], int refused);

	inline constexpr char HC_JUMP_LANDED_EXPORT[] = "HalfCraft_JumpLanded";

	/// server.dll -> client.dll, as a map loads: whether the menu's chapter cfg ran hc_new_game since the
	/// last call (a new game from the menu, not a `map` typed into the console); the call clears it. the
	/// command is client.dll's: the menu disconnects first, and server.dll's commands don't run without
	/// a server.
	/// @return nonzero: it did
	using TakeNewGameFn = int (*)();

	inline constexpr char HC_TAKE_NEW_GAME_EXPORT[] = "HalfCraft_TakeNewGame";

	/// server.dll -> client.dll, as a map loads: whether the client has a level's entities (from its
	/// LevelInitPreEntity until its LevelShutdownPostEntity). they point into the model cache's data,
	/// so the server changes no model files then (a changelevel across campaigns, if the client stays
	/// in the old level until the new one spawns).
	/// @return nonzero: it has
	using ClientInLevelFn = int (*)();

	inline constexpr char HC_CLIENT_IN_LEVEL_EXPORT[] = "HalfCraft_ClientInLevel";

	/// client.dll -> server.dll, the one export server.dll has: whether this process's server has a level (from
	/// its LevelInitPreEntity until its LevelShutdownPostEntity, which the engine's shutdown of the server runs
	/// just before it marks it inactive). the engine refuses maxplayers while a server runs, and disconnect only
	/// asks for the shutdown, so hc_host runs maxplayers once this says the level is gone.
	/// @return nonzero: it has
	using ServerInLevelFn = int (*)();

	inline constexpr char HC_SERVER_IN_LEVEL_EXPORT[] = "HalfCraft_ServerInLevel";

	/// server.dll -> client.dll: who says a close caption the server sends (a sound's or a scene's).
	/// the caption that reaches the client has only its token; minecraft's chat names the speaker.
	/// @param token - the caption's token, lower case
	/// @param speaker - "<classname>/<model file name, no folder or extension>", lower case
	using CaptionSpeakerFn = void (*)(const char* token, const char* speaker);

	inline constexpr char HC_CAPTION_SPEAKER_EXPORT[] = "HalfCraft_CaptionSpeaker";
}
