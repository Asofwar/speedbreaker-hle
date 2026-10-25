// The rules of a race in the open world (the server runs them, net/mp_server.cpp; no game code and no network in here, so tests/mp_race_test.cpp can run it).
// A race has a start marker, checkpoints and laps. A player who stops inside the start marker opens the race: others can join the same way during a short window, then a
// countdown, then the race. The server decides everything from the positions the games report: passing a checkpoint, laps, places, finishing times. Players who are not in the race
// are not part of it; they drive in the same world and can get in the way.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mp
{
    struct Vec3 { float x = 0, y = 0, z = 0; };

    struct RaceDef
    {
        std::string name = "race";
        int laps = 1;
        float radius = 25.f;           // metres: how close a car must come to a checkpoint or the start marker
        std::vector<Vec3> points;      // [0] the start marker (and the line the laps end on), then the checkpoints in order
    };

    enum class RacePhase : uint8_t { Idle = 0, Gathering = 1, Countdown = 2, Running = 3, Finished = 4 };

    struct RacerView
    {
        uint8_t id = 0;
        uint8_t lap = 0;        // laps completed
        uint8_t next = 0;       // index of the next point to pass (1..n-1, 0 = the finish line after the last point)
        uint8_t place = 0;      // 1 = first
        uint32_t finishMs = 0;  // race time at the finish, 0 while racing
        bool finished = false;
    };

    struct RaceView
    {
        int race = -1;                 // index in the list, -1 none
        RacePhase phase = RacePhase::Idle;
        uint32_t timeMs = 0;           // Gathering/Countdown: time left; Running/Finished: race time
        std::vector<RacerView> racers; // in place order
    };

    // A text file of races: "race <name> laps=2 radius=25" starts one, then one "point x y z" line each (the first is the start marker); '#' starts a comment.
    bool LoadRaces(const std::string& path, std::vector<RaceDef>& out);

    // A racer crossed the finish line: who, in which race, the race time.
    struct FinishEvent
    {
        uint8_t id = 0;
        int race = -1;
        uint32_t ms = 0;
    };

    // The best time of a race (ms 0: none yet) and who drove it.
    struct RaceRecord
    {
        uint32_t ms = 0;
        std::string name;
    };
    // The records of a races file, by race name: "record <race> <ms> <player name>" lines in <races file>.records (the server writes it on every new record).
    std::map<std::string, RaceRecord> LoadRecords(const std::string& path);
    bool SaveRecords(const std::string& path, const std::map<std::string, RaceRecord>& records);
    // A finish beats the record (none yet, or faster).
    inline bool Beats(const RaceRecord& record, uint32_t ms) { return ms && (!record.ms || ms < record.ms); }
    // "1:23.4"
    std::string FormatRaceTime(uint32_t ms);

    class RaceManager
    {
    public:
        // Timings (ms).
        uint32_t gatherMs = 15000, countdownMs = 5000, finishedHoldMs = 20000, holdStillMs = 1500;
        float stillSpeed = 3.0f;       // m/s: "stopped" in the start marker

        void SetRaces(std::vector<RaceDef> races) { races_ = std::move(races); }
        const std::vector<RaceDef>& Races() const { return races_; }

        // A game reported its car: position, speed (m/s). Call for every state received (about 30 a second).
        void Report(uint8_t id, const Vec3& pos, float speed, uint32_t nowMs);
        void Remove(uint8_t id);        // the player left
        // Advance the clock; call often (the server's tick).
        void Tick(uint32_t nowMs);

        RaceView View(uint32_t nowMs) const;
        RacePhase Phase() const { return phase_; }
        // Bumps whenever the phase or the field changes, so the server sends the state at once then.
        uint32_t Version() const { return version_; }
        // The finishes since the last call, in order.
        std::vector<FinishEvent> TakeFinishes() { std::vector<FinishEvent> out; out.swap(finishes_); return out; }

    private:
        struct Racer
        {
            Vec3 pos; float speed = 0; uint32_t lastMs = 0; bool seen = false;
            uint32_t stillSince = 0; bool still = false;
            bool joined = false;
            int lap = 0, next = 1;
            bool finished = false; uint32_t finishMs = 0; int place = 0;
        };
        int StartMarkerFor(const Vec3& p) const;
        void Join(uint8_t id, int race, uint32_t nowMs);
        void Finish(uint8_t id, Racer& r, uint32_t raceMs);
        float Progress(const Racer& r) const;

        std::vector<RaceDef> races_;
        std::map<uint8_t, Racer> players_;
        RacePhase phase_ = RacePhase::Idle;
        int race_ = -1;
        uint32_t phaseStart_ = 0, goAt_ = 0, version_ = 1;
        int finishedCount_ = 0;
        std::vector<FinishEvent> finishes_;
    };
}
