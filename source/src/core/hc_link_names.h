#pragma once

// the names of a link's shared objects (protocol/schema/header.toml's kMappingName): the mapping, the host's frame
// event and the mutex minecraft holds while it runs. a link id gives a pair (a source game and its minecraft) a link
// of its own, so two pairs can run on one pc:
//
//   no id    Local\HalfCraft_v1      Local\HalfCraft_v1_frame      Local\HalfCraft_v1_minecraft
//   id b     Local\HalfCraft_v1.b    Local\HalfCraft_v1.b_frame    Local\HalfCraft_v1.b_minecraft
//
// the id is HALFCRAFT_LINK's on both sides; source's -hc_link wins over it (choose_link_id). an id given that isn't
// one gives no names at all: the default link's would be another pair's. minecraft's LinkNames.java and
// tools/halfcraft/link.py build the same names; on linux each is posix_name's. pure: no windows, no sdk.

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "halfcraft_protocol.h"

namespace halfcraft
{
	inline constexpr char        LINK_ID_VARIABLE[] = "HALFCRAFT_LINK";
	inline constexpr char        LINK_ID_PARAMETER[] = "-hc_link";
	inline constexpr std::size_t MAX_LINK_ID = 32;

	struct LinkNames
	{
		std::string  id;  // empty: the default link
		std::wstring mapping;
		std::wstring frame_event;
		std::wstring minecraft_mutex;
	};

	/// one of A-Z, a-z, 0-9, _ and -: what a link id is made of.
	inline bool link_id_character(char c)
	{
		return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
	}

	/// a link id: 1 to MAX_LINK_ID characters, each a link_id_character.
	inline bool valid_link_id(std::string_view id)
	{
		return !id.empty() && id.size() <= MAX_LINK_ID && std::all_of(id.begin(), id.end(), link_id_character);
	}

	/// a link's names.
	/// @param id - its id, empty for the default link
	/// @return nullopt when id is neither empty nor a link id
	inline std::optional<LinkNames> link_names(std::string_view id)
	{
		if (!id.empty() && !valid_link_id(id)) {
			return std::nullopt;
		}
		LinkNames names;
		names.id = std::string(id);
		names.mapping = proto::kMappingName;
		if (!id.empty()) {
			names.mapping += L'.';
			for (const char c : id) {
				names.mapping += static_cast<wchar_t>(c);  // ascii, as valid_link_id says
			}
		}
		names.frame_event = names.mapping + L"_frame";
		names.minecraft_mutex = names.mapping + L"_minecraft";
		return names;
	}

	/// a link object's name on linux (posix shared memory, /dev/shm): what follows windows' namespace
	/// (Local\), after a slash. Local\HalfCraft_v1.b_minecraft is /HalfCraft_v1.b_minecraft, the file
	/// /dev/shm/HalfCraft_v1.b_minecraft. the names are ascii (valid_link_id), so nothing is lost.
	inline std::string posix_name(std::wstring_view windows_name)
	{
		const auto  slash = windows_name.rfind(L'\\');
		const auto  tail = slash == std::wstring_view::npos ? windows_name : windows_name.substr(slash + 1);
		std::string name = "/";
		for (const wchar_t c : tail) {
			name += static_cast<char>(c);
		}
		return name;
	}

	/// the link id a process was given, and where.
	struct LinkChoice
	{
		std::string id;
		const char* from = nullptr;  // LINK_ID_PARAMETER or LINK_ID_VARIABLE; nullptr: none given, the default link
	};

	/// -hc_link's id over HALFCRAFT_LINK's. an empty -hc_link is still given (and no link id); an empty
	/// HALFCRAFT_LINK is unset, as cmd's `set HALFCRAFT_LINK=` makes it.
	/// @param parameter - -hc_link's value: nullptr without -hc_link, "" when it has none
	/// @param variable - HALFCRAFT_LINK's, nullptr when it isn't set
	inline LinkChoice choose_link_id(const char* parameter, const char* variable)
	{
		if (parameter) {
			return { parameter, LINK_ID_PARAMETER };
		}
		if (variable && *variable) {
			return { variable, LINK_ID_VARIABLE };
		}
		return {};
	}

	/// the names of the link a choice gives.
	/// @return nullopt when it gives an id that isn't one: never the default link's, which is another pair's
	inline std::optional<LinkNames> chosen_link_names(const LinkChoice& choice)
	{
		if (choice.from && !valid_link_id(choice.id)) {
			return std::nullopt;
		}
		return link_names(choice.id);
	}
}
