// mp_racebot <host[:port]> <races file> <race index> [name] [speed m/s] [car key hex]: a stand-in racer for testing the open world race with one game. It joins, stops in the race's start
// marker until the race opens and starts, then drives the race's points in order at the given speed (default 45), as many laps as the race has, through the real server and protocol.
#include "mp_client.h"
#include "mp_race.h"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv)
{
    if (argc < 4) { fprintf(stderr, "usage: mp_racebot host[:port] races.txt index [name] [speed] [car key]\n"); return 2; }
    std::string host = argv[1];
    uint16_t port = mp::kDefaultPort;
    if (size_t c = host.rfind(':'); c != std::string::npos) { port = uint16_t(atoi(host.c_str() + c + 1)); host.resize(c); }
    std::vector<mp::RaceDef> races;
    if (!mp::LoadRaces(argv[2], races) || races.empty()) { fprintf(stderr, "cannot read races from %s\n", argv[2]); return 2; }
    const size_t idx = size_t(atoi(argv[3]));
    if (idx >= races.size()) { fprintf(stderr, "no race %zu\n", idx); return 2; }
    const std::string name = argc > 4 ? argv[4] : "racebot";
    const float speed = argc > 5 ? float(atof(argv[5])) : 45.f;
    const uint64_t carKey = argc > 6 ? strtoull(argv[6], nullptr, 16) : 0x0D928DCAF667ABA4ull;
    const mp::RaceDef& def = races[idx];
    mp::Client c;
    if (!c.Connect(host, port, name)) return 1;
    c.SetLobby(carKey, true);
    mp::Vec3 pos = def.points[0];
    pos.x += 4.f;                       // beside the marker's centre, inside it
    float heading = 0;
    size_t target = 1;
    int laps = 0;
    uint32_t lastSend = 0, lastLog = 0, lastMove = 0;
    bool wasRunning = false;
    for (;;)
    {
        c.Service();
        const uint32_t now = c.NowMs();
        const mp::RaceView v = c.CurrentRace();
        bool joined = false;
        for (const auto& r : v.racers) if (r.id == c.MyId()) joined = true;
        const bool run = v.phase == mp::RacePhase::Running && joined;
        if (run && !wasRunning) { target = 1; laps = 0; pos = def.points[0]; pos.x += 4.f; }
        wasRunning = run;
        float vx = 0, vz = 0, spd = 0;
        const float dt = std::min(0.1f, float(now - lastMove) * 0.001f);
        lastMove = now;
        if (run && laps < def.laps)
        {
            const mp::Vec3& tg = def.points[target % def.points.size()];
            float dx = tg.x - pos.x, dz = tg.z - pos.z;
            const float len = std::sqrt(dx * dx + dz * dz);
            const float step = speed * dt;
            heading = std::atan2(dx, dz);
            if (len <= step)
            {
                pos.x = tg.x; pos.z = tg.z; pos.y = tg.y;
                ++target;
                if (target % def.points.size() == 1 && target > 1) ++laps;
            }
            else { pos.x += dx / len * step; pos.z += dz / len * step; pos.y += (tg.y - pos.y) / len * step; }
            vx = dx / (len > 1e-3f ? len : 1.f) * speed; vz = dz / (len > 1e-3f ? len : 1.f) * speed; spd = speed;
        }
        if (c.Connected() && now - lastSend >= 33)
        {
            lastSend = now;
            mp::CarState s;
            s.pos[0] = pos.x; s.pos[1] = pos.y; s.pos[2] = pos.z;
            s.vel[0] = vx; s.vel[2] = vz;
            s.rot[0] = 0; s.rot[1] = std::sin(heading * 0.5f); s.rot[2] = 0; s.rot[3] = std::cos(heading * 0.5f);
            s.throttle = spd > 0 ? 1.f : 0.f;
            c.SendCar(s);
        }
        if (now - lastLog > 3000)
        {
            lastLog = now;
            fprintf(stderr, "[racebot] %s phase %d time %u ms, %zu racers, target %zu lap %d\n", c.Connected() ? "up" : "connecting", int(v.phase), v.timeMs, v.racers.size(), target, laps);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
