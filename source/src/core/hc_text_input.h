#pragma once

#include <string>

// the characters windows types for the game (WM_CHAR): the player's own keyboard layout, with its
// dead keys, AltGr and input methods. keys alone can't say which character they type on a layout
// that isn't us qwerty. on linux the same comes from sdl2's text events (SDL_TEXTINPUT).

namespace halfcraft
{
	/// starts collecting what's typed for the windows of the calling thread (source's main thread,
	/// which runs the message loop). again: nothing.
	void text_input_attach();

	/// stops collecting: before client.dll unloads.
	void text_input_detach();

	/// what was typed since the last take, in order, without control characters (backspace, enter,
	/// tab, esc and the ctrl shortcuts stay keys).
	std::u32string text_input_take();
}
