#pragma once

// co-op hosting: the console commands client.dll's hc_host runs to host co-op for two. a two-slot listen server
// on a map, with coop 0: coop 1 breaks every map change after it (the engine then starts no new map), and with two
// slots the engine sets deathmatch 1 itself, which is what makes a dead player respawn instead of the map reload.
// sv_lan 1 keeps the server to the lan; a vpn (radmin, hamachi) needs sv_lan 0. pure: no windows, no sdk.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace halfcraft
{
	// the longest map name hc_host takes: well within the engine's buffers (the sdk's MAX_MAP_NAME, 96, holds
	// maps/<name>.bsp), and far longer than any of the game's maps
	inline constexpr std::size_t MAX_HOSTED_MAP_NAME = 64;

	// the players co-op hosts: maxplayers in host_commands
	inline constexpr int HOST_SLOTS = 2;

	/// one of A-Z, a-z, 0-9 and _: what hc_host takes a map name to be made of.
	inline bool map_name_character(char c)
	{
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
	}

	/// a map name hc_host takes: 1 to MAX_HOSTED_MAP_NAME characters, each a map_name_character. the name goes into
	/// the console as it is, so nothing else may: a ; or a line break would run a command after it.
	inline bool is_map_name(std::string_view name)
	{
		return !name.empty() && name.size() <= MAX_HOSTED_MAP_NAME && std::all_of(name.begin(), name.end(), map_name_character);
	}

	/// the console commands that host co-op for two on a map: drop the game there is, take two slots, keep to the lan
	/// (or not, for a vpn), no coop, and load the map. the engine refuses maxplayers while a server runs ("cannot
	/// change maxplayers while the server is running"), and disconnect only asks for the game's shutdown, which the
	/// engine does at the start of a later frame. the wait holds the rest of the line for one pass of the console's
	/// buffer, which is often that frame but not always (the buffer runs more than once a frame), so with a server up
	/// client.dll's hc_host runs this only once the server has stopped; at the menu the wait costs nothing.
	/// @param map - the map's name, without maps/ and .bsp
	/// @param vpn - guests come over a vpn: sv_lan 0
	/// @return empty when map isn't a map name
	inline std::string host_commands(std::string_view map, bool vpn)
	{
		if (!is_map_name(map)) {
			return {};
		}
		return "disconnect; wait; maxplayers " + std::to_string(HOST_SLOTS) + "; sv_lan " + (vpn ? "0" : "1") + "; coop 0; map " + std::string(map);
	}

	/// what hc_host is asked for.
	struct HostRequest
	{
		std::string map;  // a map name (is_map_name)
		bool        vpn = false;
	};

	/// the word vpn, case aside.
	inline bool is_vpn_word(std::string_view word)
	{
		constexpr std::string_view VPN = "vpn";
		return word.size() == VPN.size() && std::equal(word.begin(), word.end(), VPN.begin(), [](char a, char b) {
			return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
		});
	}

	/// hc_host's arguments: hc_host [map] [vpn]. a lone vpn is the flag, on the current map; vpn is the only second
	/// argument there is, so a typo of it never quietly hosts on the lan (or over a vpn).
	/// @param args - the arguments after hc_host
	/// @param current_map - the map this game is on, without maps/ and .bsp; empty when there's none (the menu,
	///                      whose background is a map too)
	/// @return nullopt for the usage line: an argument too many, a second one that isn't vpn, no map given at the
	///         menu, or a name (given or the current one) that isn't a map name
	inline std::optional<HostRequest> parse_host_args(const std::vector<std::string_view>& args, std::string_view current_map)
	{
		HostRequest request;
		std::size_t next = 0;
		const bool  named = next < args.size() && !is_vpn_word(args[next]);
		if (named) {
			request.map = std::string(args[next++]);
		}
		if (next < args.size() && is_vpn_word(args[next])) {
			request.vpn = true;
			++next;
		}
		if (next != args.size()) {
			return std::nullopt;
		}
		if (!named) {
			request.map = std::string(current_map);
		}
		if (!is_map_name(request.map)) {
			return std::nullopt;
		}
		return request;
	}

	/// a map's file, as the game's file system has it: maps/<map>.bsp.
	inline std::string map_file(std::string_view map)
	{
		return "maps/" + std::string(map) + ".bsp";
	}

	/// a level hc_host started has room for co-op: HOST_SLOTS or more. fewer means the engine kept the old maxplayers.
	/// @param max_clients - the level's player slots (the engine's maxClients)
	inline bool hosted_level_has_room(int max_clients)
	{
		return max_clients >= HOST_SLOTS;
	}

	/// an ipv4 address with its port, as the engine names a net channel's server ("192.168.10.254:27015"), as
	/// HostState's coopIp holds it: a << 24 | b << 16 | c << 8 | d. nullopt for anything else (a name, ipv6, loopback's
	/// "loopback").
	inline std::optional<std::uint32_t> ipv4_of(std::string_view address)
	{
		std::uint32_t ip = 0;
		std::size_t   at = 0;
		for (int part = 0; part < 4; ++part) {
			if (part > 0) {
				if (at >= address.size() || address[at] != '.') {
					return std::nullopt;
				}
				++at;
			}
			std::uint32_t value = 0;
			std::size_t   digits = 0;
			while (at < address.size() && address[at] >= '0' && address[at] <= '9' && digits < 3) {
				value = value * 10 + static_cast<std::uint32_t>(address[at++] - '0');
				++digits;
			}
			if (digits == 0 || value > 255) {
				return std::nullopt;
			}
			ip = ip << 8 | value;
		}
		if (at != address.size() && address[at] != ':') {
			return std::nullopt;
		}
		return ip;
	}
}
