// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See frame_time.h.
//
// NFSMW times itself with mftb. The main loop (sub_823B0008) measures each
// frame and hands it, in milliseconds as 16.16 fixed point, to the frame
// (sub_823AFE20). That quantises it to 1/3600 s (sub_823C6500; the frame's
// time for its real-time users, [0x82A39660]), advances RealTimeFrames and a
// 4000 Hz tick counter at 0x82A399AC (AdvanceRealTime, 0x823C66C8), and runs
// the world simulation in fixed 1/60 s steps from an accumulator over those
// ticks (sub_823A2348: floor(elapsed / step) steps, the remainder kept; the
// step is stored once, at boot, and tasks copy it). There is no interpolation:
// a frame's world state is its steps'.
//
// So a frame that isn't 1/60 s long shows it. With the vblank locked to the
// player's 59.9726 Hz monitor, a frame lasts 16.674 ms and every 36.6 s one
// runs two steps: the car jumps, where the presenter used to drop a frame.
// And at any rate a frame's measured length jitters (the main thread's
// wake-up, the 250 us ticks), so while the accumulator's remainder sits near
// a step boundary, frames alternate between no step and two.
//
// Each guest vblank is made one step instead (NFSMW_GAME_TIME=0 turns this
// off). A frame is given whole vblanks of 1/60 s, as the console's 60 Hz
// vblank would have given it; the stepper's elapsed time, then k steps give
// or take a tick, is rounded to k. The main loop runs ahead of the display:
// the game's render thread swaps (VdSwap) during the loop's next frame, and
// the command processor lets each swap go at a vblank about two frames after
// that, so the loop's frames drift against the vblanks (counted from one
// frame's start to the next) while the display shows one frame a vblank from
// the queue: a frame the loop is slow with counts 2 or 3, the next ones none.
// Each is given one vblank while game time stays within a few vblanks of the
// vblanks that fired (game/whole_vblanks.h; given their counts, the world
// ran 2-3 steps in one displayed frame and none in the next: GitHub issue
// #9). Further behind, the queue ran dry and the display waited, and the
// frame gets the rest; a frame slower than the vblank (GPU-bound) gets its
// count, its length rounded either way. Game time adds up to the vblanks
// that fired, to within those few. It runs at the display's rate over 60
// (over 120, 180 or 240 on such a display, whose refreshes a vblank is 2-4
// of: video/vblank_lock.h): 0.044% slow on a 59.97 Hz monitor, at most 2.5%
// (the lock's range). Frames
// of more than 6 vblanks (a hitch, loading) keep their measured time; below
// 20 fps, where they are the frame rate's own, only those over half a second
// do, the rest get 6 (the stepper's own cap) and later frames give back what
// that leaves owed, so game time keeps up down to 10 fps. At Frame Rate 30
// (video::GuestVblankDivider) a guest vblank comes every other refresh and is
// given 2/60 s: two steps a frame, the game at its speed.
//
// A timedemo (game/timedemo.h) takes its own path. Its capture logs every
// frame's time and its replay plays those back whatever r3 says, so whole
// vblanks, which follow the host's display, would only make the capture
// depend on the machine it was made on: a capture with a fixed step gives
// each frame exactly 1/60 s instead (the quantiser hands out 60, 60, 59
// 3600ths of a second, the 4000 Hz clock +67, +67, +66). The stepper's
// rounding below isn't in the log, so it runs in capture and replay alike,
// whatever NFSMW_GAME_TIME and the display: one step a frame from those
// ticks, and the same steps from the same ticks in every run.
#include <stdafx.h>
#include "frame_time.h"
#include "timedemo.h"
#include "whole_vblanks.h"

#include "mp_adapter.h"

#include <debug/bench.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <video/presenter.h>
#include <video/vblank_lock.h>

namespace
{
    const bool s_enabled = [] {
        const char* v = std::getenv("NFSMW_GAME_TIME");
        bool on = !v || v[0] != '0';
        if (!on)
            fprintf(stderr, "[game] game time as measured (NFSMW_GAME_TIME=0)\n");
        return on;
    }();
    std::atomic<uint64_t> s_frames{ 0 }, s_snapped{ 0 }, s_noStep{ 0 }, s_multiStep{ 0 };
    // Game time handed to the frames so far (game::FrameClockMs), ms in 16.16.
    std::atomic<uint64_t> s_clock{ 0 };
    // NFSMW_GAME_TIME_LOG=<file>: one line per frame, written when the next
    // one starts: its measured ms, guest vblanks, whether it was given whole
    // vblanks, the world steps it ran (-1: no world), how long after the last
    // vblank it started (us), and the guest vblanks it was given (0: its
    // measured time).
    FILE* const s_log = [] {
        const char* path = std::getenv("NFSMW_GAME_TIME_LOG");
        FILE* f = path ? fopen(path, "w") : nullptr;
        if (f)
            fprintf(f, "measured_ms,vblanks,snapped,steps,after_vblank_us,given\n");
        return f;
    }();
    struct LogLine
    {
        double measuredMs = 0, afterVblankUs = 0;
        uint64_t vblanks = 0, given = 0;
        int steps = -1;
        bool snapped = false, pending = false;
    } s_line;  // main thread only
    game::WholeVblanks s_whole;  // main thread only
    std::atomic<bool> s_hold{ false };          // game::SetWorldHold
    std::atomic<uint64_t> s_worldFrames{ 0 };   // game::WorldFrameIndex

    // Whole vblanks are game time only at the lock's rates: NFSMW_VBLANK_HZ
    // far from 60 keeps game time as measured, in the stepper too.
    bool NearSixty(double vblankMs)
    {
        return vblankMs >= 1000.0 / video::RefreshFit::kMaxHz && vblankMs <= 1000.0 / video::RefreshFit::kMinHz;
    }
}

namespace game
{
    FrameTimeStats GetFrameTimeStats()
    {
        return { s_frames.load(std::memory_order_relaxed), s_noStep.load(std::memory_order_relaxed),
            s_multiStep.load(std::memory_order_relaxed), s_snapped.load(std::memory_order_relaxed) };
    }

    double FrameClockMs()
    {
        return double(s_clock.load(std::memory_order_relaxed)) / 65536.0;
    }

    void SetWorldHold(bool hold) { s_hold.store(hold, std::memory_order_relaxed); }
    bool WorldHeld() { return s_hold.load(std::memory_order_relaxed); }
    uint64_t WorldFrameIndex() { return s_worldFrames.load(std::memory_order_relaxed); }
}

// GameFrameTime, before `bl 0x823AFE20` at 0x823B0148 in the main loop: r3 is
// the frame's measured time, milliseconds in 16.16 fixed point.
void GameFrameTime(PPCRegister& r3)
{
    // Whatever the frame is given below (or its measured time), on every path.
    struct AddToClock
    {
        PPCRegister& r3;
        ~AddToClock() { s_clock.fetch_add(uint64_t(std::max(int32_t(r3.u32), 0)), std::memory_order_relaxed); }
    } addToClock{ r3 };
    static uint64_t last = 0;
    uint64_t now = video::GuestVblanks(), vblanks = now - last;
    last = now;
    // Multiplayer (game/mp_adapter.cpp): its connection and race tracking
    // every frame, the start barrier's hold, and the agreed race clock.
    game::MpFramePump();
    if (game::WorldHeld())
    {
        // No time passes for the world while it is held: not in the clock,
        // not in the stepper's accumulator.
        r3.u64 = 0;
        s_frames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Follow the agreed race clock by giving the frame a little more or less
    // time (at most 100% more, 50% less), so a game whose loading made its
    // game time lag (or run ahead) closes the gap within a few seconds,
    // before the cars leave the line. Nothing outside a multiplayer race.
    struct FollowRaceClock
    {
        PPCRegister& r3;
        ~FollowRaceClock()
        {
            const double want = game::MpWantedClock();
            if (want < 0)
                return;
            const uint32_t bits = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(g_memory.base + 0x82A153B8));
            float clockNow;
            memcpy(&clockNow, &bits, 4);
            if (!(clockNow > 0.0f) || !std::isfinite(clockNow))
                return;
            const double err = want - double(clockNow);  // seconds the world is behind (positive) or ahead
            if (std::abs(err) < 0.02 || std::abs(err) > 30.0)
                return;
            const double frameMs = double(int32_t(r3.u32)) / 65536.0;
            if (frameMs <= 0.0)
                return;
            const double extraMs = std::clamp(err * 1000.0 * 0.25, -0.5 * frameMs, 1.0 * frameMs);
            r3.u64 = uint32_t(std::llround((frameMs + extraMs) * 65536.0));
        }
    } followRaceClock{ r3 };
    s_frames.fetch_add(1, std::memory_order_relaxed);
    double measuredMs = double(int32_t(r3.u32)) / 65536.0;
    if (s_log)
    {
        if (s_line.pending)
            fprintf(s_log, "%.3f,%llu,%d,%d,%.0f,%llu\n", s_line.measuredMs, (unsigned long long)s_line.vblanks, s_line.snapped,
                s_line.steps, s_line.afterVblankUs, (unsigned long long)s_line.given);
        int64_t since = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() -
            video::LastGuestVblank();
        s_line = { measuredMs, double(since) / 1e3, vblanks, 0, -1, false, true };
    }
    // A timedemo's frame (before the frame runs), the benchmark's, and a
    // capture's fixed step (see the top).
    if (timedemo::Active())
    {
        timedemo::OnMainFrame();
        bench::OnMainFrame();
        if (timedemo::FixedStep())
        {
            r3.u64 = timedemo::kFixedFrame;
            s_snapped.fetch_add(1, std::memory_order_relaxed);
            s_line.snapped = true;
            s_line.given = 1;
            return;
        }
    }
    if (!s_enabled)
        return;
    double refreshMs = video::GuestVblank().PeriodNs() / 1e6;
    if (!NearSixty(refreshMs))
    {
        s_whole.Reset();
        return;
    }
    // At Frame Rate 30 a guest vblank is two refreshes, and two 1/60 s steps.
    int divider = video::GuestVblankDivider();
    uint64_t given = s_whole.Give(measuredMs, vblanks, refreshMs * divider);
    if (!given)
        return;
    r3.u64 = uint32_t(std::llround(double(given * divider) * (1000.0 / 60.0) * 65536.0));
    s_snapped.fetch_add(1, std::memory_order_relaxed);
    s_line.snapped = true;
    s_line.given = given;
}

// WorldStepTime, before `fadds f0,f10,f9` at 0x823A23E8 in the stepper: f10 is
// the elapsed time in steps (f9 the remainder it joins).
void WorldStepTime(PPCRegister& f10)
{
    // A timedemo rounds whatever the display and NFSMW_GAME_TIME (see the top).
    if (!timedemo::Active() && (!s_enabled || !NearSixty(video::GuestVblank().PeriodNs() / 1e6)))
        return;
    double k = std::round(f10.f64);
    if (k >= 1 && k <= 6 && std::abs(f10.f64 - k) <= 0.05)
        f10.f64 = k;
}

// WorldSteps, at `mr r4,r30` (0x823A2424) in the stepper: r30 is the number of
// steps this frame runs, before the step runner's cap.
void WorldSteps(PPCRegister& r30)
{
    s_worldFrames.fetch_add(1, std::memory_order_relaxed);
    if (game::WorldHeld())
        r30.u32 = 0;
    s_line.steps = int(r30.u32);
    if (r30.u32 == 0)
        s_noStep.fetch_add(1, std::memory_order_relaxed);
    else if (r30.u32 >= 2)
        s_multiStep.fetch_add(1, std::memory_order_relaxed);
}
