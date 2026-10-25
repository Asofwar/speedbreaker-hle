// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// What the open world race draws over the game's picture (ui/mp_overlay.cpp): text lines, a banner, and markers at world positions, with the camera to project them. The game's thread
// builds it once a frame from the server's race state (game/mp_adapter.cpp); the render thread takes a copy.
#pragma once
#include <string>
#include <vector>

namespace game
{
    struct MpHudMarker
    {
        float p[3] = {};        // world position in the body axes the server uses (x, y up, z)
        std::string label;
        int kind = 0;           // 0 a race's start marker, 1 the next checkpoint, 2 a later one
    };
    struct MpHudTag
    {
        float p[3] = {};        // the car's position, body axes as the markers
        std::string name;
    };
    struct MpHud
    {
        std::vector<MpHudTag> tags;       // the other players' names over their cars
        std::vector<std::string> chat;    // the latest chat lines, oldest first (bottom left)
        float projX = 0, projY = 0;       // the player's projection (game::PlayerProjection), 0: not known
        std::vector<std::string> lines;   // panel, top left
        std::string banner;               // big, top centre (countdown, "GO", ...)
        std::vector<MpHudMarker> markers;
        bool camValid = false;
        float eye[3] = {}, fwd[3] = {};   // the game's camera, in its own (tracker) axes: (body z, -body x, body y)
    };
    void MpHudPublish(MpHud&& hud);
    bool MpHudFetch(MpHud& out);   // false: nothing to draw
    bool MpHudActive();            // there is something to draw (any thread, cheap)
}
