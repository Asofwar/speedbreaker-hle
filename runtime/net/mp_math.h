// Orientation helpers of the multiplayer layer: the game's 3x3 matrix rows <-> a quaternion (x y z w). The two functions are exact inverses
// (tests/mp_net_test.cpp checks a round trip), and the convention is the usual one for a matrix whose rows are the body's axes in world space.
#pragma once
#include <cmath>

namespace mp
{
    inline void MatToQuat(const float m[3][3], float q[4])
    {
        const float t = m[0][0] + m[1][1] + m[2][2];
        if (t > 0)
        {
            const float r = std::sqrt(1.f + t) * 2;
            q[3] = 0.25f * r; q[0] = (m[1][2] - m[2][1]) / r; q[1] = (m[2][0] - m[0][2]) / r; q[2] = (m[0][1] - m[1][0]) / r;
        }
        else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
        {
            const float r = std::sqrt(1.f + m[0][0] - m[1][1] - m[2][2]) * 2;
            q[3] = (m[1][2] - m[2][1]) / r; q[0] = 0.25f * r; q[1] = (m[0][1] + m[1][0]) / r; q[2] = (m[0][2] + m[2][0]) / r;
        }
        else if (m[1][1] > m[2][2])
        {
            const float r = std::sqrt(1.f + m[1][1] - m[0][0] - m[2][2]) * 2;
            q[3] = (m[2][0] - m[0][2]) / r; q[0] = (m[0][1] + m[1][0]) / r; q[1] = 0.25f * r; q[2] = (m[1][2] + m[2][1]) / r;
        }
        else
        {
            const float r = std::sqrt(1.f + m[2][2] - m[0][0] - m[1][1]) * 2;
            q[3] = (m[0][1] - m[1][0]) / r; q[0] = (m[0][2] + m[2][0]) / r; q[1] = (m[1][2] + m[2][1]) / r; q[2] = 0.25f * r;
        }
    }

    inline void QuatToMat(const float q[4], float m[3][3])
    {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y + z * w);     m[0][2] = 2 * (x * z - y * w);
        m[1][0] = 2 * (x * y - z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z + x * w);
        m[2][0] = 2 * (x * z + y * w);     m[2][1] = 2 * (y * z - x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
    }
}
