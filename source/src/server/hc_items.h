#pragma once

// server.dll: minecraft's dropped items for half-life's gravity gun. each item near the player(s) (proto::ItemTable)
// gets an invisible halfcraft_item stand-in where it lies: nothing walks into it, but the gun's trace finds it, and the
// gun pulling or punting it takes the item into a held cube (take_item, hc_held_blocks.h). minecraft keeps drawing and
// moving the item itself until then.

#include <cstddef>

#include "halfcraft_protocol.h"
#include "core/hc_link.h"
#include "core/hc_units.h"

namespace halfcraft
{
	class Items
	{
	public:
		/// once per frame: the stand-ins follow the table.
		void update(Link& link, MapSlot slot);
		/// the level is going away, and its stand-ins with it.
		void reset();

	private:
		proto::ItemTable table_{};
		std::size_t      logged_ = 0;
	};
}
