// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Touch controls (Settings > Touch): a touchscreen as another controller 0. While
// shown (On, or Auto with no gamepad or wheel open, a touchscreen present, and no
// menu of ours open), each finger that lands on one of hid/touch_layout.h's
// controls holds it until it lifts; ui/touch_overlay.cpp draws them. The layout
// follows the game, by two words of its own (touch.cpp's kPauseWord and
// kControlWord): the menus' one while its pause menu or map is up, or no car has
// been under the player's control for 5 s (the front end, loading, a race's intro),
// the driving one otherwise.
//
//   - Steering: two buttons that ease the stick to full lock and back, the
//     device's tilt (its accelerometer, SDL's sensor API), or a stick that
//     centres where the finger lands on the left.
//   - Auto Gas: the right trigger held while driving, released for the brake.
//   - The menus' gestures, on fingers that land on no control: a tap is A, a
//     two-finger tap B, a swipe the D-pad that way (hid/touch_layout.h).
//   - The settings menu: the MENU button (or the three-finger tap, ui.h).
//   - NFSMW_VIRTUAL_TOUCH="2:down 1 0.9 0.85,4:up 1,5:move 2 0.1 0.8": fingers
//     put down, moved and lifted at seconds since launch (window fractions),
//     as SDL finger events of a touchscreen of its own; the controls show
//     wherever this is set (tests on a PC: vptest.ps1 shoots them).
//   - NFSMW_INPUT_LOG=1 also logs each control a finger presses or lets go.
#pragma once
#include <cstdint>
#include <vector>

#include <SDL3/SDL_events.h>

#include "touch_layout.h"

namespace hid::touch
{
    // Main thread, from hid::HandleEvent.
    void HandleEvent(const SDL_Event& event);
    // Main thread, from hid::Update, after the gamepad and the keyboard: adds the held
    // controls to the pad, and replaces the axes and triggers it steers or presses.
    // `controllerOpen`: a gamepad or a wheel is open (Auto hides the controls then).
    // True: the left stick's X is the touch's.
    bool Contribute(bool controllerOpen, uint16_t& buttons, float& lx, float& ly, float& lt, float& rt);
    // Fingers let go (a suspension, the window losing focus).
    void Release();

    // What ui/touch_overlay.cpp draws, laid out for the window's points.
    struct View
    {
        touchlayout::Layout layout;
        std::vector<bool> pressed;  // per widget
        float width = 0, height = 0;  // the window's points the layout is for
        float opacity = 0.5f;
        bool stickHeld = false;
        float stickOriginX = 0, stickOriginY = 0, stickX = 0, stickY = 0;
        float steering = 0;  // the left stick's X the touch gives (the tilt's indicator)
        bool tilt = false;
    };
    // Main thread: the controls are shown (as of the last hid::Update).
    bool Visible();
    // Main thread: false while the controls aren't shown.
    bool GetView(View& view);
    // A touchscreen is present (or NFSMW_VIRTUAL_TOUCH is set): Settings > Touch shows.
    bool ScreenPresent();
}
