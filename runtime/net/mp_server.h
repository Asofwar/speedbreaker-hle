// The multiplayer dedicated server (net/mp_server.cpp): sessions, the player table and the 30 Hz snapshot of every
// car. It does not run the game; each client simulates its own car and the server relays and orders the states.
#pragma once
#include "mp_proto.h"
#include <memory>

namespace mp
{
    class Server
    {
    public:
        Server();
        ~Server();
        bool Start(uint16_t port, int maxPlayers = kMaxPlayers);
        // Handle traffic for up to timeoutMs, then send a snapshot if a tick is due. Call in a loop.
        void Service(int timeoutMs);
        // The races of the open world (net/mp_race.h): read from a file (see LoadRaces there). Call before or while running; clients get the list when they join.
        bool LoadRaces(const std::string& path);
        int RaceCount() const;
        int PlayerCount() const;
        // The server's own chat line, to every player (shown as "server"). Call from the thread that runs Service.
        void Say(const std::string& text);
        // One line per player: id, name, the car it chose, ready or not.
        std::string Players() const;
        uint16_t Port() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> m;
    };
}
