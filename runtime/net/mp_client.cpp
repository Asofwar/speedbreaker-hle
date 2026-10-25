#include "mp_client.h"
#include <enet/enet.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

namespace mp
{
    namespace
    {
        // Remote cars are drawn this far in the past (at least: about three snapshots), more on a link whose delay varies (Clock below), at most kMaxDelayMs.
        constexpr uint32_t kInterpDelayMs = 100;
        constexpr uint32_t kMaxDelayMs = 300;
        constexpr uint32_t kMaxExtrapMs = 250;      // after that hold the last pose
        constexpr size_t kHistory = 16;
        constexpr uint32_t kStaleMs = 2000;         // no packet for this long: RemoteCar() reports nothing

        // localMs: when the state arrived (for staleness only); the pose is placed in time by the sender's own clock (CarState::ownerTimeMs) through Clock.
        struct Stamped { uint32_t localMs; CarState s; };

        // One remote player's clock against ours. offset is the smallest (arrival - sender time) seen, the least delayed packet: arrival jitter then doesn't move
        // where a state is placed in time, only how far behind it has to be drawn so that a later state has nearly always arrived (the excess delay's mean plus
        // three deviations, kept between kInterpDelayMs and kMaxDelayMs). The offset creeps up slowly, so a route that got slower (or clocks that drift) is followed.
        struct Clock
        {
            bool have = false;
            double offset = 0;
            double excessAvg = 0, excessDev = 0;
            void Arrived(uint32_t localMs, uint32_t ownerMs)
            {
                const double o = double(int64_t(localMs) - int64_t(ownerMs));
                if (!have || o < offset)
                    offset = o;
                else
                    offset += (o - offset) * 0.002;
                have = true;
                const double excess = o - offset;
                excessAvg += (excess - excessAvg) * 0.05;
                excessDev += (std::abs(excess - excessAvg) - excessDev) * 0.05;
            }
            uint32_t DelayMs() const
            {
                const double d = 40.0 + excessAvg + 3.0 * excessDev;
                return uint32_t(std::clamp(d, double(kInterpDelayMs), double(kMaxDelayMs)));
            }
            // A state's time on our clock.
            int64_t Local(const CarState& s) const { return int64_t(s.ownerTimeMs) + int64_t(std::llround(offset)); }
        };
    }

    CarState Lerp(const CarState& a, const CarState& b, float t)
    {
        CarState o = b;
        for (int i = 0; i < 3; ++i)
        {
            o.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
            o.vel[i] = a.vel[i] + (b.vel[i] - a.vel[i]) * t;
            o.angVel[i] = a.angVel[i] + (b.angVel[i] - a.angVel[i]) * t;
        }
        float dot = 0;
        for (int i = 0; i < 4; ++i) dot += a.rot[i] * b.rot[i];
        float s = dot < 0 ? -1.f : 1.f;   // shortest arc
        float n = 0;
        for (int i = 0; i < 4; ++i)
        {
            o.rot[i] = a.rot[i] * (1 - t) + b.rot[i] * s * t;   // nlerp: fine for the small steps between snapshots
            n += o.rot[i] * o.rot[i];
        }
        n = 1.f / std::sqrt(n > 1e-12f ? n : 1.f);
        for (float& f : o.rot) f *= n;
        o.steer = a.steer + (b.steer - a.steer) * t;
        o.throttle = a.throttle + (b.throttle - a.throttle) * t;
        o.brake = a.brake + (b.brake - a.brake) * t;
        return o;
    }

    struct Client::Impl
    {
        ENetHost* host = nullptr;
        ENetPeer* peer = nullptr;
        std::string name;
        bool welcome = false, failed = false;
        uint8_t myId = 0;
        uint64_t myCarKey = 0;
        bool myReady = false;
        uint32_t myRaceKey = 0;
        int heldNow = 0, heldNeed = 0;
        bool go = false;
        float goClock = 0;
        uint32_t goAtMs = 0;
        uint32_t seq = 0;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        std::map<uint8_t, Remote> remotes;
        std::map<uint8_t, std::deque<Stamped>> hist;
        std::map<uint8_t, Clock> clocks;
        mutable std::mutex raceMutex;       // the overlay reads the race from the render thread
        std::vector<RaceDef> races;
        std::vector<RaceRecord> records;
        RaceView raceView;
        uint32_t raceViewAt = 0;

        uint32_t Now() const
        {
            return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        }
        void Send(int ch, const Writer& w, bool reliable)
        {
            if (peer)
                enet_peer_send(peer, ch, enet_packet_create(w.b.data(), w.b.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED));
        }
    };

    Client::Client() : m(new Impl) {}
    Client::~Client() { Disconnect(); }

    bool Client::Connect(const std::string& hostName, uint16_t port, const std::string& name)
    {
        if (enet_initialize() != 0) return false;
        m->host = enet_host_create(nullptr, 1, kChannels, 0, 0);
        ENetAddress a{};
        if (!m->host || enet_address_set_host(&a, hostName.c_str()) != 0) return false;
        a.port = port;
        m->peer = enet_host_connect(m->host, &a, kChannels, 0);
        m->name = name;
        m->welcome = m->failed = false;
        return m->peer != nullptr;
    }

    void Client::Disconnect()
    {
        if (m->peer)
        {
            enet_peer_disconnect_now(m->peer, 0);
            m->peer = nullptr;
        }
        if (m->host)
        {
            enet_host_destroy(m->host);
            m->host = nullptr;
        }
        m->welcome = false;
        m->remotes.clear();
        m->hist.clear();
        m->clocks.clear();
    }

    void Client::Service()
    {
        if (!m->host) return;
        ENetEvent ev;
        while (enet_host_service(m->host, &ev, 0) > 0)
        {
            if (ev.type == ENET_EVENT_TYPE_CONNECT)
            {
                Writer w(Msg::Hello); w.u32(kProtocolVersion); w.str(m->name);
                m->Send(kChanEvent, w, true);
            }
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                m->failed = true;
                m->welcome = false;
                m->peer = nullptr;
            }
            else if (ev.type == ENET_EVENT_TYPE_RECEIVE)
            {
                Reader r(ev.packet->data, ev.packet->dataLength);
                switch (r.type())
                {
                case Msg::Welcome:
                    m->myId = r.u8();
                    m->welcome = r.ok();
                    if (m->welcome && (m->myCarKey || m->myReady))
                    {
                        Writer w(Msg::PlayerInfo); w.u32(uint32_t(m->myCarKey >> 32)); w.u32(uint32_t(m->myCarKey)); w.u8(m->myReady ? 1 : 0); w.u32(m->myRaceKey);
                        m->Send(kChanEvent, w, true);   // after a reconnect the server must hear it again
                    }
                    break;
                case Msg::PlayerInfo:
                {
                    const uint8_t id = r.u8();
                    const uint32_t hi = r.u32(), lo = r.u32();
                    const uint8_t rdy = r.u8();
                    const uint32_t race = r.u32();
                    if (r.ok() && id != m->myId)
                    {
                        m->remotes[id].carKey = (uint64_t(hi) << 32) | lo;
                        m->remotes[id].lobbyReady = rdy != 0;
                        m->remotes[id].raceKey = race;
                    }
                    break;
                }
                case Msg::Reject:
                    m->failed = true;
                    break;
                case Msg::PlayerJoin:
                {
                    uint8_t id = r.u8();
                    std::string n = SanitizeName(r.str());
                    if (r.ok() && id != m->myId)
                    {
                        m->remotes[id].id = id;
                        m->remotes[id].name = n;
                    }
                    break;
                }
                case Msg::PlayerLeave:
                {
                    uint8_t id = r.u8();
                    m->remotes.erase(id);
                    m->hist.erase(id);
                    m->clocks.erase(id);
                    break;
                }
                case Msg::Snapshot:
                {
                    uint8_t n = r.u8();
                    uint32_t now = m->Now();
                    for (uint8_t i = 0; i < n && r.ok(); ++i)
                    {
                        uint8_t id = r.u8();
                        CarState c = r.car();
                        if (!r.ok()) break;
                        if (!Sane(c)) continue;   // never hand the game a car state its physics could not have produced
                        Remote& rm = m->remotes[id];
                        rm.id = id;
                        auto& h = m->hist[id];
                        if (!h.empty() && int32_t(c.seq - h.back().s.seq) <= 0)
                            continue;   // old or duplicate
                        rm.latest = c;
                        rm.hasCar = true;
                        m->clocks[id].Arrived(now, c.ownerTimeMs);
                        h.push_back({now, c});
                        if (h.size() > kHistory) h.pop_front();
                    }
                    break;
                }
                case Msg::Barrier:
                {
                    const uint8_t h = r.u8(), n = r.u8();
                    if (r.ok()) { m->heldNow = h; m->heldNeed = n; }
                    break;
                }
                case Msg::Go:
                {
                    const float c = r.f32();
                    const uint32_t inMs = r.u32();
                    if (r.ok() && c == c)
                    {
                        m->go = true;
                        m->goClock = c;
                        m->goAtMs = m->Now() + inMs;
                    }
                    break;
                }
                case Msg::RaceList:
                {
                    std::vector<RaceDef> rs;
                    const uint8_t n = r.u8();
                    for (uint8_t i = 0; i < n && r.ok(); ++i)
                    {
                        RaceDef d;
                        d.name = r.str(); d.laps = r.u8(); d.sprint = (r.u8() & 1) != 0; d.radius = r.f32();
                        const uint8_t np = r.u8();
                        for (uint8_t k = 0; k < np && r.ok(); ++k)
                        {
                            Vec3 v; v.x = r.f32(); v.y = r.f32(); v.z = r.f32();
                            d.points.push_back(v);
                        }
                        rs.push_back(std::move(d));
                    }
                    if (r.ok())
                    {
                        std::lock_guard<std::mutex> g(m->raceMutex);
                        m->races = std::move(rs);
                    }
                    break;
                }
                case Msg::RaceState:
                {
                    RaceView v;
                    const uint8_t race = r.u8();
                    v.race = race == 255 ? -1 : race;
                    v.phase = RacePhase(r.u8());
                    v.timeMs = r.u32();
                    const uint8_t n = r.u8();
                    for (uint8_t i = 0; i < n && r.ok(); ++i)
                    {
                        RacerView rv;
                        rv.id = r.u8(); rv.lap = r.u8(); rv.next = r.u8(); rv.place = r.u8(); rv.finishMs = r.u32();
                        rv.finished = rv.finishMs != 0;
                        v.racers.push_back(rv);
                    }
                    if (r.ok())
                    {
                        std::lock_guard<std::mutex> g(m->raceMutex);
                        m->raceView = std::move(v);
                        m->raceViewAt = m->Now();
                    }
                    break;
                }
                case Msg::Records:
                {
                    std::vector<RaceRecord> rs;
                    const uint8_t n = r.u8();
                    for (uint8_t i = 0; i < n && r.ok(); ++i)
                    {
                        RaceRecord rec;
                        rec.ms = r.u32();
                        rec.name = r.str();
                        rs.push_back(std::move(rec));
                    }
                    if (r.ok())
                    {
                        std::lock_guard<std::mutex> g(m->raceMutex);
                        m->records = std::move(rs);
                    }
                    break;
                }
                case Msg::Chat:
                {
                    uint8_t id = r.u8();
                    std::string t = r.str();
                    if (r.ok() && onChat) onChat(id, t);
                    break;
                }
                default: break;
                }
                enet_packet_destroy(ev.packet);
            }
        }
    }

    bool Client::Connected() const { return m->welcome; }
    bool Client::Failed() const { return m->failed; }
    uint8_t Client::MyId() const { return m->myId; }
    std::vector<RaceDef> Client::Races() const
    {
        std::lock_guard<std::mutex> g(m->raceMutex);
        return m->races;
    }
    std::vector<RaceRecord> Client::Records() const
    {
        std::lock_guard<std::mutex> g(m->raceMutex);
        return m->records;
    }
    RaceView Client::CurrentRace() const
    {
        std::lock_guard<std::mutex> g(m->raceMutex);
        RaceView v = m->raceView;
        const uint32_t dt = m->Now() - m->raceViewAt;
        if (v.phase == RacePhase::Gathering || v.phase == RacePhase::Countdown)
            v.timeMs = v.timeMs > dt ? v.timeMs - dt : 0;
        else if (v.phase == RacePhase::Running)
            v.timeMs += dt;
        return v;
    }
    uint32_t Client::NowMs() const { return m->Now(); }
    uint32_t Client::RttMs() const { return m->peer ? m->peer->roundTripTime : 0; }
    const std::map<uint8_t, Remote>& Client::Remotes() const { return m->remotes; }

    void Client::SendCar(const CarState& c)
    {
        if (!m->welcome) return;
        CarState s = c;
        s.seq = ++m->seq;
        s.ownerTimeMs = m->Now();
        Writer w(Msg::CarState);
        w.car(s);
        m->Send(kChanState, w, false);
    }

    void Client::SetLobby(uint64_t key, bool ready, uint32_t raceKey)
    {
        m->myCarKey = key;
        m->myReady = ready;
        m->myRaceKey = raceKey;
        if (!m->welcome) return;   // sent when the welcome arrives
        Writer w(Msg::PlayerInfo); w.u32(uint32_t(key >> 32)); w.u32(uint32_t(key)); w.u8(ready ? 1 : 0); w.u32(raceKey);
        m->Send(kChanEvent, w, true);
    }
    void Client::SendHold(uint8_t need, float worldClock)
    {
        if (!m->welcome) return;
        m->go = false;
        Writer w(Msg::Hold); w.u8(need); w.f32(worldClock);
        m->Send(kChanEvent, w, true);
    }
    void Client::SendUnhold()
    {
        m->go = false;
        m->heldNow = m->heldNeed = 0;
        if (!m->welcome) return;
        Writer w(Msg::Unhold);
        m->Send(kChanEvent, w, true);
    }
    void Client::BarrierStatus(int& held, int& need) const { held = m->heldNow; need = m->heldNeed; }
    bool Client::TakeGo(float& slowestClock, uint32_t& startAtMs)
    {
        if (!m->go) return false;
        m->go = false;
        m->heldNow = m->heldNeed = 0;
        slowestClock = m->goClock;
        startAtMs = m->goAtMs;
        return true;
    }
    uint64_t Client::MyCarKey() const { return m->myCarKey; }

    void Client::SendChat(const std::string& text)
    {
        Writer w(Msg::Chat); w.text(text);
        m->Send(kChanEvent, w, true);
    }

    bool Client::RemoteCar(uint8_t id, uint32_t nowMs, CarState& out) const
    {
        auto it = m->hist.find(id);
        if (it == m->hist.end() || it->second.empty()) return false;
        const auto& h = it->second;
        if (nowMs - h.back().localMs > kStaleMs)
            return false;   // the player stopped sending: let the game's own driver have the car back
        const auto ck = m->clocks.find(id);
        if (ck == m->clocks.end() || !ck->second.have)
            return false;
        const Clock& clock = ck->second;
        // The states in the sender's time (on our clock), so a packet that came late is still drawn where it was taken.
        const int64_t target = int64_t(nowMs) - int64_t(clock.DelayMs());
        if (target <= clock.Local(h.front().s))
        {
            out = h.front().s;
            return true;
        }
        for (size_t i = 1; i < h.size(); ++i)
        {
            const int64_t tb = clock.Local(h[i].s);
            if (target <= tb)
            {
                const Stamped &a = h[i - 1], &b = h[i];
                const int64_t ta = clock.Local(a.s);
                const float t = tb > ta ? float(target - ta) / float(tb - ta) : 1.f;
                out = Lerp(a.s, b.s, t < 0 ? 0 : t > 1 ? 1 : t);
                return true;
            }
        }
        // Newer than anything received: dead reckon from the last state.
        const Stamped& l = h.back();
        uint32_t dt = uint32_t(target - clock.Local(l.s));
        if (dt > kMaxExtrapMs) dt = kMaxExtrapMs;
        out = l.s;
        float s = float(dt) * 0.001f;
        for (int i = 0; i < 3; ++i)
            out.pos[i] += l.s.vel[i] * s;
        return true;
    }
}
