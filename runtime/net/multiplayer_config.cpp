// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See multiplayer_config.h.
#include <stdafx.h>
#include "multiplayer_config.h"

#include <user/paths.h>

#include <filesystem>
#include <fstream>

namespace mpcfg
{
    namespace
    {
        std::filesystem::path FilePath() { return GetUserPath() / "multiplayer.txt"; }
    }

    Config Load()
    {
        Config c;
        std::ifstream in(FilePath());
        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "enabled") c.enabled = v == "1" || v == "true";
            else if (k == "server" && !v.empty()) c.server = v;
            else if (k == "name" && !v.empty()) c.name = v;
            else if (k == "players") c.players = std::clamp(std::atoi(v.c_str()), 0, 16);
            else if (k == "car") c.car = v;
            else if (k == "world") c.world = v == "1" || v == "true";
        }
        if (const char* e = std::getenv("NFSMW_MP_SERVER")) { c.server = e; c.enabled = true; }
        if (const char* e = std::getenv("NFSMW_MP_NAME")) c.name = e;
        if (const char* e = std::getenv("NFSMW_MP_PLAYERS")) c.players = std::clamp(std::atoi(e), 0, 16);
        if (std::getenv("NFSMW_MP_WORLD")) c.world = true;
        return c;
    }

    void Save(const Config& c)
    {
        std::error_code ec;
        std::filesystem::create_directories(FilePath().parent_path(), ec);
        std::ofstream out(FilePath(), std::ios::trunc);
        out << "enabled=" << (c.enabled ? 1 : 0) << "\nserver=" << c.server << "\nname=" << c.name << "\nplayers=" << c.players << "\ncar=" << c.car << "\nworld=" << (c.world ? 1 : 0) << "\n";
    }
}
