// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See ui.h (TakePhoto).
//
// A photo: the next frame as the game's final grade leaves it, before its
// HUD, menus and pause screen are drawn over it (gpu/renderer.h,
// ReadBackScenePhoto), at the internal resolution, as a PNG in the data
// folder's photos/. Pause the game (its own pause menu stays out of the
// picture) and press F12, or Settings > Game > Take Photo.
#include <stdafx.h>
#include "ui.h"
#include <gpu/renderer.h>
#include <report/zip.h>
#include <user/paths.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <fstream>

namespace
{
    std::atomic<bool> s_busy{ false };

    std::filesystem::path PhotoPath()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &local);
        const std::filesystem::path dir = GetUserPath() / "photos";
        std::filesystem::path path = dir / std::format("SpeedBreaker_{}.png", stamp);
        for (int n = 2; std::filesystem::exists(path); n++)
            path = dir / std::format("SpeedBreaker_{}_{}.png", stamp, n);
        return path;
    }
}

namespace ui
{
    void TakePhoto()
    {
        if (s_busy.exchange(true))
            return;
        gpu::renderer::ReadBackScenePhoto([](const uint8_t* rgba, uint32_t width, uint32_t height) {
            if (!rgba)
            {
                fprintf(stderr, "[photo] none: no frame with the game's final grade\n");
                Toast("No photo: this frame has no scene (a menu or a movie). Pause in a race or in free roam.", 6.0);
                s_busy = false;
                return;
            }
            std::vector<uint8_t> rgb(size_t(width) * height * 3);
            for (size_t i = 0; i < size_t(width) * height; i++)
                memcpy(&rgb[i * 3], &rgba[i * 4], 3);
            const std::vector<uint8_t> png = report::EncodePng(rgb.data(), width, height);
            const std::filesystem::path path = PhotoPath();
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream f(path, std::ios::binary);
            f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
            if (f.good())
            {
                fprintf(stderr, "[photo] %ux%u: %s\n", width, height, path.string().c_str());
                Toast(std::format("Photo saved ({}x{}): {}", width, height, path.string()), 6.0);
            }
            else
            {
                fprintf(stderr, "[photo] can't write %s\n", path.string().c_str());
                Toast("No photo: can't write " + path.string(), 6.0);
            }
            s_busy = false;
        });
    }
}
