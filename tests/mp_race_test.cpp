// SpeedBreaker runtime tests. GPL-3.0-or-later (see COPYING).
//
// The open world's race rules (net/mp_race.h) without a game or a network:
//   - a player who stops in the start marker opens the race, a second one
//     joins during the window, the countdown runs, then the race;
//   - checkpoints in order and laps decide the finish, places by order; a
//     sprint ends at its last point, without going back to the start;
//   - a jump of more than 200 m (a teleport) passes no checkpoint;
//   - every finish comes out of TakeFinishes once, with its race time;
//   - records: Beats, and the records file round trip (names with spaces).
// Planted bugs this catches (each tried): Finish not reporting the event
// (no record is ever kept), Beats taking an equal time, a record's name cut
// at its first space.
#include <net/mp_race.h>

#include <cstdio>
#include <filesystem>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace mp;

int main()
{
    RaceDef d;
    d.name = "loop";
    d.laps = 2;
    d.radius = 10.f;
    d.points = { { 0, 0, 0 }, { 300, 0, 0 }, { 300, 0, 300 } };
    RaceManager rm;
    rm.SetRaces({ d });
    uint32_t t = 1000;

    printf("Opening and joining\n");
    rm.Report(1, { 0, 0, 0 }, 0.f, t);
    rm.Report(2, { 3, 0, 3 }, 0.f, t);
    t += 1600;
    rm.Report(1, { 0, 0, 0 }, 0.f, t);
    rm.Report(2, { 3, 0, 3 }, 0.f, t);
    rm.Tick(t);
    CHECK(rm.Phase() == RacePhase::Gathering, "phase %d after both stopped in the marker", int(rm.Phase()));
    CHECK(rm.View(t).racers.size() == 2, "%zu racers", rm.View(t).racers.size());
    t += rm.gatherMs;
    rm.Tick(t);
    CHECK(rm.Phase() == RacePhase::Countdown, "phase %d after the window", int(rm.Phase()));
    t += rm.countdownMs;
    rm.Tick(t);
    CHECK(rm.Phase() == RacePhase::Running, "phase %d after the countdown", int(rm.Phase()));
    const uint32_t go = t;

    printf("Racing: checkpoints, laps, a teleport\n");
    // Player 2 teleports onto the first checkpoint, 297 m away: not passed.
    rm.Report(2, { 300, 0, 0 }, 40.f, t + 10);
    // Player 1 drives the loop twice, about 30 s a lap, each leg in four steps (under 200 m each).
    Vec3 at{ 0, 0, 0 };
    auto drive = [&](const Vec3& to, uint32_t ms) {
        const Vec3 from = at;
        for (int k = 1; k <= 4; ++k)
        {
            t += ms / 4;
            at = { from.x + (to.x - from.x) * k / 4, from.y, from.z + (to.z - from.z) * k / 4 };
            rm.Report(1, at, 40.f, t);
            rm.Tick(t);
        }
    };
    for (int lap = 0; lap < 2; ++lap)
    {
        drive({ 300, 0, 0 }, 8000);
        drive({ 300, 0, 300 }, 8000);
        drive({ 0, 0, 0 }, 12000);
    }
    const RaceView v = rm.View(t);
    const RacerView* one = nullptr;
    const RacerView* two = nullptr;
    for (const RacerView& r : v.racers)
        (r.id == 1 ? one : two) = &r;
    CHECK(one && one->finished && one->place == 1, "player 1 finished first (%d, place %d)", one ? int(one->finished) : -1, one ? int(one->place) : -1);
    CHECK(one && one->finishMs == t - go, "player 1's time %u ms, want %u", one ? one->finishMs : 0, t - go);
    CHECK(two && !two->finished && two->next == 1, "the teleport passed no checkpoint (next %d)", two ? int(two->next) : -1);

    printf("Finishes, once each\n");
    const std::vector<FinishEvent> f = rm.TakeFinishes();
    CHECK(f.size() == 1 && f[0].id == 1 && f[0].race == 0 && f[0].ms == t - go, "%zu finish(es)", f.size());
    CHECK(rm.TakeFinishes().empty(), "a second TakeFinishes is empty");

    printf("A sprint: start to the last point, once\n");
    {
        RaceDef sp;
        sp.name = "dash";
        sp.sprint = true;
        sp.radius = 10.f;
        sp.points = { { 0, 0, 0 }, { 150, 0, 0 }, { 300, 0, 0 } };
        RaceManager sm;
        sm.SetRaces({ sp });
        uint32_t st = 1000;
        sm.Report(7, { 0, 0, 0 }, 0.f, st);
        st += 1600;
        sm.Report(7, { 0, 0, 0 }, 0.f, st);
        st += sm.gatherMs;
        sm.Tick(st);
        st += sm.countdownMs;
        sm.Tick(st);
        CHECK(sm.Phase() == RacePhase::Running, "sprint phase %d", int(sm.Phase()));
        for (float x : { 75.f, 150.f, 225.f })
        {
            st += 2000;
            sm.Report(7, { x, 0, 0 }, 40.f, st);
        }
        CHECK(!sm.View(st).racers.empty() && !sm.View(st).racers[0].finished, "not finished before the last point");
        st += 2000;
        sm.Report(7, { 300, 0, 0 }, 40.f, st);
        const RaceView sv = sm.View(st);
        CHECK(!sv.racers.empty() && sv.racers[0].finished && sv.racers[0].finishMs == 8000, "finished at the last point (%u ms)", sv.racers.empty() ? 0 : sv.racers[0].finishMs);
        sm.Tick(st);
        CHECK(sm.Phase() == RacePhase::Finished, "the race is over (%d)", int(sm.Phase()));
    }

    printf("Records\n");
    CHECK(Beats({}, 1), "the first time is a record");
    CHECK(Beats({ 50000, "a" }, 49999) && !Beats({ 50000, "a" }, 50000) && !Beats({ 50000, "a" }, 0), "only a faster time beats one");
    CHECK(FormatRaceTime(83456) == "1:23.4", "%s", FormatRaceTime(83456).c_str());
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "mp_race_test.records";
    std::map<std::string, RaceRecord> recs = { { "loop", { 40000, "Big Al" } }, { "sprint", { 12345, "x" } } };
    CHECK(SaveRecords(file.string(), recs), "saved to %s", file.string().c_str());
    recs["loop"] = { 39000, "Big Al 2" };
    CHECK(SaveRecords(file.string(), recs), "saved again over the old file");
    const std::map<std::string, RaceRecord> back = LoadRecords(file.string());
    CHECK(back.size() == 2 && back.at("loop").ms == 39000 && back.at("loop").name == "Big Al 2" && back.at("sprint").ms == 12345,
        "%zu record(s) read back, loop %u by '%s'", back.size(), back.count("loop") ? back.at("loop").ms : 0, back.count("loop") ? back.at("loop").name.c_str() : "");
    std::filesystem::remove(file);
    CHECK(LoadRecords(file.string()).empty(), "no file: no records");

    if (g_failures)
    {
        printf("%d failure(s)\n", g_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
