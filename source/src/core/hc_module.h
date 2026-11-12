#pragma once

// the dlls of one game process find each other's halfcraft functions by name: client.dll, server.dll and the shader
// dll export a few with HC_EXPORT, and the others look them up with find_export. modules are named as on windows
// ("client.dll"); on linux each is the loaded .so of the same name ("client.so").

#ifdef _WIN32
#define HC_EXPORT __declspec(dllexport)
#else
// valve's linux build hides every symbol (-fvisibility=hidden) but those it marks
#define HC_EXPORT __attribute__((visibility("default")))
#endif

namespace halfcraft
{
	/// a function another loaded dll of this process exports, by name.
	/// @param module - e.g. "client.dll" (client.so on linux)
	/// @return the function, or nullptr when the dll or the export isn't there
	void* find_export(const char* module, const char* name);

	/// whether a dll is loaded in this process.
	/// @param module - e.g. "game_shader_generic_halfcraft.dll" (.so on linux)
	bool module_loaded(const char* module);
}
