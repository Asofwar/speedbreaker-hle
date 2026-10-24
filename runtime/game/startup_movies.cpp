// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Skipping the movies at launch (Settings > Advanced > Skip Startup Movies).
//
// The game plays eahd_bumper, ealogo and psa, then the title screen's attract_movie. Each is one "play movie" message to the
// front end's movie screen, handled in sub_82282F18 (r3 the screen, r4 the message: type 7 at msg+24, the movie's name at
// *(msg+36)). The handler builds the movie's file name and asks whether the file exists (sub_823BCA60). When it does not, it
// sends the "movie finished" event (0xC3960EB9, through sub_822FEB98, to the screens that wait for it) and returns 0, and the
// front end goes on to its next screen. This hook takes that same branch, with the game's own call, for the four startup
// movies while Skip Startup Movies is on: the trigger is answered as "no movie", so none is opened or drawn. No file is hidden and
// no button is pressed; the other movies (the tutorials, the career's story videos, the blacklist rivals) go through the original.
//
// (Checked by tracing the events of a run in which the four files were hidden: the game sent exactly this one event per movie from
// this branch, lr 0x82283080, and never reached the code that starts a movie. Tried before that and not working: starting the movie
// and failing its open inside the player; and pressing A, which skips the attract movie but not the three logos.)
#include <stdafx.h>

#include <kernel/function.h>
#include <user/settings.h>

#include <strings.h>

extern "C" PPC_FUNC(__imp__sub_82282F18);
extern "C" PPC_FUNC(__imp__sub_822FEB98);  // sends an event to the screens that listen

namespace
{
    constexpr uint32_t kGlobals = 0x82A30000;  // the globals base the game's code reaches (lis r, -0x7D5D)
    constexpr uint32_t kMessagePlayMovie = 7;
    constexpr uint32_t kMovieFinished = 0xC3960EB9;

    bool IsStartupMovie(const char* name)
    {
        static const char* const kNames[] = { "eahd_bumper", "ealogo", "psa", "attract_movie" };
        for (const char* n : kNames)
            if (strcasestr(name, n))
                return true;
        return false;
    }

    // The game's own event to the screens that wait for a movie: sub_822FEB98(listeners, kMovieFinished, 0, screen + 4, -1, 0).
    void SendMovieFinished(PPCContext& ctx, uint8_t* base, uint32_t screen)
    {
        ctx.r3.u64 = PPC_LOAD_U32(PPC_LOAD_U32(kGlobals - 14908));
        ctx.r4.u64 = kMovieFinished;
        ctx.r5.s64 = 0;
        ctx.r6.u64 = PPC_LOAD_U32(screen + 4);
        ctx.r7.s64 = -1;
        ctx.r8.s64 = 0;
        __imp__sub_822FEB98(ctx, base);
    }
}

PPC_FUNC(sub_82282F18)
{
    const uint32_t screen = ctx.r3.u32, message = ctx.r4.u32;
    if (PPC_LOAD_U32(message + 24) == kMessagePlayMovie && settings::GetBool(settings::Id::SkipStartupMovies))
    {
        char name[96];
        size_t n = 0;
        const uint32_t namePtr = PPC_LOAD_U32(message + 36);
        for (; n + 1 < sizeof(name); n++)
        {
            const char c = char(PPC_LOAD_U8(namePtr + uint32_t(n)));
            if (!c)
                break;
            name[n] = c;
        }
        name[n] = 0;
        if (IsStartupMovie(name))
        {
            fprintf(stderr, "[startup] the movie \"%s\" is skipped (Skip Startup Movies)\n", name);
            // As the original does before it looks the file up, then as it does for a file that is not there.
            if (PPC_LOAD_U32(kGlobals - 12772) != 0)
                SendMovieFinished(ctx, base, screen);
            SendMovieFinished(ctx, base, screen);
            ctx.r3.s64 = 0;
            return;
        }
    }
    ctx.r3.u64 = screen;
    ctx.r4.u64 = message;
    __imp__sub_82282F18(ctx, base);
}
