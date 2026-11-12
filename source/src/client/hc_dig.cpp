// client.dll: the cells minecraft dug out of half-life's walls (see hc_dig.h).

#include "cbase.h"
#include "engine/IEngineTrace.h"
#include "filesystem.h"
#include "materialsystem/imaterialsystem.h"
#include "tier1/callqueue.h"
#include "tier1/utlbuffer.h"
#include "view.h"
#include "detailobjectsystem.h"

#include "tier0/valve_minmax_off.h"
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "halfcraft_protocol.h"
#include "client/hc_dig.h"
#include "client/hc_texture.h"
#include "core/hc_dig_mask.h"
#include "core/hc_dug_cells.h"
#include "core/hc_log.h"
#include "core/hc_module.h"
#include "core/hc_units.h"
#include "shared/hc_bridge.h"
#include "shared/hc_dig_abi.h"
#include "shared/hc_hooks.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		constexpr int    SECTION_BLOCKS = 16;
		constexpr int    TEXEL_BYTES = 4;  // the dig mask's rgba
		constexpr float  CROSSHAIR_REACH = 180.0f;  // units hc_debug_dig_cell looks for a wall in (4.5 blocks: minecraft's reach)
		constexpr float  INTO_WALL = 0.5f;          // units behind the face under the crosshair: in the cell the face belongs to
		constexpr double READBACK_TIMEOUT_SECONDS = 2.0;  // hc_debug_dig_mask_dump waits for a frame, then for its read

		ConVar hc_dig("hc_dig", "1", FCVAR_ARCHIVE, "halfcraft: 0 - minecraft digs no new cells out of half-life's walls (the ones dug stay dug)");
		ConVar hc_debug_dig_mask("hc_debug_dig_mask", "0", 0,
			"halfcraft: log each time the dig mask moves to the player: laid out and uploaded again, or its layout alone (no cell dug in it before or after)");
		ConVar hc_debug_dig_clip("hc_debug_dig_clip", "0", 0,
			"halfcraft: for a hole drawn uncut: 1 - every pixel the shader dll's world shaders draw reads the dig mask's first texel, made dug (where the "
			"pixel is doesn't matter); 2 - every such pixel is cut without the mask read; 0 - the mask laid out again as it is");

		// r_novis and r_portalsopenall as they were before force_vis turned them on
		bool g_vis_forced = false;
		int  g_old_novis = 0;
		int  g_old_portals = 0;

		bool g_holes_near = false;  // dig_holes_near's, as dig_update last found it

		// the mask's texture may not hold its image any more (a level load, the material system's restore): the next frame
		// with a cell dug in the mask uploads it whole before the shader takes it (mask_needs_upload_before_clip)
		std::atomic<bool> g_mask_resync{ false };
		bool              g_restore_func_added = false;  // restore_mask is the material system's

		using SectionKey = std::tuple<int, int, int>;  // a section, minecraft section coords
		using StoredSection = std::pair<DugSection, std::uint32_t>;  // a section's cells, with the version it last changed at

		struct DigStore
		{
			std::mutex lock;
			// every section a cell was dug in, with the version it last changed at (emptied ones stay, so server.dll
			// hears they're gone)
			std::map<SectionKey, StoredSection> sections;
			std::uint32_t                       version = 0;
			MapSlot                             slot;
			DugPending<SectionKey>              pending;  // what kRenClearAll left as it was
		};

		constexpr double GRACE_SECONDS = DugPending<SectionKey>::GRACE_SECONDS;

		DigStore& store()
		{
			static DigStore instance;
			return instance;
		}

		MapSlot current_slot()
		{
			auto&                       d = store();
			std::lock_guard<std::mutex> lock(d.lock);
			return d.slot;
		}

		/// the dug cells around the player as the shader dll samples them (core/hc_dig_mask.h; main thread)
		struct DigMask
		{
			McTexture                 texture{ "dig_mask" };
			std::vector<std::uint8_t> rgba;  // the texture's image
			DigMaskLayout             layout;
			bool                      built = false;
			bool                      dug = false;       // the image may have a texel dug (mask_needs_rebuild's image_dug)
			bool                      clipping = false;  // the shader dll has the texture: a cell is dug in the mask
			int                       debug_clip = 0;    // hc_debug_dig_clip, as the mask last had it
			std::uint32_t             version = 0;       // the store's, as the mask has it
			DigMaskState              state{};
			std::vector<DugSection>   sections;  // dug_since's, kept for the next call
		};

		DigMask& mask()
		{
			static DigMask instance;
			return instance;
		}

		/// one section's mask into the store (its lock held). the one it has already changes nothing (no new version):
		/// kRenDug comes with every kRenSection.
		/// @param bits - nullptr: nothing dug there
		void replace_section(DigStore& d, const SectionKey& key, std::uint32_t count, const std::uint8_t* bits)
		{
			const auto it = d.sections.find(key);
			if (!bits) {
				if (it == d.sections.end() || it->second.first.count == 0) {
					return;  // nothing was dug there, and nothing is
				}
			} else if (it != d.sections.end() && it->second.first.count == count && std::memcmp(it->second.first.bits, bits, sizeof(it->second.first.bits)) == 0) {
				return;
			}
			StoredSection& entry = it != d.sections.end() ? it->second : d.sections[key];
			DugSection&    section = entry.first;
			section.sx = std::get<0>(key);
			section.sy = std::get<1>(key);
			section.sz = std::get<2>(key);
			section.count = bits ? count : 0;
			if (bits) {
				std::memcpy(section.bits, bits, sizeof(section.bits));
			} else {
				std::memset(section.bits, 0, sizeof(section.bits));
			}
			entry.second = ++d.version;
		}

		/// one section's mask (from minecraft, or hc_debug_dig_cell). a section kRenClearAll left pending isn't any more,
		/// the mask it had or not.
		/// @param bits - nullptr: nothing dug there
		void set_dug(std::int32_t sx, std::int32_t sy, std::int32_t sz, std::uint32_t count, const std::uint8_t* bits)
		{
			bool all_again = false;  // the last pending section came again
			{
				auto&                       d = store();
				std::lock_guard<std::mutex> lock(d.lock);
				const SectionKey            key{ sx, sy, sz };
				all_again = d.pending.settle(key) && d.pending.size() == 0;
				replace_section(d, key, count, bits);
			}
			if (all_again) {
				log_info("dig: minecraft sent every section with cells dug again");
			}
		}

		/// a section emptied (the store's lock held), at a new version: server.dll hears it's gone
		void empty_section(DigStore& d, StoredSection& entry)
		{
			entry.first.count = 0;
			std::memset(entry.first.bits, 0, sizeof(entry.first.bits));
			entry.second = ++d.version;
		}

		/// empties the sections with cells dug that pick(key) picks
		template <class Pick>
		void empty_sections(Pick&& pick)
		{
			auto&                       d = store();
			std::lock_guard<std::mutex> lock(d.lock);
			for (auto& entry : d.sections) {
				if (entry.second.first.count > 0 && pick(entry.first)) {
					empty_section(d, entry.second);
				}
			}
		}

		/// empties a pending section (the store's lock held), unless something emptied it already
		void empty_pending(DigStore& d, const SectionKey& key)
		{
			const auto it = d.sections.find(key);
			if (it != d.sections.end() && it->second.first.count > 0) {
				empty_section(d, it->second);
			}
		}

		/// kRenClearAll at now: minecraft sends every section again, the dug ones first (WorldExporter.resendEverything).
		/// the sections with cells dug stay as they are until their kRenDug comes, GRACE_SECONDS at most (DugPending)
		void keep_until_sent_again(double now)
		{
			std::size_t kept = 0;
			{
				auto&                       d = store();
				std::lock_guard<std::mutex> lock(d.lock);
				d.pending.start(now);
				for (const auto& entry : d.sections) {
					if (entry.second.first.count > 0) {
						d.pending.add(entry.first);
					}
				}
				kept = d.pending.size();
			}
			if (kept > 0) {
				log_info("dig: minecraft sends everything again: %zu section(s) with cells dug stay as they are until it sends them, %.0f s at most", kept, GRACE_SECONDS);
			}
		}

		/// the sections kRenClearAll left pending that minecraft hasn't sent again within the grace: emptied now
		void expire_pending(double now)
		{
			std::size_t emptied = 0;
			{
				auto&                       d = store();
				std::lock_guard<std::mutex> lock(d.lock);
				emptied = d.pending.expire(now, [&d](const SectionKey& key) { empty_pending(d, key); });
			}
			if (emptied > 0) {
				log_info("dig: %zu section(s) with cells dug not sent again within %.0f s of minecraft sending everything again: emptied", emptied, GRACE_SECONDS);
			}
		}

		/// whether a minecraft block's cell is dug (the store's lock held)
		bool cell_dug(const DigStore& d, int x, int y, int z)
		{
			const auto it = d.sections.find(SectionKey{ dug_detail::section_of(x), dug_detail::section_of(y), dug_detail::section_of(z) });
			if (it == d.sections.end() || it->second.first.count == 0) {
				return false;
			}
			const int bit = dug_detail::bit_of(x, y, z);
			return ((it->second.first.bits[bit >> 3] >> (bit & 7)) & 1) != 0;
		}

		/// whether a cell is dug in a section the dig mask's layout has (the ones it cuts out; dig_set_slot has emptied
		/// the sections of other maps' slots)
		bool any_dug_in(const DigMaskLayout& layout)
		{
			auto&                       d = store();
			std::lock_guard<std::mutex> lock(d.lock);
			return std::any_of(d.sections.begin(), d.sections.end(), [&layout](const auto& entry) {
				const DugSection& section = entry.second.first;
				return section.count > 0 && layout.has_section(section.sx, section.sy, section.sz);
			});
		}

		/// sets a cvar to value unless it has it already (a missing one is left alone)
		void set_cvar(ConVarRef& ref, int value)
		{
			if (ref.IsValid() && ref.GetInt() != value) {
				ref.SetValue(value);
			}
		}

		/// want: r_novis 1 and r_portalsopenall 1, so the rooms behind the holes near the player are drawn (the map's
		/// vis hides them behind its walls); otherwise their values from before come back. both are cheats:
		/// ConVar::SetValue doesn't check FCVAR_CHEAT (the flag stays on them), but the engine puts cheats back to their
		/// defaults when sv_cheats changes, so they're set again every frame.
		void force_vis(bool want)
		{
			if (!want && !g_vis_forced) {
				return;
			}
			static ConVarRef novis("r_novis"), portals("r_portalsopenall");
			if (want) {
				if (!g_vis_forced) {
					g_old_novis = novis.IsValid() ? novis.GetInt() : 0;
					g_old_portals = portals.IsValid() ? portals.GetInt() : 0;
					g_vis_forced = true;
					log_info("dig: this map has holes: r_novis 1, r_portalsopenall 1 (they were %d, %d)", g_old_novis, g_old_portals);
				}
				set_cvar(novis, 1);
				set_cvar(portals, 1);
			} else {
				set_cvar(novis, g_old_novis);
				set_cvar(portals, g_old_portals);
				g_vis_forced = false;
				log_info("dig: no holes here: r_novis %d, r_portalsopenall %d again", g_old_novis, g_old_portals);
			}
		}

		/// hc_debug_dig_cell: one cell of its section's mask dug or filled, as if minecraft had sent it so
		void debug_dig_cell(const int cell[3], bool dig)
		{
			const int    sx = dug_detail::section_of(cell[0]), sy = dug_detail::section_of(cell[1]), sz = dug_detail::section_of(cell[2]);
			std::uint8_t bits[sizeof(DugSection::bits)] = {};
			{
				auto&                       d = store();
				std::lock_guard<std::mutex> lock(d.lock);
				const auto                  it = d.sections.find(SectionKey{ sx, sy, sz });
				if (it != d.sections.end()) {
					std::memcpy(bits, it->second.first.bits, sizeof(bits));
				}
			}
			const int          bit = dug_detail::bit_of(cell[0], cell[1], cell[2]);
			const std::uint8_t mask = static_cast<std::uint8_t>(1u << (bit & 7));
			bits[bit >> 3] = static_cast<std::uint8_t>(dig ? bits[bit >> 3] | mask : bits[bit >> 3] & ~mask);
			std::uint32_t count = 0;
			for (const std::uint8_t byte : bits) {
				count += static_cast<std::uint32_t>(std::bitset<8>(byte).count());
			}
			set_dug(sx, sy, sz, count, count > 0 ? bits : nullptr);
			log_info("hc_debug_dig_cell: cell %d %d %d %s", cell[0], cell[1], cell[2], dig ? "dug" : "filled");
		}

		/// hc_debug_dig_cell: the cell of the wall under the crosshair, the one its face there belongs to
		/// @return false when there's no wall within reach
		bool crosshair_cell(int cell[3])
		{
			// the world's brushes alone: a player clip in front of a wall would stop the ray before it
			CTraceFilterWorldOnly filter;
			trace_t               tr;
			UTIL_TraceLine(MainViewOrigin(), MainViewOrigin() + MainViewForward() * CROSSHAIR_REACH, MASK_SOLID_BRUSHONLY, &filter, &tr);
			if (tr.fraction == 1.0f) {
				return false;
			}
			const Vector inside = tr.endpos - tr.plane.normal * INTO_WALL;
			const McVec  mc = source_to_mc(inside.Base(), current_slot());
			cell[0] = static_cast<int>(std::floor(mc.x));
			cell[1] = static_cast<int>(std::floor(mc.y));
			cell[2] = static_cast<int>(std::floor(mc.z));
			return true;
		}

		/// hc_debug_dig_mask_dump's: the mask's texture copied into the back buffer and read back, against its image
		struct MaskReadback
		{
			std::string               name;
			ITexture*                 texture = nullptr;
			int                       band = 0;  // rows copied and read at a time: as many as the back buffer holds
			std::vector<std::uint8_t> image;   // the mask's rgba image in the frame read back
			std::vector<std::uint8_t> pixels;  // rgb888, the texture as the gpu has it
			DigMaskLayout             layout;
			std::atomic<bool>         done{ false };
		};

		// asked for (wanted: the next finished frame takes it), then read back where the material system draws
		std::unique_ptr<MaskReadback> g_readback;
		bool                          g_readback_wanted = false;
		double                        g_readback_since = 0.0;  // when it was asked for, or queued for the read
		// given up on, but the render thread may still write them: freed once it has
		std::vector<std::unique_ptr<MaskReadback>> g_abandoned_readbacks;

		/// runs where the material system draws: on the main thread, or in order on its render thread when it queues. the
		/// texture goes into the back buffer's corner a band of rows at a time, over the finished frame: it's presented so
		void read_back_mask(MaskReadback* readback)
		{
			CMatRenderContextPtr context(materials);
			for (int top = 0; top < DigMaskLayout::TEXTURE; top += readback->band) {
				const int rows = std::min(readback->band, DigMaskLayout::TEXTURE - top);
				Rect_t    from{ 0, top, DigMaskLayout::TEXTURE, rows };
				Rect_t    to{ 0, 0, DigMaskLayout::TEXTURE, rows };
				context->CopyTextureToRenderTargetEx(0, readback->texture, &from, &to);
				context->ReadPixels(0, 0, DigMaskLayout::TEXTURE, rows, readback->pixels.data() + static_cast<std::size_t>(top) * DigMaskLayout::TEXTURE * 3,
					IMAGE_FORMAT_RGB888);
			}
			readback->done.store(true, std::memory_order_release);
		}

		/// whether a texel says its cell is dug (mask_texel_dug) in the mask's image (rgba) and in what the gpu gave back (rgb)
		bool image_texel_dug(const MaskReadback& r, std::size_t texel)
		{
			return mask_texel_dug(r.image[texel * TEXEL_BYTES], r.image[texel * TEXEL_BYTES + 2]);
		}

		bool texture_texel_dug(const MaskReadback& r, std::size_t texel)
		{
			return mask_texel_dug(r.pixels[texel * 3], r.pixels[texel * 3 + 2]);
		}

		/// a read-back mask compared with the image it should have, layer by layer (logged), and written to <name>.tga
		void finish_readback(const MaskReadback& r)
		{
			int differing_layers = 0;
			for (int k = 0; k < DigMaskLayout::SIZE_Y; ++k) {
				int u = 0, v = 0;
				r.layout.texel(r.layout.ox, r.layout.oy + k, r.layout.oz, u, v);  // the layer's corner
				int in_image = 0, in_texture = 0, differ = 0;
				for (int j = 0; j < DigMaskLayout::SIZE_XZ; ++j) {
					for (int i = 0; i < DigMaskLayout::SIZE_XZ; ++i) {
						const std::size_t texel = static_cast<std::size_t>(v + j) * DigMaskLayout::TEXTURE + u + i;
						const bool        image = image_texel_dug(r, texel), texture = texture_texel_dug(r, texel);
						in_image += image ? 1 : 0;
						in_texture += texture ? 1 : 0;
						differ += image != texture ? 1 : 0;
					}
				}
				if (in_image > 0 || in_texture > 0) {
					log_info("hc_debug_dig_mask_dump: layer %d (block y %d): %d cell(s) dug in the image, %d in the texture, %d differ", k, r.layout.oy + k, in_image,
						in_texture, differ);
				}
				differing_layers += differ > 0 ? 1 : 0;
			}
			// an uncompressed tga, top row first: the texture as the gpu has it
			CUtlBuffer         tga;
			const std::uint8_t header[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 4, 24, 0x20 };
			tga.Put(header, sizeof(header));
			for (std::size_t texel = 0; texel < static_cast<std::size_t>(DigMaskLayout::TEXTURE) * DigMaskLayout::TEXTURE; ++texel) {
				const std::uint8_t bgr[3] = { r.pixels[texel * 3 + 2], r.pixels[texel * 3 + 1], r.pixels[texel * 3] };
				tga.Put(bgr, sizeof(bgr));
			}
			const std::string file = r.name + ".tga";
			const bool        written = g_pFullFileSystem->WriteFile(file.c_str(), "MOD", tga);
			log_info("hc_debug_dig_mask_dump: mask from blocks %d %d %d: the texture %s the image (%d layer(s) differ); the texture as the gpu has it %s %s",
				r.layout.ox, r.layout.oy, r.layout.oz, differing_layers > 0 ? "differs from" : "matches", differing_layers, written ? "is in" : "didn't go to",
				file.c_str());
		}

		/// hc_debug_dig_mask_dump: the next finished frame with the hud reads the mask's texture back (dig_post_render_vgui),
		/// and a later dig_update compares it with the image the mask had in that frame
		void dump_mask(const char* name)
		{
			const auto& m = mask();
			if (!m.built || !m.texture.texture()) {
				log_info("hc_debug_dig_mask_dump: no dig mask yet");
				return;
			}
			if (g_readback) {
				log_info("hc_debug_dig_mask_dump: still reading the last one back");
				return;
			}
			g_readback = std::make_unique<MaskReadback>();
			g_readback->name = name;
			g_readback->texture = m.texture.texture();
			g_readback_wanted = true;
			g_readback_since = Plat_FloatTime();
		}

		/// main thread, each frame: a readback that came back is compared and written; one that no frame took, or that the
		/// render thread didn't finish, within READBACK_TIMEOUT_SECONDS is given up on
		void readback_update()
		{
			g_abandoned_readbacks.erase(std::remove_if(g_abandoned_readbacks.begin(), g_abandoned_readbacks.end(),
											[](const std::unique_ptr<MaskReadback>& readback) { return readback->done.load(std::memory_order_acquire); }),
				g_abandoned_readbacks.end());
			if (!g_readback) {
				return;
			}
			if (!g_readback_wanted && g_readback->done.load(std::memory_order_acquire)) {
				finish_readback(*g_readback);
				g_readback.reset();
			} else if (Plat_FloatTime() - g_readback_since > READBACK_TIMEOUT_SECONDS) {
				log_info("hc_debug_dig_mask_dump: %s within %.0f s: given up", g_readback_wanted ? "no frame with the hud came" : "the render thread didn't read it back",
					READBACK_TIMEOUT_SECONDS);
				if (!g_readback_wanted) {
					g_abandoned_readbacks.push_back(std::move(g_readback));
				}
				g_readback.reset();
				g_readback_wanted = false;
			}
		}

		/// the sections dug_since gives, into m.sections
		/// @return how many it copied
		int sections_since(DigMask& m, std::uint32_t since, std::uint32_t* now)
		{
			const int changed = dug_since(since, nullptr, 0, now);
			if (changed <= 0) {
				return 0;
			}
			m.sections.resize(static_cast<std::size_t>(changed));
			return std::min(changed, dug_since(since, m.sections.data(), changed, now));
		}

		/// MaterialBufferRestoreFunc_t: the material system has its resources back after a device reset or a mode change
		void restore_mask(int /*change_flags*/)
		{
			g_mask_resync.store(true, std::memory_order_release);
		}

		/// the mask's image laid out afresh in its layout, with every section that has cells dug, and the whole texture
		/// uploaded (made the first time: only ever once, at one size, since the material system's thread may draw with it)
		void rebuild_mask(DigMask& m)
		{
			if (!g_restore_func_added) {
				materials->AddRestoreFunc(restore_mask);
				g_restore_func_added = true;
			}
			m.rgba.assign(static_cast<std::size_t>(DigMaskLayout::TEXTURE) * DigMaskLayout::TEXTURE * TEXEL_BYTES, 0);
			m.dug = false;
			const int all = sections_since(m, 0, nullptr);
			for (int i = 0; i < all; ++i) {
				const DugSection& section = m.sections[i];
				const bool        dug = section.count > 0;
				if (write_section(m.layout, section.sx, section.sy, section.sz, dug ? section.bits : nullptr, m.rgba.data()) && dug) {
					m.dug = true;
				}
			}
			m.texture.set(m.rgba.data(), DigMaskLayout::TEXTURE, DigMaskLayout::TEXTURE);
			m.built = true;
		}

		/// the mask placed around the player: a new layout, and the image laid out afresh unless it's all zeros and stays so
		/// (no hole near the player before or after: the same zeros, nothing to clear or upload)
		/// @return whether the image was laid out and uploaded whole
		bool recentre_mask(DigMask& m, const McVec& player)
		{
			m.layout = DigMaskLayout::around(player.x, player.y, player.z);
			const bool rebuild = mask_needs_rebuild(m.built, m.dug, any_dug_in(m.layout));
			if (hc_debug_dig_mask.GetBool()) {
				log_info("dig: mask around %d %d %d (blocks from %d %d %d): %s", m.layout.ox + DigMaskLayout::CENTRE_XZ, m.layout.oy + DigMaskLayout::CENTRE_Y,
					m.layout.oz + DigMaskLayout::CENTRE_XZ, m.layout.ox, m.layout.oy, m.layout.oz,
					rebuild ? "laid out and uploaded again" : "no cell dug in it before or now, the layout alone moves");
			}
			if (rebuild) {
				rebuild_mask(m);
			}
			return rebuild;
		}

		/// one changed section into the mask and its texture: its 16 layers, a square of 16 x 16 texels each
		void update_mask(DigMask& m, const DugSection& section)
		{
			if (!write_section(m.layout, section.sx, section.sy, section.sz, section.count > 0 ? section.bits : nullptr, m.rgba.data())) {
				return;  // outside the mask: not cut out until the player comes near
			}
			if (section.count > 0) {
				m.dug = true;  // a section emptied leaves it as it is: another may still have cells dug in the image
			}
			// update_region takes the square's own rows, not the image's
			std::uint8_t block[SECTION_BLOCKS * SECTION_BLOCKS * TEXEL_BYTES];
			for (int y = 0; y < SECTION_BLOCKS; ++y) {
				int u = 0, v = 0;
				m.layout.texel(section.sx * SECTION_BLOCKS, section.sy * SECTION_BLOCKS + y, section.sz * SECTION_BLOCKS, u, v);
				for (int row = 0; row < SECTION_BLOCKS; ++row) {
					const std::uint8_t* from = m.rgba.data() + (static_cast<std::size_t>(v + row) * DigMaskLayout::TEXTURE + u) * TEXEL_BYTES;
					std::memcpy(block + row * SECTION_BLOCKS * TEXEL_BYTES, from, SECTION_BLOCKS * TEXEL_BYTES);
				}
				m.texture.update_region(u, v, SECTION_BLOCKS, SECTION_BLOCKS, block);
			}
		}

		/// hc_debug_dig_clip: the shader dll clips by the mask whatever is dug, every pixel at the mask's first cell (c20.w 0):
		/// 1 with that texel dug, 2 without reading the mask (c21.w 2). a hole drawn uncut in a view: still uncut at 1, the
		/// world position the vertex shader gives isn't what the pixel shader gets; cut at 2 alone, s11 doesn't hold the
		/// mask; uncut at 2, the constants don't reach the pixel shader or another one draws there
		void debug_clip(DigMask& m)
		{
			const int mode = hc_debug_dig_clip.GetInt();
			if (mode != m.debug_clip) {
				if (m.debug_clip == 1) {
					rebuild_mask(m);  // the first texel back as it is
				}
				m.debug_clip = mode;
			}
			if (mode != 1 && mode != 2) {
				return;
			}
			if (mode == 1 && !mask_texel_dug(m.rgba[0], m.rgba[2])) {
				const std::uint8_t dug[TEXEL_BYTES] = { 255, 0, 0, 0 };  // write_section's
				std::memcpy(m.rgba.data(), dug, sizeof(dug));
				m.texture.update_region(0, 0, 1, 1, dug);
			}
			m.state.texture = m.texture.texture();
			m.state.origin[3] = 0.0f;  // every pixel in cell (0, 0, 0): texel (0, 0)
			m.state.layout[3] = static_cast<float>(mode);
		}

		/// the mask follows the store; without the shader dll it's all zeros. the shader dll clips by it only while a cell
		/// is dug in it, whatever hc_dig says: with none, it gets no texture and clips nothing, whatever the gpu holds of
		/// it (on d2_coast_01, with no cell dug on the map, cliffs were cut in 40-unit layers: the texture sampled had
		/// whole layers red that client.dll never wrote). when a cell comes into it, and after a level load or the
		/// material system's restore, its image goes up whole before the shader dll takes it (mask_needs_upload_before_clip).
		/// @return whether a cell is dug in the mask
		bool mask_update(const McVec& player)
		{
			auto& m = mask();
			if (!dig_shader_ready()) {
				m.state = {};
				m.clipping = false;
				return false;
			}
			if (g_mask_resync.exchange(false, std::memory_order_acq_rel)) {
				m.clipping = false;
				if (hc_debug_dig_mask.GetBool()) {
					log_info("dig: a level load or the material system's restore: the mask goes up whole before the shader next clips by it");
				}
			}
			std::uint32_t now = m.version;
			const int     changed = sections_since(m, m.version, &now);
			m.version = now;
			bool recentred = false, whole = false;  // the layout moved; the image went up whole this frame
			if (!m.built || m.layout.needs_recentre(player.x, player.y, player.z)) {
				recentred = true;
				whole = recentre_mask(m, player);  // the sections that changed are in it, or nothing dug is
			}
			const bool dug = any_dug_in(m.layout);
			if (mask_needs_upload_before_clip(dug, m.clipping, whole)) {
				rebuild_mask(m);  // the sections that changed with the rest
			} else if (!recentred) {
				for (int i = 0; i < changed; ++i) {
					update_mask(m, m.sections[i]);
				}
			}
			m.clipping = dug;
			m.state.texture = dug ? m.texture.texture() : nullptr;
			m.layout.shader_constants(current_slot(), m.state.origin, m.state.layout);
			debug_clip(m);
			return dug;
		}
	}

	void dig_on_message(std::uint32_t type, const std::uint8_t* payload, std::uint32_t bytes)
	{
		if (type == proto::kRenDug) {
			if (bytes < sizeof(proto::RenDug)) {
				return;
			}
			proto::RenDug header;
			std::memcpy(&header, payload, sizeof(header));
			const bool has_bits = header.count > 0 && sizeof(header) + sizeof(DugSection::bits) <= bytes;
			set_dug(header.sx, header.sy, header.sz, has_bits ? header.count : 0, has_bits ? payload + sizeof(header) : nullptr);
		} else if (type == proto::kRenClearAll) {
			// not emptied at once: minecraft sends the dug ones again first. Plat_FloatTime, not game time: the grace
			// waits on minecraft, which doesn't stop for a pause, a level load or host_timescale
			keep_until_sent_again(Plat_FloatTime());
		}
	}

	void dig_set_slot(MapSlot slot)
	{
		{
			auto&                       d = store();
			std::lock_guard<std::mutex> lock(d.lock);
			if (slot == d.slot) {
				return;
			}
			d.slot = slot;
		}
		// another map: what minecraft dug around the old one isn't anywhere near this one
		const float centre = static_cast<float>(slot.x_blocks());
		empty_sections([centre](const SectionKey& key) { return std::fabs(static_cast<float>(std::get<0>(key) * SECTION_BLOCKS) - centre) > KEEP_SLOT_BLOCKS; });
		// and the grace a kRenClearAll gave ends: it kept the holes around a player who has left. the ones still
		// pending in this map's slot are emptied too (minecraft sends them again as it loads their chunks), so none stays
		// dug that minecraft may not have any more
		auto&                       d = store();
		std::lock_guard<std::mutex> lock(d.lock);
		d.pending.end([&d](const SectionKey& key) { empty_pending(d, key); });
	}

	bool dig_shader_ready()
	{
		static std::atomic<int> logged{ -1 };  // what the log said last: 1 loaded, 0 not, -1 nothing yet
		const bool              ready = module_loaded(HC_SHADER_DLL);
		if (logged.exchange(ready ? 1 : 0) != (ready ? 1 : 0)) {
			if (ready) {
				log_info("dig: %s is loaded", HC_SHADER_DLL);
			} else {
				log_info("dig: no %s in this engine: digging is off", HC_SHADER_DLL);
			}
		}
		return ready;
	}

	bool dig_enabled()
	{
		const bool ready = dig_shader_ready();  // first: its line is logged whatever hc_dig says
		return ready && hc_dig.GetBool();
	}

	namespace
	{
		// the map's detail props (grass, bushes) by the minecraft section the cell over their origin is in: the ones on a
		// dug cell go out of sight (detailobjectsystem.cpp). looked at only in the sections whose dug cells changed
		struct DetailProp
		{
			int  index;
			int  x, y, z;  // the cell, minecraft coords
			bool hidden;
		};
		std::map<std::tuple<int, int, int>, std::vector<DetailProp>> g_details;
		int                                                          g_details_count = -1;  // HalfCraft_DetailCount when g_details was laid out (-1: never)
		std::uint32_t                                                g_details_version = 0;  // the store's version the sections were looked at for
		bool                                                         g_details_relaid = true;  // a level loaded: laid out afresh

		int floor_div16(int v)
		{
			return v >= 0 ? v / 16 : -((15 - v) / 16);
		}

		/// the props of one section out of sight on its dug cells, back on the others
		/// @return how many it hid
		int look_at_section(const DugSection& section)
		{
			const auto found = g_details.find({ section.sx, section.sy, section.sz });
			if (found == g_details.end()) {
				return 0;
			}
			int hidden = 0;
			for (DetailProp& prop : found->second) {
				const int  lx = prop.x - section.sx * 16, ly = prop.y - section.sy * 16, lz = prop.z - section.sz * 16;
				const int  bit = lx + 16 * lz + 256 * ly;
				const bool dug = section.count > 0 && (section.bits[bit >> 3] >> (bit & 7) & 1) != 0;
				if (dug != prop.hidden) {
					HalfCraft_DetailHide(prop.index, dug);
					prop.hidden = dug;
				}
				hidden += dug ? 1 : 0;
			}
			return hidden;
		}

		/// lays the level's detail props out by section (when there are new ones), then hides and shows them in the sections
		/// whose dug cells changed (every section with dug cells, after a lay-out)
		void hide_dug_details()
		{
			const int count = HalfCraft_DetailCount();
			if (count != g_details_count || g_details_relaid) {
				MapSlot slot;
				{
					auto&                       d = store();
					std::lock_guard<std::mutex> lock(d.lock);
					slot = d.slot;
				}
				g_details.clear();
				for (int i = 0; i < count; ++i) {
					const Vector origin = HalfCraft_DetailOrigin(i);
					HalfCraft_DetailHide(i, false);
					const float over[3] = { origin.x, origin.y, origin.z + 1.0f };
					const McVec at = source_to_mc(over, slot);
					const int   x = static_cast<int>(std::floor(at.x)), y = static_cast<int>(std::floor(at.y)), z = static_cast<int>(std::floor(at.z));
					g_details[{ floor_div16(x), floor_div16(y), floor_div16(z) }].push_back({ i, x, y, z, false });
				}
				g_details_count = count;
				g_details_relaid = false;
				g_details_version = 0;
			}
			std::uint32_t now = 0;
			const int     changed = dug_since(g_details_version, nullptr, 0, &now);
			if (now == g_details_version || changed <= 0) {
				g_details_version = now;
				return;
			}
			std::vector<DugSection> sections(static_cast<std::size_t>(changed));
			const int               got = std::min(changed, dug_since(g_details_version, sections.data(), changed, &now));
			int                     hidden = 0;
			for (int i = 0; i < got; ++i) {
				hidden += look_at_section(sections[i]);
			}
			if (g_details_version == 0 && hidden > 0) {
				log_info("dig: %d of the map's %d detail props (grass, bushes) out of sight on dug cells", hidden, count);
			}
			g_details_version = now;
		}
	}

	void dig_update(const McVec& player, bool in_game)
	{
		expire_pending(Plat_FloatTime());  // after this frame's messages: a section they brought again stays
		readback_update();
		if (in_game) {
			hide_dug_details();
		}
		const bool dug = mask_update(player);
		// hc_dig 0 leaves the holes cut out (it only stops new ones): they're seen through all the same. the ones
		// outside the mask are drawn closed: nothing to see through there
		g_holes_near = in_game && dug;
		force_vis(g_holes_near);
	}

	bool dig_holes_near()
	{
		return g_holes_near;
	}

	void dig_level_loaded()
	{
		g_mask_resync.store(true, std::memory_order_release);
		g_details_relaid = true;  // the level's detail props are new: laid out and looked at again
	}

	void dig_post_render_vgui()
	{
		if (!g_readback_wanted) {
			return;
		}
		g_readback_wanted = false;
		int width = 0, height = 0;
		materials->GetBackBufferDimensions(width, height);
		if (width < DigMaskLayout::TEXTURE || height <= 0) {
			log_info("hc_debug_dig_mask_dump: the %dx%d back buffer is narrower than the mask's %d texels: run the game wider", width, height, DigMaskLayout::TEXTURE);
			g_readback.reset();
			return;
		}
		g_readback->band = std::min(height, DigMaskLayout::TEXTURE);
		g_readback->pixels.resize(static_cast<std::size_t>(DigMaskLayout::TEXTURE) * DigMaskLayout::TEXTURE * 3);
		// the frame's image: its uploads went in before the frame was drawn (dig_update)
		g_readback->image = mask().rgba;
		g_readback->layout = mask().layout;
		g_readback_since = Plat_FloatTime();
		CMatRenderContextPtr context(materials);
		if (ICallQueue* queue = context->GetCallQueue()) {
			queue->QueueCall(read_back_mask, g_readback.get());
		} else {
			read_back_mask(g_readback.get());
		}
	}

	void dig_shutdown()
	{
		// a readback the render thread may still write goes with the process
		g_abandoned_readbacks.push_back(std::move(g_readback));
		for (auto& readback : g_abandoned_readbacks) {
			if (readback && !readback->done.load(std::memory_order_acquire)) {
				static_cast<void>(readback.release());
			}
		}
		g_abandoned_readbacks.clear();
		g_readback_wanted = false;
		if (g_restore_func_added) {
			materials->RemoveRestoreFunc(restore_mask);
			g_restore_func_added = false;
		}
		force_vis(false);
		g_holes_near = false;
		auto& m = mask();
		m.state = {};  // first: HalfCraft_DigMask mustn't hand the shader dll the texture released next
		m.texture.release();
		m.built = false;
		m.clipping = false;
		auto&                       d = store();
		std::lock_guard<std::mutex> lock(d.lock);
		d.sections.clear();
		d.pending.drop();
	}

	const DigMaskState* dig_mask_state()
	{
		return &mask().state;
	}

	int dug_since(std::uint32_t since, DugSection* out, int max, std::uint32_t* now)
	{
		if (!dig_shader_ready()) {
			return 0;  // half-life draws its walls whole: they stay solid for minecraft too
		}
		auto&                       d = store();
		std::lock_guard<std::mutex> lock(d.lock);
		int                         changed = 0;
		for (const auto& entry : d.sections) {
			if (entry.second.second > since) {
				if (out && changed < max) {
					out[changed] = entry.second.first;
				}
				++changed;
			}
		}
		if (now) {
			*now = d.version;
		}
		return changed;
	}

	bool box_dug(const Vector& mins, const Vector& maxs)
	{
		if (!dig_shader_ready()) {
			return false;  // half-life draws its walls whole: they're there
		}
		auto&                       d = store();
		std::lock_guard<std::mutex> lock(d.lock);
		return source_box_dug(mins.Base(), maxs.Base(), d.slot, [&d](int x, int y, int z) { return cell_dug(d, x, y, z); });
	}
}

// server.dll's way to the cells minecraft dug (hc_bridge.h)
extern "C" HC_EXPORT int HalfCraft_DugSince(std::uint32_t since, halfcraft::DugSection* out, int max, std::uint32_t* now)
{
	return halfcraft::dug_since(since, out, max, now);
}

// the shader dll's way to the dig mask (hc_dig_abi.h, DigMaskFn)
extern "C" HC_EXPORT const halfcraft::DigMaskState* HalfCraft_DigMask()
{
	return halfcraft::dig_mask_state();
}

CON_COMMAND(hc_debug_dig_cell,
	"halfcraft: dig a cell out of half-life's walls without minecraft, or fill it again: hc_debug_dig_cell [<x> <y> <z> [1 dig | 0 fill]] (minecraft blocks; "
	"without them, the wall under the crosshair)")
{
	int  cell[3] = {};
	bool dig = true;
	if (args.ArgC() >= 4) {
		for (int k = 0; k < 3; ++k) {
			cell[k] = atoi(args[k + 1]);
		}
		dig = args.ArgC() < 5 || atoi(args[4]) != 0;
	} else if (!halfcraft::crosshair_cell(cell)) {
		halfcraft::log_info("hc_debug_dig_cell: no wall under the crosshair");
		return;
	}
	halfcraft::debug_dig_cell(cell, dig);
}

CON_COMMAND(hc_debug_dig_mask_dump,
	"halfcraft: log the dig mask's layers with cells dug and write its texture as the gpu has it to a tga (the layers in tiles of 8 x 8, red where "
	"a cell is dug): hc_debug_dig_mask_dump [name] (dig_mask by default). the next frame with the hud shows the mask over its top left corner")
{
	halfcraft::dump_mask(args.ArgC() >= 2 ? args[1] : "dig_mask");
}
