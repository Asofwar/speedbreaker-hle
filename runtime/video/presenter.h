// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Window and presentation (Phase 5, milestone 1). An SDL3 window with a
// Vulkan swapchain (MoltenVK on macOS). Each XE_SWAP hands over the game's
// front buffer (a texture in guest physical memory); it is untiled and
// converted on the CPU and scaled into the window.
#pragma once
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <vector>

#ifdef __ANDROID__
#include <volk.h>  // Vulkan through function pointers (runtime/CMakeLists.txt, VK_NO_PROTOTYPES)
#else
#include <vulkan/vulkan.h>
#endif

namespace video
{
    // The anti-aliasing pass's compute shader (its push constants: size,
    // subpixel, bandOn, band, uvScale, copyOnly, pad: 48 bytes), for the
    // renderer's own pass on the scene (gpu/renderer.cpp, RunSceneAa).
    const char* AntiAliasingGlsl();

    // NFSMW_LOG_COVERS=1 (gpu/covers.h): the picture lines logged so far
    // (each a new box or layer).
    uint64_t PictureCoverHits();

    // Main thread only (macOS requires windows there). Returns false if no
    // window could be created; the game then runs headless.
    bool Initialize();

    // Any thread (the command processor): the front buffer to show next.
    // `fetch` is its texture fetch constant (6 dwords, host order).
    void SubmitFrontBuffer(const uint32_t fetch[6], uint32_t width, uint32_t height);

    // Any thread (the renderer): present this image (RGBA8, VK_IMAGE_LAYOUT_GENERAL),
    // written by a queue submission made before this call. `aspectWidth` x
    // `aspectHeight`: the frame's shape when the image is at another
    // resolution (internal resolution scaling; 0: the image's own).
    // imageWidth x imageHeight: the image's own size when it is larger than
    // the frame (NFSMW_RESOLVE_PRESENT=2 hands over a render target's
    // image, taller than the frame): the frame is its top-left part.
    // `between`: an in-between picture (game/hfr.h), its fraction of the world
    // step; shown at that fraction of the guest vblank after the one before
    // it, ahead of the real picture (0: a real picture).
    // `shift`: a real picture to be shown that much of a vblank before its
    // own time (game/hfr.h: a step drawn only between; below 0).
    // `target`: when to show it (steady ns; game/hfr.h on a display whose
    // rate isn't a whole multiple of 60 Hz: the refresh it was drawn for; 0:
    // the usual way).
    void SubmitFrontImage(VkImage image, uint32_t width, uint32_t height, uint32_t aspectWidth = 0, uint32_t aspectHeight = 0,
        float between = 0.0f, float shift = 0.0f, int64_t target = 0, uint32_t imageWidth = 0, uint32_t imageHeight = 0);

    // Any thread (the renderer), before it records a write of one of its
    // front images: the index of the one of `images` (a ring, `count` of
    // them) to write (gpu/front_ring.h). Not the presenter's (it may not
    // have queued its read of that frame yet, or draws it again) while
    // another is free; when only the waiting frame's is, that frame is taken
    // back here (dropped: counted and logged). Until the renderer hands the
    // image over with SubmitFrontImage the presenter can't take it, so its
    // write, queued before that, is never read in place of another frame.
    int ClaimFrontImage(const VkImage* images, int count);

    // Any thread: a display at a rate that isn't a whole multiple of 60 Hz
    // (90, 144, 165) whose presents land on a fixed refresh grid (the refresh
    // notice's grid check: video/refresh_notice.h), where HFR draws a picture
    // for each refresh (game/hfr.cpp): its refresh period and a refresh's
    // time (steady ns). False elsewhere (no grid measured, a multiple of 60
    // Hz, a Mac). NFSMW_HFR_GRID_HZ=<hz> makes up a grid at that rate on any
    // display (tests).
    bool HfrGrid(double& periodNs, int64_t& gridNs);

    // Any thread: pictures the display can show evenly per guest vblank (a
    // world step) now: the refreshes a vblank is when the vblank is locked
    // to a display at a multiple of 60 Hz and new frames are held for their
    // refresh (2 at 120 Hz, at most 4), 2 on a Mac whose frames are shown at
    // their vblank's time on a 120 Hz display (video/present_schedule.h), else
    // 1 (game/hfr.h draws no in-between pictures).
    int HfrPicturesPerStep();
    // Settings > Display > Smooth Motion is offered (the menu shows it): a
    // Mac's 120 Hz screen whose pictures the presenter can time
    // (VK_GOOGLE_display_timing, as ProMotion's). Not yet elsewhere: HFR is
    // untested on real 90-240 Hz screens there (NFSMW_HFR=1 still turns it on).
    bool HfrOffered();

    // Wait until the queue is idle (resource replacement).
    void WaitGpuIdle();

    // Any thread: a Vulkan call (`what` names it and its caller) returned
    // VK_ERROR_DEVICE_LOST. The GPU or its driver gave up on the game's work
    // (a GPU timeout or reset, a driver fault; on MoltenVK any Metal command
    // buffer error) and nothing can be drawn or waited for on it again: every
    // fence wait returns at once from then on, and v0.1.4's completion thread
    // spun on that at 100% CPU while the game hung without a word. Now the
    // first thread to see it logs it and writes a crash report
    // (report::WriteCrashReport, as a crash does, which the next launch
    // mentions); the main thread tells the player in a message box (none in
    // SteamOS Game Mode, where nothing could show it) and the game exits
    // (code 4). Never returns. Call it with no lock held that the main thread
    // may take: a thread other than the main one waits here until the main
    // thread has exited the game (10 s at most; then it exits itself).
    [[noreturn]] void DeviceLost(const char* what);

    // Since launch, for the [perf] line: game frames replaced here before
    // they were shown (dropped), refreshes a game frame stayed on screen past
    // the vblanks the game gave it (repeated; needs present timing), and game
    // frames seen on screen (shown; likewise).
    struct PresentStats
    {
        uint64_t dropped, repeated, shown;
    };
    PresentStats GetPresentStats();

    // The guest vblank's clock (gpu/command_processor.cpp's Vsync thread),
    // locked to the display when presents are timed (video/vblank_lock.h).
    class VblankLock& GuestVblank();
    // Vsync thread: a guest vblank fired at `t` (ns, steady clock).
    void NoteGuestVblank(int64_t t);
    // Any thread: guest vblanks so far, and when the last one fired.
    uint64_t GuestVblanks();
    // Lock periods (1/60 s, VblankLock::PeriodNs: one refresh at 60 Hz, two
    // at 120 Hz) per guest vblank: 1, or 2 at Frame Rate 30 (the game then
    // shows a frame every other period, and each guest vblank is two world
    // steps: game/frame_time.cpp).
    int GuestVblankDivider();
    int64_t LastGuestVblank();

    // NFSMW_GPU_TIMING=1: GPU time of the post-processing of the game's
    // frames (AA, then the scale into the swapchain image with the UI; that
    // part also holds any wait for the image), totals so far.
    struct PostTiming
    {
        uint64_t frames, aaNs, totalNs;
    };
    PostTiming GetPostTiming();

    // Any thread: `image` (given to SubmitFrontImage) is about to be
    // destroyed. Returns once the presenter holds no reference it could still
    // record; the caller then waits for the GPU (WaitGpuIdle) and destroys it.
    void ReleaseFrontImage(VkImage image);

    // Main thread: handle window events and present the latest front buffer.
    // Returns false when the user closes the window.
    bool RunFrame();

    void Shutdown();

    // Shared with the renderer (gpu/renderer), which records on the command
    // processor thread. Every vkQueueSubmit/vkQueuePresentKHR holds queueMutex.
    struct VulkanContext
    {
        VkInstance instance;
        VkPhysicalDevice physical;
        VkDevice device;
        uint32_t queueFamily;
        VkQueue queue;
        std::mutex* queueMutex;
        PFN_vkGetMemoryHostPointerPropertiesEXT getMemoryHostPointerProperties;  // nullptr: no host-memory import
        PFN_vkCmdBeginRenderingKHR cmdBeginRendering;
        PFN_vkCmdEndRenderingKHR cmdEndRendering;
        // NFSMW_SHADING_RATE=<w>x<h> (an experiment: what reduced shading,
        // as foveation would do at the edges, could save at most): the game's
        // draws shade once per w x h pixels. 0x0: off or unsupported.
        uint32_t shadingRateW = 0, shadingRateH = 0;
        // NFSMW_OCCLUSION=1 on a device with precise occlusion queries: the
        // game's occlusion queries (the sun glare) are counted on the GPU
        // (gpu/renderer.cpp). resetQueryPool: host query reset (nullptr: the
        // renderer resets them in its command buffers).
        bool occlusionCounting = false;
        PFN_vkResetQueryPool resetQueryPool = nullptr;
        // VK_KHR_sampler_mirror_clamp_to_edge is enabled (where the device
        // has it): the renderer's samplers mirror once, as Xenos's mirror
        // clamp modes do (gpu/renderer.cpp, GetSampler).
        bool mirrorClampToEdge = false;
#ifdef VK_EXT_depth_bias_control
        // VK_EXT_depth_bias_control is enabled with its FORCE_UNORM
        // representation (where the device has it; not MoltenVK): the
        // renderer gives a D24S8 target's depth bias in unorm24 steps on the
        // float32 depth buffer through this (gpu/renderer.cpp, the polygon
        // offset). nullptr: vkCmdSetDepthBias, the format's steps, as before.
        // depthBiasExact: each step exactly 2^-24 (not up to twice that).
        PFN_vkCmdSetDepthBias2EXT cmdSetDepthBias2 = nullptr;
        bool depthBiasExact = false;
#endif
#ifdef VK_EXT_extended_dynamic_state
        // VK_EXT_extended_dynamic_state is enabled (where the device has it;
        // NFSMW_DYNAMIC_STATE, presenter.cpp): the renderer sets cull mode,
        // front face and the depth and stencil tests per draw through these
        // rather than baking them into its pipelines (gpu/renderer.cpp,
        // CP_OPT_DYNAMIC_STATE). All set or all nullptr (in the pipelines,
        // as before).
        PFN_vkCmdSetCullModeEXT cmdSetCullMode = nullptr;
        PFN_vkCmdSetFrontFaceEXT cmdSetFrontFace = nullptr;
        PFN_vkCmdSetDepthTestEnableEXT cmdSetDepthTestEnable = nullptr;
        PFN_vkCmdSetDepthWriteEnableEXT cmdSetDepthWriteEnable = nullptr;
        PFN_vkCmdSetDepthCompareOpEXT cmdSetDepthCompareOp = nullptr;
        PFN_vkCmdSetStencilTestEnableEXT cmdSetStencilTestEnable = nullptr;
        PFN_vkCmdSetStencilOpEXT cmdSetStencilOp = nullptr;
#endif
        // NFSMW_PASS_STATS=1 on a device with pipeline statistics queries:
        // the pass profile (NFSMW_PASS_PROFILE) counts each pass's vertex and
        // fragment shader invocations (gpu/renderer.cpp).
        bool pipelineStatistics = false;
        // NFSMW_PIPELINE_STATS=1 on a device with
        // VK_KHR_pipeline_executable_properties: each pipeline the renderer
        // makes logs its shaders' compiler statistics (registers, spills,
        // instructions; gpu/renderer.cpp).
        bool pipelineExecutableStats = false;
        // Block-compressed (BC1-BC5) images: every desktop GPU and Apple's;
        // not a phone's (Adreno, Mali), whose DXT textures the renderer
        // decodes as it loads them (gpu/renderer.cpp, GetTextureFormat).
        bool textureCompressionBC = true;
#ifdef _WIN32
        // Windows on an Intel integrated GPU: the defaults measured on one
        // (presenter.cpp, IntegratedGpuDefaults). The renderer's part:
        // fewer and lighter passes, and Auto internal resolution at 1x.
        bool integratedGpuDefaults = false;
#endif
    };

    // nullptr when running headless.
    const VulkanContext* GetVulkan();

    // Any thread, holding queueMutex (`queueLock`), before a vkQueueSubmit or
    // vkQueuePresentKHR: waits while the app is in the background. iOS lets
    // no GPU work start then (Metal fails the command buffer, and MoltenVK
    // then loses the device for good), so the game's GPU work stops there
    // until the app is active again.
    void WaitForeground(std::unique_lock<std::mutex>& queueLock);

    // The whole game stops while the app isn't the active one (iOS: from
    // "will resign active" to "did become active", SDL's app events), and
    // carries on where it stopped. SuspendGame stops every guest clock
    // (cpu/guest_time.h), then lets no GPU work start and drains the queue,
    // then pauses the audio output. ResumeGame lets GPU work start, then
    // guest time move on, then the audio play. Idempotent: UIKit reports
    // each transition twice. The main thread (the app events come there,
    // and RunFrame stops and restarts the input), or NFSMW_TEST_SUSPEND's
    // thread when headless (no main loop): never both.
    void SuspendGame();
    void ResumeGame();
    // The same thread: 2 s after a resume, logs the clocks, vblanks and
    // audio frames since, against guest time. RunFrame calls it each frame.
    void SuspendAudit();
    // NFSMW_TEST_SUSPEND (report/watchdog.cpp), any thread: SDL app event
    // `type` (SDL_EVENT_WILL_ENTER_BACKGROUND...), for the main thread to
    // hand to the same handler SDL would, twice as UIKit does, first thing
    // in its next frame.
    void TestAppEvent(uint32_t type);

    // Any thread: the window has the keyboard focus (true when headless).
    bool WindowFocused();

    // Any thread: the window can be seen (not minimized, hidden or covered;
    // true when headless). The hang watchdog doesn't count time without it
    // (nor time suspended, which it reads from guesttime itself).
    bool WindowVisible();

    // A game frame as the window shows it, without the UI over it and
    // without letterbox bars: RGB8, top row first.
    struct CapturedFrame
    {
        std::vector<uint8_t> rgb;
        uint32_t width = 0, height = 0;
    };
    // Any thread (a bug report): the next frame the main thread draws, once
    // drawn. Empty when there is no game frame to capture (headless, before
    // the game's first frame, a swapchain that can't be read back).
    std::future<CapturedFrame> CaptureFrame();

    // The timedemo's shots of the picture as shown (debug/bench.cpp,
    // NFSMW_BENCH_SHOTS_SHOWN): any thread, before the call that hands over
    // the frame to capture (SubmitFrontImage or SubmitFrontBuffer: the
    // command processor, ahead of the shot's swap). That frame's picture as
    // the window shows it, after all the post-processing (anti-aliasing,
    // scaling, FSR 1, the tone), without the UI over it and without
    // letterbox bars, as CaptureFrame's, goes to `done` on a thread of its
    // own once the GPU has drawn it; nothing waits for the copy. Empty if
    // the frame is never drawn (replaced by a newer one before the main
    // thread took it), or there is no window or readable swapchain.
    void CaptureNextFrame(std::function<void(CapturedFrame)> done);

    // SteamOS Game Mode (gamescope) and iOS: the window is always fullscreen
    // there.
    bool FullscreenLocked();

    // Main thread: whether the window is fullscreen now, and whether that
    // differs from Settings > Display > Fullscreen because the system changed
    // it this session (the green button; video/fullscreen_policy.h).
    bool WindowFullscreen();
    bool FullscreenWindowDiffers();

    // SteamOS Game Mode (gamescope; NFSMW_GAME_MODE=1|0 says otherwise), as
    // detected when the window was made. No desktop is on screen there, so a
    // system file dialog can't be seen either.
    bool GameMode();

    // NFSMW_PRESENT_SCALE (iOS): the present scale it sets for this run, over
    // Settings > Display > Screen Resolution; 0 when it isn't set.
    float PresentScaleOverride();

    // The area of the swapchain a 1280x720 frame covers now (in Fill mode the
    // whole of a wider or narrower screen, or its 4:3 box when it is
    // narrower than that; else its 16:9 box); 0x0 without a window.
    void FrameRegion(uint32_t& width, uint32_t& height);
}
