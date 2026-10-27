// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// The touch controls' geometry and rules (hid/touch.h drives them, ui/touch_overlay.cpp
// draws them): where each on-screen control sits for a window's size, which a finger
// is on, how the steering buttons ease the stick, how a tilt becomes steering, and
// what a finger's path in the menus means. Header only and SDL-free;
// tests/touch_layout_test.cpp checks the rules.
//
// Two layouts. Driving (the game's world is running): steering on the left (two
// buttons, a stick or nothing for the tilt), the pedals and the game's driving
// buttons on the right, as the game's own Settings > Controls has them: A the
// handbrake, B nitrous, X Speedbreaker, Y looking back, RB the camera, Back the
// reset after a crash, Start the pause. Menus (the front end, the pause menu, the
// map): a D-pad, A B X Y, LB RB LT RT, Start and Back, and the gestures (a tap is A,
// a two-finger tap B, a swipe the D-pad).
//
// Coordinates are the window's points, origin top left. Sizes follow a unit u, a
// hundredth of the window's height, or less on a narrow window, so both columns of
// controls fit side by side.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace hid::touchlayout
{
    // XInput's button bits (as hid/key_map.h has them).
    namespace button
    {
        constexpr uint16_t DpadUp = 0x0001, DpadDown = 0x0002, DpadLeft = 0x0004, DpadRight = 0x0008;
        constexpr uint16_t Start = 0x0010, Back = 0x0020, LeftThumb = 0x0040, RightThumb = 0x0080;
        constexpr uint16_t LB = 0x0100, RB = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
    }

    enum class Mode : uint8_t { Driving, Menu };
    // Settings > Touch > Steering (user/settings.h's TouchSteering has these values).
    enum class Steering : int32_t { Buttons = 0, Tilt = 1, Stick = 2 };

    enum class Kind : uint8_t
    {
        Button,      // holds `button`
        SteerLeft,   // the steering buttons
        SteerRight,
        Stick,       // the steering stick: its resting place (a finger lands anywhere in its zone)
        Gas,         // the right trigger
        Brake,       // the left trigger
        LeftTrigger, RightTrigger,  // the menus' (the map zooms with them)
        Settings,    // opens the runtime's settings menu
        // Edit Layout's bar (EditBar): which layout is edited, its defaults back, done.
        EditSwitch, EditReset, EditDone,
    };

    enum class Glyph : uint8_t { Text, Left, Right, Up, Down };

    struct Widget
    {
        Kind kind;
        uint16_t button = 0;     // Kind::Button
        float x = 0, y = 0, r = 0;  // the centre and the radius
        const char* label = "";
        Glyph glyph = Glyph::Text;
    };

    struct Layout
    {
        std::vector<Widget> widgets;
        float unit = 0;
        // Kind::Stick: the zone a finger may land in to take the stick (left part of the
        // window, below its top row), and the stick's travel.
        float stickZoneRight = 0, stickZoneTop = 0, stickTravel = 0;
    };

    // The player's own places for controls (Settings > Touch > Edit Layout): each moved
    // control's centre as fractions of the area's width and height, by Key.
    struct Point { float x = 0, y = 0; };
    using Placements = std::map<std::string, Point>;

    // A control's name in Placements (and touch_layout.txt): its layout, what it is, and
    // its button ("driving gas", "menus button 1000").
    inline std::string Key(Mode mode, const Widget& w)
    {
        static const char* const kKinds[] = { "button", "steer_left", "steer_right", "stick", "gas", "brake", "lt", "rt", "settings",
                                              "edit_switch", "edit_reset", "edit_done" };
        std::string key = std::string(mode == Mode::Driving ? "driving " : "menus ") + kKinds[size_t(w.kind)];
        if (w.kind == Kind::Button)
        {
            char bit[8];
            snprintf(bit, sizeof(bit), " %04X", unsigned(w.button));
            key += bit;
        }
        return key;
    }

    // `scale`: Settings > Touch > Button Size (0.6 to 1: at 1 the two columns of
    // controls fill the window's width or height, whichever runs out first).
    inline float Unit(float width, float height, float scale)
    {
        return std::max(1.0f, std::min(height / 100.0f, width / 170.0f) * scale);
    }

    inline Layout Build(Mode mode, Steering steering, bool autoGas, float width, float height, float scale,
        const Placements* placed = nullptr)
    {
        Layout l;
        const float u = l.unit = Unit(width, height, scale);
        const float m = 4.0f * u;  // the margin from the window's edges
        const float W = width, H = height;
        auto add = [&](Kind kind, uint16_t button, float x, float y, float r, const char* label, Glyph glyph = Glyph::Text) {
            l.widgets.push_back(Widget{ kind, button, x, y, r, label, glyph });
        };
        // The top row, both layouts: the runtime's settings at the left, the pause at the right.
        const float top = m + 6.0f * u;
        add(Kind::Settings, 0, m + 6.0f * u, top, 6.0f * u, "MENU");
        if (mode == Mode::Driving)
        {
            add(Kind::Button, button::Start, W - m - 6.0f * u, top, 6.0f * u, "II");
            add(Kind::Button, button::RB, W - m - 21.0f * u, top, 6.0f * u, "CAM");
            add(Kind::Button, button::Back, W - m - 36.0f * u, top, 6.0f * u, "RESET");
            add(Kind::Button, button::Y, m + 21.0f * u, top, 6.0f * u, "LOOK");
            // Steering, bottom left.
            if (steering == Steering::Buttons)
            {
                add(Kind::SteerLeft, 0, m + 12.0f * u, H - m - 12.0f * u, 12.0f * u, "", Glyph::Left);
                add(Kind::SteerRight, 0, m + 39.0f * u, H - m - 12.0f * u, 12.0f * u, "", Glyph::Right);
            }
            else if (steering == Steering::Stick)
            {
                add(Kind::Stick, 0, m + 16.0f * u, H - m - 16.0f * u, 16.0f * u, "");
                l.stickZoneRight = W * 0.42f;
                l.stickZoneTop = top + 8.0f * u;
                l.stickTravel = 12.0f * u;
            }
            // The pedals and the driving buttons, bottom right.
            const float gx = W - m - 13.0f * u, gy = H - m - 13.0f * u;
            if (!autoGas)
                add(Kind::Gas, 0, gx, gy, 13.0f * u, "GAS");
            else
                add(Kind::Button, button::B, gx, gy, 13.0f * u, "NITRO");
            add(Kind::Brake, 0, gx - 28.0f * u, H - m - 10.0f * u, 10.0f * u, "BRAKE");
            if (!autoGas)
                add(Kind::Button, button::B, gx - 2.0f * u, gy - 25.0f * u, 9.0f * u, "NITRO");
            add(Kind::Button, button::A, gx - 22.0f * u, gy - 21.0f * u, 8.0f * u, "E-BRAKE");
            add(Kind::Button, button::X, gx - 49.0f * u, H - m - 8.0f * u, 8.0f * u, "SLOW");
        }
        else
        {
            add(Kind::Button, button::Start, W - m - 6.0f * u, top, 6.0f * u, "START");
            add(Kind::Button, button::Back, W - m - 21.0f * u, top, 6.0f * u, "BACK");
            add(Kind::Button, button::RB, W - m - 36.0f * u, top, 6.0f * u, "RB");
            add(Kind::RightTrigger, 0, W - m - 51.0f * u, top, 6.0f * u, "RT");
            add(Kind::Button, button::LB, m + 21.0f * u, top, 6.0f * u, "LB");
            add(Kind::LeftTrigger, 0, m + 36.0f * u, top, 6.0f * u, "LT");
            // The D-pad, bottom left; A B X Y, bottom right, as on the pad.
            const float dx = m + 22.0f * u, dy = H - m - 22.0f * u, d = 13.0f * u, r = 8.5f * u;
            add(Kind::Button, button::DpadUp, dx, dy - d, r, "", Glyph::Up);
            add(Kind::Button, button::DpadDown, dx, dy + d, r, "", Glyph::Down);
            add(Kind::Button, button::DpadLeft, dx - d, dy, r, "", Glyph::Left);
            add(Kind::Button, button::DpadRight, dx + d, dy, r, "", Glyph::Right);
            const float fx = W - m - 22.0f * u, fy = H - m - 22.0f * u;
            add(Kind::Button, button::A, fx, fy + d, r, "A");
            add(Kind::Button, button::B, fx + d, fy, r, "B");
            add(Kind::Button, button::X, fx - d, fy, r, "X");
            add(Kind::Button, button::Y, fx, fy - d, r, "Y");
        }
        // The player's own places, kept inside the area; a moved stick takes the zone
        // around and to the left of where it now rests.
        if (placed)
            for (Widget& w : l.widgets)
                if (auto it = placed->find(Key(mode, w)); it != placed->end())
                {
                    w.x = std::clamp(it->second.x * W, w.r, std::max(w.r, W - w.r));
                    w.y = std::clamp(it->second.y * H, w.r, std::max(w.r, H - w.r));
                    if (w.kind == Kind::Stick)
                    {
                        l.stickZoneRight = std::min(W, w.x + w.r * 1.6f);
                        l.stickZoneTop = std::max(0.0f, w.y - w.r * 1.6f);
                    }
                }
        return l;
    }

    // Edit Layout's bar, along the top in the middle: the layout being edited (tap: the
    // other one), its defaults back, done.
    inline std::vector<Widget> EditBar(float width, float height, float scale)
    {
        const float u = Unit(width, height, scale), m = 4.0f * u, y = m + 7.0f * u, r = 7.0f * u;
        return {
            Widget{ Kind::EditSwitch, 0, width * 0.5f - 17.0f * u, y, r, "LAYOUT" },
            Widget{ Kind::EditReset, 0, width * 0.5f, y, r, "RESET" },
            Widget{ Kind::EditDone, 0, width * 0.5f + 17.0f * u, y, r, "DONE" },
        };
    }

    // Edit Layout: the control under a finger, the stick's circle included; -1: none.
    inline int Grab(const std::vector<Widget>& widgets, float x, float y)
    {
        int best = -1;
        float bestDistance = 0;
        for (size_t i = 0; i < widgets.size(); i++)
        {
            const float distance = std::hypot(x - widgets[i].x, y - widgets[i].y) / widgets[i].r;
            if (distance <= 1.2f && (best < 0 || distance < bestDistance))
            {
                best = int(i);
                bestDistance = distance;
            }
        }
        return best;
    }

    // touch_layout.txt: one line per moved control, "<key> <x> <y>" (Key's words, then
    // the fractions); anything else is skipped.
    inline Placements ParsePlacements(const std::string& text)
    {
        Placements p;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos)
                end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            const size_t lastSpace = line.find_last_of(' ');
            const size_t prevSpace = lastSpace == std::string::npos || lastSpace == 0 ? std::string::npos : line.find_last_of(' ', lastSpace - 1);
            if (prevSpace == std::string::npos || prevSpace == 0)
                continue;
            float x = 0, y = 0;
            if (sscanf(line.c_str() + prevSpace, " %f %f", &x, &y) != 2 || !(x >= 0 && x <= 1 && y >= 0 && y <= 1))
                continue;
            p[line.substr(0, prevSpace)] = Point{ x, y };
        }
        return p;
    }
    inline std::string FormatPlacements(const Placements& p)
    {
        std::string text;
        for (const auto& [key, at] : p)
        {
            char numbers[48];
            snprintf(numbers, sizeof(numbers), " %.4f %.4f\n", double(at.x), double(at.y));
            text += key + numbers;
        }
        return text;
    }

    // A layout built for an area of the window (its safe area: clear of a camera's
    // cutout) moved to where the area lies in it.
    inline void Offset(Layout& l, float dx, float dy)
    {
        for (Widget& w : l.widgets)
        {
            w.x += dx;
            w.y += dy;
        }
        if (l.stickZoneRight > 0)
        {
            l.stickZoneRight += dx;
            l.stickZoneTop += dy;
        }
    }

    // The widget a finger at (x, y) presses: the nearest whose circle, grown by a fifth
    // (fingers are wide), holds the point; -1: none. The stick takes its whole zone.
    inline int HitTest(const Layout& l, float x, float y)
    {
        int best = -1;
        float bestDistance = 0;
        for (size_t i = 0; i < l.widgets.size(); i++)
        {
            const Widget& w = l.widgets[i];
            if (w.kind == Kind::Stick)
                continue;
            const float distance = std::hypot(x - w.x, y - w.y);
            if (distance <= w.r * 1.2f && (best < 0 || distance / w.r < bestDistance))
            {
                best = int(i);
                bestDistance = distance / w.r;
            }
        }
        if (best >= 0)
            return best;
        for (size_t i = 0; i < l.widgets.size(); i++)
            if (l.widgets[i].kind == Kind::Stick && x <= l.stickZoneRight && y >= l.stickZoneTop)
                return int(i);
        return -1;
    }

    // Controls a finger slides between without lifting: the two steering buttons, the
    // two pedals, the D-pad's four. 0: none (a finger keeps what it landed on, wherever
    // it goes, so a thumb drifting off a button keeps holding it).
    inline int SlideGroup(Kind kind, uint16_t bit)
    {
        if (kind == Kind::SteerLeft || kind == Kind::SteerRight)
            return 1;
        if (kind == Kind::Gas || kind == Kind::Brake)
            return 2;
        if (kind == Kind::Button && bit != 0 && (bit & ~(button::DpadUp | button::DpadDown | button::DpadLeft | button::DpadRight)) == 0)
            return 3;
        return 0;
    }

    // The steering buttons: the stick moves toward the held direction (both or none:
    // the centre), to full lock in `inSeconds` (Settings > Touch > Button Steering), back
    // to the centre (or through it the other way) in under two thirds of that.
    constexpr float kSteerInSeconds = 0.14f;  // Normal
    inline float SteerInSeconds(int buttonSteering)  // settings::TouchButtonSteering's values
    {
        return buttonSteering == 0 ? 0.08f : buttonSteering == 2 ? 0.25f : kSteerInSeconds;
    }
    inline float EaseSteering(float current, int direction, float dt, float inSeconds = kSteerInSeconds)
    {
        const float target = float(std::clamp(direction, -1, 1));
        const bool returning = target == 0.0f || (current != 0.0f && (current > 0) != (target > 0));
        const float step = dt / (returning ? inSeconds * 0.6f : inSeconds);
        if (current < target)
            return std::min(target, current + step);
        return std::max(target, current - step);
    }

    // The stick: the finger's offset from where it landed, over the travel, as XInput's
    // axes (up positive), inside the unit circle, with a small deadzone rescaled out.
    struct StickAxes { float x = 0, y = 0; };
    inline StickAxes StickFrom(float originX, float originY, float x, float y, float travel)
    {
        StickAxes s;
        if (travel <= 0)
            return s;
        float dx = (x - originX) / travel, dy = -(y - originY) / travel;
        const float length = std::hypot(dx, dy);
        constexpr float kDeadzone = 0.08f;
        if (length <= kDeadzone)
            return s;
        const float scaled = std::min(1.0f, (length - kDeadzone) / (1.0f - kDeadzone));
        s.x = dx / length * scaled;
        s.y = dy / length * scaled;
        return s;
    }

    // The tilt: the accelerometer's reading in the screen's axes (x right, y up, as
    // Android's; what holds the phone up reads +g along y when it is upright) gives the
    // roll in the screen's plane; `fullLockDegrees` of it is full lock, the first 2
    // degrees nothing. Positive: right. A phone lying flat steers nothing.
    inline float SteeringFromTilt(float ax, float ay, float fullLockDegrees)
    {
        if (std::hypot(ax, ay) < 2.0f)  // m/s^2: the screen's plane is nearly level
            return 0.0f;
        const float degrees = std::atan2(-ax, ay) * 57.29578f;
        constexpr float kDeadzone = 2.0f;
        const float magnitude = std::max(0.0f, std::fabs(degrees) - kDeadzone) / std::max(1.0f, fullLockDegrees - kDeadzone);
        return std::copysign(std::min(1.0f, magnitude), degrees);
    }

    // Android's sensors are in the device's natural orientation; the screen may be
    // turned by 0, 90, 180 or 270 degrees from it (Display.getRotation: 90 when the
    // device is turned a quarter counter-clockwise, its top to the left). The reading
    // in the screen's axes, as Android's AccelerometerPlay sample has it: at 90 the
    // screen's right is the device's -y and its up the device's x. (The other way
    // round steered the wrong way on the OnePlus.)
    inline void ToScreenAxes(int rotationDegrees, float ax, float ay, float& sx, float& sy)
    {
        switch (((rotationDegrees % 360) + 360) % 360)
        {
        case 90: sx = -ay; sy = ax; break;
        case 180: sx = -ax; sy = -ay; break;
        case 270: sx = ay; sy = -ax; break;
        default: sx = ax; sy = ay; break;
        }
    }

    // The menus' gestures: fingers that landed on no control, from the first landing
    // until the last lifts. A tap (no finger moved past the slop, all lifted within
    // kTapSeconds) with one finger is A, with two B (three is the settings menu's own
    // tap: nothing here); a finger moving kSwipeUnits in one direction is the D-pad
    // that way, as it lifts.
    constexpr float kTapSeconds = 0.35f, kSlopUnits = 3.0f, kSwipeUnits = 9.0f;
    inline uint16_t Tap(int fingers, double seconds, bool moved)
    {
        if (moved || seconds > kTapSeconds)
            return 0;
        return fingers == 1 ? button::A : fingers == 2 ? button::B : 0;
    }
    inline uint16_t Swipe(float dx, float dy, float unit)
    {
        const float distance = std::hypot(dx, dy);
        if (distance < kSwipeUnits * unit)
            return 0;
        if (std::fabs(dx) > std::fabs(dy))
            return dx > 0 ? button::DpadRight : button::DpadLeft;
        return dy > 0 ? button::DpadDown : button::DpadUp;
    }
}
