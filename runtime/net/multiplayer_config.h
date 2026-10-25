// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The multiplayer layer's settings (Settings > Online > Multiplayer, saved in the user data folder as multiplayer.txt): whether it is on, the server, the player's
// name and how many players the start barrier waits for. The environment variables NFSMW_MP_SERVER, NFSMW_MP_NAME and NFSMW_MP_PLAYERS override the file (and
// NFSMW_MP_SERVER alone turns it on), for scripts and tests. Read by game/mp_adapter.cpp whenever a race appears, so a change applies to the next race.
#pragma once
#include <string>

namespace mpcfg
{
    struct Config
    {
        bool enabled = false;
        std::string server = "127.0.0.1:27960";
        std::string name = "Player";
        int players = 2;   // start barrier: wait for this many players when this player is not ready in the lobby (1 or 0: don't wait)
        bool world = false;   // open world mode: the game starts in the open world (no menus), the others are cars in it and races are the server's (applies from the next start of the game)
        std::string car;   // the car chosen in the lobby: the game's name for it (IS300, RX8, ...); empty: none yet
    };

    Config Load();                  // the file, then the environment
    void Save(const Config& c);     // the file only
}
