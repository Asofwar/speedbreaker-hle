// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The game's Direct3D GPU waits. sub_825A5B20 is D3D's GPU hang check,
// called once per poll by each of its eight busy-wait loops (fence, ring
// space, idle: 0x82596364, 0x82596908, 0x82597208, 0x82597534, 0x825A995C,
// 0x825AA0E8, 0x825AAF08, 0x825AB234). It is overridden here to back those
// waits off to short sleeps, to park them while the game is suspended, and
// to report, with NFSMW_LOG_D3D_WAIT=1, which counter they poll and how often.
#include <stdafx.h>
#include <region/region.h>
#include <kernel/function.h>
#include <cpu/guest_time.h>
#include <cpu/host_cpu.h>
#include <gpu/cp_doorbell.h>
#include <cpu/backoff.h>
#include "timeline.h"

extern "C" PPC_FUNC(__imp__sub_825A5B20);

namespace
{
    // On the 360 a spinning hardware thread cost nothing. Here the game's
    // main thread spun a whole core at 100% while it waited for the command
    // processor, and on the Steam Machine's CPU (two fast cores, four slow
    // ones: see hostcpu::ReserveFastCore) the scheduler kept the spinning
    // thread on a fast core. A wait still spins for its first 20 us, so short
    // waits end on time. After that each poll sleeps until the GPU side next
    // stores something such a loop may poll (a fence, the ring's read
    // pointer, an interrupt handler's stores: gpu::g_gpuProgress), or 1 ms as
    // the safety net for a store that nothing rings for. It slept 50 us a
    // poll before, ~20000 wake-ups a second for a thread that waits long: CPU
    // power, which a Steam Deck at a low TDP limit takes from its GPU. Linux
    // and Windows (the bell waits on WaitOnAddress there, and sleeps go
    // through compat/win/win_sleep.c's high-resolution timer); not macOS,
    // which coalesces default-QoS timers to ~10 ms (see the command
    // processor's Worker): there each poll sleeps 50 us (see backoff.h, which
    // also covers macOS's stretched timers). NFSMW_GPU_WAIT_BACKOFF=0 spins
    // throughout, =50 sleeps 50 us a poll as before.
    void BackOff()
    {
        backoff::CountGpuWaitPoll();
#if defined(__linux__) || defined(_WIN32)
        static const int mode = [] {
            const char* v = std::getenv("NFSMW_GPU_WAIT_BACKOFF");
            return !v ? 1 : v[0] == '0' ? 0 : std::atoi(v) == 50 ? 2 : 1;
        }();
        if (mode == 0)
#else
        if (!backoff::Enabled(backoff::kGpuWait))
#endif
            return;
        using Clock = std::chrono::steady_clock;
        thread_local Clock::time_point lastPoll{}, waitStart{};
        // The bell's count read at the last poll, before this poll's check.
        thread_local uint32_t seen = 0;
        auto now = Clock::now();
        if (now - lastPoll > std::chrono::microseconds(200))
            waitStart = now;  // a new wait (one wait's polls are at most ~60 us apart)
        lastPoll = now;
        const uint32_t rings = seen;
        seen = gpu::g_gpuProgress.Rings();
        if (now - waitStart < std::chrono::microseconds(20))
            return;
#if defined(__linux__) || defined(_WIN32)
        hostcpu::TightenTimerSlack();
        if (mode == 2)
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        else if (seen == rings)
            gpu::g_gpuProgress.Wait(rings, std::chrono::milliseconds(1));  // (else: rung since: check again at once)
        seen = gpu::g_gpuProgress.Rings();
#else
        backoff::Sleep(backoff::kGpuWait, std::chrono::microseconds(50));
#endif
        lastPoll = Clock::now();
    }
}

SB_HOOK(sub_825A5B20)
{
    // While the game is suspended the GPU work these loops wait for is held
    // back (Metal refuses it), and no guest time passes for the hang check:
    // sleep through it instead of spinning a core.
    guesttime::WaitWhileSuspended();
    if (timeline::g_enabled && !timeline::t_waitTargetMarked)
    {
        timeline::t_waitTargetMarked = true;
        uint32_t device = PPC_LOAD_U32(ctx.r3.u32 + 0);
        timeline::Mark(timeline::GameWaitTarget, PPC_LOAD_U32(device + 10396) - PPC_LOAD_U32(ctx.r3.u32 + 8));
    }
    static const bool log = std::getenv("NFSMW_LOG_D3D_WAIT") != nullptr;
    if (log)
    {
        static uint64_t polls = 0;
        static auto last = std::chrono::steady_clock::now();
        polls++;
        auto now = std::chrono::steady_clock::now();
        if (now - last > std::chrono::seconds(1))
        {
            uint32_t args = ctx.r3.u32;
            uint32_t device = PPC_LOAD_U32(args + 0);
            uint32_t counterPtr = PPC_LOAD_U32(device + 10384);
            fprintf(stderr, "[d3dwait] %llu polls/s | counter at %08X = %08X, issued %08X, target %08X\n",
                (unsigned long long)polls, counterPtr, PPC_LOAD_U32(counterPtr), PPC_LOAD_U32(device + 10396),
                PPC_LOAD_U32(args + 8));
            polls = 0;
            last = now;
        }
    }
    BackOff();
    SB_ORIGINAL(sub_825A5B20)(ctx, base);
}
