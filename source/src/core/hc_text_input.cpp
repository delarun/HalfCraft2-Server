#include "hc_text_input.h"

#include "hc_log.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#endif

namespace halfcraft
{
#ifdef _WIN32
	namespace
	{
		HHOOK          g_hook = nullptr;
		bool           g_gave_up = false;
		wchar_t        g_high_surrogate = 0;
		std::u32string g_typed;

		void typed(wchar_t unit)
		{
			if (unit >= 0xD800 && unit <= 0xDBFF) {
				g_high_surrogate = unit;
				return;
			}
			char32_t ch = unit;
			if (unit >= 0xDC00 && unit <= 0xDFFF) {
				if (!g_high_surrogate) {
					return;
				}
				ch = 0x10000 + ((static_cast<char32_t>(g_high_surrogate) - 0xD800) << 10) + (unit - 0xDC00);
			}
			g_high_surrogate = 0;
			if (ch >= 0x20 && ch != 0x7F) {
				g_typed.push_back(ch);
			}
		}

		// source's message loop is ansi: by the time WM_CHAR reaches the window it went through the
		// ansi code page, and what that lacks (cyrillic on a western system, emoji) is a ?. a unicode
		// hook sees the message as it leaves the queue, still utf-16
		LRESULT CALLBACK message_hook(int code, WPARAM removed, LPARAM message)
		{
			if (code == HC_ACTION && removed == PM_REMOVE) {
				// a WH_GETMESSAGE hook gets its MSG's address as an LPARAM: windows' own contract
				const auto* msg = reinterpret_cast<const MSG*>(message);  // NOLINT(performance-no-int-to-ptr)
				if (msg->message == WM_CHAR) {
					typed(static_cast<wchar_t>(msg->wParam));
				}
			}
			return ::CallNextHookEx(g_hook, code, removed, message);
		}
	}

	void text_input_attach()
	{
		if (g_hook || g_gave_up) {
			return;
		}
		g_hook = ::SetWindowsHookExW(WH_GETMESSAGE, message_hook, nullptr, ::GetCurrentThreadId());
		if (!g_hook) {
			log_warning("text input: can't watch the game's messages (error %lu); minecraft's text fields get nothing typed", ::GetLastError());
			g_gave_up = true;
			return;
		}
		log_info("text input: watching the game's messages for typed characters");
	}

	void text_input_detach()
	{
		if (g_hook) {
			::UnhookWindowsHookEx(g_hook);
			g_hook = nullptr;
		}
		g_typed.clear();
	}

	std::u32string text_input_take()
	{
		std::u32string taken;
		taken.swap(g_typed);
		return taken;
	}
#else
	// linux: the engine runs on sdl2, which types characters for the window as SDL_TEXTINPUT events, utf-8 in the
	// player's layout with its input methods (sdl starts text input itself on a desktop). an event watch on the sdl
	// the engine loaded sees each one as it's queued
	namespace
	{
		constexpr std::uint32_t SDL_TEXTINPUT = 0x303;

		struct TextInputEvent  // SDL_TextInputEvent, the part of SDL_Event a text event uses
		{
			std::uint32_t type;
			std::uint32_t timestamp;
			std::uint32_t window;
			char          text[32];
		};

		using EventFilter = int (*)(void* userdata, void* event);
		using WatchFn = void (*)(EventFilter filter, void* userdata);

		std::mutex     g_lock;
		std::u32string g_typed;
		WatchFn        g_delete_watch = nullptr;
		bool           g_watching = false;
		bool           g_gave_up = false;

		/// a utf-8 string's characters, but control ones (sdl types none, but a stray byte decodes to nothing).
		void typed(const char* utf8)
		{
			const auto* p = reinterpret_cast<const unsigned char*>(utf8);
			while (*p) {
				char32_t ch = *p;
				int      more = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2
						 : ch >= 0xC0                       ? 1
															: 0;
				ch = more == 3 ? ch & 0x07 : more == 2 ? ch & 0x0F
					: more == 1                        ? ch & 0x1F
													   : ch;
				++p;
				for (; more > 0 && (*p & 0xC0) == 0x80; --more, ++p) {
					ch = (ch << 6) | (*p & 0x3F);
				}
				if (more == 0 && ch >= 0x20 && ch != 0x7F) {
					g_typed.push_back(ch);
				}
			}
		}

		int watch(void*, void* event)
		{
			const auto* text = static_cast<const TextInputEvent*>(event);
			if (text->type == SDL_TEXTINPUT) {
				char copy[sizeof(text->text) + 1] = {};
				std::memcpy(copy, text->text, sizeof(text->text));
				std::lock_guard<std::mutex> hold(g_lock);
				typed(copy);
			}
			return 1;
		}

		void* sdl()
		{
			return ::dlopen("libSDL2-2.0.so.0", RTLD_LAZY | RTLD_NOLOAD);
		}
	}

	void text_input_attach()
	{
		if (g_watching || g_gave_up) {
			return;
		}
		void* library = sdl();
		auto  add = library ? reinterpret_cast<WatchFn>(::dlsym(library, "SDL_AddEventWatch")) : nullptr;
		g_delete_watch = library ? reinterpret_cast<WatchFn>(::dlsym(library, "SDL_DelEventWatch")) : nullptr;
		if (!add || !g_delete_watch) {
			log_warning("text input: the game's sdl2 isn't loaded; minecraft's text fields get nothing typed");
			g_gave_up = true;
			return;
		}
		add(watch, nullptr);
		g_watching = true;
		log_info("text input: watching sdl's text events for typed characters");
	}

	void text_input_detach()
	{
		if (g_watching) {
			g_delete_watch(watch, nullptr);
			g_watching = false;
		}
		std::lock_guard<std::mutex> hold(g_lock);
		g_typed.clear();
	}

	std::u32string text_input_take()
	{
		std::lock_guard<std::mutex> hold(g_lock);
		std::u32string              taken;
		taken.swap(g_typed);
		return taken;
	}
#endif
}
