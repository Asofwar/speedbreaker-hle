// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See cheats.h.
#include <stdafx.h>
#include "cheats.h"
#include "timedemo.h"
#include <kernel/memory.h>
#include <user/paths.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace
{
    constexpr const char* kTemplate =
        "# SpeedBreaker cheats: switched on and off on Settings > Game.\n"
        "# They write to the game's memory: back up your save first.\n"
        "#\n"
        "# One block a cheat:\n"
        "#\n"
        "#   [Name]\n"
        "#   note = What it does\n"
        "#   u32 0x82A01234 1000000\n"
        "#   f32 *0x82A05678+0x1C 2.5     (a pointer: the word at 0x82A05678, plus 0x1C)\n"
        "#\n"
        "# Types: u8, u16, u32, f32. Addresses are the Xbox 360's (as Xenia's\n"
        "# memory search shows them). NFSMW Recompiled's nfsmw_trucos.txt has the\n"
        "# same format. While a cheat is on its writes are made again every 100 ms.\n";

    struct Write
    {
        enum class Type { U8, U16, U32, F32 } type = Type::U32;
        uint32_t address = 0, offset = 0, value = 0;
        bool pointer = false;
    };

    struct Entry
    {
        game::cheats::Cheat cheat;
        std::vector<Write> writes;
    };

    std::mutex s_mutex;
    std::vector<Entry> s_entries;
    std::atomic<bool> s_started{ false };

    std::filesystem::path CheatsPath() { return GetUserPath() / "cheats.txt"; }
    std::filesystem::path OnPath() { return GetUserPath() / "cheats_on.txt"; }

    std::string Trim(const std::string& s)
    {
        const size_t a = s.find_first_not_of(" \t\r");
        if (a == std::string::npos)
            return {};
        return s.substr(a, s.find_last_not_of(" \t\r") - a + 1);
    }

    bool Number(const std::string& s, uint32_t& out)
    {
        if (s.empty())
            return false;
        char* end = nullptr;
        const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
        if (*end || v > 0xFFFFFFFFull)
            return false;
        out = uint32_t(v);
        return true;
    }

    bool ParseWrite(const std::string& line, Write& w)
    {
        std::istringstream in(line);
        std::string type, where, value;
        if (!(in >> type >> where >> value))
            return false;
        if (type == "u8")
            w.type = Write::Type::U8;
        else if (type == "u16")
            w.type = Write::Type::U16;
        else if (type == "u32")
            w.type = Write::Type::U32;
        else if (type == "f32")
            w.type = Write::Type::F32;
        else
            return false;
        if (where[0] == '*')
        {
            w.pointer = true;
            where = where.substr(1);
            if (const size_t plus = where.find('+'); plus != std::string::npos)
            {
                if (!Number(where.substr(plus + 1), w.offset))
                    return false;
                where = where.substr(0, plus);
            }
        }
        if (!Number(where, w.address))
            return false;
        if (w.type == Write::Type::F32)
        {
            char* end = nullptr;
            const float f = std::strtof(value.c_str(), &end);
            if (*end)
                return false;
            memcpy(&w.value, &f, 4);
            return true;
        }
        return Number(value, w.value);
    }

    std::set<std::string> ReadOn()
    {
        std::set<std::string> on;
        std::ifstream f(OnPath());
        for (std::string line; std::getline(f, line);)
        {
            if (on.empty() && line.rfind("\xEF\xBB\xBF", 0) == 0)
                line.erase(0, 3);
            if (std::string name = Trim(line); !name.empty())
                on.insert(name);
        }
        return on;
    }

    void SaveOnLocked()
    {
        std::ofstream f(OnPath(), std::ios::trunc);
        for (const Entry& e : s_entries)
            if (e.cheat.on)
                f << e.cheat.name << '\n';
    }

    void LoadLocked()
    {
        s_entries.clear();
        const std::filesystem::path path = CheatsPath();
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
        {
            std::ofstream(path) << kTemplate;
            return;
        }
        const std::set<std::string> on = ReadOn();
        std::ifstream f(path);
        int lineNumber = 0;
        for (std::string raw; std::getline(f, raw);)
        {
            lineNumber++;
            if (lineNumber == 1 && raw.rfind("\xEF\xBB\xBF", 0) == 0)
                raw.erase(0, 3);  // (a UTF-8 byte order mark, as Notepad saves one)
            const std::string line = Trim(raw);
            if (line.empty() || line[0] == '#' || line[0] == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                Entry& e = s_entries.emplace_back();
                e.cheat.name = Trim(line.substr(1, line.size() - 2));
                e.cheat.on = on.count(e.cheat.name) != 0;
                continue;
            }
            if (s_entries.empty())
            {
                fprintf(stderr, "[cheats] cheats.txt line %d is before any [name]: ignored\n", lineNumber);
                continue;
            }
            if (line.rfind("note", 0) == 0 && line.find('=') != std::string::npos)
            {
                s_entries.back().cheat.note = Trim(line.substr(line.find('=') + 1));
                continue;
            }
            Write w;
            if (ParseWrite(line, w))
                s_entries.back().writes.push_back(w);
            else
                fprintf(stderr, "[cheats] cheats.txt line %d isn't a write (\"u32 0x82A01234 5\"): ignored\n", lineNumber);
        }
        fprintf(stderr, "[cheats] %zu in %s\n", s_entries.size(), path.string().c_str());
    }

    // Where the game keeps data: its heap, its image, physical memory.
    bool Plausible(uint32_t address, uint32_t size)
    {
        return (address >= 0x00010000u && address + size <= 0x7F000000u) || (address >= 0x82000000u && address + size <= 0x83000000u) ||
            (address >= 0xA0000000u && address + size <= 0xC0000000u);
    }

    void Apply(const Write& w)
    {
        uint8_t* base = g_memory.base;
        uint32_t address = w.address;
        if (w.pointer)
        {
            if (!Plausible(address, 4) || (address & 3))
                return;
            address = ByteSwap(*reinterpret_cast<const uint32_t*>(base + address)) + w.offset;
        }
        const uint32_t size = w.type == Write::Type::U8 ? 1 : w.type == Write::Type::U16 ? 2 : 4;
        if (!Plausible(address, size) || (address & (size - 1)))
            return;
        switch (w.type)
        {
        case Write::Type::U8:
            base[address] = uint8_t(w.value);
            break;
        case Write::Type::U16:
            *reinterpret_cast<uint16_t*>(base + address) = ByteSwap(uint16_t(w.value));
            break;
        default:
            *reinterpret_cast<uint32_t*>(base + address) = ByteSwap(w.value);
            break;
        }
    }

    void Writer()
    {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard lock(s_mutex);
            for (const Entry& e : s_entries)
                if (e.cheat.on)
                    for (const Write& w : e.writes)
                        Apply(w);
        }
    }
}

namespace game::cheats
{
    void Start()
    {
        if (timedemo::Active())
        {
            fprintf(stderr, "[cheats] none in a timedemo\n");
            return;
        }
        {
            std::lock_guard lock(s_mutex);
            LoadLocked();
        }
        if (!s_started.exchange(true))
            std::thread(Writer).detach();
    }

    std::vector<Cheat> List()
    {
        std::lock_guard lock(s_mutex);
        std::vector<Cheat> out;
        for (const Entry& e : s_entries)
            out.push_back(e.cheat);
        return out;
    }

    void Set(size_t index, bool on)
    {
        std::lock_guard lock(s_mutex);
        if (index >= s_entries.size() || s_entries[index].cheat.on == on)
            return;
        s_entries[index].cheat.on = on;
        fprintf(stderr, "[cheats] %s: %s\n", s_entries[index].cheat.name.c_str(), on ? "on" : "off");
        SaveOnLocked();
    }

    void Reload()
    {
        if (!s_started.load())
            return;
        std::lock_guard lock(s_mutex);
        LoadLocked();
    }

    std::string FilePath()
    {
        return CheatsPath().string();
    }
}
