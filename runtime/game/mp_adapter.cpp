// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The multiplayer adapter: the only place where the multiplayer layer (net/mp_*) meets the game's own code.
//
// The game keeps every simulated rigid body in one linked list (head 0x8290BE18, links at +16 of each body object) and steps them from sub_825285C8, which
// calls sub_82528448(body, dt) for each. A body object points (+28) to its state: three 16-byte rows of the orientation matrix (x y z 0) at +0, +16, +32,
// the world position (x y z 1.0) at +48, the linear velocity at +64 and (looks like) the angular velocity at +80, big-endian floats, metres and seconds.
// The player's position is also kept in a static copy at 0x82A2DBF0 (docs/MULTIPLAYER.md, "The player's car in memory").
//
// This hook runs the game's own step first, then reads the local player's car (to send) or writes a remote player's state into the body that stands for
// that player. NFSMW_MP_GHOST=<seconds> is the test without a network: the stand-in body replays the local car as it was that many seconds ago.
#include <stdafx.h>
#include <region/region.h>

#include <kernel/function.h>
#include <kernel/heap.h>
#include "frame_time.h"
#include "mp_adapter.h"
#include "mp_hud.h"
#include "world_mode.h"
#include "ultrawide.h"
#include <gpu/renderer.h>
#include <algorithm>
#include <cstring>
#include <hid/input.h>
#include <ui/ui.h>
#include "car_select.h"
#include <net/mp_client.h>
#include <net/mp_math.h>
#include <net/multiplayer_config.h>
#include <chrono>
#include <cmath>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#endif

extern "C" PPC_FUNC(__imp__sub_82528448);
extern "C" PPC_FUNC(__imp__sub_82420FC0);

namespace
{
    // A remote player is in the race when it is ready in the lobby (its car is made for it) or is sending car states (the test bots, a player who joined late).
    bool InRace(const mp::Remote& r) { return r.lobbyReady || r.hasCar; }

    // The lobby choice of this player (set from the settings page, any thread): the car's type key and whether it is ready to race.
    std::atomic<uint64_t> g_lobbyCar{ 0 };
    std::atomic<bool> g_lobbyReady{ false };
    std::atomic<uint8_t> g_raceHost{ 0xFF };   // the player whose selected race the next race is (0xFF: none)

    // The front end's player record (*kFeRecord) and the selected race's key in it (Quick Race > Custom).
    constexpr uint32_t kFeRecord = 0x82A2C684, kFeRaceKey = 0x34;
    constexpr uint32_t kPlayerView = 0x82C3F590 + 384;   // the player camera's eView (view 1: +0x50 its view matrix)

    constexpr const char* kWaitToast = "Waiting for the other players. Press B to start without them.";
    std::mutex g_statusMutex;
    std::string g_status = "Multiplayer is off.";
    std::vector<game::MpPlayer> g_players;

    // Android 17 (API 37) keeps an app off the local network (a UDP send fails with EPERM) until the user allows "Nearby devices" (ACCESS_LOCAL_NETWORK).
    // It is asked for when multiplayer first connects, and again after another server is chosen; the connection waits for the answer. A server on the
    // internet works either way. Older systems have no such permission.
    enum : int { kLanUnasked, kLanAsking, kLanGranted, kLanDenied };
    std::atomic<int> g_lanAccess{ kLanUnasked };
    constexpr const char* kLanDeniedText = "Multiplayer may not use the local network: allow Nearby devices in Settings > Apps > SpeedBreaker > Permissions.";

    // false while the question is on screen
    bool LanAccessAnswered()
    {
#ifdef __ANDROID__
        if (g_lanAccess.load() == kLanUnasked)
        {
            if (SDL_GetAndroidSDKVersion() < 37)
                g_lanAccess = kLanGranted;
            else
            {
                g_lanAccess = kLanAsking;
                auto answered = [](void*, const char*, bool granted) {   // (any thread; at once when it was allowed before)
                    g_lanAccess = granted ? kLanGranted : kLanDenied;
                    fprintf(stderr, "[mp] local network access %s\n", granted ? "allowed" : "denied");
                };
                if (!SDL_RequestAndroidPermission("android.permission.ACCESS_LOCAL_NETWORK", answered, nullptr))
                    g_lanAccess = kLanDenied;
            }
        }
        return g_lanAccess.load() != kLanAsking;
#else
        return true;
#endif
    }

    // Chat: lines to send (from the settings page), and the latest lines received, with when (steady clock seconds) for the overlay.
    std::mutex g_chatMutex;
    std::vector<std::string> g_chatOut;
    std::deque<std::pair<double, std::string>> g_chatLog;
    double ChatNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    constexpr uint32_t kBodyListHead = 0x8290BE18;
    // The game's vehicle table: a fixed vector at 0x82C745A8 ({vtable, data, capacity in bytes, count}, data = 8-byte entries {owner object, 1}), the player first,
    // then the opponents in creation order. A vehicle's body is the body whose +8 points at that owner object.
    constexpr uint32_t kVehicleTable = 0x82C745A8;

    struct Pose
    {
        float m[3][3];     // rows
        float pos[3];
        float vel[3];
        float ang[3];
    };

    float LoadF(const uint8_t* base, uint32_t a)
    {
        uint32_t u = __builtin_bswap32(*(const volatile uint32_t*)(base + a));
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    void StoreF(uint8_t* base, uint32_t a, float f)
    {
        uint32_t u;
        memcpy(&u, &f, 4);
        *(volatile uint32_t*)(base + a) = __builtin_bswap32(u);
    }

    Pose ReadPose(const uint8_t* base, uint32_t s)
    {
        Pose p;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                p.m[r][c] = LoadF(base, s + 16 * r + 4 * c);
        for (int c = 0; c < 3; ++c)
        {
            p.pos[c] = LoadF(base, s + 48 + 4 * c);
            p.vel[c] = LoadF(base, s + 64 + 4 * c);
            p.ang[c] = LoadF(base, s + 80 + 4 * c);
        }
        return p;
    }
    void WritePose(uint8_t* base, uint32_t s, const Pose& p)
    {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                StoreF(base, s + 16 * r + 4 * c, p.m[r][c]);
        for (int c = 0; c < 3; ++c)
        {
            StoreF(base, s + 48 + 4 * c, p.pos[c]);
            StoreF(base, s + 64 + 4 * c, p.vel[c]);
            StoreF(base, s + 80 + 4 * c, p.ang[c]);
        }
    }

    // The game's real physics body of a vehicle. A body object (the tracker the list holds) points at its owner (+8); the owner's component (+128) points through a
    // pointer (+48) at the rigid body, an element of an array in static memory: orientation quaternion (x y z w) at +0, position at +0x10, linear velocity at +0x20,
    // angular velocity at +0x30, mass at +0x2C, the orientation matrix (three 16-byte rows) at +0x80. Its axes are not the tracker's: tracker (x, y, z) = (z, -x, y)
    // of the body's. The tracker the list holds is refreshed from this body by the game's step (sub_82528448), and collisions use this body, so the remote players'
    // cars are written here (docs/MULTIPLAYER.md, "The physics body").
    struct Phys
    {
        float q[4], pos[3], vel[3], ang[3];
    };

    uint32_t PhysBody(const uint8_t* base, uint32_t trackerObj)
    {
        const uint32_t owner = PPC_LOAD_U32(trackerObj + 8);
        if (!owner)
            return 0;
        const uint32_t comp = PPC_LOAD_U32(owner + 128);
        if (!comp)
            return 0;
        const uint32_t ptr = PPC_LOAD_U32(comp + 48);
        return ptr ? PPC_LOAD_U32(ptr) : 0;
    }

    Phys ReadPhys(const uint8_t* base, uint32_t b)
    {
        Phys p;
        for (int i = 0; i < 4; ++i) p.q[i] = LoadF(base, b + 4 * i);
        for (int i = 0; i < 3; ++i)
        {
            p.pos[i] = LoadF(base, b + 0x10 + 4 * i);
            p.vel[i] = LoadF(base, b + 0x20 + 4 * i);
            p.ang[i] = LoadF(base, b + 0x30 + 4 * i);
        }
        return p;
    }

    void WritePhys(uint8_t* base, uint32_t b, const Phys& p)
    {
        float m[3][3];
        mp::QuatToMat(p.q, m);
        for (int i = 0; i < 4; ++i) StoreF(base, b + 4 * i, p.q[i]);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                StoreF(base, b + 0x80 + 16 * r + 4 * c, m[r][c]);
        for (int i = 0; i < 3; ++i)
        {
            StoreF(base, b + 0x10 + 4 * i, p.pos[i]);
            StoreF(base, b + 0x20 + 4 * i, p.vel[i]);
            StoreF(base, b + 0x30 + 4 * i, p.ang[i]);
        }
    }

    float Dist(const float* a, const float* b)
    {
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    }

    // A player's name as the game lists its racers: capital letters, digits, space, '-' and '_', ten characters at most (the racer records keep 11 bytes).
    std::string RacerText(const std::string& name)
    {
        std::string t;
        for (char ch : name)
        {
            if (ch >= 'a' && ch <= 'z') ch = char(ch - 32);
            if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == ' ' || ch == '-' || ch == '_')
                t += ch;
            if (t.size() == 10) break;
        }
        return t;
    }

    struct Adapter
    {
        bool ghost = false;
        float ghostSeconds = 5.f;
        float ghostOffset = 0.f;            // NFSMW_MP_OFFSET: metres along the car's first matrix row, to see the stand-in beside the car
        int ghostBody = -1;                 // NFSMW_MP_BODY: the n-th body of the list stands in for the remote car
        uint32_t localObj = 0;
        uint32_t standInObj = 0;
        std::deque<std::pair<double, Pose>> history;
        // Network (NFSMW_MP_SERVER=host[:port], NFSMW_MP_NAME): the local car goes out at 30 Hz, each remote player drives one stand-in body.
        std::unique_ptr<mp::Client> net;
        std::string server, name = "player";
        double lastSend = 0, lastTry = 0, lastStatus = -10;
        bool lanDeniedShown = false;
        double lastSkins = 0;
        uint32_t sentSkinAt = 0;   // the address of the skin this game sent last (a new one, a new race: sent again)
        // Start barrier (NFSMW_MP_PLAYERS=n, n > 1): the race is held, world frozen, from the moment it appears until n-1 other players are there and ready too.
        double lastHud = 0;
        bool world = mpcfg::Load().world;   // open-world mode: the others are vehicles made in the world, no race start barrier
        std::set<uint32_t> parked;        // pool vehicles held out of the way (world mode)
        std::set<uint8_t> worldPending;   // players whose vehicle is being made
        size_t nameCalls = 0;             // racer names picked so far in this race (sub_82335248)
        std::vector<uint8_t> roster;      // the players (ids, ascending) the opponents of this race were made for
        std::string cfgCar;               // the lobby car from the settings file (by name; its key is looked up once the catalogue exists)
        int expected = 0;                 // the number of players from the settings (used when this player is not ready in the lobby)
        int barrierExpected = 0;          // players the barrier waits for in this race
        bool pushedReady = false;
        uint64_t pushedCar = 0;
        uint32_t pushedRace = 0;
        uint32_t ownRace = 0;      // the race this game's front end has selected (when no other player's is held there)
        uint32_t heldRace = 0;     // the host's race, held in this game's front end (0: none)
        // How many players the next race has: this one plus the others that are ready in the lobby, or the number from the settings when this player is not ready.
        int Expected() const
        {
            if (!g_lobbyReady.load() || !net)
                return expected;
            int n = 1;
            for (const auto& [id, r] : net->Remotes())
                if (r.lobbyReady)
                    ++n;
            return n;
        }
        bool barrier = false;
        bool gridLogged = false;
        uint64_t myCarKey = 0;        // the car this player's vehicle was made (announced to the others)
        bool fewShown = false;        // this race: told the player that there are fewer opponent cars than other players
        bool mismatchShown = false;   // this race: the other player was seen in a different race
        double emptySince = 0;
        int raceEpoch = 0;   // counts new races and finished races: the grid animation's vehicle list starts over with each
        bool barrierPending = false;      // a new race: the barrier is armed when the opponents exist
        double barrierPendingSince = 0;
        double barrierSince = 0;
        bool holdSent = false;            // the server has been told this game is held (Hold), and is deciding the release
        bool goPending = false;           // the server released the start: this game starts at goStart (plus the clock difference to the slowest game)
        double goStart = 0;
        float goRef = 0;
        // The agreed race clock after the start: every game aims at refClock + real seconds since refTime (game/frame_time.cpp nudges the world's speed to follow it),
        // so the clocks stay together whatever each machine's loading did to its game time. refClock < 0: not in a synchronised race.
        double refClock = -1, refTime = 0;
        double readyUntil = 0;   // the ready flag is kept on for a while after the release: a game that released at once (the others were ready) must still be seen as ready
        double localSince = 0, connectedAt = 0, lastTable = -1;
        std::map<uint8_t, uint32_t> standIns;   // remote player id -> body object
        double lastLog = -10;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

        Adapter()
        {
            Reload();
            if (const char* g = std::getenv("NFSMW_MP_GHOST")) { ghost = true; ghostSeconds = float(atof(g)); }
            if (const char* b = std::getenv("NFSMW_MP_BODY")) ghostBody = atoi(b);
            if (const char* o = std::getenv("NFSMW_MP_OFFSET")) ghostOffset = float(atof(o));
        }
        // The settings (Settings > Online > Multiplayer, or the NFSMW_MP_* variables), read again every couple of seconds so a change applies to the next race.
        double lastCfg = -10;
        void Reload()
        {
            const mpcfg::Config c = mpcfg::Load();
            const std::string wanted = c.enabled ? c.server : std::string();
            if (wanted != server)
            {
                server = wanted;
                if (g_lanAccess.load() == kLanDenied)   // another server: ask again
                {
                    g_lanAccess = kLanUnasked;
                    lanDeniedShown = false;
                }
                if (net)
                {
                    net->Disconnect();
                    net.reset();
                }
                standIns.clear();
            }
            name = c.name;
            world = c.world;
            expected = c.players;
            cfgCar = c.car;
        }
        double Now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
    };

    Adapter& A()
    {
        static Adapter a;
        return a;
    }

    // The bodies of the list in order (links are at +16, objects start 16 before the link; the list is circular through a sentinel at 0x8290BE08).
    std::vector<uint32_t> Bodies(const uint8_t* base)
    {
        std::vector<uint32_t> v;
        const uint32_t sentinel = kBodyListHead - 16;
        uint32_t obj = PPC_LOAD_U32(kBodyListHead) - 16;
        while (obj && obj != sentinel && v.size() < 256)
        {
            v.push_back(obj);
            obj = PPC_LOAD_U32(obj + 16) - 16;
        }
        return v;
    }

    // Bodies of the racing vehicles (player first), 0 where a vehicle's body is not (yet) in the list.
    std::vector<uint32_t> VehicleBodies(const uint8_t* base)
    {
        std::vector<uint32_t> out;
        const uint32_t data = PPC_LOAD_U32(kVehicleTable + 4);
        const uint32_t count = PPC_LOAD_U32(kVehicleTable + 12);
        if (data != kVehicleTable + 16)
            return out;
        // The count word grows with the traffic (29 was seen about a lap into a race) while the room in the table is 20 entries: read at most 20. The player and the
        // opponents are the first entries; what comes after is traffic.
        const std::vector<uint32_t> bodies = Bodies(base);
        for (uint32_t i = 0; i < std::min<uint32_t>(count, 20); ++i)
        {
            const uint32_t owner = PPC_LOAD_U32(data + 8 * i);
            uint32_t body = 0;
            for (uint32_t b : bodies)
                if (PPC_LOAD_U32(b + 8) == owner) { body = b; break; }
            out.push_back(body);
        }
        return out;
    }
}

namespace
{
    // If `obj` stands in for a remote player, write that player's state into it. Done before the game's step as well as after it: the game finds contacts
    // from the pose it starts the step with, so the remote car is where its player is when it is hit, and the step's own drift is undone afterwards.
    void ApplyRemote(Adapter& a, uint8_t* base, uint32_t obj)
    {
        // NFSMW_MP_TELEPORT="x,y,z": the local car is put there once, five seconds into the world (a test: to the start of a race marker without driving there).
        static const char* tp = std::getenv("NFSMW_MP_TELEPORT");
        static bool tpDone = false;
        if (tp && !tpDone && a.localObj && obj == a.localObj && a.Now() - a.localSince > 5.0)
            if (const uint32_t pb = PhysBody(base, obj))
            {
                float x, y, z;
                if (sscanf(tp, "%f,%f,%f", &x, &y, &z) == 3)
                {
                    Phys ph = ReadPhys(base, pb);
                    ph.pos[0] = x; ph.pos[1] = y; ph.pos[2] = z;
                    ph.vel[0] = ph.vel[1] = ph.vel[2] = 0;
                    WritePhys(base, pb, ph);
                    fprintf(stderr, "[mp] teleported the local car to %.1f %.1f %.1f\n", double(x), double(y), double(z));
                }
                tpDone = true;
            }
        // The open world: every game's dev start puts its car on the same spot (body axes -2148.4, 153, 981.9, facing a house across the road), so two players
        // started inside each other and their collision threw them off. Three seconds in, once, a car still there moves along the road (body +z there) by its rank
        // among the players' ids on the server: 0, +10 m, -10 m, +20 m, ...
        static bool spreadDone = false;
        if (a.world && !spreadDone && a.localObj && obj == a.localObj && a.net && a.net->Connected() && a.Now() - a.localSince > 3.0)
            if (const uint32_t pb = PhysBody(base, obj))
            {
                spreadDone = true;
                int rank = 0;
                for (const auto& [id, r] : a.net->Remotes())
                    if (id < a.net->MyId())
                        ++rank;
                Phys ph = ReadPhys(base, pb);
                const float dx = ph.pos[0] + 2148.4f, dz = ph.pos[2] - 981.9f;
                if (rank > 0 && dx * dx + dz * dz < 30.f * 30.f)
                {
                    const float along = 10.f * float((rank + 1) / 2) * (rank % 2 ? 1.f : -1.f);
                    ph.pos[2] += along;
                    ph.vel[0] = ph.vel[1] = ph.vel[2] = 0;
                    WritePhys(base, pb, ph);
                    fprintf(stderr, "[mp] spawn spread: player rank %d, the car moved %+.0f m along the road\n", rank, double(along));
                }
            }
        if (a.parked.count(obj))
        {
            if (const uint32_t pb = PhysBody(base, obj))
            {
                Phys ph = ReadPhys(base, pb);
                ph.pos[0] = 0; ph.pos[1] = -600.f; ph.pos[2] = 0;
                ph.vel[0] = ph.vel[1] = ph.vel[2] = 0;
                ph.ang[0] = ph.ang[1] = ph.ang[2] = 0;
                WritePhys(base, pb, ph);
            }
            return;
        }
        if (!a.net || !a.net->Connected())
            return;
        for (const auto& [id, body] : a.standIns)
        {
            if (body != obj)
                continue;
            const uint32_t pb = PhysBody(base, obj);
            mp::CarState c;
            if (!pb || !a.net->RemoteCar(id, a.net->NowMs(), c))
                return;
            // NFSMW_MP_TRACE_REMOTE=1: the pose each remote car is given, at most every 50 ms (tests: how smooth it stays with delay and loss).
            static const bool traceRemote = std::getenv("NFSMW_MP_TRACE_REMOTE") != nullptr;
            if (traceRemote)
            {
                static std::map<uint8_t, double> lastTrace;
                if (a.Now() - lastTrace[id] >= 0.05)
                {
                    lastTrace[id] = a.Now();
                    fprintf(stderr, "[mptrace] %.3f %u %.2f %.2f %.2f %.2f\n", a.Now(), unsigned(id), double(c.pos[0]), double(c.pos[1]), double(c.pos[2]),
                        double(std::sqrt(c.vel[0] * c.vel[0] + c.vel[1] * c.vel[1] + c.vel[2] * c.vel[2])));
                }
            }
            Phys out;
            for (int i = 0; i < 4; ++i) out.q[i] = c.rot[i];
            for (int i = 0; i < 3; ++i) { out.pos[i] = c.pos[i]; out.vel[i] = c.vel[i]; out.ang[i] = c.angVel[i]; }
            WritePhys(base, pb, out);
            return;
        }
    }
}

namespace
{
    std::vector<std::string> ParseHostPort(const std::string& hostPort, uint16_t& port)
    {
        std::string host = hostPort;
        port = mp::kDefaultPort;
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos) { port = uint16_t(atoi(host.c_str() + colon + 1)); host.resize(colon); }
        return { host };
    }

    // The multiplayer layer's once-a-frame work, from MpFramePump (so it also runs while the world is held, when no body is stepped): follow the races
    // (the local player's body, new race / race over, the start barrier), keep the connection (only while a race is on), send the local car, and hand the
    // remote players their opponent bodies.
    std::string RaceClock(uint32_t ms)
    {
        char b[32];
        snprintf(b, sizeof b, "%u:%02u.%u", ms / 60000, (ms / 1000) % 60, (ms / 100) % 10);
        return b;
    }

    // The open world race for the overlay (ui/mp_overlay.cpp): what the server says about the race, with this player's place in it and the markers to drive to.
    void BuildHud(Adapter& a, uint8_t* base, double now)
    {
        if (now - a.lastHud < 0.05)
            return;
        a.lastHud = now;
        mp::Client* net = a.net.get();
        if (!net || !net->Connected())
        {
            game::MpHudPublish({});
            return;
        }
        game::MpHud h;
        {
            // The chat lines of the last 15 s (at most 6).
            std::lock_guard<std::mutex> g(g_chatMutex);
            const double t = ChatNow();
            for (const auto& [at, line] : g_chatLog)
                if (t - at < 15.0)
                    h.chat.push_back(line);
            if (h.chat.size() > 6)
                h.chat.erase(h.chat.begin(), h.chat.end() - 6);
        }
        if (!a.localObj)
        {
            game::MpHudPublish(std::move(h));
            return;
        }
        h.camValid = true;
        for (int i = 0; i < 3; ++i) { h.eye[i] = LoadF(base, 0x82A2DAC0 + 4 * i); h.fwd[i] = LoadF(base, 0x82A2DAE0 + 4 * i); }
        game::PlayerProjection(h.projX, h.projY);
        // The other players' names over the cars that stand for them (a race's opponents, the open world's vehicles), where they are drawn.
        for (const auto& [id, obj] : a.standIns)
        {
            const uint32_t pb = PhysBody(base, obj);
            const auto it = net->Remotes().find(id);
            if (!pb || it == net->Remotes().end())
                continue;
            const Phys ph = ReadPhys(base, pb);
            game::MpHudTag t;
            for (int i = 0; i < 3; ++i) t.p[i] = ph.pos[i];
            t.p[1] += 1.3f;
            t.name = it->second.name.empty() ? "player " + std::to_string(id) : it->second.name;
            h.tags.push_back(std::move(t));
        }
        const std::vector<mp::RaceDef> races = net->Races();
        if (races.empty())
        {
            game::MpHudPublish(std::move(h));
            return;
        }
        const mp::RaceView v = net->CurrentRace();
        const std::vector<mp::RaceRecord> records = net->Records();
        auto recordOf = [&](size_t race) -> std::string {
            if (race >= records.size() || !records[race].ms)
                return "no record yet";
            return "record " + mp::FormatRaceTime(records[race].ms) + " by " + records[race].name;
        };
        const uint8_t me = net->MyId();
        auto nameOf = [&](uint8_t id) -> std::string {
            if (id == me)
                return a.name;
            const auto it = net->Remotes().find(id);
            return it == net->Remotes().end() || it->second.name.empty() ? "player " + std::to_string(id) : it->second.name;
        };
        float mine[3] = {};
        if (const uint32_t pb = PhysBody(base, a.localObj))
        {
            const Phys ph = ReadPhys(base, pb);
            for (int i = 0; i < 3; ++i) mine[i] = ph.pos[i];
        }
        auto distTo = [&](const mp::Vec3& p) { const float dx = p.x - mine[0], dz = p.z - mine[2]; return std::sqrt(dx * dx + dz * dz); };

        const mp::RacerView* my = nullptr;
        for (const mp::RacerView& r : v.racers)
            if (r.id == me) my = &r;

        if (v.phase == mp::RacePhase::Idle || v.race < 0 || size_t(v.race) >= races.size())
        {
            // No race: the start markers and how far the nearest is.
            float best = 1e9f;
            std::string bestName;
            size_t bestIndex = 0;
            for (const mp::RaceDef& d : races)
            {
                game::MpHudMarker m;
                m.p[0] = d.points[0].x; m.p[1] = d.points[0].y; m.p[2] = d.points[0].z;
                m.label = "START " + d.name;
                m.kind = 0;
                h.markers.push_back(m);
                const float dd = distTo(d.points[0]);
                if (dd < best) { best = dd; bestName = d.name; bestIndex = size_t(&d - races.data()); }
            }
            char b[200];
            snprintf(b, sizeof b, "Race %s: start marker %.0f m away. Stop inside it to start a race (%s).", bestName.c_str(), double(best), recordOf(bestIndex).c_str());
            h.lines.push_back(b);
        }
        else
        {
            const mp::RaceDef& d = races[size_t(v.race)];
            // Where a racer is: its lap on a circuit, its checkpoint on a sprint.
            auto progressOf = [&](const mp::RacerView& r) {
                if (d.sprint)
                    return "checkpoint " + std::to_string(std::min<int>(r.next, int(d.points.size()) - 1)) + "/" + std::to_string(d.points.size() - 1);
                return "lap " + std::to_string(std::min<int>(r.lap + 1, d.laps)) + "/" + std::to_string(d.laps);
            };
            auto addStart = [&] {
                game::MpHudMarker m;
                m.p[0] = d.points[0].x; m.p[1] = d.points[0].y; m.p[2] = d.points[0].z;
                m.label = "START " + d.name;
                m.kind = 0;
                h.markers.push_back(m);
            };
            switch (v.phase)
            {
            case mp::RacePhase::Gathering:
            {
                addStart();
                char b[200];
                snprintf(b, sizeof b, "Race %s opens in %u s: stop in the marker to join (%zu in). %s.", d.name.c_str(), (v.timeMs + 999) / 1000, v.racers.size(),
                    recordOf(size_t(v.race)).c_str());
                h.lines.push_back(b);
                if (!my) h.banner = "";
                break;
            }
            case mp::RacePhase::Countdown:
                addStart();
                h.banner = my ? std::to_string((v.timeMs + 999) / 1000) : std::string();
                h.lines.push_back(my ? "Get ready. The race starts at 0." : "A race is about to start.");
                break;
            default:
            {
                if (v.phase == mp::RacePhase::Running && v.timeMs < 1500 && my) h.banner = "GO!";
                if (v.phase == mp::RacePhase::Finished) h.banner = my && my->finished ? "FINISHED" : "";
                char b[200];
                if (my && v.phase == mp::RacePhase::Running && !my->finished)
                {
                    snprintf(b, sizeof b, "%s  %s  place %d/%zu  %s  (%s)", d.name.c_str(), progressOf(*my).c_str(), my->place, v.racers.size(), RaceClock(v.timeMs).c_str(), recordOf(size_t(v.race)).c_str());
                    h.lines.push_back(b);
                    const int n = int(d.points.size());
                    for (int k = 0; k < 2; ++k)
                    {
                        if (d.sprint && my->next + k >= n)
                            break;   // (nothing after a sprint's finish)
                        const int idx = (my->next + k) % n;
                        game::MpHudMarker m;
                        m.p[0] = d.points[size_t(idx)].x; m.p[1] = d.points[size_t(idx)].y; m.p[2] = d.points[size_t(idx)].z;
                        m.label = (d.sprint ? idx == n - 1 : idx == 0) ? "FINISH" : "CP " + std::to_string(idx);
                        m.kind = k == 0 ? 1 : 2;
                        h.markers.push_back(m);
                    }
                }
                else
                    h.lines.push_back(v.phase == mp::RacePhase::Finished ? "Race over (" + recordOf(size_t(v.race)) + "). Results:" : "A race is in progress. Racers:");
                for (const mp::RacerView& r : v.racers)
                {
                    if (r.finished)
                        snprintf(b, sizeof b, "%d. %s  %s", r.place, nameOf(r.id).c_str(), RaceClock(r.finishMs).c_str());
                    else
                        snprintf(b, sizeof b, "%d. %s  %s", r.place, nameOf(r.id).c_str(), progressOf(r).c_str());
                    h.lines.push_back(b);
                }
                break;
            }
            }
        }
        if (std::getenv("NFSMW_MP_HUD_LOG"))
        {
            static std::string last;
            std::string now2 = h.banner;
            for (const auto& l : h.lines) now2 += "|" + l;
            if (now2 != last) { last = now2; fprintf(stderr, "[hud] banner \"%s\", %zu lines, %zu markers: %s\n", h.banner.c_str(), h.lines.size(), h.markers.size(), now2.c_str()); for (const auto& rv : v.racers) fprintf(stderr, "[hud]   racer %u lap %u next %u place %u finish %u\n", unsigned(rv.id), unsigned(rv.lap), unsigned(rv.next), unsigned(rv.place), rv.finishMs); }
        }
        game::MpHudPublish(std::move(h));
    }

    void Tick(Adapter& a, uint8_t* base, double now)
    {
        if (a.server.empty() && !a.ghost)
            return;

        // The local player's body: the first vehicle of the game's vehicle table (the player is always first), looked up every frame so a new race (new bodies, or the
        // same addresses after the vehicles were gone) is noticed at once, and not by position, because a stand-in can overlap the car.
        {
            const std::vector<uint32_t> vb = VehicleBodies(base);
            const uint32_t first = vb.empty() ? 0 : vb[0];
            // The race is over when the vehicle table has been empty for a while: it can be empty for a moment inside a race (the game rebuilds it), and a
            // false "new race" would hold the start again in the middle of a race.
            if (first)
                a.emptySince = 0;
            else if (a.emptySince == 0)
                a.emptySince = now;
            if (!first && a.localObj && now - a.emptySince > 2.5)
            {
                fprintf(stderr, "[mp] %.2f s race over (the vehicle table, %08X count %u, has been empty since %.2f s)\n", now, PPC_LOAD_U32(kVehicleTable + 4), PPC_LOAD_U32(kVehicleTable + 12), a.emptySince);
                a.refClock = -1;
                ++a.raceEpoch;
                a.localObj = 0;
                a.standIns.clear();
                a.standInObj = 0;
                a.myCarKey = 0;
                a.barrierPending = false;
                g_lobbyReady = false;   // the next race needs the players to say they are ready again
                game::ResetOpponentCount();
                game::SetOpponentCars({});
                if (a.barrier)
                {
                    a.barrier = false;
                    if (a.net)
                        a.net->SendUnhold();
                    a.holdSent = a.goPending = false;
                    game::SetWorldHold(false);
                    ui::WithdrawToast(kWaitToast);
                }
            }
            if (first && first != a.localObj)
            {
                // A new race: forget the old bodies (the stand-ins are looked up again) and hold the start if others are expected.
                fprintf(stderr, "[mp] %.2f s local car body is now %08X\n", now, first);
                ++a.raceEpoch;
                a.refClock = -1;
                a.gridLogged = false;
                a.mismatchShown = false;
                a.fewShown = false;
                a.localObj = first;
                a.standIns.clear();
                a.standInObj = 0;
                a.history.clear();
                // The start barrier (a held world) is armed once the race script has created its opponents (see below): a world held before that would create them after the
                // loading phase, and their models would never appear.
                a.barrierPending = !a.world && a.Expected() > 1 && !a.server.empty();
                a.barrierPendingSince = now;
                a.localSince = now;
            }
        }

        // Arm the start barrier once the race script has finished creating the opponents (none created for 0.5 s after the first). None within 5 s: not a race the
        // others can be in (they drive its opponents), such as free roam (career, the dev start), so nothing is held: holding it froze free roam until B.
        if (a.barrierPending && a.localObj)
        {
            const int made = game::OpponentsCreated();
            if (made == 0 && now - a.barrierPendingSince > 5.0)
            {
                a.barrierPending = false;
                fprintf(stderr, "[mp] no opponents 5 s after the player's car: free roam or a race without opponents, not held for the others\n");
            }
            else if (made > 0 && game::SecondsSinceLastOpponent() > 0.5)
            {
                a.barrierPending = false;
                a.barrier = true;
                a.barrierExpected = a.Expected();
                a.barrierSince = now;
                a.holdSent = a.goPending = false;
                a.readyUntil = now + 20.0;
                game::SetWorldHold(true);
                fprintf(stderr, "[mp] start barrier: race held until %d players are ready (%d opponents made)\n", a.barrierExpected, made);
                ui::Toast(kWaitToast, 600.0);
            }
        }
        if (a.server.empty())
        {
            // Only the ghost test: no connection.
            if (a.net)
            {
                a.net->Disconnect();
                a.net.reset();
                a.standIns.clear();
            }
            return;
        }

        if (!LanAccessAnswered())
            return;
        if (g_lanAccess.load() == kLanDenied && !a.lanDeniedShown)
        {
            a.lanDeniedShown = true;
            ui::Toast(kLanDeniedText, 20.0);
        }

        // The connection: made as soon as multiplayer is on and kept between races, so that the players present and their cars are known before a race is created.
        uint16_t port;
        if (!a.net)
        {
            a.net.reset(new mp::Client);
            a.net->onChat = [&a](uint8_t id, const std::string& text) {
                std::string who = id == mp::kServerId ? std::string("server") : id == a.net->MyId() ? a.name : std::string();
                if (who.empty())
                {
                    const auto it = a.net->Remotes().find(id);
                    who = it == a.net->Remotes().end() || it->second.name.empty() ? "player " + std::to_string(id) : it->second.name;
                }
                fprintf(stderr, "[mp] chat %s: %s\n", who.c_str(), text.c_str());
                std::lock_guard<std::mutex> g(g_chatMutex);
                g_chatLog.emplace_back(ChatNow(), who + ": " + text);
                while (g_chatLog.size() > 50)
                    g_chatLog.pop_front();
            };
            const std::string host = ParseHostPort(a.server, port)[0];
            if (!a.net->Connect(host, port, a.name))
                fprintf(stderr, "[mp] cannot connect to %s\n", a.server.c_str());
            else
                fprintf(stderr, "[mp] connecting to %s:%u as %s\n", host.c_str(), port, a.name.c_str());
        }
        a.net->Service();
        if (a.net->Failed() && now - a.lastTry > 5.0)
        {
            // The server went away or refused us: try again every few seconds (the stand-ins are looked up again once it is back).
            a.lastTry = now;
            a.standIns.clear();
            const std::string host = ParseHostPort(a.server, port)[0];
            a.net->Disconnect();
            a.net->Connect(host, port, a.name);
            fprintf(stderr, "[mp] connection lost, reconnecting to %s\n", a.server.c_str());
        }
        if (a.net->Connected())
        {
            // NFSMW_TEST_CHAT=<text>: a test: that chat line once, as the settings page sends one.
            static const char* testChat = std::getenv("NFSMW_TEST_CHAT");
            if (testChat && *testChat)
            {
                game::MpSendChat(testChat);
                testChat = nullptr;
            }
            std::vector<std::string> out;
            {
                std::lock_guard<std::mutex> g(g_chatMutex);
                out.swap(g_chatOut);
            }
            for (const std::string& line : out)
                a.net->SendChat(line);
        }
        // The race (Quick Race > Custom): the front end's player record (*0x82A2C684) keeps the selected race's key at +0x34 (checked: Ironwood BCDB34C4, Campus Way
        // 910968AF; a key held there while another race is selected in the menu makes the race the held one). The host is the ready player with the smallest id that
        // has selected one; every other ready game holds the host's key there until its race exists, so all of them race the host's choice.
        if (a.net->Connected() && !a.world && !a.localObj)
        {
            auto load = [&](uint32_t at) { return PPC_LOAD_U32(at); };
            const uint32_t rec = load(kFeRecord);
            if (rec > 0x80000000 && rec < 0xD0000000)
            {
                const uint32_t sel = load(rec + kFeRaceKey);
                if (sel != a.heldRace)
                    a.ownRace = sel;   // (the player chose something else since, or nothing is held)
                uint8_t hostId = 0xFF;
                uint32_t hostRace = 0;
                if (g_lobbyReady.load() && a.ownRace)
                {
                    hostId = a.net->MyId();
                    hostRace = a.ownRace;
                }
                for (const auto& [id, r] : a.net->Remotes())
                    if (r.lobbyReady && r.raceKey && id < hostId)
                    {
                        hostId = id;
                        hostRace = r.raceKey;
                    }
                const bool hold = g_lobbyReady.load() && hostId != a.net->MyId() && hostRace;
                if (hold && sel != hostRace)
                {
                    PPC_STORE_U32(rec + kFeRaceKey, hostRace);
                    if (a.heldRace != hostRace)
                        fprintf(stderr, "[mp] the race is player %u's (race key %08X); this game had selected %08X\n", unsigned(hostId), hostRace, a.ownRace);
                }
                a.heldRace = hold ? hostRace : 0;
                g_raceHost = hold ? hostId : g_lobbyReady.load() && a.ownRace ? a.net->MyId() : 0xFF;
            }
        }
        const uint32_t myRace = g_lobbyReady.load() ? a.ownRace : 0;
        if (a.net->Connected() && (a.pushedReady != g_lobbyReady.load() || a.pushedCar != g_lobbyCar.load() || a.pushedRace != myRace))
        {
            a.pushedReady = g_lobbyReady.load();
            a.pushedCar = g_lobbyCar.load();
            a.pushedRace = myRace;
            a.net->SetLobby(a.pushedCar, a.pushedReady, a.pushedRace);
        }
        if (!a.localObj)
            return;

        // The cars' skins, their paint and vinyl (docs/MULTIPLAYER.md, "Same paint"): an opponent or pool car of a player's model comes with the game's random
        // paint on every machine. Twice a second the skins the renderer saw drawn (gpu::renderer::CarSkins: where each draw put its car, in view space) are matched
        // to the cars by putting their bodies through the player camera's view matrix; this game's own car's skin goes to the server once (it keeps it for the
        // others), and a skin another player sent is written over the skin of the car that stands for that player, the same model's same texture (format and
        // size checked), again whenever the game has put its own back. NFSMW_MP_SKINS=0: off.
        static const bool skinsOn = [] { const char* v = std::getenv("NFSMW_MP_SKINS"); return !(v && v[0] == '0'); }();
        if (!a.net->Connected())
            a.sentSkinAt = 0;   // (a new connection: the server has nothing of this game's)
        if (skinsOn && a.net->Connected() && now - a.lastSkins >= 0.5)
        {
            a.lastSkins = now;
            const std::vector<gpu::renderer::CarSkin> skins = gpu::renderer::CarSkins();
            const uint64_t frame = gpu::renderer::Frame();
            float view[16];
            for (int k = 0; k < 16; ++k)
                view[k] = LoadF(base, kPlayerView + 0x50 + 4 * k);   // (rows; the translation in the last)
            auto skinOf = [&](uint32_t obj) -> const gpu::renderer::CarSkin* {
                const uint32_t pb = PhysBody(base, obj);
                if (!pb)
                    return nullptr;
                const Phys ph = ReadPhys(base, pb);
                const float t[3] = { ph.pos[2], -ph.pos[0], ph.pos[1] };   // tracker axes, as the view matrix takes them
                float v[3];
                for (int i = 0; i < 3; ++i)
                    v[i] = t[0] * view[i] + t[1] * view[4 + i] + t[2] * view[8 + i] + view[12 + i];
                const gpu::renderer::CarSkin* best = nullptr;
                float bestD = 3.f * 3.f;   // (the draw's origin is the model's, a metre or so from the body's)
                for (const auto& s : skins)
                {
                    if (s.frame + 30 < frame)
                        continue;   // (not drawn in the last half second: the view has moved on)
                    const float dx = s.view[0] - v[0], dz = s.view[2] - v[2];
                    if (dx * dx + dz * dz < bestD)
                    {
                        bestD = dx * dx + dz * dz;
                        best = &s;
                    }
                }
                return best;
            };
            auto bytesOf = [](const gpu::renderer::CarSkin& s, uint32_t& base_, uint32_t& mips) {
                base_ = 1024 * 1024 * 4;   // (8_8_8_8, the 1024x1024 tiles exactly)
                mips = 0;   // (the base level only: the mips' packed layout isn't known, so they are never written; a far car may keep its own colours)
                (void)s;
            };
            auto guest = [&](uint32_t physical) { return base + 0xA0000000u + physical; };   // (the CPU's view: the write watch sees the stores)
            if (const auto* own = skinOf(a.localObj); own && own->address != a.sentSkinAt)
            {
                mp::SkinData d;
                d.fetch1 = own->fetch1;
                d.fetch2 = own->fetch2;
                bytesOf(*own, d.baseBytes, d.mipBytes);
                d.bytes.resize(size_t(d.baseBytes) + d.mipBytes);
                memcpy(d.bytes.data(), guest(own->address), d.baseBytes);
                if (d.mipBytes)
                    memcpy(d.bytes.data() + d.baseBytes, guest(own->mipAddress), d.mipBytes);
                a.net->SendSkin(d);
                a.sentSkinAt = own->address;
                fprintf(stderr, "[mp] this car's skin (%08X, %zu KB) sent to the server\n", own->address, d.bytes.size() / 1024);
            }
            for (const auto& [id, obj] : a.standIns)
            {
                const auto it = a.net->Remotes().find(id);
                if (it == a.net->Remotes().end() || !it->second.skin)
                    continue;
                const mp::SkinData& d = *it->second.skin;
                // Only over a car of the owner's model (another model's skin has another layout): in the open world a pool car made as that car, in a race one of the
                // roster's (whose opponents were made as the players' cars; a player that came later drives whatever opponent was left).
                if (a.world)
                {
                    std::string want;
                    for (const auto& c : game::CarCatalog())
                        if (c.key == it->second.carKey)
                            want = c.name;
                    for (char& ch : want)
                        if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
                    if (want.empty() || game::WorldPoolCar(obj) != want)
                        continue;
                }
                else if (std::find(a.roster.begin(), a.roster.end(), id) == a.roster.end())
                    continue;
                const auto* s = skinOf(obj);
                if (!s)
                    continue;
                uint32_t baseBytes, mips;
                bytesOf(*s, baseBytes, mips);
                if ((s->fetch1 & 0x3F) != (d.fetch1 & 0x3F) || s->fetch2 != d.fetch2 || baseBytes != d.baseBytes || mips != d.mipBytes)
                    continue;   // (another model or texture: never written over)
                // Already the owner's? (Three samples of it.)
                bool same = true;
                for (uint32_t at : { 0u, d.baseBytes / 2, d.baseBytes - 256 })
                    same = same && memcmp(guest(s->address) + at, d.bytes.data() + at, 256) == 0;
                if (same)
                    continue;
                memcpy(guest(s->address), d.bytes.data(), d.baseBytes);
                if (d.mipBytes)
                    memcpy(guest(s->mipAddress), d.bytes.data() + d.baseBytes, d.mipBytes);
                fprintf(stderr, "[mp] player %u's car skin written over %08X (%zu KB)\n", unsigned(id), s->address, d.bytes.size() / 1024);
            }
        }

        // The local car, 30 times a second, with the ready flag while the start is held.
        if (a.net->Connected() && now - a.lastSend >= 1.0 / 30.0)
        {
            if (const uint32_t pb = PhysBody(base, a.localObj))
            {
                a.lastSend = now;
                const Phys ph = ReadPhys(base, pb);
                mp::CarState c;
                for (int i = 0; i < 4; ++i) c.rot[i] = ph.q[i];
                for (int i = 0; i < 3; ++i) { c.pos[i] = ph.pos[i]; c.vel[i] = ph.vel[i]; c.angVel[i] = ph.ang[i]; }
                c.flags = (a.barrier || now < a.readyUntil) ? mp::kFlagReady : 0;
                c.raceClock = LoadF(base, 0x82A153B8);
                a.net->SendCar(c);
            }
        }

        // Each remote player drives one opponent vehicle's body, in the order of the game's vehicle table (player first). NFSMW_MP_BODIES=i,j,... picks list
        // positions by hand instead (the log of the step hook shows the list).
        size_t inRaceCount = 0;
        for (const auto& [id, r] : a.net->Remotes())
            if (InRace(r))
                ++inRaceCount;
        if (!a.world && a.net->Connected() && a.standIns.size() < inRaceCount)
        {
            static const std::vector<int> picks = [] {
                std::vector<int> v;
                if (const char* e = std::getenv("NFSMW_MP_BODIES"))
                    for (const char* c = e; *c;) { v.push_back(atoi(c)); while (*c && *c != ',') ++c; if (*c) ++c; }
                return v;
            }();
            // Opponent k (the k-th vehicle after the player in the game's vehicle table) stands for the k-th player in the race. If that vehicle's body is not in the list yet
            // it is waited for: skipping it would give the next player's car to the wrong vehicle (which then has another model).
            std::vector<uint32_t> slots;
            if (picks.empty())
            {
                const std::vector<uint32_t> vb = VehicleBodies(base);
                for (size_t i = 1; i < vb.size(); ++i)
                    slots.push_back(vb[i]);
            }
            else
            {
                const std::vector<uint32_t> bodies = Bodies(base);
                for (int p : picks)
                    slots.push_back(size_t(p) < bodies.size() ? bodies[p] : 0);
            }
            const std::vector<uint32_t>& candidates = slots;
            // The order the opponents were made in is the order of the roster fixed when the race was created (a.roster, by player id); players that came later follow it.
            std::vector<uint8_t> order = a.roster;
            for (const auto& [id, r] : a.net->Remotes())
                if (InRace(r) && std::find(order.begin(), order.end(), id) == order.end())
                    order.push_back(id);
            size_t k = 0;
            for (uint8_t id : order)
            {
                const auto rit = a.net->Remotes().find(id);
                if (rit == a.net->Remotes().end())
                {
                    ++k;   // left since: its opponent stays with the game's own driver
                    continue;
                }
                const mp::Remote& r = rit->second;
                if (!a.standIns.count(id))
                {
                    if (k < candidates.size() && candidates[k])
                    {
                        a.standIns[id] = candidates[k];
                        fprintf(stderr, "[mp] remote player %u (%s) drives opponent %zu, body %08X\n", id, r.name.c_str(), k, candidates[k]);
                    }
                    else if (k < candidates.size())
                        break;   // its vehicle is not there yet: try again next frame (and do not give the later players the earlier ones' vehicles)
                }
                ++k;
            }
            // More players than opponent cars: the last players have no car to drive here (the race needs at least as many opponents as other players).
            if (!a.fewShown && !candidates.empty() && inRaceCount > candidates.size())
            {
                a.fewShown = true;
                ui::Toast("This race has fewer opponents than other players, so some players are not shown. Next time choose more opponents.", 20.0);
                fprintf(stderr, "[mp] only %zu opponent cars for %zu other players\n", candidates.size(), inRaceCount);
            }
        }
    }
}

SB_HOOK(sub_82528448)
{
    const uint32_t obj = ctx.r3.u32;
    ApplyRemote(A(), base, obj);
    SB_ORIGINAL(sub_82528448)(ctx, base);

    Adapter& a = A();
    if (!a.ghost && a.server.empty())
        return;
    const uint32_t s = PPC_LOAD_U32(obj + 28);
    if (!s)
        return;
    const double now = a.Now();

    const float here[3] = { LoadF(base, s + 48), LoadF(base, s + 52), LoadF(base, s + 56) };
    if (a.localObj && obj == a.localObj)
    {
        const Pose pose = ReadPose(base, s);
        if (a.ghost)
        {
            a.history.emplace_back(now, pose);
            while (a.history.size() > 2 && a.history[1].first < now - a.ghostSeconds - 1.0)
                a.history.pop_front();
        }
        if (now - a.lastLog > 3.0)
        {
            a.lastLog = now;
            std::vector<uint32_t> bodies = Bodies(base);
            fprintf(stderr, "[mp] local car body %08X at (%.1f %.1f %.1f); %zu bodies:", obj, here[0], here[1], here[2], bodies.size());
            for (size_t i = 0; i < bodies.size(); ++i)
            {
                uint32_t bs = PPC_LOAD_U32(bodies[i] + 28);
                fprintf(stderr, " #%zu %08X(%.0f,%.0f)", i, bodies[i], bs ? LoadF(base, bs + 48) : 0.f, bs ? LoadF(base, bs + 52) : 0.f);
            }
            if (const double want = game::MpWantedClock(); want >= 0)
                fprintf(stderr, " | race clock minus the agreed clock: %+.3f s", double(LoadF(base, 0x82A153B8)) - want);
            if (a.net)
                for (const auto& [id, r] : a.net->Remotes())
                    if (r.hasCar)
                        fprintf(stderr, " | clock of player %u minus mine: %+.2f s", id, double(r.latest.raceClock) - double(LoadF(base, 0x82A153B8)) - 0.001 * a.net->RttMs() * 0.5);
            if (a.net)
                fprintf(stderr, " | net %s, %zu remote, rtt %u ms", a.net->Connected() ? "up" : a.net->Failed() ? "FAILED" : "connecting", a.net->Remotes().size(), a.net->RttMs());
            fprintf(stderr, "\n");
        }
        return;
    }

    // Ghost test: the n-th body of the list (NFSMW_MP_BODY, see the log above) replays the local car from `ghostSeconds` ago.
    if (a.ghost && a.ghostBody >= 0)
    {
        if (!a.standInObj)
        {
            std::vector<uint32_t> bodies = Bodies(base);
            if (size_t(a.ghostBody) < bodies.size())
                a.standInObj = bodies[a.ghostBody];
        }
        if (obj == a.standInObj && !a.history.empty())
        {
            const double want = now - a.ghostSeconds;
            const Pose* pick = &a.history.front().second;
            for (const auto& h : a.history)
                if (h.first <= want)
                    pick = &h.second;
            Pose out = *pick;
            for (int c = 0; c < 3; ++c)
                out.pos[c] += out.m[0][c] * a.ghostOffset;
            WritePose(base, s, out);
        }
    }

    ApplyRemote(a, base, obj);
}

// The start barrier, once a game frame (from GameFrameTime, so it runs while the world is held, when no body is stepped): keep the network going, tell the others
// this game is ready (the flag in the car state), and let the world go when the expected players are ready, or after a timeout.
namespace game
{
    void MpFramePump()
    {
        Adapter& a = A();
        {
            const double t = a.Now();
            if (t - a.lastCfg > 2.0)
            {
                a.lastCfg = t;
                a.Reload();
            }
            if (t - a.lastStatus > 0.5)
            {
                a.lastStatus = t;
                std::string text;
                if (a.server.empty())
                    text = "Multiplayer is off.";
                else if (!a.net)
                    text = g_lanAccess.load() == kLanAsking ? "Waiting for the local network permission." : "On. Connecting...";
                else if (a.net->Failed())
                    text = g_lanAccess.load() == kLanDenied ? std::string(kLanDeniedText) : "Cannot reach " + a.server + ", retrying.";
                else if (!a.net->Connected())
                    text = "Connecting to " + a.server + "...";
                else
                {
                    int held = 0, need = 0;
                    a.net->BarrierStatus(held, need);
                    char b[160];
                    if (a.barrier)
                        snprintf(b, sizeof b, "Connected. Holding the start: %d of %d players ready.", std::max(held, 1), std::max(need, a.barrierExpected));
                    else
                    {
                        int inRace = 0;
                        for (const auto& [id, r] : a.net->Remotes())
                            if (InRace(r))
                                ++inRace;
                        snprintf(b, sizeof b, "Connected as player %u: %zu other player(s) on the server, %d in a race, %u ms.", unsigned(a.net->MyId()), a.net->Remotes().size(), inRace, a.net->RttMs());
                    }
                    text = b;
                }
                // NFSMW_MP_READY=1 / NFSMW_MP_CAR=<name> (tests and scripts): the lobby choice without the settings page.
                static const bool envReady = std::getenv("NFSMW_MP_READY") != nullptr;
                if (const char* ec = std::getenv("NFSMW_MP_CAR"))
                    a.cfgCar = ec;
                if (!g_lobbyCar.load() && !a.cfgCar.empty())
                    for (const auto& c : game::CarCatalog())
                        if (c.name == a.cfgCar)
                            g_lobbyCar = c.key;
                if (envReady && g_lobbyCar.load() && !g_lobbyReady.load() && !a.localObj)
                    g_lobbyReady = true;
                {
                    // log what the lobby says about the others, when it changes
                    static std::string lastLobby;
                    std::string now_;
                    if (a.net)
                        for (const auto& [id, r] : a.net->Remotes())
                        {
                            char b[96];
                            snprintf(b, sizeof b, "%u:%s:%016llX:%d ", unsigned(id), r.name.c_str(), (unsigned long long)r.carKey, r.lobbyReady ? 1 : 0);
                            now_ += b;
                        }
                    if (now_ != lastLobby)
                    {
                        lastLobby = now_;
                        fprintf(stderr, "[mp] lobby: %s(you: car %016llX, ready %d)\n", now_.c_str(), (unsigned long long)g_lobbyCar.load(), g_lobbyReady.load() ? 1 : 0);
                    }
                }
                std::vector<game::MpPlayer> players;
                if (a.net && a.net->Connected())
                {
                    const std::vector<game::CarEntry> catalog = game::CarCatalog();
                    auto carName = [&](uint64_t key) {
                        for (const auto& c : catalog)
                            if (c.key == key)
                                return c.display;
                        return std::string(key ? "a car" : "");
                    };
                    const uint8_t host = g_raceHost.load();
                    players.push_back({ a.name, carName(g_lobbyCar.load() ? g_lobbyCar.load() : a.myCarKey), g_lobbyReady.load() || a.localObj != 0, true,
                        host == a.net->MyId() });
                    for (const auto& [id, r] : a.net->Remotes())
                        players.push_back({ r.name, carName(r.carKey), InRace(r), false, host == id });
                }
                std::lock_guard<std::mutex> g(g_statusMutex);
                g_status = text;
                g_players = std::move(players);
            }
        }
        // NFSMW_MP_WATCH=1: log when the state words found by tools/statdiff.py change (which of them mark the end of loading / the start of the intro).
        static const bool watch = std::getenv("NFSMW_MP_WATCH") != nullptr;
        if (watch)
        {
            static const uint32_t words[] = { 0x82A2C23C, 0x82A2C244, 0x82A3067C, 0x82A2C33C, 0x82A2BF4C, 0x82A2C26C, 0x82A2BB4C, 0x82A2DA3C, 0x82A2CF40, 0x82A2D608, 0x82A2C410 };
            static uint32_t last[sizeof words / sizeof *words];
            static bool first = true;
            const uint8_t* wbase = g_memory.base;
            for (size_t i = 0; i < sizeof words / sizeof *words; ++i)
            {
                const uint32_t v = __builtin_bswap32(*(const volatile uint32_t*)(wbase + words[i]));
                if (first || v != last[i])
                    fprintf(stderr, "[watch] %7.2f s %08X = %u%s\n", a.Now(), words[i], v, first ? " (initial)" : "");
                last[i] = v;
            }
            first = false;
        }
        Tick(a, g_memory.base, a.Now());
        BuildHud(a, g_memory.base, a.Now());
        if (!a.barrier || !a.net)
            return;
        const double now = a.Now();
        // Race check: cars of one race on one track and direction are held within a few metres of each other (the start); a player held hundreds of metres away is in another race.
        if (!a.mismatchShown && a.localObj)
            if (const uint32_t pb = PhysBody(g_memory.base, a.localObj))
            {
                const Phys mine = ReadPhys(g_memory.base, pb);
                for (const auto& [id, r] : a.net->Remotes())
                    if (r.hasCar && (r.latest.flags & mp::kFlagReady) && Dist(mine.pos, r.latest.pos) > 200.f)
                    {
                        a.mismatchShown = true;
                        const std::string who = r.name.empty() ? ("player " + std::to_string(id)) : r.name;
                        ui::Toast(who + " is not in the same race as you (a different track or direction). Quit and choose the same track.", 20.0);
                        fprintf(stderr, "[mp] race check: %s is %.0f m from our start: not the same race\n", who.c_str(), double(Dist(mine.pos, r.latest.pos)));
                        break;
                    }
            }
        // The release is the server's decision (net/mp_server.cpp, MaybeGo): this game says it is held and how many players the race needs, and waits for Go. The games are not
        // at the same point of their race world when they are held (the race appears at a different moment of loading on each machine: 1.8 s apart between the ARM and x86 test
        // machines), so the server sends the slowest world clock and one start time: the game that is ahead waits the difference, so the others catch up in real time and the
        // countdown and GO come together.
        if (!a.net->Connected())
            a.holdSent = false;
        else if (!a.holdSent)
        {
            a.net->SendHold(uint8_t(std::clamp(a.barrierExpected, 1, 16)), LoadF(g_memory.base, 0x82A153B8));
            a.holdSent = true;
        }
        float ref = 0;
        uint32_t startAtMs = 0;
        if (a.net->TakeGo(ref, startAtMs))
        {
            a.goPending = true;
            a.goRef = ref;
            a.goStart = now + std::max(0.0, double(int32_t(startAtMs - a.net->NowMs())) / 1000.0);
        }
        const float mine = LoadF(g_memory.base, 0x82A153B8);
        const double extra = a.goPending ? std::clamp(double(mine - a.goRef), 0.0, 20.0) : 0.0;
        const bool released = a.goPending && now >= a.goStart + extra;
        const bool timeout = now - a.barrierSince > 300.0 || (a.net->Failed() && now - a.barrierSince > 10.0);
        // B (or Back) leaves the wait: start without the others.
        const bool cancel = (hid::GameButtons() & (0x2000 | 0x0020)) != 0 && now - a.barrierSince > 1.0;
        if (released || timeout || cancel)
        {
            a.barrier = false;
            if (released && std::isfinite(a.goRef) && a.goRef >= 0)
            {
                // The slowest game's world starts at goStart; one that waited `extra` s starts later with a clock that is `extra` s ahead, so the same line fits both.
                a.refClock = double(a.goRef);
                a.refTime = a.goStart;
            }
            else
                a.net->SendUnhold();
            a.holdSent = a.goPending = false;
            game::SetWorldHold(false);
            ui::WithdrawToast(kWaitToast);
            fprintf(stderr, "[mp] start barrier released after %.1f s (%s; waited %.2f s for the others' race clocks to catch up)\n", now - a.barrierSince, released ? "the server released the start" : cancel ? "cancelled with B" : "timeout", extra);
        }
    }
}

// sub_82420FC0(vehicle, dt, pose, flag): sets a vehicle's pose from a 4x4 matrix (rows, translation row at +48; the body's own axes) and is what the game's grid
// animation uses: every frame before the countdown it is called once per racing vehicle, in the order of the vehicle table (player first), with the pose that
// vehicle's animation has reached (the cars roll from where they appear to their starting slots). Every game puts its own player in slot 0; with a start barrier
// the local player is given the slot of its join order instead: its call gets the pose that slot's vehicle was just given, so the car really starts there, and
// the other players' cars (written into the opponents' bodies) are where their games put them. NFSMW_MP_TRACE_POSE=1 logs the calls.
SB_HOOK(sub_82420FC0)
{
    static const bool trace = std::getenv("NFSMW_MP_TRACE_POSE") != nullptr;
    static std::vector<uint32_t> vehicles;                   // vehicle objects in order of first call: the table order
    static std::vector<std::array<uint32_t, 16>> given;      // the last pose each was given by the animation (raw guest words), before any change
    const uint32_t v = ctx.r3.u32, m = ctx.r4.u32;
    {
        static int epoch = 0;
        if (epoch != A().raceEpoch)
        {
            epoch = A().raceEpoch;
            vehicles.clear();
            given.clear();
        }
    }
    size_t idx = 0;
    while (idx < vehicles.size() && vehicles[idx] != v)
        ++idx;
    if (idx == vehicles.size() && idx < 16)
    {
        vehicles.push_back(v);
        given.emplace_back();
    }
    if (idx < vehicles.size() && m)
    {
        for (int i = 0; i < 16; ++i)
            given[idx][i] = *(const volatile uint32_t*)(base + m + 4 * i);   // raw, still big-endian
        Adapter& a = A();
        if (idx == 0 && a.Expected() > 1 && a.net && a.net->Connected())
        {
            size_t rank = 0;
            for (const auto& [id, r] : a.net->Remotes())
                if (id < a.net->MyId() && InRace(r))
                    ++rank;
            if (rank > 0 && rank < vehicles.size())
            {
                bool valid = false;
                for (uint32_t w : given[rank]) valid |= w != 0;
                if (valid)
                {
                    for (int i = 0; i < 16; ++i)
                        *(volatile uint32_t*)(base + m + 4 * i) = given[rank][i];
                    if (trace || !a.gridLogged)
                    {
                        a.gridLogged = true;
                        fprintf(stderr, "[mp] local car follows the grid animation of slot %zu (join order)\n", rank);
                    }
                }
            }
        }
    }
    if (trace)
    {
        static int n = 0;
        if (n < 40 || n % 120 == 0)
            fprintf(stderr, "[pose] #%d vehicle %08X (#%zu) pos (%.2f %.2f %.2f)\n", n, v, idx, LoadF(base, m + 48), LoadF(base, m + 52), LoadF(base, m + 56));
        ++n;
    }
    SB_ORIGINAL(sub_82420FC0)(ctx, base);
}

// For the settings page (ui/online_menu.cpp), any thread.
namespace game
{
    void MpSendChat(const std::string& text)
    {
        std::string line;
        for (char ch : text)
            if (line.size() < mp::kMaxChat)
                line += (ch >= 32 && ch < 127) ? ch : '?';
        if (line.find_first_not_of(' ') == std::string::npos)
            return;
        std::lock_guard<std::mutex> g(g_chatMutex);
        if (g_chatOut.size() < 8)
            g_chatOut.push_back(std::move(line));
    }

    std::vector<std::string> MpChatLog()
    {
        std::lock_guard<std::mutex> g(g_chatMutex);
        std::vector<std::string> out;
        for (const auto& [at, line] : g_chatLog)
            out.push_back(line);
        return out;
    }

    std::string MpStatus()
    {
        std::lock_guard<std::mutex> g(g_statusMutex);
        return g_status;
    }
}

// The race clock every game aims at (seconds), or a negative number when there is none (no synchronised race). Any thread that is the game's main loop.
namespace game
{
    double MpWantedClock()
    {
        Adapter& a = A();
        if (a.refClock < 0 || a.barrier)
            return -1;
        return a.refClock + std::max(0.0, a.Now() - a.refTime);
    }
}

// Called by game/car_select.cpp right after the game created the player's vehicle (the first vehicle of a race): check that it is the car this player chose in the lobby (the others
// were given that car when they created their opponents), and say so if it is not.
namespace game
{
    void MpPlayerCreated(uint8_t* base)
    {
        Adapter& a = A();
        if (a.server.empty())
            return;
        const uint32_t data = PPC_LOAD_U32(kVehicleTable + 4), count = PPC_LOAD_U32(kVehicleTable + 12);
        if (data != kVehicleTable + 16 || count == 0)
            return;
        const uint32_t record = PPC_LOAD_U32(PPC_LOAD_U32(data) + 0x74);
        const uint64_t key = game::CarKeyOfRecord(base, record);
        if (!key)
            return;
        a.myCarKey = key;
        auto nameOf = [](uint64_t k) {
            for (const auto& c : game::CarCatalog())
                if (c.key == k)
                    return c.display;
            return std::string("another car");
        };
        fprintf(stderr, "[mp] my car is %s (%016llX)\n", nameOf(key).c_str(), (unsigned long long)key);
        const uint64_t chosen = g_lobbyCar.load();
        if (g_lobbyReady.load() && chosen && chosen != key)
        {
            const std::string text = "You are racing the " + nameOf(key) + ", but the other players were given the " + nameOf(chosen) + " for you. Choose the " + nameOf(chosen) + " in the car selection next time.";
            ui::Toast(text, 25.0);
            fprintf(stderr, "[mp] car mismatch: chose %s in the lobby, races %s\n", nameOf(chosen).c_str(), nameOf(key).c_str());
        }
    }

    // Called by game/car_select.cpp before the race script creates its first opponent: the opponents are made as the cars the other ready players chose in the lobby, in the order of the
    // players' ids (the same order the stand-ins are given out in). Nothing is waited for: a car swapped in late is not drawn, so the choices have to be known by now.
    void MpBeforeOpponents(uint8_t*)
    {
        Adapter& a = A();
        std::vector<uint64_t> keys;
        a.roster.clear();
        a.nameCalls = 0;
        if (!a.server.empty() && a.net)
            for (const auto& [id, r] : a.net->Remotes())
                if (InRace(r))
                {
                    keys.push_back(r.carKey);
                    a.roster.push_back(id);
                }   // 0 where that player chose no car: that opponent keeps the game's own car, but keeps its place
        fprintf(stderr, "[mp] opponents are made as %zu car(s) chosen in the lobby (%zu players on the server)\n", keys.size(), a.net ? a.net->Remotes().size() : size_t(0));
        game::SetOpponentCars(keys);
    }

    void MpSetLobby(uint64_t carKey, bool ready)
    {
        g_lobbyCar = carKey;
        g_lobbyReady = ready;
    }
    bool MpLobbyReady() { return g_lobbyReady.load(); }

    std::string MpRaceCarName()
    {
        Adapter& a = A();
        // The lobby's choice, or (before the frame pump has applied it: a race that starts right away) the settings' car by name and NFSMW_MP_READY.
        const char* envCar = std::getenv("NFSMW_MP_CAR");
        const std::string byName = envCar ? std::string(envCar) : a.cfgCar;
        const uint64_t key = g_lobbyCar.load();
        const bool ready = g_lobbyReady.load() || std::getenv("NFSMW_MP_READY") != nullptr;
        if (std::getenv("NFSMW_LOG_CARS"))
            fprintf(stderr, "[cars] the race car: server '%s', world %d, ready %d, lobby car %016llX / '%s', catalogue %zu\n", a.server.c_str(), int(a.world),
                int(ready), (unsigned long long)key, byName.c_str(), CarCatalog().size());
        if (a.server.empty() || a.world || !ready || (!key && byName.empty()))
            return {};
        // The game looks the car up by its name (the Attrib collection of the lower-case folder name), so the settings' name is enough; the catalogue only when
        // nothing but the key is known.
        if (!byName.empty())
        {
            std::string name = byName;
            for (char& ch : name)
                if (ch >= 'A' && ch <= 'Z')
                    ch = char(ch + 32);
            return name;
        }
        for (const CarEntry& c : CarCatalog())
            if (c.key == key)
            {
                std::string name = c.name;
                for (char& ch : name)
                    if (ch >= 'A' && ch <= 'Z')
                        ch = char(ch + 32);
                return name;
            }
        return {};
    }
}

namespace game
{
    std::vector<MpPlayer> MpPlayers()
    {
        std::lock_guard<std::mutex> g(g_statusMutex);
        return g_players;
    }
}

// A racer (the standings and the results show its name) is a record of 0x1A0 bytes whose +8 is a pointer to its name; sub_82335248(record) picks that name, a random driver name from
// the game's table, as the race script creates each opponent. The opponents that are other players get their own name here instead, in the order the opponents are made (the
// players in the race, in id order: the same roster the opponents' cars are made for). The text goes in a block of the game's own heap, kept for the whole session.
extern "C" PPC_FUNC(__imp__sub_82335248);
SB_HOOK(sub_82335248)
{
    Adapter& a = A();
    const uint32_t racer = ctx.r3.u32;
    SB_ORIGINAL(sub_82335248)(ctx, base);
    if (a.server.empty() || !a.net)
        return;
    std::vector<uint8_t> players;
    for (const auto& [id, r] : a.net->Remotes())
        if (InRace(r))
            players.push_back(id);
    const size_t index = a.nameCalls++;
    if (index >= players.size() || racer < 0x10000)
        return;
    const auto it = a.net->Remotes().find(players[index]);
    const std::string t = it == a.net->Remotes().end() ? std::string() : RacerText(it->second.name);
    if (t.empty())
        return;
    static uint8_t* block = static_cast<uint8_t*>(g_userHeap.Alloc(16 * mp::kMaxPlayers));
    uint8_t* text = block + 16 * (it->first % mp::kMaxPlayers);
    memset(text, 0, 16);
    memcpy(text, t.data(), t.size());   // the heap's host memory is the guest's: plain bytes, one char each
    PPC_STORE_U32(racer + 8, uint32_t(text - base));
    fprintf(stderr, "[mp] opponent %zu (record %08X) is listed as %s\n", index, racer, t.c_str());
}

namespace game
{
    bool MpWorldMode() { return A().world; }

    bool MpWorldNextSpawn(uint8_t& id, std::string& car)
    {
        Adapter& a = A();
        if (!a.world || !a.net || !a.net->Connected() || !a.localObj)
            return false;
        // A player that left gives its vehicle back to the pool.
        for (auto it = a.standIns.begin(); it != a.standIns.end();)
            if (!a.net->Remotes().count(it->first))
            {
                game::WorldPoolRelease(it->second);
                a.parked.insert(it->second);
                fprintf(stderr, "[mp] player %u left: its vehicle %08X goes back to the pool\n", unsigned(it->first), it->second);
                it = a.standIns.erase(it);
            }
            else
                ++it;
        for (const auto& [rid, r] : a.net->Remotes())
        {
            // A player that is on the server (with the car it announced) gets its vehicle at once, not when its car state arrives: a vehicle made later than a few seconds after the
            // player's own gets no model (the game asks for a car's files only while the world is still loading, docs/MULTIPLAYER.md).
            if ((!r.hasCar && !r.carKey) || a.standIns.count(rid) || a.worldPending.count(rid))
                continue;
            id = rid;
            car = "is300";
            for (const auto& c : game::CarCatalog())
                if (c.key == r.carKey)
                    car = c.name;
            for (char& ch : car)
                if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
            a.worldPending.insert(rid);
            return true;
        }
        return false;
    }

    void MpWorldSpawned(uint8_t id, uint32_t owner)
    {
        Adapter& a = A();
        a.worldPending.erase(id);
        if (owner)
        {
            a.parked.erase(owner);
            a.standIns[id] = owner;
            fprintf(stderr, "[mp] remote player %u is a vehicle in the world, owner %08X\n", unsigned(id), owner);
        }
    }

    std::vector<std::string> MpWorldWantedCars()
    {
        Adapter& a = A();
        std::vector<std::string> out;
        if (a.net && a.net->Connected())
            for (const auto& [rid, r] : a.net->Remotes())
                if (r.carKey)
                    for (const auto& c : game::CarCatalog())
                        if (c.key == r.carKey)
                        {
                            std::string n = c.name;
                            for (char& ch : n) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
                            out.push_back(n);
                        }
        return out;
    }
    void MpWorldPark(uint32_t vehicle) { A().parked.insert(vehicle); }
    std::vector<uint32_t> MpWorldVehicles(uint8_t* base) { return VehicleBodies(base); }
    uint32_t MpWorldPhysBody(uint8_t* base, uint32_t vehicle) { return vehicle > 0x10000 ? PhysBody(base, vehicle) : 0; }
}
