// mp_bot <host[:port]> [delay seconds] [offset metres] [name to follow] [own name]: a stand-in second player for testing with one game. It joins, watches the first other player
// and drives a car that replays that player's car delayed (default 3 s) and shifted sideways (default 8 m along x), through the real server and protocol.
#include "mp_client.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: mp_bot host[:port] [delay s] [offset m]\n"); return 2; }
    std::string host = argv[1];
    uint16_t port = mp::kDefaultPort;
    if (size_t c = host.rfind(':'); c != std::string::npos) { port = uint16_t(atoi(host.c_str() + c + 1)); host.resize(c); }
    const double delay = argc > 2 ? atof(argv[2]) : 3.0;
    const float offset = argc > 3 ? float(atof(argv[3])) : 8.f;
    const std::string follow = argc > 4 ? argv[4] : "";   // replay only the player with this name (several bots must not follow each other)
    const std::string ownName = argc > 5 ? argv[5] : "bot";
    const float offsetZ = argc > 7 ? float(atof(argv[7])) : 0.f;   // metres along the cars' z (in front or behind)
    const uint64_t carKey = argc > 6 ? strtoull(argv[6], nullptr, 16) : 0x0D928DCAF667ABA4ull;   // announced when it sees a player (default: an RX-8)
    mp::Client c;
    if (!c.Connect(host, port, ownName)) return 1;
    std::deque<std::pair<uint32_t, mp::CarState>> hist;   // (local ms, state)
    uint32_t lastSeq = 0, lastSend = 0, lastLog = 0;
    bool held = false;   // told the server that this stand-in is held at the start when the player it follows is
    for (;;)
    {
        c.Service();
        const uint32_t now = c.NowMs();
        if (c.Connected() && c.MyCarKey() == 0 && !c.Remotes().empty())
            c.SetLobby(carKey, true);
        if (c.Connected())
            for (const auto& [id, r] : c.Remotes())
                if (r.hasCar && (follow.empty() || r.name == follow) && r.latest.seq != lastSeq)
                {
                    lastSeq = r.latest.seq;
                    hist.emplace_back(now, r.latest);
                    break;
                }
        while (hist.size() > 1 && hist[1].first + uint32_t(delay * 1000) < now) hist.pop_front();
        if (c.Connected() && !hist.empty() && now - lastSend >= 33)
        {
            lastSend = now;
            mp::CarState s = hist.front().second;
            const bool wantHold = (s.flags & mp::kFlagReady) != 0;
            if (wantHold && !held)
                c.SendHold(uint8_t(c.Remotes().size() + 1), s.raceClock);
            else if (!wantHold && held)
                c.SendUnhold();
            held = wantHold;
            s.pos[0] += offset;
            s.pos[2] += offsetZ;
            c.SendCar(s);
        }
        if (now - lastLog > 3000)
        {
            lastLog = now;
            fprintf(stderr, "[bot] %s, %zu remote, %zu queued, rtt %u ms\n", c.Connected() ? "up" : c.Failed() ? "failed" : "connecting", c.Remotes().size(), hist.size(), c.RttMs());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
