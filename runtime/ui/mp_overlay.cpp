// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The open world race over the game's picture: a text panel, the countdown banner and markers (the start marker, the next checkpoints) projected from the game's camera.
// The other players' names over their cars and the chat's latest lines, in races too.
// The data is game/mp_hud.h's. The projection is the player's camera's own (game::PlayerProjection: the field of view, the screen's shape), the picture
// the UI's work area; before the game has built it, a vertical field of view of 55 degrees (NFSMW_MP_FOV, degrees, to change).
#include <stdafx.h>
#include "ui.h"

#include <game/mp_hud.h>

#include <imgui.h>

#include <cmath>

namespace ui
{
    namespace
    {
        // Project a world point (the body axes) with the game's camera: false when it is behind the camera.
        bool Project(const game::MpHud& h, const float p[3], float vfovDeg, ImVec2 origin, ImVec2 size, ImVec2& out, float& depth)
        {
            const float t[3] = { p[2], -p[0], p[1] };   // tracker axes: x = body z, y = -body x, z = body y (up)
            const float v[3] = { t[0] - h.eye[0], t[1] - h.eye[1], t[2] - h.eye[2] };
            float f[3] = { h.fwd[0], h.fwd[1], h.fwd[2] };
            const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
            if (fl < 1e-4f) return false;
            for (float& c : f) c /= fl;
            // right = forward x up(0,0,1); up' = right x forward
            float r[3] = { f[1], -f[0], 0.f };
            const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1]);
            if (rl < 1e-4f) return false;
            r[0] /= rl; r[1] /= rl;
            const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
            depth = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
            if (depth < 1.f) return false;
            const float xc = v[0] * r[0] + v[1] * r[1] + v[2] * r[2];
            const float yc = v[0] * u[0] + v[1] * u[1] + v[2] * u[2];
            const float ty = h.projY > 0 ? 1.f / h.projY : std::tan(vfovDeg * 3.14159265f / 360.f);
            const float tx = h.projX > 0 ? 1.f / h.projX : ty * size.x / size.y;
            out = ImVec2(origin.x + size.x * (0.5f + 0.5f * xc / (depth * tx)), origin.y + size.y * (0.5f - 0.5f * yc / (depth * ty)));
            return true;
        }
    }

    void DrawMpOverlay()
    {
        game::MpHud h;
        if (!game::MpHudFetch(h))
            return;
        static const float vfov = [] { const char* e = std::getenv("NFSMW_MP_FOV"); return e ? float(atof(e)) : 55.f; }();
        const float s = Scale();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 work = ImGui::GetMainViewport()->WorkPos, size = ImGui::GetMainViewport()->WorkSize;

        // Markers: a ring on the ground and a beam up, with the label and the distance.
        if (h.camValid)
            for (const game::MpHudMarker& m : h.markers)
            {
                ImVec2 base2, top2;
                float d0 = 0, d1 = 0;
                float top[3] = { m.p[0], m.p[1] + 14.f, m.p[2] };
                const bool a = Project(h, m.p, vfov, work, size, base2, d0), b = Project(h, top, vfov, work, size, top2, d1);
                const ImU32 col = m.kind == 0 ? IM_COL32(255, 200, 40, 235) : m.kind == 1 ? IM_COL32(60, 220, 255, 235) : IM_COL32(120, 170, 190, 170);
                if (!a || !b)
                    continue;
                const float rad = std::clamp(2200.f / d0, 5.f, 60.f) * s;
                dl->AddLine(base2, top2, col, std::max(2.f, 3.f * s));
                dl->AddCircle(top2, rad, col, 24, std::max(2.f, 3.f * s));
                dl->AddCircleFilled(top2, rad * 0.25f, col);
                char buf[96];
                snprintf(buf, sizeof buf, "%s  %.0f m", m.label.c_str(), double(d0));
                const ImVec2 ts = ImGui::CalcTextSize(buf);
                dl->AddText(ImVec2(top2.x - ts.x * 0.5f, top2.y - rad - ts.y - 2.f), col, buf);
            }

        // The other players' names over their cars, fading with distance (none past 400 m).
        if (h.camValid)
            for (const game::MpHudTag& t : h.tags)
            {
                ImVec2 at;
                float d = 0;
                if (!Project(h, t.p, vfov, work, size, at, d) || d > 400.f)
                    continue;
                const int alpha = int(255.f * std::clamp(1.2f - d / 400.f, 0.25f, 1.f));
                const ImVec2 ts = ImGui::CalcTextSize(t.name.c_str());
                const ImVec2 p0(at.x - ts.x * 0.5f - 5.f, at.y - ts.y - 4.f), p1(at.x + ts.x * 0.5f + 5.f, at.y);
                dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, alpha * 140 / 255), 4.f);
                dl->AddText(ImVec2(p0.x + 5.f, p0.y + 2.f), IM_COL32(255, 255, 255, alpha), t.name.c_str());
            }

        // The chat, bottom left.
        {
            float cy = work.y + size.y * 0.6f;  // (above the minimap)
            for (auto it = h.chat.rbegin(); it != h.chat.rend(); ++it)
            {
                const ImVec2 ts = ImGui::CalcTextSize(it->c_str());
                cy -= ts.y + 6.f;
                dl->AddRectFilled(ImVec2(work.x + 10.f * s, cy), ImVec2(work.x + 10.f * s + ts.x + 12.f, cy + ts.y + 4.f), IM_COL32(0, 0, 0, 150), 4.f);
                dl->AddText(ImVec2(work.x + 16.f * s, cy + 2.f), IM_COL32(200, 235, 255, 245), it->c_str());
            }
        }

        // The panel, top left of the picture.
        float y = work.y + 90.f * s;
        for (const std::string& l : h.lines)
        {
            const ImVec2 ts = ImGui::CalcTextSize(l.c_str());
            dl->AddRectFilled(ImVec2(work.x + 10.f * s, y), ImVec2(work.x + 10.f * s + ts.x + 12.f, y + ts.y + 6.f), IM_COL32(0, 0, 0, 150), 4.f);
            dl->AddText(ImVec2(work.x + 16.f * s, y + 3.f), IM_COL32(255, 255, 255, 240), l.c_str());
            y += ts.y + 8.f;
        }
        if (!h.banner.empty())
        {
            ImFont* font = ImGui::GetFont();
            const float fs = ImGui::GetFontSize() * 3.0f;
            const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, 0.f, h.banner.c_str());
            dl->AddText(font, fs, ImVec2(work.x + size.x * 0.5f - ts.x * 0.5f + 2.f, work.y + 30.f * s + 2.f), IM_COL32(0, 0, 0, 200), h.banner.c_str());
            dl->AddText(font, fs, ImVec2(work.x + size.x * 0.5f - ts.x * 0.5f, work.y + 30.f * s), IM_COL32(255, 220, 60, 255), h.banner.c_str());
        }
    }
}
