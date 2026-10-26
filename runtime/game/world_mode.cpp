// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The open-world multiplayer mode (docs/MULTIPLAYER.md, "Open world"): players share the game's open world (the dev start, game/online_cfg.cpp, boots into it) and see each
// other's cars; a marker starts a race that happens in the world. This file holds the first experiment: a vehicle made in the world outside a race, with the game's own
// opponent creation (sub_82333680, the script native the race script calls with a racer key), to be driven by a remote player's state.
#include <stdafx.h>

#include "car_select.h"
#include "mp_adapter.h"
#include "world_mode.h"
#include <algorithm>
#include <kernel/function.h>
#include <kernel/heap.h>

extern "C" PPC_FUNC(__imp__sub_82333680);

namespace
{
    constexpr uint32_t kVehicleTable = 0x82C745A8;
}

namespace
{
    // The cars of the pool: common ones, made right after the player's own car (each is the game's own racer for that car). Other cars show as the nearest free one.
    // The game makes at most seven vehicles this way (the player's and six more): the pool is the cars of the players on the server first, then these, for the players and AI
    // racers that come later (a car that is not in the pool is shown as one that is).
    const char* const kDefaults[] = { "bmwm3gtre46", "lancerevo8", "rx8", "supra", "mustanggt", "cobaltss" };
    constexpr size_t kPoolSize = 6;
    std::vector<std::string> g_plan;
    struct PoolCar { std::string name; uint32_t owner = 0; bool free = true; };
    std::vector<PoolCar> g_pool;
    size_t g_made = 0;
    int g_frames = 0;

    // The game's opponent creation (the script native a race script calls), called with a hand-made call object; the new vehicle is the owner that was not in the vehicle table.
    uint32_t MakeVehicle(PPCContext& ctx, uint8_t* base, const std::string& car)
    {
        const std::vector<uint32_t> before = game::MpWorldVehicles(base);
        uint8_t* callBlock = static_cast<uint8_t*>(g_userHeap.Alloc(512));
        memset(callBlock, 0, 512);
        const uint32_t call = uint32_t(callBlock - base);
        PPC_STORE_U32(call + 4, call + 256);   // the parameter block: empty, the racer key is the fallback
        PPCContext saved = ctx;
        ctx.r3.u64 = call;
        ctx.r4.u64 = game::RacerKeyOf(car);
        const bool wasStock = game::StockPartsFor(true);   // a pool car stands for a player: the car's stock parts
        __imp__sub_82333680(ctx, base);
        game::StockPartsFor(wasStock);
        ctx = saved;
        uint32_t owner = 0;
        for (uint32_t v : game::MpWorldVehicles(base))
            if (std::find(before.begin(), before.end(), v) == before.end())
                owner = v;
        g_userHeap.Free(callBlock);
        return owner;
    }
}

namespace game
{
    uint32_t WorldPoolAcquire(const std::string& car)
    {
        PoolCar* any = nullptr;
        for (PoolCar& p : g_pool)
        {
            if (!p.free || !p.owner) continue;
            if (p.name == car) { p.free = false; return p.owner; }
            if (!any) any = &p;
        }
        if (any) { any->free = false; return any->owner; }
        return 0;
    }
    std::string WorldPoolCar(uint32_t owner)
    {
        for (const PoolCar& p : g_pool)
            if (p.owner == owner)
                return p.name;
        return {};
    }
    void WorldPoolRelease(uint32_t owner)
    {
        for (PoolCar& p : g_pool)
            if (p.owner == owner) p.free = true;
    }

    // Called once a game frame from the game's own thread (car_select.cpp's frame hook).
    void WorldModeFrame(PPCContext& ctx, uint8_t* base)
    {
        // NFSMW_RECORD_PATH=<file>: the position of the first opponent (a race's AI driver, which follows the road) is appended to the file as "point x y z" lines, a point every
        // 30 m or more, to make the races of a server from a real lap (docs/MULTIPLAYER.md).
        static const char* record = std::getenv("NFSMW_RECORD_PATH");
        if (record)
        {
            static uint32_t lastOwner = 0;
            static float lx = 1e9f, lz = 0;
            const std::vector<uint32_t> vs = MpWorldVehicles(base);
            if (vs.size() >= 2)
                if (const uint32_t body = MpWorldPhysBody(base, vs[1]); body > 0x10000)
                {
                    auto f32 = [&](uint32_t a) { const uint32_t v = PPC_LOAD_U32(a); float f; memcpy(&f, &v, 4); return f; };
                    const float x = f32(body + 0x10), y = f32(body + 0x14), z = f32(body + 0x18);
                    if (vs[1] != lastOwner || (x - lx) * (x - lx) + (z - lz) * (z - lz) > 30.f * 30.f)
                    {
                        lastOwner = vs[1];
                        if (FILE* fp = fopen(record, "a")) { fprintf(fp, "point %.1f %.1f %.1f\n", x, y, z); fclose(fp); }
                        lx = x; lz = z;
                    }
                }
        }

        if (!MpWorldMode())
            return;
        // The pool: one vehicle every other frame from the frame after the player's own car exists, while the world is still loading.
        if (PPC_LOAD_U32(kVehicleTable + 12) == 0)
            return;
        if (g_plan.empty())
        {
            g_plan = MpWorldWantedCars();
            for (const char* d : kDefaults)
                if (g_plan.size() < kPoolSize)
                    g_plan.push_back(d);
            g_plan.resize(std::min(g_plan.size(), kPoolSize));
        }
        if (g_made < g_plan.size())
        {
            if (++g_frames % 2 == 0)
            {
                PoolCar p;
                p.name = g_plan[g_made++];
                p.owner = MakeVehicle(ctx, base, p.name);
                fprintf(stderr, "[world] pool car %s -> vehicle %08X (table count %u)\n", p.name.c_str(), p.owner, PPC_LOAD_U32(kVehicleTable + 12));
                if (p.owner)
                    MpWorldPark(p.owner);
                g_pool.push_back(p);
            }
            return;
        }
        // Players that need a car now take one from the pool.
        uint8_t id = 0;
        std::string car;
        while (MpWorldNextSpawn(id, car))
        {
            const uint32_t owner = WorldPoolAcquire(car);
            fprintf(stderr, "[world] player %u (%s) gets pool vehicle %08X\n", unsigned(id), car.c_str(), owner);
            MpWorldSpawned(id, owner);
        }
    }
}
