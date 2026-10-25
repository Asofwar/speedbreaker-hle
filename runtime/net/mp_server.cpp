#include "mp_server.h"
#include "mp_race.h"
#include <enet/enet.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>

namespace mp
{
    namespace
    {
        uint32_t NowMs()
        {
            using namespace std::chrono;
            return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
        }
        constexpr uint32_t kTickMs = 33;
    }

    struct Server::Impl
    {
        struct Player
        {
            uint8_t id = 0;
            std::string name;
            ENetPeer* peer = nullptr;
            CarState car;
            bool hasCar = false;
            uint32_t windowStart = 0, inWindow = 0;
            uint64_t carKey = 0;
            bool ready = false;
        };
        ENetHost* host = nullptr;
        uint16_t port = 0;
        std::map<uint8_t, Player> players;
        // The start barrier, decided here: the players held at the start of a race (id -> world clock) and how many the race needs. When all are there the server sends Go with the
        // slowest world clock; every game's world stands still while held, so the others wait the difference and the countdown and GO come together.
        struct Held { float clock = 0; uint8_t need = 0; };
        std::map<uint8_t, Held> held;
        RaceManager race;                 // the open world's races: rules in net/mp_race.cpp
        std::string recordsPath;          // <races file>.records
        std::map<std::string, RaceRecord> records;
        uint32_t raceVersionSent = 0, raceSentAt = 0;
        std::map<ENetPeer*, uint32_t> pending;   // connected, no Hello yet (when it connected)
        uint32_t nextTick = 0, nextSweep = 0;

        void Send(ENetPeer* p, int ch, const Writer& w, bool reliable)
        {
            enet_peer_send(p, ch, enet_packet_create(w.b.data(), w.b.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED));
        }
        void Broadcast(int ch, const Writer& w, bool reliable, uint8_t except = 0xFF)
        {
            for (auto& [id, pl] : players)
                if (id != except)
                    Send(pl.peer, ch, w, reliable);
        }
        Player* FindByPeer(ENetPeer* p)
        {
            for (auto& [id, pl] : players)
                if (pl.peer == p)
                    return &pl;
            return nullptr;
        }
        void SendRaceList(ENetPeer* p)
        {
            const auto& rs = race.Races();
            Writer w(Msg::RaceList);
            w.u8(uint8_t(rs.size()));
            for (const RaceDef& d : rs)
            {
                w.str(d.name); w.u8(uint8_t(d.laps)); w.f32(d.radius); w.u8(uint8_t(d.points.size()));
                for (const Vec3& v : d.points) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
            }
            Send(p, kChanEvent, w, true);
        }
        Writer RecordsMessage() const
        {
            Writer w(Msg::Records);
            const auto& rs = race.Races();
            w.u8(uint8_t(rs.size()));
            for (const RaceDef& d : rs)
            {
                const auto it = records.find(d.name);
                w.u32(it == records.end() ? 0 : it->second.ms);
                w.str(it == records.end() ? std::string() : it->second.name);
            }
            return w;
        }
        // Chat from the server (its console, records).
        void SayAll(const std::string& line)
        {
            Writer w(Msg::Chat); w.u8(kServerId); w.text(line);
            Broadcast(kChanEvent, w, true);
            fprintf(stderr, "[mp-server] chat server: %s\n", line.c_str());
        }
        // The race's finishes: a new best time is kept, saved and announced.
        void TakeFinishes()
        {
            for (const FinishEvent& f : race.TakeFinishes())
            {
                if (f.race < 0 || size_t(f.race) >= race.Races().size())
                    continue;
                const std::string& raceName = race.Races()[size_t(f.race)].name;
                const auto pit = players.find(f.id);
                const std::string who = pit == players.end() ? "player " + std::to_string(f.id) : pit->second.name;
                fprintf(stderr, "[mp-server] %s finished %s in %s\n", who.c_str(), raceName.c_str(), FormatRaceTime(f.ms).c_str());
                RaceRecord& rec = records[raceName];
                if (!Beats(rec, f.ms))
                    continue;
                const RaceRecord was = rec;
                rec = { f.ms, who };
                if (!recordsPath.empty() && !SaveRecords(recordsPath, records))
                    fprintf(stderr, "[mp-server] cannot write %s\n", recordsPath.c_str());
                SayAll("New record on " + raceName + ": " + who + " " + FormatRaceTime(f.ms) +
                    (was.ms ? " (was " + FormatRaceTime(was.ms) + " by " + was.name + ")" : ""));
                Broadcast(kChanEvent, RecordsMessage(), true);
            }
        }
        void BroadcastRaceState(uint32_t now)
        {
            const RaceView v = race.View(now);
            Writer w(Msg::RaceState);
            w.u8(v.race < 0 ? 255 : uint8_t(v.race)); w.u8(uint8_t(v.phase)); w.u32(v.timeMs); w.u8(uint8_t(std::min<size_t>(v.racers.size(), 16)));
            for (size_t i = 0; i < v.racers.size() && i < 16; ++i)
            {
                const RacerView& r = v.racers[i];
                w.u8(r.id); w.u8(r.lap); w.u8(r.next); w.u8(r.place); w.u32(r.finishMs);
            }
            Broadcast(kChanEvent, w, true);
            raceVersionSent = race.Version();
            raceSentAt = now;
        }
        void SendBarrier()
        {
            int need = 0;
            for (auto& [id, h] : held)
                need = std::max<int>(need, h.need);
            Writer w(Msg::Barrier); w.u8(uint8_t(held.size())); w.u8(uint8_t(need));
            for (auto& [id, h] : held)
                if (auto it = players.find(id); it != players.end())
                    Send(it->second.peer, kChanEvent, w, true);
        }
        void MaybeGo()
        {
            int need = 0;
            float slowest = 1.0e30f;
            for (auto& [id, h] : held)
            {
                need = std::max<int>(need, h.need);
                slowest = std::min(slowest, h.clock);
            }
            if (held.empty() || need == 0 || int(held.size()) < need)
                return;
            for (auto& [id, h] : held)
                if (auto it = players.find(id); it != players.end())
                {
                    // All games start at the same instant 300 ms from now: this one hears of it after half its round trip.
                    const uint32_t rtt = it->second.peer->roundTripTime;
                    const uint32_t inMs = rtt / 2 >= 300 ? 0 : 300 - rtt / 2;
                    Writer w(Msg::Go); w.f32(slowest); w.u32(inMs);
                    Send(it->second.peer, kChanEvent, w, true);
                }
            fprintf(stderr, "[mp-server] start released for %zu players (slowest world clock %.2f s)\n", held.size(), double(slowest));
            held.clear();
        }
        void OnReceive(ENetPeer* peer, const uint8_t* data, size_t n)
        {
            Reader r(data, n);
            Msg t = r.type();
            Player* pl = FindByPeer(peer);
            if (t == Msg::Hello)
            {
                uint32_t ver = r.u32();
                std::string name = SanitizeName(r.str());
                if (!r.ok() || pl)
                    return;
                pending.erase(peer);
                if (ver != kProtocolVersion)
                {
                    Writer w(Msg::Reject); w.u8(kRejectVersion);
                    Send(peer, kChanEvent, w, true);
                    enet_peer_disconnect_later(peer, 0);
                    return;
                }
                uint8_t id = 1;
                while (players.count(id) && id < 255) ++id;
                if (players.size() >= size_t(kMaxPlayers))
                {
                    Writer w(Msg::Reject); w.u8(kRejectFull);
                    Send(peer, kChanEvent, w, true);
                    enet_peer_disconnect_later(peer, 0);
                    return;
                }
                Player np; np.id = id; np.name = name; np.peer = peer;
                Writer w(Msg::Welcome); w.u8(id); w.u8(uint8_t(1000 / kTickMs));
                Send(peer, kChanEvent, w, true);
                for (auto& [oid, op] : players)   // tell the newcomer who is here
                {
                    Writer j(Msg::PlayerJoin); j.u8(oid); j.str(op.name);
                    Send(peer, kChanEvent, j, true);
                    if (op.carKey || op.ready)
                    {
                        Writer inf(Msg::PlayerInfo); inf.u8(oid); inf.u32(uint32_t(op.carKey >> 32)); inf.u32(uint32_t(op.carKey)); inf.u8(op.ready ? 1 : 0);
                        Send(peer, kChanEvent, inf, true);
                    }
                }
                Writer j(Msg::PlayerJoin); j.u8(id); j.str(name);
                Broadcast(kChanEvent, j, true);
                players[id] = np;
                if (!race.Races().empty())
                {
                    SendRaceList(peer);
                    Send(peer, kChanEvent, RecordsMessage(), true);
                }
                fprintf(stderr, "[mp-server] %s joined as %u (%zu players)\n", name.c_str(), id, players.size());
            }
            else if (!pl)
                return;
            else if (t == Msg::CarState)
            {
                CarState c = r.car();
                // At most 120 states a second from one player (a game sends 30), and only plausible ones.
                const uint32_t now = NowMs();
                if (now - pl->windowStart >= 1000) { pl->windowStart = now; pl->inWindow = 0; }
                if (++pl->inWindow > 120 || !Sane(c))
                    return;
                if (r.ok() && (!pl->hasCar || int32_t(c.seq - pl->car.seq) > 0))
                {
                    pl->car = c;
                    pl->hasCar = true;
                    const float speed = std::sqrt(c.vel[0] * c.vel[0] + c.vel[1] * c.vel[1] + c.vel[2] * c.vel[2]);
                    race.Report(pl->id, { c.pos[0], c.pos[1], c.pos[2] }, speed, now);
                }
            }
            else if (t == Msg::PlayerInfo)
            {
                const uint32_t hi = r.u32(), lo = r.u32();
                const uint8_t rdy = r.u8();
                if (!r.ok()) return;
                pl->carKey = (uint64_t(hi) << 32) | lo;
                pl->ready = rdy != 0;
                Writer w(Msg::PlayerInfo); w.u8(pl->id); w.u32(hi); w.u32(lo); w.u8(rdy);
                Broadcast(kChanEvent, w, true, pl->id);
            }
            else if (t == Msg::Hold)
            {
                Held h; h.need = r.u8(); h.clock = r.f32();
                if (!r.ok() || !(h.clock == h.clock) || h.need == 0 || h.need > kMaxPlayers) return;
                held[pl->id] = h;
                SendBarrier();
                MaybeGo();
            }
            else if (t == Msg::Unhold)
            {
                if (held.erase(pl->id))
                    SendBarrier();
            }
            else if (t == Msg::Chat)
            {
                std::string text = r.str();
                if (!r.ok()) return;
                for (char& ch : text) if (ch < 32 || ch >= 127) ch = '?';
                if (text.empty()) return;
                Writer w(Msg::Chat); w.u8(pl->id); w.text(text);
                Broadcast(kChanEvent, w, true);
                fprintf(stderr, "[mp-server] chat %s: %s\n", pl->name.c_str(), text.c_str());
            }
        }
        void OnDisconnect(ENetPeer* peer)
        {
            Player* pl = FindByPeer(peer);
            if (!pl) return;
            uint8_t id = pl->id;
            fprintf(stderr, "[mp-server] %s (%u) left\n", pl->name.c_str(), id);
            players.erase(id);
            if (held.erase(id))
                SendBarrier();
            race.Remove(id);
            Writer w(Msg::PlayerLeave); w.u8(id);
            Broadcast(kChanEvent, w, true);
        }
        void Tick()
        {
            const uint32_t now = NowMs();
            race.Tick(now);
            TakeFinishes();
            if (!race.Races().empty() && (race.Version() != raceVersionSent || now - raceSentAt >= 200))
                BroadcastRaceState(now);
            for (auto& [id, pl] : players)
            {
                Writer w(Msg::Snapshot);
                uint8_t count = 0;
                size_t countAt = w.b.size();
                w.u8(0);
                for (auto& [oid, op] : players)
                {
                    if (oid == id || !op.hasCar) continue;
                    w.u8(oid);
                    w.car(op.car);
                    ++count;
                }
                if (count)
                {
                    w.b[countAt] = count;
                    Send(pl.peer, kChanState, w, false);
                }
            }
        }
    };

    Server::Server() : m(new Impl) {}
    Server::~Server()
    {
        if (m->host)
            enet_host_destroy(m->host);
    }
    bool Server::Start(uint16_t port, int maxPlayers)
    {
        if (enet_initialize() != 0) return false;
        ENetAddress a{};
        a.host = ENET_HOST_ANY;
        a.port = port;
        m->host = enet_host_create(&a, size_t(maxPlayers) + 4, kChannels, 0, 0);
        m->port = port;
        m->nextTick = NowMs();
        return m->host != nullptr;
    }
    void Server::Service(int timeoutMs)
    {
        ENetEvent ev;
        int wait = timeoutMs;
        while (enet_host_service(m->host, &ev, wait) > 0)
        {
            wait = 0;
            if (ev.type == ENET_EVENT_TYPE_RECEIVE)
            {
                m->OnReceive(ev.peer, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            }
            else if (ev.type == ENET_EVENT_TYPE_CONNECT)
                m->pending[ev.peer] = NowMs();
            else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                m->pending.erase(ev.peer);
                m->OnDisconnect(ev.peer);
            }
        }
        uint32_t now = NowMs();
        if (int32_t(now - m->nextSweep) >= 0)
        {
            // A connection that has not said Hello within five seconds is dropped (it holds a slot and a socket for nothing).
            m->nextSweep = now + 1000;
            for (auto it = m->pending.begin(); it != m->pending.end();)
            {
                if (now - it->second > 5000)
                {
                    enet_peer_disconnect_now(it->first, 0);
                    it = m->pending.erase(it);
                }
                else
                    ++it;
            }
        }
        if (int32_t(now - m->nextTick) >= 0)
        {
            m->Tick();
            m->nextTick = now + kTickMs;
        }
    }
    bool Server::LoadRaces(const std::string& path)
    {
        std::vector<RaceDef> rs;
        if (!mp::LoadRaces(path, rs)) return false;
        m->race.SetRaces(std::move(rs));
        m->recordsPath = path + ".records";
        m->records = LoadRecords(m->recordsPath);
        fprintf(stderr, "[mp-server] %zu race(s) loaded from %s, %zu record(s) from %s\n", m->race.Races().size(), path.c_str(), m->records.size(),
            m->recordsPath.c_str());
        for (const auto& [id, pl] : m->players)
        {
            m->SendRaceList(pl.peer);
            m->Send(pl.peer, kChanEvent, m->RecordsMessage(), true);
        }
        return true;
    }
    int Server::RaceCount() const { return int(m->race.Races().size()); }
    int Server::PlayerCount() const { return int(m->players.size()); }

    void Server::Say(const std::string& text)
    {
        std::string line;
        for (char ch : text)
            if (line.size() < kMaxChat)
                line += (ch >= 32 && ch < 127) ? ch : '?';
        if (line.empty())
            return;
        m->SayAll(line);
    }

    std::string Server::Races() const
    {
        std::string out;
        for (const RaceDef& d : m->race.Races())
        {
            const auto it = m->records.find(d.name);
            char b[200];
            snprintf(b, sizeof b, "  %s: %d lap(s), %zu points, start %.0f %.0f %.0f, record %s\n", d.name.c_str(), d.laps, d.points.size(), double(d.points[0].x),
                double(d.points[0].y), double(d.points[0].z), it == m->records.end() || !it->second.ms ? "none" : (FormatRaceTime(it->second.ms) + " by " + it->second.name).c_str());
            out += b;
        }
        return out.empty() ? std::string("  (no races: mp_server <port> <races file>)\n") : out;
    }

    bool Server::Kick(const std::string& name)
    {
        for (auto& [id, pl] : m->players)
            if (pl.name == name)
            {
                m->SayAll(name + " was removed from the server");
                enet_peer_disconnect_later(pl.peer, 0);
                return true;
            }
        return false;
    }

    std::string Server::Players() const
    {
        std::string out;
        for (const auto& [id, pl] : m->players)
        {
            char b[160];
            snprintf(b, sizeof b, "  %u %s: car %016llx, %s%s\n", unsigned(id), pl.name.c_str(), (unsigned long long)pl.carKey, pl.ready ? "ready" : "not ready",
                pl.hasCar ? ", driving" : "");
            out += b;
        }
        return out.empty() ? std::string("  (no players)\n") : out;
    }
    uint16_t Server::Port() const { return m->port; }
}
