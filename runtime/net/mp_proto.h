// Wire format of the multiplayer layer (net/mp_*). Little-endian, fixed layouts, no pointers: every message is a
// one-byte type followed by its body. Unreliable messages go on channel 0 (car states, snapshots), reliable ones on
// channel 1 (join, leave, chat, events). Bump kProtocolVersion on any change.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mp
{
    constexpr uint16_t kDefaultPort = 27960;
    constexpr uint32_t kProtocolVersion = 10;
    constexpr size_t kMaxChat = 160;        // a chat line's characters (Writer::text)
    constexpr uint8_t kServerId = 0xFF;     // Chat's sender id for the server's own lines (its console)
    constexpr int kMaxPlayers = 16;
    constexpr int kChanState = 0;   // unreliable, latest wins
    constexpr int kChanEvent = 1;   // reliable, ordered
    constexpr int kChannels = 2;

    enum class Msg : uint8_t
    {
        Hello = 1,      // C->S reliable: version, name
        Welcome,        // S->C reliable: your id, server tick rate
        Reject,         // S->C reliable: reason code
        PlayerJoin,     // S->C reliable: id, name
        PlayerLeave,    // S->C reliable: id
        CarState,       // C->S unreliable: the sender's car
        Snapshot,       // S->C unreliable: every other player's latest car, one message per server tick
        Chat,           // C->S reliable: text; S->C reliable: sender id (kServerId: the server), text. Up to kMaxChat printable ASCII characters
        PlayerInfo,     // C->S reliable: the car I chose in the lobby (type key, 0: none), whether I am ready to race and the race my game has selected (the front end's
                        // race key, 0: none); S->C reliable: id, car key, ready, race key
        Hold,           // C->S reliable: my game is held at the start of a race: players the race needs (u8), my world clock (f32)
        Unhold,         // C->S reliable: I leave the start barrier without a Go (cancelled, or the race was left)
        Barrier,        // S->C reliable: how many players are held at the start, how many the race needs (u8, u8); sent on every change to the held players
        RaceList,       // S->C reliable: the races the server runs: count, then per race name, laps, flags (bit 0: a sprint), radius, points (the first is the start marker)
        RaceState,      // S->C reliable, at most 5 a second: the race in progress: race index (255 none), phase, time (ms), racers (id, lap, next point, place, finish time)
        Go,             // S->C reliable: the server released the start: the slowest world clock (f32), and in how many ms (from receipt, round trip allowed for) that clock's game starts (u32)
        Records,        // S->C reliable, after RaceList and on every new record: count (the races'), then per race its best time (u32 ms, 0 none) and who drove it (str)
    };

    constexpr uint16_t kFlagReady = 0x10;   // CarState::flags: this game is held at the start barrier and ready to go

    enum RejectReason : uint8_t { kRejectVersion = 1, kRejectFull = 2 };

    // One car's physical state as the game's own physics produced it on its owner's machine.
    struct CarState
    {
        uint32_t seq = 0;          // owner's counter, newer wins
        uint32_t ownerTimeMs = 0;  // owner's clock (for interpolation delay)
        float pos[3] = {};
        float rot[4] = {0, 0, 0, 1};   // quaternion x y z w
        float vel[3] = {};
        float angVel[3] = {};
        float steer = 0;           // -1..1
        float throttle = 0;        // 0..1
        float brake = 0;           // 0..1
        uint16_t carId = 0;        // which car model (game's own id), so the other side can spawn it
        uint16_t flags = 0;        // bit 0: handbrake, bit 1: nitrous, bit 2: horn, bit 4 (kFlagReady): held at the start barrier
        float raceClock = 0;       // the sender's world clock (seconds, game/frame_time: 0x82A153B8) when the state was taken, to compare the two games' clocks
    };

    // A state that a game's own physics could have produced: finite numbers in a plausible range and a quaternion that is roughly a unit. A damaged or modified
    // client could send anything, and a remote car is written into the physics of every other game, so both the server and the clients drop what fails this.
    inline bool Sane(const CarState& c)
    {
        auto ok = [](float v, float lim) { return v == v && v > -lim && v < lim; };   // NaN fails v == v, infinities fail the limit
        for (float f : c.pos) if (!ok(f, 100000.f)) return false;
        for (float f : c.vel) if (!ok(f, 600.f)) return false;
        for (float f : c.angVel) if (!ok(f, 200.f)) return false;
        float n = 0;
        for (float f : c.rot) { if (!ok(f, 4.f)) return false; n += f * f; }
        return n > 0.25f && n < 4.f && ok(c.raceClock, 1.0e7f);
    }

    // A player name as the others see it: printable ASCII only (the rest becomes '?'), 24 characters at most, never empty.
    inline std::string SanitizeName(const std::string& in)
    {
        std::string out;
        for (char ch : in)
            if (out.size() < 24)
                out += (ch >= 32 && ch < 127) ? ch : '?';
        return out.empty() ? std::string("player") : out;
    }

    // Writer/reader over a byte vector.
    class Writer
    {
    public:
        explicit Writer(Msg t) { u8(uint8_t(t)); }
        void u8(uint8_t v) { b.push_back(v); }
        void u16(uint16_t v) { raw(&v, 2); }
        void u32(uint32_t v) { raw(&v, 4); }
        void f32(float v) { raw(&v, 4); }
        void str(const std::string& s)
        {
            u8(uint8_t(s.size() > 31 ? 31 : s.size()));
            b.insert(b.end(), s.begin(), s.begin() + (s.size() > 31 ? 31 : s.size()));
        }
        // A longer string (chat), up to `max` bytes; Reader::str reads it.
        void text(const std::string& s, size_t max = kMaxChat)
        {
            const size_t n = std::min<size_t>({ s.size(), max, 255 });
            u8(uint8_t(n));
            b.insert(b.end(), s.begin(), s.begin() + n);
        }
        void car(const CarState& c)
        {
            u32(c.seq); u32(c.ownerTimeMs);
            for (float f : c.pos) f32(f);
            for (float f : c.rot) f32(f);
            for (float f : c.vel) f32(f);
            for (float f : c.angVel) f32(f);
            f32(c.steer); f32(c.throttle); f32(c.brake);
            u16(c.carId); u16(c.flags); f32(c.raceClock);
        }
        std::vector<uint8_t> b;
    private:
        void raw(const void* p, size_t n) { const uint8_t* s = (const uint8_t*)p; b.insert(b.end(), s, s + n); }
    };

    class Reader
    {
    public:
        Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
        bool ok() const { return !bad_; }
        Msg type() { return Msg(u8()); }
        uint8_t u8() { uint8_t v = 0; raw(&v, 1); return v; }
        uint16_t u16() { uint16_t v = 0; raw(&v, 2); return v; }
        uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
        float f32() { float v = 0; raw(&v, 4); return v; }
        std::string str()
        {
            size_t l = u8();
            if (l > n_ - p_ ) { bad_ = true; return {}; }
            std::string s((const char*)d_ + p_, l);
            p_ += l;
            return s;
        }
        CarState car()
        {
            CarState c;
            c.seq = u32(); c.ownerTimeMs = u32();
            for (float& f : c.pos) f = f32();
            for (float& f : c.rot) f = f32();
            for (float& f : c.vel) f = f32();
            for (float& f : c.angVel) f = f32();
            c.steer = f32(); c.throttle = f32(); c.brake = f32();
            c.carId = u16(); c.flags = u16(); c.raceClock = f32();
            return c;
        }
    private:
        void raw(void* o, size_t n)
        {
            if (n > n_ - p_) { bad_ = true; return; }
            memcpy(o, d_ + p_, n);
            p_ += n;
        }
        const uint8_t* d_;
        size_t n_, p_ = 0;
        bool bad_ = false;
    };
}
