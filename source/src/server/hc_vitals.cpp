// server.dll: minecraft's health mirrored onto half-life's player (see hc_vitals.h).

#include "cbase.h"
#include "player.h"

#include "tier0/valve_minmax_off.h"
#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "core/hc_log.h"
#include "core/hc_module.h"
#include "server/hc_players.h"
#include "server/hc_vitals.h"
#include "shared/hc_bridge.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		constexpr float SOURCE_PER_MINECRAFT = 5.0f;  // minecraft divides host damage and healing by 5
		constexpr int   MAX_ARMOR = 100;              // half-life's full suit
		constexpr float PENDING_SECONDS = 1.0f;       // healing minecraft hasn't shown by then was refused (full already)
		constexpr int   DROWN_DEBT_RESET = 1 << 30;   // CBasePlayer::AdjustDrownDmg clamps the count to what it gave back already

		bool                    g_minecraft_owns_health = false;
		Link*                   g_dedicated_link = nullptr;
		std::unordered_set<int> g_owned;  // a dedicated server's players whose health minecraft owns, by userid

		void push_heal(CBasePlayer* player, int kind, float amount)
		{
			if (g_dedicated_link) {
				send_player_input(player, proto::kInHeal, kind, static_cast<int>(amount * 100.0f));
				return;
			}
			static PushHealFn push = nullptr;
			if (!push) {
				push = reinterpret_cast<PushHealFn>(find_export("client.dll", HC_PUSH_HEAL_EXPORT));
				if (!push) {
					return;
				}
			}
			push(kind, amount);
		}
	}

	void set_dedicated_link(Link* link)
	{
		g_dedicated_link = link;
		// the world's events (blasts, bullets into blocks, the gravity gun's blocks, hurt mobs) go to the halfcraft
		// server's minecraft as a single player's go to its own: for no player in particular (kInFor 0)
		dedicated_push_input() = link ? +[](int type, int code, int a, int b, int c) {
			if (g_dedicated_link) {
				g_dedicated_link->push_input(proto::kInFor, 0, 0);
				g_dedicated_link->push_input(static_cast<proto::InputType>(type), static_cast<std::uint16_t>(code), a, b, c);
			}
		}
									  : nullptr;
	}

	Link* dedicated_link()
	{
		return g_dedicated_link;
	}

	void send_player_input(CBasePlayer* player, proto::InputType type, int code, int a, int b, int c)
	{
		if (g_dedicated_link) {
			if (player) {
				g_dedicated_link->push_input(proto::kInFor, 0, player->GetUserID());
				g_dedicated_link->push_input(type, static_cast<std::uint16_t>(code), a, b, c);
			}
			return;
		}
		static PushInputFn push = nullptr;
		if (!push) {
			push = reinterpret_cast<PushInputFn>(find_export("client.dll", HC_PUSH_INPUT_EXPORT));
			if (!push) {
				return;
			}
		}
		push(type, code, a, b, c);
	}

	bool minecraft_owns_health(CBasePlayer* player)
	{
		if (!g_dedicated_link) {
			return g_minecraft_owns_health;
		}
		return player && g_owned.count(player->GetUserID()) > 0;
	}

	void Vitals::update_players(Link& link)
	{
		proto::PlayerVitalsTable table{};
		const bool               read = link.mc_alive() && link.read_player_vitals(table);
		g_owned.clear();
		const float frametime = gpGlobals->frametime;
		for_each_player([&](CBasePlayer& player) {
			const int userid = player.GetUserID();
			if (!read || !player.IsAlive()) {
				players_.erase(userid);
				return;
			}
			const proto::PlayerVitalsRecord* mc = nullptr;
			for (std::uint32_t i = 0; i < std::min<std::uint32_t>(table.count, proto::kMaxServerPlayers); ++i) {
				if (static_cast<int>(table.players[i].userId) == userid) {
					mc = &table.players[i];
				}
			}
			if (!mc || !(mc->flags & proto::kVitalsInWorld) || (mc->flags & proto::kVitalsDead) || mc->maxHealth <= 0.0f) {
				players_.erase(userid);
				return;
			}
			g_owned.insert(userid);
			auto& [health_mirror, armor_mirror] = players_[userid];
			player.AdjustDrownDmg(-DROWN_DEBT_RESET);
			const int max_health = static_cast<int>(std::lround(mc->maxHealth * SOURCE_PER_MINECRAFT));
			const int health = std::clamp(health_mirror.update(&player, player.GetHealth(), mc->health, proto::kHealHealth, frametime), 1, max_health);
			player.SetMaxHealth(max_health);
			player.SetHealth(health);
			health_mirror.set = health;
			const int armor = std::clamp(armor_mirror.update(&player, player.ArmorValue(), mc->absorption, proto::kHealArmor, frametime), 0, MAX_ARMOR);
			player.SetArmorValue(armor);
			armor_mirror.set = armor;
		});
	}

	int Vitals::Mirror::update(CBasePlayer* player, int current, float minecraft, int kind, float frametime)
	{
		// half-life added some since the last frame (a kit, a charger tick, a battery, a medic)
		if (set >= 0 && current > set) {
			const float gain = static_cast<float>(current - set);
			push_heal(player, kind, gain);
			pending += gain;
			waited = 0.0f;
		}
		// minecraft's own rise uses up what's pending
		if (seen >= 0.0f && minecraft > seen) {
			pending = std::max(0.0f, pending - (minecraft - seen) * SOURCE_PER_MINECRAFT);
		}
		seen = minecraft;
		if (pending > 0.0f) {
			waited += frametime;
			if (waited > PENDING_SECONDS) {
				pending = 0.0f;
			}
		}
		return static_cast<int>(std::lround(minecraft * SOURCE_PER_MINECRAFT + pending));
	}

	bool minecraft_owns_health()
	{
		return g_minecraft_owns_health;
	}

	void Vitals::update(Link& link, CBasePlayer* player)
	{
		proto::McState mc{};
		g_minecraft_owns_health = player && player->IsAlive() && link.mc_alive() && link.read_mc_state(mc) && (mc.flags & proto::kMcInWorld) &&
			!(mc.flags & proto::kMcDead) && mc.maxHealth > 0.0f;
		if (!g_minecraft_owns_health) {
			health_.reset();
			armor_.reset();
			return;
		}
		// minecraft drowns the player on its own air (server_player_damage drops half-life's DMG_DROWN), but
		// half-life still counts the drowning it would have dealt and gives it back as health once the
		// player surfaces: drop that count so its recovery never heals minecraft for damage it never took
		player->AdjustDrownDmg(-DROWN_DEBT_RESET);
		const float frametime = gpGlobals->frametime;
		const int   max_health = static_cast<int>(std::lround(mc.maxHealth * SOURCE_PER_MINECRAFT));
		const int   health = std::clamp(health_.update(player, player->GetHealth(), mc.health, proto::kHealHealth, frametime), 1, max_health);
		player->SetMaxHealth(max_health);
		player->SetHealth(health);
		health_.set = health;

		const int armor = std::clamp(armor_.update(player, player->ArmorValue(), mc.absorption, proto::kHealArmor, frametime), 0, MAX_ARMOR);
		player->SetArmorValue(armor);
		armor_.set = armor;
	}
}
