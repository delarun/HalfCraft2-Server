#pragma once

// each campaign's own characters. half-life 2, episode one and episode two play as one game, the later campaign's
// content mounted over the earlier's (source\mod\<engine>\gameinfo.txt), and the campaigns ship their own versions of
// some characters under the same names, each with the animation models it includes ($includemodel): episode two's
// alyx has none of half-life 2's gestures or its vending machine, its g-man none of his walks, its dog none of
// episode one's intro. server.dll (server/hc_campaign_models.cpp) copies the versions a map's campaign plays from that
// campaign's own content (gameinfo.txt mounts each once more under its own path id) into the game folder, which is
// searched first, and takes them out on episode two's maps, whose merged content is its own. lost coast plays half-life
// 2's characters and its own soldiers' animations. the lists are of names only: what's copied comes from the player's
// own game files, and tools/game/campaign_models.py checks the lists against the installed games. pure code, no engine.
//
// in server.dll, include it after the sdk's headers and tier0/valve_minmax_off.h, which the sdk's min and max macros need.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hc_campaign.h"

namespace halfcraft
{
	/// whose characters a map plays.
	enum class ModelSet
	{
		kHalfLife2,
		kEpisodeOne,
		kEpisodeTwo,
		kLostCoast,
	};

	/// the root models: each one's closure (its includes, theirs, ...) as its set's own content resolves it is what
	/// that set plays
	inline constexpr const char* CAMPAIGN_CHARACTERS[] = {
		"models/advisor.mdl",
		"models/alyx.mdl",
		"models/alyx_intro.mdl",
		"models/blackout.mdl",
		"models/combine_dropship.mdl",
		"models/combine_soldier.mdl",
		"models/combine_soldier_prisonguard.mdl",
		"models/combine_super_soldier.mdl",
		"models/dog.mdl",
		"models/gman.mdl",
		"models/kleiner.mdl",
		"models/kleiner_monitor.mdl",
		"models/lamarr.mdl",
		"models/stalker.mdl",
		"models/vortigaunt.mdl",
		"models/vortigaunt_blue.mdl",
		"models/vortigaunt_slave.mdl",
		"models/zombie/classic.mdl",
	};

	/// every model a set copies into the game folder (the others' closures the merged mount already resolves as their own)
	inline constexpr const char* CAMPAIGN_MODELS_SWAPPED[] = {
		"models/advisor.mdl",
		"models/alyx.mdl",
		"models/alyx_animations.mdl",
		"models/alyx_gestures.mdl",
		"models/alyx_postures.mdl",
		"models/blackout.mdl",
		"models/combine_dropship.mdl",
		"models/combine_dropship_animations.mdl",
		"models/combine_soldier_anims.mdl",
		"models/dog.mdl",
		"models/dog_animations.mdl",
		"models/dog_gestures.mdl",
		"models/dog_postures.mdl",
		"models/gman.mdl",
		"models/gman_animations.mdl",
		"models/gman_gestures.mdl",
		"models/gman_postures.mdl",
		"models/kleiner.mdl",
		"models/kleiner_animations.mdl",
		"models/kleiner_gestures.mdl",
		"models/kleiner_postures.mdl",
		"models/lamarr.mdl",
		"models/stalker.mdl",
		"models/vortigaunt.mdl",
		"models/vortigaunt_anims.mdl",
		"models/vortigaunt_blue.mdl",
		"models/vortigaunt_gestures.mdl",
		"models/vortigaunt_postures.mdl",
		"models/zombie/classic.mdl",
	};

	/// the models that include a swapped one (directly or through another include) and aren't swapped themselves: the
	/// model cache keeps a virtual model of each, with its includes' sequences, which goes stale when the files change
	inline constexpr const char* CAMPAIGN_MODEL_DEPENDENTS[] = {
		"models/alyx_ep2.mdl",
		"models/alyx_interior.mdl",
		"models/alyx_intro.mdl",
		"models/combine_soldier.mdl",
		"models/combine_soldier_prisonguard.mdl",
		"models/combine_super_soldier.mdl",
		"models/gman_high.mdl",
		"models/kleiner_monitor.mdl",
		"models/vortigaunt_doctor.mdl",
		"models/vortigaunt_slave.mdl",
	};

	/// a file that goes with a .mdl (<its stem><extension>), and where it carries the .mdl's checksum
	struct ModelCompanion
	{
		const char* extension;
		std::size_t checksum_offset;
	};

	inline constexpr ModelCompanion MODEL_COMPANIONS[] = {
		{ ".vvd", 8 },
		{ ".dx90.vtx", 16 },
		{ ".dx80.vtx", 16 },
		{ ".sw.vtx", 16 },
		{ ".phy", 12 },
	};

	/// each campaign's own content's path id (gameinfo.txt), in the order the merged mount (GAME) has them
	inline constexpr const char* MERGED_MODEL_SOURCES[] = { "hc_ep2", "hc_ep1", "hc_hl2", "hc_lc" };

	inline constexpr char LOST_COAST_MAP[] = "d2_lostcoast";

	/// the rules plan_model_set() follows, as a number: a state file written under other rules is stale
	inline constexpr int CAMPAIGN_MODEL_RULES = 2;

	/// whose characters a map plays: lost coast's by its map's name, otherwise its campaign's (any map that isn't an
	/// episode's, as for its skill values and titles).
	inline ModelSet model_set_of(const char* map)
	{
		if (map) {
			const char* slash = std::strrchr(map, '/');
			const char* backslash = std::strrchr(map, '\\');
			const char* name = slash && slash > backslash ? slash + 1 : backslash ? backslash + 1
																				  : map;
			std::size_t i = 0;
			while (LOST_COAST_MAP[i] && name[i] && std::tolower(static_cast<unsigned char>(name[i])) == LOST_COAST_MAP[i]) {
				++i;
			}
			if (!LOST_COAST_MAP[i] && (!name[i] || name[i] == '.')) {
				return ModelSet::kLostCoast;
			}
		}
		switch (campaign_of(map)) {
		case Campaign::kEpisodeOne:
			return ModelSet::kEpisodeOne;
		case Campaign::kEpisodeTwo:
			return ModelSet::kEpisodeTwo;
		case Campaign::kHalfLife2:
			break;
		}
		return ModelSet::kHalfLife2;
	}

	inline constexpr ModelSet MODEL_SETS[] = { ModelSet::kHalfLife2, ModelSet::kEpisodeOne, ModelSet::kEpisodeTwo, ModelSet::kLostCoast };

	/// for the log: "half-life 2", "episode one", ...
	inline const char* model_set_name(ModelSet set)
	{
		switch (set) {
		case ModelSet::kEpisodeOne:
			return "episode one";
		case ModelSet::kEpisodeTwo:
			return "episode two";
		case ModelSet::kLostCoast:
			return "lost coast";
		case ModelSet::kHalfLife2:
			break;
		}
		return "half-life 2";
	}

	/// its word in the state file
	inline const char* model_set_key(ModelSet set)
	{
		switch (set) {
		case ModelSet::kEpisodeOne:
			return "ep1";
		case ModelSet::kEpisodeTwo:
			return "ep2";
		case ModelSet::kLostCoast:
			return "lc";
		case ModelSet::kHalfLife2:
			break;
		}
		return "hl2";
	}

	inline std::optional<ModelSet> model_set_from_key(const std::string& key)
	{
		for (const ModelSet set : MODEL_SETS) {
			if (key == model_set_key(set)) {
				return set;
			}
		}
		return std::nullopt;
	}

	/// the path ids a set's own models come from, first to last (valve's own mount of that campaign). none: the
	/// merged mount is episode two's own
	inline std::vector<const char*> model_set_sources(ModelSet set)
	{
		switch (set) {
		case ModelSet::kEpisodeOne:
			return { "hc_ep1", "hc_hl2" };
		case ModelSet::kEpisodeTwo:
			return {};
		case ModelSet::kLostCoast:
			return { "hc_lc", "hc_hl2" };
		case ModelSet::kHalfLife2:
			break;
		}
		return { "hc_hl2" };
	}

	/// what a model's header says that matters here.
	struct StudioHeader
	{
		std::uint32_t            checksum = 0;
		std::vector<std::string> includes;    // "models/....mdl", lower case
		std::string              anim_block;  // "models/....ani" its animations stream from, lower case; empty: none
	};

	namespace campaign_models_detail
	{
		inline constexpr std::size_t   HEADER_BYTES = 408;  // studiohdr_t on disk
		inline constexpr std::size_t   VERSION = 4, CHECKSUM = 8, LENGTH = 76, INCLUDES = 336, INCLUDE_INDEX = 340,
									   ANIM_BLOCK_NAME = 348, ANIM_BLOCKS = 352;
		inline constexpr int           MIN_VERSION = 44, MAX_VERSION = 49;
		inline constexpr std::uint32_t MAX_INCLUDES = 256;

		inline std::optional<std::uint32_t> le32(const std::uint8_t* data, std::size_t size, std::size_t at)
		{
			if (!data || at > size || size - at < 4) {
				return std::nullopt;
			}
			return static_cast<std::uint32_t>(data[at]) | static_cast<std::uint32_t>(data[at + 1]) << 8 |
				static_cast<std::uint32_t>(data[at + 2]) << 16 | static_cast<std::uint32_t>(data[at + 3]) << 24;
		}

		inline char normal_char(char c)
		{
			return c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a')
														  : c;
		}

		/// the nul-terminated string at an offset, lower case with forward slashes; nullopt when it runs off the end
		inline std::optional<std::string> name_at(const std::uint8_t* data, std::size_t size, std::uint64_t at)
		{
			if (at >= size) {
				return std::nullopt;
			}
			std::string name;
			for (std::size_t i = static_cast<std::size_t>(at); i < size; ++i) {
				if (!data[i]) {
					return name;
				}
				name += normal_char(static_cast<char>(data[i]));
			}
			return std::nullopt;
		}

		inline bool ends_with(const std::string& text, const char* end)
		{
			const std::size_t length = std::strlen(end);
			return text.size() >= length && text.compare(text.size() - length, length, end) == 0;
		}

		/// a name the game folder may get: models/<something>.<extension>, no way out of models/
		inline bool model_path(const std::string& name, const char* extension)
		{
			return name.rfind("models/", 0) == 0 && name.size() > 7 + std::strlen(extension) && ends_with(name, extension) &&
				name.find("..") == std::string::npos && name.find(':') == std::string::npos;
		}

		inline std::string stem(const std::string& mdl)
		{
			return ends_with(mdl, ".mdl") ? mdl.substr(0, mdl.size() - 4) : mdl;
		}

		template <std::size_t N>
		bool listed(const char* const (&list)[N], const std::string& name)
		{
			for (const char* entry : list) {
				if (name == entry) {
					return true;
				}
			}
			return false;
		}

		inline bool has(const std::vector<const char*>& ids, const char* id)
		{
			for (const char* entry : ids) {
				if (std::strcmp(entry, id) == 0) {
					return true;
				}
			}
			return false;
		}

		inline std::string joined(const std::vector<std::string>& items, const char* between = ", ")
		{
			std::string text;
			for (const std::string& item : items) {
				text += (text.empty() ? "" : between) + item;
			}
			return text;
		}
	}

	/// a model or file name as the lists write it: lower case, forward slashes
	inline std::string normal_model_name(std::string name)
	{
		for (char& c : name) {
			c = campaign_models_detail::normal_char(c);
		}
		return name;
	}

	/// a checksum as the log and the state file write it
	inline std::string checksum_text(std::uint32_t value)
	{
		char text[9];
		std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(value));
		return text;
	}

	/// a .mdl's header, read by its offsets on disk (64-bit engines lay studiohdr_t out differently in memory). nullopt
	/// when it isn't a studio model this can trust: shorter than the header, not "IDST", a version outside 44-49, a
	/// length other than its size (a file cut short), or an include or anim block name off the end or outside models/.
	inline std::optional<StudioHeader> read_studio_header(const std::uint8_t* data, std::size_t size)
	{
		namespace d = campaign_models_detail;
		if (!data || size < d::HEADER_BYTES || std::memcmp(data, "IDST", 4) != 0) {
			return std::nullopt;
		}
		const auto version = static_cast<int>(*d::le32(data, size, d::VERSION));
		if (version < d::MIN_VERSION || version > d::MAX_VERSION || *d::le32(data, size, d::LENGTH) != size) {
			return std::nullopt;
		}
		StudioHeader header;
		header.checksum = *d::le32(data, size, d::CHECKSUM);
		const std::uint32_t includes = *d::le32(data, size, d::INCLUDES);
		const std::uint32_t include_index = *d::le32(data, size, d::INCLUDE_INDEX);
		if (includes > d::MAX_INCLUDES || (includes && static_cast<std::uint64_t>(include_index) + 8ull * includes > size)) {
			return std::nullopt;
		}
		for (std::uint32_t i = 0; i < includes; ++i) {
			const std::size_t group = include_index + 8u * i;
			const auto        name = d::name_at(data, size, static_cast<std::uint64_t>(group) + *d::le32(data, size, group + 4));
			if (!name || !d::model_path(*name, ".mdl")) {
				return std::nullopt;
			}
			header.includes.push_back(*name);
		}
		if (*d::le32(data, size, d::ANIM_BLOCKS) != 0) {
			const auto name = d::name_at(data, size, *d::le32(data, size, d::ANIM_BLOCK_NAME));
			if (!name || !d::model_path(*name, ".ani")) {
				return std::nullopt;
			}
			header.anim_block = *name;
		}
		return header;
	}

	/// the checksum a companion file carries (the .mdl's, when they belong together); nullopt when it's too short
	inline std::optional<std::uint32_t> companion_checksum(const ModelCompanion& companion, const std::vector<std::uint8_t>& data)
	{
		return campaign_models_detail::le32(data.data(), data.size(), companion.checksum_offset);
	}

	/// the game's files as the planner reads them: server.dll's file system, a map in the tests.
	class ModelFileSource
	{
	public:
		virtual ~ModelFileSource() = default;
		/// whether the content under a path id has the file
		virtual bool exists(const std::string& name, const char* path_id) const = 0;
		/// its size, -1 when it isn't there
		virtual long size(const std::string& name, const char* path_id) const = 0;
		/// its bytes, all of them, or up to max_bytes when that's nonzero
		virtual bool read(const std::string& name, const char* path_id, std::vector<std::uint8_t>& out, std::size_t max_bytes = 0) const = 0;
	};

	struct PlannedFile
	{
		std::string               name;
		const char*               source = nullptr;  // its path id
		std::vector<std::uint8_t> data;
	};

	struct PlannedModel
	{
		std::string   name;
		std::string   source;  // its path id
		std::uint32_t checksum = 0;
	};

	/// what gives a set its own characters: the models it copies, sorted by name, and their files
	struct ModelSetPlan
	{
		std::vector<PlannedModel> models;
		std::vector<PlannedFile>  files;
		std::vector<std::string>  skipped;  // "<root>'s closure: <why>", for each closure left out, and a set none of whose
											// characters its sources have
		int roots = 0;                      // the characters the set's own content has

		std::size_t bytes() const
		{
			std::size_t total = 0;
			for (const PlannedFile& file : files) {
				total += file.data.size();
			}
			return total;
		}
	};

	namespace campaign_models_detail
	{
		/// a model as a set's own content has it
		struct OwnModel
		{
			const char*              source = nullptr;
			StudioHeader             header;
			bool                     copied = false;  // the merged mount resolves another version
			std::vector<PlannedFile> files;           // when copied: the .mdl, its companions and its anim block
			std::string              error;           // why it can't be: then its closures are left out
		};

		/// reads a whole file and checks it's as big as the file system says
		inline bool read_whole(const ModelFileSource& files, const std::string& name, const char* source, std::vector<std::uint8_t>& out)
		{
			const long size = files.size(name, source);
			return size > 0 && files.read(name, source, out) && out.size() == static_cast<std::size_t>(size);
		}

		inline void add_file(const ModelFileSource& files, const std::string& name, const char* source, OwnModel& model)
		{
			PlannedFile file{ name, source, {} };
			if (!read_whole(files, name, source, file.data)) {
				model.error = name + " couldn't be read whole from " + source;
				return;
			}
			model.files.push_back(std::move(file));
		}

		/// the merged sources whose version of a file the game would find for a copy that lacks it, where valve's own
		/// mount of the set wouldn't: the ones ahead of the copy's source, and the ones outside the set's own content
		inline std::vector<const char*> slip_in_sources(const std::vector<const char*>& set_sources, const char* source)
		{
			std::vector<const char*> found;
			bool                     ahead = true;
			for (const char* merged : MERGED_MODEL_SOURCES) {
				ahead = ahead && std::strcmp(merged, source) != 0;
				if (ahead || !has(set_sources, merged)) {
					found.push_back(merged);
				}
			}
			return found;
		}

		/// whether the merged mount resolves another version of a .mdl than the set's own source has
		inline bool merged_differs(const ModelFileSource& files, const std::string& name, const char* source, std::uint32_t checksum, std::string& error)
		{
			for (const char* merged : MERGED_MODEL_SOURCES) {
				if (!files.exists(name, merged)) {
					continue;
				}
				if (std::strcmp(merged, source) == 0) {
					return false;
				}
				std::vector<std::uint8_t> head;
				const auto                theirs = files.read(name, merged, head, CHECKSUM + 4) ? le32(head.data(), head.size(), CHECKSUM) : std::nullopt;
				if (!theirs) {
					error = name + "'s version under " + merged + " couldn't be read";
					return true;
				}
				return *theirs != checksum;
			}
			return false;
		}

		/// the companions and the anim block of a model that's copied, all from its .mdl's source
		inline void add_companions(const ModelFileSource& files, const std::vector<const char*>& set_sources, const std::string& name, OwnModel& model)
		{
			const std::string base = stem(name);
			for (const ModelCompanion& companion : MODEL_COMPANIONS) {
				const std::string file = base + companion.extension;
				if (!files.exists(file, model.source)) {
					for (const char* other : slip_in_sources(set_sources, model.source)) {
						if (files.exists(file, other)) {
							model.error = file + " isn't in " + model.source + " but " + other + " has one, which the game would take with this .mdl";
							return;
						}
					}
					continue;
				}
				add_file(files, file, model.source, model);
				if (!model.error.empty()) {
					return;
				}
				const auto checksum = companion_checksum(companion, model.files.back().data);
				if (!checksum || *checksum != model.header.checksum) {
					model.error = file + "'s checksum " + (checksum ? checksum_text(*checksum) : std::string("(none)")) + " isn't the .mdl's " +
						checksum_text(model.header.checksum);
					return;
				}
			}
			if (!model.header.anim_block.empty()) {
				// the game folder's leftovers are found by <stem>.ani: a copy streaming from another name would be lost
				if (model.header.anim_block != base + ".ani") {
					model.error = name + " streams its animations from " + model.header.anim_block + ", not " + base + ".ani";
					return;
				}
				if (!files.exists(model.header.anim_block, model.source)) {
					model.error = model.header.anim_block + " isn't in " + model.source;
					return;
				}
				add_file(files, model.header.anim_block, model.source, model);
			}
		}

		/// a model as the set's own sources resolve it, and whether and what it copies
		inline OwnModel own_model(const ModelFileSource& files, const std::vector<const char*>& sources, const std::string& name)
		{
			OwnModel model;
			for (const char* source : sources) {
				if (files.exists(name, source)) {
					model.source = source;
					break;
				}
			}
			if (!model.source) {
				model.error = name + " isn't in the campaign's own content";
				return model;
			}
			PlannedFile mdl{ name, model.source, {} };
			if (!read_whole(files, name, model.source, mdl.data)) {
				model.error = name + " couldn't be read whole from " + model.source;
				return model;
			}
			const auto header = read_studio_header(mdl.data.data(), mdl.data.size());
			if (!header) {
				model.error = name + " under " + model.source + " isn't a studio model this can read";
				return model;
			}
			model.header = *header;
			model.copied = merged_differs(files, name, model.source, header->checksum, model.error);
			if (!model.error.empty() || !model.copied) {
				return model;
			}
			if (!listed(CAMPAIGN_MODELS_SWAPPED, name)) {
				model.error = name + " would be copied but isn't in CAMPAIGN_MODELS_SWAPPED (tools/game/campaign_models.py)";
				return model;
			}
			model.files.push_back(std::move(mdl));
			add_companions(files, sources, name, model);
			return model;
		}

		inline bool contains(const std::vector<std::string>& list, const std::string& name)
		{
			for (const std::string& entry : list) {
				if (entry == name) {
					return true;
				}
			}
			return false;
		}
	}

	/// the files that give a set its own characters. each root its own sources have is taken with its closure, every
	/// member as those sources resolve it; a member is copied when the merged mount's first version of it (by
	/// MERGED_MODEL_SOURCES) is another one, with its companions and its anim block from the source of its .mdl. a
	/// root's closure goes all or nothing: a member that's missing, unreadable, a companion whose checksum isn't the
	/// .mdl's or that the game would take from another pak, an anim block that isn't <stem>.ani, a copied name off
	/// CAMPAIGN_MODELS_SWAPPED, leaves that closure out (skipped says why). a member another root's whole closure has is
	/// still copied, so that character plays a mix: skipped names those members.
	inline ModelSetPlan plan_model_set(ModelSet set, const ModelFileSource& files)
	{
		namespace d = campaign_models_detail;
		ModelSetPlan plan;
		const auto   sources = model_set_sources(set);
		if (sources.empty()) {
			return plan;
		}
		std::map<std::string, d::OwnModel>                            models;
		std::set<std::string>                                         kept;
		std::vector<std::pair<std::string, std::vector<std::string>>> failed;  // a root, the members it had
		for (const char* root : CAMPAIGN_CHARACTERS) {
			bool own = false;
			for (const char* source : sources) {
				own = own || files.exists(root, source);
			}
			if (!own) {
				continue;
			}
			++plan.roots;
			std::vector<std::string> closure{ root };
			std::string              failure;
			for (std::size_t i = 0; i < closure.size() && failure.empty(); ++i) {
				auto found = models.find(closure[i]);
				if (found == models.end()) {
					found = models.emplace(closure[i], d::own_model(files, sources, closure[i])).first;
				}
				failure = found->second.error;
				for (const std::string& include : found->second.header.includes) {
					if (!d::contains(closure, include)) {
						closure.push_back(include);
					}
				}
			}
			if (!failure.empty()) {
				plan.skipped.push_back(std::string(root) + "'s closure: " + failure);
				failed.emplace_back(root, closure);
				continue;
			}
			kept.insert(closure.begin(), closure.end());
		}
		for (const std::string& name : kept) {  // sorted
			d::OwnModel& model = models[name];
			if (!model.copied) {
				continue;
			}
			plan.models.push_back({ name, model.source, model.header.checksum });
			for (PlannedFile& file : model.files) {
				plan.files.push_back(std::move(file));
			}
		}
		for (std::size_t i = 0; i < failed.size(); ++i) {
			std::vector<std::string> mixed;
			for (const std::string& member : failed[i].second) {
				if (kept.count(member) && models[member].copied) {
					mixed.push_back(member);
				}
			}
			if (!mixed.empty()) {
				plan.skipped[i] += "; it still gets " + d::joined(mixed) + " of this set through other characters";
			}
		}
		if (plan.roots == 0) {
			std::vector<std::string> ids(sources.begin(), sources.end());
			plan.skipped.push_back("none of the characters is in " + d::joined(ids) + " (gameinfo.txt mounts them there)");
		}
		return plan;
	}

	/// every file a plan may write (each swapped model's .mdl, companions and <stem>.ani), to take out what's left over
	inline std::vector<std::string> campaign_model_files()
	{
		std::vector<std::string> names;
		for (const char* model : CAMPAIGN_MODELS_SWAPPED) {
			const std::string base = campaign_models_detail::stem(model);
			names.push_back(model);
			names.push_back(base + ".ani");
			for (const ModelCompanion& companion : MODEL_COMPANIONS) {
				names.push_back(base + companion.extension);
			}
		}
		return names;
	}

	/// the model cache's entries that changed files invalidate: the swapped models and the ones that include them
	inline std::vector<std::string> campaign_models_to_flush()
	{
		std::vector<std::string> names(std::begin(CAMPAIGN_MODELS_SWAPPED), std::end(CAMPAIGN_MODELS_SWAPPED));
		for (const char* dependent : CAMPAIGN_MODEL_DEPENDENTS) {
			if (!campaign_models_detail::listed(CAMPAIGN_MODELS_SWAPPED, dependent)) {
				names.emplace_back(dependent);
			}
		}
		return names;
	}

	/// the .mdl a file of the game folder's goes with (models/x.ani, models/x.dx90.vtx: models/x.mdl); nullopt for one
	/// that isn't a model's
	inline std::optional<std::string> model_of_file(const std::string& file)
	{
		namespace d = campaign_models_detail;
		std::string name = normal_model_name(file);
		if (d::ends_with(name, ".mdl")) {
			return name;
		}
		std::vector<const char*> extensions{ ".ani" };
		for (const ModelCompanion& companion : MODEL_COMPANIONS) {
			extensions.push_back(companion.extension);
		}
		for (const char* extension : extensions) {
			if (d::ends_with(name, extension)) {
				return name.substr(0, name.size() - std::strlen(extension)) + ".mdl";
			}
		}
		return std::nullopt;
	}

	/// the models some files go with, sorted, each once
	inline std::vector<std::string> models_of_files(const std::vector<std::string>& files)
	{
		std::set<std::string> models;
		for (const std::string& file : files) {
			if (const auto model = model_of_file(file)) {
				models.insert(*model);
			}
		}
		return { models.begin(), models.end() };
	}

	/// what the game folder holds, halfcraft-campaign-models.txt: the set and every file written for it (with its size,
	/// and its source's, to notice a pak valve updated), and each copied model's checksum and source. written last, so a
	/// folder without it may hold anything
	struct CampaignModelFile
	{
		std::string name;
		long        size = 0;
		std::string source;
		long        source_size = 0;
	};

	struct CampaignModelState
	{
		std::uint32_t                  list_hash = 0;
		ModelSet                       set = ModelSet::kEpisodeTwo;
		std::vector<CampaignModelFile> files;
		std::vector<PlannedModel>      models;
	};

	inline constexpr char CAMPAIGN_MODEL_STATE_HEADER[] = "halfcraft campaign models 2";

	/// the fingerprint (fnv-1a) of the lists and the rules a state was written under: every name and number in the
	/// lists, the sets' sources, lost coast's map and the planner's rules. a state written under others is stale
	inline std::uint32_t campaign_model_list_hash()
	{
		std::uint32_t hash = 2166136261u;
		const auto    add = [&hash](const std::string& text) {
			for (const char c : text) {
				hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619u;
			}
			hash = (hash ^ 0xffu) * 16777619u;
		};
		add(CAMPAIGN_MODEL_STATE_HEADER);
		add(std::to_string(CAMPAIGN_MODEL_RULES));
		add(LOST_COAST_MAP);
		for (const char* name : CAMPAIGN_CHARACTERS) {
			add(name);
		}
		for (const char* name : CAMPAIGN_MODELS_SWAPPED) {
			add(name);
		}
		for (const char* name : CAMPAIGN_MODEL_DEPENDENTS) {
			add(name);
		}
		for (const ModelCompanion& companion : MODEL_COMPANIONS) {
			add(companion.extension + std::to_string(companion.checksum_offset));
		}
		for (const char* source : MERGED_MODEL_SOURCES) {
			add(source);
		}
		for (const ModelSet set : MODEL_SETS) {
			add(model_set_key(set));
			for (const char* source : model_set_sources(set)) {
				add(source);
			}
		}
		return hash;
	}

	inline std::string format_campaign_model_state(const CampaignModelState& state)
	{
		std::string text = std::string(CAMPAIGN_MODEL_STATE_HEADER) + "\n";
		text += "list " + checksum_text(state.list_hash) + "\n";
		text += std::string("set ") + model_set_key(state.set) + "\n";
		for (const CampaignModelFile& file : state.files) {
			text += "file " + file.name + " " + std::to_string(file.size) + " " + file.source + " " + std::to_string(file.source_size) + "\n";
		}
		for (const PlannedModel& model : state.models) {
			text += "model " + model.name + " " + checksum_text(model.checksum) + " " + model.source + "\n";
		}
		return text;
	}

	namespace campaign_models_detail
	{
		inline std::vector<std::string> words(const std::string& line)
		{
			std::vector<std::string> found;
			std::size_t              at = 0;
			while (at < line.size()) {
				const std::size_t start = line.find_first_not_of(' ', at);
				if (start == std::string::npos) {
					break;
				}
				std::size_t end = line.find(' ', start);
				if (end == std::string::npos) {
					end = line.size();
				}
				found.push_back(line.substr(start, end - start));
				at = end;
			}
			return found;
		}

		/// a whole number in a base, nothing else; nullopt otherwise
		inline std::optional<unsigned long> number(const std::string& text, int base)
		{
			if (text.empty() || text.size() > 10) {
				return std::nullopt;
			}
			unsigned long value = 0;
			for (const char c : text) {
				const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
																						: 99;
				if (digit >= base) {
					return std::nullopt;
				}
				value = value * static_cast<unsigned long>(base) + static_cast<unsigned long>(digit);
			}
			return value;
		}

		inline bool checksum_word(const std::string& word)
		{
			return word.size() == 8 && number(word, 16).has_value();
		}

		/// a line of the state file, read into it; false when it isn't one
		inline bool read_state_line(const std::vector<std::string>& line, CampaignModelState& state, bool& has_list, bool& has_set)
		{
			const std::string& kind = line[0];
			if (kind == "list" && line.size() == 2 && checksum_word(line[1])) {
				state.list_hash = static_cast<std::uint32_t>(*number(line[1], 16));
				has_list = true;
			} else if (const auto set = kind == "set" && line.size() == 2 ? model_set_from_key(line[1]) : std::nullopt; set) {
				state.set = *set;
				has_set = true;
			} else if (kind == "file" && line.size() == 5 && number(line[2], 10).value_or(0) > 0 && number(line[4], 10).value_or(0) > 0) {
				state.files.push_back({ line[1], static_cast<long>(*number(line[2], 10)), line[3], static_cast<long>(*number(line[4], 10)) });
			} else if (kind == "model" && line.size() == 4 && checksum_word(line[2])) {
				state.models.push_back({ line[1], line[3], static_cast<std::uint32_t>(*number(line[2], 16)) });
			} else {
				return false;
			}
			return true;
		}
	}

	/// nullopt for anything that isn't such a file, whole
	inline std::optional<CampaignModelState> parse_campaign_model_state(const std::string& text)
	{
		namespace d = campaign_models_detail;
		CampaignModelState state;
		bool               has_list = false, has_set = false;
		std::size_t        start = 0;
		for (int line_number = 0; start < text.size(); ++line_number) {
			std::size_t end = text.find('\n', start);
			if (end == std::string::npos) {
				end = text.size();
			}
			std::string line = text.substr(start, end - start);
			start = end + 1;
			if (!line.empty() && line.back() == '\r') {
				line.pop_back();
			}
			if (line_number == 0) {
				if (line != CAMPAIGN_MODEL_STATE_HEADER) {
					return std::nullopt;
				}
				continue;
			}
			const auto words = d::words(line);
			if (!words.empty() && !d::read_state_line(words, state, has_list, has_set)) {
				return std::nullopt;
			}
		}
		if (!has_list || !has_set) {
			return std::nullopt;
		}
		return state;
	}

	/// why a state's copies may no longer be what the set's sources give (a pak valve updated), checked once a run:
	/// a file's source no longer as big, a copied model's source version or the merged mount's no longer what made it
	/// a copy. nullopt: they still are
	inline std::optional<std::string> stale_campaign_models(const CampaignModelState& state, const ModelFileSource& files)
	{
		namespace d = campaign_models_detail;
		for (const CampaignModelFile& file : state.files) {
			if (files.size(file.name, file.source.c_str()) != file.source_size) {
				return file.name + " under " + file.source + " isn't the size it was";
			}
		}
		for (const PlannedModel& model : state.models) {
			std::vector<std::uint8_t> head;
			const auto                own = files.read(model.name, model.source.c_str(), head, d::CHECKSUM + 4) ? d::le32(head.data(), head.size(), d::CHECKSUM) : std::nullopt;
			if (own != model.checksum) {
				return model.name + " under " + model.source + " isn't the version copied";
			}
			std::string error;
			if (!d::merged_differs(files, model.name, model.source.c_str(), model.checksum, error) || !error.empty()) {
				return model.name + " is the merged mount's version now";
			}
		}
		return std::nullopt;
	}

	/// the game folder as a swap sees it
	struct CampaignModelFolder
	{
		/// a file's size there, -1 when it isn't there
		std::function<long(const std::string&)> size;
		/// whether the file there has exactly these bytes (read only for a file of the same size)
		std::function<bool(const std::string&, const std::vector<std::uint8_t>&)> holds;
	};

	/// how a swap changes the game folder: the plan's files it already holds byte for byte, which stay; the files there
	/// that go (every name a plan may write, and the old state's, but those that stay); the plan's files that come
	struct CampaignModelSwap
	{
		std::vector<std::string> keep;
		std::vector<std::string> remove;
		std::vector<std::size_t> write;  // indices into the plan's files
	};

	/// @param old - the state the folder had, if any: the names of its files go too (they may be from older lists)
	inline CampaignModelSwap plan_campaign_model_swap(const std::optional<CampaignModelState>& old, const ModelSetPlan& plan, const CampaignModelFolder& folder)
	{
		CampaignModelSwap swap;
		for (std::size_t i = 0; i < plan.files.size(); ++i) {
			const PlannedFile& file = plan.files[i];
			if (folder.size(file.name) == static_cast<long>(file.data.size()) && folder.holds(file.name, file.data)) {
				swap.keep.push_back(file.name);
			} else {
				swap.write.push_back(i);
			}
		}
		std::set<std::string>    seen(swap.keep.begin(), swap.keep.end());
		std::vector<std::string> candidates = campaign_model_files();
		if (old) {
			for (const CampaignModelFile& had : old->files) {
				candidates.push_back(had.name);
			}
		}
		for (const std::string& name : candidates) {
			if (seen.insert(name).second && folder.size(name) >= 0) {
				swap.remove.push_back(name);
			}
		}
		return swap;
	}

	/// a file to put back after a swap stopped halfway: the plan's (by its index), or the old state's (from its source)
	struct CampaignModelRestore
	{
		std::string                name;
		std::optional<std::size_t> plan_file;
		std::string                source;  // the old state's file's, when not the plan's
		long                       size = 0;
	};

	/// after a swap that stopped halfway and couldn't take every copy out: the files that make each model whose files
	/// are left whole again, in the version its leftovers are. a leftover with the plan's bytes brings the rest of the
	/// plan's files of its model back; otherwise the old state's files of that model come back from their sources. a model
	/// neither has files of stays as it is.
	inline std::vector<CampaignModelRestore> plan_campaign_model_restore(const std::vector<std::string>& leftovers, const std::optional<CampaignModelState>& old,
		const ModelSetPlan& plan, const CampaignModelFolder& folder)
	{
		std::vector<CampaignModelRestore> restore;
		for (const std::string& model : models_of_files(leftovers)) {
			bool planned_version = false;
			for (const std::string& left : leftovers) {
				for (const PlannedFile& file : plan.files) {
					planned_version = planned_version ||
						(file.name == left && model_of_file(left) == model && folder.size(left) == static_cast<long>(file.data.size()) && folder.holds(left, file.data));
				}
			}
			if (planned_version) {
				for (std::size_t i = 0; i < plan.files.size(); ++i) {
					if (model_of_file(plan.files[i].name) == model && folder.size(plan.files[i].name) < 0) {
						restore.push_back({ plan.files[i].name, i, {}, static_cast<long>(plan.files[i].data.size()) });
					}
				}
				continue;
			}
			if (old) {
				for (const CampaignModelFile& file : old->files) {
					if (model_of_file(file.name) == model && folder.size(file.name) < 0) {
						restore.push_back({ file.name, std::nullopt, file.source, file.size });
					}
				}
			}
		}
		return restore;
	}
}
