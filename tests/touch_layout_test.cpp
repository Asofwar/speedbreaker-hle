// Tests for hid/touch_layout.h: the touch controls' layouts, which control a finger
// presses, the steering buttons' easing, the stick, the tilt and the menus' gestures.
// Header only; from the repo root:
//   clang++ -std=c++20 -O2 -Iruntime tests/touch_layout_test.cpp -o build/touch_layout_test && build/touch_layout_test
#include <hid/touch_layout.h>

#include <cmath>
#include <cstdio>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_failures++; printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); printf(__VA_ARGS__); printf("\n"); } } while (0)

using namespace hid::touchlayout;

static int Find(const Layout& l, Kind kind, uint16_t bit = 0)
{
    for (size_t i = 0; i < l.widgets.size(); i++)
        if (l.widgets[i].kind == kind && l.widgets[i].button == bit)
            return int(i);
    return -1;
}

static bool Overlap(const Widget& a, const Widget& b)
{
    return std::hypot(a.x - b.x, a.y - b.y) < a.r + b.r;
}

int main()
{
    // Every layout fits its window and no two controls overlap, on a 16:9 window, a
    // 20:9 phone, a foldable's near-square screen and 4:3, at Settings > Touch >
    // Button Size's 60% to 100%.
    const float shapes[][2] = { { 1280, 720 }, { 2400, 1080 }, { 2480, 2248 }, { 800, 600 } };
    for (const auto& shape : shapes)
        for (float scale : { 0.6f, 0.8f, 1.0f })
            for (Mode mode : { Mode::Driving, Mode::Menu })
                for (Steering steering : { Steering::Buttons, Steering::Tilt, Steering::Stick })
                    for (bool autoGas : { false, true })
                    {
                        const Layout l = Build(mode, steering, autoGas, shape[0], shape[1], scale);
                        for (size_t i = 0; i < l.widgets.size(); i++)
                        {
                            const Widget& w = l.widgets[i];
                            CHECK(w.x - w.r >= 0 && w.x + w.r <= shape[0] && w.y - w.r >= 0 && w.y + w.r <= shape[1],
                                "%gx%g scale %g: control %zu inside the window", shape[0], shape[1], scale, i);
                            for (size_t j = i + 1; j < l.widgets.size(); j++)
                                if (w.kind != Kind::Stick && l.widgets[j].kind != Kind::Stick)
                                    CHECK(!Overlap(w, l.widgets[j]), "%gx%g scale %g mode %d: controls %zu and %zu apart",
                                        shape[0], shape[1], scale, int(mode), i, j);
                        }
                    }

    // Driving: the game's own buttons (Settings > Controls): A the handbrake, B nitrous, X
    // Speedbreaker, Y the rear view, RB the camera, Back the reset, Start the pause.
    {
        const Layout l = Build(Mode::Driving, Steering::Buttons, false, 1280, 720, 1.0f);
        CHECK(Find(l, Kind::Gas) >= 0 && Find(l, Kind::Brake) >= 0, "the pedals");
        CHECK(Find(l, Kind::SteerLeft) >= 0 && Find(l, Kind::SteerRight) >= 0, "the steering buttons");
        for (uint16_t bit : { button::A, button::B, button::X, button::Y, button::RB, button::Back, button::Start })
            CHECK(Find(l, Kind::Button, bit) >= 0, "button %04X", unsigned(bit));
        CHECK(Find(l, Kind::Settings) >= 0, "the settings button");
        // Auto Gas: no gas pedal; nitrous takes its place.
        const Layout a = Build(Mode::Driving, Steering::Buttons, true, 1280, 720, 1.0f);
        CHECK(Find(a, Kind::Gas) < 0 && Find(a, Kind::Button, button::B) >= 0, "auto gas: nitrous for the gas pedal");
        // Tilt: nothing to steer with on the screen, the brake and the handbrake for the
        // left thumb, the gas and nitrous for the right; Stick: the stick.
        for (bool autoGas : { false, true })
        {
            const Layout t = Build(Mode::Driving, Steering::Tilt, autoGas, 1280, 720, 1.0f);
            CHECK(Find(t, Kind::SteerLeft) < 0, "tilt: no steering buttons");
            CHECK(Find(t, Kind::Brake) >= 0 && t.widgets[size_t(Find(t, Kind::Brake))].x < 640 &&
                  t.widgets[size_t(Find(t, Kind::Button, button::A))].x < 640, "tilt: the brakes at the left");
            CHECK(Find(t, Kind::Button, button::B) >= 0 && t.widgets[size_t(Find(t, Kind::Button, button::B))].x > 640 &&
                  t.widgets[size_t(Find(t, Kind::Button, button::X))].x > 640, "tilt: nitrous and Speedbreaker at the right");
            if (!autoGas)
                CHECK(t.widgets[size_t(Find(t, Kind::Gas))].x > 640, "tilt: the gas at the right");
        }
        CHECK(Find(Build(Mode::Driving, Steering::Stick, false, 1280, 720, 1.0f), Kind::Stick) >= 0, "stick: the stick");
    }
    // The menus: the D-pad, A B X Y, LB RB LT RT, Start and Back.
    {
        const Layout l = Build(Mode::Menu, Steering::Buttons, false, 1280, 720, 1.0f);
        for (uint16_t bit : { button::DpadUp, button::DpadDown, button::DpadLeft, button::DpadRight, button::A, button::B, button::X,
                              button::Y, button::LB, button::RB, button::Start, button::Back })
            CHECK(Find(l, Kind::Button, bit) >= 0, "menus: button %04X", unsigned(bit));
        CHECK(Find(l, Kind::LeftTrigger) >= 0 && Find(l, Kind::RightTrigger) >= 0, "menus: the triggers");
        CHECK(Find(l, Kind::Gas) < 0 && Find(l, Kind::SteerLeft) < 0, "menus: no driving controls");
    }

    // Hit testing: a control's centre and a point just outside its circle (fingers are
    // wide: a fifth more counts); empty space is nothing, but the stick's zone is the stick.
    {
        const Layout l = Build(Mode::Driving, Steering::Buttons, false, 1280, 720, 1.0f);
        for (size_t i = 0; i < l.widgets.size(); i++)
            CHECK(HitTest(l, l.widgets[i].x, l.widgets[i].y) == int(i), "control %zu at its centre", i);
        const Widget& gas = l.widgets[size_t(Find(l, Kind::Gas))];
        CHECK(HitTest(l, gas.x - gas.r * 1.1f, gas.y) == Find(l, Kind::Gas), "just outside the gas pedal's circle");
        CHECK(HitTest(l, 640, 360) == -1, "the middle of the screen is nothing");
        const Layout s = Build(Mode::Driving, Steering::Stick, false, 1280, 720, 1.0f);
        CHECK(HitTest(s, 300, 500) == Find(s, Kind::Stick), "the stick's zone");
        CHECK(HitTest(s, 640, 360) == -1, "outside the stick's zone");
    }

    // The player's places: a moved control sits there (kept inside the window); the file
    // reads back what it wrote, and skips what isn't a place.
    {
        const Layout plain = Build(Mode::Driving, Steering::Buttons, false, 1280, 720, 1.0f);
        const Widget& gas = plain.widgets[size_t(Find(plain, Kind::Gas))];
        Placements p;
        p[Key(Mode::Driving, Steering::Buttons, gas)] = Point{ 0.5f, 0.5f };
        p["menus button 1000"] = Point{ 0.1f, 0.1f };
        const Layout moved = Build(Mode::Driving, Steering::Buttons, false, 1280, 720, 1.0f, &p);
        const Widget& g = moved.widgets[size_t(Find(moved, Kind::Gas))];
        CHECK(g.x == 640.0f && g.y == 360.0f, "the gas pedal moved: %g %g", g.x, g.y);
        CHECK(moved.widgets.size() == plain.widgets.size(), "the same controls");
        p[Key(Mode::Driving, Steering::Buttons, gas)] = Point{ 1.0f, 1.0f };
        const Layout edge = Build(Mode::Driving, Steering::Buttons, false, 1280, 720, 1.0f, &p);
        const Widget& e = edge.widgets[size_t(Find(edge, Kind::Gas))];
        CHECK(e.x + e.r <= 1280.0f && e.y + e.r <= 720.0f, "kept inside the window");
        CHECK(Key(Mode::Menu, Steering::Buttons, Widget{ Kind::Button, button::A }) == "menus button 1000" &&
              Key(Mode::Driving, Steering::Buttons, gas) == "driving gas" && Key(Mode::Driving, Steering::Stick, gas) == "driving gas" &&
              Key(Mode::Driving, Steering::Tilt, gas) == "tilt gas", "keys");
        const Placements back = ParsePlacements(FormatPlacements(p) + "junk\nmenus button 2000 5 5\n");
        CHECK(back.size() == 2 && back.at("driving gas").x == 1.0f && std::fabs(back.at("menus button 1000").y - 0.1f) < 1e-4f,
            "the file round trip, junk and out-of-range places skipped (%zu)", back.size());
        const std::vector<Widget> bar = EditBar(1280, 720, 1.0f);
        CHECK(bar.size() == 3 && Grab(bar, bar[2].x, bar[2].y) == 2 && Grab(bar, 0, 700) == -1, "the edit bar");
    }

    // Sliding: the steering buttons with each other, the pedals and nitrous, the D-pad; nothing else.
    {
        CHECK(SlideGroup(Kind::SteerLeft, 0) == SlideGroup(Kind::SteerRight, 0) && SlideGroup(Kind::SteerLeft, 0) != 0, "steering");
        CHECK(SlideGroup(Kind::Gas, 0) == SlideGroup(Kind::Brake, 0) && SlideGroup(Kind::Gas, 0) == SlideGroup(Kind::Button, button::B) &&
              SlideGroup(Kind::Gas, 0) != SlideGroup(Kind::SteerLeft, 0), "the pedals and nitrous");
        CHECK(SlideGroup(Kind::Button, button::DpadUp) == SlideGroup(Kind::Button, button::DpadRight) &&
              SlideGroup(Kind::Button, button::DpadUp) != 0, "the D-pad");
        CHECK(SlideGroup(Kind::Button, button::A) == 0 && SlideGroup(Kind::Button, button::X) == 0 && SlideGroup(Kind::Settings, 0) == 0 &&
              SlideGroup(Kind::Stick, 0) == 0, "buttons keep their finger");
    }

    // The steering buttons ease: to full lock in kSteerInSeconds, back faster, and the
    // other way through the centre.
    {
        float x = 0;
        int frames = 0;
        while (x < 1.0f && frames < 100)
        {
            x = EaseSteering(x, 1, 1.0f / 60);
            frames++;
        }
        CHECK(std::fabs(frames / 60.0f - kSteerInSeconds) < 2.0f / 60, "full lock in %d frames", frames);
        int back = 0;
        while (x > 0.0f && back < 100)
        {
            x = EaseSteering(x, 0, 1.0f / 60);
            back++;
        }
        CHECK(back < frames && x == 0.0f, "back to the centre in %d frames", back);
        CHECK(EaseSteering(0.5f, -1, 1.0f / 60) < 0.5f && EaseSteering(0.5f, 1, 1.0f / 60) > 0.5f, "the way it's held");
        CHECK(EaseSteering(0.3f, 0, 1.0f) == 0.0f && EaseSteering(-0.3f, 0, 1.0f) == 0.0f, "a long frame stops at the centre");
        // Button Steering: Quick reaches full lock sooner than Normal, Smooth later.
        CHECK(SteerInSeconds(0) < SteerInSeconds(1) && SteerInSeconds(1) < SteerInSeconds(2) && SteerInSeconds(1) == kSteerInSeconds, "speeds");
        CHECK(EaseSteering(0, 1, 0.05f, SteerInSeconds(0)) > EaseSteering(0, 1, 0.05f, SteerInSeconds(2)), "quick is quicker");
    }

    // The stick: the finger's offset over the travel, up positive, inside the circle.
    {
        CHECK(StickFrom(100, 100, 102, 100, 50).x == 0.0f, "inside the deadzone");
        const StickAxes right = StickFrom(100, 100, 150, 100, 50);
        CHECK(std::fabs(right.x - 1.0f) < 1e-5f && right.y == 0.0f, "full right");
        const StickAxes up = StickFrom(100, 100, 100, 40, 50);
        CHECK(std::fabs(up.y - 1.0f) < 1e-5f, "up is positive, clamped at full");
        const StickAxes diagonal = StickFrom(100, 100, 200, 200, 50);
        CHECK(std::fabs(std::hypot(diagonal.x, diagonal.y) - 1.0f) < 1e-4f, "inside the unit circle");
    }

    // The tilt: upright is straight on; turning the device clockwise steers right; past
    // the full-lock angle is full lock; lying flat steers nothing.
    {
        const float g = 9.81f;
        CHECK(SteeringFromTilt(0, g, 25) == 0.0f, "upright");
        const float a = 20.0f / 57.29578f;
        const float right = SteeringFromTilt(-g * std::sin(a), g * std::cos(a), 25);
        CHECK(right > 0.7f && right < 0.85f, "20 degrees clockwise of 25: %.3f", right);
        CHECK(SteeringFromTilt(g * std::sin(a), g * std::cos(a), 25) == -right, "the other way");
        const float b = 40.0f / 57.29578f;
        CHECK(SteeringFromTilt(-g * std::sin(b), g * std::cos(b), 25) == 1.0f, "past full lock");
        CHECK(SteeringFromTilt(0.3f, 0.2f, 25) == 0.0f, "lying flat");
        // The screen turned from the device's natural orientation (Android's remap).
        float sx = 0, sy = 0;
        ToScreenAxes(0, 1, 2, sx, sy);
        CHECK(sx == 1 && sy == 2, "rotation 0");
        ToScreenAxes(90, 1, 2, sx, sy);
        CHECK(sx == -2 && sy == 1, "rotation 90");
        ToScreenAxes(180, 1, 2, sx, sy);
        CHECK(sx == -1 && sy == -2, "rotation 180");
        ToScreenAxes(270, 1, 2, sx, sy);
        CHECK(sx == 2 && sy == -1, "rotation 270");
        // A phone with a tall natural screen, held in landscape at rotation 90 (its top
        // to the left) and turned clockwise as a wheel: the device's top rises, so what
        // holds it up reads along the device's +x and -y. That steers right.
        {
            const float t = 15.0f / 57.29578f, g = 9.81f;
            // Upright at rotation 90: up is the device's +x. Turned clockwise by t.
            const float ax = g * std::cos(t), ay = g * std::sin(t);
            ToScreenAxes(90, ax, ay, sx, sy);
            CHECK(SteeringFromTilt(sx, sy, 25) > 0.4f, "turned clockwise at rotation 90 steers right: %.3f", SteeringFromTilt(sx, sy, 25));
        }
    }

    // The menus' gestures.
    {
        CHECK(Tap(1, 0.1, false) == button::A, "a tap is A");
        CHECK(Tap(2, 0.2, false) == button::B, "a two-finger tap is B");
        CHECK(Tap(3, 0.2, false) == 0, "three fingers: the settings menu's own");
        CHECK(Tap(1, 0.1, true) == 0 && Tap(1, 1.0, false) == 0, "moved or held: no tap");
        const float u = 7.2f;
        CHECK(Swipe(100, 5, u) == button::DpadRight && Swipe(-100, 5, u) == button::DpadLeft, "horizontal swipes");
        CHECK(Swipe(5, 100, u) == button::DpadDown && Swipe(5, -100, u) == button::DpadUp, "vertical swipes");
        CHECK(Swipe(20, 10, u) == 0, "too short");
    }

    if (g_failures)
    {
        printf("touch_layout_test: %d failed\n", g_failures);
        return 1;
    }
    printf("touch_layout_test: ok\n");
    return 0;
}
