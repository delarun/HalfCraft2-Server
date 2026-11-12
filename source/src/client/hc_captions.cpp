// client.dll: half-life's close captions go to minecraft while its hud is up, which writes speech
// into its chat under its speaker's name and sounds into its subtitles (#26). the speaker comes from
// server.dll by the caption's token (HalfCraft_CaptionSpeaker). minecraft's show subtitles option
// turns captions on and off: it sets closecaption (speech and sounds both, cc_subtitles 0) whenever
// it changes.

#include "cbase.h"

#include "tier0/valve_minmax_off.h"
#include <mutex>
#include <string>
#include <unordered_map>

#include "client/hc_client.h"
#include "core/hc_log.h"
#include "core/hc_module.h"
#include "shared/hc_hooks.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

namespace halfcraft
{
	namespace
	{
		constexpr int UNSET = -1;
		int           g_applied = UNSET;  // minecraft's option as last put into closecaption

		// who said each caption token last ("<classname>/<model>"), as server.dll sent it
		std::mutex                                   g_speakers_lock;
		std::unordered_map<std::string, std::string> g_speakers;

		std::string speaker_of(const char* token)
		{
			std::lock_guard<std::mutex> lock(g_speakers_lock);
			const auto                  found = token ? g_speakers.find(token) : g_speakers.end();
			return found == g_speakers.end() ? "-" : found->second;
		}
	}

	void captions_follow_minecraft(const ClientSession& s)
	{
		if (!s.have_mc) {
			return;
		}
		const int want = (s.mc.flags & proto::kMcSubtitles) ? 1 : 0;
		if (want == g_applied) {
			return;
		}
		g_applied = want;
		ConVarRef closecaption("closecaption");
		ConVarRef subtitles("cc_subtitles");
		if (closecaption.IsValid() && subtitles.IsValid()) {
			closecaption.SetValue(want);
			subtitles.SetValue(0);
			log_info("captions %s (minecraft's show subtitles)", want ? "on" : "off");
		}
	}

	bool client_caption(const wchar_t* stream, const char* token, bool from_player)
	{
		auto& s = client_session();
		if (!s.link_ready || !s.link.mc_alive() || !s.minecraft_hud) {
			return false;
		}
		char utf8[4096];
		Q_UnicodeToUTF8(stream, utf8, sizeof(utf8));
		if (!s.link.push_string(proto::kStrCaption, std::string(from_player ? "1 " : "0 ") + speaker_of(token) + " " + utf8)) {
			log_warning("captions: the link is full; minecraft misses a caption");
		}
		return true;
	}
}

// server.dll: who says the caption it sends the client now (hc_bridge.h)
extern "C" HC_EXPORT void HalfCraft_CaptionSpeaker(const char* token, const char* speaker)
{
	std::lock_guard<std::mutex> lock(halfcraft::g_speakers_lock);
	halfcraft::g_speakers[token] = speaker;
}
