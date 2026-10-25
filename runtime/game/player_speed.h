// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The player's car's speed, read from the game's own physics body: the first
// entry of the game's vehicle table (0x82C745A8, {vtable, data, capacity in
// bytes, count}, data = 8-byte entries {owner object, 1}, the player first)
// is the player's vehicle; its component at +128 points (+48) at a pointer to
// its rigid body, whose linear velocity (x y z, metres a second, big-endian floats) is
// at +0x20. As AndhikaFW's multiplayer adapter found them (game/mp_adapter.cpp
// in their fork). One caller (the renderer): plain reads of guest memory, a frame late at
// worst.
#pragma once

namespace game
{
    // km/h, or a negative value while there is no player's car (menus,
    // loading, a cutscene without one) or what is there doesn't look like one.
    float PlayerSpeedKph();
}
