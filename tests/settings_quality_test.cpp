// SpeedBreaker tests. GPL-3.0-or-later (see COPYING).
//
// Settings > Graphics > Quality (user/settings.cpp, linked as the game links
// it): a preset sets the options it covers; changing one of them by hand
// makes it Custom; Custom changes nothing; Reset of the preset alone touches
// nothing else.
//
// Build by hand (from the repo root):
//   c++ -std=c++20 -O2 -UNDEBUG -I runtime tests/settings_quality_test.cpp runtime/user/settings.cpp -o build/settings_quality_test
//   build/settings_quality_test
#include <user/settings.h>

#include <cstdio>

namespace
{
    int s_failures = 0;

#define CHECK(cond, ...)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            s_failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n");                                          \
        }                                                                   \
    } while (0)

    using settings::Id;
    using Preset = settings::GraphicsQualityPreset;

    double Stored(Id id) { return settings::GetStoredValue(id); }
}

int main()
{
    CHECK(Stored(Id::GraphicsQuality) == double(Preset::Custom), "the default is Custom: %g", Stored(Id::GraphicsQuality));

    settings::Set(Id::GraphicsQuality, double(Preset::Ultra));
    CHECK(Stored(Id::ShadowScale) == 32, "Ultra: shadow map scale 4x, %g", Stored(Id::ShadowScale));
    CHECK(Stored(Id::ShadowFiltering) == double(settings::ShadowFilteringMode::Pcss), "Ultra: PCSS, %g", Stored(Id::ShadowFiltering));
    CHECK(Stored(Id::AmbientOcclusion) > 0.7, "Ultra: ambient occlusion, %g", Stored(Id::AmbientOcclusion));
    CHECK(Stored(Id::GraphicsQuality) == double(Preset::Ultra), "it stays Ultra after setting its options: %g", Stored(Id::GraphicsQuality));

    settings::Set(Id::GraphicsQuality, double(Preset::Performance));
    CHECK(Stored(Id::ShadowResolution) == 800, "Performance: an 800 shadow map, %g", Stored(Id::ShadowResolution));
    CHECK(Stored(Id::ShadowScale) == 8, "Performance: shadow map scale 1x, %g", Stored(Id::ShadowScale));
    CHECK(Stored(Id::ReflectionRefresh) == 3, "Performance: reflections every 3rd frame, %g", Stored(Id::ReflectionRefresh));
    CHECK(Stored(Id::AmbientOcclusion) == 0, "Performance: no ambient occlusion, %g", Stored(Id::AmbientOcclusion));

    // A covered option set to the preset's own value keeps the preset; another makes it Custom.
    settings::Set(Id::ReflectionRefresh, 3);
    CHECK(Stored(Id::GraphicsQuality) == double(Preset::Performance), "the preset's own value keeps it: %g", Stored(Id::GraphicsQuality));
    settings::Set(Id::MotionBlur, 0.4);
    CHECK(Stored(Id::GraphicsQuality) == double(Preset::Custom), "a hand change makes it Custom: %g", Stored(Id::GraphicsQuality));
    CHECK(Stored(Id::MotionBlur) > 0.39 && Stored(Id::MotionBlur) < 0.41, "the hand change stays: %g", Stored(Id::MotionBlur));
    CHECK(Stored(Id::ShadowResolution) == 800, "the others stay: %g", Stored(Id::ShadowResolution));

    // An option the presets don't cover leaves the preset alone.
    settings::Set(Id::GraphicsQuality, double(Preset::High));
    settings::Set(Id::Sharpening, 0.5);
    CHECK(Stored(Id::GraphicsQuality) == double(Preset::High), "Sharpening isn't covered: %g", Stored(Id::GraphicsQuality));

    // Custom (and Reset to it) changes nothing.
    settings::Reset(Id::GraphicsQuality);
    CHECK(Stored(Id::ShadowScale) == 16, "Custom keeps High's values: %g", Stored(Id::ShadowScale));

    if (s_failures)
        fprintf(stderr, "%d failure(s)\n", s_failures);
    else
        printf("settings_quality_test: ok\n");
    return s_failures ? 1 : 0;
}
