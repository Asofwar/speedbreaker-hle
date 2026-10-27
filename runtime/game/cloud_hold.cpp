// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// A timedemo replay's clouds: held still, the same in every run.
//
// The sky's clouds drift by a scroll offset (the sky's vertex shader
// constant c13.x, kept at 0x82C5E718) that the sky's effect setup
// (sub_824507C0, when it draws) moves on whenever it finds the main thread's
// tick counter ([0x828FD888]) changed since it last looked ([0x82C5E71C]),
// by the wind's speed times the main thread's frame time ([0x82A2CD28]),
// which the main thread sets and clears within its frame. Both are the main
// thread's live values, read from the drawing side whenever it gets there:
// how often the offset moved, and by how much, was the two threads' timing.
// So two replays of one race drew their skies with the clouds a little apart
// (between uncapped runs of builds of different speeds, up to 24 levels over
// a fifth of a picture; the shader constant was the only one that differed).
//
// In a replay the effect setup finds the counter unchanged every time, so the
// offset stays where the replay's start left it; should the main thread move
// the counter between that and the setup's own look, the setup runs again
// with the offset put back. The clouds then hold still in a replay, in every
// run. NFSMW_TIMEDEMO_CLOUD_HOLD=0 turns it off. Capture and normal play are
// untouched: the sky is drawing only.
#include <stdafx.h>
#include <region/region.h>
#include "timedemo.h"

#include <kernel/function.h>

extern "C" PPC_FUNC(__imp__sub_824507C0);  // the sky's effect setup

namespace
{
    constexpr uint32_t kCloudOffset = 0x82C5E718;  // c13.x
    constexpr uint32_t kCloudSeen = 0x82C5E71C;    // the tick counter when the offset last moved
    constexpr uint32_t kTickCounter = 0x828FD888;  // the main thread's

    const bool s_enabled = [] {
        const char* v = std::getenv("NFSMW_TIMEDEMO_CLOUD_HOLD");
        return !(v && v[0] == '0');
    }();
}

SB_HOOK(sub_824507C0)
{
    if (!s_enabled || !timedemo::Replaying())
    {
        SB_ORIGINAL(sub_824507C0)(ctx, base);
        return;
    }
    static std::atomic<bool> s_said{ false };
    if (!s_said.exchange(true, std::memory_order_relaxed))
        fprintf(stderr, "[timedemo] clouds: held still for the replay (NFSMW_TIMEDEMO_CLOUD_HOLD=0: they drift as timing has it)\n");
    const uint32_t offset = PPC_LOAD_U32(kCloudOffset);
    const PPCContext saved = ctx;
    for (int attempt = 0; attempt < 4; attempt++)
    {
        if (attempt)
        {
            PPC_STORE_U32(kCloudOffset, offset);
            ctx = saved;
        }
        PPC_STORE_U32(kCloudSeen, PPC_LOAD_U32(kTickCounter));
        SB_ORIGINAL(sub_824507C0)(ctx, base);
        if (PPC_LOAD_U32(kCloudOffset) == offset)
            return;
    }
    PPC_STORE_U32(kCloudOffset, offset);
}
