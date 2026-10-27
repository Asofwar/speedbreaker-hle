// Tests for gpu/cp_doorbell.h: how the command processor sleeps while it has
// nothing to do, and that whoever ends its waits wakes it. A waiter that
// follows the protocol (Prepare, look again, Wait) must never miss a ring:
// a ring before Prepare is seen by the look, one after it ends the Wait.
// A stress run hands a producer's work over to a consumer thousands of times
// with a long timeout, which a single lost wake-up would run into. With the
// sources (ctest passes them), they are checked to ring the doorbell where
// the command processor's waits end: after the game's vblank handler and its
// ring kicks (command_processor.cpp), after a completion (renderer.cpp's
// completion thread) and when a guest thread asks for a submit
// (kernel/write_watch.cpp). From the repo root:
//   clang++ -std=c++20 -O2 -UNDEBUG -Iruntime tests/gpu_cp_doorbell_test.cpp -o build/gpu_cp_doorbell_test -lpthread
//   build/gpu_cp_doorbell_test runtime/gpu/command_processor.cpp runtime/gpu/renderer.cpp runtime/kernel/write_watch.cpp
#include <gpu/cp_doorbell.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using Clock = std::chrono::steady_clock;

static double MsSince(Clock::time_point t)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

static void TestRungBeforeWait()
{
    gpu::Doorbell bell;
    uint32_t rings = bell.Prepare();
    bell.Ring();  // between Prepare and Wait: the Wait must not sleep
    auto start = Clock::now();
    bool rung = bell.Wait(rings, std::chrono::seconds(2));
    CHECK(rung, "a ring after Prepare reads as rung");
    CHECK(MsSince(start) < 500, "and the Wait returns at once, not after %.0f ms", MsSince(start));

    // A ring before Prepare is the look's business, not the Wait's: the
    // count moved on before the waiter took it, so this Wait sleeps it out.
    bell.Ring();
    rings = bell.Prepare();
    start = Clock::now();
    rung = bell.Wait(rings, std::chrono::milliseconds(20));
    CHECK(!rung, "a ring before Prepare doesn't end a later Wait");
    CHECK(MsSince(start) >= 15, "which sleeps its timeout (%.1f ms)", MsSince(start));
}

static void TestTimeout()
{
    gpu::Doorbell bell;
    uint32_t rings = bell.Prepare();
    auto start = Clock::now();
    bool rung = bell.Wait(rings, std::chrono::milliseconds(30));
    double ms = MsSince(start);
    CHECK(!rung, "an unrung Wait times out");
    CHECK(ms >= 25 && ms < 5000, "after its timeout (%.1f ms for 30)", ms);

    // A zero timeout (the idle loop's recording already due) never sleeps.
    rings = bell.Prepare();
    start = Clock::now();
    bell.Wait(rings, std::chrono::nanoseconds(0));
    CHECK(MsSince(start) < 5, "a zero timeout returns at once (%.2f ms)", MsSince(start));

    // Cancel ends a Prepare without sleeping; a later Wait still works.
    rings = bell.Prepare();
    bell.Cancel();
    CHECK(bell.Rings() == rings, "Cancel rings nothing");
}

static void TestWakeFromAnotherThread()
{
    if (!gpu::Doorbell::kBlocking)
    {
        printf("  (not Linux: Wait sleeps out its timeout, the command processor polls there)\n");
        return;
    }
    gpu::Doorbell bell;
    std::atomic<bool> sleeping{ false };
    double wokeAfterMs = -1;
    bool rung = false;
    std::thread waiter([&] {
        uint32_t rings = bell.Prepare();
        sleeping = true;
        auto start = Clock::now();
        rung = bell.Wait(rings, std::chrono::seconds(10));
        wokeAfterMs = MsSince(start);
    });
    while (!sleeping)
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // well inside the futex by now
    auto rang = Clock::now();
    bell.Ring();
    waiter.join();
    CHECK(rung, "a ring wakes the sleeper");
    CHECK(wokeAfterMs < 5000 && MsSince(rang) < 5000, "at the ring, not at its 10 s timeout (slept %.1f ms)", wokeAfterMs);
}

// The command processor's use: the producer (the game kicking the ring)
// publishes work, then rings; the consumer looks for work, and with none
// Prepares, looks again and Waits. Every hand-over must come from a ring or
// the look: a Wait that runs into its (long) timeout with work waiting is a
// lost wake-up.
static void TestNoLostWakeups()
{
    if (!gpu::Doorbell::kBlocking)
        return;
    constexpr uint32_t kRounds = 20000;
    constexpr auto kTimeout = std::chrono::seconds(3);
    gpu::Doorbell bell;
    std::atomic<uint32_t> published{ 0 };
    std::atomic<uint32_t> consumed{ 0 };
    uint32_t sleeps = 0, rungs = 0, lost = 0;
    auto start = Clock::now();
    std::thread consumer([&] {
        uint32_t seen = 0;
        while (seen < kRounds)
        {
            uint32_t now = published.load(std::memory_order_acquire);
            if (now != seen)
            {
                seen = now;
                consumed.store(seen, std::memory_order_release);
                continue;
            }
            uint32_t rings = bell.Prepare();
            if (published.load(std::memory_order_acquire) != seen)
            {
                bell.Cancel();
                continue;
            }
            sleeps++;
            auto waitStart = Clock::now();
            if (bell.Wait(rings, kTimeout))
                rungs++;
            else if (Clock::now() - waitStart >= kTimeout - std::chrono::milliseconds(100) &&
                published.load(std::memory_order_acquire) != seen)
                lost++;
        }
    });
    std::mt19937 rng(12345);
    for (uint32_t i = 1; i <= kRounds; i++)
    {
        published.store(i, std::memory_order_release);
        bell.Ring();
        // Sometimes wait for the hand-over (the consumer goes back to sleep
        // between kicks), sometimes kick again at once (a burst).
        if (rng() % 4 == 0)
            while (consumed.load(std::memory_order_acquire) < i && MsSince(start) < 60000)
                std::this_thread::yield();
        else if (rng() % 4 == 0)
            std::this_thread::sleep_for(std::chrono::microseconds(rng() % 50));
        if (MsSince(start) > 60000)
            break;
    }
    consumer.join();
    printf("  %u hand-overs: %u sleeps, %u rung, %.0f ms\n", kRounds, sleeps, rungs, MsSince(start));
    CHECK(lost == 0, "no Wait ran into its timeout with work waiting (%u did)", lost);
    CHECK(sleeps > 100, "the consumer did sleep (%u times): the test exercised the futex", sleeps);
}

static std::string ReadFile(const char* path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// `then` within `window` bytes after `first` (each found once at least).
static bool Follows(const std::string& text, const std::string& first, const std::string& then, size_t window)
{
    for (size_t at = text.find(first); at != std::string::npos; at = text.find(first, at + 1))
    {
        size_t next = text.find(then, at + first.size());
        if (next != std::string::npos && next - at <= window)
            return true;
    }
    return false;
}

static void TestSources(const char* commandProcessor, const char* renderer, const char* writeWatch)
{
    std::string cp = ReadFile(commandProcessor), r = ReadFile(renderer), ww = ReadFile(writeWatch);
    CHECK(!cp.empty() && !r.empty() && !ww.empty(), "the sources read");
    // The vblank: the game's handler ends the wait for it, then the ring.
    CHECK(Follows(cp, "DispatchInterrupt(thread.ppcContext, 0, 2);", "g_cpDoorbell.Ring();", 400),
        "the vsync thread rings after the game's vblank handler");
    // The GPU interrupt, when the completion thread runs it.
    CHECK(Follows(cp, "DispatchInterrupt(*guest, 1, n);", "g_cpDoorbell.Ring();", 400),
        "a PM4 INTERRUPT rings after the game's handler");
    // The game's two kick functions, the original first.
    CHECK(Follows(cp, "SB_HOOK(sub_82596A00)", "SB_ORIGINAL(sub_82596A00)(ctx, base);", 40) &&
        Follows(cp, "SB_ORIGINAL(sub_82596A00)(ctx, base);", "g_cpDoorbell.Ring();", 80),
        "sub_82596A00 (sets CP_RB_WPTR) rings after it ran");
    CHECK(Follows(cp, "SB_HOOK(sub_82596A20)", "SB_ORIGINAL(sub_82596A20)(ctx, base);", 40) &&
        Follows(cp, "SB_ORIGINAL(sub_82596A20)(ctx, base);", "g_cpDoorbell.Ring();", 80),
        "sub_82596A20 (kicks indirect buffers) rings after it ran");
    // Both of the command processor's sleeps follow the protocol: Prepare,
    // look again, then Wait.
    CHECK(Follows(cp, "g_cpDoorbell.Prepare();", "RingHasWork(", 200), "the idle sleep looks for work after Prepare");
    CHECK(Follows(cp, "g_cpDoorbell.Prepare();", "passes(current())", 200), "a WAIT_REG_MEM looks again after Prepare");
    // The completion thread, after retiring a submission.
    CHECK(Follows(r, "writewatch::SetCompleted(sub.number);", "g_cpDoorbell.Ring();", 900),
        "the completion thread rings after a submission retired");
    // A guest thread that waits on recorded work.
    CHECK(Follows(ww, "g_submitRequested.store(true);", "g_cpDoorbell.Ring();", 120),
        "the write watch rings when it asks for a submit");
}

int main(int argc, char** argv)
{
    printf("rung before Wait\n");
    TestRungBeforeWait();
    printf("timeout\n");
    TestTimeout();
    printf("wake from another thread\n");
    TestWakeFromAnotherThread();
    printf("no lost wake-ups\n");
    TestNoLostWakeups();
    if (argc >= 4)
    {
        printf("sources\n");
        TestSources(argv[1], argv[2], argv[3]);
    }
    else
        printf("(sources not checked: give command_processor.cpp, renderer.cpp and write_watch.cpp)\n");
    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
