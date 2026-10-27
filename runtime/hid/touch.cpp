// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See touch.h.
#include <stdafx.h>
#include "touch.h"

#include <SDL3/SDL.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
#include <kernel/memory.h>
#include <ui/ui.h>
#include <user/settings.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace hid::touch
{
    namespace
    {
        using namespace touchlayout;
        using settings::Id;

        // The touchscreen NFSMW_VIRTUAL_TOUCH's fingers are on (no device has it).
        constexpr SDL_TouchID kVirtualTouchID = 0x5B5B0001;

        struct Finger
        {
            float x = 0, y = 0;            // where it is (window points)
            float startX = 0, startY = 0;  // where it landed
            Uint64 since = 0;              // when (SDL ticks, ns)
            Kind kind = Kind::Button;      // what it holds, taken at the landing (the layout may change under it)
            uint16_t button = 0;
            bool onControl = false;        // false: a gesture finger (the menus) or nothing (driving)
        };
        std::unordered_map<SDL_FingerID, Finger> s_fingers;

        // The menus' gesture: the fingers that landed on no control, from the first until the last lifts.
        struct Gesture
        {
            int down = 0, most = 0;
            Uint64 since = 0;
            bool moved = false, done = false;
        };
        Gesture s_gesture;

        // A tap or a swipe holds its button this long, so the game's frame sees it.
        constexpr Uint64 kPulseNs = 120'000'000;
        std::vector<std::pair<uint16_t, Uint64>> s_pulses;  // button, until

        Mode s_mode = Mode::Menu;
        bool s_controllerOpen = false;
        bool s_shown = false;
        float s_steer = 0;      // the steering buttons' eased stick
        float s_touchLx = 0;    // the left stick X the touch gave last (the overlay's tilt indicator)
        std::chrono::steady_clock::time_point s_lastContribute{};

        // The tilt: the accelerometer, opened while Steering is Tilt.
        SDL_Sensor* s_accel = nullptr;
        bool s_sensorTried = false;
        float s_gravityX = 0, s_gravityY = 0;
        bool s_gravityValid = false;

        const bool s_log = [] { const char* v = std::getenv("NFSMW_INPUT_LOG"); return v && v[0] == '1'; }();

        // NFSMW_VIRTUAL_TOUCH's steps.
        struct Step
        {
            double at = 0;
            Uint32 type = 0;  // SDL_EVENT_FINGER_*
            SDL_FingerID id = 0;
            float x = 0, y = 0;
        };
        std::vector<Step> s_script;
        size_t s_scriptNext = 0;
        const auto s_launch = std::chrono::steady_clock::now();
        const bool s_virtual = [] {
            const char* v = std::getenv("NFSMW_VIRTUAL_TOUCH");
            if (!v || !*v)
                return false;
            std::string spec(v);
            for (size_t pos = 0; pos < spec.size();)
            {
                size_t end = spec.find(',', pos);
                if (end == std::string::npos)
                    end = spec.size();
                const std::string item = spec.substr(pos, end - pos);
                pos = end + 1;
                Step s;
                char what[16] = {};
                unsigned long long id = 0;
                const int n = sscanf(item.c_str(), " %lf : %15s %llu %f %f", &s.at, what, &id, &s.x, &s.y);
                const std::string w(what);
                s.id = SDL_FingerID(id + 1);  // (0 isn't a finger)
                if (n >= 3 && w == "up")
                    s.type = SDL_EVENT_FINGER_UP;
                else if (n == 5 && w == "down")
                    s.type = SDL_EVENT_FINGER_DOWN;
                else if (n == 5 && w == "move")
                    s.type = SDL_EVENT_FINGER_MOTION;
                else
                {
                    fprintf(stderr, "[touch] NFSMW_VIRTUAL_TOUCH: \"%s\" is not \"<s>:down|move <id> <x> <y>\" or \"<s>:up <id>\"\n", item.c_str());
                    continue;
                }
                s_script.push_back(s);
            }
            std::stable_sort(s_script.begin(), s_script.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
            fprintf(stderr, "[touch] NFSMW_VIRTUAL_TOUCH: %zu steps\n", s_script.size());
            return true;
        }();

        SDL_Window* Window()
        {
            int count = 0;
            SDL_Window** windows = SDL_GetWindows(&count);
            SDL_Window* w = windows && count > 0 ? windows[0] : nullptr;
            SDL_free(windows);
            return w;
        }

        bool WindowSize(float& w, float& h)
        {
            int iw = 0, ih = 0;
            SDL_Window* window = Window();
            if (!window || !SDL_GetWindowSize(window, &iw, &ih) || iw <= 0 || ih <= 0)
                return false;
            w = float(iw);
            h = float(ih);
            return true;
        }

        Steering SteeringSetting()
        {
            return Steering(settings::GetInt(Id::TouchSteering));
        }

        // The window's safe area (clear of a camera's cutout and the like), in its points.
        void SafeArea(float w, float h, float& x0, float& y0, float& aw, float& ah)
        {
            x0 = y0 = 0;
            aw = w;
            ah = h;
            SDL_Rect safe;
            if (SDL_Window* window = Window(); window && SDL_GetWindowSafeArea(window, &safe) && safe.w > 0 && safe.h > 0)
            {
                x0 = std::clamp(float(safe.x), 0.0f, w);
                y0 = std::clamp(float(safe.y), 0.0f, h);
                aw = std::clamp(float(safe.w), 1.0f, w - x0);
                ah = std::clamp(float(safe.h), 1.0f, h - y0);
            }
        }

        Layout CurrentLayout(float w, float h)
        {
            float x0, y0, aw, ah;
            SafeArea(w, h, x0, y0, aw, ah);
            Layout l = Build(s_mode, SteeringSetting(), settings::GetBool(Id::TouchAutoGas), aw, ah,
                float(settings::GetInt(Id::TouchSize)) / 100.0f);
            Offset(l, x0, y0);
            return l;
        }

        const char* Name(const Finger& f)
        {
            switch (f.kind)
            {
            case Kind::SteerLeft: return "steer left";
            case Kind::SteerRight: return "steer right";
            case Kind::Stick: return "stick";
            case Kind::Gas: return "gas";
            case Kind::Brake: return "brake";
            case Kind::LeftTrigger: return "LT";
            case Kind::RightTrigger: return "RT";
            case Kind::Settings: return "settings";
            default: break;
            }
            switch (f.button)
            {
            case button::A: return "A";
            case button::B: return "B";
            case button::X: return "X";
            case button::Y: return "Y";
            case button::LB: return "LB";
            case button::RB: return "RB";
            case button::Start: return "Start";
            case button::Back: return "Back";
            case button::DpadUp: return "up";
            case button::DpadDown: return "down";
            case button::DpadLeft: return "left";
            case button::DpadRight: return "right";
            default: return "?";
            }
        }

        void Pulse(uint16_t bit, Uint64 now, const char* why)
        {
            if (!bit)
                return;
            s_pulses.emplace_back(bit, now + kPulseNs);
            if (s_log)
                fprintf(stderr, "[touch] %s: button %04X\n", why, unsigned(bit));
        }

        bool Accepted(const SDL_TouchFingerEvent& f)
        {
            if (f.touchID == kVirtualTouchID)
                return true;
            return f.touchID != SDL_MOUSE_TOUCHID && f.touchID != SDL_PEN_TOUCHID &&
                   SDL_GetTouchDeviceType(f.touchID) == SDL_TOUCH_DEVICE_DIRECT;
        }

        void Down(const SDL_TouchFingerEvent& e, float w, float h)
        {
            Finger f;
            f.x = f.startX = e.x * w;
            f.y = f.startY = e.y * h;
            f.since = e.timestamp;
            const Layout l = CurrentLayout(w, h);
            int hit = HitTest(l, f.x, f.y);
            // The stick is the first finger's: another landing in its zone (a second finger,
            // the palm) would snap it back to the centre.
            if (hit >= 0 && l.widgets[size_t(hit)].kind == Kind::Stick)
                for (const auto& [id, other] : s_fingers)
                    if (other.onControl && other.kind == Kind::Stick)
                        hit = -1;
            if (hit >= 0)
            {
                f.onControl = true;
                f.kind = l.widgets[size_t(hit)].kind;
                f.button = l.widgets[size_t(hit)].button;
                if (s_log)
                    fprintf(stderr, "[touch] finger %llu on %s\n", (unsigned long long)e.fingerID, Name(f));
            }
            else if (s_mode == Mode::Menu && settings::GetBool(Id::TouchGestures))
            {
                if (s_gesture.down == 0)
                    s_gesture = Gesture{ 0, 0, e.timestamp, false, false };
                s_gesture.down++;
                s_gesture.most = std::max(s_gesture.most, s_gesture.down);
            }
            else
                return;  // driving, on no control: nothing
            s_fingers[e.fingerID] = f;
        }

        void Up(const SDL_TouchFingerEvent& e, float w, float h, bool canceled)
        {
            auto it = s_fingers.find(e.fingerID);
            if (it == s_fingers.end())
                return;
            const Finger f = it->second;
            s_fingers.erase(it);
            const float x = e.x * w, y = e.y * h;
            if (f.onControl)
            {
                if (s_log)
                    fprintf(stderr, "[touch] finger %llu off %s\n", (unsigned long long)e.fingerID, Name(f));
                // The settings menu opens as the finger lifts on its button (on its landing, the mouse
                // press SDL makes of the finger would reach the menu just opened).
                if (f.kind == Kind::Settings && !canceled)
                {
                    const Layout l = CurrentLayout(w, h);
                    for (const Widget& wd : l.widgets)
                        if (wd.kind == Kind::Settings && std::hypot(x - wd.x, y - wd.y) <= wd.r * 1.4f)
                        {
                            fprintf(stderr, "[touch] settings button\n");
                            Release();
                            ui::OpenSettings();
                            break;
                        }
                }
                return;
            }
            // A gesture finger.
            if (!canceled && !s_gesture.done)
                if (uint16_t bit = Swipe(x - f.startX, y - f.startY, Unit(w, h, 1.0f)))
                {
                    Pulse(bit, e.timestamp, "swipe");
                    s_gesture.done = true;
                }
            if (canceled)
                s_gesture.done = true;
            if (--s_gesture.down > 0)
                return;
            if (!s_gesture.done)
                Pulse(Tap(s_gesture.most, double(e.timestamp - s_gesture.since) / 1e9, s_gesture.moved), e.timestamp,
                    s_gesture.most == 2 ? "two-finger tap" : "tap");
            s_gesture = Gesture{};
        }

        void StepScript()
        {
            if (!s_virtual || s_scriptNext >= s_script.size())
                return;
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_launch).count();
            SDL_Window* window = Window();
            while (s_scriptNext < s_script.size() && s_script[s_scriptNext].at <= now)
            {
                const Step& s = s_script[s_scriptNext++];
                SDL_Event e{};
                e.type = s.type;
                e.tfinger.timestamp = SDL_GetTicksNS();
                e.tfinger.touchID = kVirtualTouchID;
                e.tfinger.fingerID = s.id;
                if (s.type != SDL_EVENT_FINGER_UP)
                {
                    e.tfinger.x = s.x;
                    e.tfinger.y = s.y;
                }
                else
                    for (const auto& [id, f] : s_fingers)
                        if (id == s.id)
                        {
                            float w = 1, h = 1;
                            WindowSize(w, h);
                            e.tfinger.x = f.x / w;
                            e.tfinger.y = f.y / h;
                        }
                e.tfinger.pressure = s.type == SDL_EVENT_FINGER_UP ? 0.0f : 1.0f;
                e.tfinger.windowID = window ? SDL_GetWindowID(window) : 0;
                SDL_PushEvent(&e);
            }
        }

        // The menus' layout while the game shows a menu, from two words of its own (found by
        // diffing its memory between states; the stepper runs in its front end too, so it
        // tells nothing):
        //   - its pause word (guest 0x82A2C5CC): 1 while the pause menu or the map is up, 0
        //     while the world runs, in free roam and in a race alike: the menus' layout at once;
        //   - its car control word (guest 0x82A153BC): 1 while the player's car is under their
        //     control, 0 in the front end, loading, a race's intro and after its finish, but
        //     also for a moment in a crash or a jump (up to 3 s seen): the menus' layout only
        //     once it has read 0 for 5 s.
        constexpr uint32_t kPauseWord = 0x82A2C5CC, kControlWord = 0x82A153BC;
        constexpr auto kNoControlMenus = std::chrono::seconds(5);
        std::chrono::steady_clock::time_point s_controlSeen{};

        // (Both are in .data/.bss, which the PAL release keeps where the USA one has them, as
        // game/frame_time.cpp reads 0x82A153B8: no region::Addr lookup.)
        uint32_t GuestWord(uint32_t address)
        {
            if (!g_memory.base)
                return 0;
            return ByteSwap(__atomic_load_n(reinterpret_cast<const uint32_t*>(g_memory.base + address), __ATOMIC_RELAXED));
        }

        void UpdateMode()
        {
            const auto now = std::chrono::steady_clock::now();
            if (GuestWord(kControlWord) == 1)
                s_controlSeen = now;
            const bool paused = GuestWord(kPauseWord) != 0;
            const bool control = s_controlSeen.time_since_epoch().count() != 0 && now - s_controlSeen < kNoControlMenus;
            const Mode mode = control && !paused ? Mode::Driving : Mode::Menu;
            if (mode != s_mode)
            {
                fprintf(stderr, "[touch] layout: %s%s\n", mode == Mode::Driving ? "driving" : "menus",
                    mode == Mode::Driving ? "" : paused ? " (the game's pause)" : " (no car under control)");
                s_mode = mode;
                s_gesture = Gesture{};  // (its fingers carry on as nothing)
                for (auto it = s_fingers.begin(); it != s_fingers.end();)
                    it = it->second.onControl ? std::next(it) : s_fingers.erase(it);
            }
        }

        bool Shown()
        {
            const auto mode = settings::TouchControlsMode(settings::GetInt(Id::TouchControls));
            if (mode == settings::TouchControlsMode::Off || !ScreenPresent())
                return false;
            if (mode == settings::TouchControlsMode::Auto && !s_virtual)
            {
                if (s_controllerOpen)
                    return false;
#ifndef __ANDROID__
                // A desktop's keyboard counts as a controller (a laptop with a touchscreen is
                // played from it); On shows the controls there anyway.
                if (SDL_HasKeyboard())
                    return false;
#endif
            }
            return ui::Ready() && !ui::CapturingInput() && !ui::HasModal();
        }

        // The roll the accelerometer reads in the screen's plane, as the left stick's X. False
        // without an accelerometer (a Steam Deck: SDL has no sensors on Linux).
        bool TiltSteering(float& steering)
        {
            if (!s_accel && !s_sensorTried)
            {
                s_sensorTried = true;
                if (!SDL_WasInit(SDL_INIT_SENSOR) && !SDL_InitSubSystem(SDL_INIT_SENSOR))
                    fprintf(stderr, "[touch] tilt: no sensors (%s)\n", SDL_GetError());
                int count = 0;
                SDL_SensorID* ids = SDL_GetSensors(&count);
                for (int i = 0; i < count && !s_accel; i++)
                    if (SDL_GetSensorTypeForID(ids[i]) == SDL_SENSOR_ACCEL)
                        s_accel = SDL_OpenSensor(ids[i]);
                SDL_free(ids);
                fprintf(stderr, "[touch] tilt: %s\n", s_accel ? SDL_GetSensorName(s_accel) : "no accelerometer");
            }
            if (!s_accel)
                return false;
            float data[3] = {};
            if (!SDL_GetSensorData(s_accel, data, 3))
                return false;
            // The screen's turn from the device's natural orientation (Display.getRotation).
            // SDL's current orientation is that turn from portrait (SDL_android.c adds 90 on a
            // device whose natural orientation is landscape, a tablet), so the natural
            // orientation's own angle comes off it.
            auto angle = [](SDL_DisplayOrientation o) {
                switch (o)
                {
                case SDL_ORIENTATION_LANDSCAPE: return 90;
                case SDL_ORIENTATION_PORTRAIT_FLIPPED: return 180;
                case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return 270;
                default: return 0;
                }
            };
            int rotation = 0;
            if (SDL_Window* window = Window())
            {
                const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
                rotation = (angle(SDL_GetCurrentDisplayOrientation(display)) - angle(SDL_GetNaturalDisplayOrientation(display)) + 360) % 360;
            }
            float sx = 0, sy = 0;
            ToScreenAxes(rotation, data[0], data[1], sx, sy);
            // A little smoothing: the reading shakes with the hands and the engine's rumble.
            constexpr float kKeep = 0.6f;
            if (!s_gravityValid)
            {
                s_gravityX = sx;
                s_gravityY = sy;
                s_gravityValid = true;
            }
            s_gravityX = s_gravityX * kKeep + sx * (1.0f - kKeep);
            s_gravityY = s_gravityY * kKeep + sy * (1.0f - kKeep);
            steering = SteeringFromTilt(s_gravityX, s_gravityY, float(settings::GetInt(Id::TiltSensitivity)));
            return true;
        }

        void CloseSensor()
        {
            if (s_accel)
            {
                SDL_CloseSensor(s_accel);
                s_accel = nullptr;
            }
            s_sensorTried = false;
            s_gravityValid = false;
        }
    }

    bool ScreenPresent()
    {
        if (s_virtual)
            return true;
#if defined(__APPLE__) && TARGET_OS_IOS
        // Not on iPhone and iPad (they say the game needs a controller; the tilt is untested
        // there).
        return false;
#endif
        int count = 0;
        SDL_TouchID* ids = SDL_GetTouchDevices(&count);
        bool direct = false;
        for (int i = 0; i < count && !direct; i++)
            direct = SDL_GetTouchDeviceType(ids[i]) == SDL_TOUCH_DEVICE_DIRECT;
        SDL_free(ids);
        return direct;
    }

    void Release()
    {
        s_fingers.clear();
        s_gesture = Gesture{};
        s_pulses.clear();
        s_steer = 0;
    }

    void HandleEvent(const SDL_Event& e)
    {
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST)
        {
            Release();
            return;
        }
        if (e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION && e.type != SDL_EVENT_FINGER_UP &&
            e.type != SDL_EVENT_FINGER_CANCELED)
            return;
        if (!Accepted(e.tfinger))
            return;
        float w = 0, h = 0;
        if (!s_shown || !WindowSize(w, h))
        {
            s_fingers.erase(e.tfinger.fingerID);
            return;
        }
        switch (e.type)
        {
        case SDL_EVENT_FINGER_DOWN:
            Down(e.tfinger, w, h);
            break;
        case SDL_EVENT_FINGER_MOTION:
            if (auto it = s_fingers.find(e.tfinger.fingerID); it != s_fingers.end())
            {
                Finger& f = it->second;
                f.x = e.tfinger.x * w;
                f.y = e.tfinger.y * h;
                if (!f.onControl && std::hypot(f.x - f.startX, f.y - f.startY) > kSlopUnits * Unit(w, h, 1.0f))
                    s_gesture.moved = true;
            }
            break;
        default:
            Up(e.tfinger, w, h, e.type == SDL_EVENT_FINGER_CANCELED);
            break;
        }
    }

    bool Contribute(bool controllerOpen, uint16_t& buttons, float& lx, float& ly, float& lt, float& rt)
    {
        StepScript();
        s_controllerOpen = controllerOpen;
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - s_lastContribute).count(), 0.0f, 0.05f);
        s_lastContribute = now;
        const auto setting = settings::TouchControlsMode(settings::GetInt(Id::TouchControls));
        const bool possible = setting != settings::TouchControlsMode::Off && ScreenPresent();
        if (possible)
            UpdateMode();
        const bool shown = possible && Shown();
        if (shown != s_shown)
        {
            fprintf(stderr, "[touch] controls %s\n", shown ? "shown" : "hidden");
            s_shown = shown;
            Release();
        }
        const Steering steering = SteeringSetting();
        if (!shown || steering != Steering::Tilt)
            CloseSensor();
        if (!shown)
            return false;

        int steerDirection = 0;
        bool gas = false, brake = false, leftTrigger = false, rightTrigger = false, stick = false;
        StickAxes stickAxes;
        float w = 0, h = 0;
        WindowSize(w, h);
        const Layout layout = CurrentLayout(w, h);
        for (const auto& [id, f] : s_fingers)
        {
            if (!f.onControl)
                continue;
            switch (f.kind)
            {
            case Kind::Button: buttons |= f.button; break;
            case Kind::SteerLeft: steerDirection--; break;
            case Kind::SteerRight: steerDirection++; break;
            case Kind::Gas: gas = true; break;
            case Kind::Brake: brake = true; break;
            case Kind::LeftTrigger: leftTrigger = true; break;
            case Kind::RightTrigger: rightTrigger = true; break;
            case Kind::Stick:
                stick = true;
                stickAxes = StickFrom(f.startX, f.startY, f.x, f.y, layout.stickTravel);
                break;
            default: break;
            }
        }
        const Uint64 ticks = SDL_GetTicksNS();
        std::erase_if(s_pulses, [&](const auto& p) { return p.second <= ticks; });
        for (const auto& [bit, until] : s_pulses)
            buttons |= bit;

        bool steered = false;
        const bool otherBrake = lt >= 0.5f;  // the pad's or the keyboard's brake
        if (s_mode == Mode::Driving)
        {
            if (steering == Steering::Buttons)
            {
                s_steer = EaseSteering(s_steer, steerDirection, dt);
                if (s_steer != 0.0f || steerDirection != 0)
                {
                    lx = s_steer;
                    steered = true;
                }
            }
            else if (steering == Steering::Stick && stick)
            {
                lx = stickAxes.x;
                ly = stickAxes.y;
                steered = true;
            }
            else if (steering == Steering::Tilt)
            {
                float tilt = 0;
                if (TiltSteering(tilt))
                {
                    lx = tilt;
                    steered = true;
                }
            }
            if (gas || (settings::GetBool(Id::TouchAutoGas) && !brake && !otherBrake))
                rt = 1.0f;
            if (brake)
            {
                lt = 1.0f;
                if (!gas)
                    rt = 0.0f;
            }
        }
        else
        {
            s_steer = 0;
            if (leftTrigger)
                lt = 1.0f;
            if (rightTrigger)
                rt = 1.0f;
        }
        s_touchLx = steered ? lx : 0.0f;
        return steered;
    }

    bool Visible()
    {
        return s_shown;
    }

    bool GetView(View& view)
    {
        if (!s_shown)
            return false;
        float w = 0, h = 0;
        if (!WindowSize(w, h))
            return false;
        view.layout = CurrentLayout(w, h);
        view.width = w;
        view.height = h;
        view.opacity = float(settings::GetInt(Id::TouchOpacity)) / 100.0f;
        view.pressed.assign(view.layout.widgets.size(), false);
        view.stickHeld = false;
        view.tilt = s_mode == Mode::Driving && SteeringSetting() == Steering::Tilt;
        view.steering = s_touchLx;
        for (const auto& [id, f] : s_fingers)
        {
            if (!f.onControl)
                continue;
            for (size_t i = 0; i < view.layout.widgets.size(); i++)
            {
                const Widget& wd = view.layout.widgets[i];
                if (wd.kind == f.kind && wd.button == f.button)
                    view.pressed[i] = true;
            }
            if (f.kind == Kind::Stick)
            {
                view.stickHeld = true;
                view.stickOriginX = f.startX;
                view.stickOriginY = f.startY;
                view.stickX = f.x;
                view.stickY = f.y;
            }
        }
        // A pulse lights its button too (a tap or a swipe in the menus).
        for (const auto& [bit, until] : s_pulses)
            for (size_t i = 0; i < view.layout.widgets.size(); i++)
                if (view.layout.widgets[i].kind == Kind::Button && view.layout.widgets[i].button == bit)
                    view.pressed[i] = true;
        return true;
    }
}
