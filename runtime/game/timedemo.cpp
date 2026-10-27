// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See timedemo.h.
//
// EA's Joylog, as this default.xex has it. The boot (sub_823AF408) calls its
// init (sub_823B7D90 -> sub_823B7898) once, before it seeds the random
// numbers; the init does nothing unless the u32 at 0x82A2CC78 is set, which
// no code writes (0 in the image). With it set, the game replays when
// ReplayJoylog.jlg exists (0x82A2CC84 = 1, the log object at 0x82A2CC8C) and
// otherwise captures to CaptureJoylog.jlg (0x82A2CC88 = 1, its object at
// 0x82A2CC90), both opened as d:\<name>; a capture whose file can't be
// opened switches itself off without a word (0x823B7A28). From then on the
// game logs (capture) or reads back (replay) everything that isn't the same
// from run to run: the input events (and skips the pad), each frame's time
// quantum (sub_823C6500; a replay ignores the host's frame time), the random
// seed, the file loading status (a replay waits for a load to reach the
// state it had), the pathfinder's time budget. A log object keeps its
// position at +0, how far it has been written to the file at +12, a
// replay's file size at +16, the name at +20 and a 1 MB buffer at +280.
//
// What the runtime adds (the overrides below, kernel/vfs.cpp, frame_time.cpp,
// movie_pacing.cpp, stream_status.cpp, exposure_lock.cpp, cloud_hold.cpp,
// main.cpp):
//   - the flag, before the init runs, and a check that the mode came on
//     (exit 2 otherwise: the game would quietly play, or capture over the
//     log meant for a replay);
//   - the two names on the timedemo's file (vfs.cpp: the disc is read-only);
//   - the capture's tail: the game writes its buffer out when less than 256
//     bytes are left, or in the frame after the u32 at 0x82A2CC7C is set
//     (0x823AFF14, which clears it), which no code sets either. It is set
//     every 600 frames, so a crash loses at most that, and at the quit
//     (FinishCapture), after which the capture is switched off: the file
//     ends at that frame's write and nothing comes after it;
//   - the game's checks: sub_823AF830 compares each frame's checksums (the
//     world objects' positions, the menu, the seed, the loop counter) with
//     the log, and a mismatch, a record of the wrong channel or a log from
//     another build calls DbgBreakPoint (sub_8262CE48), which the runtime's
//     import stub ends the process on (exit 3). Those four callers are
//     reported instead and the game goes on (it stops replaying by itself);
//     every other caller, the game's asserts, still exits as before;
//   - what the log doesn't hold and the game waits on: an audio stream's
//     first data, shown to the main thread a fixed two frames after the
//     stream started (stream_status.cpp; a race's start waits on its intro
//     stream);
//   - in a replay, what a picture took from the timing: the exposure's
//     brightness, read once the GPU has it (exposure_lock.cpp), and the
//     clouds' scroll, held still (cloud_hold.cpp).
//
// A replay's log ends when a read finds no more (sub_823B72D0 clears the
// replay flag and returns 0, and the game plays live from there). The last
// frame in the log is read up to its write-out point, so its checksum reads
// come after the end and compare zeros: that DbgBreakPoint arrives with the
// replay flag already clear and is the log's end, not a divergence. Both a
// capture and its replay see the end at the next frame's start: the capture's
// end_loop is the first RealLoop not wholly in the log, and the replay's
// "log ended at loop L" names the same loop.
#include <stdafx.h>
#include <region/region.h>
#include "timedemo.h"

#include <debug/bench.h>
#include <kernel/function.h>
#include <kernel/vfs.h>
#include <report/report.h>
#include <user/paths.h>
#include <user/settings.h>

#include <SDL3/SDL.h>

#include <cstdarg>
#include <ctime>
#include <fstream>
#include <sstream>

extern char** environ;

extern "C" PPC_FUNC(__imp__sub_823B7D90);  // Joylog init, from the boot (sub_823AF408)
extern "C" PPC_FUNC(__imp__sub_8262CE48);  // `b DbgBreakPoint`: the game's assert break

namespace
{
    using timedemo::Mode;
    namespace detail = timedemo::detail;

    // Guest addresses (the supported default.xex only).
    constexpr uint32_t kJoylogEnable = 0x82A2CC78;  // read once, by the init
    constexpr uint32_t kFlushRequest = 0x82A2CC7C;  // the next frame writes the capture buffer out, and clears it
    constexpr uint32_t kReplaying = 0x82A2CC84;
    constexpr uint32_t kCapturing = 0x82A2CC88;
    constexpr uint32_t kReplayLog = 0x82A2CC8C;
    constexpr uint32_t kCaptureLog = 0x82A2CC90;
    constexpr uint32_t kLogFlushed = 12, kLogFileSize = 16;  // a log object's fields
    constexpr uint32_t kRealLoopCounter = 0x82A2CD20;
    constexpr uint32_t kWorld = 0x82A2CB6C, kWorldLoopCounter = 40;
    constexpr uint32_t kGameFlowState = 0x82A39AD8;
    constexpr uint32_t kRacing = 6;     // GameFlowState once FinishedGameLoading ran
    constexpr uint32_t kFlushEvery = 600;  // frames: a crash loses no more of a capture

    const detail::Config& Config()
    {
        static const detail::Config config = detail::ParseConfig([](const char* name) -> const char* { return std::getenv(name); });
        return config;
    }

    uint32_t Load(uint32_t address)
    {
        return ByteSwap(*static_cast<const uint32_t*>(g_memory.Translate(address)));
    }

    void Store(uint32_t address, uint32_t value)
    {
        *static_cast<uint32_t*>(g_memory.Translate(address)) = ByteSwap(value);
    }

    [[noreturn]] __attribute__((format(printf, 1, 2))) void Fatal(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        fprintf(stderr, "[timedemo] FATAL ");
        vfprintf(stderr, format, args);
        va_end(args);
        fputc('\n', stderr);
        fflush(stderr);
        report::FlushLog();  // the line above, into the session log, before _Exit
        std::_Exit(2);
    }

    std::string s_xexSha256;  // Start (main thread, before the guest runs)
    std::atomic<bool> s_captureOn{ false }, s_replayOn{ false };  // the boot's init came on
    std::atomic<int64_t> s_raceLoop{ -1 };
    std::atomic<bool> s_logEnded{ false }, s_diverged{ false };
    std::atomic<uint32_t> s_divergences{ 0 };
    // Capture: the runtime's menu paused the game (XN_SYS_UI) at this loop
    // after the race loaded, or before (-1: never).
    std::atomic<int64_t> s_systemUiLoop{ -1 }, s_systemUiBeforeRace{ -1 };

    // Capture: FinishCapture asks, the main loop's next frames flush and end
    // the capture. The last write-out the game made: up to `position` in the
    // file, with `loop` the first frame not wholly in it.
    std::atomic<bool> s_finishRequested{ false }, s_finished{ false };
    struct Flushed
    {
        uint32_t position = 0, loop = 0;
        bool any = false;
    };
    std::mutex s_flushedMutex;
    Flushed s_flushed;

    // The guest main thread's own (OnMainFrame).
    struct
    {
        uint32_t sinceFlush = 0;
        bool flushPending = false, finishing = false, quitSent = false;
    } s_main;
    thread_local bool t_mainThread = false;

    std::filesystem::path Sibling(const char* suffix)
    {
        return timedemo::File().string() + suffix;
    }

    std::string UtcNow()
    {
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        gmtime_r(&now, &tm);
        char text[32];
        std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return text;
    }

    std::vector<std::pair<std::string, std::string>> NfsmwEnvironment()
    {
        std::vector<std::pair<std::string, std::string>> vars;
        for (char** e = environ; e && *e; e++)
            if (strncmp(*e, "NFSMW_", 6) == 0)
                if (const char* eq = strchr(*e, '='))
                    vars.emplace_back(std::string(*e, size_t(eq - *e)), std::string(eq + 1));
        std::sort(vars.begin(), vars.end());
        return vars;
    }

    // The settings a replay must share with its capture to stay in step (the
    // guest sees them) or to be compared at all.
    std::vector<std::pair<std::string, std::string>> GameSettings()
    {
        using settings::Id;
        auto value = [](Id id) { return settings::FormatValue(id, settings::GetValue(id)); };
        return {
            { "frame_rate", value(Id::FrameRate) },
            { "aspect", value(Id::AspectRatio) },
            { "resolution", value(Id::PresentScale) },
            { "internal_resolution", value(Id::InternalResolution) },
            { "window", std::to_string(settings::GetInt(Id::WindowWidth)) + "x" + std::to_string(settings::GetInt(Id::WindowHeight)) },
            // XGetLanguage and XCONFIG_USER_LANGUAGE are fixed at 1.
            { "language", "English" },
        };
    }

    // Start, capture: the save the game is about to read, as a replay must
    // find it: <file>.save/save/ and <file>.save/profile_settings.bin.
    // Taken before the guest runs, so before any read or autosave; built
    // beside the old snapshot and swapped in whole.
    void SnapshotSave()
    {
        namespace fs = std::filesystem;
        fs::path root = GetSaveRootPath(), saves = GetSavePath(true);
        fs::path snapshot = Sibling(".save"), building = Sibling(".save.new");
        std::error_code ec, probe;
        fs::remove_all(building, ec);
        fs::create_directories(building / "save", ec);
        if (!ec && fs::is_directory(saves, probe))
            fs::copy(saves, building / "save", fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (!ec && fs::exists(root / "profile_settings.bin", probe))
            fs::copy_file(root / "profile_settings.bin", building / "profile_settings.bin", fs::copy_options::overwrite_existing, ec);
        if (!ec)
        {
            fs::remove_all(snapshot, ec);
            if (!ec)
                fs::rename(building, snapshot, ec);
        }
        if (ec)
        {
            fprintf(stderr, "[timedemo] capture: can't snapshot the save from %s into %s: %s (copy save/ and "
                "profile_settings.bin there yourself before replaying)\n", root.string().c_str(), snapshot.string().c_str(), ec.message().c_str());
            fs::remove_all(building, ec);
            return;
        }
        fprintf(stderr, "[timedemo] capture: the save (%s) snapshot in %s\n", root.string().c_str(), snapshot.string().c_str());
    }

    void CaptureFrame(uint32_t loop)
    {
        auto& m = s_main;
        if (!s_captureOn.load(std::memory_order_relaxed) || s_finished.load(std::memory_order_relaxed))
            return;
        uint32_t log = Load(kCaptureLog);
        if (!log || !Load(kCapturing))
            return;

        // A write-out asked for before the frame that just ran: it ran in
        // that frame, which cleared the request.
        if (m.flushPending && Load(kFlushRequest) == 0)
        {
            m.flushPending = false;
            uint32_t position = Load(log + kLogFlushed);
            {
                std::lock_guard lock(s_flushedMutex);
                s_flushed = { position, loop, true };
            }
            if (m.finishing)
            {
                // Nothing more goes in the log, so it ends exactly here: no
                // later frame's records, and no write-out of its own when
                // the buffer fills before the process ends.
                Store(kCapturing, 0);
                s_finished.store(true, std::memory_order_release);
                fprintf(stderr, "[timedemo] capture ended at loop %u: %u bytes\n", loop, position);
                return;
            }
        }
        if (!m.flushPending)
        {
            bool finish = s_finishRequested.load(std::memory_order_acquire);
            if (finish || ++m.sinceFlush >= kFlushEvery)
            {
                m.sinceFlush = 0;
                m.flushPending = true;
                m.finishing = finish;
                Store(kFlushRequest, 1);
            }
        }

        // NFSMW_TIMEDEMO_CAPTURE_SECONDS: the quit a window close makes
        // (main.cpp then finishes the capture while the game still runs).
        int64_t steps = detail::CaptureSteps(Config());
        if (steps > 0 && !m.quitSent && s_raceLoop.load(std::memory_order_relaxed) >= 0 && timedemo::FlowState() == kRacing &&
            timedemo::WorldStep() >= steps)
        {
            m.quitSent = true;
            fprintf(stderr, "[timedemo] capture: %lld world steps of the race recorded (NFSMW_TIMEDEMO_CAPTURE_SECONDS=%g): quitting\n",
                (long long)steps, Config().captureSeconds);
            SDL_Event e{};
            e.type = SDL_EVENT_QUIT;
            e.quit.timestamp = SDL_GetTicksNS();
            if (!SDL_PushEvent(&e))
                fprintf(stderr, "[timedemo] capture: couldn't queue the quit: %s (close the game to end the capture)\n", SDL_GetError());
        }
    }

    void ReplayFrame(uint32_t loop)
    {
        if (!s_replayOn.load(std::memory_order_relaxed) || Load(kReplaying) != 0)
            return;
        // The game stopped replaying in the frame that just ran.
        s_replayOn.store(false, std::memory_order_relaxed);
        if (s_diverged.load(std::memory_order_acquire))
        {
            fprintf(stderr, "[timedemo] replay stopped at loop %u, after the divergence: live play from here\n", loop);
            return;
        }
        s_logEnded.store(true, std::memory_order_release);
        const auto& captured = timedemo::Captured();
        if (captured.end && *captured.end != loop)
            fprintf(stderr, "[timedemo] log ended at loop %u (the capture's end_loop is %u): live play from here\n", loop, *captured.end);
        else
            fprintf(stderr, "[timedemo] log ended at loop %u: live play from here\n", loop);
    }

    void JoylogBreak(const char* kind)
    {
        uint32_t loop = timedemo::RealLoop(), flow = timedemo::FlowState();
        int64_t step = timedemo::WorldStep();
        if (strcmp(kind, "exe") == 0)
            Fatal("%s was captured with another build of the game (\"Joylog captured with %%s, you're running %%s\": "
                "DbgBreakPoint from 823B7D48): capture it again", timedemo::File().string().c_str());
        // The log ran out inside the frame's checksum reads, which then
        // compared zeros: its end (ReplayFrame says so), not a divergence.
        if (strcmp(kind, "checksum") == 0 && Load(kReplaying) == 0)
            return;
        s_diverged.store(true, std::memory_order_release);
        // A "verify" mismatch leaves the replay on, so it can repeat; the
        // first few are enough to tell where.
        uint32_t n = s_divergences.fetch_add(1, std::memory_order_relaxed);
        if (n < 8)
            fprintf(stderr, "[timedemo] DIVERGENCE kind=%s loop=%u step=%lld flow=%u\n", kind, loop, (long long)step, flow);
        else if (n == 8)
            fprintf(stderr, "[timedemo] DIVERGENCE: more of them, not logged\n");
        bench::Diverged(kind, loop, step);
    }
}

namespace timedemo
{
    Mode GetMode()
    {
        return Config().mode;
    }

    bool FixedStep()
    {
        return detail::FixedStep(Config());
    }

    bool Uncapped()
    {
        return detail::Uncapped(Config());
    }

    const std::filesystem::path& File()
    {
        // Relative to the folder the game was started from, made absolute
        // once: every later use names the same file.
        static const std::filesystem::path file = [] {
            const std::filesystem::path& given = Config().file;
            if (given.empty())
                return GetUserPath() / "timedemo" / "race.jlg";
            std::error_code ec;
            std::filesystem::path absolute = std::filesystem::absolute(given, ec);
            return ec ? given : absolute;
        }();
        return file;
    }

    uint32_t RealLoop()
    {
        return Load(kRealLoopCounter);
    }

    int64_t WorldStep()
    {
        uint32_t world = Load(kWorld);
        return world ? int64_t(Load(world + kWorldLoopCounter)) : -1;
    }

    uint32_t FlowState()
    {
        return Load(kGameFlowState);
    }

    std::optional<uint32_t> RaceLoop()
    {
        int64_t loop = s_raceLoop.load(std::memory_order_acquire);
        return loop >= 0 ? std::optional<uint32_t>(uint32_t(loop)) : std::nullopt;
    }

    bool LogEnded()
    {
        return s_logEnded.load(std::memory_order_acquire);
    }

    bool Diverged()
    {
        return s_diverged.load(std::memory_order_acquire);
    }

    const CapturedLoops& Captured()
    {
        static const CapturedLoops loops = [] {
            CapturedLoops l;
            std::ifstream f(Sibling(".json"));
            if (!f)
                return l;
            std::stringstream text;
            text << f.rdbuf();
            std::string json = text.str();
            auto get = [&](const char* key) -> std::optional<uint32_t> {
                std::optional<int64_t> v = detail::JsonInteger(json, key);
                if (v && *v >= 0 && *v <= int64_t(UINT32_MAX))
                    return uint32_t(*v);
                return std::nullopt;
            };
            l.race = get("race_loop");
            l.end = get("end_loop");
            l.validUntil = get("valid_until_loop");
            return l;
        }();
        return loops;
    }

    std::optional<std::filesystem::path> RedirectJoylog(std::string_view guestPath, bool& readOnly)
    {
        Mode mode = GetMode();
        if (mode == Mode::Off)
            return std::nullopt;
        detail::JoylogName name = detail::MatchJoylog(guestPath);
        detail::JoylogTarget target = detail::RouteJoylog(mode, name);
        if (target.route == detail::JoylogRoute::Unchanged)
            return std::nullopt;
        readOnly = target.readOnly;
        // Absent: beside the log, never created (read-only), so the game
        // finds no file there.
        std::filesystem::path host = target.route == detail::JoylogRoute::LogFile ? File() : Sibling(".absent");
        static std::atomic<bool> s_said[3];
        if (!s_said[size_t(name)].exchange(true, std::memory_order_relaxed))
            fprintf(stderr, "[timedemo] \"%.*s\" is %s%s\n", int(guestPath.size()), guestPath.data(), host.string().c_str(),
                target.route == detail::JoylogRoute::Absent ? " (hidden in this mode)" : target.readOnly ? " (read-only)" : "");
        return host;
    }

    void Start(std::string_view xexSha256)
    {
        const detail::Config& c = Config();
        if (!c.invalid.empty())
            Fatal("NFSMW_TIMEDEMO=%s: neither capture nor replay", c.invalid.c_str());
        if (c.mode == Mode::Off)
            return;
        s_xexSha256 = xexSha256;
        const std::filesystem::path& file = File();
        std::error_code ec;
        if (const char* saveDir = std::getenv("NFSMW_SAVE_DIR"); saveDir && !SaveDirOverride())
            fprintf(stderr, "[timedemo] NFSMW_SAVE_DIR=%s isn't an absolute path: ignored\n", saveDir);
        if (c.mode == Mode::Capture)
        {
            if (c.uncapped)
                fprintf(stderr, "[timedemo] NFSMW_TIMEDEMO_UNCAPPED is for a replay: ignored\n");
            // A fixed step is 1/60 s a frame however often frames come: at 30
            // (or Auto's 30 when the device is hot) the race runs at half speed.
            if (c.fixed && settings::GetInt(settings::Id::FrameRate) != 60)
                fprintf(stderr, "[timedemo] capture: Frame Rate is %s, not 60: with a fixed step the game runs slow while "
                    "frames come less often than 60 a second (NFSMW_FRAME_RATE=60)\n",
                    settings::FormatValue(settings::Id::FrameRate, settings::GetValue(settings::Id::FrameRate)).c_str());
            int64_t steps = detail::CaptureSteps(c);
            fprintf(stderr, "[timedemo] capture to %s: %s, %s\n", file.string().c_str(),
                c.fixed ? "1/60 s a frame (NFSMW_TIMEDEMO_FIXED)" : "frames as timed (NFSMW_TIMEDEMO_FIXED=0)",
                steps ? std::format("{} world steps after the race loads", steps).c_str() : "until the quit");
            std::filesystem::create_directories(file.parent_path(), ec);
            // The game starts the log over; an older capture's .json would
            // describe another log until FinishCapture writes this one's.
            std::filesystem::remove(Sibling(".json"), ec);
            SnapshotSave();
        }
        else
        {
            if (!std::filesystem::is_regular_file(file, ec))
                Fatal("replay: %s not found (NFSMW_TIMEDEMO_FILE)", file.string().c_str());
            fprintf(stderr, "[timedemo] replay of %s%s\n", file.string().c_str(),
                c.uncapped ? ": uncapped, swaps released without a vblank" : "");
            if (!SaveDirOverride())
                fprintf(stderr, "[timedemo] replay without NFSMW_SAVE_DIR: the game reads, and may autosave over, the "
                    "player's own save, and a save other than the capture's (%s) puts the replay out of step\n",
                    Sibling(".save").string().c_str());
            const CapturedLoops& captured = Captured();
            if (captured.end)
                fprintf(stderr, "[timedemo] the capture: race at loop %s, log to loop %u, valid until loop %s\n",
                    captured.race ? std::to_string(*captured.race).c_str() : "(none)", *captured.end,
                    captured.validUntil ? std::to_string(*captured.validUntil).c_str() : "(none)");
            else
                fprintf(stderr, "[timedemo] no %s: the capture's loops aren't known\n", Sibling(".json").string().c_str());
        }
        if (const char* saveDir = SaveDirOverride())
            fprintf(stderr, "[timedemo] saves in %s (NFSMW_SAVE_DIR)\n", saveDir);
    }

    bool OnMainThread()
    {
        return t_mainThread;
    }

    void OnMainFrame()
    {
        t_mainThread = true;
        uint32_t loop = RealLoop();
        if (s_raceLoop.load(std::memory_order_relaxed) < 0 && FlowState() == kRacing)
        {
            s_raceLoop.store(loop, std::memory_order_release);
            std::optional<uint32_t> captured = Replaying() ? Captured().race : std::nullopt;
            if (captured && *captured != loop)
                fprintf(stderr, "[timedemo] race loaded at loop %u, the capture's at %u: the replay is out of step\n", loop, *captured);
            else
                fprintf(stderr, "[timedemo] race loaded at loop %u\n", loop);
        }
        if (Capturing())
            CaptureFrame(loop);
        else if (Replaying())
            ReplayFrame(loop);
    }

    void FinishCapture()
    {
        if (!Capturing())
            return;
        // Both exit paths can get here (the launcher gone while the window
        // closes): the second waits for the first, then has nothing to do.
        static std::mutex mutex;
        std::lock_guard lock(mutex);
        static bool done = false;
        if (std::exchange(done, true))
            return;
        if (!s_captureOn.load(std::memory_order_acquire))
        {
            fprintf(stderr, "[timedemo] capture: the game never started capturing: no log\n");
            return;
        }

        // The guest main loop writes the buffer out and ends the capture in
        // its next frames (OnMainFrame): about two frames, 3 s at most (a
        // loading screen runs no such frames).
        using Clock = std::chrono::steady_clock;
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(3);
        s_finishRequested.store(true, std::memory_order_release);
        while (!s_finished.load(std::memory_order_acquire) && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const bool finished = s_finished.load(std::memory_order_acquire);
        Flushed flushed;
        {
            std::lock_guard flushedLock(s_flushedMutex);
            flushed = s_flushed;
        }
        // The game's file writes go through its own I/O thread: wait, in the
        // same 3 s, for the file to hold what the log object says it wrote.
        const std::filesystem::path& file = File();
        auto onDisk = [&] {
            std::error_code ec;
            uintmax_t size = std::filesystem::file_size(file, ec);
            return ec ? uintmax_t(0) : size;
        };
        while (flushed.any && onDisk() < flushed.position && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const uintmax_t size = onDisk();

        if (!finished)
            fprintf(stderr, "[timedemo] capture: the game didn't write its log out within 3 s (no game frames ran): "
                "the log ends at its last write-out%s\n", flushed.any ? std::format(", loop {}", flushed.loop).c_str() : " (none)");
        if (flushed.any && size != flushed.position)
            fprintf(stderr, "[timedemo] capture: %s is %llu bytes, the game wrote %u\n", file.string().c_str(), (unsigned long long)size,
                flushed.position);

        detail::Meta meta;
        meta.created = UtcNow();
        meta.runtime = report::VersionString();
        meta.buildId = report::BuildId();
        meta.exeCheck = s_xexSha256;
        meta.file = file.filename().string();
        meta.size = flushed.any ? flushed.position : size;
        meta.complete = finished && size == flushed.position;
        meta.fixedStep = FixedStep();
        meta.captureSeconds = Config().captureSeconds;
        meta.raceLoop = RaceLoop();
        if (flushed.any)
            meta.endLoop = flushed.loop;
        // The runtime's menu paused the game where the log doesn't say so:
        // a replay is out of step from there.
        int64_t systemUi = s_systemUiLoop.load(std::memory_order_acquire);
        meta.validUntilLoop = meta.endLoop;
        if (systemUi >= 0 && (!meta.endLoop || uint32_t(systemUi) < *meta.endLoop))
            meta.validUntilLoop = uint32_t(systemUi);
        if (int64_t before = s_systemUiBeforeRace.load(std::memory_order_acquire); before >= 0)
            meta.systemUiBeforeRace = uint32_t(before);
        meta.settings = GameSettings();
        meta.env = NfsmwEnvironment();
        std::string json = detail::MetaJson(meta);
        std::filesystem::path metaPath = Sibling(".json");
        if (std::string error = vfs::WriteAtomically(metaPath, json.data(), json.size()); !error.empty())
        {
            fprintf(stderr, "[timedemo] capture: can't write %s: %s\n", metaPath.string().c_str(), error.c_str());
            return;
        }
        auto loopText = [](const std::optional<uint32_t>& v) { return v ? std::to_string(*v) : std::string("none"); };
        fprintf(stderr, "[timedemo] capture saved: %s (%llu bytes; race at loop %s, log to loop %s, valid until loop %s) and %s\n",
            file.string().c_str(), (unsigned long long)meta.size, loopText(meta.raceLoop).c_str(), loopText(meta.endLoop).c_str(),
            loopText(meta.validUntilLoop).c_str(), metaPath.filename().string().c_str());
    }

    bool OnSystemUi(bool shown)
    {
        switch (GetMode())
        {
        case Mode::Off:
            return false;
        case Mode::Capture:
            // The pause goes to the game as usual (the player is driving), but
            // the log doesn't hold it: note where the capture stops being
            // replayable. Before the race only a note: the checksum will tell
            // whether the front end minded.
            if (shown && s_captureOn.load(std::memory_order_relaxed))
            {
                uint32_t loop = RealLoop();
                int64_t none = -1;
                if (s_raceLoop.load(std::memory_order_acquire) < 0)
                {
                    if (s_systemUiBeforeRace.compare_exchange_strong(none, loop))
                        fprintf(stderr, "[timedemo] capture: the runtime's menu opened at loop %u, before the race: a replay "
                            "may go out of step there\n", loop);
                }
                else if (s_systemUiLoop.compare_exchange_strong(none, loop))
                    fprintf(stderr, "[timedemo] capture: the runtime's menu paused the game at loop %u: a replay is valid up "
                        "to there\n", loop);
            }
            return false;
        case Mode::Replay:
            // The capture had no pause here: telling the game would put the
            // replay out of step. The run doesn't measure what it would have.
            if (shown)
            {
                fprintf(stderr, "[timedemo] replay: the runtime's menu opened at loop %u (the game isn't paused): the run is "
                    "interrupted\n", RealLoop());
                bench::Interrupted();
            }
            return true;
        }
        return false;
    }

    bool HideGuestSystemUi()
    {
        if (!Active())
            return false;
        static std::atomic<bool> s_said{ false };
        if (!s_said.exchange(true, std::memory_order_relaxed))
            fprintf(stderr, "[timedemo] the device selector's XN_SYS_UI isn't sent (as on a 360 with one storage device)\n");
        return true;
    }
}

// Joylog's init, once, from the boot. With a mode on: the flag the init
// checks, then whether the mode came on.
SB_HOOK(sub_823B7D90)
{
    const detail::Config& c = Config();
    if (c.mode == Mode::Off)
    {
        SB_ORIGINAL(sub_823B7D90)(ctx, base);
        return;
    }
    Store(kJoylogEnable, 1);
    SB_ORIGINAL(sub_823B7D90)(ctx, base);
    const std::filesystem::path& file = timedemo::File();
    if (c.mode == Mode::Capture)
    {
        uint32_t on = Load(kCapturing), log = Load(kCaptureLog);
        if (on != 1 || !log)
            Fatal("capture didn't start: [0x82A2CC88]=%u [0x82A2CC90]=%08X (the game couldn't open d:\\CaptureJoylog.jlg, "
                "%s: see its [file] line)", on, log, file.string().c_str());
        s_captureOn.store(true, std::memory_order_release);
        fprintf(stderr, "[timedemo] capture on: %s\n", file.string().c_str());
    }
    else
    {
        uint32_t on = Load(kReplaying), log = Load(kReplayLog);
        if (on != 1 || !log)
            Fatal("replay didn't start: [0x82A2CC84]=%u [0x82A2CC8C]=%08X (the game didn't find d:\\ReplayJoylog.jlg, %s)", on,
                log, file.string().c_str());
        s_replayOn.store(true, std::memory_order_release);
        fprintf(stderr, "[timedemo] replay on: %s (%u bytes)\n", file.string().c_str(), Load(log + kLogFileSize));
    }
}

// DbgBreakPoint (a `b` to the import): the Joylog checks' four callers are
// reported while a mode is on; every other caller, and any caller without a
// mode, goes to the import as before (kernel/unimplemented.cpp: exit 3, or
// on with NFSMW_STUB_CONTINUE=1).
SB_HOOK(sub_8262CE48)
{
    if (timedemo::Active())
    {
        if (const char* kind = detail::BreakKind(uint32_t(ctx.lr)))
        {
            JoylogBreak(kind);
            return;
        }
    }
    SB_ORIGINAL(sub_8262CE48)(ctx, base);
}
