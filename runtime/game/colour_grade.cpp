// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The game's colour grade, at its source (Settings > Display > Remove Orange Tint).
//
// sub_82223BB8 is the post-processing function that hands the final grade its constants. It
// sends four 16-byte vectors, stored in its object at +304, +320, +336 and +352, to four shader
// constant registers (their numbers come from the effect, which is why the grade is at c6 in one
// shader variant and c9 in the other), through sub_82449820, the Direct3D "set a constant by handle"
// call. The four vectors are the coefficients A0..A3 of a cubic that sets, with a per-pixel value
// w, how much of the desaturated picture to use (the x components, which add up to 1) and the gain on
// red, green and blue (the y, z and w components):
//     gain(w) = ((A3 * w + A2) * w + A1) * w + A0
// The game's values give blue a third of red's gain (1.40 : 1.24 : 0.41 at w = 1): the orange of its
// look.
//
// Patching the object before sub_82223BB8 runs does nothing: it recomputes the block itself first
// (through sub_82224C78 and sub_82223AA0). So the block is replaced where it is sent: sub_82449820, a
// call used all over the renderer, is overridden (the generated function is weak; no ppc/
// regeneration), and when it is called from the first of that function's four sends (return address
// 0x82223CB0, with r5 the block's start) the 64 bytes are replaced before the original runs, the
// other three sends then reading the replaced values. Each coefficient's three channel gains are
// moved toward their luminance-weighted mean (Rec. 709), so all three channels get the same curve:
// brightness kept, hue shift gone.
//
// The values stay the game's own: the original block is remembered, and what is written is
// recomputed from it at every send, so the setting works live (down to 0, the game's own colour), and
// a block the game changes (a new look) is taken up as the new original.
#include <stdafx.h>

#include <kernel/function.h>
#include <user/settings.h>

#include <atomic>
#include <mutex>
#include <vector>
#include <set>

extern "C" PPC_FUNC(__imp__sub_82449820);
extern "C" PPC_FUNC(__imp__sub_824491F8);
extern "C" PPC_FUNC(__imp__sub_82448FE0);

namespace { void ApplyGuestPatches(uint8_t* base); }
extern "C" PPC_FUNC(__imp__sub_82458C20);
extern "C" PPC_FUNC(__imp__sub_82589EA8);
extern "C" PPC_FUNC(__imp__sub_8245D1B8);

namespace
{
    constexpr uint32_t kFirstSend = 0x82223CB0;  // the return address of the first of sub_82223BB8's four sends
    constexpr float kWr = 0.2125f, kWg = 0.7154f, kWb = 0.0721f;  // Rec. 709 luminance weights

    struct State
    {
        bool valid = false;
        uint32_t object = 0;
        uint32_t original[16] = {};  // the game's values, as stored (big-endian words, read as host integers)
        uint32_t written[16] = {};   // what this function last stored
    } s_state;
    std::mutex s_mutex;

    float ToFloat(uint32_t bits)
    {
        float f;
        memcpy(&f, &bits, sizeof(f));
        return f;
    }

    uint32_t ToBits(float f)
    {
        uint32_t bits;
        memcpy(&bits, &f, sizeof(bits));
        return bits;
    }

    // The block has the shape of the grade: coefficients whose x components add up to 1 (the weight of
    // the desaturated picture reaches 1 at w = 1), with positive channel gains there. Anything else
    // (another look, a block not yet set up) is left alone.
    bool LooksLikeGrade(const uint32_t* words)
    {
        float a[16];
        for (int i = 0; i < 16; i++)
        {
            a[i] = ToFloat(words[i]);
            if (!std::isfinite(a[i]) || std::fabs(a[i]) > 8.0f)
                return false;
        }
        const float sumX = a[0] + a[4] + a[8] + a[12];
        if (std::fabs(sumX - 1.0f) > 0.004f)
            return false;
        for (int c = 1; c <= 3; c++)
            if (a[c] + a[4 + c] + a[8 + c] + a[12 + c] <= 0.1f)
                return false;
        return true;
    }
}

namespace
{
    // NFSMW_FIND_SEND=<value>: every constant send (either function) whose first float is that value, once per call site and handle.
    void FindSend(const char* which, PPCContext& ctx, uint8_t* base)
    {
        static const char* want = std::getenv("NFSMW_FIND_SEND");
        if (!want)
            return;
        static const float target = float(std::atof(want));
        const uint32_t bits = PPC_LOAD_U32(ctx.r5.u32);
        float v;
        memcpy(&v, &bits, 4);
        if (v != target)
            return;
        static std::mutex m;
        static std::set<std::pair<uint32_t, uint32_t>> seen;
        std::lock_guard lock(m);
        if (seen.insert({ uint32_t(ctx.lr), ctx.r4.u32 }).second)
        {
            float w[4];
            for (int i = 0; i < 4; i++)
            {
                const uint32_t b = PPC_LOAD_U32(ctx.r5.u32 + i * 4);
                memcpy(&w[i], &b, 4);
            }
            fprintf(stderr, "[findsend] %s from %08X, handle %08X: %.5f %.5f %.5f %.5f\n", which, uint32_t(ctx.lr), ctx.r4.u32, w[0], w[1], w[2], w[3]);
        }
    }
}

PPC_FUNC(sub_82449820)
{
    {
        static std::atomic<uint32_t> calls{ 0 };
        if ((calls.fetch_add(1, std::memory_order_relaxed) & 63u) == 0)
            ApplyGuestPatches(base);
    }
    FindSend("vec4", ctx, base);
    if (ctx.lr != kFirstSend)
    {
        __imp__sub_82449820(ctx, base);
        return;
    }

    const uint32_t block = ctx.r5.u32;  // the four vec4s, A0 first
    uint32_t current[16];
    for (int i = 0; i < 16; i++)
        current[i] = PPC_LOAD_U32(block + i * 4);

    {
        std::lock_guard lock(s_mutex);
        // What is there now: what this hook wrote (then the original is the one it remembered), or
        // something the game stored since (then that is the new original).
        const bool ours = s_state.valid && s_state.object == block && memcmp(current, s_state.written, sizeof(current)) == 0;
        if (!ours)
        {
            if (!LooksLikeGrade(current))
            {
                __imp__sub_82449820(ctx, base);
                return;
            }
            memcpy(s_state.original, current, sizeof(current));
            s_state.object = block;
            s_state.valid = true;
        }

        const float strength = std::clamp(settings::GetFloat(settings::Id::OrangeTint), 0.0f, 1.0f);
        uint32_t out[16];
        memcpy(out, s_state.original, sizeof(out));
        if (strength > 0.0f)
        {
            for (int i = 0; i < 4; i++)
            {
                const uint32_t* o = s_state.original + i * 4;
                const float mean = kWr * ToFloat(o[1]) + kWg * ToFloat(o[2]) + kWb * ToFloat(o[3]);
                for (int c = 1; c <= 3; c++)
                {
                    const float v = ToFloat(o[c]);
                    out[i * 4 + c] = ToBits(v + (mean - v) * strength);
                }
            }
        }
        // Settings > Graphics > Color Filter, here too, so in every scene
        // (the race's, the pursuits', the cutscenes' variants all take this
        // block): the curve moved toward the neutral one, A0 (0, 1, 1, 1) and
        // the rest zeros (all of the way: Off, half: Soft; the curve is linear
        // in its coefficients). Its vignette stays the renderer's
        // (gpu/renderer.cpp's ColorFilterConstants).
        const auto filter = settings::ColorFilterMode(settings::GetInt(settings::Id::ColorFilter));
        if (filter != settings::ColorFilterMode::Original)
        {
            const float f = filter == settings::ColorFilterMode::Off ? 1.0f : 0.5f;
            for (int i = 0; i < 16; i++)
            {
                const float neutral = i == 1 || i == 2 || i == 3 ? 1.0f : 0.0f;
                const float v = ToFloat(out[i]);
                out[i] = ToBits(v + (neutral - v) * f);
            }
        }
        memcpy(s_state.written, out, sizeof(out));
        for (int i = 0; i < 16; i++)
            PPC_STORE_U32(block + i * 4, out[i]);

        static bool logged = false;
        if (!logged && strength > 0.0f)
        {
            logged = true;
            fprintf(stderr, "[grade] the game's colour grade (block at guest %08X) is neutralised where it is sent, strength %.2f: "
                "gains at w = 1 were %.2f : %.2f : %.2f\n", block, strength,
                ToFloat(s_state.original[1]) + ToFloat(s_state.original[5]) + ToFloat(s_state.original[9]) + ToFloat(s_state.original[13]),
                ToFloat(s_state.original[2]) + ToFloat(s_state.original[6]) + ToFloat(s_state.original[10]) + ToFloat(s_state.original[14]),
                ToFloat(s_state.original[3]) + ToFloat(s_state.original[7]) + ToFloat(s_state.original[11]) + ToFloat(s_state.original[15]));
        }
    }
    __imp__sub_82449820(ctx, base);
}


// The scalar sends of the same function (sub_82223BB8, 0x82223BB8..0x82224000 as return addresses).
PPC_FUNC(sub_824491F8)
{
    FindSend("scalar", ctx, base);
    // g_fBloomScale (the game's own bloom, 4.0 by default), sent from sub_82224380 (looked up by name at
    // 0x82224400, sent at return address 0x8222441C): scaled by the Bloom setting, 0.4 being the game's own.
    if (ctx.lr == 0x8222441C)
    {
        const float bloom = std::clamp(settings::GetFloat(settings::Id::Bloom), 0.0f, 1.0f);
        const uint32_t bits = PPC_LOAD_U32(ctx.r5.u32);
        float v;
        memcpy(&v, &bits, 4);
        if (std::isfinite(v) && v > 0.0f && v <= 64.0f)
        {
            v *= bloom / 0.4f;
            uint32_t out;
            memcpy(&out, &v, 4);
            PPC_STORE_U32(ctx.r5.u32, out);
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                fprintf(stderr, "[bloom] the game's own bloom scale (g_fBloomScale) is set by the Bloom setting: x%.2f\n", bloom / 0.4f);
            }
        }
    }
    // g_fBrightPassThreshold (0.5: what the bloom takes of the picture, the
    // sky's haze most of all), looked up by name at 0x822232E4 in
    // sub_822232xx and sent at 0x822232FC: raised by Settings > Graphics >
    // Glow toward 0.95 (only the brightest lights and the sun still glow), an
    // idea from NFSMW Recompiled's sky glow option.
    if (ctx.lr == 0x822232FC)
    {
        const float glow = std::clamp(settings::GetFloat(settings::Id::Glow), 0.0f, 1.0f);
        const uint32_t bits = PPC_LOAD_U32(ctx.r5.u32);
        float v;
        memcpy(&v, &bits, 4);
        if (glow < 1.0f && std::isfinite(v) && v >= 0.0f && v < 0.95f)
        {
            v += (0.95f - v) * (1.0f - glow);
            uint32_t out;
            memcpy(&out, &v, 4);
            PPC_STORE_U32(ctx.r5.u32, out);
        }
    }
    static const bool log = std::getenv("NFSMW_LOG_GRADE_SENDS") != nullptr;
    if (log && ctx.lr >= 0x82223BB8 && ctx.lr < 0x82224000)
    {
        static std::mutex logMutex;
        std::lock_guard lock(logMutex);
        static int count = 0;
        if (count++ < 40)
        {
            float v[4];
            for (int i = 0; i < 4; i++)
            {
                const uint32_t bits = PPC_LOAD_U32(ctx.r5.u32 + i * 4);
                memcpy(&v[i], &bits, 4);
            }
            fprintf(stderr, "[gradesend] scalar send from %08X, handle %08X: %.5f %.5f %.5f %.5f\n", uint32_t(ctx.lr), ctx.r4.u32, v[0], v[1], v[2], v[3]);
        }
    }
    __imp__sub_824491F8(ctx, base);
}


// The effect parameter lookup by name (sub_82448FE0, r4 the name), logged for the post-processing code's lookups.
PPC_FUNC(sub_82448FE0)
{
    static const bool log = std::getenv("NFSMW_LOG_PARAM_NAMES") != nullptr;
    const uint32_t lr = uint32_t(ctx.lr), name = ctx.r4.u32;
    __imp__sub_82448FE0(ctx, base);
    if (log)
    {
        static std::mutex m;
        static std::set<std::pair<uint32_t, uint32_t>> seen;
        std::lock_guard lock(m);
        if (seen.insert({ lr, name }).second)
        {
            char text[64] = {};
            for (int i = 0; i < 63; i++)
            {
                text[i] = char(PPC_LOAD_U8(name + i));
                if (!text[i])
                    break;
            }
            fprintf(stderr, "[paramname] lookup from %08X: \"%s\" -> handle %08X\n", lr, text, ctx.r3.u32);
        }
    }
}


// Rich Blacks, at the source: sub_82458C20 is the game's display-ramp builder. For each of 256 entries it
// stores pow(k / 255, f1) (scaled and offset into 16 bits) into the red, green and blue tables at
// r3 + 2, +512, +1024, where f1 is the exponent the game derives from its brightness setting (about 0.95
// for the lighter of the ramps it sets at start, 1.2 for the deeper). The exponent is moved toward 1.4
// by the Rich Blacks setting, so the ramp the game itself programs has deeper blacks and darks. The game builds
// its ramp at start (and when its brightness changes), so the setting applies from the next start.
// NFSMW_GUEST_PATCH=<addr>:<old>=<new>[,...] (hex address; decimal int or float with a '.'): a word of the game's
// data is changed once it holds <old>. The game fills most of its tables a while after start, so each patch
// waits for its original value (checked every 64th call of the busiest hooked function) and is applied once.
// Experiments with the game's own constants, such as its shadow map size.
namespace
{
    struct GuestPatch { uint32_t address, from, to; bool done; };
    std::vector<GuestPatch> s_patches;
    std::once_flag s_patchParse;

    uint32_t WordOf(const std::string& value)
    {
        uint32_t bits;
        if (value.find('.') != std::string::npos)
        {
            const float f = std::strtof(value.c_str(), nullptr);
            memcpy(&bits, &f, 4);
        }
        else
            bits = uint32_t(std::strtoul(value.c_str(), nullptr, 10));
        return bits;
    }

    void ApplyGuestPatches(uint8_t* base)
    {
        std::call_once(s_patchParse, [] {
            const char* spec = std::getenv("NFSMW_GUEST_PATCH");
            if (!spec)
                return;
            std::string text(spec);
            size_t at = 0;
            while (at < text.size())
            {
                size_t comma = text.find(',', at);
                std::string item = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                at = comma == std::string::npos ? text.size() : comma + 1;
                const size_t colon = item.find(':'), eq = item.find('=');
                if (colon == std::string::npos || eq == std::string::npos || eq < colon)
                    continue;
                s_patches.push_back({ uint32_t(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16)),
                    WordOf(item.substr(colon + 1, eq - colon - 1)), WordOf(item.substr(eq + 1)), false });
            }
        });
        for (GuestPatch& p : s_patches)
            if (!p.done && PPC_LOAD_U32(p.address) == p.from)
            {
                PPC_STORE_U32(p.address, p.to);
                p.done = true;
                fprintf(stderr, "[patch] %08X: %08X -> %08X\n", p.address, p.from, p.to);
            }
    }
}

PPC_FUNC(sub_82458C20)
{
    constexpr double kDeep = 1.4;
    const float rich = std::clamp(settings::GetFloat(settings::Id::RichBlacks), 0.0f, 1.0f);
    const double e = ctx.f1.f64;
    if (rich > 0.0f && std::isfinite(e) && e > 0.2 && e < kDeep)
    {
        ctx.f1.f64 = e + (kDeep - e) * rich;
        static int logged = 0;
        if (logged++ < 4)
            fprintf(stderr, "[ramp] the game's display ramp exponent %.3f is deepened to %.3f (Rich Blacks %.2f)\n", e, ctx.f1.f64, rich);
    }
    __imp__sub_82458C20(ctx, base);
}


// Shadow Resolution, at the source. The sun's shadow map is 1600 x 1600 in the game's code, in these places:
//   sub_82454B58 and sub_82454B60: its width and height getters, a bare "li r3,1600" each (what the shadow
//     code reads when it sets up the pass, its viewport and its texel size);
//   sub_82454C18, the shadow system's creation: it stores 1600 twice into a parameter block it passes to
//     sub_8245D1B8 (at +8 and +12), and twice calls sub_82589EA8 (create a render target: r3 width, r4 height)
//     with 1600 immediates, for the two maps.
// The getters are replaced, and the two calls get the size on the way in (told apart by their return
// addresses 0x82454C7C and 0x82454CB8). Chosen at start by the Shadow Resolution setting.
namespace
{
    uint32_t ShadowSize()
    {
        static const uint32_t size = [] {
            const int32_t v = settings::GetInt(settings::Id::ShadowResolution);
            const uint32_t s = (v >= 400 && v <= 1600) ? uint32_t(v) & ~15u : 1600u;
            if (s != 1600)
                fprintf(stderr, "[shadow] the sun's shadow map is created at %ux%u instead of 1600x1600\n", s, s);
            return s;
        }();
        return size;
    }
}

PPC_FUNC(sub_82454B58)
{
    ctx.r3.s64 = ShadowSize();
}

PPC_FUNC(sub_82454B60)
{
    ctx.r3.s64 = ShadowSize();
}

PPC_FUNC(sub_82589EA8)
{
    if (ctx.lr == 0x82454CB8 && ctx.r3.u32 == 1600 && ctx.r4.u32 == 1600)
        ctx.r3.u32 = ctx.r4.u32 = ShadowSize();
    __imp__sub_82589EA8(ctx, base);
}

PPC_FUNC(sub_8245D1B8)
{
    if (ctx.lr == 0x82454C7C)
    {
        const uint32_t block = ctx.r5.u32;
        if (PPC_LOAD_U32(block + 8) == 1600 && PPC_LOAD_U32(block + 12) == 1600)
        {
            PPC_STORE_U32(block + 8, ShadowSize());
            PPC_STORE_U32(block + 12, ShadowSize());
        }
    }
    __imp__sub_8245D1B8(ctx, base);
}
