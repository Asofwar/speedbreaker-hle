// Tests for the timedemo's parts that need neither the game nor the host
// (runtime/game/timedemo.h, header only): its switches, which Joylog path
// goes where, which DbgBreakPoint callers are the log's checks, the fixed
// step and the uncapped swap, and the capture's .json. From the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/timedemo_logic_test.cpp -o build/timedemo_logic_test
//   build/timedemo_logic_test runtime
// With the runtime folder it also checks how the runtime is wired to it:
// vfs::Resolve asks it first, GameFrameTime runs it (and the benchmark) and
// fixes the step before the whole-vblank logic, WorldStepTime rounds in a
// timedemo whatever the display, the swap callback releases at once in an
// uncapped replay, main() finishes a capture on both ways out before the
// renderer stops, and the runtime's menu asks it before pausing the game.
#include <game/timedemo.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

namespace
{
    using timedemo::Mode;
    namespace detail = timedemo::detail;

    detail::Config Parse(const std::map<std::string, std::string>& env)
    {
        return detail::ParseConfig([&](const char* name) -> const char* {
            auto it = env.find(name);
            return it == env.end() ? nullptr : it->second.c_str();
        });
    }

    void Switches()
    {
        // Nothing set: off, and every other switch at its default.
        detail::Config c = Parse({});
        CHECK(c.mode == Mode::Off && c.invalid.empty() && c.file.empty() && c.fixed && !c.uncapped && c.captureSeconds == 0,
            "unset: off, fixed by default, capped, until the quit");
        for (const char* off : { "", "0", "off", "OFF" })
        {
            c = Parse({ { "NFSMW_TIMEDEMO", off } });
            CHECK(c.mode == Mode::Off && c.invalid.empty(), "NFSMW_TIMEDEMO='%s': off, not a mistake", off);
        }
        for (const char* capture : { "capture", "Capture", "CAPTURE" })
            CHECK(Parse({ { "NFSMW_TIMEDEMO", capture } }).mode == Mode::Capture, "'%s' is capture", capture);
        for (const char* replay : { "replay", "Replay", "REPLAY" })
            CHECK(Parse({ { "NFSMW_TIMEDEMO", replay } }).mode == Mode::Replay, "'%s' is replay", replay);
        // A typo stops the run (Start) instead of playing the game.
        for (const char* typo : { "captur", "replay ", "1", "record", "on" })
        {
            c = Parse({ { "NFSMW_TIMEDEMO", typo } });
            CHECK(c.mode == Mode::Off && c.invalid == typo, "'%s': no mode, kept as invalid ('%s')", typo, c.invalid.c_str());
        }

        c = Parse({ { "NFSMW_TIMEDEMO", "replay" }, { "NFSMW_TIMEDEMO_FILE", "/home/deck/sb-bench/race.jlg" } });
        CHECK(c.file == "/home/deck/sb-bench/race.jlg", "the file as given: %s", c.file.c_str());
        CHECK(Parse({ { "NFSMW_TIMEDEMO_FILE", "" } }).file.empty(), "an empty file: the default");

        // 0/1 switches: the usual spellings; anything else keeps the default.
        for (auto [value, fixed] : std::vector<std::pair<const char*, bool>>{ { "0", false }, { "1", true }, { "false", false },
                 { "no", false }, { "off", false }, { "true", true }, { "yes", true }, { "on", true }, { "", true }, { "2", true }, { "maybe", true } })
            CHECK(Parse({ { "NFSMW_TIMEDEMO_FIXED", value } }).fixed == fixed, "NFSMW_TIMEDEMO_FIXED='%s': %d", value, fixed);
        for (auto [value, uncapped] : std::vector<std::pair<const char*, bool>>{ { "1", true }, { "0", false }, { "", false }, { "x", false } })
            CHECK(Parse({ { "NFSMW_TIMEDEMO_UNCAPPED", value } }).uncapped == uncapped, "NFSMW_TIMEDEMO_UNCAPPED='%s': %d", value, uncapped);

        for (auto [value, seconds] : std::vector<std::pair<const char*, double>>{ { "150", 150 }, { "2.5", 2.5 }, { "0", 0 },
                 { "-3", 0 }, { "abc", 0 }, { "inf", 0 }, { "nan", 0 }, { "", 0 } })
            CHECK(Parse({ { "NFSMW_TIMEDEMO_CAPTURE_SECONDS", value } }).captureSeconds == seconds,
                "NFSMW_TIMEDEMO_CAPTURE_SECONDS='%s': %g", value, seconds);
    }

    // Each switch only in its own mode.
    void Decisions()
    {
        detail::Config capture = Parse({ { "NFSMW_TIMEDEMO", "capture" }, { "NFSMW_TIMEDEMO_UNCAPPED", "1" },
            { "NFSMW_TIMEDEMO_CAPTURE_SECONDS", "150" } });
        detail::Config replay = Parse({ { "NFSMW_TIMEDEMO", "replay" }, { "NFSMW_TIMEDEMO_UNCAPPED", "1" },
            { "NFSMW_TIMEDEMO_CAPTURE_SECONDS", "150" } });
        detail::Config off = Parse({ { "NFSMW_TIMEDEMO_UNCAPPED", "1" }, { "NFSMW_TIMEDEMO_FIXED", "1" } });
        CHECK(detail::FixedStep(capture) && !detail::FixedStep(replay) && !detail::FixedStep(off), "a fixed step in capture only");
        CHECK(!detail::FixedStep(Parse({ { "NFSMW_TIMEDEMO", "capture" }, { "NFSMW_TIMEDEMO_FIXED", "0" } })),
            "NFSMW_TIMEDEMO_FIXED=0: a capture as timed");
        CHECK(detail::Uncapped(replay) && !detail::Uncapped(capture) && !detail::Uncapped(off), "uncapped in replay only");
        CHECK(!detail::Uncapped(Parse({ { "NFSMW_TIMEDEMO", "replay" } })), "a replay is capped by default");
        CHECK(detail::CaptureSteps(capture) == 9000, "150 s of a capture: 9000 world steps, not %lld", (long long)detail::CaptureSteps(capture));
        CHECK(detail::CaptureSteps(Parse({ { "NFSMW_TIMEDEMO", "capture" }, { "NFSMW_TIMEDEMO_CAPTURE_SECONDS", "2.5" } })) == 150,
            "2.5 s: 150 steps");
        CHECK(detail::CaptureSteps(replay) == 0 && detail::CaptureSteps(Parse({ { "NFSMW_TIMEDEMO", "capture" } })) == 0,
            "no stop in replay, nor in a capture without the switch");

        // The fixed step: 1000/60 ms in 16.16 fixed point, as the main loop
        // hands r3 to the frame, and as close to it as 16.16 goes.
        CHECK(timedemo::kFixedFrame == uint32_t(std::llround(1000.0 / 60.0 * 65536.0)), "kFixedFrame is 1000/60 ms in 16.16");
        CHECK(std::abs(timedemo::kFixedFrame / 65536.0 - 1000.0 / 60.0) < 0.5 / 65536.0, "within half a 16.16 step of 1/60 s");
    }

    // The swap callback (sub_825985A0) as disassembled: the vblanks the
    // swap waits for (0: released at once). r8 = the release vblank plus the
    // interval, less the vblanks so far; above 0 that is the countdown, else
    // the beam position in percent (clipped to 100) against the threshold.
    uint32_t SwapWait(uint32_t arg, uint32_t released, uint32_t vblanks, uint32_t vCounter, uint32_t viewport)
    {
        uint32_t interval = arg >> 16, threshold = arg & 0xFFFF;
        int32_t r8 = int32_t(released + interval - vblanks);
        if (r8 > 0)
            return uint32_t(r8);
        uint32_t beam = std::min<uint32_t>(vCounter * 100 / (viewport & 0xFFF) + 1, 100);
        return beam <= threshold ? 0 : 1;
    }

    void UncappedSwap()
    {
        // The runtime pins D1MODE_V_COUNTER at 720 and the viewport at 720
        // lines (command_processor.cpp's StartOnce).
        constexpr uint32_t kVCounter = 720, kViewport = 0x050002D0;
        constexpr uint32_t kShipped = 0x0001000A;  // interval 1, threshold 10%
        CHECK(timedemo::kUncappedSwap >> 16 == 0 && (timedemo::kUncappedSwap & 0xFFFF) == 100, "interval 0, threshold 100%%");
        for (uint32_t since = 0; since < 4; since++)
        {
            uint32_t released = 1000, vblanks = released + since;
            CHECK(SwapWait(timedemo::kUncappedSwap, released, vblanks, kVCounter, kViewport) == 0,
                "uncapped: released at once, %u vblanks after the last release", since);
            CHECK(SwapWait(kShipped, released, vblanks, kVCounter, kViewport) == 1,
                "the game's own: a vblank to wait, %u after the last release", since);
        }
    }

    void JoylogPaths()
    {
        using detail::JoylogName;
        struct Case
        {
            const char* path;
            JoylogName name;
        };
        for (const Case& c : std::vector<Case>{
                 { "d:\\CaptureJoylog.jlg", JoylogName::Capture },
                 { "D:\\capturejoylog.JLG", JoylogName::Capture },
                 { "d:\\ReplayJoylog.jlg", JoylogName::Replay },
                 { "game:\\ReplayJoylog.jlg", JoylogName::Replay },
                 { "d:/sub/ReplayJoylog.jlg", JoylogName::Replay },
                 { "\\Device\\Cdrom0\\CaptureJoylog.jlg", JoylogName::Capture },
                 { "CaptureJoylog.jlg", JoylogName::Capture },
                 { "d:ReplayJoylog.jlg", JoylogName::Replay },
                 { "d:\\CaptureJoylog.jlg.bak", JoylogName::None },
                 { "d:\\XCaptureJoylog.jlg", JoylogName::None },
                 { "d:\\ReplayJoylog.jlg\\", JoylogName::None },
                 { "d:\\GLOBAL\\GLOBALB.BUN", JoylogName::None },
                 { "", JoylogName::None },
             })
            CHECK(detail::MatchJoylog(c.path) == c.name, "\"%s\"", c.path);

        using detail::JoylogRoute;
        auto route = [](Mode mode, JoylogName name) { return detail::RouteJoylog(mode, name); };
        for (JoylogName name : { JoylogName::None, JoylogName::Capture, JoylogName::Replay })
            CHECK(route(Mode::Off, name).route == JoylogRoute::Unchanged, "no mode: every path as before");
        for (Mode mode : { Mode::Capture, Mode::Replay })
            CHECK(route(mode, JoylogName::None).route == JoylogRoute::Unchanged, "other paths as before");
        // Capture: its own name is the log, written; the replay's name is
        // hidden, so a stray file there can't make the game replay.
        CHECK(route(Mode::Capture, JoylogName::Capture).route == JoylogRoute::LogFile && !route(Mode::Capture, JoylogName::Capture).readOnly,
            "capture: CaptureJoylog.jlg is the log, writable");
        CHECK(route(Mode::Capture, JoylogName::Replay).route == JoylogRoute::Absent && route(Mode::Capture, JoylogName::Replay).readOnly,
            "capture: ReplayJoylog.jlg absent, and can't be created");
        // Replay: its name is the log, read-only; the capture's is hidden,
        // so its open (which truncates) can never reach the log.
        CHECK(route(Mode::Replay, JoylogName::Replay).route == JoylogRoute::LogFile && route(Mode::Replay, JoylogName::Replay).readOnly,
            "replay: ReplayJoylog.jlg is the log, read-only");
        CHECK(route(Mode::Replay, JoylogName::Capture).route == JoylogRoute::Absent && route(Mode::Replay, JoylogName::Capture).readOnly,
            "replay: CaptureJoylog.jlg absent, read-only");
    }

    void Breaks()
    {
        auto is = [](uint32_t lr, const char* kind) {
            const char* k = detail::BreakKind(lr);
            return k && std::string(k) == kind;
        };
        CHECK(is(0x823AF9CC, "checksum"), "sub_823AF830's checksum");
        CHECK(is(0x823B6F4C, "channel"), "a record of another channel");
        CHECK(is(0x823B7884, "verify"), "sub_823B7800's check");
        CHECK(is(0x823B7D48, "exe"), "a log from another build");
        // The game's asserts (the other 259 callers) and near misses (the
        // call's own address, not its return) stay with the import.
        for (uint32_t lr : { 0x823AF9C8u, 0x823AF9D0u, 0x823B6F48u, 0x823B7880u, 0x823B7D44u, 0u, 0x8262CE48u })
            CHECK(detail::BreakKind(lr) == nullptr, "%08X isn't a Joylog check", lr);
    }

    // A minimal JSON checker (RFC 8259's grammar, no numbers beyond what
    // MetaJson writes): the .json must load in the benchmark's scripts.
    struct JsonChecker
    {
        std::string_view s;
        size_t i = 0;
        void Space() { while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) i++; }
        bool Literal(std::string_view word) { if (s.substr(i, word.size()) != word) return false; i += word.size(); return true; }
        bool String()
        {
            if (i >= s.size() || s[i] != '"')
                return false;
            for (i++; i < s.size(); i++)
            {
                unsigned char c = static_cast<unsigned char>(s[i]);
                if (c == '"') { i++; return true; }
                if (c < 0x20) return false;
                if (c == '\\')
                {
                    if (++i >= s.size()) return false;
                    if (s[i] == 'u') { for (int k = 0; k < 4; k++) if (++i >= s.size() || !isxdigit(static_cast<unsigned char>(s[i]))) return false; }
                    else if (std::string_view("\"\\/bfnrt").find(s[i]) == std::string_view::npos) return false;
                }
            }
            return false;
        }
        bool Number()
        {
            size_t start = i;
            if (i < s.size() && s[i] == '-') i++;
            while (i < s.size() && (isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) i++;
            return i > start;
        }
        bool Value()
        {
            Space();
            if (i >= s.size()) return false;
            bool ok;
            if (s[i] == '{')
            {
                i++; Space();
                if (i < s.size() && s[i] == '}') { i++; return true; }
                do { Space(); if (!String()) return false; Space(); if (i >= s.size() || s[i++] != ':') return false; if (!Value()) return false; Space(); }
                while (i < s.size() && s[i] == ',' && ++i);
                ok = i < s.size() && s[i++] == '}';
            }
            else if (s[i] == '"') ok = String();
            else if (s[i] == 't') ok = Literal("true");
            else if (s[i] == 'f') ok = Literal("false");
            else if (s[i] == 'n') ok = Literal("null");
            else ok = Number();
            return ok;
        }
        bool Document() { bool ok = Value(); Space(); return ok && i == s.size(); }
    };

    void Meta()
    {
        detail::Meta m;
        m.created = "2026-10-15T12:34:56Z";
        m.runtime = "v0.1.5 (fae4bd1)";
        m.buildId = "fae4bd1";
        m.exeCheck = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        m.file = "race \"2\".jlg";
        m.size = 4194305;
        m.complete = true;
        m.fixedStep = true;
        m.captureSeconds = 150;
        m.raceLoop = 2412;
        m.endLoop = 11530;
        m.validUntilLoop = 11000;
        m.settings = { { "frame_rate", "60" }, { "aspect", "Auto" }, { "window", "1280x800" } };
        m.env = { { "NFSMW_TIMEDEMO", "capture" }, { "NFSMW_TIMEDEMO_FILE", "C:\\race\\\"q\".jlg\n\t\x01" },
            { "NFSMW_ODD", "\"race_loop\": 7" } };
        std::string json = detail::MetaJson(m);
        CHECK(JsonChecker{ json }.Document(), "valid JSON:\n%s", json.c_str());
        CHECK(detail::JsonInteger(json, "schema") == 1, "schema 1");
        CHECK(detail::JsonInteger(json, "race_loop") == 2412, "race_loop");
        CHECK(detail::JsonInteger(json, "end_loop") == 11530, "end_loop");
        CHECK(detail::JsonInteger(json, "valid_until_loop") == 11000, "valid_until_loop");
        CHECK(detail::JsonInteger(json, "size") == 4194305, "size");
        CHECK(!detail::JsonInteger(json, "system_ui_before_race_loop"), "null: none");
        CHECK(detail::JsonInteger(json, "capture_seconds") == 150, "capture_seconds");
        CHECK(!detail::JsonInteger(json, "created") && !detail::JsonInteger(json, "missing"), "a string, or no such key: none");
        CHECK(json.find("\"C:\\\\race\\\\\\\"q\\\".jlg\\n\\t\\u0001\"") != std::string::npos, "escaped:\n%s", json.c_str());
        CHECK(json.find("\"complete\": true") != std::string::npos && json.find("\"fixed_step\": true") != std::string::npos, "booleans");

        // Nothing known: nulls and empty objects, still JSON.
        detail::Meta empty;
        std::string bare = detail::MetaJson(empty);
        CHECK(JsonChecker{ bare }.Document(), "valid JSON:\n%s", bare.c_str());
        CHECK(!detail::JsonInteger(bare, "race_loop") && !detail::JsonInteger(bare, "end_loop"), "nulls");
        CHECK(bare.find("\"settings\": {}") != std::string::npos && bare.find("\"env\": {}") != std::string::npos, "empty objects");

        // The integer reader's edges.
        CHECK(detail::JsonInteger("{\"a\":-5}", "a") == -5, "negative");
        CHECK(detail::JsonInteger("{\"a\" :\n 12 }", "a") == 12, "spaces around the colon");
        CHECK(!detail::JsonInteger("{\"a\": 1.5}", "a") && !detail::JsonInteger("{\"a\": 1e3}", "a"), "not an integer");
        CHECK(!detail::JsonInteger("{\"a\": \"7\"}", "a") && !detail::JsonInteger("{\"ab\": 7}", "a"), "a string, another key");
        CHECK(!detail::JsonInteger("{\"a\": 99999999999999999999}", "a"), "too large");
    }

    std::string Read(const std::string& path)
    {
        std::ifstream f(path);
        std::stringstream s;
        s << f.rdbuf();
        return s.str();
    }

    size_t Count(const std::string& text, const std::string& what)
    {
        size_t n = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
            n++;
        return n;
    }

    // `first` comes before `second`, both present (from `from` on).
    bool Before(const std::string& text, const std::string& first, const std::string& second, size_t from = 0)
    {
        size_t a = text.find(first, from), b = text.find(second, from);
        return a != std::string::npos && b != std::string::npos && a < b;
    }

    void Wiring(const std::string& rt)
    {
        std::string vfs = Read(rt + "/kernel/vfs.cpp");
        size_t resolve = vfs.find("std::optional<Resolved> Resolve(");
        CHECK(resolve != std::string::npos && Before(vfs, "timedemo::RedirectJoylog(", "auto root = Root(guestPath);", resolve),
            "vfs::Resolve asks for the Joylog file before any device");

        std::string frame = Read(rt + "/game/frame_time.cpp");
        size_t game = frame.find("void GameFrameTime(");
        size_t world = frame.find("void WorldStepTime(");
        CHECK(game != std::string::npos && Before(frame, "if (timedemo::Active())", "timedemo::OnMainFrame();", game) &&
            Before(frame, "timedemo::OnMainFrame();", "bench::OnMainFrame();", game) &&
            Before(frame, "bench::OnMainFrame();", "if (!s_enabled)", game) &&
            Before(frame, "r3.u64 = timedemo::kFixedFrame;", "s_whole.Give(", game),
            "GameFrameTime: the timedemo, then the benchmark, then the fixed step, all before the whole vblanks");
        CHECK(world != std::string::npos && frame.find("if (!timedemo::Active() && (!s_enabled", world) != std::string::npos,
            "WorldStepTime rounds in a timedemo whatever NFSMW_GAME_TIME and the display");

        std::string movie = Read(rt + "/game/movie_pacing.cpp");
        size_t swap = movie.find("PPC_FUNC(sub_825985A0)");
        CHECK(swap != std::string::npos && Before(movie, "ctx.r3.u64 = timedemo::kUncappedSwap;", "uint32_t arg = ctx.r3.u32;", swap),
            "the swap callback's uncapped argument comes before anything else it does");

        std::string main = Read(rt + "/main.cpp");
        CHECK(Count(main, "timedemo::FinishCapture();") == 3, "a capture finishes on every way out (the window, the launcher gone, a quit signal without a window)");
        for (size_t at = main.find("timedemo::FinishCapture();"); at != std::string::npos; at = main.find("timedemo::FinishCapture();", at + 1))
        {
            size_t wait = main.rfind("vfs::WaitForStagedFiles(", at), renderer = main.find("gpu::renderer::Shutdown();", at);
            CHECK(wait != std::string::npos && at - wait < 400 && renderer != std::string::npos && renderer - at < 200,
                "after the saves, before the renderer stops (the guest still runs)");
        }
        CHECK(Before(main, "timedemo::Start(sha);", "platform::StartThread(&thread"), "the timedemo starts before the guest");
        CHECK(Count(main, "std::_Exit(bench::Enabled() ? bench::ExitCode() : 0);") == 3, "a benchmark's status as the exit code (all three ways out)");

        std::string ui = Read(rt + "/ui/ui.cpp");
        CHECK(Count(ui, "XamNotifySystemUi(") == 2 && Before(ui, "if (!timedemo::OnSystemUi(true))", "XamNotifySystemUi(true);") &&
            Before(ui, "if (!timedemo::OnSystemUi(false))", "XamNotifySystemUi(false);"),
            "the runtime's menu asks the timedemo before it pauses the game");
        std::string unleashed = Read(rt + "/kernel/imports_unleashed.cpp");
        size_t selector = unleashed.find("uint32_t XamShowDeviceSelectorUI");
        CHECK(selector != std::string::npos && Before(unleashed, "timedemo::HideGuestSystemUi()", "XamNotifyEnqueueEvent(9, true);", selector),
            "the device selector's notifications go through the timedemo");

        std::string timedemo = Read(rt + "/game/timedemo.cpp");
        size_t brk = timedemo.find("PPC_FUNC(sub_8262CE48)");
        CHECK(brk != std::string::npos && timedemo.find("__imp__sub_8262CE48(ctx, base);", brk) != std::string::npos,
            "every other DbgBreakPoint still reaches the import");
    }
}

int main(int argc, char** argv)
{
    Switches();
    Decisions();
    UncappedSwap();
    JoylogPaths();
    Breaks();
    Meta();
    if (argc > 1)
        Wiring(argv[1]);
    else
        printf("  (no runtime folder given: the wiring not checked)\n");
    printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
    return g_failures ? 1 : 0;
}
