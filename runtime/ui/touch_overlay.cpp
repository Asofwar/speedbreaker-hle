// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The touch controls over the game (hid/touch.h): each control a circle with its
// label or arrow, brighter while a finger holds it; the stick's ring and knob where
// the finger took it; the tilt's steering as a bar along the bottom. Settings >
// Touch > Button Opacity sets how much they cover the game.
#include <stdafx.h>
#include "ui.h"
#include <hid/touch.h>
#include <imgui.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace ui
{
    namespace
    {
        using hid::touchlayout::Glyph;
        using hid::touchlayout::Kind;

        ImU32 Colour(int r, int g, int b, float alpha)
        {
            return IM_COL32(r, g, b, int(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
        }

        void DrawArrow(ImDrawList* dl, ImVec2 c, float size, Glyph glyph, ImU32 col)
        {
            const float s = size;
            ImVec2 a, b, d;
            switch (glyph)
            {
            case Glyph::Left: a = ImVec2(c.x - s, c.y); b = ImVec2(c.x + s * 0.6f, c.y - s * 0.85f); d = ImVec2(c.x + s * 0.6f, c.y + s * 0.85f); break;
            case Glyph::Right: a = ImVec2(c.x + s, c.y); b = ImVec2(c.x - s * 0.6f, c.y + s * 0.85f); d = ImVec2(c.x - s * 0.6f, c.y - s * 0.85f); break;
            case Glyph::Up: a = ImVec2(c.x, c.y - s); b = ImVec2(c.x + s * 0.85f, c.y + s * 0.6f); d = ImVec2(c.x - s * 0.85f, c.y + s * 0.6f); break;
            case Glyph::Down: a = ImVec2(c.x, c.y + s); b = ImVec2(c.x - s * 0.85f, c.y - s * 0.6f); d = ImVec2(c.x + s * 0.85f, c.y - s * 0.6f); break;
            default: return;
            }
            dl->AddTriangleFilled(a, b, d, col);
        }
    }

    void DrawTouchControls()
    {
        hid::touch::View v;
        if (!hid::touch::GetView(v) || v.width <= 0 || v.height <= 0)
            return;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float sx = display.x / v.width, sy = display.y / v.height, s = std::min(sx, sy);
        const float alpha = v.opacity;
        ImFont* font = ImGui::GetFont();
        for (size_t i = 0; i < v.layout.widgets.size(); i++)
        {
            const auto& w = v.layout.widgets[i];
            const bool pressed = v.pressed[i];
            const ImVec2 c(w.x * sx, w.y * sy);
            const float r = w.r * s;
            if (w.kind == Kind::Stick)
            {
                // The stick: its ring where it rests, or where the finger took it, and the knob.
                const ImVec2 origin = v.stickHeld ? ImVec2(v.stickOriginX * sx, v.stickOriginY * sy) : c;
                const float travel = v.layout.stickTravel * s;
                dl->AddCircleFilled(origin, travel, Colour(0, 0, 0, alpha * 0.45f), 48);
                dl->AddCircle(origin, travel, Colour(255, 255, 255, alpha * 0.8f), 48, std::max(1.5f, 2.5f * s));
                ImVec2 knob = origin;
                if (v.stickHeld)
                {
                    float dx = (v.stickX - v.stickOriginX) * sx, dy = (v.stickY - v.stickOriginY) * sy;
                    const float length = std::hypot(dx, dy);
                    if (length > travel)
                    {
                        dx *= travel / length;
                        dy *= travel / length;
                    }
                    knob = ImVec2(origin.x + dx, origin.y + dy);
                }
                dl->AddCircleFilled(knob, travel * 0.45f, Colour(255, 255, 255, alpha * (v.stickHeld ? 0.9f : 0.55f)), 32);
                continue;
            }
            const bool pedal = w.kind == Kind::Gas || w.kind == Kind::Brake;
            const ImU32 fill = pressed ? Colour(255, 190, 60, alpha * 0.85f)
                : pedal && w.kind == Kind::Gas ? Colour(30, 90, 40, alpha * 0.6f)
                : pedal ? Colour(100, 30, 30, alpha * 0.6f) : Colour(0, 0, 0, alpha * 0.5f);
            dl->AddCircleFilled(c, r, fill, 48);
            dl->AddCircle(c, r, Colour(255, 255, 255, alpha * (pressed ? 1.0f : 0.75f)), 48, std::max(1.5f, 2.5f * s));
            const ImU32 ink = Colour(255, 255, 255, std::min(1.0f, alpha * 1.6f));
            if (w.glyph != Glyph::Text)
            {
                DrawArrow(dl, c, r * 0.45f, w.glyph, ink);
                continue;
            }
            // The label as large as fits across the circle.
            const size_t letters = std::max<size_t>(1, std::strlen(w.label));
            const float fontSize = std::min(r * 0.62f, r * 1.7f / (0.6f * float(letters)));
            const ImVec2 size = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, w.label);
            dl->AddText(font, fontSize, ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), ink, w.label);
        }
        if (v.tilt)
        {
            // The tilt's steering: a bar along the bottom, filled from the middle.
            const float u = v.layout.unit * s, half = display.x * 0.18f, y = display.y - 3.0f * u;
            const float mid = display.x * 0.5f;
            dl->AddRectFilled(ImVec2(mid - half, y - 0.8f * u), ImVec2(mid + half, y + 0.8f * u), Colour(0, 0, 0, alpha * 0.5f), 0.8f * u);
            const float to = mid + half * std::clamp(v.steering, -1.0f, 1.0f);
            dl->AddRectFilled(ImVec2(std::min(mid, to), y - 0.8f * u), ImVec2(std::max(mid, to), y + 0.8f * u), Colour(255, 190, 60, alpha * 0.9f), 0.8f * u);
            dl->AddLine(ImVec2(mid, y - 1.4f * u), ImVec2(mid, y + 1.4f * u), Colour(255, 255, 255, alpha), std::max(1.0f, 2.0f * s));
        }
    }
}
