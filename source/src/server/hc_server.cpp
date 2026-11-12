// server.dll: streams half-life 2's collision to minecraft and hides half-life's own hud and
// weapons while minecraft drives the player. the client writes the host state; this side only
// reads it (for the collision epoch and whether a map is loaded).

#include "cbase.h"
#include "player.h"
#include "in_buttons.h"
#include "hl2/weapon_physcannon.h"
#include "tier0/icommandline.h"

#include "tier0/valve_minmax_off.h"
#include <algorithm>
#include <memory>
#include <vector>

#include "core/hc_collision.h"
#include "core/hc_link.h"
#include "core/hc_log.h"
#include "core/hc_module.h"
#include "server/hc_blast.h"
#include "server/hc_block_solids.h"
#include "server/hc_checkpoints.h"
#include "server/hc_combat.h"
#include "server/hc_debug_target.h"
#include "server/hc_dig.h"
#include "server/hc_hazards.h"
#include "server/hc_held_blocks.h"
#include "server/hc_mobs.h"
#include "server/hc_players.h"
#include "server/hc_push.h"
#include "server/hc_vitals.h"
#include "server/hc_weapons.h"
#include "server/hc_world_collision.h"
#include "shared/hc_bridge.h"
#include "shared/hc_floors.h"
#include "shared/hc_hooks.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		// half-life's hud parts minecraft replaces while it owns the player's health (also on ladders
		// and rides): its health/suit, weapon selection and crosshair. the flashlight meter stays (V
		// still toggles the flashlight).
		constexpr int   HIDDEN_HUD = HIDEHUD_HEALTH | HIDEHUD_WEAPONSELECTION | HIDEHUD_CROSSHAIR;
		constexpr float PUPPET_GRACE_SECONDS = 0.5f;
		// where the pvs looks around a dug cell's centre along each axis, units: 1 inside its face and 1 outside
		// it (the centre is often in a brush, and a wall can end on the cell's face)
		constexpr float       PVS_PROBE_INSIDE = static_cast<float>(UNITS_PER_BLOCK) / 2.0f - 1.0f;
		constexpr float       PVS_PROBE_OUTSIDE = static_cast<float>(UNITS_PER_BLOCK) / 2.0f + 1.0f;
		constexpr std::size_t PVS_NOT_LOGGED = static_cast<std::size_t>(-1);  // hc_debug_dig_pvs logged nothing yet

		ConVar hc_debug_use("hc_debug_use", "0", 0, "halfcraft: log what half-life's use finds whenever it's pressed");
		ConVar hc_debug_dig_pvs("hc_debug_dig_pvs", "0", 0, "halfcraft: log what the holes near the player add to its pvs");

		float g_last_puppet_move = -1.0f;

		void console_sink(int severity, const char* line)
		{
			if (severity == LOG_INFO) {
				Msg("%s\n", line);
			} else {
				Warning("%s\n", line);
			}
		}

		class HalfCraftServerSystem final : public CAutoGameSystemPerFrame
		{
		public:
			HalfCraftServerSystem() : CAutoGameSystemPerFrame("HalfCraftServer") {}

			bool Init() override
			{
				set_log_sink(console_sink);
				world_.set_dig(&dig_);
				// the link client.dll creates: server.dll starts first, from the same command line and environment.
				// a dedicated server has no client.dll: server.dll makes the link itself, for the halfcraft server's
				// minecraft (docs/server/PLAN.md), and keeps its heartbeat
				const char* parameter = CommandLine()->FindParm(LINK_ID_PARAMETER) ? CommandLine()->ParmValue(LINK_ID_PARAMETER, "") : nullptr;
				const auto  link = process_link_names(parameter);
				dedicated_ = engine->IsDedicatedServer();
				link_ready_ = link && (dedicated_ ? link_.create(*link) : link_.attach(*link));
				set_dedicated_link(dedicated_ && link_ready_ ? &link_ : nullptr);
				if (link_ready_) {
					streamer_ = std::make_unique<CollisionStreamer>(link_);
				}
				return true;
			}

			void Shutdown() override
			{
				streamer_.reset();
				set_log_sink(nullptr);
			}

			void LevelInitPostEntity() override
			{
				slot_ = { map_slot(STRING(gpGlobals->mapname)), map_grid_z(STRING(gpGlobals->mapname)) };
				world_.reset(slot_, STRING(gpGlobals->mapname));
				solids_.reset(slot_);
				dig_.reset(slot_);
				held_blocks_.reset(slot_);
				combat_.reset(slot_);
				half_life_blasts_reset(slot_);
				vitals_.reset();
				checkpoints_.on_level_loaded(slot_);
				g_last_puppet_move = -1.0f;  // the clock starts over with the map
				if (dedicated_ && streamer_) {
					// no client.dll counts the levels for a dedicated server: each one is a new epoch, which
					// minecraft's collision starts over on
					streamer_->reset(++epoch_);
				}
			}

			// a save is being written (its entities come next)
			void OnSave() override { checkpoints_.on_save(); }

			void LevelShutdownPreEntity() override
			{
				world_.reset({}, nullptr);
				mobs_.reset();
			}

			void FrameUpdatePostEntityThink() override;
			/// a dedicated server's streaming (FrameUpdatePostEntityThink)
			void stream_for_players();

			/// hc_debug_voxels
			void check_voxels(int radius)
			{
				CBasePlayer* player = UTIL_GetLocalPlayer();
				if (!player) {
					log_info("hc_debug_voxels: no player");
					return;
				}
				const Vector origin = player->GetAbsOrigin();
				const float  feet[3] = { origin.x, origin.y, origin.z };
				world_.check_voxels(source_to_mc(feet, slot_), radius);
			}

			/// hc_debug_dig_material
			void log_dig_material()
			{
				CBasePlayer* player = UTIL_GetLocalPlayer();
				if (!player) {
					log_info("hc_debug_dig_material: no player");
					return;
				}
				Vector forward;
				player->EyeVectors(&forward);
				world_.log_dig_material(player->EyePosition(), forward);
			}

			/// box_dug (shared/hc_hooks.h): the cells cut out of the collision, in this map's slot
			bool box_dug(const Vector& mins, const Vector& maxs) const
			{
				return source_box_dug(mins.Base(), maxs.Base(), slot_, [this](int x, int y, int z) { return dig_.dug(x, y, z); });
			}

			/// server_add_dig_pvs (shared/hc_hooks.h)
			void add_dig_pvs(CBasePlayer* player);

		private:
			void update_hud(CBasePlayer* player, bool hide);
			void update_use(CBasePlayer* player);
			void outline_cut_pieces(CBasePlayer* player) const;
			void add_pvs_point(const Vector& point);

			Link                               link_;
			bool                               link_ready_ = false;
			std::unique_ptr<CollisionStreamer> streamer_;
			bool                               dedicated_ = false;  // srcds: no client.dll, no local player
			WorldCollision                     world_;
			BlockSolids                        solids_;
			DigState                           dig_;
			HeldBlocks                         held_blocks_;
			Weapons                            weapons_;
			Combat                             combat_;
			Mobs                               mobs_;
			Push                               push_;
			DebugTarget                        debug_target_;
			Vitals                             vitals_;
			Hazards                            hazards_;
			Checkpoints                        checkpoints_;
			MapSlot                            slot_;
			std::uint32_t                      epoch_ = ~0u;
			std::vector<Vector>                pvs_points_;    // the dug cells near the player, this snapshot
			std::vector<int>                   pvs_clusters_;  // the clusters they put in its pvs, after the eye's own
			std::size_t                        pvs_logged_holes_ = PVS_NOT_LOGGED;  // hc_debug_dig_pvs's last line
			std::size_t                        pvs_logged_clusters_ = PVS_NOT_LOGGED;
		};

		HalfCraftServerSystem g_server_system;

		void HalfCraftServerSystem::FrameUpdatePostEntityThink()
		{
			if (!link_ready_ || !streamer_) {
				return;
			}
			solids_.update();  // minecraft's blocks stop npcs, props and bullets
			dig_.update(streamer_.get(), dedicated_ ? &link_ : nullptr);  // and the cells it dug come out of the collision it gets

			CBasePlayer* player = UTIL_GetLocalPlayer();
			const bool   puppeted = player && server_player_puppeted();
			if (dig_.outlined() && player) {
				outline_cut_pieces(player);  // hc_debug_dig
			}
			if (player) {
				update_hud(player, minecraft_owns_health());
				update_use(player);
			}
			weapons_.update(link_, player, minecraft_owns_health());
			// minecraft's weapons work on ladders, rides and in vehicles too: only the movement is source's
			if (dedicated_) {
				combat_.update_players(link_, slot_);  // every player's, through the halfcraft server's minecraft
			} else {
				combat_.update(link_, player, slot_, minecraft_owns_health());
			}
			for (const proto::McEvent& news : combat_.take_held_block_news()) {
				held_blocks_.on_news(news);
			}
			held_blocks_.update();  // the gravity gun's blocks land where they come to rest
			push_.update(player, puppeted && link_.mc_alive());
			mobs_.update(link_, slot_);
			if (dedicated_) {
				vitals_.update_players(link_);  // each player's health is their minecraft's on the halfcraft server
			} else {
				vitals_.update(link_, player);
			}
			if (link_.mc_alive()) {
				hazards_.update(player, slot_);
			}
			debug_target_.update(link_, player);

			if (dedicated_) {
				stream_for_players();
				return;
			}

			proto::HostState host{};
			if (!link_.read_host_state(host)) {
				return;
			}
			// a new level, or minecraft dropped collision it couldn't read: stream it all afresh
			const bool lost = combat_.take_collision_lost();
			if (host.collisionEpoch != epoch_ || lost) {
				epoch_ = host.collisionEpoch;
				streamer_->reset(epoch_);
			}
			const bool in_game = (host.flags & proto::kHostInGame) && !(host.flags & proto::kHostLoading);
			if (!player || !in_game || !link_.mc_alive()) {
				return;
			}

			// collision streams around where minecraft's player is (the same as source's while puppeted,
			// and where minecraft is heading after a teleport otherwise), and under its projectiles that
			// fell out of the bottom of that
			for (const McVec& point : combat_.take_collision_wanted()) {
				streamer_->want_below(point);
			}
			const McVec centre{ host.posX, host.posY, host.posZ };
			world_.track_movers(centre, *streamer_);
			streamer_->update(centre, world_);
		}

		void HalfCraftServerSystem::stream_for_players()
		{
			// a dedicated server: the minecraft server's heartbeat is the link's own, and collision streams around
			// every living player, where source has them (each one's minecraft follows its source player)
			link_.heartbeat();
			for (const McVec& point : combat_.take_collision_wanted()) {
				streamer_->want_below(point);
			}
			std::vector<McVec> players;
			for_each_player([&](CBasePlayer& player) {
				if (player.IsAlive()) {
					const Vector origin = player.GetAbsOrigin();
					const float  feet[3] = { origin.x, origin.y, origin.z };
					players.push_back(source_to_mc(feet, slot_));
				}
			});
			if (players.empty() || !link_.mc_alive()) {
				return;
			}
			world_.track_movers(players.front(), *streamer_);
			streamer_->update(players, world_);
		}

		void HalfCraftServerSystem::outline_cut_pieces(CBasePlayer* player) const
		{
			constexpr double    NEAR_BLOCKS = 16.0;
			const Vector&       origin = player->GetAbsOrigin();
			const float         feet[3] = { origin.x, origin.y, origin.z };
			std::vector<DigBox> pieces;
			world_.cut_pieces_near(source_to_mc(feet, slot_), NEAR_BLOCKS, pieces);
			for (const DigBox& piece : pieces) {
				outline_mc_box(piece.lo, piece.hi, slot_, 0, 160, 255);
			}
		}

		void HalfCraftServerSystem::add_dig_pvs(CBasePlayer* player)
		{
			if (dig_.count() == 0) {
				return;  // every snapshot: most maps have no holes
			}
			const Vector eye = player->EyePosition();
			const float  at[3] = { eye.x, eye.y, eye.z };
			dig_.pvs_points(source_to_mc(at, slot_), pvs_points_);
			pvs_clusters_.assign(1, engine->GetClusterForOrigin(eye));  // the eye's cluster is in the pvs already
			constexpr float OFFSETS[] = { -PVS_PROBE_OUTSIDE, -PVS_PROBE_INSIDE, PVS_PROBE_INSIDE, PVS_PROBE_OUTSIDE };
			for (const Vector& centre : pvs_points_) {
				add_pvs_point(centre);
				for (int axis = 0; axis < 3; ++axis) {
					for (const float offset : OFFSETS) {
						Vector point = centre;
						point[axis] += offset;
						add_pvs_point(point);
					}
				}
			}

			const std::size_t holes = pvs_points_.size();
			const std::size_t clusters = pvs_clusters_.size() - 1;
			if (!hc_debug_dig_pvs.GetBool()) {
				pvs_logged_holes_ = PVS_NOT_LOGGED;  // turned on again, it logs what it finds at once
			} else if (holes != pvs_logged_holes_ || clusters != pvs_logged_clusters_) {
				pvs_logged_holes_ = holes;
				pvs_logged_clusters_ = clusters;
				log_info("dig pvs: %zu holes near the player add %zu clusters", holes, clusters);
			}
		}

		void HalfCraftServerSystem::add_pvs_point(const Vector& point)
		{
			// a solid leaf has no cluster (inside a brush, behind the map's walls), and one point a cluster is enough
			const int cluster = engine->GetClusterForOrigin(point);
			if (cluster < 0 || std::find(pvs_clusters_.begin(), pvs_clusters_.end(), cluster) != pvs_clusters_.end()) {
				return;
			}
			pvs_clusters_.push_back(cluster);
			engine->AddOriginToPVS(point);
		}

		void HalfCraftServerSystem::update_use(CBasePlayer* player)
		{
			// the client sends the mouse to source while a prop is carried (throw, drop)
			static SetHoldingFn set_holding = reinterpret_cast<SetHoldingFn>(find_export("client.dll", HC_SET_HOLDING_EXPORT));
			if (set_holding) {
				set_holding(GetPlayerHeldEntity(player) != nullptr ? 1 : 0);
			}
			if (hc_debug_use.GetBool() && (player->m_afButtonPressed & IN_USE)) {
				CBaseEntity* target = player->FindUseEntity();
				log_info("use: %s%s", target ? target->GetClassname() : "nothing in reach", GetPlayerHeldEntity(player) ? " (carrying)" : "");
			}
		}

		void HalfCraftServerSystem::update_hud(CBasePlayer* player, bool hide)
		{
			// the player's own flags, not a copy of them: a save brings back whatever it was saved with.
			// the viewmodel is the weapons' (hc_weapons.cpp)
			const bool hidden = (player->m_Local.m_iHideHUD & HIDDEN_HUD) == HIDDEN_HUD;
			if (hide == hidden) {
				return;
			}
			if (hide) {
				player->m_Local.m_iHideHUD |= HIDDEN_HUD;
			} else {
				player->m_Local.m_iHideHUD &= ~HIDDEN_HUD;
			}
		}
	}

	void server_note_puppet_move()
	{
		g_last_puppet_move = gpGlobals->curtime;
	}

	bool server_player_puppeted()
	{
		return g_last_puppet_move >= 0.0f && gpGlobals->curtime - g_last_puppet_move < PUPPET_GRACE_SECONDS;
	}

	void server_add_dig_pvs(CBasePlayer* player)
	{
		if (player != nullptr) {
			g_server_system.add_dig_pvs(player);
		}
	}

	bool box_dug(const Vector& mins, const Vector& maxs)
	{
		return g_server_system.box_dug(mins, maxs);
	}
}

CON_COMMAND(hc_debug_voxels, "halfcraft: check minecraft's collision voxels around the player against source's own collision: hc_debug_voxels [regions around, 0-6, default 2]")
{
	if (!UTIL_IsCommandIssuedByServerAdmin()) {
		return;
	}
	const int radius = args.ArgC() > 1 ? std::clamp(atoi(args[1]), 0, 6) : 2;
	halfcraft::g_server_system.check_voxels(radius);
}

CON_COMMAND(hc_debug_dig_material, "halfcraft: log the dig material of the world brush under the crosshair: its side's surface, game material and DigMaterial")
{
	if (!UTIL_IsCommandIssuedByServerAdmin()) {
		return;
	}
	halfcraft::g_server_system.log_dig_material();
}
