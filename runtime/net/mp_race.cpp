#include "mp_race.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mp
{
    namespace
    {
        float Dist(const Vec3& a, const Vec3& b)
        {
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        // How close the segment a-b came to p (a car moves 2.5 m or more between two reports at speed, so a point test alone could step over a small radius).
        float SegDist(const Vec3& a, const Vec3& b, const Vec3& p)
        {
            const float vx = b.x - a.x, vy = b.y - a.y, vz = b.z - a.z;
            const float len2 = vx * vx + vy * vy + vz * vz;
            float t = len2 > 1e-6f ? ((p.x - a.x) * vx + (p.y - a.y) * vy + (p.z - a.z) * vz) / len2 : 0.f;
            t = std::clamp(t, 0.f, 1.f);
            return Dist({ a.x + vx * t, a.y + vy * t, a.z + vz * t }, p);
        }
    }

    int RaceManager::StartMarkerFor(const Vec3& p) const
    {
        for (size_t i = 0; i < races_.size(); ++i)
            if (!races_[i].points.empty() && Dist(p, races_[i].points[0]) <= races_[i].radius)
                return int(i);
        return -1;
    }

    void RaceManager::Join(uint8_t id, int race, uint32_t nowMs)
    {
        Racer& r = players_[id];
        if (r.joined)
            return;
        r.joined = true;
        r.lap = 0;
        r.next = 1;
        r.finished = false;
        r.finishMs = 0;
        r.place = 0;
        ++version_;
        if (phase_ == RacePhase::Idle || phase_ == RacePhase::Finished)
        {
            phase_ = RacePhase::Gathering;
            race_ = race;
            phaseStart_ = nowMs;
            for (auto& [pid, o] : players_)
                if (pid != id) { o.joined = false; }
            finishedCount_ = 0;
        }
    }

    void RaceManager::Report(uint8_t id, const Vec3& pos, float speed, uint32_t nowMs)
    {
        Racer& r = players_[id];
        const Vec3 before = r.pos;
        const bool had = r.seen;
        // A jump of more than 200 m between two reports cannot be a car: ignore it for the race (a teleport or a damaged client does not pass checkpoints).
        const bool jumped = had && Dist(before, pos) > 200.f;
        r.pos = pos;
        r.speed = speed;
        r.lastMs = nowMs;
        r.seen = true;
        if (jumped)
            return;

        // Opening or joining: stopped inside the start marker of a race.
        const int marker = StartMarkerFor(pos);
        if (marker >= 0 && speed < stillSpeed)
        {
            if (!r.still) { r.still = true; r.stillSince = nowMs; }
            if (!r.joined && nowMs - r.stillSince >= holdStillMs &&
                (phase_ == RacePhase::Idle || phase_ == RacePhase::Finished || (phase_ == RacePhase::Gathering && marker == race_)))
                Join(id, marker, nowMs);
        }
        else
            r.still = false;

        // Racing: checkpoints in order, the finish line after the last one.
        if (phase_ == RacePhase::Running && r.joined && !r.finished && had)
        {
            const RaceDef& def = races_[size_t(race_)];
            const int n = int(def.points.size());
            if (n >= 2)
            {
                const Vec3& target = def.points[size_t(r.next % n)];
                if (SegDist(before, pos, target) <= def.radius)
                {
                    if (def.sprint && r.next >= n - 1)
                    {
                        r.lap = 1;
                        Finish(id, r, nowMs - goAt_);
                    }
                    else if (def.sprint)
                        ++r.next;
                    else if (r.next == 0 || r.next % n == 0)
                    {
                        ++r.lap;
                        r.next = 1;
                        if (r.lap >= def.laps)
                            Finish(id, r, nowMs - goAt_);
                    }
                    else
                        r.next = (r.next + 1 >= n) ? 0 : r.next + 1;
                    ++version_;
                    if (std::getenv("MP_RACE_LOG"))
                        fprintf(stderr, "[race] player %u passes a point: lap %d next %d at race time %u ms (pos %.0f %.0f %.0f)\n", unsigned(id), r.lap, r.next, nowMs - goAt_, double(pos.x), double(pos.y), double(pos.z));
                }
            }
        }
    }

    void RaceManager::Finish(uint8_t id, Racer& r, uint32_t raceMs)
    {
        r.finished = true;
        r.finishMs = raceMs ? raceMs : 1;
        r.place = ++finishedCount_;
        ++version_;
        finishes_.push_back({ id, race_, r.finishMs });
    }

    void RaceManager::Remove(uint8_t id)
    {
        if (players_.erase(id))
            ++version_;
    }

    float RaceManager::Progress(const Racer& r) const
    {
        if (race_ < 0) return 0;
        const RaceDef& def = races_[size_t(race_)];
        const int n = int(def.points.size());
        const int next = r.next % n;
        const Vec3& a = def.points[size_t((next + n - 1) % n)];
        const Vec3& b = def.points[size_t(next)];
        const float seg = std::max(1.f, Dist(a, b));
        const float frac = 1.f - std::clamp(Dist(r.pos, b) / seg, 0.f, 1.f);
        const int passed = (r.next == 0 ? n - 1 : r.next - 1);
        return float(r.lap * n + passed) + frac * 0.99f;
    }

    void RaceManager::Tick(uint32_t nowMs)
    {
        // Players that stopped reporting for 3 s are not part of anything.
        for (auto it = players_.begin(); it != players_.end();)
            if (it->second.seen && nowMs - it->second.lastMs > 3000 && !it->second.joined)
                it = players_.erase(it);
            else
                ++it;

        switch (phase_)
        {
        case RacePhase::Gathering:
            if (nowMs - phaseStart_ >= gatherMs)
            {
                phase_ = RacePhase::Countdown;
                phaseStart_ = nowMs;
                goAt_ = nowMs + countdownMs;
                ++version_;
            }
            break;
        case RacePhase::Countdown:
            if (int32_t(nowMs - goAt_) >= 0)
            {
                phase_ = RacePhase::Running;
                phaseStart_ = nowMs;
                ++version_;
            }
            break;
        case RacePhase::Running:
        {
            bool all = true, any = false;
            for (auto& [id, r] : players_)
                if (r.joined) { any = true; if (!r.finished) all = false; }
            // Over when every racer has finished or left, or one has finished and the rest had 60 s more.
            const bool someone = finishedCount_ > 0;
            if (!any || all)
            {
                phase_ = RacePhase::Finished; phaseStart_ = nowMs; ++version_;
            }
            else if (someone && nowMs - phaseStart_ > 0)
            {
                uint32_t firstFinish = 0xFFFFFFFFu;
                for (auto& [id, r] : players_)
                    if (r.joined && r.finished) firstFinish = std::min(firstFinish, r.finishMs);
                if (firstFinish != 0xFFFFFFFFu && nowMs - goAt_ > firstFinish + 60000)
                {
                    phase_ = RacePhase::Finished; phaseStart_ = nowMs; ++version_;
                }
            }
            break;
        }
        case RacePhase::Finished:
            if (nowMs - phaseStart_ >= finishedHoldMs)
            {
                phase_ = RacePhase::Idle;
                race_ = -1;
                for (auto& [id, r] : players_) { r.joined = false; r.still = false; }
                ++version_;
            }
            break;
        default: break;
        }
    }

    RaceView RaceManager::View(uint32_t nowMs) const
    {
        RaceView v;
        v.race = race_;
        v.phase = phase_;
        switch (phase_)
        {
        case RacePhase::Gathering: v.timeMs = nowMs - phaseStart_ >= gatherMs ? 0 : gatherMs - (nowMs - phaseStart_); break;
        case RacePhase::Countdown: v.timeMs = int32_t(goAt_ - nowMs) > 0 ? goAt_ - nowMs : 0; break;
        case RacePhase::Running: v.timeMs = nowMs - goAt_; break;
        case RacePhase::Finished: v.timeMs = 0; break;
        default: break;
        }
        std::vector<std::pair<float, RacerView>> order;
        for (const auto& [id, r] : players_)
        {
            if (!r.joined) continue;
            RacerView rv;
            rv.id = id; rv.lap = uint8_t(r.lap); rv.next = uint8_t(r.next); rv.finished = r.finished; rv.finishMs = r.finishMs;
            float key = r.finished ? 1.0e6f - float(r.place) : (race_ >= 0 && phase_ >= RacePhase::Running ? Progress(r) : 0.f);
            order.push_back({ key, rv });
        }
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        uint8_t place = 1;
        for (auto& [key, rv] : order) { rv.place = place++; v.racers.push_back(rv); }
        if (phase_ == RacePhase::Idle) { v.race = -1; }
        return v;
    }

    bool LoadRaces(const std::string& path, std::vector<RaceDef>& out)
    {
        FILE* f = fopen(path.c_str(), "r");
        if (!f)
            return false;
        char line[256];
        while (fgets(line, sizeof line, f))
        {
            if (char* hash = strchr(line, '#')) *hash = 0;
            char name[64] = {};
            float x, y, z;
            if (strncmp(line, "race ", 5) == 0 && sscanf(line + 5, "%63s", name) == 1)
            {
                RaceDef d;
                d.name = name;
                if (const char* l = strstr(line, "laps=")) d.laps = std::clamp(atoi(l + 5), 1, 50);
                if (const char* r = strstr(line, "radius=")) d.radius = std::clamp(float(atof(r + 7)), 5.f, 200.f);
                if (strstr(line, " sprint")) { d.sprint = true; d.laps = 1; }
                out.push_back(d);
            }
            else if (strncmp(line, "point ", 6) == 0 && !out.empty() && sscanf(line + 6, "%f %f %f", &x, &y, &z) == 3 && out.back().points.size() < 64)
                out.back().points.push_back({ x, y, z });
        }
        fclose(f);
        out.erase(std::remove_if(out.begin(), out.end(), [](const RaceDef& d) { return d.points.size() < 2; }), out.end());
        return true;
    }

    std::map<std::string, RaceRecord> LoadRecords(const std::string& path)
    {
        std::map<std::string, RaceRecord> out;
        FILE* f = fopen(path.c_str(), "r");
        if (!f)
            return out;
        char line[256];
        while (fgets(line, sizeof line, f))
        {
            char race[64] = {};
            unsigned ms = 0;
            int used = 0;
            if (strncmp(line, "record ", 7) != 0 || sscanf(line + 7, "%63s %u %n", race, &ms, &used) < 2 || !ms)
                continue;
            std::string name = line + 7 + used;
            while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' '))
                name.pop_back();
            out[race] = { ms, name.empty() ? std::string("player") : name };
        }
        fclose(f);
        return out;
    }

    bool SaveRecords(const std::string& path, const std::map<std::string, RaceRecord>& records)
    {
        const std::string tmp = path + ".new";
        FILE* f = fopen(tmp.c_str(), "w");
        if (!f)
            return false;
        fprintf(f, "# The best time of each race (written by mp_server): record <race> <ms> <player>\n");
        for (const auto& [race, r] : records)
            if (r.ms)
                fprintf(f, "record %s %u %s\n", race.c_str(), r.ms, r.name.c_str());
        const bool ok = fclose(f) == 0;
        std::remove(path.c_str());   // (rename doesn't replace a file on Windows)
        return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
    }

    std::string FormatRaceTime(uint32_t ms)
    {
        char b[32];
        snprintf(b, sizeof b, "%u:%02u.%u", ms / 60000, (ms / 1000) % 60, (ms / 100) % 10);
        return b;
    }
}
