// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See world_mode.cpp.
#pragma once
#include <cstdint>
#include <string>

namespace game
{
    // Open world mode's pool of cars: vehicles made while the world is still loading (the game asks for a car's files only then), parked until a player or an AI racer of that car
    // is shown. Returns the vehicle's owner object (for the multiplayer layer's body hook), or 0 when the pool has none free. A car that is not in the pool gets any free one.
    uint32_t WorldPoolAcquire(const std::string& car);
    void WorldPoolRelease(uint32_t owner);
}
