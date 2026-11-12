#pragma once

// the minecraft a halfcraft release ships: a portable prism launcher with a ready "HalfCraft"
// instance, in the release's minecraft folder beside the half-life game folders. HalfCraft.exe and
// client.dll both start it from there. engine-agnostic. on linux prism is Prism/prismlauncher, and
// a running minecraft is a lock on /dev/shm/<its link's mutex name> (hc_link_names.h's posix_name).
//
//   HalfCraft/
//     HalfCraft.exe
//     game-hl2/                   the game folder half-life 2's hl2.exe runs (-game), 32-bit
//     game-hl2dm/                 the one half-life 2: deathmatch's hl2mp_win64.exe runs, 64-bit
//     minecraft/
//       Prism/prismlauncher.exe   portable prism: the account, minecraft, java and the world live here
//       defaults/                 prism's settings for its first start

#include <filesystem>
#include <string>

#include "hc_link_names.h"

namespace halfcraft::prism
{
	/// the bundled minecraft beside a mod folder.
	/// @param game_dir - the game folder (<release>/game-hl2 or game-hl2dm)
	/// @return <release>/minecraft, or empty when there's no bundled prism (a development checkout)
	std::filesystem::path find_bundle(const std::filesystem::path& game_dir);

	/// a minecraft with the halfcraft mod is running on this link (it holds the link's minecraft_mutex for as
	/// long as it runs).
	bool minecraft_running(const LinkNames& link);

	/// the bundle's own prism is running: downloading, asking for an account, or minding the game.
	bool launcher_running(const std::filesystem::path& bundle);

	/// prism has an account to play with.
	bool signed_in(const std::filesystem::path& bundle);

	/// starts the bundle's "HalfCraft" instance through prism, which signs in and downloads minecraft
	/// and java itself the first time. that first start also puts prism's default settings in place.
	/// prism gives the minecraft it starts its own environment (all but a few java variables), so
	/// HALFCRAFT_LINK, as this process has it, says which link that minecraft takes. a prism of the same
	/// bundle already running takes the launch over instead, and with it its own environment's link.
	/// @param error - prism's exe and why it couldn't start it, when it couldn't (the callers say what failed)
	/// @return false when prism couldn't be started
	bool start(const std::filesystem::path& bundle, std::wstring& error);
}
