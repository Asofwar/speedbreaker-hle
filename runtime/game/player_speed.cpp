// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See player_speed.h.
#include <stdafx.h>
#include "player_speed.h"
#include <kernel/memory.h>

#include <chrono>
#include <cmath>
#include <cstring>

namespace
{
    constexpr uint32_t kVehicleTable = 0x82C745A8;

    uint32_t Load32(uint32_t address)
    {
        uint32_t v = __atomic_load_n(reinterpret_cast<const uint32_t*>(g_memory.base + address), __ATOMIC_RELAXED);
        return ByteSwap(v);
    }

    float LoadFloat(uint32_t address)
    {
        const uint32_t bits = Load32(address);
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

    // Where the game keeps objects: its heap, its image's data, physical memory.
    bool Plausible(uint32_t address)
    {
        if (address & 3)
            return false;
        return (address >= 0x00010000u && address < 0x7F000000u) || (address >= 0x82000000u && address < 0x82CD0000u) ||
            (address >= 0xA0000000u && address < 0xC0000000u);
    }
}

namespace game
{
    float PlayerSpeedKph()
    {
        if (!g_memory.base)
            return -1.0f;
        const uint32_t data = Load32(kVehicleTable + 4);
        const uint32_t count = Load32(kVehicleTable + 12);
        if (data != kVehicleTable + 16 || count == 0)
            return -1.0f;
        const uint32_t owner = Load32(data);
        if (!Plausible(owner))
            return -1.0f;
        const uint32_t component = Load32(owner + 128);
        if (!Plausible(component))
            return -1.0f;
        const uint32_t pointer = Load32(component + 48);
        if (!Plausible(pointer))
            return -1.0f;
        const uint32_t body = Load32(pointer);
        if (!Plausible(body))
            return -1.0f;
        const float x = LoadFloat(body + 0x20), y = LoadFloat(body + 0x24), z = LoadFloat(body + 0x28);
        const float speed = std::sqrt(x * x + y * y + z * z);
        if (!std::isfinite(speed) || speed > 150.0f)  // (540 km/h: not a car's)
            return -1.0f;
        // A paused game keeps the body's velocity but not its place: a car
        // that hasn't moved for a quarter of a second stands (its position
        // at +0x10).
        static float lastPos[3] = {};
        static auto movedAt = std::chrono::steady_clock::now();
        const float pos[3] = { LoadFloat(body + 0x10), LoadFloat(body + 0x14), LoadFloat(body + 0x18) };
        const auto now = std::chrono::steady_clock::now();
        if (memcmp(pos, lastPos, sizeof(pos)) != 0)
        {
            memcpy(lastPos, pos, sizeof(pos));
            movedAt = now;
        }
        else if (now - movedAt > std::chrono::milliseconds(250))
            return 0.0f;
        return speed * 3.6f;
    }
}
