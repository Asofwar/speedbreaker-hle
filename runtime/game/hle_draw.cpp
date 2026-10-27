// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// NFSMW_HLE_DRAW: the hot part of the game's draw path on its render thread
// (nfsmw-main), written natively instead of run as recompiled PowerPC. Off
// by default; NFSMW_HLE_DRAW=1 turns all of it on, or the sum of its parts:
//   2 - D3D's commit of dirty state into the command buffer, which every
//       draw makes (sub_825A3EC8) with its three copy loops: registers
//       (sub_825A28A8, also called from 23 other places in D3D), shader
//       constants (sub_825A2A60) and fetch constants (sub_825A2968);
//   4 - the game's effect code that applies a material's dirty parameters
//       to the device (sub_82699100): the shader float constants copied
//       into D3D's shadow of them, their dirty bits, and the calls for the
//       integer and bool constants and the textures;
//   8 - D3D's SetStreamSource (sub_8258D770, ~3700 calls a frame);
//  16 - D3D's DrawIndexedVertices (sub_82593A58, ~2600 calls a frame): its
//       DRAW_INDX packets (and under predication the tiles' patch records),
//       made from the hook in debug/timeline.cpp, which then skips its
//       per-draw timing.
// What these write is byte for byte what the recompiled code writes: the
// same PM4 packets in the same places of the ring, the same shadow and the
// same dirty bits, and the same D3D calls with the same arguments in the
// same order. Nothing downstream changes: the command processor parses the
// same stream, the pictures and the replay's trace hash are the same.
//
// Why it pays: the recompiled code keeps every guest register in the
// PPCContext and makes every guest load and store a byte-swapped volatile
// access; the effect code (compiled for the console with its loop counters
// on the stack) copies ~70k vec4 constants a frame one at a time that way.
// On a Steam Deck at 5 W (perf of nfsmw-main, race window) the effect code
// was 16% of the thread's samples and the commit with its loops 19.6%.
//
// The ring's segment end: a copy loop that would cross it calls D3D's own
// spill (sub_825A27F0, which moves to the next segment through MakeSpace)
// with the arguments the recompiled loop would pass, so a segment ends where
// it ended before.
//
// NFSMW_HLE_DRAW_VERIFY=1 checks the native code against the recompiled one
// as the game runs: a copy loop that stays within its segment runs both ways
// over the same ring bytes and compares them; an effect apply that makes no
// D3D calls runs both ways over a snapshot of the device's shadow and the
// material's dirty bits. Mismatches are reported on stderr; a summary every
// 2^20 checks.
#include <stdafx.h>
#include <region/region.h>

#include <kernel/function.h>

extern "C" PPC_FUNC(__imp__sub_825A3EC8);  // D3D: commit dirty state (every draw)
extern "C" PPC_FUNC(__imp__sub_825A28A8);  // D3D: copy dirty registers into type-0 packets
extern "C" PPC_FUNC(__imp__sub_825A2A60);  // D3D: copy dirty shader float constants
extern "C" PPC_FUNC(__imp__sub_825A2968);  // D3D: copy dirty fetch constants
extern "C" PPC_FUNC(__imp__sub_82699100);  // game (effects): apply a material's dirty parameters
SB_HOOK(sub_825A27F0);  // D3D: a copy that crosses the segment's end (MakeSpace)
SB_HOOK(sub_825A2B88);  // D3D: vertex fetch constants from the stream sources
SB_HOOK(sub_825A38F8);  // D3D: shaders (IM_LOAD)
SB_HOOK(sub_8259B4C0);  // D3D: SetVertexShaderConstantI
SB_HOOK(sub_8259B510);  // D3D: SetPixelShaderConstantI
SB_HOOK(sub_8259B380);  // D3D: SetVertexShaderConstantB
SB_HOOK(sub_8259B3D8);  // D3D: SetPixelShaderConstantB
SB_HOOK(sub_8258A450);  // D3D: SetTexture
SB_HOOK(sub_82594A78);  // D3D: a resource's last reference gone
extern "C" PPC_FUNC(__imp__sub_8258D770);  // D3D: SetStreamSource(dev, stream, vb, offset, stride)
extern "C" PPC_FUNC(__imp__sub_82593A58);  // D3D: DrawIndexedVertices
SB_HOOK(sub_82597070);  // D3D: MakeSpace (next ring segment)
SB_HOOK(sub_82595E78);  // D3D: grow the predicated draws' record list
extern "C" PPC_FUNC(__imp__sub_825A2B88);
SB_HOOK(sub_825A3EC8);  // below

namespace
{
    const uint32_t s_mode = [] {
        const char* v = std::getenv("NFSMW_HLE_DRAW");
        if (!v || !v[0] || v[0] == '0')
            return 0u;
        uint32_t m = uint32_t(std::strtoul(v, nullptr, 0));
        m = m == 1 ? 15 : m >> 1;  // "1": all of it; else the parts' bits (2, 4, 8, 16)
        fprintf(stderr, "[hle-draw] native draw path:%s%s%s%s (NFSMW_HLE_DRAW=%s)\n", (m & 1) ? " commit" : "",
            (m & 2) ? " effect-constants" : "", (m & 4) ? " stream-source" : "", (m & 8) ? " draw" : "", v);
        return m;
    }();
    const bool s_verify = [] {
        const char* v = std::getenv("NFSMW_HLE_DRAW_VERIFY");
        return s_mode && v && v[0] == '1';
    }();

    std::atomic<uint64_t> s_checked{ 0 }, s_unchecked{ 0 }, s_mismatches{ 0 };

    void Checked(bool ok, const char* what, uint32_t a, uint32_t b)
    {
        if (!ok)
        {
            uint64_t n = s_mismatches.fetch_add(1, std::memory_order_relaxed);
            if (n < 20)
                fprintf(stderr, "[hle-draw] VERIFY MISMATCH %s (%08X %08X)\n", what, a, b);
        }
        uint64_t c = s_checked.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((c & ((1u << 20) - 1)) == 0)
            fprintf(stderr, "[hle-draw] verify: %llu checked, %llu mismatches, %llu not checked\n", (unsigned long long)c,
                (unsigned long long)s_mismatches.load(), (unsigned long long)s_unchecked.load());
    }

    // Guest memory: big-endian; nothing here races with another thread (the
    // device and the ring's current segment are the render thread's).
    inline uint32_t Ld32(uint8_t* base, uint32_t a) { uint32_t v; memcpy(&v, base + a, 4); return __builtin_bswap32(v); }
    inline uint64_t Ld64(uint8_t* base, uint32_t a) { uint64_t v; memcpy(&v, base + a, 8); return __builtin_bswap64(v); }
    inline uint8_t Ld8(uint8_t* base, uint32_t a) { return base[a]; }
    inline void St32(uint8_t* base, uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(base + a, &v, 4); }
    inline void St64(uint8_t* base, uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(base + a, &v, 8); }
    // lvx/stvx: 16 bytes at the address rounded down to 16, as they are.
    inline void Copy16(uint8_t* base, uint32_t dst, uint32_t src) { memcpy(base + (dst & ~15u), base + (src & ~15u), 16); }

    inline uint32_t Clz64(uint64_t x) { return x ? uint32_t(__builtin_clzll(x)) : 64; }
    // sld/srd/srad: the shift's low 7 bits; 64 and more shift everything out.
    inline uint64_t Sld(uint64_t x, uint32_t s) { s &= 0x7F; return (s & 0x40) ? 0 : x << s; }
    inline uint64_t Srd(uint64_t x, uint32_t s) { s &= 0x7F; return (s & 0x40) ? 0 : x >> s; }
    inline uint64_t Srad(uint64_t x, uint32_t s) { s &= 0x7F; return uint64_t((s & 0x40) ? int64_t(x) >> 63 : int64_t(x) >> s); }
    inline uint32_t Rotl32(uint32_t x, uint32_t s) { return (x << s) | (x >> ((32 - s) & 31)); }

    // A call into guest code from here: a frame of `frame` bytes below the
    // caller's stack (with its back chain, as stwu leaves it), lr as the
    // recompiled call sets it.
    struct GuestFrame
    {
        PPCContext& ctx;
        uint8_t* base;
        uint64_t savedSp;
        GuestFrame(PPCContext& c, uint8_t* b, uint32_t frame) : ctx(c), base(b), savedSp(c.r1.u64)
        {
            uint32_t sp = c.r1.u32 - frame;
            St32(b, sp, c.r1.u32);
            c.r1.u64 = sp;
        }
        ~GuestFrame() { ctx.r1.u64 = savedSp; }
        uint32_t Sp() const { return ctx.r1.u32; }
    };

    // sub_825A27F0(dev, cursor, register, source - 4, count, unit): the spill
    // the copy loops call when a run would reach the segment's end. Returns
    // the new cursor.
    uint32_t Spill(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t cursor, uint32_t reg, uint32_t src,
        uint32_t count, uint32_t unit, uint32_t lr)
    {
        GuestFrame f(ctx, base, 144);
        ctx.r3.u64 = dev;
        ctx.r4.u64 = cursor;
        ctx.r5.u64 = reg;
        ctx.r6.u64 = src;
        ctx.r7.u64 = count;
        ctx.r8.u64 = unit;
        ctx.lr = lr;
        sub_825A27F0(ctx, base);
        return ctx.r3.u32;
    }

    // ---- sub_825A28A8(dev, mask, register, source): one type-0 packet per
    // run of set bits (MSB first = `reg`), one dword per register.
    bool RegsFit(uint8_t* base, uint32_t dev, uint64_t mask)
    {
        uint32_t cur = Ld32(base, dev), limit = Ld32(base, dev + 4);
        do
        {
            uint32_t lz = Clz64(mask);
            mask = Sld(mask, lz);
            uint32_t run = Clz64(~mask);
            if (!(cur + run * 4 < limit))
                return false;
            cur += 4 + run * 4;
            mask = Sld(mask, run);
        } while (mask);
        return true;
    }
    void Regs(PPCContext& ctx, uint8_t* base, uint32_t dev, uint64_t mask, uint32_t reg, uint32_t src)
    {
        uint32_t s = src - 4;
        uint32_t cur = Ld32(base, dev);
        do
        {
            uint32_t limit = Ld32(base, dev + 4);
            uint32_t lz = Clz64(mask);
            uint32_t r = reg + lz;
            s += lz * 4;
            mask = Sld(mask, lz);
            uint32_t run = Clz64(~mask);
            if (cur + run * 4 < limit)
            {
                St32(base, cur + 4, ((run - 1) << 16) | r);
                cur += 4;
                reg = run + r;
                memcpy(base + cur + 4, base + s + 4, size_t(run) * 4);
                cur += run * 4;
                s += run * 4;
                mask = Sld(mask, run);
            }
            else
            {
                cur = Spill(ctx, base, dev, cur, r, s, run, 1, 0x825A2914);
                s += run * 4;
                reg = run + r;
                mask = Sld(mask, run);
            }
        } while (mask);
        St32(base, dev, cur);
    }

    // ---- sub_825A2A60(dev, mask, register, source): a bit is 4 vec4s; each
    // run's packet is 16-byte aligned past its header (filler 0x80000000).
    bool ConstsFit(uint8_t* base, uint32_t dev, uint64_t mask)
    {
        uint32_t cur = Ld32(base, dev), limit = Ld32(base, dev + 4);
        do
        {
            uint32_t lz = Clz64(mask);
            mask = Sld(mask, lz);
            uint32_t run = Clz64(~mask), n = run * 16;
            if (!((n + 3) * 4 + cur < limit))
                return false;
            cur += ((uint32_t(-8) - cur) & 15) + 4 + n * 4;
            mask = Sld(mask, run);
        } while (mask);
        return true;
    }
    void Consts(PPCContext& ctx, uint8_t* base, uint32_t dev, uint64_t mask, uint32_t reg, uint32_t src)
    {
        uint32_t s = src - 4;
        uint32_t cur = Ld32(base, dev);
        do
        {
            uint32_t limit = Ld32(base, dev + 4);
            uint32_t lz = Clz64(mask);
            s += lz * 64;
            uint32_t r = lz * 16 + reg;
            mask = Sld(mask, lz);
            uint32_t run = Clz64(~mask), n = run * 16;
            if ((n + 3) * 4 + cur < limit)
            {
                uint32_t pad = (uint32_t(-8) - cur) & 15;
                St32(base, cur + 4, 0x80000000u);
                St32(base, cur + 8, 0x80000000u);
                St32(base, cur + 12, 0x80000000u);
                cur += pad;
                St32(base, cur + 4, ((n - 1) << 16) | r);
                cur += 4;
                reg = n + r;
                for (uint32_t k = 0; k < run; k++)
                {
                    for (uint32_t q = 0; q < 64; q += 16)
                        Copy16(base, cur + 4 + q, s + 4 + q);
                    s += 64;
                    cur += 64;
                }
                mask = Sld(mask, run);
            }
            else
            {
                cur = Spill(ctx, base, dev, cur, r, s, n, 16, 0x825A2AD8);
                s += n * 4;
                reg = n + r;
                mask = Sld(mask, run);
            }
        } while (mask);
        St32(base, dev, cur);
    }

    // ---- sub_825A2968(dev, mask, -, source): fetch constants, 6 dwords a
    // bit from 0x4800, 8-byte aligned past the header.
    bool FetchFit(uint8_t* base, uint32_t dev, uint64_t mask)
    {
        uint32_t cur = Ld32(base, dev), limit = Ld32(base, dev + 4);
        do
        {
            uint32_t lz = Clz64(mask);
            mask = Sld(mask, lz);
            uint32_t run = Clz64(~mask), n = run * 6;
            if (!(n * 4 + cur + 4 < limit))
                return false;
            cur += (cur & 7) + 4 + n * 4;
            mask = Sld(mask, run);
        } while (mask);
        return true;
    }
    void Fetch(PPCContext& ctx, uint8_t* base, uint32_t dev, uint64_t mask, uint32_t src)
    {
        uint32_t s = src - 4;
        uint32_t cur = Ld32(base, dev);
        uint32_t regBase = 0x4800;
        do
        {
            uint32_t limit = Ld32(base, dev + 4);
            uint32_t lz = Clz64(mask);
            mask = Sld(mask, lz);
            uint32_t r = lz * 6 + regBase;
            s += lz * 24;
            uint32_t run = Clz64(~mask), n = run * 6;
            if (n * 4 + cur + 4 < limit)
            {
                St32(base, cur + 4, 0x80000000u);
                cur += cur & 7;
                St32(base, cur + 4, ((n - 1) << 16) | r);
                cur += 4;
                regBase = n + r;
                memcpy(base + cur + 4, base + s + 4, size_t(run) * 24);
                cur += run * 24;
                s += run * 24;
                mask = Sld(mask, run);
            }
            else
            {
                cur = Spill(ctx, base, dev, cur, r, s, n, 6, 0x825A29E4);
                s += n * 4;
                regBase = n + r;
                mask = Sld(mask, run);
            }
        } while (mask);
        St32(base, dev, cur);
    }

    // Verify a copy loop: the recompiled one over the same ring bytes first,
    // then the native one from the same cursor; same bytes, same cursor.
    template <typename Native>
    void VerifyLoop(PPCContext& ctx, uint8_t* base, PPCFunc* original, bool fits, const char* what, uint32_t dev,
        uint64_t mask, uint32_t reg, uint32_t src, Native native)
    {
        if (!fits)
        {
            s_unchecked.fetch_add(1, std::memory_order_relaxed);
            native();
            return;
        }
        const uint32_t c0 = Ld32(base, dev);
        {
            const PPCContext saved = ctx;
            ctx.r3.u64 = dev;
            ctx.r4.u64 = mask;
            ctx.r5.u64 = reg;
            ctx.r6.u64 = src;
            original(ctx, base);
            ctx = saved;
        }
        const uint32_t c1 = Ld32(base, dev);
        std::vector<uint8_t> want(base + c0 + 4, base + c1 + 4);
        St32(base, dev, c0);
        native();
        const uint32_t c2 = Ld32(base, dev);
        Checked(c1 == c2 && memcmp(want.data(), base + c0 + 4, want.size()) == 0, what, c1, c2);
    }

    void RegsEntry(PPCContext& ctx, uint8_t* base, uint32_t dev, uint64_t mask, uint32_t reg, uint32_t src)
    {
        if (s_verify)
            VerifyLoop(ctx, base, __imp__sub_825A28A8, RegsFit(base, dev, mask), "registers", dev, mask, reg, src,
                [&] { Regs(ctx, base, dev, mask, reg, src); });
        else
            Regs(ctx, base, dev, mask, reg, src);
    }
    void ConstsEntry(PPCContext& ctx, uint8_t* base, uint32_t dev, uint64_t mask, uint32_t reg, uint32_t src)
    {
        if (s_verify)
            VerifyLoop(ctx, base, __imp__sub_825A2A60, ConstsFit(base, dev, mask), "constants", dev, mask, reg, src,
                [&] { Consts(ctx, base, dev, mask, reg, src); });
        else
            Consts(ctx, base, dev, mask, reg, src);
    }
    void FetchEntry(PPCContext& ctx, uint8_t* base, uint32_t dev, uint64_t mask, uint32_t reg, uint32_t src)
    {
        if (s_verify)
            VerifyLoop(ctx, base, __imp__sub_825A2968, FetchFit(base, dev, mask), "fetch", dev, mask, reg, src,
                [&] { Fetch(ctx, base, dev, mask, src); });
        else
            Fetch(ctx, base, dev, mask, src);
    }

    void CallDev(PPCContext& ctx, uint8_t* base, PPCFunc* f, uint32_t dev, uint32_t lr)
    {
        GuestFrame frame(ctx, base, 112);
        ctx.r3.u64 = dev;
        ctx.lr = lr;
        f(ctx, base);
    }

    // ---- sub_825A2B88(dev): the vertex declaration's streams ([decl+12] + 1
    // of them, [dev+11408]) into vertex fetch constants: each bound stream's
    // address (+1912 - 8n) and size (+1916 - 8n) and its fetch constant's
    // dirty bit in +32; +48 bit 128 when the streams' strides differ from
    // what the shader was set up with.
    void Streams(uint8_t* base, uint32_t dev)
    {
        const uint32_t decl = Ld32(base, dev + 11408);
        if (!decl)
            return;
        uint32_t count = Ld32(base, decl + 12) + 1;
        uint32_t slot = dev + 12556, fetch = dev + 1916, k = 0x5AAB6;
        do
        {
            if (const uint32_t vb = Ld32(base, slot + 4))
            {
                const uint32_t size = Ld32(base, vb + 16), vbBase = Ld32(base, vb + 12), offset = Ld32(base, slot);
                const uint64_t bit = Srd(0x8000000000000000ull, (k >> 16) + 26);
                const uint32_t address = vbBase + offset;
                const uint32_t dword1 = ((size - offset) & ~0x30000000u) | 0x10000000u;
                const uint32_t dword0 = ((((address >> 20) & 0xFFF) + 512) & 0x1000) + (address & 0x1FFFFFFF);
                St32(base, fetch, dword1);
                St32(base, fetch - 4, dword0);
                St64(base, dev + 32, Ld64(base, dev + 32) | bit);
            }
            slot += 8;
            fetch -= 8;
            k -= 21846;
        } while (--count);
        if ((Ld64(base, dev + 12696) ^ Ld64(base, dev + 11432)) | (Ld64(base, dev + 12688) ^ Ld64(base, dev + 11424)))
            St64(base, dev + 48, Ld64(base, dev + 48) | 128);
    }

    void StreamsEntry(PPCContext& ctx, uint8_t* base, uint32_t dev)
    {
        if (!s_verify)
        {
            Streams(base, dev);
            return;
        }
        // What it writes: the fetch constants of up to 32 streams below
        // +1920 and the dirty words +32 and +48.
        struct Region { uint32_t at, size; };
        const Region regions[] = { { dev + 1920 - 256, 256 }, { dev + 32, 24 } };
        std::vector<uint8_t> before, after;
        for (const Region& r : regions)
            before.insert(before.end(), base + r.at, base + r.at + r.size);
        CallDev(ctx, base, __imp__sub_825A2B88, dev, 0x825A3F00);
        size_t at = 0;
        for (const Region& r : regions)
        {
            after.insert(after.end(), base + r.at, base + r.at + r.size);
            memcpy(base + r.at, before.data() + at, r.size);
            at += r.size;
        }
        Streams(base, dev);
        at = 0;
        bool ok = true;
        for (const Region& r : regions)
        {
            ok = ok && memcmp(base + r.at, after.data() + at, r.size) == 0;
            at += r.size;
        }
        Checked(ok, "streams", dev, 0);
    }

    // ---- sub_825A3EC8(dev): the commit, in its order; the masks are read
    // again after every step, as the recompiled code does (the shader and
    // stream steps set more bits).
    void Commit(PPCContext& ctx, uint8_t* base, uint32_t dev)
    {
        uint64_t m = Ld64(base, dev + 48);
        if (m)
        {
            if (uint32_t(m) & 0x400)
                StreamsEntry(ctx, base, dev);
            m = Ld64(base, dev + 48);
            if (uint32_t(m) & 0x1E0)
                CallDev(ctx, base, sub_825A38F8, dev, 0x825A3F18);
            m = Ld64(base, dev + 48);
            if (uint64_t x = m & 0xFFFFFFFFFC000000ull)
                RegsEntry(ctx, base, dev, x, 0x2300, dev + 11768);
            m = Ld64(base, dev + 48);
            if (uint32_t(m) & 0x03FC0000u)
                RegsEntry(ctx, base, dev, (m << 38) & 0xFF00000000000000ull, 0x2380, dev + 11920);
            St64(base, dev + 48, 0);
        }
        if (uint64_t c = Ld64(base, dev + 16))
        {
            ConstsEntry(ctx, base, dev, c, 0x4000, dev + 1920);
            St64(base, dev + 16, 0);
        }
        if (uint64_t c = Ld64(base, dev + 24))
        {
            ConstsEntry(ctx, base, dev, c, 0x4400, dev + 6016);
            St64(base, dev + 24, 0);
        }
        if (Ld64(base, dev + 32))
        {
            m = Ld64(base, dev + 32);
            if (uint64_t x = m & 0xFFFFFFFF00000000ull)
                FetchEntry(ctx, base, dev, x, 0x4800, dev + 1152);
            m = Ld64(base, dev + 32);
            if (uint32_t(m) & 0x80000000u)
            {
                St64(base, dev + 11440, 0xFFFFFFFFFF000000ull);
                RegsEntry(ctx, base, dev, 0xFFFFFFFFFF000000ull, 0x4900, dev + 10112);
            }
            m = Ld64(base, dev + 32);
            if (uint32_t(m) & 0x3FFFC000u)
                RegsEntry(ctx, base, dev, (m << 34) & 0xFFFF000000000000ull, 0x2000, dev + 11456);
            St64(base, dev + 32, 0);
        }
        if (Ld64(base, dev + 40))
        {
            m = Ld64(base, dev + 40);
            if (uint64_t x = m & 0xFFFFF80000000000ull)
                RegsEntry(ctx, base, dev, x, 0x2100, dev + 11532);
            m = Ld64(base, dev + 40);
            if (m & (31ull << 38))
                RegsEntry(ctx, base, dev, (m << 21) & 0xF800000000000000ull, 0x2180, dev + 11616);
            m = Ld64(base, dev + 40);
            if (m & 0x3FFFC000000ull)
                RegsEntry(ctx, base, dev, (m << 26) & 0xFFF0000000000000ull, 0x2200, dev + 11636);
            m = Ld64(base, dev + 40);
            if (uint32_t(m) & 0x03FFFFE0u)
                RegsEntry(ctx, base, dev, (m << 38) & 0xFFFFF80000000000ull, 0x2280, dev + 11684);
            St64(base, dev + 40, 0);
        }
    }

    // ---- sub_82699100(material): its parameters in two groups, the
    // material's own (dirty bits at +0, 64 a word, [+288] words, tables at
    // [+536]) and a shared one (dirty bits at [+256], [+292] words, tables
    // [+536] or a global set); each has eight kinds of parameter (table
    // slots): VS and PS float constants, VS/PS integer, VS/PS bool, two kinds
    // of texture. A table slot's words mask the dirty words; a set bit is a
    // 16-byte record at the group's records + word * 1024 + bit * 16.
    struct Group
    {
        uint32_t dirty;      // dirty words
        uint32_t countOff;   // material offset of the word count
        uint32_t table;      // slots at +slotBase, records at +recOff, int tails at +80
        uint32_t slotBase;   // 0 or 32
        uint32_t recOff;     // 64 or 68
        uint32_t names;      // r23: records' word 0 picks an entry here (+4: where the value is)
        uint32_t values;     // r24
        uint32_t lr;         // return address of the group's first call site (slot 2)
    };

    // The value a record names: values + the entry's offset.
    inline uint32_t ValueOf(uint8_t* base, const Group& g, uint32_t rec)
    {
        uint32_t w0 = Ld32(base, rec);
        uint32_t v = Ld32(base, g.names + (Rotl32(w0, 17) & 0x1FFF8) + 4);
        return g.values + (Rotl32(v, 4) & 0xFFFF0);
    }

    template <typename Each>
    void ForEachRecord(uint8_t* base, uint32_t material, const Group& g, uint32_t slot, Each each)
    {
        for (uint32_t i = 0; i < Ld32(base, material + g.countOff); i++)
        {
            uint32_t tableWords = Ld32(base, g.table + g.slotBase + slot * 4);
            uint32_t rec = Ld32(base, g.table + g.recOff) + i * 1024;
            uint64_t mask = Ld64(base, g.dirty + 8 * i) & Ld64(base, tableWords + 8 * i);
            while (mask)
            {
                uint32_t lz = Clz64(mask);
                rec += lz * 16;
                mask = Sld(mask, lz);
                uint32_t run = Clz64(~mask);
                uint32_t end = rec + run * 16;
                mask = Sld(mask, run);
                do
                {
                    each(rec);
                    rec += 16;
                } while (rec < end);
            }
        }
    }

    bool AnyRecord(uint8_t* base, uint32_t material, const Group& g, uint32_t slot)
    {
        for (uint32_t i = 0; i < Ld32(base, material + g.countOff); i++)
        {
            uint32_t tableWords = Ld32(base, g.table + g.slotBase + slot * 4);
            if (Ld64(base, g.dirty + 8 * i) & Ld64(base, tableWords + 8 * i))
                return true;
        }
        return false;
    }

    // Slots 0/1: float constants into the shadow (+1920 VS, +6016 PS) and
    // their dirty bits (+16, +24), a bit per 4 vec4s.
    void FloatConstants(uint8_t* base, uint32_t material, uint32_t dev, const Group& g, uint32_t slot)
    {
        const uint32_t wordOff = slot == 0 ? 4 : 8, regAdd = slot == 0 ? 120 : 376, maskOff = slot == 0 ? 16 : 24;
        ForEachRecord(base, material, g, slot, [&](uint32_t rec) {
            const uint32_t src = ValueOf(base, g, rec);
            uint32_t w = Ld32(base, rec + wordOff);
            const uint32_t dst = dev + (((w & 0x3FF) + regAdd) << 4);
            for (uint32_t k = 0; k <= ((w >> 10) & 3); k++)
                Copy16(base, dst + k * 16, src + k * 16);
            w = Ld32(base, rec + wordOff);
            const uint32_t start = w & 0x3FF, n = (w >> 10) & 3;
            const uint32_t a = start >> 2, b = (n + start) >> 2;
            const uint64_t bits = Srd(Srad(0x8000000000000000ull, b - a), a);
            St64(base, dev + maskOff, Ld64(base, dev + maskOff) | bits);
        });
    }

    // A guest vector as the recompiled code holds it (bytes reversed).
    inline simde__m128i LoadV(uint8_t* base, uint32_t a)
    {
        return simde_mm_shuffle_epi8(simde_mm_loadu_si128((const simde__m128i*)(base + (a & ~15u))),
            simde_mm_load_si128((const simde__m128i*)VectorMaskL));
    }
    inline void StoreV(uint8_t* base, uint32_t a, simde__m128i v)
    {
        simde_mm_storeu_si128((simde__m128i*)(base + (a & ~15u)), simde_mm_shuffle_epi8(v, simde_mm_load_si128((const simde__m128i*)VectorMaskL)));
    }

    // Slots 2..7: D3D calls, one a record, in the recompiled code's order.
    void Calls(PPCContext& ctx, uint8_t* base, uint32_t material, uint32_t dev, const Group& g, uint32_t slot, uint32_t sp)
    {
        static PPCFunc* const fns[] = { sub_8259B4C0, sub_8259B510, sub_8259B380, sub_8259B3D8, sub_8258A450, sub_8258A450 };
        // The recompiled call sites' return addresses (group A; group B's
        // are its own): slots 2..7.
        static const uint32_t lrA[] = { 0x82699504, 0x826995FC, 0x826996D4, 0x826997AC, 0x82699860, 0x82699914 };
        static const uint32_t lrB[] = { 0x82699D20, 0x82699E18, 0x82699EF0, 0x82699FC0, 0x8269A074, 0x8269A128 };
        const uint32_t lr = (g.slotBase ? lrB : lrA)[slot - 2];
        PPCFunc* const fn = fns[slot - 2];
        const uint32_t ints = Ld32(base, g.table + 80);
        ForEachRecord(base, material, g, slot, [&](uint32_t rec) {
            ctx.r3.u64 = dev;
            if (slot == 2 || slot == 3)
            {
                // Integer constants: x converted (signed, saturating), y z w
                // the record's tail entry as they are.
                const uint32_t tail = (uint32_t(Ld8(base, rec + (slot == 2 ? 13 : 12))) << 4) + ints;
                ctx.fpscr.enableFlushMode();
                simde__m128i v = simde_mm_vctsxs(simde_mm_castsi128_ps(LoadV(base, ValueOf(base, g, rec))));
                simde__m128 merged = simde_mm_blend_ps(simde_mm_castsi128_ps(v), simde_mm_castsi128_ps(LoadV(base, tail)), 7);
                StoreV(base, sp + 160, simde_mm_castps_si128(merged));
                const uint32_t w = Ld32(base, rec + (slot == 2 ? 4 : 8));
                ctx.r4.u64 = (w >> 12) & 0xFF;
                ctx.r5.u64 = sp + 160;
                ctx.r6.u64 = ((w >> 20) & 3) + 1;
            }
            else if (slot == 4 || slot == 5)
            {
                // Bool constants: unsigned, saturating.
                ctx.fpscr.enableFlushMode();
                StoreV(base, sp + 256, simde_mm_vctuxs(simde_mm_castsi128_ps(LoadV(base, ValueOf(base, g, rec)))));
                const uint32_t w = Ld32(base, rec + (slot == 4 ? 4 : 8));
                ctx.r4.u64 = Ld8(base, rec + (slot == 4 ? 15 : 14));
                ctx.r5.u64 = sp + 256;
                ctx.r6.u64 = (w >> 30) + 1;
            }
            else
            {
                // Textures: SetTexture(dev, sampler, texture).
                const uint32_t w = Ld32(base, rec + (slot == 6 ? 4 : 8));
                ctx.r4.u64 = (w >> 22) & 0xFF;
                ctx.r5.u64 = Ld32(base, ValueOf(base, g, rec));
            }
            ctx.lr = lr;
            fn(ctx, base);
        });
    }

    Group GroupA(uint8_t* base, uint32_t material)
    {
        return Group{ material, 288, Ld32(base, material + 536), 0, 64, Ld32(base, material + 264), Ld32(base, material + 296), 0 };
    }
    // Group B's tables: a global set while both [material+696] and
    // [0x828E0B40] are set, else the material's own.
    uint32_t TableB(uint8_t* base, uint32_t material, uint32_t tableA)
    {
        return (Ld32(base, material + 696) && Ld32(base, 0x828E0B40)) ? Ld32(base, 0x8290E564) : tableA;
    }

    void EffectApply(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t material = ctx.r3.u32;
        const uint32_t dev = Ld32(base, material + 700);
        const uint32_t dirtyB = Ld32(base, material + 256);
        const uint32_t valuesB = Ld32(base, Ld32(base, material + 300));
        const uint32_t namesB = Ld32(base, Ld32(base, material + 268));
        GuestFrame frame(ctx, base, 448);
        const uint32_t sp = frame.Sp();
        const Group a = GroupA(base, material);
        FloatConstants(base, material, dev, a, 0);
        FloatConstants(base, material, dev, a, 1);
        for (uint32_t slot = 2; slot < 8; slot++)
            Calls(ctx, base, material, dev, a, slot, sp);
        const Group b{ dirtyB, 292, TableB(base, material, a.table), 32, 68, namesB, valuesB, 0 };
        FloatConstants(base, material, dev, b, 0);
        FloatConstants(base, material, dev, b, 1);
        for (uint32_t slot = 2; slot < 8; slot++)
            Calls(ctx, base, material, dev, b, slot, sp);
        // dcbzl: the dirty words' cache lines, 128 bytes each.
        memset(base + (material & ~127u), 0, 128);
        memset(base + (Ld32(base, material + 256) & ~127u), 0, 128);
    }

    // Verify: an apply that makes no D3D calls, both ways over the same
    // starting state (the device's constants shadow and dirty words, the
    // material's dirty lines).
    void EffectVerify(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t material = ctx.r3.u32;
        const uint32_t dev = Ld32(base, material + 700);
        const Group a = GroupA(base, material);
        const uint32_t tableB = TableB(base, material, a.table);
        const Group b{ Ld32(base, material + 256), 292, tableB, 32, 68, 0, 0, 0 };
        for (uint32_t slot = 2; slot < 8; slot++)
            if (AnyRecord(base, material, a, slot) || AnyRecord(base, material, b, slot))
            {
                s_unchecked.fetch_add(1, std::memory_order_relaxed);
                EffectApply(ctx, base);
                return;
            }
        // What the float constants touch: the dirty words (+16, +24) and the
        // shadow (+1920 VS, +6016 PS, 256 vec4s each). Only these are saved
        // and put back: other threads' D3D code (vblank, interrupts) may
        // write elsewhere in the device meanwhile.
        struct Region { uint32_t at, size; };
        const Region regions[] = { { dev + 16, 16 }, { dev + 1920, 8192 }, { material & ~127u, 128 }, { b.dirty & ~127u, 128 } };
        std::vector<uint8_t> before, after;
        for (const Region& r : regions)
            before.insert(before.end(), base + r.at, base + r.at + r.size);
        {
            const PPCContext saved = ctx;
            SB_ORIGINAL(sub_82699100)(ctx, base);
            ctx = saved;
        }
        for (const Region& r : regions)
            after.insert(after.end(), base + r.at, base + r.at + r.size);
        size_t at = 0;
        for (const Region& r : regions)
        {
            memcpy(base + r.at, before.data() + at, r.size);
            at += r.size;
        }
        EffectApply(ctx, base);
        bool ok = true;
        at = 0;
        for (const Region& r : regions)
        {
            ok = ok && memcmp(base + r.at, after.data() + at, r.size) == 0;
            at += r.size;
        }
        Checked(ok, "effect constants", material, dev);
    }

    // A guest address as the GPU sees it (D3D's GPU_CONVERT_CPU_TO_GPU_ADDRESS).
    inline uint32_t GpuAddress(uint32_t a) { return ((((a >> 20) & 0xFFF) + 512) & 0x1000) + (a & 0x1FFFFFFF); }

    struct DrawPacket
    {
        uint32_t viz, initiator, dmaBase, dmaSize;  // DRAW_INDX: viz query, VGT_DRAW_INITIATOR, VGT_DMA_BASE, VGT_DMA_SIZE
    };

    // DRAW_INDX of `count` indices from `start` of the device's index buffer.
    DrawPacket MakeDraw(uint8_t* base, uint32_t dev, uint32_t start, uint32_t count)
    {
        const uint32_t ib = Ld32(base, dev + 12532);
        const uint32_t ibInfo = Ld32(base, ib), ibBase = Ld32(base, ib + 12);
        const uint32_t prim = Ld8(base, dev + 12299);
        const uint32_t viz = (uint32_t(Ld8(base, dev + 12234 + prim)) | (prim ? 0x100u : 0u)) & 0x13F;
        DrawPacket p{ viz, (count << 16) | 4, GpuAddress(start * 2 + ibBase), (count & 0x00FFFFFF) | 0x40000000 };
        if (ibInfo & 0x80000000u)  // 32-bit indices
        {
            p.initiator |= 0x800;
            p.dmaBase = GpuAddress(start * 4 + ibBase);
            p.dmaSize = ((count << 1) & 0x00FFFFFE) | 0x80000000u;
        }
        return p;
    }

    constexpr uint32_t kDrawIndx = 0xC0032201, kPredBegin = 0xC0015000, kPredEnd = 0xC0015A01, kSetBin = 0xC0004601;

    // The words a draw writes past `cursor` (5, or 16 under predication with
    // its record at `rec`).
    uint32_t DrawWords(uint32_t* w, const DrawPacket& p, bool predicated, uint32_t rec)
    {
        if (!predicated)
        {
            const uint32_t words[] = { kDrawIndx, p.viz, p.initiator, p.dmaBase, p.dmaSize };
            memcpy(w, words, sizeof(words));
            return 5;
        }
        const uint32_t words[] = { kPredBegin, 0xFFFFFFFFu, 0, kDrawIndx, p.viz, p.initiator, p.dmaBase, p.dmaSize,
            kPredBegin, 0x80000000u, 0, kPredEnd, 26, (GpuAddress(rec + 4) & ~1u) | 1, kSetBin, 25 };
        memcpy(w, words, sizeof(words));
        return 16;
    }

    uint32_t MakeSpace(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t lr)
    {
        GuestFrame frame(ctx, base, 240);
        ctx.r3.u64 = dev;
        ctx.lr = lr;
        sub_82597070(ctx, base);
        return ctx.r3.u32;
    }

    // sub_82593A58(dev, prim, baseVertex, start, count): D3D's
    // DrawIndexedVertices as the game calls it (prim 4, base vertex 0, from
    // sub_82439BC0 and LTCG copies); split into draws of at most 65532
    // indices. Under predication (BeginTiling's tiles, +10432 bit 4) each
    // draw also gets a record in the device's list at +13536 (its place in
    // the ring and three 0xF00D words; sub_82595E78 grows the list) and the
    // packets around it that let the tiles patch it.
    void DrawIndexed(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t dev = ctx.r3.u32;
        uint32_t start = ctx.r6.u32, count = ctx.r7.u32;
        if (Ld32(base, dev + 11540))
        {
            St32(base, dev + 11540, 0);
            St64(base, dev + 40, Ld64(base, dev + 40) | 0x2000000000000000ull);
        }
        {
            GuestFrame frame(ctx, base, 240);
            ctx.r3.u64 = dev;
            ctx.lr = 0x82593AA4;
            sub_825A3EC8(ctx, base);
        }
        uint32_t cur = Ld32(base, dev);
        if (cur > Ld32(base, dev + 8))
            cur = MakeSpace(ctx, base, dev, 0x82593AC0);
        while (true)
        {
            const uint32_t n = count > 65535 ? 65532 : count;
            const DrawPacket p = MakeDraw(base, dev, start, n);
            uint32_t w[16], words;
            if (Ld8(base, dev + 10432) & 4)
            {
                const uint32_t list = dev + 13536;
                uint32_t rec = Ld32(base, list + 8);
                if (rec + 16 > Ld32(base, list + 12))
                {
                    GuestFrame frame(ctx, base, 240);
                    ctx.r3.u64 = list;
                    ctx.lr = 0x82593BC0;
                    sub_82595E78(ctx, base);
                    rec = ctx.r3.u32;
                }
                St32(base, rec, cur);
                St32(base, rec + 8, 0xF00D);
                St32(base, rec + 12, 0xF00D);
                St32(base, rec + 4, 0xF00D);
                St32(base, dev + 13544, rec + 16);
                words = DrawWords(w, p, true, rec);
            }
            else
                words = DrawWords(w, p, false, 0);
            for (uint32_t i = 0; i < words; i++)
                St32(base, cur + 4 + i * 4, w[i]);
            cur += words * 4;
            count -= n;
            St32(base, dev, cur);
            if (count == 0)
                return;
            start += n;
            if (cur > Ld32(base, dev + 8))
                cur = MakeSpace(ctx, base, dev, 0x82593C9C);
        }
    }

    // Verify a draw after the recompiled one: one draw, no new segment: its
    // words end at the cursor.
    void DrawVerify(PPCContext& ctx, uint8_t* base)
    {
        const uint32_t dev = ctx.r3.u32, start = ctx.r6.u32, count = ctx.r7.u32;
        const uint32_t list = dev + 13536;
        const uint32_t recBefore = Ld32(base, list + 8), recLimit = Ld32(base, list + 12);
        {
            const PPCContext saved = ctx;
            __imp__sub_82593A58(ctx, base);
            ctx = saved;
        }
        const bool predicated = Ld8(base, dev + 10432) & 4;
        if (count > 65535 || (predicated && recBefore + 16 > recLimit))
        {
            s_unchecked.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const uint32_t cur = Ld32(base, dev);
        uint32_t w[16];
        const uint32_t words = DrawWords(w, MakeDraw(base, dev, start, count), predicated, recBefore);
        bool ok = true;
        for (uint32_t i = 0; i < words; i++)
            ok = ok && Ld32(base, cur - (words - 1 - i) * 4) == w[i];
        if (predicated)
            ok = ok && Ld32(base, list + 8) == recBefore + 16 && Ld32(base, recBefore) == cur - words * 4 &&
                Ld32(base, recBefore + 4) == 0xF00D && Ld32(base, recBefore + 8) == 0xF00D && Ld32(base, recBefore + 12) == 0xF00D;
        Checked(ok, predicated ? "draw (predicated)" : "draw", cur, words);
    }
}

namespace hledraw
{
    // Called first by the DrawIndexedVertices hook (debug/timeline.cpp):
    // true when the draw was made here.
    bool DrawIndexedVertices(PPCContext& ctx, uint8_t* base)
    {
        if (!(s_mode & 8))
            return false;
        if (s_verify)
            DrawVerify(ctx, base);
        else
            DrawIndexed(ctx, base);
        return true;
    }
}

SB_HOOK(sub_825A3EC8)
{
    if (!(s_mode & 1))
    {
        SB_ORIGINAL(sub_825A3EC8)(ctx, base);
        return;
    }
    Commit(ctx, base, ctx.r3.u32);
}

SB_HOOK(sub_825A28A8)
{
    if (!(s_mode & 1))
    {
        SB_ORIGINAL(sub_825A28A8)(ctx, base);
        return;
    }
    RegsEntry(ctx, base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32);
}

SB_HOOK(sub_825A2A60)
{
    if (!(s_mode & 1))
    {
        SB_ORIGINAL(sub_825A2A60)(ctx, base);
        return;
    }
    ConstsEntry(ctx, base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32);
}

SB_HOOK(sub_825A2968)
{
    if (!(s_mode & 1))
    {
        SB_ORIGINAL(sub_825A2968)(ctx, base);
        return;
    }
    FetchEntry(ctx, base, ctx.r3.u32, ctx.r4.u64, ctx.r5.u32, ctx.r6.u32);
}

SB_HOOK(sub_82699100)
{
    if (!(s_mode & 2))
    {
        SB_ORIGINAL(sub_82699100)(ctx, base);
        return;
    }
    if (s_verify)
        EffectVerify(ctx, base);
    else
        EffectApply(ctx, base);
}

// SetStreamSource(dev, stream, vb, offset, stride): the new buffer's
// reference (0x80000 in its Common word) and the stream's dirty bit (1024 in
// +48), the old one's reference dropped (its fence, +4, the device's current
// one, [+10396]; freed when no reference is left), the stream's slot.
SB_HOOK(sub_8258D770)
{
    if (!(s_mode & 4))
    {
        SB_ORIGINAL(sub_8258D770)(ctx, base);
        return;
    }
    const uint32_t dev = ctx.r3.u32, stream = ctx.r4.u32, vb = ctx.r5.u32, offset = ctx.r6.u32, stride = ctx.r7.u32;
    if (vb)
    {
        St32(base, vb, Ld32(base, vb) + 0x80000);
        St64(base, dev + 48, Ld64(base, dev + 48) | 1024);
    }
    const uint32_t slot = dev + 12556 + stream * 8;
    if (const uint32_t old = Ld32(base, slot + 4))
    {
        const uint32_t w = Ld32(base, old) - 0x80000, fence = Ld32(base, dev + 10396);
        St32(base, old + 4, fence);
        St32(base, old, w);
        if ((w & 0x00F800FFu) == 0)
        {
            // As the recompiled call has them.
            GuestFrame frame(ctx, base, 144);
            ctx.r3.u64 = old;
            ctx.r9.u64 = fence;
            ctx.r10.u64 = 0;
            ctx.r11.u64 = w;
            ctx.lr = 0x8258D7F4;
            sub_82594A78(ctx, base);
        }
    }
    St32(base, slot + 4, vb);
    St32(base, slot, offset);
    base[dev + stream + 12688] = uint8_t(stride >> 2);
}
