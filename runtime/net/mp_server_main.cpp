// mp_server [port] [races file]: the multiplayer dedicated server as a stand-alone program (no game files needed).
// What is typed into it goes to every player as the server's chat line; /players lists the players, /help the commands.
#include "mp_server.h"
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    uint16_t port = argc > 1 ? uint16_t(atoi(argv[1])) : mp::kDefaultPort;
    mp::Server s;
    if (!s.Start(port))
    {
        fprintf(stderr, "[mp-server] cannot listen on UDP %u\n", port);
        return 1;
    }
    if (argc > 2 && !s.LoadRaces(argv[2]))
        fprintf(stderr, "[mp-server] cannot read the races file %s\n", argv[2]);
    fprintf(stderr, "[mp-server] listening on UDP %u (type a line to send it to the players, /players, /help)\n", port);

    // The console: lines from stdin, handed to the loop below (the server is not thread-safe). A server run without one (stdin closed) just serves.
    std::mutex mutex;
    std::vector<std::string> lines;
    std::thread([&] {
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::lock_guard<std::mutex> g(mutex);
            lines.push_back(line);
        }
    }).detach();

    for (;;)
    {
        s.Service(5);
        std::vector<std::string> todo;
        {
            std::lock_guard<std::mutex> g(mutex);
            todo.swap(lines);
        }
        for (const std::string& line : todo)
        {
            if (line.empty())
                continue;
            if (line == "/players")
                fprintf(stderr, "[mp-server] %d players:\n%s", s.PlayerCount(), s.Players().c_str());
            else if (line == "/help")
                fprintf(stderr, "[mp-server] a line: chat to every player as \"server\"; /players: who is on; /help: this\n");
            else if (line[0] == '/')
                fprintf(stderr, "[mp-server] unknown command %s (/help)\n", line.c_str());
            else
                s.Say(line);
        }
    }
}
