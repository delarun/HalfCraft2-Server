#include "hc_module.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <string>
#endif

namespace halfcraft
{
#ifdef _WIN32
	void* find_export(const char* module, const char* name)
	{
		HMODULE handle = ::GetModuleHandleA(module);
		return handle ? reinterpret_cast<void*>(::GetProcAddress(handle, name)) : nullptr;
	}

	bool module_loaded(const char* module)
	{
		return ::GetModuleHandleA(module) != nullptr;
	}
#else
	namespace
	{
		/// a windows module name as linux names it: client.dll is client.so.
		std::string linux_name(const char* module)
		{
			std::string       name = module;
			const std::size_t dot = name.rfind('.');
			if (dot != std::string::npos && strcasecmp(name.c_str() + dot, ".dll") == 0) {
				name.replace(dot, std::string::npos, ".so");
			}
			return name;
		}

		struct Search
		{
			const char* name;
			std::string path;  // the loaded object's full path, once found
		};

		/// the engine loads each module by its full path: match the file name of every loaded object.
		int match(dl_phdr_info* info, std::size_t, void* data)
		{
			auto&       search = *static_cast<Search*>(data);
			const char* path = info->dlpi_name;
			if (!path || !*path) {
				return 0;
			}
			const char* slash = std::strrchr(path, '/');
			if (std::strcmp(slash ? slash + 1 : path, search.name) == 0) {
				search.path = path;
				return 1;
			}
			return 0;
		}

		/// a handle on the loaded module (one more reference, which is never given back: the modules stay loaded
		/// for the process's life, as GetModuleHandle's take none), or nullptr when it isn't loaded.
		void* loaded(const char* module)
		{
			const std::string name = linux_name(module);
			Search            search{ name.c_str(), {} };
			if (!::dl_iterate_phdr(match, &search)) {
				return nullptr;
			}
			return ::dlopen(search.path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
		}
	}

	void* find_export(const char* module, const char* name)
	{
		void* handle = loaded(module);
		return handle ? ::dlsym(handle, name) : nullptr;
	}

	bool module_loaded(const char* module)
	{
		return loaded(module) != nullptr;
	}
#endif
}
