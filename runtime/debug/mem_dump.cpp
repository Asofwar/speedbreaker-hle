// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// NFSMW_MEMDUMP=<folder>,<s>,<s>,... (reverse engineering): at each of those seconds
// after launch, the image's data (guest 0x82000000 to 0x82CD0000) written to
// <folder>/mem-<n>.bin, for diffing two states of the game (a flag that holds while
// the pause menu is up, say). Plain reads racing the game's writes.
#include <stdafx.h>
#include <kernel/memory.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t kFrom = 0x82000000, kTo = 0x82CD0000;

    struct MemDump
    {
        MemDump()
        {
            const char* v = std::getenv("NFSMW_MEMDUMP");
            if (!v || !*v)
                return;
            std::string spec(v), folder;
            std::vector<double> times;
            size_t pos = 0;
            for (int field = 0; pos <= spec.size(); field++)
            {
                size_t end = spec.find(',', pos);
                if (end == std::string::npos)
                    end = spec.size();
                const std::string item = spec.substr(pos, end - pos);
                pos = end + 1;
                if (field == 0)
                    folder = item;
                else if (!item.empty())
                    times.push_back(std::atof(item.c_str()));
            }
            const auto launch = std::chrono::steady_clock::now();
            std::thread([folder, times, launch] {
                for (size_t i = 0; i < times.size(); i++)
                {
                    std::this_thread::sleep_until(launch + std::chrono::duration<double>(times[i]));
                    if (!g_memory.base)
                        continue;
                    const std::string path = folder + "/mem-" + std::to_string(i) + ".bin";
                    if (FILE* f = fopen(path.c_str(), "wb"))
                    {
                        fwrite(g_memory.base + kFrom, 1, kTo - kFrom, f);
                        fclose(f);
                        fprintf(stderr, "[memdump] %.1f s: %s\n", times[i], path.c_str());
                    }
                }
            }).detach();
        }
    };
    MemDump s_memDump;
}
