// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Cheats: writes to the game's memory, from cheats.txt in the game's data
// folder (user/paths.h: GetUserPath), each switched on or off on Settings >
// Game. The file's format is NFSMW Recompiled's (nfsmw_trucos.txt), so its
// cheats work here as they are:
//
//   [Name]
//   note = What it does
//   u32 0x82A01234 1000000
//   f32 *0x82A05678+0x1C 2.5     (a pointer: the word there, plus 0x1C)
//
// Types u8, u16, u32 and f32; addresses are the Xbox 360's (as Xenia's
// memory search shows them). While a cheat is on its writes are made again
// every 100 ms. Which are on is kept in cheats_on.txt (one name a line). A
// timedemo (game/timedemo.h) runs none: its replays must stay in step.
#pragma once

#include <string>
#include <vector>

namespace game::cheats
{
    struct Cheat
    {
        std::string name, note;
        bool on = false;
    };

    // Main thread, before the game runs: reads the files (writing a cheats.txt
    // that explains the format if there is none) and starts the writes.
    void Start();
    // The cheats as the file has them, in its order. Any thread.
    std::vector<Cheat> List();
    // Switches one on or off (and saves which are on). Any thread.
    void Set(size_t index, bool on);
    // Reads cheats.txt again (after an edit). Any thread.
    void Reload();
    // The file's path, for the menu.
    std::string FilePath();
}
