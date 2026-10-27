// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See stream_hold.h.
//
// A timedemo's audio streams: the frame in which the game sees a stream's
// first data.
//
// The game's audio streams (speech, music, a race's intro track) are
// channels of EA's stream player, and the game's stream class asks for a
// channel's status through sub_821C8AD8 (r3 the stream, its channel handle at
// +20; r4 three words out: the channel's state, ..., and how much of it is
// in, in ms; the player's sub_825D1FA0 under the sound lock). A stream's
// data comes from the player's reads, on its own threads, as fast as the
// file delivers it, and nothing of that is in the Joylog: the frame in which
// the main thread first sees a stream it started with data was the reads'
// timing. One wait on it matters. A race's start (sub_8248D468, from the
// game flow's update) waits for its intro stream to have data (position
// above 0), then calls back to start the race (sub_82206910), and the cars
// move two frames later. In the default race the stream starts at loop 4412
// and had its data at 4414 in the capture (on a Steam Deck, the read lands
// within the 120 ms the frame after the intro movie takes); a replay whose
// read landed a frame earlier or later started the race a frame earlier or
// later: "DIVERGENCE kind=checksum loop=4415" (the cars already moving) or
// "loop=4416" (not yet), 1% of the replays of the usual build on a Steam
// Deck, a sixth of ThinLTO and PGO builds', about half of them headless.
//
// In a timedemo, capture and replay alike, the main thread sees a stream it
// has seen started (state nonzero) with no data for exactly kHeldFrames
// frames (stream_hold.h): until then a position is reported as 0 even if data
// is in, and from then on the main thread waits for the data if it isn't
// (kWaitMax at most, after which the status goes to the game as it is; a
// hold, too, lets go after kWaitMax, should one frame ask again and again
// without ending). The wait is between two calls of the wrapper, so the sound
// lock isn't held meanwhile. The player's own threads and its own update see the channel as
// it is. Each stream's first data leaves a [timedemo] stream line. Outside a
// timedemo nothing changes.
#include <stdafx.h>
#include <region/region.h>
#include "stream_hold.h"
#include "timedemo.h"

#include <kernel/function.h>

extern "C" PPC_FUNC(__imp__sub_821C8AD8);  // the game's stream class: a channel's status, under the sound lock

namespace
{
    constexpr uint32_t kChannels = 64;
    constexpr auto kWaitMax = std::chrono::seconds(3);
    constexpr auto kWaitStep = std::chrono::microseconds(500);

    game::streams::Channel s_channels[kChannels];  // the main thread's only
    // When each held channel's current frame first asked (host time), and
    // that frame's loop: a hold that one frame asks about for kWaitMax (a
    // frame that never ends, asking again and again) lets go. The count
    // starts again with each frame, so a slow frame inside the hold (a load,
    // a pipeline built on the spot, a debugger) doesn't end it a frame early.
    std::chrono::steady_clock::time_point s_heldSince[kChannels];
    uint32_t s_heldLoop[kChannels];
}

SB_HOOK(sub_821C8AD8)
{
    using game::streams::Action;
    const PPCRegister stream = ctx.r3, out = ctx.r4;
    SB_ORIGINAL(sub_821C8AD8)(ctx, base);
    if (!timedemo::Active() || !timedemo::OnMainThread() || ctx.r3.s32 != 0)
        return;
    const uint32_t channel = PPC_LOAD_U32(stream.u32 + 20);
    if (channel >= kChannels)
        return;
    auto& c = s_channels[channel];
    const uint32_t loop = timedemo::RealLoop();
    const bool held = c.phase == game::streams::Channel::Phase::Held;
    auto next = [&] { return game::streams::Next(c, loop, PPC_LOAD_U32(out.u32), PPC_LOAD_U32(out.u32 + 8)); };
    Action action = next();
    const auto start = std::chrono::steady_clock::now();
    if (c.phase == game::streams::Channel::Phase::Held && (!held || loop != s_heldLoop[channel]))
    {
        s_heldSince[channel] = start;
        s_heldLoop[channel] = loop;
    }
    if (action == Action::Hide)
    {
        if (start - s_heldSince[channel] > kWaitMax)
        {
            game::streams::GiveUp(c);
            fprintf(stderr, "[timedemo] stream %u: started at loop %u, still held at loop %u after %lld ms: as it is from here\n",
                channel, c.startLoop, loop, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(kWaitMax).count());
            return;
        }
        PPC_STORE_U32(out.u32 + 8, 0);
        return;
    }
    while (action == Action::Wait)
    {
        if (std::chrono::steady_clock::now() - start > kWaitMax)
        {
            game::streams::GiveUp(c);
            fprintf(stderr, "[timedemo] stream %u: started at loop %u, still no data at loop %u after %lld ms: as it is from here\n",
                channel, c.startLoop, loop, (long long)std::chrono::duration_cast<std::chrono::milliseconds>(kWaitMax).count());
            return;
        }
        std::this_thread::sleep_for(kWaitStep);
        ctx.r3 = stream;
        ctx.r4 = out;
        SB_ORIGINAL(sub_821C8AD8)(ctx, base);
        if (ctx.r3.s32 != 0)
            return;
        action = next();
    }
    if (held && c.phase == game::streams::Channel::Phase::Flowing)
    {
        double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        fprintf(stderr, "[timedemo] stream %u: started at loop %u, its data at loop %u (%u ms in; waited %.1f ms)\n", channel,
            c.startLoop, loop, PPC_LOAD_U32(out.u32 + 8), waited);
    }
}
