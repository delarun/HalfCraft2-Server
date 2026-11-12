// server.dll: the halfcraft server's sandbox (core/hc_sandbox.h): with more than one slot the map's entities lose
// their story before they spawn. singleplayer plays the story as it always did.

#include "cbase.h"

#include "tier0/valve_minmax_off.h"

#include <string>

#include "core/hc_log.h"
#include "core/hc_sandbox.h"
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

		std::string g_entities;  // the lump the engine parses: alive until the next level's
	}

	const char* server_map_entities(const char* map, const char* entities)
	{
		if (entities == nullptr || gpGlobals->maxClients <= 1 || !hc_sandbox.GetBool()) {
			return entities;
		}
		sandbox::Report report;
		g_entities = sandbox::serialize(sandbox::apply(sandbox::parse(entities), report));
		log_info("sandbox: %s without its story: %d entities out, %d vehicle(s) unlocked, %d output(s) that lock a vehicle out", map ? map : "?",
			report.removed, report.unlocked, report.outputs_removed);
		return g_entities.c_str();
	}
}
