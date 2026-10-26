// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Xenos -> Vulkan renderer (Phase 5, milestone 3). Runs on the command
// processor thread: draws and resolves are recorded as the PM4 stream is
// executed and submitted at synchronisation points (Flush).
//
//   - Guest physical memory is imported as one storage buffer
//     (VK_EXT_external_memory_host), so vertex fetches and resolves read and
//     write guest memory directly. Shaders come from gpu/shader_translator.
//   - EDRAM is modelled as host render targets keyed by EDRAM base, format and
//     pitch. A resolve (a draw in copy mode) converts a rectangle of one into
//     guest memory as a tiled texture, through a compute shader, and applies
//     the requested clears.
//   - Synchronisation: work is submitted without waiting (up to 8 command
//     buffers in flight). Fence and memory writes, scratch registers and the
//     read pointer are deferred until the GPU has finished the work before
//     them (QueueWrite); interrupts and swaps wait for the GPU (Flush).
//
// State translation follows Xenia (BSD): draw_util.cc (viewport, scissor,
// resolve rectangles) and the Vulkan backend's pipeline cache.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace gpu::renderer
{
    // `regs` is the command processor's register file (host order). Returns
    // false (and every other call is then a no-op) when running headless.
    bool Initialize(const uint32_t* regs);
    bool Enabled();

    // NFSMW_LOG_COVERS=1 (gpu/covers.h): the lines logged so far (each a new
    // place).
    uint64_t CoverHits();

    // IM_LOAD / IM_LOAD_IMMEDIATE: `words` in guest (big-endian) order.
    void LoadShader(uint32_t type, const uint32_t* words, uint32_t dwords);

    // DRAW_INDX / DRAW_INDX_2 (VGT_DRAW_INITIATOR, and VGT_DMA_BASE/SIZE for
    // indexed draws). In copy mode this is a resolve.
    void Draw(uint32_t initiator, uint32_t dmaBase, uint32_t dmaSize);

    // Submit recorded work without waiting.
    void Submit();

    // Submit only if the GPU has nothing in flight (keeps it fed while
    // letting consecutive batches share one submission otherwise).
    void SubmitIfIdle();
    // Idle command processor: submit if the GPU queue is shallow or the
    // recording has aged (NFSMW_SUBMIT_DEPTH, NFSMW_SUBMIT_AGE_US).
    void SubmitIfDue();
    // Command processor: when SubmitIfDue will submit the recording by its
    // age alone (time_point::max(): nothing recorded). The other reason, a
    // shallow queue, comes with a completion, which rings its doorbell.
    std::chrono::steady_clock::time_point SubmitDueAt();

    // Submit recorded work and wait until the GPU has finished everything.
    void Flush();

    // A CPU-visible effect of the command stream (a fence or memory write, the
    // read pointer): runs on the completion thread once the GPU has finished
    // the submission being recorded (or the last one in flight; immediately
    // when idle). Whoever then waits on the CPU must Submit() first.
    void QueueWrite(std::function<void()> write);

    // EVENT_WRITE_ZPD at `address` (RB_SAMPLE_COUNT_ADDR when it runs), on
    // the command processor thread. NFSMW_OCCLUSION=1 (the presenter
    // decides: VulkanContext::occlusionCounting) counts the game's occlusion
    // queries on the GPU: a BEGIN opens a bracket and returns false (the
    // caller stores its zeros, as without counting), an END closes it and
    // returns true (its report is stored at GPU completion with the samples
    // counted in between: gpu/zpd_report.h). False when not counting.
    bool ZpassDone(uint32_t address);
    // Any thread (a capture, `what` names it): with NFSMW_OCCLUSION=1, the
    // latest END count stored per report address. Nothing otherwise.
    void LogOcclusionCounts(const char* what);

    // XE_SWAP: convert the front buffer (its texture fetch constant) into an
    // image on the GPU and hand it to the presenter. False when headless.
    // `between`: an in-between picture's fraction of the step (game/hfr.h; 0:
    // a real picture), which gets an image of its own.
    // `shift`: a real picture shown that much of a vblank early (a step drawn
    // only between, its own picture's fraction minus 1; 0 otherwise).
    // `target`: when to show it (video::SubmitFrontImage's).
    bool PresentFrontBuffer(const uint32_t fetch[6], uint32_t width, uint32_t height, float between = 0.0f, float shift = 0.0f,
        int64_t target = 0);

    // The timedemo's shots (NFSMW_BENCH_SHOTS, debug/bench.h): copy the image
    // the last PresentFrontBuffer wrote (the game's frame as the presenter
    // gets it, before its post-processing and scaling) into memory the CPU
    // reads, without waiting for the GPU. Command processor thread, right
    // after that swap. `done` runs on a thread of its own once the copy has
    // landed: RGBA8, top row first, rows width * 4 bytes apart, valid only
    // during the call; or with null pixels when there was no image (headless,
    // NFSMW_DUMP_FRAMES) or no memory for the copy.
    using FrontPixels = std::function<void(const uint8_t* rgba, uint32_t width, uint32_t height)>;
    void ReadBackFrontImage(FrontPixels done);
    // The next frame's picture as the game's final grade leaves it, before
    // its HUD, its menus and the pause screen are drawn over it (the photo:
    // ui/photo.cpp), at internal resolution; `done` as above, (nullptr, 0,
    // 0) when the frame had no final grade (a menu, a movie) within a
    // second. Any thread.
    void ReadBackScenePhoto(FrontPixels done);
    // The latest frames' scene was anti-aliased before its HUD (Settings >
    // Graphics > Keep HUD Sharp): the presenter's own pass is left out. Any
    // thread.
    bool SceneAntiAliased();

    // A car's skin, its paint and vinyl (docs/MULTIPLAYER.md, "Same paint"): a 1024x1024 8_8_8_8 texture the car body shaders sample, as a recent draw used
    // it. view is the car's place in view space as that draw's vertex shader had it (the w of c5..c7: x right, y up, z ahead); the multiplayer layer matches
    // it to the cars it knows (game/mp_adapter.cpp). Any thread.
    struct CarSkin
    {
        uint32_t address = 0;      // guest physical
        uint32_t fetch1 = 0, fetch2 = 0;  // the texture fetch's address/format and size words
        uint32_t mipAddress = 0;   // 0: no mips
        float view[3] = {};
        uint64_t frame = 0;        // when it was last drawn
    };
    std::vector<CarSkin> CarSkins();
    uint64_t Frame();

    // After each swap (NFSMW_PASS_PROFILE=<frame> prints that frame's passes).
    void OnSwap(uint64_t frame);

    // Counters for the periodic log line.
    struct Stats
    {
        uint64_t draws, skipped, resolves, submits, pipelines, uploads, gpuBusyUs, passes, constantUploads, gpuExecUs, tileSkips, snapshotBytes, shadowWaits, shaderCacheHits, pipelineSkips, lateFrames;
        uint64_t snapshotHits, snapshotFallbacks;  // CP_OPT_SNAPSHOT_CACHE: ranges served from the cache; copies into the slot for want of room
        uint64_t narrowedRanges;  // HFR M3: large vertex ranges copied or guarded as the draw's vertices only
    };
    Stats GetStats();
    // The scaled data path (internal resolution above 1x), totals so far:
    // scaled texture loads, bytes the scaled untiles wrote (textures and
    // the front buffer), samples scaled resolves wrote, and the scaled
    // memory's slot allocations, evictions and ranges found stale (1x).
    struct ScaledStats
    {
        uint64_t loads, untiledBytes, resolvedBytes, allocations, evictions, fallbacks;
        uint64_t hosted;  // scaled resolves that also stored into their scaled texture (no load needed)
    };
    ScaledStats GetScaledStats();
    // Device memory held now, in bytes: the renderer's render targets,
    // cached textures, scaled resolve memory and front images, and the
    // device's local heaps as it counts this process's use and budget
    // (VK_EXT_memory_budget; 0 without it). Command processor thread (it
    // walks the caches): the [perf] line, to see that live setting changes
    // give back what they replace.
    //   Image memory (renderer.cpp): the blocks images are bound into, the
    // bytes of images in them, the spare blocks, and the images with an
    // allocation of their own; totals so far of images made and their time,
    // of vkAllocateMemory calls on this thread for images and blocks and
    // their time, of blocks taken from the spares, blocks allocated here
    // for want of one, blocks freed, and blocks the memory thread made.
    //   The texture cache's limits (gpu/texture_budget.h): totals so far of
    // textures evicted and their bytes, and of image allocations that
    // failed for want of memory and asked the cache for room. (Evicted
    // textures still count in `textures` until the GPU is done with them.)
    struct MemoryStats
    {
        uint64_t targets, textures, scaled, front, deviceUsed, deviceBudget;
        uint32_t targetCount, textureCount;
        uint64_t blockBytes, rangeBytes, spareBytes, ownBytes;
        uint32_t blocks, ranges, spares, own;
        uint64_t images, imageNs, allocations, allocationNs, fromSpare, blocksHere, blocksFreed, spareBlocks, spareNs;
        uint64_t textureEvictions, textureEvictedBytes, textureReclaims;
    };
    MemoryStats GetMemoryStats();
    // Per-frame averages over the last 30 swaps, for the performance overlay
    // (published by the command processor).
    struct PerfNumbers
    {
        float cpMs = 0, gpuMs = 0, draws = 0;
    };
    PerfNumbers GetPerfNumbers();
    void PublishPerfNumbers(const PerfNumbers& numbers);
    // At exit: persist caches.
    void Shutdown();
    // GLSL to SPIR-V (glslang, through the shader disk cache): stage 0
    // vertex, 1 fragment, 2 compute. Empty on failure, with `log` set.
    std::vector<uint32_t> CompileShader(const std::string& glsl, int stage, std::string& log);
    // Once per swap (the command processor's swap-to-swap time, and the
    // guest vblanks since the last swap): log where the frame's time went if
    // it was late.
    void HitchReport(uint64_t frame, double frameMs, uint32_t vblanks);

    // Set by the command processor when any shader constant register (float,
    // fetch, bool, loop) is written; the next draw uploads constants again.
    inline std::atomic<bool> g_constantsDirty{ true };
    // CPU-visible writes of the command stream (fences, scratch registers,
    // interrupts) happen when the command processor reaches them, as in
    // Xenia; the write-watch GPU guard keeps the guest from reusing memory
    // the GPU still has to access. NFSMW_EAGER_WRITES=0 defers them to GPU
    // completion instead (one frame in flight: ~45 fps in race on the M1 Pro).
    inline const bool g_eagerWrites = [] { const char* v = std::getenv("NFSMW_EAGER_WRITES"); return !v || v[0] != '0'; }();

    // Command-processor fast paths, all on by default; NFSMW_CP_OPT=<mask>
    // keeps only the given ones (0: the plain paths, for A/B checks).
    enum : uint32_t
    {
        CP_OPT_BULK_REGISTERS = 1,   // runs of plain register writes copied at once
        CP_OPT_FAST_INDICES = 2,     // indices converted straight into the ring (16-bit kept)
        CP_OPT_TEXTURE_EPOCH = 4,    // texture validity rescanned only after some page was written
        CP_OPT_PACKED_CONSTANTS = 8, // shaders read only their used constants, packed (shader_translator)
        CP_OPT_SKIP_REDUNDANT = 16,  // Vulkan state commands skipped when the value is already set
        CP_OPT_TEXTURE_CACHE = 32,   // texture lookups kept per fetch slot (NFSMW_TEXTURE_CACHE=0)
        CP_OPT_SHADER_CACHE = 64,    // IM_LOAD shaders found by address (NFSMW_SHADER_CACHE=0)
        CP_OPT_VERTEX_SPANS = 128,   // vertex snapshots copy only what the draw's indices reach (gpu/vertex_snapshot.h)
        CP_OPT_DYNAMIC_STATE = 256,  // cull mode, front face, depth and stencil tests set per draw, not in the pipeline
                                     // (where the device has VK_EXT_extended_dynamic_state: NFSMW_DYNAMIC_STATE, presenter)
        CP_OPT_KEEP_BINDINGS = 512,  // the ring stays bound as the index buffer (firstIndex picks a draw's indices),
                                     // compute work leaves the graphics bindings bound, push constants pushed from the
                                     // first changed row
        CP_OPT_TEXTURE_MEMO = 1024,  // a draw's textures are the draw before's, not looked up, while nothing they
                                     // depend on changed (renderer.cpp, TextureDrawMemo; needs CP_OPT_TEXTURE_CACHE)
        CP_OPT_SNAPSHOT_CACHE = 2048, // vertex snapshots kept across draws while their guest bytes stay unchanged
                                     // (renderer.cpp, SnapshotEntry). Off by default: measured on a Steam Deck at
                                     // 5 W (NFSMW_CP_OPT=0xffffefff vs the default, 3 pairs), the compares cost the
                                     // command processor 2.4 ms a frame more than the copies did and the GPU took
                                     // 0.8 ms more a frame reading the scattered copies.
        CP_OPT_INDEX_CACHE = 4096,   // converted indices kept the same way (renderer.cpp, CachedIndices); off, as above
        CP_OPT_NO_CONSTANT_MIRROR = 8192, // shader constants written in runs skip the guest-visible register
                                     // mirror (command_processor.cpp, WriteRegistersBulk)
    };
    inline const uint32_t g_cpOpt = [] {
        const char* v = std::getenv("NFSMW_CP_OPT");
        return v ? uint32_t(strtoul(v, nullptr, 0)) : ~0u & ~uint32_t(CP_OPT_SNAPSHOT_CACHE | CP_OPT_INDEX_CACHE);
    }();

    // Command-processor thread time, for the [perf] line: busy in ring
    // batches, i.e. not blocked (WAIT_REG_MEM, the frame throttle, GPU and
    // pipeline waits, a suspension: g_cpWaitNs; by kind for hitch reports).
    enum CpWait : uint32_t
    {
        CP_WAIT_REGMEM,    // WAIT_REG_MEM (the game's stores, or the GPU's)
        CP_WAIT_THROTTLE,  // the previous frame's GPU work (one frame in flight)
        CP_WAIT_SHADOW,    // a submission still reading pages being synced (shadow mode)
        CP_WAIT_FLUSH,     // all submitted work (Flush)
        CP_WAIT_SLOT,      // a free command buffer
        CP_WAIT_PIPELINE,  // an asynchronous pipeline compile
        CP_WAIT_SUSPENDED, // the game is suspended (iOS: the app isn't active; guest time stands still)
        CP_WAIT_KINDS
    };
    inline uint64_t g_cpBusyNs = 0, g_cpWaitNs = 0, g_cpWaitKindNs[CP_WAIT_KINDS] = {};
    inline thread_local bool t_commandProcessor = false;
    struct CpWaitTimer
    {
        CpWait kind;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        explicit CpWaitTimer(CpWait k) : kind(k) {}
        ~CpWaitTimer()
        {
            if (!t_commandProcessor)
                return;
            uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            g_cpWaitNs += ns;
            g_cpWaitKindNs[kind] += ns;
        }
    };
}
