// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// EA's JDLZ decompressor (sub_823BD558), with bounds.
//
// The game's own loop takes a compressed block ("ZLDJ", version 2; +8 the
// size it decompresses to, +12 its own size, the header included) and a
// destination. It counts the input down from the block's size less 18 and
// stops only when the count is exactly 0. A literal is not written past the
// end; a back-reference copy is, and a copy takes 2 off the count, so in a
// block that is short of its stated size the count can step over 0 and the
// loop goes on through whatever follows, writing until something faults.
//
// Such blocks exist: a fan translation (Russian) appends a bundle to
// NFS/ZZDATA0.BIN whose packer leaves the last 17 bytes of the bundle out,
// and the front-end screen at its very end (UI_XENON_BLANK.fng, shown on the
// way into Xbox Live) is then cut short inside it. Decompressed, it wrote
// megabytes over the heap, the online code's state among them, and the game
// went to a black screen right after logging in to the lobby.
//
// Here the same algorithm runs on the host, step for step, with two bounds:
// nothing is written past the stated size, and the input is not read past
// the block. A sound block decompresses to the same bytes as before (its
// count reaches 0 exactly where the original's does); a damaged one fills
// its stated size and stops.
#include <stdafx.h>
#include <region/region.h>
#include <kernel/function.h>

extern "C" PPC_FUNC(__imp__sub_823BD558);

namespace
{
    // The cut-short screen itself (above): an 820-byte FEng package ("FEn\xE7",
    // 812 bytes of chunks) whose last 24 bytes the translation's bundle lost.
    // Its chunks up to there are whole: the package's resource list, PkgR,
    // 16 bytes, holds its id (Mi) and, an empty screen, no resources (MC 0);
    // the targets' list after it, Targ, is just as empty (Tc 0). Those two
    // fields are the missing 24 bytes, written over whatever the decompressor
    // made of the garbage past the cut, once the rest matches byte for byte.
    void RepairBlankScreen(uint32_t out, uint32_t size)
    {
        static const uint8_t kKept[16] = { 'P', 'k', 'g', 'R', 0x10, 0, 0, 0, 'M', 'i', 4, 0, 0x42, 0x51, 0x6C, 0x7F };
        static const uint8_t kLost[24] = { 'M', 'C', 4, 0, 0, 0, 0, 0, 'T', 'a', 'r', 'g', 8, 0, 0, 0, 'T', 'c', 4, 0, 0, 0, 0, 0 };
        uint8_t* base = g_memory.base;
        if (size != 820 || memcmp(base + out, "FEn\xE7\x2C\x03\x00\x00", 8) != 0 || memcmp(base + out + 780, kKept, sizeof(kKept)) != 0)
            return;
        if (memcmp(base + out + 796, kLost, sizeof(kLost)) == 0)
            return;  // a sound copy of the screen
        memcpy(base + out + 796, kLost, sizeof(kLost));
        static std::atomic<bool> told{ false };
        if (!told.exchange(true))
            fprintf(stderr, "[game] a damaged front-end screen (UI_XENON_BLANK.fng, cut short in a translation's bundle) repaired\n");
    }
}

SB_HOOK(sub_823BD558)
{
    const uint32_t block = ctx.r3.u32;
    uint32_t out = ctx.r4.u32;
    if (PPC_LOAD_U32(block) != 0x5A4C444A || base[block + 4] != 2)  // "ZLDJ" as the game stores it, version 2
    {
        ctx.r3.u64 = 0;
        return;
    }
    const uint32_t size = PPC_LOAD_U32(block + 8), packed = PPC_LOAD_U32(block + 12);
    const uint32_t end = out + size;
    const uint32_t inEnd = block + packed;
    uint32_t in = block + 16;
    int32_t count = int32_t(packed) - 18;
    auto next = [&]() -> uint32_t { return in < inEnd ? base[in++] : (in++, 0u); };
    uint32_t flags1 = next() | 0x100, flags2 = next() | 0x100;
    while (count > 0 && in <= inEnd && out < end)
    {
        if (flags1 & 1)
        {
            const uint32_t b0 = in < inEnd ? base[in] : 0, b1 = in + 1 < inEnd ? base[in + 1] : 0;
            uint32_t distance, length;
            if (flags2 & 1)
            {
                distance = (b0 & 0x0F) + 1;
                length = (((b0 << 4) & ~0xFFu) | b1) + 3;
            }
            else
            {
                distance = (((b0 << 3) & ~0xFFu) | b1) + 17;
                length = (b0 & 0x1F) + 3;
            }
            for (uint32_t i = 0; i < length && out < end; i++, out++)
                base[out] = base[out - distance];  // (may overlap: byte by byte, as the game)
            in += 2;
            count -= 2;
            flags2 >>= 1;
        }
        else
        {
            base[out++] = uint8_t(next());
            count -= 1;
        }
        flags1 >>= 1;
        if (flags1 == 1)
        {
            flags1 = next() | 0x100;
            count -= 1;
        }
        if (flags2 == 1)
        {
            flags2 = next() | 0x100;
            count -= 1;
        }
    }
    RepairBlankScreen(ctx.r4.u32, size);
    ctx.r3.u64 = size;  // the game's returns the stated size
}
