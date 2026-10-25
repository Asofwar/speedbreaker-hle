#include "mp_hud.h"
#include <atomic>
#include <mutex>

namespace game
{
    namespace
    {
        std::mutex g_mutex;
        MpHud g_hud;
        bool g_have = false;
        std::atomic<bool> g_active{ false };
    }
    void MpHudPublish(MpHud&& hud)
    {
        std::lock_guard<std::mutex> g(g_mutex);
        g_have = !hud.lines.empty() || !hud.banner.empty() || !hud.markers.empty() || !hud.tags.empty() || !hud.chat.empty();
        g_hud = std::move(hud);
        g_active = g_have;
    }
    bool MpHudFetch(MpHud& out)
    {
        std::lock_guard<std::mutex> g(g_mutex);
        if (!g_have)
            return false;
        out = g_hud;
        return true;
    }
}
namespace game { bool MpHudActive() { return g_active.load(); } }
