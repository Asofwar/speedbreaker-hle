// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
// See renderer.h. State translation follows Xenia (BSD): draw_util.cc and
// vulkan/vulkan_pipeline_cache.cc.
#include <stdafx.h>
#include "renderer.h"
#include "cpu_locks.h"
#include <cpu/guest_thread.h>
#include <cpu/host_cpu.h>
#include <cpu/guest_time.h>
#include <debug/timeline.h>
#include <unordered_set>
#include "block_ranges.h"
#include "cp_doorbell.h"
#include "draw_state.h"
#include "covers.h"
#include "edge_bars.h"
#include "front_ring.h"
#include "memory_plan.h"
#include "pipeline_cache_header.h"
#include "pipeline_keys.h"
#include "shader_translator.h"
#include "zpd_report.h"
#include "depth24_glsl.h"
#include "depth_only.h"
#include "poly_offset.h"
#include "render_scale.h"
#include "shared_memory_glsl.h"
#include "texture_budget.h"
#include "texture_formats.h"
#include "vertex_snapshot.h"
#include "split_views.h"
#include "xenos/registers.h"

#include <kernel/vmem.h>
#include <kernel/write_watch.h>
#include <video/presenter.h>
#include <video/picture_fit.h>
#include <video/scale_budget.h>
#include <game/hfr.h>
#include <user/paths.h>
#include <user/settings.h>
#include <game/cover_callers.h>
#include <game/split_screen.h>
#include <game/ultrawide.h>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#if __has_include(<glslang/build_info.h>)
#include <glslang/build_info.h>
#endif

#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <memory>
#include <unordered_map>

namespace gpu::renderer
{
    namespace
    {
        using namespace xe::gpu;

        const video::VulkanContext* s_vk = nullptr;
        VkDevice s_dev = VK_NULL_HANDLE;
        const uint32_t* s_regs = nullptr;
        PFN_vkCmdPushDescriptorSetKHR s_pushDescriptorSet = nullptr;
        // CP_OPT_DYNAMIC_STATE where the device has VK_EXT_extended_dynamic_state
        // (the presenter's function pointers): cull mode, front face and the
        // depth and stencil tests are set per draw, not baked into pipelines.
        bool s_dynamicState = false;  // constant after Initialize
        Stats s_stats{};
        // NFSMW_GPU_EXP bitmask, for performance experiments: 1 skip draw
        // calls, 2 skip resolve copies, 4 skip texture loads, 8 bind 1x1
        // placeholders instead of textures, 32 record every draw with no
        // vertices (its state changes without its shading), 64 run the empty
        // depth-only pixel shader for every draw (no guest pixel shading),
        // 128 a 1x1 scissor for every draw (vertex shading without
        // rasterization), 256 no barrier at a pass's end (what the pass
        // structure's waits cost: the pictures may be wrong), 512 no colour
        // clears of targets in passes (their load ops load, no in-pass
        // clears), 1024 the same for depth and stencil.
        const uint32_t s_exp = [] { const char* v = std::getenv("NFSMW_GPU_EXP"); return v ? uint32_t(std::atoi(v)) : 0u; }();
        // Fewer render passes and texture reloads (NFSMW_PASS_MERGE,
        // NFSMW_CLEAR_FLUSH_RECT, NFSMW_DIRECT_CUBE, NFSMW_FUSE_PARTIAL): on
        // by default where passes cost most, the tile GPUs: under MoltenVK
        // (iPad race frame: 46 -> 20 passes; heavy scenes ~4 ms less GPU
        // time) and Turnip on arm64 Linux (Steam Frame: 37 -> 21 passes,
        // -0.3 ms GPU a busy frame); and on x86-64 Linux, measured on the
        // Steam Deck (below). Lighter passes (NFSMW_UNTILED_AREA,
        // NFSMW_PASS_BARRIER): Apple and x86-64 Linux. On the Frame the
        // smaller area cost +0.3 ms (Turnip chooses tiled or direct
        // rendering by the area) and the barrier changed nothing.
        // Steam Deck at a 5 W TDP limit, where its GPU (~300 MHz) bounds the
        // race (alternated runs, 1x): both with NFSMW_RESOLVE_DEFER 33-36 ->
        // 36-38 fps, GPU busy -1.4 to -2.4 ms a frame (fewer passes and the
        // deferral alone: about half of it); the pictures as before
        // (compare_shots.py --strict: 2 of 7 identical, the rest 3 levels
        // off at most, PSNR 80 dB and up). Each variable overrides.
#if defined(__APPLE__) || defined(__linux__)
        constexpr bool kFewerPasses = true;
#else
        constexpr bool kFewerPasses = false;
#endif
#if defined(__APPLE__) || (defined(__linux__) && defined(__x86_64__))
        constexpr bool kLighterPasses = true;
#else
        constexpr bool kLighterPasses = false;
#endif

        // Where a frame's time went, for hitch reports (renderer::HitchReport).
        struct HitchTimes
        {
            double shaderMs = 0, pipelineMs = 0, textureMs = 0, shadowMs = 0, imageMs = 0, submitMs = 0;
            uint32_t shaders = 0, pipelines = 0, textures = 0, images = 0, scaledTextures = 0;
            uint64_t textureBytes = 0;
        };
        HitchTimes s_hitch;
        struct HitchTimer
        {
            double& total;
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            explicit HitchTimer(double& t) : total(t) {}
            ~HitchTimer() { total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
        };

        float RegF(uint32_t index)
        {
            float f;
            memcpy(&f, &s_regs[index], 4);
            return f;
        }

        template<typename T>
        T Reg()
        {
            T t;
            t.value = s_regs[T::register_index];
            return t;
        }

        template<typename T>
        T RegAt(uint32_t index)
        {
            T t;
            t.value = s_regs[index];
            return t;
        }

        // A Vulkan call the renderer can't go on without: a failure is logged,
        // then abort() writes a crash report. A lost device instead takes the
        // presenter's way out (video::DeviceLost): logged and reported the
        // same, then the player is told and the game exits. Not with the
        // queue lock held (the main thread takes it to present).
        void Check(VkResult r, const char* what)
        {
            if (r == VK_ERROR_DEVICE_LOST)
                video::DeviceLost(what);
            if (r != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] %s failed: %d\n", what, int(r));
                abort();
            }
        }

        // Log a message the first time `key` is seen.
        void LogOnce(uint64_t key, const char* fmt, ...)
        {
            static std::unordered_map<uint64_t, bool> seen;
            if (seen[key])
                return;
            seen[key] = true;
            va_list args;
            va_start(args, fmt);
            fprintf(stderr, "[renderer] ");
            vfprintf(stderr, fmt, args);
            fprintf(stderr, "\n");
            va_end(args);
        }

        uint32_t GpuSwap(uint32_t v, uint32_t endian)
        {
            switch (endian & 3)
            {
            case 1: return ((v << 8) & 0xFF00FF00) | ((v >> 8) & 0x00FF00FF);
            case 2: return __builtin_bswap32(v);
            case 3: return (v >> 16) | (v << 16);
            default: return v;
            }
        }

        // ---------------------------------------------------------------
        // Memory

        VkPhysicalDeviceMemoryProperties s_memProps;
        bool s_memoryBudget = false;  // VK_EXT_memory_budget (the [perf] line's device memory)

        uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want)
        {
            for (uint32_t i = 0; i < s_memProps.memoryTypeCount; i++)
                if ((bits & (1u << i)) && (s_memProps.memoryTypes[i].propertyFlags & want) == want)
                    return i;
            for (uint32_t i = 0; i < s_memProps.memoryTypeCount; i++)
                if (bits & (1u << i))
                    return i;
            return 0;
        }

        // Guest physical memory (512 MB), imported without a copy.
        constexpr VkDeviceSize kSharedSize = 512ull << 20;
        VkBuffer s_shared = VK_NULL_HANDLE;
        VkDeviceMemory s_sharedMemory = VK_NULL_HANDLE;
        uint8_t* s_sharedHost = nullptr;
        // Devices that bind less of a storage buffer than guest memory
        // (maxStorageBufferRange: 128 MB on Turnip, the Steam Frame's Adreno,
        // where everything above the first 128 MB read as zeros: a black
        // picture) get it as four 128 MB parts: binding 0 and
        // kSharedPartBinding.. in every layout that has it, SPLIT_MEMORY in
        // the shaders (gpu/shared_memory_glsl.h). Elsewhere (RADV, MoltenVK:
        // 4 GB) one binding, and the shaders compile as before.
        // NFSMW_SPLIT_MEMORY=1|0 forces the choice.
        constexpr VkDeviceSize kSharedPart = 128ull << 20;
        constexpr uint32_t kSharedPartBinding = 13;
        bool s_splitMemory = false;
        VkDescriptorBufferInfo s_sharedInfo[4]{};  // the binding(s): [0] alone, or the four parts
        // The translated shaders' multiplication in the form Mesa folds into
        // fmulz (shader_translator.cpp, SB_FMULZ): NFSMW_FMULZ=0|1, by default
        // on with RADV. Set before the first shader is compiled.
        bool s_fmulz = false;

        void SetSharedInfo()
        {
            for (uint32_t k = 0; k < 4; k++)
                s_sharedInfo[k] = s_splitMemory ? VkDescriptorBufferInfo{ s_shared, k * kSharedPart, kSharedPart }
                                                : VkDescriptorBufferInfo{ s_shared, 0, kSharedSize };
        }

        // A layout's bindings, plus (split) guest memory's other three
        // parts: copies of its binding `shared`.
        std::vector<VkDescriptorSetLayoutBinding> WithSharedParts(const VkDescriptorSetLayoutBinding* b, uint32_t count,
            uint32_t shared = 0)
        {
            std::vector<VkDescriptorSetLayoutBinding> out(b, b + count);
            for (uint32_t k = 0; s_splitMemory && k < 3; k++)
            {
                out.push_back(b[shared]);
                out.back().binding = kSharedPartBinding + k;
            }
            return out;
        }

        // Appends the writes for guest memory's other three parts (split
        // only) after `count` writes; returns the new count. `w` has room
        // for 3 more.
        uint32_t AddSharedParts(VkWriteDescriptorSet* w, uint32_t count, VkDescriptorSet set = VK_NULL_HANDLE)
        {
            for (uint32_t k = 1; s_splitMemory && k < 4; k++)
                w[count++] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, kSharedPartBinding + k - 1, 0, 1,
                    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[k], nullptr };
            return count;
        }

        // A stand-in storage buffer for a binding a shader won't read this
        // time (no scaled memory): guest memory, as much as one binding takes.
        VkDescriptorBufferInfo DummyStorage(VkBuffer buffer)
        {
            return { buffer ? buffer : s_shared, 0, buffer || !s_splitMemory ? VK_WHOLE_SIZE : kSharedPart };
        }
        // Discrete GPUs whose CPU-mapped VRAM holds it, and drivers that can't
        // import the guest's memory (amdgpu imports only anonymous memory, and
        // guest physical memory is a shared mapping, mirrored three times;
        // Intel's Linux driver loses the device when the write watch protects
        // imported pages): the GPU gets its own copy, kept current page by
        // page from the write watch (writewatch::SyncShadow). Where it goes,
        // and when guest memory is imported instead: gpu/memory_plan.h.
        // NFSMW_SHARED_MEMORY=import|shadow overrides the choice.
        bool s_shadowMode = false;
        bool s_shadowInVram = false;  // (the "ready" line)
        std::unordered_map<uint64_t, uint64_t> s_shadowSynced;  // range -> writewatch::Current() at its last sync

        uint32_t LoadPhysical(uint32_t address)
        {
            uint32_t v;
            memcpy(&v, s_sharedHost + (address & 0x1FFFFFFC), 4);
            return v;  // host-order load of the guest's bytes (as the GPU reads them)
        }

        // Upload ring: draw constants, converted indices and texture staging.
        // One slot per in-flight submission, so a slot is reused only after
        // the GPU finished with it.
        constexpr uint32_t kSlots = 12;
        constexpr VkDeviceSize kSlotSize = 16ull << 20;
        // After the slots: the region vertex snapshots stay in across draws
        // (CP_OPT_SNAPSHOT_CACHE, see SnapshotEntry).
        constexpr VkDeviceSize kSnapshotCacheSize = 32ull << 20;
        constexpr VkDeviceSize kRingSize = kSlots * kSlotSize + kSnapshotCacheSize;
        VkBuffer s_ring = VK_NULL_HANDLE;
        VkDeviceMemory s_ringMemory = VK_NULL_HANDLE;
        uint8_t* s_ringHost = nullptr;
        VkDeviceSize s_ringOffset = 0;

        // XenosConstants (shader_translator.cpp): c[512], fetch[48], bools[2],
        // loops[8], drawInfo[4], each a vec4/uvec4.
        constexpr uint32_t kConstantsSize = (512 + 48 + 2 + 8 + 4) * 16;

        // Image memory. Images are bound at offsets into blocks of device
        // memory (64 MB; gpu/block_ranges.h keeps each block's free ranges),
        // one list per memory type, instead of an allocation each. On the
        // Deck vkAllocateMemory was most of a new texture's cost (40-220 us
        // in scripted runs, 380 us while the kernel was moving memory out of
        // VRAM, about 1 ms after a long free roam, where frames that streamed
        // textures spent a median 9.5 ms making images); a range in a block
        // is a few us. A block is allocated ahead of need, off the command
        // processor: the nfsmw-memory thread keeps an empty spare of each
        // memory type in use (0.1-0.2 ms a block there, unless the kernel has
        // memory to move), so the command processor takes the spare when a
        // block fills and the thread makes the next. Without a spare ready (the first block, or a
        // burst that outran the thread) the command processor allocates the
        // block itself, counted in the [perf] line. An image bigger than half
        // a block, one the driver wants in an allocation of its own
        // (VkMemoryDedicatedRequirements) and any image when a block can't be
        // had keep their own allocation, as before. A block whose images all
        // went is a spare too, freed by the thread after a few seconds unused
        // beside another. Images are destroyed only with the GPU idle, or
        // (textures the cache evicted) once the submission that could last
        // use them has completed (DestroyImage), so a range can be handed out
        // again at once.
        // NFSMW_IMAGE_POOL=0: an allocation per image (the old path);
        // NFSMW_IMAGE_BLOCK_MB: the block size (16-1024).
        struct ImageBlock
        {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            uint32_t type = 0;
            gpu::BlockRanges ranges;
        };

        struct Image
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;  // its own, or its block's
            VkImageView view = VK_NULL_HANDLE;
            VkDeviceSize bytes = 0;  // its allocation or range (the [perf] line's memory)
            ImageBlock* block = nullptr;  // bound at `offset` in this block, if any
            VkDeviceSize offset = 0;
        };

        const bool s_imagePool = [] { const char* v = std::getenv("NFSMW_IMAGE_POOL"); return !v || v[0] != '0'; }();
        const VkDeviceSize s_blockSize = [] {
            const char* v = std::getenv("NFSMW_IMAGE_BLOCK_MB");
            return VkDeviceSize(v ? std::clamp(std::atoi(v), 16, 1024) : 64) << 20;
        }();
        std::vector<std::unique_ptr<ImageBlock>> s_blocks;  // in use (command processor)
        // Empty blocks by memory type, the newest last, under s_spareMutex:
        // the one the nfsmw-memory thread keeps ready, and blocks whose images
        // all went. Those are kept kSpareKeep (a settings change drops
        // textures or targets and makes them again at once) and then freed
        // down to one.
        struct Spare
        {
            VkDeviceMemory memory;
            std::chrono::steady_clock::time_point since;
        };
        constexpr auto kSpareKeep = std::chrono::seconds(5);
        std::mutex s_spareMutex;
        std::condition_variable s_spareCv;
        std::vector<Spare> s_spares[VK_MAX_MEMORY_TYPES];
        uint32_t s_spareWanted = 0, s_spareFailed = 0;  // memory type bits
        // Totals for the [perf] line (command processor): images made and
        // the time it took, vkAllocateMemory calls for them (their own or a
        // block's) and the time those took, blocks taken from the spares,
        // blocks the spares didn't cover; under s_spareMutex, the memory
        // thread's: blocks it made and their time, blocks freed.
        struct ImageTotals
        {
            uint64_t images = 0, imageNs = 0, allocations = 0, allocationNs = 0, fromSpare = 0, blocksHere = 0;
            uint64_t spareBlocks = 0, spareNs = 0, blocksFreed = 0;
        };
        ImageTotals s_imageTotals;

        uint64_t SinceNs(std::chrono::steady_clock::time_point start)
        {
            return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        }

        VkDeviceMemory AllocateBlock(uint32_t type)
        {
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = s_blockSize;
            mai.memoryTypeIndex = type;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            return vkAllocateMemory(s_dev, &mai, nullptr, &memory) == VK_SUCCESS ? memory : VK_NULL_HANDLE;
        }

        // nfsmw-memory: makes a spare when one is wanted, and frees spares
        // beyond one once they have been empty kSpareKeep.
        void MemoryThread()
        {
            SetHostThreadName("nfsmw-memory");
            hostcpu::LeaveReservedCore();  // started by the command processor
            std::unique_lock lock(s_spareMutex);
            while (true)
            {
                auto now = std::chrono::steady_clock::now();
                auto next = now + std::chrono::hours(1);
                bool worked = false;
                for (uint32_t type = 0; type < VK_MAX_MEMORY_TYPES; type++)
                {
                    std::vector<Spare>& spares = s_spares[type];
                    if (spares.size() > 1 && now - spares.front().since >= kSpareKeep)
                    {
                        VkDeviceMemory memory = spares.front().memory;
                        spares.erase(spares.begin());
                        s_imageTotals.blocksFreed++;
                        lock.unlock();
                        vkFreeMemory(s_dev, memory, nullptr);
                        lock.lock();
                        worked = true;
                        break;
                    }
                    if (spares.size() > 1)
                        next = std::min(next, spares.front().since + kSpareKeep);
                    uint32_t bit = 1u << type;
                    if ((s_spareWanted & bit) && !(s_spareFailed & bit) && spares.empty())
                    {
                        lock.unlock();
                        auto start = std::chrono::steady_clock::now();
                        VkDeviceMemory memory = AllocateBlock(type);
                        uint64_t ns = SinceNs(start);
                        lock.lock();
                        s_spareWanted &= ~bit;
                        if (!memory)
                        {
                            // Left to the command processor, which tries again
                            // (and falls back to an allocation per image).
                            s_spareFailed |= bit;
                            fprintf(stderr, "[renderer] image memory: no spare block of memory type %u (%llu MB)\n", type,
                                (unsigned long long)(s_blockSize >> 20));
                        }
                        else
                        {
                            s_imageTotals.spareBlocks++;
                            s_imageTotals.spareNs += ns;
                            s_spares[type].push_back({ memory, std::chrono::steady_clock::now() });
                        }
                        worked = true;
                        break;
                    }
                    if (s_spareWanted & bit)
                        s_spareWanted &= ~bit;  // (a spare came back meanwhile, or the thread can't make one)
                }
                if (!worked)
                    s_spareCv.wait_until(lock, next);
            }
        }

        // A block of `type` for a new image: a spare (the newest; the thread
        // makes the next when it was the last), or one allocated here.
        ImageBlock* NewBlock(uint32_t type)
        {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            {
                std::lock_guard lock(s_spareMutex);
                std::vector<Spare>& spares = s_spares[type];
                if (!spares.empty())
                {
                    memory = spares.back().memory;
                    spares.pop_back();
                }
                if (spares.empty())
                    s_spareWanted |= 1u << type;
            }
            s_spareCv.notify_one();
            if (memory)
                s_imageTotals.fromSpare++;
            else
            {
                auto start = std::chrono::steady_clock::now();
                memory = AllocateBlock(type);
                s_imageTotals.allocations++;
                s_imageTotals.allocationNs += SinceNs(start);
                if (!memory)
                    return nullptr;
                s_imageTotals.blocksHere++;
                std::lock_guard lock(s_spareMutex);
                s_spareFailed &= ~(1u << type);
            }
            s_blocks.push_back(std::make_unique<ImageBlock>(ImageBlock{ memory, type, gpu::BlockRanges(s_blockSize) }));
            return s_blocks.back().get();
        }

        // Binds `img` into a block of `type`; false when no block can be had.
        bool BindToBlock(Image& img, const VkMemoryRequirements& req, uint32_t type)
        {
            ImageBlock* block = nullptr;
            VkDeviceSize offset = gpu::BlockRanges::kNone;
            for (auto& b : s_blocks)
                if (b->type == type && (offset = b->ranges.Allocate(req.size, req.alignment)) != gpu::BlockRanges::kNone)
                {
                    block = b.get();
                    break;
                }
            if (!block)
            {
                block = NewBlock(type);
                if (!block || (offset = block->ranges.Allocate(req.size, req.alignment)) == gpu::BlockRanges::kNone)
                    return false;
            }
            img.block = block;
            img.offset = offset;
            img.memory = block->memory;
            return true;
        }

        void DestroyImage(Image& img);
        // An image allocation failed for want of memory (`need` bytes):
        // evicts and frees what the texture cache can give (`round` 0: its
        // oldest textures; 1: every one the frame being recorded hasn't
        // drawn). False when nothing was freed (the failure stands).
        bool ReclaimTextureMemory(VkDeviceSize need, uint32_t round);

        // `imageNext`, `viewNext`: chained to the image's and its view's create infos.
        // `fallible`: an image or view the device turns down returns an empty
        // Image (nothing kept) instead of aborting.
        Image CreateImage(VkImageType type, VkFormat format, VkExtent3D extent, uint32_t layers,
            VkImageUsageFlags usage, VkImageViewType viewType, VkImageAspectFlags aspect, VkImageCreateFlags flags = 0,
            uint32_t levels = 1, const void* imageNext = nullptr, const void* viewNext = nullptr, bool fallible = false)
        {
            HitchTimer hitchTimer(s_hitch.imageMs);
            s_hitch.images++;
            auto start = std::chrono::steady_clock::now();
            Image img;
            VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
            ici.pNext = imageNext;
            ici.flags = flags;
            ici.imageType = type;
            ici.format = format;
            ici.extent = extent;
            ici.mipLevels = levels;
            ici.arrayLayers = layers;
            ici.samples = VK_SAMPLE_COUNT_1_BIT;
            ici.tiling = VK_IMAGE_TILING_OPTIMAL;  // (image memory blocks hold nothing linear)
            ici.usage = usage;
            ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkResult result = vkCreateImage(s_dev, &ici, nullptr, &img.image);
            // (Out of memory already: room from the texture cache, as for
            // its memory below.)
            for (uint32_t round = 0; (result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY) &&
                     ReclaimTextureMemory(0, round); round++)
                result = vkCreateImage(s_dev, &ici, nullptr, &img.image);
            if (fallible && result != VK_SUCCESS)
                return {};
            Check(result, "vkCreateImage");
            VkMemoryDedicatedRequirements dedicated{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
            VkMemoryRequirements2 req2{ VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated };
            if (s_imagePool)
            {
                VkImageMemoryRequirementsInfo2 info{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, nullptr, img.image };
                vkGetImageMemoryRequirements2(s_dev, &info, &req2);
            }
            else
                vkGetImageMemoryRequirements(s_dev, img.image, &req2.memoryRequirements);
            const VkMemoryRequirements& req = req2.memoryRequirements;
            uint32_t memoryType = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            img.bytes = req.size;
            const bool pooled = s_imagePool && !dedicated.prefersDedicatedAllocation && !dedicated.requiresDedicatedAllocation &&
                req.size <= s_blockSize / 2;
            if (!pooled || !BindToBlock(img, req, memoryType))
            {
                VkMemoryDedicatedAllocateInfo dai{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, img.image };
                VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                mai.pNext = dedicated.requiresDedicatedAllocation ? &dai : nullptr;
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = memoryType;
                auto allocationStart = std::chrono::steady_clock::now();
                VkResult allocated = vkAllocateMemory(s_dev, &mai, nullptr, &img.memory);
                s_imageTotals.allocations++;
                s_imageTotals.allocationNs += SinceNs(allocationStart);
                // Out of memory: the texture cache gives back what the frame
                // being recorded hasn't drawn (the oldest first, then all of
                // it, and the spare blocks), and the image is tried again, in
                // a range the evicted textures left first. Only when nothing
                // is left to give is it fatal, as every failure was before
                // (the cache never gave anything back, so a long free roam
                // ended here).
                for (uint32_t round = 0; (allocated == VK_ERROR_OUT_OF_DEVICE_MEMORY || allocated == VK_ERROR_OUT_OF_HOST_MEMORY) &&
                         ReclaimTextureMemory(req.size, round); round++)
                {
                    img.memory = VK_NULL_HANDLE;
                    if (pooled && BindToBlock(img, req, memoryType))
                        allocated = VK_SUCCESS;
                    else
                        allocated = vkAllocateMemory(s_dev, &mai, nullptr, &img.memory);
                }
                Check(allocated, "vkAllocateMemory(image)");
            }
            Check(vkBindImageMemory(s_dev, img.image, img.memory, img.offset), "vkBindImageMemory");
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.pNext = viewNext;
            vci.image = img.image;
            vci.viewType = viewType;
            vci.format = format;
            vci.subresourceRange = { aspect, 0, levels, 0, layers };
            result = vkCreateImageView(s_dev, &vci, nullptr, &img.view);
            if (fallible && result != VK_SUCCESS)
            {
                img.view = VK_NULL_HANDLE;
                DestroyImage(img);
                return {};
            }
            Check(result, "vkCreateImageView");
            s_imageTotals.images++;
            s_imageTotals.imageNs += SinceNs(start);
            return img;
        }

        // Nothing the GPU may still run can use it (see ApplyLiveSettings and
        // ReleaseRetiredTextures): its memory, or its range of a block, is
        // free for reuse at once.
        void DestroyImage(Image& img)
        {
            if (img.view)
                vkDestroyImageView(s_dev, img.view, nullptr);
            if (img.image)
                vkDestroyImage(s_dev, img.image, nullptr);
            if (ImageBlock* block = img.block)
            {
                if (!block->ranges.Free(img.offset, img.bytes))
                {
                    fprintf(stderr, "[renderer] image memory: %llu bytes at %llu aren't in use in their block\n",
                        (unsigned long long)img.bytes, (unsigned long long)img.offset);
                    abort();
                }
                if (block->ranges.Empty())
                {
                    // A spare now: the memory thread frees it if it stays
                    // unused beside another.
                    Spare spare{ block->memory, std::chrono::steady_clock::now() };
                    uint32_t type = block->type;
                    std::erase_if(s_blocks, [block](const std::unique_ptr<ImageBlock>& b) { return b.get() == block; });
                    {
                        std::lock_guard lock(s_spareMutex);
                        s_spares[type].push_back(spare);
                    }
                    s_spareCv.notify_one();
                }
            }
            else if (img.memory)
                vkFreeMemory(s_dev, img.memory, nullptr);
            img = {};
        }

        // ---------------------------------------------------------------
        // Command recording

        // Submissions. Work is recorded into the current slot's command
        // buffer and submitted without waiting; CPU-visible effects of the
        // command stream (fences, the read pointer) are attached to the
        // submission they follow and applied by the completion thread once
        // the GPU is done, as the real GPU would write them.
        // NFSMW_OCCLUSION=1 (ZpassDone): a counting segment is one precise
        // occlusion query, begun before a draw while a query bracket is open
        // and ended at its pass's end, at any ZPD or when its weight changes;
        // an END report is stored once the submission holding it completes,
        // with the samples of segments [s0, s1).
        struct ZpdSegment { uint64_t id; uint32_t query, num, den; };
        struct ZpdEnd { uint32_t address; uint64_t s0, s1, frame; std::chrono::steady_clock::time_point queued; };
        struct Submission
        {
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            bool inFlight = false;
            // A write whose `event` is set runs as soon as the GPU passes that
            // point of the command buffer (vkCmdSetEvent after the work it
            // follows); the rest run when the whole submission completes.
            struct Write { std::function<void()> fn; VkEvent event = VK_NULL_HANDLE; std::chrono::steady_clock::time_point queued = std::chrono::steady_clock::now(); };
            std::vector<Write> writes;
            std::vector<VkEvent> events;  // returned to the pool on completion
            uint64_t number = 0;           // s_submissionCounter when submitted (write-watch guard)
            size_t unsignaled = 0;         // first write not yet covered by an event
            std::chrono::steady_clock::time_point submitted;
            // NFSMW_OCCLUSION=1 (else always empty): the segments recorded
            // here, harvested at completion, then the END reports, in order.
            std::vector<ZpdSegment> zpdSegments;
            std::vector<ZpdEnd> zpdEnds;
            // NFSMW_PREPASS: secondary command buffers its passes recorded
            // into (the first `secondaryUsed`; reused once it completes).
            std::vector<VkCommandBuffer> secondaries;
            size_t secondaryUsed = 0;
        };
        // NFSMW_FENCE_EVENTS=0 disables mid-buffer fence publication.
        const bool s_fenceEvents = [] { const char* v = std::getenv("NFSMW_FENCE_EVENTS"); return !v || v[0] != '0'; }();
        std::vector<VkEvent> s_eventPool;  // under s_submitMutex
        // NFSMW_LOG_LATENCY: queue -> publish delay of deferred writes, by
        // how they were published (0 event, 1 completion). Under s_submitMutex.
        double s_writeDelayUs[2] = {};
        uint32_t s_writeCount[2] = {};
        void RunWrite(Submission::Write& write, int how)
        {
            s_writeDelayUs[how] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - write.queued).count();
            s_writeCount[how]++;
            if (how)
                timeline::Mark(timeline::WriteAtComplete);
            write.fn();
        }
        // GPU busy time: sum over submissions of the time from max(submit,
        // previous completion) to completion (completion-thread wake-up).
        std::atomic<uint64_t> s_gpuBusyUs{ 0 };
        // True GPU execution time: timestamps at each command buffer's start
        // and end (2 queries per slot).
        VkQueryPool s_timestamps = VK_NULL_HANDLE;  // NFSMW_GPU_TIMING=1 (each costs a blit encoder on MoltenVK)
        double s_timestampPeriod = 1.0;

        // NFSMW_PASS_PROFILE=<frame>[,<frames>]: time every render pass of one
        // frame (or of that many from it, each printed) and print them
        // (targets, area, draws, GPU us). 256 passes max. The compute work
        // between them is timed too, each dispatch with its barrier as an
        // entry of its own (ProfileCompute: a resolve, a texture's load, the
        // scaled memory's, the front buffer).
        const int s_profileFrame = [] { const char* v = std::getenv("NFSMW_PASS_PROFILE"); return v ? std::atoi(v) : -1; }();
        const int s_profileFrames = [] {
            const char* v = std::getenv("NFSMW_PASS_PROFILE");
            const char* comma = v ? std::strchr(v, ',') : nullptr;
            return comma ? std::max(std::atoi(comma + 1), 1) : 1;
        }();
        // NFSMW_RESOLVE_STATS=<from>,<to> (renderer frames; the race's
        // window is 4500,8100): per frame, what resolves move and what
        // the frame's structure costs, printed at exit ([resolve-stats]):
        // bytes the resolves read from their targets, wrote into guest
        // memory then, wrote into cached textures, and wrote into guest
        // memory later (deferred ones, written back); passes, barriers
        // (full, narrow), submits; renamed and presented resolves.
        struct HleStats
        {
            uint64_t frames = 0, resolves = 0, srcBytes = 0, guestBytes = 0, textureBytes = 0, writebackBytes = 0;
            uint64_t passes = 0, fullBarriers = 0, narrowBarriers = 0, submits = 0, renamed = 0, presented = 0;
        } s_hle;
        bool s_hleCounting = false;
        const std::pair<uint64_t, uint64_t> s_hleWindow = [] {
            const char* v = std::getenv("NFSMW_RESOLVE_STATS");
            if (!v)
                return std::pair<uint64_t, uint64_t>{ 0, 0 };
            const char* comma = std::strchr(v, ',');
            return std::pair<uint64_t, uint64_t>{ uint64_t(std::max(std::atoll(v), 0ll)), comma ? uint64_t(std::max(std::atoll(comma + 1), 0ll)) : UINT64_MAX };
        }();
        VkQueryPool s_passQueries = VK_NULL_HANDLE;
        // Per profiled pass, also what was recorded in the gap before it:
        // resolves, texture uploads, submissions, guest memory synced to the
        // GPU copy (KB).
        // And why it began: what ended the pass before
        // it (PassBreak), whether that pass had the same targets (a break
        // that only the reason forced), and for a submit its site (s_submitWhy).
        struct PassInfo { std::string desc; uint32_t draws = 0; uint32_t slot, query; uint32_t resolves = 0, uploads = 0, submits = 0; uint64_t shadowKB = 0;
            uint8_t why = 0, same = 0, submitWhy = 0; bool stats = false; uint64_t areaPx = 0;
            // NFSMW_PASS_STATS: its draws a depth pre-pass could take
            // (PrepassReason), and the others by why not.
            uint32_t prepass[8] = {}; };
        // Why a draw couldn't be in a depth pre-pass of its pass (0: it could:
        // it tests and writes depth with LESS_OR_EQUAL, no stencil test, no
        // blending into what it writes, no alpha test, kill or depth export).
        enum PrepassReason : uint8_t { kPrepassOk, kPrepassNoDepth, kPrepassNoWrite, kPrepassFunc, kPrepassStencil, kPrepassBlend,
            kPrepassDiscard, kPrepassOther };
        constexpr const char* kPrepassReasonNames[] = { "ok", "no depth", "no depth write", "depth func", "stencil", "blend",
            "alpha test/kill/depth out", "other" };
        // NFSMW_PASS_STATS=1 (with NFSMW_PASS_PROFILE): each pass's vertex and
        // fragment shader invocations (a pipeline statistics query, its index
        // the pass's first timestamp's / 2), and its fragments per pixel of
        // its area: how much of the frame is shaded more than once.
        VkQueryPool s_passStats = VK_NULL_HANDLE;
        // NFSMW_PASS_STATS=2, with the game's occlusion queries off
        // (NFSMW_OCCLUSION=0; one occlusion query at a time): also each
        // pass's samples that passed the depth and stencil tests (a precise
        // occlusion query), so fragments shaded per sample that was kept.
        VkQueryPool s_passSamples = VK_NULL_HANDLE;
        struct GapCounters { uint64_t resolves = 0, uploads = 0, submits = 0, shadowBytes = 0; } s_gapLast;
        std::vector<PassInfo> s_passInfos;
        bool s_profiling = false;
        uint32_t s_passQueryNext = 0;
        // The last entry's end timestamp is still to be written (EndPass,
        // ProfileComputeEnd). With the pool full no entry is added, and the
        // previous one's end, written already, must not be written again.
        bool s_passEndPending = false;
        std::atomic<uint64_t> s_gpuExecNs{ 0 };
        std::chrono::steady_clock::time_point s_lastCompletion;
        VkCommandPool s_commandPool = VK_NULL_HANDLE;
        Submission s_submissions[kSlots];
        uint32_t s_slot = 0;
        uint64_t s_submissionCounter = 1;       // bumps on every submit
        std::atomic<uint64_t> s_completedSubmission{ 0 };  // the last one the GPU finished (CachedSnapshot)
        VkDeviceSize s_lastConstantsOffset = 0;
        uint64_t s_lastConstantsSubmission = 0; // submission holding s_lastConstantsOffset
        const void* s_lastConstLayoutVs = nullptr;  // the layout of that upload (see Draw)
        const void* s_lastConstLayoutPs = nullptr;
        VkCommandBuffer s_cmd = VK_NULL_HANDLE;
        // NFSMW_PREPASS=1 (see kPrepassDefault): a depth pre-pass in each pass with
        // colour and depth targets. The pass records into two secondary
        // command buffers, run in its rendering in turn at its end: the
        // pre-pass, where each draw that writes depth (LESS_OR_EQUAL, no
        // stencil test, no blending, alpha test, kill or depth export) writes
        // it with no pixel shader (its pipeline's vertex stage keeps only the
        // position: `invariant gl_Position`, the same depth), and the pass,
        // where those draws test EQUAL without writing it. Every pixel then
        // runs the pixel shaders of the draws that end up on it (and of those
        // not in the pre-pass: alpha-tested, blended), not of all the opaque
        // ones drawn over each other on the way: a race frame's scene keeps
        // 2.4 samples a pixel, its mirror 2.6, the cube faces about 2
        // (NFSMW_PASS_STATS=2). The picture: a fragment the pre-pass hides was
        // drawn over by an opaque one later (or failed its test then); ties
        // keep the last of LESS_OR_EQUAL draws, as before.
        // s_cmd is the pass's buffer while one records (s_preCmd the
        // pre-pass's, s_primaryCmd the submission's).
        // NFSMW_PREPASS=2: alpha-tested draws and those with kills too, with
        // their pixel shader in the pre-pass (a variant writing no colour:
        // only what decides the discard is left of it).
        // By default 1 on x86-64 Linux (0 turns it off): a Steam Deck at 5 W,
        // 3 alternated pairs (deck_ab.sh): GPU busy -1.39, -1.78, -1.90 ms a
        // frame (24.1 -> 22.3), the command processor +1.8 ms, the 7 shots
        // within --strict, the trace hash the same. Level 2 measured worse
        // than 1 there (the alpha-tested draws' pixel shaders run twice).
        // Not on the tile GPUs (Apple, Turnip): unmeasured there.
#if defined(__linux__) && defined(__x86_64__)
        constexpr uint32_t kPrepassDefault = 1;
#else
        constexpr uint32_t kPrepassDefault = 0;
#endif
        const uint32_t s_prepassLevel = [] { const char* v = std::getenv("NFSMW_PREPASS"); return v ? uint32_t(std::clamp(std::atoi(v), 0, 2)) : kPrepassDefault; }();
        const bool s_prepassOn = s_prepassLevel >= 1;
        bool s_passSecondary = false;
        VkCommandBuffer s_primaryCmd = VK_NULL_HANDLE, s_preCmd = VK_NULL_HANDLE;
        uint64_t s_preEpoch = 1;  // the pre-pass buffer's bound state (Draw)
        struct DeferredRendering
        {
            VkRenderingInfoKHR ri;
            VkRenderingAttachmentInfoKHR colors[4];
            VkRenderingAttachmentInfoKHR ds;
        } s_deferredRendering;
        struct PrepassCounts { uint64_t passes = 0, draws = 0, prepassed = 0, waiting = 0, depthOnly = 0; } s_prepassCounts;
        bool s_recording = false;
        std::chrono::steady_clock::time_point s_recordStart;
        std::mutex s_submitMutex;
        std::condition_variable s_submitCv;
        std::deque<uint32_t> s_inFlight;
        // NFSMW_LOG_LATENCY: time with nothing in flight (under s_submitMutex).
        std::chrono::steady_clock::time_point s_emptySince = std::chrono::steady_clock::now();
        double s_emptyUs = 0;

        // Occlusion queries counted on the GPU (NFSMW_OCCLUSION=1; the
        // presenter decides: VulkanContext::occlusionCounting). Command
        // processor thread, but for the results ring (completion thread) and
        // the store statistics (under s_submitMutex). See ZpassDone.
        bool s_zpdOn = false;                  // constant after Initialize
        constexpr uint32_t kZpdQueries = 256;  // per slot
        VkQueryPool s_zpdPool[kSlots] = {};
        uint32_t s_zpdUsed[kSlots] = {};       // used by the slot's recording (reset at its next Begin)
        int32_t s_zpdQuery = -1;               // the active segment's query in s_zpdPool[s_slot], or -1
        uint32_t s_zpdNum = 0, s_zpdDen = 0;   // its weight (guest samples per host sample, num / den)
        uint64_t s_zpdSegNext = 1;             // segment ids
        zpd::BracketBook s_zpdOpen;
        std::unique_ptr<zpd::ResultRing> s_zpdResults;
        // The [zpd] line's counts: the command processor's, and the stores'
        // (under s_submitMutex; the delays in ms, CP -> store).
        struct ZpdCounts { uint64_t zpds, begins, ends, segments, draws, unmatched, dropped, replaced, unknown, overflow, clamped, unclamped; };
        ZpdCounts s_zpdCounts{};
        struct ZpdStores { uint64_t stored, nonZero, lost; double delaySum, delayMax; };
        ZpdStores s_zpdStores{};
        std::atomic<uint64_t> s_zpdUnavailable{ 0 };
        // The latest END count per report address (count, frame), for
        // captures (LogOcclusionCounts, any thread).
        std::mutex s_zpdLatestMutex;
        std::map<uint32_t, std::pair<uint32_t, uint64_t>> s_zpdLatest;
        // NFSMW_LOG_ZPD=<lines>: each BEGIN, counted draw, segment and END,
        // that many lines at most (any thread).
        const int64_t s_zpdLogLines = [] { const char* v = std::getenv("NFSMW_LOG_ZPD"); return v ? int64_t(std::atoll(v)) : int64_t(0); }();
        std::atomic<int64_t> s_zpdLogged{ 0 };
        __attribute__((format(printf, 1, 2))) void ZpdLog(const char* fmt, ...)
        {
            if (s_zpdLogged.load(std::memory_order_relaxed) >= s_zpdLogLines || s_zpdLogged.fetch_add(1) >= s_zpdLogLines)
                return;
            char line[512];
            va_list args;
            va_start(args, fmt);
            vsnprintf(line, sizeof(line), fmt, args);
            va_end(args);
            fprintf(stderr, "[zpd] %s\n", line);
        }
        bool ZpdLogging() { return s_zpdLogged.load(std::memory_order_relaxed) < s_zpdLogLines; }

        // Ends the active segment. Inside its pass: one is begun only right
        // before a draw, and every pass end ends it first (EndPass).
        void ZpdEndSegment()
        {
            if (s_zpdQuery < 0)
                return;
            vkCmdEndQuery(s_cmd, s_zpdPool[s_slot], uint32_t(s_zpdQuery));
            s_zpdQuery = -1;
        }

        // What ended a render pass (NFSMW_PASS_PROFILE
        // prints it with the pass after it, as why that one had to begin).
        enum PassBreak : uint8_t { kBreakStart, kBreakTargets, kBreakResolve, kBreakSubmit, kBreakClear, kBreakUpload, kBreakBarrier,
            kBreakPresent, kBreakOther };
        constexpr const char* kPassBreakNames[] = { "start", "targets", "resolve", "submit", "clear", "upload", "barrier", "present", "other" };
        PassBreak s_passBreak = kBreakStart;      // what ended the last pass
        uint8_t s_passBreakSubmit = 0;            // and, a submit, its site
        const void* s_lastPassTargets[5] = {};    // that pass's colour targets and depth
        void EndPass(PassBreak why = kBreakOther);
        // NFSMW_RESOLVE_RENAME: a target's image is destroyed: views of it
        // go, and a texture renamed to it samples its own image again.
        void ForgetRenamedImage(VkImage image);
        // Bumped when the command buffer's graphics state is unknown (a new
        // command buffer; without CP_OPT_KEEP_BINDINGS also compute work,
        // which shares the push constants).
        uint64_t s_stateEpoch = 1;
        // Bumped by compute work with CP_OPT_KEEP_BINDINGS: the next draw
        // pushes all of its push constants again (RADV keeps one copy of them
        // for both bind points, so a dispatch's overwrites the draw's), and
        // keeps the rest. Compute work binds its pipeline and descriptors at
        // the compute bind point, which leaves the graphics one's bound, and
        // no dynamic state changes (the Vulkan spec), so the draw after a
        // resolve or an untile no longer binds and sets everything again
        // (about 15 times a race frame, ~3 us each on RADV).
        uint64_t s_pushEpoch = 1;
        void ComputeWorkRecorded()
        {
            if (g_cpOpt & CP_OPT_KEEP_BINDINGS)
                s_pushEpoch++;
            else
                s_stateEpoch++;
        }

        // Write-watch GPU guard for the submission being recorded, once per
        // range and submission (the same vertex buffer serves many draws).
        // The ranges guarded so far are a direct-mapped table stamped with
        // the submission: no clearing, no allocation (a range that a
        // collision evicted is guarded again, which changes nothing).
        // NFSMW_WATCH_RANGES=0 (write_watch.cpp): a hash set, as before.
        const bool s_guardTable = [] { const char* v = std::getenv("NFSMW_WATCH_RANGES"); return !v || v[0] != '0'; }();
        struct GuardedRange { uint64_t key, submission; };
        std::unique_ptr<GuardedRange[]> s_guardedRanges;  // 4096
        std::unordered_set<uint64_t> s_guarded;
        uint64_t s_guardedSubmission = 0;
        void GuardGpu(uint32_t base, uint32_t size, bool write, const char* tag, bool untile = false)
        {
            if ((!g_eagerWrites && !s_shadowMode) || size == 0)
                return;
            // (A texture's untile is keyed apart from a plain read of the same
            // range, or it would lose its flag: see writewatch::GpuRead.)
            uint64_t key = uint64_t(base) << 32 | (size ^ (write ? 0x80000000u : untile ? 0x40000000u : 0));
            if (s_guardTable)
            {
                if (!s_guardedRanges)
                    s_guardedRanges.reset(new GuardedRange[4096]());
                GuardedRange& slot = s_guardedRanges[(key * 0x9E3779B97F4A7C15ull) >> 52];
                if (slot.key == key && slot.submission == s_submissionCounter)
                    return;
                slot = { key, s_submissionCounter };
            }
            else
            {
                if (s_guardedSubmission != s_submissionCounter)
                {
                    s_guarded.clear();
                    s_guardedSubmission = s_submissionCounter;
                }
                if (!s_guarded.insert(key).second)
                    return;
            }
            if (write)
                writewatch::GpuWrite(base, size, s_submissionCounter, tag);
            else
                writewatch::GpuRead(base, size, s_submissionCounter, tag, untile);
        }

        // Outside a render pass: an event after everything recorded so far,
        // covering the current submission's writes that have none yet.
        void SignalWrites()
        {
            if (!s_fenceEvents || !s_recording)
                return;
            Submission& sub = s_submissions[s_slot];
            VkEvent event;
            {
                std::lock_guard lock(s_submitMutex);
                if (sub.unsignaled == sub.writes.size())
                    return;
                if (s_eventPool.empty())
                {
                    VkEventCreateInfo eci{ VK_STRUCTURE_TYPE_EVENT_CREATE_INFO };
                    Check(vkCreateEvent(s_dev, &eci, nullptr, &event), "vkCreateEvent");
                }
                else
                {
                    event = s_eventPool.back();
                    s_eventPool.pop_back();
                }
                for (size_t i = sub.unsignaled; i < sub.writes.size(); i++)
                    sub.writes[i].event = event;
                sub.unsignaled = sub.writes.size();
                sub.events.push_back(event);
            }
            vkCmdSetEvent(s_cmd, event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        void Begin()
        {
            if (s_recording)
                return;
            Submission& sub = s_submissions[s_slot];
            {
                CpWaitTimer wait(CP_WAIT_SLOT);
                std::unique_lock lock(s_submitMutex);
                s_submitCv.wait(lock, [&] { return !sub.inFlight; });
            }
            vkResetFences(s_dev, 1, &sub.fence);
            vkResetCommandBuffer(sub.cmd, 0);
            sub.secondaryUsed = 0;
            s_cmd = sub.cmd;
            VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            Check(vkBeginCommandBuffer(s_cmd, &bi), "vkBeginCommandBuffer");
            s_recording = true;
            s_stateEpoch++;
            s_recordStart = std::chrono::steady_clock::now();
            if (s_timestamps)
            {
                vkCmdResetQueryPool(s_cmd, s_timestamps, s_slot * 2, 2);
                vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_timestamps, s_slot * 2);
            }
            // Occlusion queries: the slot's last submission has retired, its
            // results harvested; reset the queries it used (a host reset is a
            // memset on MoltenVK; else a command, outside any pass here: for
            // devices before Vulkan 1.2, which requires host reset, so not
            // exercised on MoltenVK, RADV or Turnip).
            if (s_zpdOn && s_zpdUsed[s_slot])
            {
                if (s_vk->resetQueryPool)
                    s_vk->resetQueryPool(s_dev, s_zpdPool[s_slot], 0, s_zpdUsed[s_slot]);
                else
                    vkCmdResetQueryPool(s_cmd, s_zpdPool[s_slot], 0, s_zpdUsed[s_slot]);
                s_zpdUsed[s_slot] = 0;
            }
            s_ringOffset = 0;
        }

        // Submits per call site (NFSMW_LOG_LATENCY): 1 ring full, 2 Submit, 3 SubmitIfIdle,
        // 4 SubmitIfDue, 5 Flush, 6 QueueWrite on an idle GPU, 7 present, 8 after a draw
        // to a starving GPU, 9 after a resolve or upload to one (SubmitIfStarving).
        uint32_t s_submitWhy[10] = {};
        const uint32_t s_logTextureUse = [] { const char* v = std::getenv("NFSMW_LOG_TEXTURE_USE"); return v ? uint32_t(strtoul(v, nullptr, 16)) : 0u; }();
        bool s_drawUsesLoggedTexture = false;
        bool s_drawScaled = false;  // the current draw renders into scaled targets (internal resolution)
        bool s_logDrawsNow = false;
        std::string s_loggedTextureFetch;
        uint32_t s_drawsSinceCheck = 0;
        bool s_submitAfterDraw = false;  // mid-batch submit to a starving GPU (eager writes)
        double s_queueLockUs = 0, s_queueSubmitUs = 0;  // waiting for the queue lock, in vkQueueSubmit

        void SubmitRecorded(int why = 0)
        {
            if (!s_recording)
                return;
            s_submitWhy[why]++;
            timeline::Mark(timeline::SubmitBegin, why);
            s_passBreakSubmit = uint8_t(why);
            EndPass(kBreakSubmit);
            if (s_timestamps)
                vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_timestamps, s_slot * 2 + 1);
            Check(vkEndCommandBuffer(s_cmd), "vkEndCommandBuffer");
            Submission& sub = s_submissions[s_slot];
            sub.number = s_submissionCounter;
            VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.commandBufferCount = 1;
            si.pCommandBuffers = &s_cmd;
            VkResult submitted;
            {
                auto t0 = std::chrono::steady_clock::now();
                std::unique_lock lock(*s_vk->queueMutex);
                auto tl = std::chrono::steady_clock::now();
                // The presenter's gate holds GPU work while the app is
                // inactive (iOS). It closes only after guesttime::Suspend()
                // and opens before Resume(), so a hold here is always the
                // suspension's time, not this submit's or the queue lock's.
                if (guesttime::Suspended())
                {
                    CpWaitTimer held(CP_WAIT_SUSPENDED);
                    video::WaitForeground(lock);
                }
                else
                    video::WaitForeground(lock);
                auto t1 = std::chrono::steady_clock::now();
                submitted = vkQueueSubmit(s_vk->queue, 1, &si, sub.fence);
                auto t2 = std::chrono::steady_clock::now();
                s_queueLockUs += std::chrono::duration<double, std::micro>(tl - t0).count();
                s_queueSubmitUs += std::chrono::duration<double, std::micro>(t2 - t1).count();
                s_hitch.submitMs += std::chrono::duration<double, std::milli>((t2 - t0) - (t1 - tl)).count();
            }
            // Out of the queue lock: on a lost device this thread waits in
            // video::DeviceLost for the main thread, which takes it.
            Check(submitted, "vkQueueSubmit (the renderer)");
            {
                std::lock_guard lock(s_submitMutex);
                sub.inFlight = true;
                sub.submitted = std::chrono::steady_clock::now();
                if (s_inFlight.empty())
                    s_emptyUs += std::chrono::duration<double, std::micro>(sub.submitted - s_emptySince).count();
                s_inFlight.push_back(s_slot);
                timeline::Mark(timeline::Submit, s_slot | std::min<uint32_t>(uint32_t(sub.writes.size()), 0xFFFF) << 8 | uint32_t(why) << 24);
            }
            s_submitCv.notify_all();
            s_slot = (s_slot + 1) % kSlots;
            s_recording = false;
            s_submissionCounter++;
            s_stats.submits++;
            s_hle.submits += s_hleCounting;
            writewatch::Rearm();
        }

        // Completion thread, `sub` (in `slot`) done: its segments' counts into
        // the ring, in guest samples. Never waits: the fence has signalled
        // (MoltenVK makes queries available before that), and WAIT_BIT could
        // hang elsewhere on a query never ended; one not available counts 0.
        void ZpdHarvest(uint32_t slot, const Submission& sub)
        {
            static uint64_t results[kZpdQueries * 2];  // value, availability
            uint32_t n = 0;
            for (const ZpdSegment& s : sub.zpdSegments)
                n = std::max(n, s.query + 1);
            memset(results, 0, size_t(n) * 16);
            VkResult r = vkGetQueryPoolResults(s_dev, s_zpdPool[slot], 0, n, size_t(n) * 16, results, 16,
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            const bool read = r == VK_SUCCESS || r == VK_NOT_READY;
            for (const ZpdSegment& s : sub.zpdSegments)
            {
                const bool available = read && results[s.query * 2 + 1] != 0;
                uint64_t raw = available ? results[s.query * 2] : 0;
                if (!available)
                    s_zpdUnavailable++;
                uint64_t guest = zpd::Normalize(raw, s.num, s.den);
                s_zpdResults->Put(s.id, guest, raw);
                ZpdLog("segment %llu: %s%llu host samples x %u/%u = %llu", (unsigned long long)s.id, available ? "" : "not available, ",
                    (unsigned long long)raw, s.num, s.den, (unsigned long long)guest);
            }
        }

        // An END report from its segments' counts, under s_submitMutex (the
        // completion thread; the command processor when nothing is in
        // flight). Stored as the GPU's stores are, around the write watch's
        // guard (opened once for its 8 dwords): only the CPU reads these
        // bytes (GetData), never the GPU, so imported and shadow memory
        // alike, and nothing guards them (GetData polls and takes "not
        // ready").
        void ZpdStore(const ZpdEnd& end)
        {
            uint32_t lost = 0;
            uint64_t raw = 0;
            // A runaway count stays one the game rejects (zpd::EndCount).
            const uint32_t count = zpd::EndCount(s_zpdResults->Sum(end.s0, end.s1, lost, &raw));
            uint8_t* host = s_sharedHost + end.address;
            writewatch::GpuThreadWrite(host, 32, [host, count] {
                zpd::StoreEnd(count, [host](uint32_t offset, uint32_t value) { memcpy(host + offset, &value, 4); });
            });
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - end.queued).count();
            s_zpdStores.stored++;
            s_zpdStores.nonZero += count != 0;
            s_zpdStores.lost += lost;
            s_zpdStores.delaySum += ms;
            s_zpdStores.delayMax = std::max(s_zpdStores.delayMax, ms);
            {
                std::lock_guard lock(s_zpdLatestMutex);
                if (s_zpdLatest.size() >= 64 && !s_zpdLatest.contains(end.address))
                    s_zpdLatest.clear();
                s_zpdLatest[end.address] = { count, end.frame };
            }
            ZpdLog("f%llu END %08X: segments %llu..%llu, %llu host samples: %u stored %.2f ms after its ZPD%s",
                (unsigned long long)end.frame, end.address, (unsigned long long)end.s0, (unsigned long long)end.s1,
                (unsigned long long)raw, count, ms, lost ? std::format(" ({} segments lost)", lost).c_str() : "");
        }

        // NFSMW_TEST_DEVICE_LOST=<s>: <s> seconds after the completion
        // thread first looks at a fence, the fence reads as lost
        // (VK_ERROR_DEVICE_LOST), to try video::DeviceLost's way out (the
        // log line, the crash report, the message, the exit) on any GPU: a
        // real GPU timeout is hard to cause on purpose.
        bool TestDeviceLost()
        {
            static const double after = [] {
                const char* v = std::getenv("NFSMW_TEST_DEVICE_LOST");
                return v ? std::atof(v) : -1.0;
            }();
            if (after < 0)
                return false;
            static const auto start = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= after;
        }

        void CompletionThread()
        {
#ifdef __APPLE__
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // see the command processor's Worker
#endif
            writewatch::t_gpuThread = true;
            SetHostThreadName("nfsmw-gpu-done");
            hostcpu::LeaveReservedCore();  // started by the command processor
            while (true)
            {
                uint32_t slot;
                {
                    std::unique_lock lock(s_submitMutex);
                    s_submitCv.wait(lock, [] { return !s_inFlight.empty(); });
                    slot = s_inFlight.front();
                }
                Submission& sub = s_submissions[slot];
                // Publish writes as the GPU passes their events; block on the
                // fence once none is left to watch.
                size_t done = 0;
                while (true)
                {
                    // A lost device never comes back: every status and wait
                    // says VK_ERROR_DEVICE_LOST at once from then on, and
                    // v0.1.4 spun here on it at 100% CPU while the command
                    // processor waited for this slot forever (a silent hang).
                    const VkResult status = TestDeviceLost() ? VK_ERROR_DEVICE_LOST : vkGetFenceStatus(s_dev, sub.fence);
                    if (status == VK_ERROR_DEVICE_LOST)
                        video::DeviceLost("vkGetFenceStatus (the renderer's completion thread)");
                    bool finished = status == VK_SUCCESS;
                    bool watching = false;
                    const size_t written = done;
                    {
                        std::lock_guard lock(s_submitMutex);
                        while (!finished && done < sub.writes.size() && sub.writes[done].event &&
                            vkGetEventStatus(s_dev, sub.writes[done].event) == VK_EVENT_SET)
                            { timeline::Mark(timeline::WriteByEvent); RunWrite(sub.writes[done++], 0); }
                        watching = done < sub.writes.size() && sub.writes[done].event;
                    }
                    // A fence the command processor's WAIT_REG_MEM sleeps on
                    // may be among them (gpu/cp_doorbell.h).
                    if (done != written)
                        g_cpDoorbell.Ring();
                    if (finished)
                        break;
                    if (watching)
                        std::this_thread::sleep_for(std::chrono::microseconds(30));
                    else
                    {
                        // Nothing mid-buffer to watch: wait for the fence, but
                        // wake now and then in case a write arrives with an event.
                        VkResult r = vkWaitForFences(s_dev, 1, &sub.fence, VK_TRUE, 1'000'000);
                        if (r == VK_SUCCESS)
                            break;
                        if (r == VK_ERROR_DEVICE_LOST)
                            video::DeviceLost("vkWaitForFences (the renderer's completion thread)");
                        if (r != VK_TIMEOUT)
                        {
                            // Anything else (out of memory) returns at once
                            // too: logged, and not a busy loop on it.
                            static int logged = 0;
                            if (logged++ < 10)
                                fprintf(stderr, "[renderer] vkWaitForFences (completion) returned %d\n", int(r));
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }
                }
                auto now = std::chrono::steady_clock::now();
                {
                    // NFSMW_LOG_LATENCY=1: submit -> completion-wake latency.
                    static const bool logLatency = std::getenv("NFSMW_LOG_LATENCY") != nullptr;
                    static double sumUs = 0, maxUs = 0;
                    static uint32_t n = 0;
                    static auto window = now;
                    if (logLatency)
                    {
                        double us = std::chrono::duration<double, std::micro>(now - sub.submitted).count();
                        sumUs += us;
                        maxUs = std::max(maxUs, us);
                        n++;
                        if (now - window > std::chrono::seconds(1))
                        {
                            double empty;
                            char writes[160];
                            {
                                std::lock_guard lock(s_submitMutex);
                                empty = s_emptyUs;
                                s_emptyUs = 0;
                                snprintf(writes, sizeof(writes), " | writes: %u by event (avg %.0f us), %u at completion (avg %.0f us)",
                                    s_writeCount[0], s_writeCount[0] ? s_writeDelayUs[0] / s_writeCount[0] : 0.0,
                                    s_writeCount[1], s_writeCount[1] ? s_writeDelayUs[1] / s_writeCount[1] : 0.0);
                                s_writeDelayUs[0] = s_writeDelayUs[1] = 0;
                                s_writeCount[0] = s_writeCount[1] = 0;
                            }
                            double windowUs = std::chrono::duration<double, std::micro>(now - window).count();
                            fprintf(stderr, "[latency] %u submissions/s, submit->done avg %.0f us, max %.0f us, queue empty %.0f%% | by site ring %u submit %u ifidle %u ifdue %u flush %u write %u present %u draw %u copies %u%s | queue lock %.1f ms/s, vkQueueSubmit %.1f ms/s\n",
                                n, sumUs / n, maxUs, 100.0 * empty / windowUs, s_submitWhy[1], s_submitWhy[2], s_submitWhy[3],
                                s_submitWhy[4], s_submitWhy[5], s_submitWhy[6], s_submitWhy[7], s_submitWhy[8], s_submitWhy[9], writes,
                                s_queueLockUs / 1000, s_queueSubmitUs / 1000);
                            s_queueLockUs = s_queueSubmitUs = 0;
                            for (auto& w : s_submitWhy)
                                w = 0;
                            sumUs = maxUs = 0;
                            n = 0;
                            window = now;
                        }
                    }
                }
                auto start = std::max(sub.submitted, s_lastCompletion);
                if (now > start)
                    s_gpuBusyUs += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - start).count());
                s_lastCompletion = now;
                if (s_timestamps)
                {
                    uint64_t ts[2];
                    if (vkGetQueryPoolResults(s_dev, s_timestamps, slot * 2, 2, sizeof(ts), ts, sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0])
                        s_gpuExecNs += uint64_t(double(ts[1] - ts[0]) * s_timestampPeriod);
                }
                if (!sub.zpdSegments.empty())
                    ZpdHarvest(slot, sub);
                {
                    // Writes run under the lock so QueueWrite can't append to
                    // a submission that is being retired.
                    std::lock_guard lock(s_submitMutex);
                    for (; done < sub.writes.size(); done++)
                        RunWrite(sub.writes[done], 1);
                    sub.writes.clear();
                    // Occlusion query reports, in stream order: a query's
                    // last tile, which makes it ready, is stored last.
                    for (const ZpdEnd& end : sub.zpdEnds)
                        ZpdStore(end);
                    sub.zpdEnds.clear();
                    sub.zpdSegments.clear();
                    sub.unsignaled = 0;
                    for (VkEvent event : sub.events)
                    {
                        vkResetEvent(s_dev, event);
                        s_eventPool.push_back(event);
                    }
                    sub.events.clear();
                    writewatch::SetCompleted(sub.number);
                    s_completedSubmission.store(sub.number, std::memory_order_release);
                    s_inFlight.pop_front();
                    timeline::Mark(timeline::Complete, slot);
                    if (s_inFlight.empty())
                        s_emptySince = std::chrono::steady_clock::now();
                    sub.inFlight = false;
                }
                s_submitCv.notify_all();
                // The queue is shallower: an idle command processor submits
                // what it has recorded (SubmitIfDue), and a WAIT_REG_MEM may
                // pass on the writes just stored. Wake it if it sleeps.
                g_cpDoorbell.Ring();
            }
        }

        // Until submission `number` completed (submitting it if it is still
        // being recorded).
        void WaitForSubmission(uint64_t number)
        {
            s_stats.shadowWaits++;
            if (number >= s_submissionCounter)
                SubmitRecorded(9);
            CpWaitTimer wait(CP_WAIT_SHADOW);
            std::unique_lock lock(s_submitMutex);
            s_submitCv.wait(lock, [&] { return s_inFlight.empty() || s_submissions[s_inFlight.front()].number > number; });
        }

        void WriteDeferredIn(uint32_t base, uint32_t size);

        // NFSMW_RESOLVE_USE=<swap> (diagnostic, shadow mode): from that swap on,
        // per resolve destination (its guest range), how often anything read its
        // guest bytes before the next resolve there wrote them again: GPU work
        // (EnsureShadow), or the CPU (a page of it no longer GPU-owned: read
        // back) - or nothing (dead: written over unread). [resolve-use] lines
        // every 600 swaps and at exit.
        const uint64_t s_resolveUseFrom = [] { const char* v = std::getenv("NFSMW_RESOLVE_USE"); return v ? uint64_t(std::max(std::atoll(v), 0ll)) : 0ull; }();
        bool s_resolveUse = false;  // counting (from s_resolveUseFrom)
        struct ResolveUse
        {
            uint32_t width = 0, height = 0, format = 0;
            bool depth = false, fused = false, partial = false, deferred = false;
            bool pending = false, gpuRead = false;  // written by a resolve since; read by GPU work since
            uint64_t resolves = 0, bytes = 0, dead = 0, deadBytes = 0, gpuReads = 0, cpuReads = 0;
        };
        std::map<uint64_t, ResolveUse> s_resolveUses;  // (range base << 32) | range bytes
        bool s_resolveUseQuiet = false;  // (a resolve's own EnsureShadow isn't a read)
        std::mutex s_resolveUseMutex;    // (the exit report reads the map on another thread)
        uint64_t s_resolveUseFrames = 0;

        // A resolve into [base, base + size) of `bytes` pixel bytes: the last
        // one there was read, or not. Returns its entry (for what the resolve
        // turns out to be).
        ResolveUse* NoteResolveUse(uint32_t base, uint32_t size, uint32_t w, uint32_t h, uint32_t format, bool depth, uint64_t bytes)
        {
            std::lock_guard lock(s_resolveUseMutex);
            ResolveUse& u = s_resolveUses[uint64_t(base) << 32 | size];
            if (u.pending)
            {
                bool cpu = false;
                for (uint32_t page = base >> 12; page <= (base + size - 1) >> 12 && !cpu; page++)
                    cpu = !writewatch::AnyGpuOwned(page << 12, 4096);
                if (u.gpuRead)
                    u.gpuReads++;
                else if (cpu)
                    u.cpuReads++;
                else
                {
                    u.dead++;
                    u.deadBytes += bytes;
                }
            }
            u.pending = true;
            u.gpuRead = false;
            u.width = w;
            u.height = h;
            u.format = format;
            u.depth = depth;
            u.resolves++;
            u.bytes += bytes;
            return &u;
        }

        void NoteResolveRead(uint32_t base, uint32_t size)
        {
            if (s_resolveUseQuiet)
                return;
            std::lock_guard lock(s_resolveUseMutex);
            for (auto& [key, u] : s_resolveUses)
            {
                uint32_t b = uint32_t(key >> 32), n = uint32_t(key);
                if (u.pending && uint64_t(b) < uint64_t(base) + size && uint64_t(base) < uint64_t(b) + n)
                    u.gpuRead = true;
            }
        }

        void ReportResolveUse(const char* when)
        {
            if (!s_resolveUseFrames)
                return;
            std::lock_guard lock(s_resolveUseMutex);
            uint64_t bytes = 0, dead = 0, resolves = 0, deadResolves = 0;
            std::vector<std::pair<uint64_t, const ResolveUse*>> order;
            for (auto& [key, u] : s_resolveUses)
            {
                bytes += u.bytes;
                dead += u.deadBytes;
                resolves += u.resolves;
                deadResolves += u.dead;
                order.push_back({ key, &u });
            }
            std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second->bytes > b.second->bytes; });
            const double f = double(s_resolveUseFrames);
            fprintf(stderr, "[resolve-use] %s, %llu frames: a frame %.1f resolves, %.2f MB; dead (written over unread) %.1f resolves, %.2f MB (%.0f%%)\n",
                when, (unsigned long long)s_resolveUseFrames, double(resolves) / f, double(bytes) / f / 1048576.0, double(deadResolves) / f,
                double(dead) / f / 1048576.0, bytes ? 100.0 * double(dead) / double(bytes) : 0.0);
            for (size_t i = 0; i < order.size() && i < 24; i++)
            {
                const ResolveUse& u = *order[i].second;
                fprintf(stderr, "[resolve-use]   %08X+%u %ux%u fmt %u %s%s%s: %.2f a frame, %.3f MB; dead %llu, read by GPU work %llu, by the CPU %llu\n",
                    uint32_t(order[i].first >> 32), uint32_t(order[i].first), u.width, u.height, u.format, u.depth ? "depth" : "colour",
                    u.fused ? (u.partial ? " fused (part)" : " fused") : "", u.deferred ? " deferred" : "", double(u.resolves) / f,
                    double(u.bytes) / f / 1048576.0, (unsigned long long)u.dead, (unsigned long long)u.gpuReads, (unsigned long long)u.cpuReads);
            }
        }

        // Shadow mode: bring the GPU's copy of [base, base + size) up to date
        // before recording work that reads it (deferred resolves' bytes there
        // too: NFSMW_RESOLVE_DEFER).
        void EnsureShadow(uint32_t base, uint32_t size)
        {
            if (!s_shadowMode || size == 0)
                return;
            base &= 0x1FFFFFFF;
            size = std::min<uint32_t>(size, uint32_t(kSharedSize) - base);
            if (s_resolveUse)
                NoteResolveRead(base, size);
            WriteDeferredIn(base, size);
            uint64_t key = uint64_t(base) << 32 | size;
            auto it = s_shadowSynced.find(key);
            if (it != s_shadowSynced.end() && !writewatch::RangeWrittenSince(base, size, it->second))
                return;
            uint64_t sequence = writewatch::Current();
            uint64_t since = it != s_shadowSynced.end() ? it->second : 0;
            HitchTimer hitchTimer(s_hitch.shadowMs);
            while (uint64_t busy = writewatch::SyncShadow(base, size, since))
                WaitForSubmission(busy);
            if (s_shadowSynced.size() > 65536)
                s_shadowSynced.clear();
            s_shadowSynced[key] = sequence;
        }

        // NFSMW_PASS_BARRIER=0|1|2|3 (default 2 with kLighterPasses, else 0): narrower
        // barriers between passes. On MoltenVK a barrier outside a pass is a
        // Metal fence wait per stage (vertex, fragment, compute, copy) of the
        // encoders after it on those before it; a full one makes the next
        // pass's vertex stage wait for everything before, so no pass's vertex
        // work overlaps the last pass's fragments or the resolve between them.
        // 1: a pass ends with its attachment writes (and its shaders' reads)
        // before fragment, compute and transfer work only: no vertex shader
        // reads an attachment (targets reach draws only through resolves).
        // Every pass is followed by a resolve now, so alone it overlaps
        // nothing (a control). 2: a resolve's writes, too, go before those
        // stages only, and a draw whose vertex shader may read what one wrote
        // (vertex or index data in its range, the cached texture it stored
        // into) gets a full barrier first (VertexBarrierIfNeeded). Vertex
        // stages then run ahead through a chain of passes and resolves (the
        // cube faces'). 3: 2 with full pass ends: a pass's vertex stage
        // overlaps only the resolve before it. Texture loads keep full
        // barriers (rare since the write watch's subpages; the ring they
        // stage through is a Metal-tracked index buffer anyway).
        // An M4 Max (race, alternating windows): at 2 and 3 all but one
        // post-resolve vertex stage start early, and the renderer's GPU time
        // drops 0.22-0.34 ms (12-16%); 1 changes nothing. For the iPad to
        // decide.
        // NFSMW_PASS_BARRIER_ALT=1 (A/B): that level in every other 120-frame
        // [perf] window, off in the rest: one run, the same content in both
        // arms. Each window's [barrier] line names the level it ran at.
        // Default 2 with kLighterPasses (Apple: iPad Pro M2 GPU-bound at 2x,
        // same-run windows: executing -0.5 ms), else 0.
        const uint32_t s_passBarrierLevel = [] { const char* v = std::getenv("NFSMW_PASS_BARRIER"); return v ? uint32_t(std::clamp(std::atoi(v), 0, 3)) : kLighterPasses ? 2u : 0u; }();
        const bool s_passBarrierAlt = [] { const char* v = std::getenv("NFSMW_PASS_BARRIER_ALT"); return v && v[0] == '1'; }() && s_passBarrierLevel;
        uint32_t s_passBarrier = s_passBarrierAlt ? 0u : s_passBarrierLevel;
        // Levels 2-3: guest memory resolves wrote since the last full barrier,
        // sorted and merged [base, end) (no vertex stage waits for them yet),
        // their hull for the common miss, and the cached textures (by guest
        // base) they stored into.
        std::vector<std::pair<uint32_t, uint32_t>> s_vertexPending;
        uint32_t s_vertexPendingLo = UINT32_MAX, s_vertexPendingHi = 0;
        std::vector<uint32_t> s_vertexPendingTextures;
        // Pass ends narrowed since the last full barrier. MoltenVK names each
        // stage's fences from a ring of 63 (MVKCommandEncoder::setBarrier):
        // a pass's vertex wait left on an old fragment fence must never come
        // round to the one that pass updates itself, so every 48th is full.
        uint32_t s_narrowPassEnds = 0;
        // The [barrier] line (OnSwap): narrowed pass ends and resolves, full
        // ones forced by the cap, vertex barriers (by reason: vertex data,
        // indices, a texture) and those that ended an open pass.
        struct BarrierCounts { uint64_t passEnds, resolves, capped, vertex[3], splits; };
        BarrierCounts s_barrierCounts{};

        // Everything before -> everything after. Outside rendering only.
        void FullBarrier()
        {
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
            s_vertexPending.clear();
            s_vertexPendingLo = UINT32_MAX;
            s_vertexPendingHi = 0;
            s_vertexPendingTextures.clear();
            s_narrowPassEnds = 0;
            s_hle.fullBarriers += s_hleCounting;
        }

        // `src` stages' work (and, in Vulkan, everything logically earlier)
        // -> every stage after but the vertex ones: on MoltenVK nothing waits
        // before a vertex stage. Outside rendering only.
        void NarrowBarrier(VkPipelineStageFlags src, VkAccessFlags srcAccess)
        {
            s_hle.narrowBarriers += s_hleCounting;
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = srcAccess;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(s_cmd, src,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
        }

        // At a pass's end. Its attachment writes go before what reads or
        // writes those images next (passes' loads, clears and stores;
        // resolves), its shaders' reads before compute and transfer writes
        // (resolves, texture loads). The vertex stage is named in the source
        // although Vulkan implies it: MoltenVK maps stages one to one, and
        // without it a resolve overwriting what this pass's vertex shader
        // read would wait only for its fragment stage.
        void PassEndBarrier()
        {
            if (s_exp & 256)
                return;
            if (s_passBarrier < 1 || s_passBarrier == 3)
                return FullBarrier();
            if (s_narrowPassEnds >= 48)
            {
                s_barrierCounts.capped++;
                return FullBarrier();
            }
            s_narrowPassEnds++;
            s_barrierCounts.passEnds++;
            NarrowBarrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }

        // After a 1x resolve's dispatch, which wrote guest memory [base, base
        // + size) and, unless `texture` is 0, the texels of the cached texture
        // at that guest base. Levels 2-3 leave the vertex stages out and keep
        // both for VertexBarrierIfNeeded until a full barrier (the front
        // buffer's, once a frame, at least).
        void ResolveBarrier(uint32_t base, uint32_t size, uint32_t texture)
        {
            if (s_passBarrier < 2)
                return FullBarrier();
            s_barrierCounts.resolves++;
            NarrowBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
            if (texture && std::find(s_vertexPendingTextures.begin(), s_vertexPendingTextures.end(), texture) == s_vertexPendingTextures.end())
                s_vertexPendingTextures.push_back(texture);
            if (!size)
                return;
            uint32_t end = base + size;
            s_vertexPendingLo = std::min(s_vertexPendingLo, base);
            s_vertexPendingHi = std::max(s_vertexPendingHi, end);
            // Into the sorted list, merged with what it overlaps or touches.
            auto it = std::lower_bound(s_vertexPending.begin(), s_vertexPending.end(), base,
                [](const std::pair<uint32_t, uint32_t>& r, uint32_t b) { return r.second < b; });
            auto last = it;
            while (last != s_vertexPending.end() && last->first <= end)
            {
                base = std::min(base, last->first);
                end = std::max(end, last->second);
                ++last;
            }
            it = s_vertexPending.erase(it, last);
            s_vertexPending.insert(it, { base, end });
        }

        // As FullBarrier, when the only work since the last one is a compute
        // dispatch: its shader writes before everything after.
        void ComputeBarrier()
        {
            s_hle.narrowBarriers += s_hleCounting;
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 1, &mb, 0, nullptr, 0, nullptr);
        }

        void ToGeneral(VkImage image, VkImageAspectFlags aspect, uint32_t layers)
        {
            VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = { aspect, 0, VK_REMAINING_MIP_LEVELS, 0, layers };
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 0, nullptr, 0, nullptr, 1, &b);
        }

        // After a resolve or a texture upload, as every 128 draws: submit to a
        // starving GPU (fewer than two submissions in flight) a recording at
        // least NFSMW_STARVE_SUBMIT_US old. Around the shadow maps and the
        // mirror there are few draws but long GPU work, and on the Steam Frame
        // the GPU sat idle beside them. NFSMW_STARVE_AT_COPIES=0 turns it off.
        void SubmitIfStarving()
        {
            static const bool on = [] { const char* v = std::getenv("NFSMW_STARVE_AT_COPIES"); return !v || v[0] != '0'; }();
            static const int64_t starveUs = [] { const char* v = std::getenv("NFSMW_STARVE_SUBMIT_US"); return v ? int64_t(std::atoll(v)) : 1000; }();
            if (!on || !g_eagerWrites || !s_recording)  // (called between passes)
                return;
            if (std::chrono::steady_clock::now() - s_recordStart < std::chrono::microseconds(starveUs))
                return;
            {
                std::lock_guard lock(s_submitMutex);
                if (s_inFlight.size() >= 2)
                    return;
            }
            SubmitRecorded(9);
        }

        // Space in the current submission's ring slot (may submit and move
        // on to the next slot). Returns ~0 if `size` can never fit.
        VkDeviceSize RingAllocate(VkDeviceSize size)
        {
            if (size > kSlotSize)
                return ~VkDeviceSize(0);
            Begin();
            VkDeviceSize offset = (s_ringOffset + 255) & ~VkDeviceSize(255);
            if (offset + size > kSlotSize)
            {
                SubmitRecorded(1);
                Begin();
                offset = 0;
            }
            s_ringOffset = offset + size;
            return VkDeviceSize(s_slot) * kSlotSize + offset;
        }

        // CP_OPT_SNAPSHOT_CACHE (off by default, see renderer.h): vertex
        // snapshots kept across draws. In a race on the Steam Deck ~97% of
        // the snapshots equal the range's last one: 10-16 MB a frame copied
        // into the ring and read back by the GPU. A range's copy (the fetch
        // constant's base and size) goes into the region after the slots,
        // with a host copy of it (s_cacheMirror), and every later draw
        // reading inside the span copied uses it while the guest's bytes
        // still equal the host copy. (The write watch can't tell: the
        // game's vertex pages aren't protected, and watching them faulted
        // ~550 pages a frame, dynamic buffers mostly; a sequence read before
        // the watch misses a store between the two.) The region is written
        // round; an entry in the way of a new copy is dropped once the last
        // submission that read it completed (s_completedSubmission), and
        // while one is still in flight the draw copies into its slot as
        // before (Stats::snapshotFallbacks). A range written after it was
        // cached isn't cached again for kCacheBurnFrames. Command processor
        // only. Measured at 5 W: the compares cost more than the copies
        // (command processor +2.4 ms a frame) and the GPU reads the
        // scattered copies slower (+0.8 ms a frame), with the pictures the
        // same: kept for the record, off.
        struct SnapshotEntry
        {
            uint32_t base, size;     // the range (the key)
            uint32_t start, length;  // the span copied, bytes from base
            VkDeviceSize offset;     // in the ring buffer
            uint64_t sequence;       // the span's write sequence when copied
            uint64_t lastUse;        // the submission of the last draw reading it
            bool valid;              // false: this draw's copy only (a page was written as it was made)
        };
        // CP_OPT_INDEX_CACHE: a draw's converted indices (ConvertIndices)
        // kept the same way, with the vertex range they span, by the index
        // buffer's address and the conversion.
        struct IndexEntry
        {
            uint32_t base, bytes;    // the guest bytes converted
            uint32_t how;            // the conversion's bits (CachedIndices)
            VkDeviceSize offset;
            uint64_t sequence, lastUse;
            vsnap::IndexRange vertices;
            bool valid;
        };
        std::unordered_map<uint64_t, SnapshotEntry> s_snapshotCache;
        std::unordered_map<uint64_t, IndexEntry> s_indexCache;
        // The region's bytes in use, by offset: an entry's (its kind and key),
        // or a replaced entry's, kept until the last submission reading it
        // completed (dead: no key).
        struct CachedSpace { bool index, dead; uint64_t key, lastUse; };
        std::map<VkDeviceSize, CachedSpace> s_snapshotByOffset;
        VkDeviceSize s_snapshotCursor = 0;  // the region's next byte to write
        // Host copies of the region's bytes (cached memory): a hit compares
        // the guest's bytes with them, since the write watch misses stores
        // to pages it doesn't protect (and a store between the sequence
        // read and the watch). s_cacheRaw: for indices, the guest bytes
        // converted (the region holds the conversion).
        std::vector<uint8_t> s_cacheMirror, s_cacheRaw;
        uint8_t* CacheMirror(VkDeviceSize offset) { return s_cacheMirror.data() + (offset - VkDeviceSize(kSlots) * kSlotSize); }
        uint8_t* CacheRaw(VkDeviceSize offset) { return s_cacheRaw.data() + (offset - VkDeviceSize(kSlots) * kSlotSize); }
        // Ranges written after they were cached, by the frame it was seen:
        // a range written again within kCacheBurnFrames isn't cached (its
        // draws copy into the slot as before): dynamic vertex data (particles,
        // the front end) faulted ~550 pages a frame when every range was watched.
        std::unordered_map<uint64_t, uint64_t> s_snapshotBurned, s_indexBurned;
        constexpr uint64_t kCacheBurnFrames = 120;
        uint64_t s_cacheFrame = 0;  // s_frame (set with it: declared later)
        bool Burned(std::unordered_map<uint64_t, uint64_t>& burned, uint64_t key)
        {
            auto it = burned.find(key);
            if (it == burned.end())
                return false;
            if (s_cacheFrame - it->second < kCacheBurnFrames)
                return true;
            burned.erase(it);
            return false;
        }
        constexpr uint64_t SnapshotKey(uint32_t base, uint32_t size) { return uint64_t(base) << 32 | size; }
        // NFSMW_SNAPSHOT_VERIFY=1: every hit's bytes are compared with the
        // guest's now (reading the ring back, slowly); a mismatch is logged
        // and the entry dropped.
        const bool s_snapshotVerify = [] { const char* v = std::getenv("NFSMW_SNAPSHOT_VERIFY"); return v && v[0] == '1'; }();
        // NFSMW_SNAPSHOT_SHADOW=1 (a bisection): the cache is kept as usual,
        // but every draw copies into its slot as before and the GPU reads that.
        const bool s_snapshotShadow = [] { const char* v = std::getenv("NFSMW_SNAPSHOT_SHADOW"); return v && v[0] == '1'; }();
        uint64_t s_snapshotVerified = 0, s_snapshotBad = 0;

        // An entry goes (replaced); its bytes stay reserved until the GPU is done with them.
        void DropSnapshot(std::unordered_map<uint64_t, SnapshotEntry>::iterator it)
        {
            CachedSpace& space = s_snapshotByOffset[it->second.offset];
            space.dead = true;
            space.lastUse = it->second.lastUse;
            s_snapshotCache.erase(it);
        }
        void DropIndices(std::unordered_map<uint64_t, IndexEntry>::iterator it)
        {
            CachedSpace& space = s_snapshotByOffset[it->second.offset];
            space.dead = true;
            space.lastUse = it->second.lastUse;
            s_indexCache.erase(it);
        }

        // Room for `length` bytes of the region at `offset`: the entries in
        // the way go, unless the GPU may still read one (false: use the slot).
        bool AllocateCached(uint32_t length, VkDeviceSize& offset)
        {
            const VkDeviceSize need = (VkDeviceSize(length) + 255) & ~VkDeviceSize(255);
            if (need > kSnapshotCacheSize)
                return false;
            if (s_snapshotCursor + need > kSnapshotCacheSize)
                s_snapshotCursor = 0;
            const uint64_t completed = s_completedSubmission.load(std::memory_order_acquire);
            const VkDeviceSize at = VkDeviceSize(kSlots) * kSlotSize + s_snapshotCursor;  // (the records' keys are ring offsets)
            for (auto o = s_snapshotByOffset.lower_bound(at); o != s_snapshotByOffset.end() && o->first < at + need;)
            {
                CachedSpace& space = o->second;
                if (!space.dead)
                {
                    if (space.index)
                        space.lastUse = s_indexCache.find(space.key)->second.lastUse;
                    else
                        space.lastUse = s_snapshotCache.find(space.key)->second.lastUse;
                }
                if (space.lastUse > completed)
                {
                    s_stats.snapshotFallbacks++;
                    return false;
                }
                if (!space.dead)
                {
                    if (space.index)
                        s_indexCache.erase(space.key);
                    else
                        s_snapshotCache.erase(space.key);
                }
                o = s_snapshotByOffset.erase(o);
            }
            offset = at;
            s_snapshotCursor += need;
            return true;
        }

        // The cached copy of range [base, base + size) that holds its bytes
        // [start, start + length), made now if none does; null when the
        // region has no room the GPU is done with (copy into the slot).
        const SnapshotEntry* CachedSnapshot(uint32_t base, uint32_t size, uint32_t start, uint32_t length)
        {
            const uint64_t key = SnapshotKey(base, size);
            auto it = s_snapshotCache.find(key);
            if (it != s_snapshotCache.end())
            {
                SnapshotEntry& e = it->second;
                bool stale = !e.valid || memcmp(CacheMirror(e.offset), s_sharedHost + base + e.start, e.length) != 0;
                if (!stale && s_snapshotVerify && start >= e.start && start + length <= e.start + e.length)
                {
                    s_snapshotVerified++;
                    if (memcmp(s_ringHost + e.offset + (start - e.start), s_sharedHost + base + start, length) != 0)
                    {
                        stale = true;
                        if (s_snapshotBad++ < 20)
                            fprintf(stderr, "[snapshot-verify] vertex range %08X+%u span %u+%u: cached at %llu (sequence %llu, now %llu, valid %d) differs from the guest; %llu checked\n",
                                base, size, start, length, (unsigned long long)e.offset, (unsigned long long)e.sequence,
                                (unsigned long long)writewatch::WriteSequence(base + e.start, e.length), int(e.valid), (unsigned long long)s_snapshotVerified);
                    }
                }
                if (!stale && start >= e.start && start + length <= e.start + e.length)
                {
                    e.lastUse = s_submissionCounter;
                    s_stats.snapshotHits++;
                    return &e;
                }
                DropSnapshot(it);
                if (stale)
                {
                    s_snapshotBurned[key] = s_cacheFrame;
                    return nullptr;
                }
            }
            else if (Burned(s_snapshotBurned, key))
                return nullptr;
            VkDeviceSize offset;
            if (!AllocateCached(length, offset))
                return nullptr;
            if (s_cacheMirror.empty())
                s_cacheMirror.resize(size_t(kSnapshotCacheSize));
            memcpy(CacheMirror(offset), s_sharedHost + base + start, length);
            memcpy(s_ringHost + offset, CacheMirror(offset), length);
            s_stats.snapshotBytes += (VkDeviceSize(length) + 255) & ~VkDeviceSize(255);
            SnapshotEntry& e = s_snapshotCache[key] = SnapshotEntry{ base, size, start, length, offset, 0, s_submissionCounter, true };
            s_snapshotByOffset[offset] = { false, false, key, s_submissionCounter };
            return &e;
        }

        // The converted indices of `count` from `src` as `convert` makes
        // them (into the region), by their guest bytes and the conversion's
        // bits (`how`): cached while those bytes stay unwritten. Null when
        // the region has no room (convert into the slot).
        template <typename Convert>
        const IndexEntry* CachedIndices(uint32_t base, uint32_t bytes, uint32_t how, Convert convert)
        {
            const uint64_t key = uint64_t(base >> 1) << 35 | uint64_t(how & 31u) << 30 | (bytes & 0x3FFFFFFFu);
            auto it = s_indexCache.find(key);
            if (it != s_indexCache.end())
            {
                IndexEntry& e = it->second;
                bool stale = !e.valid || memcmp(CacheRaw(e.offset), s_sharedHost + base, bytes) != 0;
                if (!stale && s_snapshotVerify && e.base == base && e.bytes == bytes && e.how == how)
                {
                    std::vector<uint8_t> fresh(bytes);
                    const vsnap::IndexRange v = convert(fresh.data());
                    s_snapshotVerified++;
                    if (memcmp(s_ringHost + e.offset, fresh.data(), bytes) != 0 || v.lo != e.vertices.lo || v.hi != e.vertices.hi)
                    {
                        stale = true;
                        if (s_snapshotBad++ < 20)
                            fprintf(stderr, "[snapshot-verify] indices %08X+%u how %u: cached at %llu (sequence %llu, now %llu, valid %d) differ (range %u..%u vs %u..%u); %llu checked\n",
                                base, bytes, how, (unsigned long long)e.offset, (unsigned long long)e.sequence,
                                (unsigned long long)writewatch::WriteSequence(base, bytes), int(e.valid), e.vertices.lo, e.vertices.hi, v.lo, v.hi,
                                (unsigned long long)s_snapshotVerified);
                    }
                }
                if (!stale && e.base == base && e.bytes == bytes && e.how == how)
                {
                    e.lastUse = s_submissionCounter;
                    s_stats.snapshotHits++;
                    return &e;
                }
                DropIndices(it);
                if (stale)
                {
                    s_indexBurned[key] = s_cacheFrame;
                    return nullptr;
                }
            }
            else if (Burned(s_indexBurned, key))
                return nullptr;
            VkDeviceSize offset;
            if (!AllocateCached(bytes, offset))
                return nullptr;
            if (s_cacheRaw.empty())
            {
                s_cacheRaw.resize(size_t(kSnapshotCacheSize));
                s_cacheMirror.resize(size_t(kSnapshotCacheSize));
            }
            // The guest bytes first (a store meanwhile shows at the next compare).
            memcpy(CacheRaw(offset), s_sharedHost + base, bytes);
            const vsnap::IndexRange vertices = convert(s_ringHost + offset);
            s_stats.snapshotBytes += (VkDeviceSize(bytes) + 255) & ~VkDeviceSize(255);
            // The conversion read the guest again: a store between the two reads
            // makes the raw copy differ from what was converted; compare once.
            const bool valid = memcmp(CacheRaw(offset), s_sharedHost + base, bytes) == 0;
            IndexEntry& e = s_indexCache[key] = IndexEntry{ base, bytes, how, offset, 0, s_submissionCounter, vertices, valid };
            s_snapshotByOffset[offset] = { true, false, key, s_submissionCounter };
            return &e;
        }

        // ---------------------------------------------------------------
        // Shaders

        // A translated shader. Its SPIR-V is compiled when a pipeline first
        // needs it, on a pipeline worker (compileMutex guards the modules).
        struct Shader
        {
            ShaderStage stage;
            uint64_t hash;
            TranslatedShader info;         // immutable once loaded
            // The guest's words it was made from (guest order; none for the
            // depth-only one): the pipeline key list keeps them, to make it
            // again in a later session (gpu/pipeline_keys.h).
            std::vector<uint32_t> words;
            std::mutex compileMutex;
            bool vertexCompiled = false, rectCompiled = false;
            VkShaderModule moduleRect = VK_NULL_HANDLE;  // VS with SB_RECT: for rectangle lists (raster bit 9)
            VkShaderModule module = VK_NULL_HANDLE;  // VS
            // PS variants by (colour outputs, interpolator inputs, alpha test).
            std::unordered_map<uint32_t, VkShaderModule> variants;
        };
        std::unordered_map<uint64_t, std::unique_ptr<Shader>> s_shaders;
        Shader* s_vs = nullptr;
        Shader* s_ps = nullptr;

        // Depth-only draws run no pixel shader on Xenos (depth_only.h): they
        // get this empty one instead of the guest's bound one, left over from
        // earlier draws. NFSMW_DEPTH_ONLY_PS=1 runs the bound one, as up to
        // v0.1.2 (A/B only).
        constexpr uint64_t kDepthOnlyPsHash = 0xDE97400000000001ull;  // (a guest shader hashing to it: a 2^-64 chance)
        const bool s_depthOnlyGuestPs = [] { const char* v = std::getenv("NFSMW_DEPTH_ONLY_PS"); return v && v[0] == '1'; }();

        Shader* DepthOnlyPixelShader()
        {
            static Shader* const ps = [] {
                auto& slot = s_shaders[kDepthOnlyPsHash];
                slot = std::make_unique<Shader>();
                slot->stage = ShaderStage::Pixel;
                slot->hash = kDepthOnlyPsHash;
                slot->info.ok = true;
                slot->info.constPacked = true;  // (reads no constants: none uploaded for it)
                slot->info.glsl = gpu::kEmptyPixelShaderGlsl;
                return slot.get();
            }();
            return ps;
        }

        // A guest shader made from its words (guest order) and kept in
        // s_shaders under `hash` (pipeline_keys::ShaderHash of them): for
        // LoadShader, and for the pipeline prewarm from the key list's words
        // (the same words translate to the same shader).
        Shader* MakeShader(uint32_t type, uint64_t hash, const uint32_t* words, uint32_t dwords)
        {
            auto& slot = s_shaders[hash];
            slot = std::make_unique<Shader>();
            slot->stage = type == 0 ? ShaderStage::Vertex : ShaderStage::Pixel;
            slot->hash = hash;
            slot->words.assign(words, words + dwords);
            std::vector<uint32_t> ucode(dwords);
            for (uint32_t i = 0; i < dwords; i++)
                ucode[i] = __builtin_bswap32(words[i]);
            slot->info = TranslateShader(slot->stage, ucode.data(), dwords);
            if (!slot->info.ok)
                fprintf(stderr, "[renderer] %s_%016llx: translation failed: %s\n", type == 0 ? "vs" : "ps",
                    (unsigned long long)hash, slot->info.error.c_str());
            // NFSMW_DUMP_GLSL=<dir>: each shader's translation as
            // <dir>/<vs|ps>_<hash>.glsl (the hashes the pass profile prints).
            static const char* dumpGlsl = std::getenv("NFSMW_DUMP_GLSL");
            if (dumpGlsl && slot->info.ok)
            {
                const std::string path = std::format("{}/{}_{:016x}.glsl", dumpGlsl, type == 0 ? "vs" : "ps", hash);
                if (FILE* f = fopen(path.c_str(), "w"))
                {
                    fputs(slot->info.glsl.c_str(), f);
                    fclose(f);
                }
            }
            // Compiled to SPIR-V with its first pipeline, on a pipeline worker.
            return slot.get();
        }

        // IM_LOAD shaders by where they are (LoadShader). A dense frame loads
        // thousands, the same few hundred from the same addresses, and
        // finding each by a hash of all its dwords (FNV-1a, a latency-bound
        // multiply chain) was 2-2.7% of the command processor (Deck
        // profile). An entry keeps the words it was made from and is used
        // only while memory holds exactly those words: a compare, many times
        // cheaper than the hash, and the shader the hash would find.
        // NFSMW_SHADER_CACHE=0 turns it off; NFSMW_CP_CACHE_VERIFY=1 also
        // hashes on every hit and logs any difference.
        struct ShaderCacheEntry
        {
            const uint32_t* words = nullptr;  // where they were loaded from (the key)
            uint32_t dwords = 0, type = 0;
            Shader* shader = nullptr;
            std::vector<uint32_t> copy;  // the words, as loaded (guest order)
        };
        struct ShaderCache
        {
            ShaderCacheEntry entries[1024];
            bool on = false, verify = false;
            uint64_t loads = 0, hits = 0, comparedBytes = 0, mismatches = 0;
        };
        ShaderCache* const s_shaderCache = [] {
            auto* c = new ShaderCache();
            const char* v = std::getenv("NFSMW_SHADER_CACHE");
            c->on = (g_cpOpt & CP_OPT_SHADER_CACHE) && !(v && v[0] == '0');
            c->verify = std::getenv("NFSMW_CP_CACHE_VERIFY") != nullptr;
            return c;
        }();

        // Compiled shaders are cached on disk (GetCachePath()/spirv), keyed by
        // their exact source: new shaders compile once (10-20 ms each on the
        // Steam Machine, a hitch whenever new content appears), then load.
        // NFSMW_SHADER_CACHE=0 disables it.
        const bool s_shaderCacheEnabled = [] { const char* v = std::getenv("NFSMW_SHADER_CACHE"); return !v || v[0] != '0'; }();

        std::filesystem::path ShaderCacheFile(const std::string& source, glslang_stage_t stage, const std::string& preamble)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            auto mix = [&](const void* data, size_t size) {
                for (size_t i = 0; i < size; i++)
                    h = (h ^ static_cast<const uint8_t*>(data)[i]) * 0x100000001B3ull;
            };
            const uint32_t format = 1;  // bump when the compile options below change
            mix(&format, sizeof(format));
            mix(&stage, sizeof(stage));
            mix(preamble.data(), preamble.size());
            mix("\0", 1);
            mix(source.data(), source.size());
            return GetCachePath() / "spirv" / std::format("{:016x}.spv", h);
        }

        std::vector<uint32_t> CompileGlslUncached(const std::string& source, glslang_stage_t stage, std::string& log,
            const std::string& preamble);
        std::atomic<uint64_t> s_shaderCacheHits{ 0 };

        // NFSMW_SPIRV_OPT=1 (off by default): glslang runs SPIRV-Tools'
        // optimizer on the SPIR-V it makes (its performance passes, as
        // glslangValidator -O), which it otherwise leaves as generated. The
        // drivers optimize again; whether this helps the GPU is what the
        // flag is for (Steam Deck at 5 W). Kept apart in the shader cache
        // (its preamble names it).
        const bool s_spirvOpt = [] { const char* v = std::getenv("NFSMW_SPIRV_OPT"); return v && v[0] == '1'; }();
        // HFR M3: large vertex ranges narrowed to a draw's vertices (see the
        // draw's guard loop). NFSMW_NARROW_VERTICES=1/0; on by default with
        // Smooth Motion (game/hfr.h), where the guard stalled on them.
        bool NarrowVerticesOn()
        {
            static const bool on = [] {
                const char* v = std::getenv("NFSMW_NARROW_VERTICES");
                const bool r = v ? v[0] == '1' : hfr::Enabled();
                if (r)
                    fprintf(stderr, "[renderer] large dynamic vertex buffers copied or guarded as each draw's vertices only\n");
                return r;
            }();
            return on;
        }
        // A large vertex range the CPU has written since a draw last read it
        // (a dynamic buffer; once seen, kept): a small cache by base address.
        bool DynamicVertexRange(uint32_t base, uint32_t size)
        {
            struct Entry
            {
                uint32_t base = 0, size = 0;
                uint64_t seen = 0;
                bool dynamic = false;
            };
            static Entry cache[256];
            Entry& e = cache[(base >> 12) & 255];
            const uint64_t now = writewatch::Current();
            if (e.base != base || e.size != size)
            {
                e = { base, size, now, false };
                return false;
            }
            if (!e.dynamic && writewatch::WrittenSince(base, size, e.seen))
                e.dynamic = true;
            e.seen = now;
            return e.dynamic;
        }

        std::vector<uint32_t> CompileGlsl(const std::string& source, glslang_stage_t stage, std::string& log,
            const std::string& shaderPreamble = {})
        {
            const std::string preamble = std::string(s_splitMemory ? "#define SPLIT_MEMORY\n" : "") +
                (s_fmulz ? "#define SB_FMULZ\n" : "") + (s_spirvOpt ? "// NFSMW_SPIRV_OPT\n" : "") +
shaderPreamble;
            if (!s_shaderCacheEnabled)
                return CompileGlslUncached(source, stage, log, preamble);
            std::filesystem::path file = ShaderCacheFile(source, stage, preamble);
            if (FILE* f = fopen(file.c_str(), "rb"))
            {
                std::vector<uint32_t> spirv;
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (size >= 20 && size % 4 == 0)
                {
                    spirv.resize(size_t(size) / 4);
                    if (fread(spirv.data(), 4, spirv.size(), f) != spirv.size() || spirv[0] != 0x07230203u)
                        spirv.clear();
                }
                fclose(f);
                if (!spirv.empty())
                {
                    s_shaderCacheHits.fetch_add(1, std::memory_order_relaxed);
                    return spirv;
                }
            }
            std::vector<uint32_t> spirv = CompileGlslUncached(source, stage, log, preamble);
            if (!spirv.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(file.parent_path(), ec);
                // Unique per thread: two workers may compile the same source.
                std::filesystem::path temp = file;
                temp += std::format(".{}.tmp", std::hash<std::thread::id>{}(std::this_thread::get_id()));
                if (FILE* f = fopen(temp.c_str(), "wb"))
                {
                    bool ok = fwrite(spirv.data(), 4, spirv.size(), f) == spirv.size();
                    ok = fclose(f) == 0 && ok;
                    if (ok)
                        std::filesystem::rename(temp, file, ec);
                    else
                        std::filesystem::remove(temp, ec);
                }
            }
            return spirv;
        }

        std::vector<uint32_t> CompileGlslUncached(const std::string& source, glslang_stage_t stage, std::string& log,
            const std::string& preamble)
        {
            glslang_input_t input{};
            input.language = GLSLANG_SOURCE_GLSL;
            input.stage = stage;
            input.client = GLSLANG_CLIENT_VULKAN;
            input.client_version = GLSLANG_TARGET_VULKAN_1_2;
            input.target_language = GLSLANG_TARGET_SPV;
            input.target_language_version = GLSLANG_TARGET_SPV_1_5;
            input.code = source.c_str();
            input.default_version = 460;
            input.default_profile = GLSLANG_NO_PROFILE;
            input.messages = glslang_messages_t(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
            input.resource = glslang_default_resource();

            std::vector<uint32_t> spirv;
            glslang_shader_t* shader = glslang_shader_create(&input);
            if (!preamble.empty())
                glslang_shader_set_preamble(shader, preamble.c_str());
            if (!glslang_shader_preprocess(shader, &input) || !glslang_shader_parse(shader, &input))
            {
                log = glslang_shader_get_info_log(shader);
                glslang_shader_delete(shader);
                return spirv;
            }
            glslang_program_t* program = glslang_program_create();
            glslang_program_add_shader(program, shader);
            if (!glslang_program_link(program, int(GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)))
            {
                log = glslang_program_get_info_log(program);
            }
            else
            {
                if (s_spirvOpt)
                {
                    // (Since glslang 16.6 its optimizer runs for an
                    // optimize_performance or optimize_size request only.)
                    glslang_spv_options_t options{};
                    options.disable_optimizer = false;
#ifdef GLSLANG_VERSION_GREATER_OR_EQUAL_TO
#if GLSLANG_VERSION_GREATER_OR_EQUAL_TO(16, 6, 0)
                    options.optimize_performance = true;
#endif
#endif
                    glslang_program_SPIRV_generate_with_options(program, stage, &options);
                }
                else
                    glslang_program_SPIRV_generate(program, stage);
                spirv.resize(glslang_program_SPIRV_get_size(program));
                glslang_program_SPIRV_get(program, spirv.data());
            }
            glslang_program_delete(program);
            glslang_shader_delete(shader);
            return spirv;
        }

        // Vulkan pipeline cache, persisted as GetCachePath()/pipelines.bin.
        // Saved every 600 frames when new pipelines appeared, and at exit.
        VkPipelineCache s_pipelineCache = VK_NULL_HANDLE;
        uint32_t s_pipelinesAtSave = 0;

        void CreatePipelineCache()
        {
            std::vector<uint8_t> data;
            if (s_shaderCacheEnabled)
                if (FILE* f = fopen((GetCachePath() / "pipelines.bin").c_str(), "rb"))
                {
                    fseek(f, 0, SEEK_END);
                    long size = ftell(f);
                    fseek(f, 0, SEEK_SET);
                    if (size > 0)
                    {
                        data.resize(size_t(size));
                        if (fread(data.data(), 1, data.size(), f) != data.size())
                            data.clear();
                    }
                    fclose(f);
                }
            // A cache another GPU, driver or OS version made is set aside by
            // the driver without a word (MoltenVK's device ID carries the
            // macOS version, so every macOS update does it), and the first
            // drive afterwards stutters while the pipelines compile again.
            // The header says so in advance (gpu/pipeline_cache_header.h): one
            // line for the log, and the stale data isn't handed over. The
            // next save writes this device's header, so it's said once. No
            // toast: it isn't something to act on, it would come after every
            // SteamOS and macOS update, and the stutter is a frame or two at
            // the first world entry (a first run with no cache at all has
            // more, and says nothing either).
            {
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(s_vk->physical, &props);
                gpu::pipeline_cache::Maker device{ props.vendorID, props.deviceID, {} };
                std::memcpy(device.uuid.data(), props.pipelineCacheUUID, device.uuid.size());
                const auto check = gpu::pipeline_cache::CheckHeader(data.data(), data.size(), device);
                if (const std::string line = gpu::pipeline_cache::LogLine(check, device); !line.empty())
                {
                    fprintf(stderr, "%s\n", line.c_str());
                    data.clear();
                }
            }
            VkPipelineCacheCreateInfo pcci{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
            pcci.initialDataSize = data.size();
            pcci.pInitialData = data.empty() ? nullptr : data.data();
            if (vkCreatePipelineCache(s_dev, &pcci, nullptr, &s_pipelineCache) != VK_SUCCESS)
            {
                pcci.initialDataSize = 0;
                pcci.pInitialData = nullptr;
                Check(vkCreatePipelineCache(s_dev, &pcci, nullptr, &s_pipelineCache), "vkCreatePipelineCache");
            }
            fprintf(stderr, "[renderer] pipeline cache: %zu KB loaded\n", data.size() / 1024);
        }

        // Saves are serialized (the background saver and the one at exit).
        std::mutex s_pipelineSaveMutex;

        void SavePipelineCache()
        {
            if (!s_pipelineCache || !s_shaderCacheEnabled)
                return;
            std::lock_guard saveLock(s_pipelineSaveMutex);
            // Workers may add pipelines between the two calls (VK_INCOMPLETE).
            size_t size = 0;
            std::vector<uint8_t> data;
            for (int attempt = 0;; attempt++)
            {
                if (vkGetPipelineCacheData(s_dev, s_pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0)
                    return;
                data.resize(size);
                VkResult r = vkGetPipelineCacheData(s_dev, s_pipelineCache, &size, data.data());
                if (r == VK_SUCCESS)
                    break;
                if (r != VK_INCOMPLETE || attempt == 3)
                    return;
            }
            std::error_code ec;
            std::filesystem::create_directories(GetCachePath(), ec);
            std::filesystem::path file = GetCachePath() / "pipelines.bin", temp = file;
            temp += ".tmp";
            if (FILE* f = fopen(temp.c_str(), "wb"))
            {
                bool ok = fwrite(data.data(), 1, size, f) == size;
                ok = fclose(f) == 0 && ok;
                if (ok)
                    std::filesystem::rename(temp, file, ec);
                else
                    std::filesystem::remove(temp, ec);
            }
        }

        VkShaderModule CreateModule(const std::vector<uint32_t>& spirv)
        {
            VkShaderModuleCreateInfo smci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            smci.codeSize = spirv.size() * 4;
            smci.pCode = spirv.data();
            VkShaderModule module;
            Check(vkCreateShaderModule(s_dev, &smci, nullptr, &module), "vkCreateShaderModule");
            return module;
        }

        // A pixel shader variant: colour outputs only for bound attachments
        // (Metal rejects others), inputs only for interpolators the vertex
        // shader writes, and the alpha test compiled in only when enabled.
        VkShaderModule VertexModule(Shader& vs, bool rect = false)
        {
            std::lock_guard lock(vs.compileMutex);
            bool& compiled = rect ? vs.rectCompiled : vs.vertexCompiled;
            VkShaderModule& module = rect ? vs.moduleRect : vs.module;
            if (!compiled)
            {
                compiled = true;
                std::string log;
                auto spirv = CompileGlsl(vs.info.glsl, GLSLANG_STAGE_VERTEX, log, rect ? "#define SB_RECT\n" : "");
                if (spirv.empty())
                    fprintf(stderr, "[renderer] vs_%016llx%s: GLSL compile failed:\n%s\n", (unsigned long long)vs.hash, rect ? " (SB_RECT)" : "",
                        log.c_str());
                else
                    module = CreateModule(spirv);
            }
            return module;
        }

        VkShaderModule PixelVariant(Shader& ps, uint32_t outputs, uint32_t inputs, bool alphaTest)
        {
            uint32_t key = (outputs & 0xF) | ((inputs & 0xFFFF) << 4) | (alphaTest ? 1u << 20 : 0);
            std::lock_guard lock(ps.compileMutex);
            auto it = ps.variants.find(key);
            if (it != ps.variants.end())
                return it->second;
            std::string preamble;
            for (int i = 0; i < 4; i++)
                if (outputs & (1u << i))
                    preamble += std::format("#define OUT{}\n", i);
            for (int i = 0; i < 16; i++)
                if (inputs & (1u << i))
                    preamble += std::format("#define IN{}\n", i);
            if (alphaTest)
                preamble += "#define ALPHA_TEST\n";
            std::string log;
            auto spirv = CompileGlsl(ps.info.glsl, GLSLANG_STAGE_FRAGMENT, log, preamble);
            VkShaderModule module = VK_NULL_HANDLE;
            if (spirv.empty())
                fprintf(stderr, "[renderer] ps_%016llx (variant %x): GLSL compile failed:\n%s\n",
                    (unsigned long long)ps.hash, key, log.c_str());
            else
                module = CreateModule(spirv);
            ps.variants[key] = module;
            return module;
        }

        // ---------------------------------------------------------------
        // Layouts: set 0 = shared memory + constants (one static set),
        // set 1 = the textures a shader pair uses (push descriptors).

        VkDescriptorSetLayout s_set0Layout = VK_NULL_HANDLE;
        VkDescriptorPool s_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet s_set0 = VK_NULL_HANDLE;
        VkSampler s_sampler = VK_NULL_HANDLE;
        Image s_dummy[3];  // 2D, 3D, cube: until the texture cache (M4)
        Image s_dummyStorage;  // R32_UINT 1x1: a resolve's texture binding when it stores into none

        struct DrawConstants
        {
            uint32_t vsConstBase, psConstBase, indexAddress, indexInfo, indexOffset, alphaTest;
            float alphaRef;
            uint32_t flags;
            float posScale[4];
            uint32_t vfRedirect[4][4];  // guest start, size, ring offset, 0 (vertex data snapshots)
            float lodBias;              // the computed LODs' bias: log2(scale) below 1x, else 0 (Draw)
            uint32_t pad[3];            // (whole 16-byte rows: draw_state::ChangedRows)
        };
        // 128 bytes: the push-constant size every Vulkan device has
        // (maxPushConstantsSize's minimum), so nothing more fits here.
        static_assert(sizeof(DrawConstants) == 128);

        struct TextureLayout
        {
            VkDescriptorSetLayout set = VK_NULL_HANDLE;
            VkPipelineLayout pipeline = VK_NULL_HANDLE;
            std::array<uint32_t, 3> vsMasks{}, psMasks{};
            // Its bindings in binding order, as Draw looks them up and pushes
            // them: d * 32 + n is fetch constant n sampled as dimension d (2D,
            // 3D, cube). A list, so a draw walks its few textures instead of
            // testing 96 mask bits twice (~6% of the command processor in a
            // race, Deck profile). `fetchSlots`: the fetch constants they read
            // (bit n), whatever the dimension.
            uint32_t bindingCount = 0;
            uint8_t bindings[96] = {};
            uint32_t fetchSlots = 0;
        };
        std::map<std::array<uint32_t, 6>, std::unique_ptr<TextureLayout>> s_textureLayouts;

        const TextureLayout& GetTextureLayoutUncached(const Shader& vs, const Shader& ps);

        const TextureLayout& GetTextureLayout(const Shader& vs, const Shader& ps)
        {
            static const Shader *lastVs = nullptr, *lastPs = nullptr;
            static const TextureLayout* last = nullptr;
            if (&vs == lastVs && &ps == lastPs)
                return *last;
            lastVs = &vs;
            lastPs = &ps;
            last = &GetTextureLayoutUncached(vs, ps);
            return *last;
        }

        const TextureLayout& GetTextureLayoutUncached(const Shader& vs, const Shader& ps)
        {
            std::array<uint32_t, 3> v = { vs.info.texture2DMask, vs.info.texture3DMask, vs.info.textureCubeMask };
            std::array<uint32_t, 3> p = { ps.info.texture2DMask, ps.info.texture3DMask, ps.info.textureCubeMask };
            std::array<uint32_t, 6> key = { v[0], v[1], v[2], p[0], p[1], p[2] };
            auto& slot = s_textureLayouts[key];
            if (slot)
                return *slot;
            slot = std::make_unique<TextureLayout>();
            slot->vsMasks = v;
            slot->psMasks = p;
            std::vector<VkDescriptorSetLayoutBinding> bindings;
            for (uint32_t d = 0; d < 3; d++)
                for (uint32_t n = 0; n < 32; n++)
                {
                    VkShaderStageFlags stages = ((v[d] >> n) & 1 ? VK_SHADER_STAGE_VERTEX_BIT : 0) |
                        ((p[d] >> n) & 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : 0);
                    if (stages)
                    {
                        bindings.push_back({ d * 32 + n, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr });
                        slot->bindings[slot->bindingCount++] = uint8_t(d * 32 + n);
                        slot->fetchSlots |= 1u << n;
                    }
                }
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = uint32_t(bindings.size());
            dslci.pBindings = bindings.data();
            Check(vkCreateDescriptorSetLayout(s_dev, &dslci, nullptr, &slot->set), "vkCreateDescriptorSetLayout(textures)");
            VkDescriptorSetLayout sets[] = { s_set0Layout, slot->set };
            VkPushConstantRange range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(DrawConstants) };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 2;
            plci.pSetLayouts = sets;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &range;
            Check(vkCreatePipelineLayout(s_dev, &plci, nullptr, &slot->pipeline), "vkCreatePipelineLayout");
            return *slot;
        }

        // ---------------------------------------------------------------
        // Render targets (EDRAM)

        struct RenderTarget
        {
            uint32_t base, format, pitch;
            bool depth;
            bool wide;                         // side-by-side tiles drawn untiled (see GetTarget)
            uint32_t width, height;            // host pixels (guest x scale, rounded up)
            uint32_t guestWidth, guestHeight;  // guest pixels
            gpu::RenderScale scale;            // host pixels per guest pixel (gpu/render_scale.h)
            VkFormat vkFormat;
            bool blendable;
            Image image;
            VkImageView sampleView = VK_NULL_HANDLE;  // depth aspect only, for resolves
            // NFSMW_UNTILED_AREA: in the last frame untiled draws drew it (until
            // the next one's first), whether one was cut short of the
            // attachments' common size, what those cut draws drew (their host
            // scissors' union; one cut to nothing adds nothing) and that
            // common size; and whether a resolve has read past them (its
            // draws then keep the whole target).
            uint64_t areaFrame = ~0ull;
            VkRect2D cutDrawn{};
            VkExtent2D cutReach{};
            bool cut = false;
            bool wholeArea = false;
        };
        std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> s_targets;

        VkFormat ColorFormat(uint32_t format)
        {
            switch (xenos::ColorRenderTargetFormat(format))
            {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return VK_FORMAT_R8G8B8A8_UNORM;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
            case xenos::ColorRenderTargetFormat::k_16_16: return VK_FORMAT_R16G16_SNORM;
            case xenos::ColorRenderTargetFormat::k_16_16_16_16: return VK_FORMAT_R16G16B16A16_SNORM;
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
            default: return VK_FORMAT_R8G8B8A8_UNORM;
            }
        }

        bool Is64bpp(uint32_t format)
        {
            switch (xenos::ColorRenderTargetFormat(format))
            {
            case xenos::ColorRenderTargetFormat::k_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: return true;
            default: return false;
            }
        }

        // Untiled rendering (see TileShift); NFSMW_TILING=1 renders per tile.
        const bool s_untiled = [] { const char* v = std::getenv("NFSMW_TILING"); return !(v && v[0] == '1'); }();
        // NFSMW_UNTILED_AREA=1 (default with kLighterPasses): an untiled draw with clipping on
        // is scissored to its guest viewport, not the whole EDRAM-sized
        // target, so its pass's area (what a tile GPU loads and stores:
        // MoltenVK's render target runs from (0,0) to the area's far corner)
        // is the frame: the scene's 1280x720, not 1280x1024, and a cube
        // face's 256x256, not 1280x896. ~130 MB a race frame less (modelled:
        // the area's far corner x the attachments' bytes, loaded and stored);
        // an M4 Max measured -0.09 ms, a phone is unmeasured. The console
        // never draws past the viewport there (its window scissor is the
        // tile inside it); the host clipped only to the target, so the road
        // below the frame and the neighbouring faces' geometry were shaded
        // too. Clipping off, or x and y not both scaled and offset: the whole
        // target, as before. Where cutting would change more than the area,
        // the target falls back to the whole target from then on
        // (RenderTarget::wholeArea), logged: a resolve reading past what its
        // cut draws drew (rows the host used to overdraw; see Resolve), a
        // draw that would restart its pass for its area (no untiled draw
        // did), two cuts whose union isn't exact.
        // =alt (measurement only): on in every other 120-frame [perf] window,
        // a paired A/B within one run.
        // NFSMW_LOG_UNTILED_AREA=1: counts with each [perf] line (draws cut,
        // pass restarts, the passes' area and bytes), and each distinct cut
        // and each shader pair keeping the whole target, once.
        // On by default with kLighterPasses (Apple: iPad Pro M2 GPU-bound at
        // 2x, same-run windows: executing -1.0 ms), else off; =0 turns it off.
        const uint32_t s_untiledArea = [] {
            const char* v = std::getenv("NFSMW_UNTILED_AREA");
            return !v ? (kLighterPasses ? 1u : 0u) : v[0] == '1' ? 1u : strcmp(v, "alt") == 0 ? 2u : 0u;
        }();
        const bool s_logUntiledArea = [] { const char* v = std::getenv("NFSMW_LOG_UNTILED_AREA"); return v && v[0] == '1'; }();
        struct UntiledAreaCounts
        {
            uint64_t cut = 0, whole = 0, clipOff = 0, noViewport = 0, wholeArea = 0, empty = 0, restarts = 0;
            uint64_t passes = 0, passPixels = 0, passBytes = 0;                // every pass: its area, its attachments' bytes there
            uint64_t untiledPasses = 0, untiledPixels = 0, untiledBytes = 0;  // passes on wide or MSAA-tiled targets
            uint64_t resolvesPast = 0;
        } s_ua;

        // Internal resolution. Render targets of the scene's surface pitches
        // (NFSMW_SCALE_PITCH, default 640-1280: the scene, its side-by-side
        // tiles, the road reflection, the final composite) are created at
        // guest size x (sx, sy) host pixels; the rest (the 1600x1600 shadow
        // map, the 256x256 cube faces, the bloom and luminance chain from
        // 320x180 down) stay 1x. The bloom blurs step in texels: at scale
        // they cover a fraction of their area and the game's haze thins (the
        // colour grade visibly changed with 320 scaled), and they are blurry
        // by design anyway. Draws keep the guest's viewport transform in guest pixels
        // and scale only the host viewport and scissor; resolves average each
        // guest pixel's host pixels down into guest memory (depth takes one
        // sample), so the game and the texture cache see 1x data as before.
        // The scale is a ratio on each axis (gpu/render_scale.h): 1, 1.5
        // (3 host pixels for every 2 guest ones: 1920x1080), 2, 3, 4, and on
        // the axis a Vert+ or Hor+ picture extends, the screen's own rows or
        // columns at that density (video::NativeShape: 1x10/9 on a Steam
        // Deck's 1280x800, GitHub #34); and below the 360's 720p 0.75 (3/4:
        // 960x540), 0.625 (5/8: 800x450) and 0.5 (1/2: 640x360), for a GPU
        // that can't afford 720p, whose frame the presenter upscales
        // (Upscaling FSR 1), with no native rows or columns. The 1x targets
        // stay 1x there too: the bloom chain's blurs read the 1x copy of the
        // scene the resolves still write (box-filtered up from the host pixels).
        // NFSMW_RENDER_SCALE=<n> or <sx>x<sy> (1, 1.5, 2, 3 or 4, 0.5, 0.625
        // or 0.75, or a ratio such as 10/9; default 1): exactly that, no
        // native rows or columns.
        // The setting applies live (ApplyLiveSettings): every scaled target,
        // the scaled resolve memory and the scaled textures are made again at
        // the new scale between two frames, and Auto follows the picture's
        // size (the window, fullscreen, the aspect ratio setting).
        gpu::RenderScale s_renderScale;
        uint32_t s_scalePitchMin = 640, s_scalePitchMax = 1280;
        uint32_t s_scaleBudget = 6;  // Auto's most host pixels per guest pixel
        bool s_scaleFixed = false;   // NFSMW_RENDER_SCALE: no live changes
        // Native rows and columns on a Vert+ or Hor+ picture (video::NativeShape);
        // NFSMW_NATIVE_SHAPE=0 keeps the setting's scale on both axes, as
        // before (an A/B switch).
        const bool s_nativeShape = [] {
            const char* v = std::getenv("NFSMW_NATIVE_SHAPE");
            return !(v && v[0] == '0');
        }();

        // What the Internal Resolution setting asks for with the picture
        // covering `regionW` x `regionH` of the swapchain.
        // The setting's value is the scale in eighths (settings.h: 8 is 1x,
        // 12 is 1.5x, 16 is 2x, 24 is 3x; below 720p 6 is 0.75x, 5 0.625x
        // and 4 0.5x): that many host pixels for each guest one across a
        // Vert+ picture (down a Hor+ one), and from 1x up, on the axis it
        // extends, the picture's own shape at that density, 1:1 on the
        // screen at 1x on a Steam Deck (video::NativeShape). A 16:9 picture
        // gets the setting's scale both ways, as before. Auto (whole scales
        // only, for now: video::AutoRenderScale) could pick 1.5x by
        // returning one; it never goes below 1x (that is the player's call,
        // for a weak GPU).
        gpu::RenderScale WantedRenderScale(int32_t setting, uint32_t regionW, uint32_t regionH)
        {
            if (setting > 0)
            {
                gpu::AxisScale a;
                if (!gpu::AxisFromEighths(uint32_t(setting), a))
                    a = {};  // (not one of the entries: 1x)
                return s_nativeShape && !a.Below() ? video::NativeShape(a, regionW, regionH) : gpu::RenderScale{ a, a };
            }
            uint32_t sx = 1, sy = 1;
            // Auto: enough host pixels to cover the screen area the frame
            // fills (video/picture_fit: 3440x1440 filled: 3x2; a 1440p 16:9
            // box: 2x2; a Steam Deck's 1280x800 filled with Vert+: 1x1),
            // within a budget of sx x sy host pixels per guest pixel: 6 on a
            // discrete GPU, 4 on an integrated one (the M1 Pro holds a race
            // at 2x2), 9 on a Mac with an M3 or later Max or Ultra, and on an
            // iPhone or iPad the chip's (video/scale_budget: 1 on every chip
            // measured so far).
            // HFR (game/hfr.h, NFSMW_HFR=1): two pictures a step, so half
            // the budget, and the same scale both ways (a 1x2 picture is
            // half as sharp across: M1 Pro at 2x2 under HFR drew 64-102 fps,
            // and 5.3-8.2 ms of GPU a picture at 1x, near its 8.3 ms).
            // Only where the screen can show them (video::HfrOffered: Smooth
            // Motion on, a 60 Hz screen draws none).
            if (hfr::Enabled() && video::HfrOffered())
            {
                video::AutoRenderScale(regionW, regionH, std::max(1u, s_scaleBudget / 2), sx, sy);
                sx = sy = std::min(sx, sy);
                return gpu::RenderScale::Whole(sx, sy);
            }
            video::AutoRenderScale(regionW, regionH, s_scaleBudget, sx, sy);
            // A Vert+ or Hor+ picture's own rows or columns at that scale,
            // within the budget too (video::NativeAuto: the Deck's 1x10/9).
            return s_nativeShape ? video::NativeAuto(sx, sy, regionW, regionH, s_scaleBudget) : gpu::RenderScale::Whole(sx, sy);
        }

        std::string ScaleReason(int32_t setting, uint32_t regionW, uint32_t regionH)
        {
            if (s_scaleFixed)
                return "NFSMW_RENDER_SCALE";
            // Native rows or columns: unlike the setting's (Auto's) scale on its own.
            gpu::RenderScale plain;
            if (setting > 0)
            {
                gpu::AxisScale a;
                if (!gpu::AxisFromEighths(uint32_t(setting), a))
                    a = {};
                plain = { a, a };
            }
            else
            {
                uint32_t sx = 1, sy = 1;
                const bool hfrAuto = hfr::Enabled() && video::HfrOffered();
                video::AutoRenderScale(regionW, regionH, hfrAuto ? std::max(1u, s_scaleBudget / 2) : s_scaleBudget, sx, sy);
                if (hfrAuto)
                    sx = sy = std::min(sx, sy);
                plain = gpu::RenderScale::Whole(sx, sy);
            }
            const gpu::RenderScale want = WantedRenderScale(setting, regionW, regionH);
            const char* native = want == plain ? "" : !(want.y == plain.y) ? ", the picture's own rows" : ", the picture's own columns";
            if (setting > 0)
                return std::format("setting{}{}", native, *native ? std::format(" for a {}x{} picture", regionW, regionH) : "");
            return std::format("auto for a {}x{} picture{}{}", regionW, regionH, native,
                               hfr::Enabled() && video::HfrOffered() ? ", half the budget for Smooth Motion's two pictures a step" : "");
        }

        void ReadRenderScale(const VkPhysicalDeviceProperties& props)
        {
            // Settings > Video > Internal Resolution; NFSMW_RENDER_SCALE
            // (<n> or <sx>x<sy>, developer override) wins.
            s_scaleBudget = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 6 : 4;
#if defined(__APPLE__) && TARGET_OS_IOS
            // iPhones and iPads: chip by chip (video/scale_budget), more than
            // 1x1 only where a chip was measured to hold a race at 60 fps with
            // it and stay out of serious thermal throttling (none yet: the A20
            // Pro gets hot at 1x1 already), and 1x1 on a chip not measured.
            // Scripted drive, GPU time a frame: iPhone 15 Pro Max (A17 Pro)
            // 15 ms at 1x, 36-40 at 2x; iPad Pro M2 20 ms at 2x (43-50 fps).
            {
                const video::ScaleBudget mobile = video::MobileScaleBudget(props.deviceName);
                s_scaleBudget = mobile.pixels;
                fprintf(stderr, "[renderer] Auto internal resolution: at most %u host pixel%s per guest pixel on the %s (%s)\n",
                    s_scaleBudget, s_scaleBudget == 1 ? "" : "s", props.deviceName, mobile.reason);
            }
#elif defined(__APPLE__)
            // Apple GPUs are all integrated, but a recent Max or Ultra has
            // the headroom for 3x3 (4K). Scripted race, GPU time a frame, M4
            // Max: 5-6.6 ms at 2x2, 9-11 ms at 3x3, 60 fps locked.
            // An M1 or M2 Max has about half that GPU (its 3x3 would be ~20
            // ms): from the M3 on only. MoltenVK names the device after the
            // Metal one ("Apple M4 Max").
            std::string_view name = props.deviceName;
            if (name.starts_with("Apple M") && (name.ends_with(" Max") || name.ends_with(" Ultra")))
            {
                unsigned generation = 0;
                for (char c : name.substr(7))
                {
                    if (c < '0' || c > '9')
                        break;
                    generation = generation * 10 + unsigned(c - '0');
                }
                if (generation >= 3)
                    s_scaleBudget = 9;
            }
#endif
            int32_t setting = settings::GetInt(settings::Id::InternalResolution);
            uint32_t regionW = 0, regionH = 0;
            const char* fixed = std::getenv("NFSMW_RENDER_SCALE");
            if (fixed && gpu::ParseRenderScale(fixed, s_renderScale))
                s_scaleFixed = true;
            else
            {
                if (fixed)
                    fprintf(stderr, "[renderer] NFSMW_RENDER_SCALE=%s ignored: each axis takes 1, 1.5, 2, 3 or 4, below 720p "
                        "0.5, 0.625 or 0.75, or a ratio (\"1.5\", \"3x2\", \"0.75\", \"1x10/9\")\n", fixed);
                video::FrameRegion(regionW, regionH);
                s_renderScale = WantedRenderScale(setting, regionW, regionH);
            }
            if (const char* v = std::getenv("NFSMW_SCALE_PITCH"))
                sscanf(v, "%u-%u", &s_scalePitchMin, &s_scalePitchMax);
            fprintf(stderr, "[renderer] internal resolution %s (%ux%u, %s; surface pitches %u-%u)\n", gpu::ScaleName(s_renderScale).c_str(),
                s_renderScale.x.Size(1280), s_renderScale.y.Size(720), ScaleReason(setting, regionW, regionH).c_str(), s_scalePitchMin,
                s_scalePitchMax);
        }

        bool ScaledPitch(uint32_t pitch)
        {
            return pitch >= s_scalePitchMin && pitch <= s_scalePitchMax;
        }

        // Untiled rendering draws tile 0 over the whole frame (TileShift).
        // With tiles side by side (NFSMW switches to two 640-wide tiles in
        // some scenes) the frame is wider than the surface pitch: those
        // targets get the full 720p width.
        constexpr uint32_t kUntiledWidth = 1280;

        // ~5 lookups a draw: a small direct-mapped cache before the map
        // (targets are freed only when the internal resolution changes,
        // which clears it).
        struct RecentTarget { uint64_t key = ~0ull; RenderTarget* target = nullptr; };
        RecentTarget s_recentTargets[16];

        // A new target, at the current scale if its pitch is a scaled one,
        // cleared to zero (new EDRAM contents).
        std::unique_ptr<RenderTarget> CreateTarget(uint32_t base, uint32_t format, bool depth, uint32_t pitch, bool wide)
        {
            // Height: whatever fits in EDRAM (2048 tiles of 80x16 32-bit
            // samples) from this base, capped at the 2048-line maximum.
            uint32_t tilesPerRow = std::max(1u, (pitch + 79) / 80) * (!depth && Is64bpp(format) ? 2 : 1);
            uint32_t rows = std::clamp((2048u - std::min(base, 2047u)) / tilesPerRow * 16, 16u, 2048u);

            auto rt = std::make_unique<RenderTarget>();
            rt->base = base;
            rt->format = format;
            rt->pitch = pitch;
            rt->depth = depth;
            rt->wide = wide;
            rt->guestWidth = wide ? kUntiledWidth : pitch;
            rt->guestHeight = rows;
            if (ScaledPitch(pitch))
                rt->scale = s_renderScale;
            // (Rounded up: a pitch that isn't whole blocks at 1.5x still
            // has room for its last host column.)
            rt->width = rt->scale.x.Size(rt->guestWidth);
            rt->height = rt->scale.y.Size(rt->guestHeight);
            rt->vkFormat = depth ? VK_FORMAT_D32_SFLOAT_S8_UINT : ColorFormat(format);
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(s_vk->physical, rt->vkFormat, &fp);
            rt->blendable = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
            VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
            rt->image = CreateImage(VK_IMAGE_TYPE_2D, rt->vkFormat, { rt->width, rt->height, 1 }, 1, usage,
                VK_IMAGE_VIEW_TYPE_2D, aspect);
            if (depth)
            {
                VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                vci.image = rt->image.image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = rt->vkFormat;
                vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                Check(vkCreateImageView(s_dev, &vci, nullptr, &rt->sampleView), "vkCreateImageView(depth)");
            }
            else
            {
                rt->sampleView = rt->image.view;
            }

            // New EDRAM contents: zero.
            EndPass();
            Begin();
            ToGeneral(rt->image.image, aspect, 1);
            VkImageSubresourceRange range{ aspect, 0, 1, 0, 1 };
            if (depth)
            {
                VkClearDepthStencilValue v{ 0.0f, 0 };
                vkCmdClearDepthStencilImage(s_cmd, rt->image.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            }
            else
            {
                VkClearColorValue v{};
                vkCmdClearColorImage(s_cmd, rt->image.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
            }
            FullBarrier();

            fprintf(stderr, "[renderer] EDRAM target: base %u %s format %u pitch %u -> %ux%u%s\n",
                base, depth ? "depth" : "color", format, pitch, rt->guestWidth, rt->guestHeight,
                !rt->scale.IsOne() ? std::format(" (host {}x{})", rt->width, rt->height).c_str() : "");
            return rt;
        }

        void DestroyTarget(RenderTarget& rt)
        {
            ForgetRenamedImage(rt.image.image);
            if (rt.sampleView && rt.sampleView != rt.image.view)
                vkDestroyImageView(s_dev, rt.sampleView, nullptr);
            rt.sampleView = VK_NULL_HANDLE;
            DestroyImage(rt.image);
        }

        RenderTarget* GetTarget(uint32_t base, uint32_t format, bool depth, uint32_t pitch)
        {
            bool wide = s_untiled && Reg<reg::RB_SURFACE_INFO>().msaa_samples != xenos::MsaaSamples::k1X &&
                Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable && pitch < kUntiledWidth;
            uint64_t key = uint64_t(base) | (uint64_t(format) << 12) | (uint64_t(depth) << 16) | (uint64_t(pitch) << 17) |
                (uint64_t(wide) << 31);
            RecentTarget& hit = s_recentTargets[(key ^ (key >> 12) ^ (key >> 17)) & 15];
            if (hit.key == key)
                return hit.target;
            auto& slot = s_targets[key];
            if (!slot)
                slot = CreateTarget(base, format, depth, pitch, wide);
            hit = { key, slot.get() };
            return slot.get();
        }

        // Current dynamic rendering pass.
        RenderTarget* s_passColor[4] = {};
        RenderTarget* s_passDepth = nullptr;
        bool s_passActive = false;
        VkExtent2D s_passExtent{};  // the attachments' common size
        VkRect2D s_passArea{};      // what this pass loads and stores (tile GPUs move only this)
        // NFSMW_PASS_MERGE bitmask (Draw; default 3 with kFewerPasses, else
        // 0), fewer passes around the sun: 1 skip draws that write nothing (its
        // occlusion quad), 2 draw into the open pass a draw whose targets are
        // some of its (its glow). In every view with the sun in it (the
        // mirror, up to three cube faces, the scene) the pair split the
        // view's pass in three: 10 of the 46 passes of an iPad race frame's
        // profile. Expected ~0.4-0.5 ms of its GPU time (bit 1 ~0.2, bit 2
        // ~0.15-0.25: the glow and flare draws still run).
        const uint32_t s_passMerge = [] { const char* v = std::getenv("NFSMW_PASS_MERGE"); return v ? uint32_t(std::atoi(v)) : kFewerPasses ? 3u : 0u; }();

        void EndPass(PassBreak why)
        {
            if (!s_passActive)
                return;
            ZpdEndSegment();  // a segment never outlives its pass
            if (s_passSecondary)
            {
                // NFSMW_PREPASS: the pre-pass, then the pass, in its rendering.
                Check(vkEndCommandBuffer(s_preCmd), "vkEndCommandBuffer (pre-pass)");
                Check(vkEndCommandBuffer(s_cmd), "vkEndCommandBuffer (pass)");
                const VkCommandBuffer secondaries[2] = { s_preCmd, s_cmd };
                s_cmd = s_primaryCmd;
                s_deferredRendering.ri.flags |= VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT_KHR;
                s_vk->cmdBeginRendering(s_cmd, &s_deferredRendering.ri);
                vkCmdExecuteCommands(s_cmd, 2, secondaries);
                s_vk->cmdEndRendering(s_cmd);
                s_passSecondary = false;
                // (The submission's state is undefined after them.)
                s_stateEpoch++;
                s_pushEpoch++;
            }
            else
                s_vk->cmdEndRendering(s_cmd);
            if (s_profiling && s_passEndPending && s_passStats && s_passInfos.back().stats)
            {
                vkCmdEndQuery(s_cmd, s_passStats, s_passInfos.back().query / 2);
                if (s_passSamples)
                    vkCmdEndQuery(s_cmd, s_passSamples, s_passInfos.back().query / 2);
            }
            s_passActive = false;
            s_passBreak = why;
            std::copy(s_passColor, s_passColor + 4, s_lastPassTargets);
            s_lastPassTargets[4] = s_passDepth;
            if (s_profiling && s_passEndPending)
            {
                vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_passQueries, s_passInfos.back().query + 1);
                s_passEndPending = false;
            }
            PassEndBarrier();
            SignalWrites();
        }

        // NFSMW_PASS_PROFILE: compute work outside the passes (a resolve, a
        // texture's load...) timed as an entry of its own, `desc` naming it:
        // ProfileCompute before its dispatch (outside a pass, recording),
        // ProfileComputeEnd after it and its barrier. Off: one test each.
        void ProfileCompute(const char* desc)
        {
            if (!s_profiling || s_passQueryNext + 2 > 512)
                return;
            vkCmdResetQueryPool(s_cmd, s_passQueries, s_passQueryNext, 2);
            vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_passQueries, s_passQueryNext);
            GapCounters now{ s_stats.resolves, s_stats.uploads, s_stats.submits, writewatch::GetGuardStats().uploadBytes };
            s_passInfos.push_back({ desc, 0, s_slot, s_passQueryNext, uint32_t(now.resolves - s_gapLast.resolves),
                uint32_t(now.uploads - s_gapLast.uploads), uint32_t(now.submits - s_gapLast.submits), (now.shadowBytes - s_gapLast.shadowBytes) / 1024 });
            s_gapLast = now;
            s_passQueryNext += 2;
            s_passEndPending = true;
        }

        void ProfileComputeEnd()
        {
            if (s_profiling && s_passEndPending)
            {
                vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s_passQueries, s_passInfos.back().query + 1);
                s_passEndPending = false;
            }
        }

        // Guest rectangle [x0, x1) x [y0, y1) in host pixels at `scale`: its
        // edges' (gpu/render_scale.h; a negative edge is 0), so rectangles
        // that share a guest edge share the host one.
        VkRect2D HostRect(const gpu::RenderScale& scale, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
        {
            const int32_t hx0 = scale.x.EdgeAt(x0), hy0 = scale.y.EdgeAt(y0), hx1 = scale.x.EdgeAt(x1), hy1 = scale.y.EdgeAt(y1);
            return { { hx0, hy0 }, { uint32_t(std::max(hx1 - hx0, 0)), uint32_t(std::max(hy1 - hy0, 0)) } };
        }

        bool Contains(const VkRect2D& outer, const VkRect2D& inner)
        {
            return inner.offset.x >= outer.offset.x && inner.offset.y >= outer.offset.y &&
                inner.offset.x + int32_t(inner.extent.width) <= outer.offset.x + int32_t(outer.extent.width) &&
                inner.offset.y + int32_t(inner.extent.height) <= outer.offset.y + int32_t(outer.extent.height);
        }

        VkRect2D Union(const VkRect2D& a, const VkRect2D& b)
        {
            int32_t x0 = std::min(a.offset.x, b.offset.x), y0 = std::min(a.offset.y, b.offset.y);
            int32_t x1 = std::max(a.offset.x + int32_t(a.extent.width), b.offset.x + int32_t(b.extent.width));
            int32_t y1 = std::max(a.offset.y + int32_t(a.extent.height), b.offset.y + int32_t(b.extent.height));
            return { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
        }

        bool Overlaps(const VkRect2D& a, const VkRect2D& b)
        {
            return a.offset.x < b.offset.x + int32_t(b.extent.width) && b.offset.x < a.offset.x + int32_t(a.extent.width) &&
                a.offset.y < b.offset.y + int32_t(b.extent.height) && b.offset.y < a.offset.y + int32_t(a.extent.height);
        }

        // Stacked or side by side with a whole edge in common: their union is
        // exactly the two.
        bool Adjoins(const VkRect2D& a, const VkRect2D& b)
        {
            int32_t ax1 = a.offset.x + int32_t(a.extent.width), ay1 = a.offset.y + int32_t(a.extent.height);
            int32_t bx1 = b.offset.x + int32_t(b.extent.width), by1 = b.offset.y + int32_t(b.extent.height);
            if (a.offset.x == b.offset.x && ax1 == bx1)
                return ay1 == b.offset.y || by1 == a.offset.y;
            if (a.offset.y == b.offset.y && ay1 == by1)
                return ax1 == b.offset.x || bx1 == a.offset.x;
            return false;
        }

        // `area` (in target pixels) is what the next draw or clear touches; a
        // pass on the same targets is kept while it covers that, else it is
        // restarted over the union.
        // Resolve clears are deferred into the next pass on their target (a
        // pass of their own would load and store the attachments on a tile
        // GPU): a load-op clear when they cover the render area, else an
        // in-pass clear.
        // Untiled rendering of predicated tiling. At 720p with 4x MSAA the
        // game renders the scene once per EDRAM tile, 3 tiles of 256 rows
        // (window offsets 0, -256, -512), and resolves each tile. The host
        // has no EDRAM limit (and renders at 1x), so tile 0's draws cover the
        // whole target, later tiles' draws are skipped, and each tile's
        // resolve and clear read the target at screen coordinates (EDRAM
        // rectangle minus the window offset). NFSMW_TILING=1 keeps the
        // per-tile rendering.

        // The window offset to undo for the current MSAA surface, or 0,0.
        std::pair<int32_t, int32_t> TileShift()
        {
            if (!s_untiled || Reg<reg::RB_SURFACE_INFO>().msaa_samples == xenos::MsaaSamples::k1X ||
                !Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
                return { 0, 0 };
            auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
            return { -wo.window_x_offset, -wo.window_y_offset };
        }

        // Clears by how they ran: [depth][0 a load op, 1 in the pass].
        struct ClearCounts { struct Kind { uint64_t count = 0, pixels = 0; } kind[2][2]; } s_clearCounts;
        struct PendingClear
        {
            RenderTarget* rt;
            VkRect2D rect;
            VkClearValue value;
        };
        std::vector<PendingClear> s_pendingClears;
        // NFSMW_CLEAR_FLUSH_RECT=1 (default kFewerPasses; 0: a resolve runs
        // every pending clear of its source): a resolve runs only the pending clears of
        // what it reads (FlushPendingClears), and a clear adjoining its
        // target's newest pending one of the same value extends it
        // (ClearRect). The 4 empty passes a race frame spent on the scene's
        // tile bands, ~0.2-0.3 ms of the iPad's GPU time. The clears kept
        // wait for the next pass on their target, which must hold them
        // (BeginPass logs one that reaches past its pass).
        const bool s_clearFlushRect = [] { const char* v = std::getenv("NFSMW_CLEAR_FLUSH_RECT"); return v ? v[0] == '1' : kFewerPasses; }();

        void BeginPass(RenderTarget* const color[4], RenderTarget* depth, VkRect2D area)
        {
            bool sameTargets = s_passActive && depth == s_passDepth && std::equal(color, color + 4, s_passColor);
            if (sameTargets && Contains(s_passArea, area))
                return;
            if (sameTargets)
            {
                area = Union(area, s_passArea);
                if (s_logUntiledArea)
                    s_ua.restarts++;
            }
            auto attached = [&](RenderTarget* rt) { return rt == depth || std::find(color, color + 4, rt) != color + 4; };
            for (const PendingClear& pc : s_pendingClears)
                if (attached(pc.rt))
                    area = Union(area, pc.rect);
            EndPass(kBreakTargets);
            Begin();
            uint32_t w = UINT32_MAX, h = UINT32_MAX;
            VkRenderingAttachmentInfoKHR colors[4]{};
            for (int i = 0; i < 4; i++)
            {
                s_passColor[i] = color[i];
                colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR;
                colors[i].imageView = color[i] ? color[i]->image.view : VK_NULL_HANDLE;
                colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                if (color[i])
                {
                    w = std::min(w, color[i]->width);
                    h = std::min(h, color[i]->height);
                }
            }
            s_passDepth = depth;
            VkRenderingAttachmentInfoKHR ds{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
            if (depth)
            {
                ds.imageView = depth->image.view;
                ds.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                ds.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                ds.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                w = std::min(w, depth->width);
                h = std::min(h, depth->height);
            }
            s_passExtent = { w, h };
            // Clamp to the attachments.
            int32_t x0 = std::clamp(area.offset.x, 0, int32_t(w)), y0 = std::clamp(area.offset.y, 0, int32_t(h));
            int32_t x1 = std::clamp(area.offset.x + int32_t(area.extent.width), x0, int32_t(w));
            int32_t y1 = std::clamp(area.offset.y + int32_t(area.extent.height), y0, int32_t(h));
            s_passArea = { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
            if (s_logUntiledArea)
            {
                // What Metal loads and stores: MoltenVK's render target runs
                // from (0,0) to the area's far corner. D32S8: 4 + 1 bytes.
                uint64_t px = uint64_t(x1) * uint64_t(y1), bpp = depth ? 5 : 0;
                bool untiled = depth && depth->wide;
                for (int i = 0; i < 4; i++)
                    if (color[i])
                    {
                        bpp += Is64bpp(color[i]->format) ? 8 : 4;
                        untiled = untiled || color[i]->wide;
                    }
                untiled = untiled || (s_untiled && Reg<reg::RB_SURFACE_INFO>().msaa_samples != xenos::MsaaSamples::k1X &&
                    Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable);
                s_ua.passes++;
                s_ua.passPixels += px;
                s_ua.passBytes += 2 * px * bpp;
                if (untiled)
                {
                    s_ua.untiledPasses++;
                    s_ua.untiledPixels += px;
                    s_ua.untiledBytes += 2 * px * bpp;
                }
            }

            // Pending clears of these targets: whole-area ones become load ops.
            std::vector<PendingClear> inPass;
            for (auto it = s_pendingClears.begin(); it != s_pendingClears.end();)
            {
                if (!attached(it->rt))
                {
                    ++it;
                    continue;
                }
                bool whole = Contains(it->rect, s_passArea);
                // One reaching past the attachments' common size (a target
                // larger than another attached) is consumed here, but the
                // part beyond this pass is never cleared; one not covering
                // the pass is also a vkCmdClearAttachments outside the render
                // area (invalid usage). A deferred clear relies on its
                // target's next pass holding it, and NFSMW_CLEAR_FLUSH_RECT
                // defers more of them: with it, logged once per target.
                if (s_clearFlushRect && !Contains(s_passArea, it->rect))
                    LogOnce((0x27ull << 56) | (uint64_t(it->rt->base) << 24) | (uint64_t(it->rt->pitch) << 1) | uint64_t(it->rt->depth),
                        "pending clear %ux%u@%d,%d of target b%u p%u%s reaches past its pass (area %ux%u@%d,%d): the rest is not cleared%s",
                        it->rect.extent.width, it->rect.extent.height, it->rect.offset.x, it->rect.offset.y, it->rt->base, it->rt->pitch,
                        it->rt->depth ? " (depth)" : "", s_passArea.extent.width, s_passArea.extent.height, s_passArea.offset.x,
                        s_passArea.offset.y, whole ? "" : " (cleared in the pass: a vkCmdClearAttachments rect outside its render area)");
                VkRenderingAttachmentInfoKHR* att = nullptr;
                if (it->rt == depth)
                    att = &ds;
                else
                    for (int i = 0; i < 4; i++)
                        if (color[i] == it->rt)
                            att = &colors[i];
                if (s_exp & (it->rt->depth ? 1024 : 512))
                {
                    it = s_pendingClears.erase(it);
                    continue;
                }
                {
                    // [clears] (with NFSMW_PASS_BARRIER's line): how each clear runs.
                    ClearCounts::Kind& k = s_clearCounts.kind[it->rt->depth][whole && att ? 0 : 1];
                    k.count++;
                    k.pixels += uint64_t(it->rect.extent.width) * it->rect.extent.height;
                }
                if (whole && att)
                {
                    // Supersedes any older partial clear of this target.
                    std::erase_if(inPass, [&](const PendingClear& pc) { return pc.rt == it->rt; });
                    att->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    att->clearValue = it->value;
                }
                else
                {
                    inPass.push_back(*it);
                }
                it = s_pendingClears.erase(it);
            }
            VkRenderingInfoKHR ri{ VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
            ri.renderArea = s_passArea;
            ri.layerCount = 1;
            ri.colorAttachmentCount = 4;
            ri.pColorAttachments = colors;
            ri.pDepthAttachment = depth ? &ds : nullptr;
            ri.pStencilAttachment = depth ? &ds : nullptr;
            const bool secondary = s_prepassOn && s_dynamicState && depth && std::any_of(color, color + 4, [](RenderTarget* t) { return t != nullptr; });
            if (s_profiling && s_passQueryNext + 2 <= 512)
            {
                char desc[256];
                int n = snprintf(desc, sizeof(desc), "area %ux%u@%d,%d", s_passArea.extent.width, s_passArea.extent.height,
                    s_passArea.offset.x, s_passArea.offset.y);
                for (int i = 0; i < 4; i++)
                    if (color[i])
                        n += snprintf(desc + n, sizeof(desc) - n, " C%d[b%u f%u p%u]", i, color[i]->base, color[i]->format, color[i]->pitch);
                if (depth)
                    n += snprintf(desc + n, sizeof(desc) - n, " D[b%u p%u]", depth->base, depth->pitch);
                vkCmdResetQueryPool(s_cmd, s_passQueries, s_passQueryNext, 2);
                vkCmdWriteTimestamp(s_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s_passQueries, s_passQueryNext);
                GapCounters now{ s_stats.resolves, s_stats.uploads, s_stats.submits, writewatch::GetGuardStats().uploadBytes };
                s_passInfos.push_back({ desc, 0, s_slot, s_passQueryNext, uint32_t(now.resolves - s_gapLast.resolves),
                    uint32_t(now.uploads - s_gapLast.uploads), uint32_t(now.submits - s_gapLast.submits), (now.shadowBytes - s_gapLast.shadowBytes) / 1024 });
                PassInfo& pi = s_passInfos.back();
                if (s_passStats && !secondary)
                {
                    vkCmdResetQueryPool(s_cmd, s_passStats, pi.query / 2, 1);
                    vkCmdBeginQuery(s_cmd, s_passStats, pi.query / 2, 0);
                    if (s_passSamples)
                    {
                        vkCmdResetQueryPool(s_cmd, s_passSamples, pi.query / 2, 1);
                        vkCmdBeginQuery(s_cmd, s_passSamples, pi.query / 2, VK_QUERY_CONTROL_PRECISE_BIT);
                    }
                    pi.stats = true;
                    pi.areaPx = uint64_t(s_passArea.extent.width) * s_passArea.extent.height;
                }
                pi.why = s_passBreak;
                pi.same = std::equal(color, color + 4, s_lastPassTargets) && s_lastPassTargets[4] == depth;
                pi.submitWhy = s_passBreakSubmit;
                s_gapLast = now;
                s_passQueryNext += 2;
                s_passEndPending = true;
            }
            if (secondary)
            {
                s_deferredRendering.ri = ri;
                std::copy(colors, colors + 4, s_deferredRendering.colors);
                s_deferredRendering.ds = ds;
                s_deferredRendering.ri.pColorAttachments = s_deferredRendering.colors;
                s_deferredRendering.ri.pDepthAttachment = &s_deferredRendering.ds;
                s_deferredRendering.ri.pStencilAttachment = &s_deferredRendering.ds;
                VkFormat formats[4];
                for (int i = 0; i < 4; i++)
                    formats[i] = color[i] ? color[i]->vkFormat : VK_FORMAT_UNDEFINED;
                VkCommandBufferInheritanceRenderingInfoKHR inherit{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO_KHR };
                inherit.colorAttachmentCount = 4;
                inherit.pColorAttachmentFormats = formats;
                inherit.depthAttachmentFormat = depth->vkFormat;
                inherit.stencilAttachmentFormat = depth->vkFormat;
                inherit.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkCommandBufferInheritanceInfo ii{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, &inherit };
                VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
                bi.pInheritanceInfo = &ii;
                auto take = [] {
                    Submission& sub = s_submissions[s_slot];
                    if (sub.secondaryUsed == sub.secondaries.size())
                    {
                        VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
                        cbai.commandPool = s_commandPool;
                        cbai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
                        cbai.commandBufferCount = 1;
                        VkCommandBuffer cb;
                        Check(vkAllocateCommandBuffers(s_dev, &cbai, &cb), "vkAllocateCommandBuffers (secondary)");
                        sub.secondaries.push_back(cb);
                    }
                    return sub.secondaries[sub.secondaryUsed++];
                };
                s_preCmd = take();
                const VkCommandBuffer pass = take();
                Check(vkBeginCommandBuffer(s_preCmd, &bi), "vkBeginCommandBuffer (pre-pass)");
                Check(vkBeginCommandBuffer(pass, &bi), "vkBeginCommandBuffer (pass)");
                s_primaryCmd = s_cmd;
                s_cmd = pass;
                s_passSecondary = true;
                s_stateEpoch++;
                s_preEpoch++;
                s_prepassCounts.passes++;
            }
            else
                s_vk->cmdBeginRendering(s_cmd, &ri);
            s_passActive = true;
            s_stats.passes++;
            s_hle.passes += s_hleCounting;
            for (const PendingClear& pc : inPass)
            {
                VkClearAttachment ca{};
                ca.clearValue = pc.value;
                if (pc.rt == depth)
                    ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                else
                {
                    ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    for (uint32_t i = 0; i < 4; i++)
                        if (color[i] == pc.rt)
                            ca.colorAttachment = i;
                }
                VkClearRect cr{ pc.rect, 0, 1 };
                vkCmdClearAttachments(secondary ? s_preCmd : s_cmd, 1, &ca, 1, &cr);  // (before a pre-pass)
            }
        }

        // Before something reads `rt` outside a pass (a resolve): run its
        // pending clears. Given what it reads (`read`, target pixels; see
        // s_clearFlushRect), only those overlapping that, and older ones
        // overlapping those (they must land first); the rest stay pending for
        // the next pass on `rt`. Each tile resolve of the untiled scene reads
        // the band after the last tile's clear: four passes of their own a
        // frame, each a Metal render encoder and a FullBarrier.
        void FlushPendingClears(RenderTarget* rt, const VkRect2D* read = nullptr)
        {
            std::vector<PendingClear> kept;
            if (read)
            {
                // Newest first, so `run` (the read, then each clear that runs
                // now) holds every newer clear an older one must land under.
                std::vector<VkRect2D> run{ *read };
                for (size_t i = s_pendingClears.size(); i-- > 0;)
                {
                    if (s_pendingClears[i].rt != rt)
                        continue;
                    const VkRect2D rect = s_pendingClears[i].rect;
                    if (std::any_of(run.begin(), run.end(), [&](const VkRect2D& o) { return Overlaps(o, rect); }))
                        run.push_back(rect);
                    else
                    {
                        kept.insert(kept.begin(), s_pendingClears[i]);
                        s_pendingClears.erase(s_pendingClears.begin() + i);
                    }
                }
            }
            for (const PendingClear& pc : s_pendingClears)
                if (pc.rt == rt)
                {
                    RenderTarget* none[4] = {};
                    RenderTarget* color[4] = { rt };
                    BeginPass(rt->depth ? none : color, rt->depth ? rt : nullptr, pc.rect);
                    EndPass();
                    break;
                }
            // (In their order: the pass took every other clear of `rt`.)
            s_pendingClears.insert(s_pendingClears.end(), kept.begin(), kept.end());
        }

        // Levels 2-3 (see s_passBarrier): before a draw whose vertex shader may
        // read what a resolve wrote since the last full barrier (its vertex
        // data, the indices it fetches itself: `indexBytes` from `indexBase`,
        // a texture of `vsMasks` a resolve stored into), that barrier.
        // Mid-pass it ends the pass. Guest memory is no Metal-tracked binding
        // of a draw, so this is its only ordering.
        void VertexBarrierIfNeeded(const std::array<uint32_t, 3>& vsMasks, uint32_t indexBase, uint32_t indexBytes)
        {
            if (s_vertexPendingLo >= s_vertexPendingHi && s_vertexPendingTextures.empty())
                return;
            auto pending = [](uint32_t base, uint32_t size) {
                base &= 0x1FFFFFFF;
                uint32_t end = base + size;
                if (size == 0 || base >= s_vertexPendingHi || end <= s_vertexPendingLo)
                    return false;
                auto it = std::upper_bound(s_vertexPending.begin(), s_vertexPending.end(), base,
                    [](uint32_t b, const std::pair<uint32_t, uint32_t>& r) { return b < r.second; });
                return it != s_vertexPending.end() && it->first < end;
            };
            int why = -1;
            // (A texture's cache entry and image are keyed by its guest base.)
            for (uint32_t d = 0; d < 3 && why < 0 && !s_vertexPendingTextures.empty(); d++)
                for (uint32_t bits = vsMasks[d]; bits && why < 0; bits &= bits - 1)
                {
                    xenos::xe_gpu_texture_fetch_t f;
                    memcpy(&f, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + uint32_t(__builtin_ctz(bits)) * 6], sizeof(f));
                    if (std::find(s_vertexPendingTextures.begin(), s_vertexPendingTextures.end(), (f.base_address << 12) & 0x1FFFFFFF) !=
                        s_vertexPendingTextures.end())
                        why = 2;
                }
            for (uint32_t word = 0; word < 3 && why < 0; word++)
                for (uint32_t bits = s_vs->info.vertexFetchConstants[word]; bits && why < 0; bits &= bits - 1)
                {
                    const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + (word * 32 + uint32_t(__builtin_ctz(bits))) * 2];
                    if (pending(vf[0] & 0x1FFFFFFC, ((vf[1] >> 2) & 0xFFFFFF) * 4))
                        why = 0;
                }
            if (why < 0 && pending(indexBase, indexBytes))
                why = 1;
            if (why < 0)
                return;
            static const char* const kWhy[3] = { "vertex data", "indices", "a texture" };
            LogOnce((0x2Full << 56) | (s_vs->hash & 0xFFFFFFFFFFFFFCull) | uint64_t(why),
                "vertex barrier (NFSMW_PASS_BARRIER): vs_%016llx reads %s a resolve wrote since the last full barrier%s",
                (unsigned long long)s_vs->hash, kWhy[why], s_passActive ? " (its pass is split)" : "");
            s_barrierCounts.vertex[why]++;
            s_barrierCounts.splits += s_passActive;
            EndPass(kBreakBarrier);
            Begin();
            FullBarrier();
        }

        // ---------------------------------------------------------------
        // Pipelines

        struct PipelineKey
        {
            uint64_t vs, ps;
            uint32_t topology, restart;
            uint32_t colorFormats[4];
            uint32_t depthFormat;
            uint32_t blend[4];       // RB_BLENDCONTROL, or ~0u when blending is off
            uint32_t colorMask;      // 4 bits per target
            uint32_t depthControl;   // RB_DEPTHCONTROL (0 without a depth target, or one attached for the pass only;
                                     // 0 with dynamic state)
            uint32_t raster;         // cull | front CW << 2 | depth clamp << 3 | depth bias << 4 | alpha test << 5 |
                                     // dynamic state << 6 (CP_OPT_DYNAMIC_STATE: no cull or front face then)
            uint32_t pad;

            bool operator==(const PipelineKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
        };
        static_assert(sizeof(PipelineKey) == 80);
        // The pipeline key list saves keys as their bytes, its shaders' hashes
        // first (gpu/pipeline_keys.h): bump pipeline_keys::kFormat when a
        // field here changes its place or meaning, so older lists are set
        // aside instead of misread.
        static_assert(sizeof(PipelineKey) == gpu::pipeline_keys::kKeyBytes && offsetof(PipelineKey, vs) == 0 &&
            offsetof(PipelineKey, ps) == 8);

        struct PipelineKeyHash
        {
            size_t operator()(const PipelineKey& k) const
            {
                uint64_t words[sizeof(k) / 8];
                memcpy(words, &k, sizeof(k));
                uint64_t h = 0xCBF29CE484222325ull;
                for (uint64_t w : words)
                    h = (h ^ w) * 0x100000001B3ull ^ (h >> 29);
                return size_t(h);
            }
        };
        struct ReadyPipeline
        {
            VkPipeline pipeline = VK_NULL_HANDLE;
            // Made ahead from the key list and not drawn with yet: its first
            // draw stamps its key as used this session (see the prewarm).
            bool prewarmed = false;
        };
        std::unordered_map<PipelineKey, ReadyPipeline, PipelineKeyHash> s_pipelines;

        VkBlendFactor BlendFactor(uint32_t f)
        {
            switch (f)
            {
            case 0: return VK_BLEND_FACTOR_ZERO;
            case 1: return VK_BLEND_FACTOR_ONE;
            case 4: return VK_BLEND_FACTOR_SRC_COLOR;
            case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
            case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            case 8: return VK_BLEND_FACTOR_DST_COLOR;
            case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
            case 10: return VK_BLEND_FACTOR_DST_ALPHA;
            case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
            case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
            case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
            case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            default: return VK_BLEND_FACTOR_ONE;
            }
        }

        VkBlendOp BlendOp(uint32_t op)
        {
            switch (op)
            {
            case 1: return VK_BLEND_OP_SUBTRACT;
            case 2: return VK_BLEND_OP_MIN;
            case 3: return VK_BLEND_OP_MAX;
            case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
            default: return VK_BLEND_OP_ADD;
            }
        }

        // Compiles the pipeline for `key` (and the SPIR-V it needs). Runs on a
        // pipeline worker: touches only its arguments, the shaders' own
        // locked modules and the (internally synchronized) pipeline cache.
        VkPipeline BuildPipeline(const PipelineKey& key, VkPipelineLayout layout, Shader& vs, Shader& ps)
        {
            VkShaderModule vsModule = VertexModule(vs, (key.raster >> 9) & 1);
            if (!vsModule)
                return VK_NULL_HANDLE;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                vsModule, "main", nullptr };
            uint32_t outputs = 0;
            for (int i = 0; i < 4; i++)
                if (key.colorFormats[i] != 0)
                    outputs |= 1u << i;
            // Raster bit 7: an NFSMW_PREPASS pipeline, depth only (no fragment
            // stage, no colour writes).
            // Bit 8 with it: the guest's pixel shader writing no colour (its
            // discards: alpha test, kills).
            const bool prepass = (key.raster >> 7) & 1, prepassDiscards = prepass && ((key.raster >> 8) & 1);
            if (!prepass || prepassDiscards)
            {
                uint32_t psRegs = std::min(16u, ps.info.registerCount);
                uint32_t inputs = vs.info.interpolatorMask & ((1u << psRegs) - 1);
                VkShaderModule psModule = PixelVariant(ps, prepass ? 0u : outputs, inputs, (key.raster >> 5) & 1);
                if (!psModule)
                    return VK_NULL_HANDLE;
                stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                    psModule, "main", nullptr };
            }

            VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
            VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
            ia.topology = VkPrimitiveTopology(key.topology);
            ia.primitiveRestartEnable = key.restart;
            VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
            vp.viewportCount = 1;
            vp.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
            rs.depthClampEnable = (key.raster >> 3) & 1;
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = key.raster & 3;
            rs.frontFace = (key.raster >> 2) & 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.depthBiasEnable = (key.raster >> 4) & 1;
            rs.lineWidth = 1.0f;

            VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo dss{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
            if (key.depthFormat != 0)
            {
                // As Draw sets them per draw with CP_OPT_DYNAMIC_STATE (gpu/draw_state.h).
                const gpu::draw_state::DepthStencil d = gpu::draw_state::FromDepthControl(key.depthControl);
                dss.depthTestEnable = d.depthTest;
                dss.depthWriteEnable = d.depthWrite;
                dss.depthCompareOp = VkCompareOp(d.depthCompareOp);
                dss.stencilTestEnable = d.stencilTest;
                dss.front = { VkStencilOp(d.front.failOp), VkStencilOp(d.front.passOp), VkStencilOp(d.front.depthFailOp),
                    VkCompareOp(d.front.compareOp), 0xFF, 0xFF, 0 };
                dss.back = { VkStencilOp(d.back.failOp), VkStencilOp(d.back.passOp), VkStencilOp(d.back.depthFailOp),
                    VkCompareOp(d.back.compareOp), 0xFF, 0xFF, 0 };
            }

            VkPipelineColorBlendAttachmentState att[4]{};
            VkFormat formats[4];
            for (int i = 0; i < 4; i++)
            {
                formats[i] = VkFormat(key.colorFormats[i]);
                if (key.colorFormats[i] == 0)
                    continue;
                att[i].colorWriteMask = (key.colorMask >> (4 * i)) & 0xF;
                if (key.blend[i] != ~0u)
                {
                    reg::RB_BLENDCONTROL bc{};
                    bc.value = key.blend[i];
                    att[i].blendEnable = VK_TRUE;
                    att[i].srcColorBlendFactor = BlendFactor(uint32_t(bc.color_srcblend));
                    att[i].dstColorBlendFactor = BlendFactor(uint32_t(bc.color_destblend));
                    att[i].colorBlendOp = BlendOp(uint32_t(bc.color_comb_fcn));
                    att[i].srcAlphaBlendFactor = BlendFactor(uint32_t(bc.alpha_srcblend));
                    att[i].dstAlphaBlendFactor = BlendFactor(uint32_t(bc.alpha_destblend));
                    att[i].alphaBlendOp = BlendOp(uint32_t(bc.alpha_comb_fcn));
                }
            }
            VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
            cb.attachmentCount = 4;
            cb.pAttachments = att;

            VkDynamicState dyn[14] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                VK_DYNAMIC_STATE_DEPTH_BIAS };
            uint32_t dynCount = 7;
#ifdef VK_EXT_extended_dynamic_state
            // Raster bit 6 (CP_OPT_DYNAMIC_STATE): cull mode, front face and
            // the depth and stencil tests are set per draw (Draw), and the
            // key leaves them out (rs and dss above hold zeros, ignored).
            if ((key.raster >> 6) & 1)
                for (VkDynamicState d : { VK_DYNAMIC_STATE_CULL_MODE_EXT, VK_DYNAMIC_STATE_FRONT_FACE_EXT, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT,
                         VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE_EXT,
                         VK_DYNAMIC_STATE_STENCIL_OP_EXT })
                    dyn[dynCount++] = d;
#endif
            VkPipelineDynamicStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
            ds.dynamicStateCount = dynCount;
            ds.pDynamicStates = dyn;

            VkPipelineRenderingCreateInfoKHR rci{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
            rci.colorAttachmentCount = 4;
            rci.pColorAttachmentFormats = formats;
            rci.depthAttachmentFormat = VkFormat(key.depthFormat);
            rci.stencilAttachmentFormat = VkFormat(key.depthFormat);
            // NFSMW_SHADING_RATE (an experiment, video/presenter.h): every draw
            // shades once per w x h pixels.
            VkPipelineFragmentShadingRateStateCreateInfoKHR rate{ VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR };
            if (s_vk->shadingRateW)
            {
                rate.fragmentSize = { s_vk->shadingRateW, s_vk->shadingRateH };
                rate.combinerOps[0] = rate.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
                rci.pNext = &rate;
            }

            VkGraphicsPipelineCreateInfo gpci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
            gpci.pNext = &rci;
            gpci.stageCount = prepass && !prepassDiscards ? 1 : 2;
            gpci.pStages = stages;
            gpci.pVertexInputState = &vi;
            gpci.pInputAssemblyState = &ia;
            gpci.pViewportState = &vp;
            gpci.pRasterizationState = &rs;
            gpci.pMultisampleState = &ms;
            gpci.pDepthStencilState = &dss;
            gpci.pColorBlendState = &cb;
            gpci.pDynamicState = &ds;
            gpci.layout = layout;
            if (s_vk->pipelineExecutableStats)
                gpci.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkResult r = vkCreateGraphicsPipelines(s_dev, s_pipelineCache, 1, &gpci, nullptr, &pipeline);
            if (r == VK_SUCCESS && s_vk->pipelineExecutableStats)
            {
                // NFSMW_PIPELINE_STATS: one line a pipeline, each shader's
                // statistics as the driver names them.
                static auto getProps = reinterpret_cast<PFN_vkGetPipelineExecutablePropertiesKHR>(
                    vkGetDeviceProcAddr(s_dev, "vkGetPipelineExecutablePropertiesKHR"));
                static auto getStats = reinterpret_cast<PFN_vkGetPipelineExecutableStatisticsKHR>(
                    vkGetDeviceProcAddr(s_dev, "vkGetPipelineExecutableStatisticsKHR"));
                VkPipelineInfoKHR pi{ VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR };
                pi.pipeline = pipeline;
                uint32_t count = 0;
                std::string line;
                if (getProps && getStats && getProps(s_dev, &pi, &count, nullptr) == VK_SUCCESS)
                {
                    std::vector<VkPipelineExecutablePropertiesKHR> props(count, { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR });
                    getProps(s_dev, &pi, &count, props.data());
                    for (uint32_t e = 0; e < count; e++)
                    {
                        VkPipelineExecutableInfoKHR ei{ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR };
                        ei.pipeline = pipeline;
                        ei.executableIndex = e;
                        uint32_t n = 0;
                        getStats(s_dev, &ei, &n, nullptr);
                        std::vector<VkPipelineExecutableStatisticKHR> st(n, { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR });
                        getStats(s_dev, &ei, &n, st.data());
                        line += std::format(" | {} (subgroup {})", props[e].name, props[e].subgroupSize);
                        for (const VkPipelineExecutableStatisticKHR& x : st)
                        {
                            uint64_t v = x.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR ? x.value.b32
                                : x.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR ? uint64_t(x.value.i64)
                                : x.format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR ? x.value.u64 : uint64_t(x.value.f64);
                            line += std::format(" {}={}", x.name, v);
                        }
                    }
                }
                fprintf(stderr, "[pipeline-stats] vs %016llx ps %016llx raster %x%s\n", (unsigned long long)key.vs,
                    (unsigned long long)key.ps, key.raster, line.c_str());
            }
            if (r != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] pipeline vs %016llx ps %016llx failed: %d\n",
                    (unsigned long long)key.vs, (unsigned long long)key.ps, int(r));
                pipeline = VK_NULL_HANDLE;
            }
            return pipeline;
        }

        // Pipelines are compiled on worker threads. A new one costs 20-80 ms
        // on RADV (it was 100-460 ms before the vertex shader's single call
        // site), and a command processor waiting on it stalls the game: a
        // traffic car streaming in mid-race froze it for 2 s. The command
        // processor waits briefly (NFSMW_PIPELINE_WAIT_US a frame, default
        // 3000: disk cache hits make it), then skips the draw until it is
        // ready, as Xenia does. For 2 s after a loading screen (a frame of
        // under 64 draws) it waits in full, so scene setup, including
        // anything the game renders once, is drawn exactly.
        // NFSMW_PIPELINE_THREADS=<n> (default: a third of the cores, at most
        // 4; 0 compiles on the command processor). Settings > Advanced >
        // Asynchronous Shaders off (NFSMW_ASYNC_PIPELINES=0): always wait.
        // NFSMW_PIPELINE_LOG=1 logs each pipeline's timing.
        // Pipelines earlier sessions used are made before a draw needs them
        // (the prewarm, below): the workers take those only when no draw
        // waits for one, and one a draw comes to wait for is queued again
        // with the draws' (the first worker to take it builds it).
        struct PendingPipeline
        {
            // What to build: set before it is queued, read-only afterwards.
            PipelineKey key{};
            VkPipelineLayout layout = VK_NULL_HANDLE;
            Shader* vs = nullptr;
            Shader* ps = nullptr;
            bool prewarm = false;   // queued by the prewarm, ahead of any draw
            bool promoted = false;  // a prewarm job a draw waits for, in the draws' queue too (command processor)
            std::atomic<bool> claimed{ false };  // a worker has taken it (from either queue)
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
            VkPipeline pipeline = VK_NULL_HANDLE;
            std::chrono::steady_clock::time_point queued = std::chrono::steady_clock::now();
            double createMs = 0;
            uint64_t queuedFrame = 0;
            bool outlasted = false;  // outlasted a frame's wait budget: a real compile, not worth waiting for again
        };
        std::unordered_map<PipelineKey, std::shared_ptr<PendingPipeline>, PipelineKeyHash> s_pendingPipelines;
        const uint32_t s_pipelineThreads = [] {
            if (const char* v = std::getenv("NFSMW_PIPELINE_THREADS"))
                return uint32_t(std::atoi(v));
            return std::clamp(std::thread::hardware_concurrency() / 3, 1u, 4u);
        }();
        const int64_t s_pipelineWaitUs = [] { const char* v = std::getenv("NFSMW_PIPELINE_WAIT_US"); return v ? int64_t(std::atoll(v)) : 3000; }();
        const bool s_logPipelines = std::getenv("NFSMW_PIPELINE_LOG") != nullptr;
        uint64_t s_frame = 0, s_drawsAtSwap = 0, s_waitPipelinesUntil = 120;  // boot: wait in full
        uint32_t s_pipelineSkips = 0;         // draws skipped this frame, their pipeline not ready
        int64_t s_pipelineWaitLeftUs = 0;     // this frame's wait budget left

        std::mutex s_compileMutex;
        std::condition_variable s_compileCv;
        // The jobs draws wait for, and the prewarm's, taken only when no
        // draw waits for one and by all workers but one (with two or more),
        // so a draw's compile never queues behind a prewarm compile (20-80
        // ms each with a cold driver cache).
        std::deque<std::shared_ptr<PendingPipeline>> s_compileQueue, s_prewarmQueue;
        uint32_t s_compileActive = 0;         // jobs running (under s_compileMutex)
        uint32_t s_prewarmActive = 0;         // of them the prewarm queue's
        const uint32_t s_prewarmWorkers = std::max(s_pipelineThreads, 2u) - 1;  // at most that many
        bool s_compileStopping = false;

        void RunPipelineJob(PendingPipeline& job)
        {
            if (job.claimed.exchange(true))
                return;  // a promoted prewarm job, built from its other queue
            auto start = std::chrono::steady_clock::now();
            VkPipeline pipeline = BuildPipeline(job.key, job.layout, *job.vs, *job.ps);
            {
                std::lock_guard lock(job.mutex);
                job.pipeline = pipeline;
                job.createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                job.done = true;
            }
            job.cv.notify_all();
        }

        void CompileWorker()
        {
            SetHostThreadName("nfsmw-pipelines");
            hostcpu::LeaveReservedCore();  // started by the command processor
            auto prewarmReady = [] { return !s_prewarmQueue.empty() && s_prewarmActive < s_prewarmWorkers; };
            while (true)
            {
                std::shared_ptr<PendingPipeline> job;
                bool prewarm;
                {
                    std::unique_lock lock(s_compileMutex);
                    s_compileCv.wait(lock, [&] { return s_compileStopping || !s_compileQueue.empty() || prewarmReady(); });
                    if (s_compileStopping)
                        return;
                    prewarm = s_compileQueue.empty();
                    auto& queue = prewarm ? s_prewarmQueue : s_compileQueue;
                    job = std::move(queue.front());
                    queue.pop_front();
                    s_compileActive++;
                    s_prewarmActive += prewarm;
                }
                RunPipelineJob(*job);
                {
                    std::lock_guard lock(s_compileMutex);
                    s_compileActive--;
                    s_prewarmActive -= prewarm;
                }
                s_compileCv.notify_all();
            }
        }

        // Starts making `key`'s pipeline (with `layout`, `vs` and `ps`) on the
        // workers, or makes it now without them (NFSMW_PIPELINE_THREADS=0).
        // Command processor; the job is s_pendingPipelines' until taken in.
        std::shared_ptr<PendingPipeline> QueuePipeline(const PipelineKey& key, VkPipelineLayout layout, Shader* vs, Shader* ps, bool prewarm)
        {
            auto job = std::make_shared<PendingPipeline>();
            job->key = key;
            job->layout = layout;
            job->vs = vs;
            job->ps = ps;
            job->prewarm = prewarm;
            job->queuedFrame = s_frame;
            s_pendingPipelines[key] = job;
            if (s_pipelineThreads == 0)
            {
                double notAHitch = 0;  // (the prewarm's, at a swap: no draw waited)
                HitchTimer hitchTimer(prewarm ? notAHitch : s_hitch.pipelineMs);
                RunPipelineJob(*job);
                return job;
            }
            static std::once_flag started;
            std::call_once(started, [] {
                for (uint32_t i = 0; i < s_pipelineThreads; i++)
                    std::thread(CompileWorker).detach();
            });
            {
                std::lock_guard lock(s_compileMutex);
                (prewarm ? s_prewarmQueue : s_compileQueue).push_back(job);
            }
            s_compileCv.notify_all();
            return job;
        }

        // A prewarm job a draw now waits for: queued with the draws' jobs too,
        // unless a worker has it already.
        void PromotePrewarmJob(const std::shared_ptr<PendingPipeline>& job)
        {
            if (!job->prewarm || job->promoted || job->claimed.load() || s_pipelineThreads == 0)
                return;
            job->promoted = true;
            {
                std::lock_guard lock(s_compileMutex);
                s_compileQueue.push_back(job);
            }
            s_compileCv.notify_all();
        }

        // The pipeline key list (gpu/pipeline_keys.h): every pipeline this
        // session draws with is noted, with its shaders' guest words, and the
        // list is saved beside the pipeline cache (with it every 10 s when new
        // pipelines appeared, and at exit), so the next session makes them
        // ahead (the prewarm). Noted by the command processor, saved from
        // other threads: s_keyListMutex. Off with the disk caches
        // (NFSMW_SHADER_CACHE=0).
        std::mutex s_keyListMutex;
        gpu::pipeline_keys::Recorder s_keyList;  // under s_keyListMutex, as are the three below
        bool s_keyListOn = false;                // set by Initialize (LoadPipelineKeys), on the command processor
        uint32_t s_keyListVendor = 0, s_keyListDevice = 0;
        std::mutex s_keyListSaveMutex;           // the background saver and the one at exit

        std::filesystem::path PipelineKeysFile()
        {
            return GetCachePath() / "pipeline_keys.bin";
        }

        // A pipeline drawn with: added to the list when new (with its
        // shaders' words), else stamped as used this session. Command
        // processor; rare (once per pipeline a session).
        void NotePipelineKey(const PipelineKey& key, const Shader* vs, const Shader* ps)
        {
            if (!s_keyListOn)  // (this thread set it)
                return;
            gpu::pipeline_keys::Key bytes;
            std::memcpy(bytes.data(), &key, sizeof(key));
            std::lock_guard lock(s_keyListMutex);
            if (s_keyList.Note(bytes))
                for (const Shader* s : { vs, ps })
                    if (s)
                        s_keyList.NoteShader(s->hash, uint32_t(s->stage), s->words);
        }

        // Any thread (the background saver, Shutdown).
        void SavePipelineKeys()
        {
            std::lock_guard saveLock(s_keyListSaveMutex);
            std::vector<uint8_t> data;
            {
                std::lock_guard lock(s_keyListMutex);
                if (!s_keyListOn || !s_keyList.Dirty())
                    return;
                data = s_keyList.Save(s_keyListVendor, s_keyListDevice);
            }
            std::error_code ec;
            std::filesystem::create_directories(GetCachePath(), ec);
            std::filesystem::path file = PipelineKeysFile(), temp = file;
            temp += ".tmp";
            bool ok = false;
            if (FILE* f = fopen(temp.c_str(), "wb"))
            {
                ok = fwrite(data.data(), 1, data.size(), f) == data.size();
                ok = fclose(f) == 0 && ok;
                if (ok)
                    std::filesystem::rename(temp, file, ec);
                ok = ok && !ec;
                if (!ok)
                    std::filesystem::remove(temp, ec);
            }
            if (!ok)
            {
                std::lock_guard lock(s_keyListMutex);
                s_keyList.MarkUnsaved();  // (the next save tries again)
            }
        }

        Shader* FindShader(uint64_t hash)
        {
            auto it = s_shaders.find(hash);
            return it == s_shaders.end() ? nullptr : it->second.get();
        }

        // The pipeline for `key`; VK_NULL_HANDLE with `pending` set while a
        // worker still compiles it (the caller skips the draw).
        Shader* DepthOnlyPixelShader();
        // NFSMW_PREPASS: a pre-pass pipeline (`key` with raster bit 7), or
        // none while it compiles (the draw then isn't in the pre-pass).
        // Never waits; not kept in the key list.
        VkPipeline PrepassPipeline(const PipelineKey& key, VkPipelineLayout layout)
        {
            // (Draws in a row share theirs often: the last one, as GetPipeline's.)
            static PipelineKey lastKey{};
            static VkPipeline lastPipeline = VK_NULL_HANDLE;
            if (lastPipeline != VK_NULL_HANDLE && key == lastKey)
                return lastPipeline;
            if (auto it = s_pipelines.find(key); it != s_pipelines.end())
            {
                lastKey = key;
                lastPipeline = it->second.pipeline;
                return it->second.pipeline;
            }
            auto p = s_pendingPipelines.find(key);
            if (p == s_pendingPipelines.end())
            {
                QueuePipeline(key, layout, s_shaders[key.vs].get(), (key.raster >> 8) & 1 ? s_shaders[key.ps].get() : DepthOnlyPixelShader(), false);
                return VK_NULL_HANDLE;
            }
            std::shared_ptr<PendingPipeline> job = p->second;
            {
                std::lock_guard lock(job->mutex);
                if (!job->done)
                    return VK_NULL_HANDLE;
            }
            s_pipelines[key] = { job->pipeline, false };
            s_pendingPipelines.erase(key);
            return job->pipeline;
        }

        VkPipeline GetPipeline(const PipelineKey& key, VkPipelineLayout layout, bool& pending)
        {
            pending = false;
            static PipelineKey lastKey{};
            static VkPipeline lastPipeline = VK_NULL_HANDLE;
            if (lastPipeline != VK_NULL_HANDLE && key == lastKey)
                return lastPipeline;
            auto it = s_pipelines.find(key);
            if (it != s_pipelines.end())
            {
                if (it->second.prewarmed) [[unlikely]]
                {
                    // Its first draw: the key list keeps it as used this
                    // session (the list keeps the most recently used).
                    it->second.prewarmed = false;
                    NotePipelineKey(key, FindShader(key.vs), FindShader(key.ps));
                }
                lastKey = key;
                lastPipeline = it->second.pipeline;
                return it->second.pipeline;
            }
            std::shared_ptr<PendingPipeline> job;
            if (auto p = s_pendingPipelines.find(key); p != s_pendingPipelines.end())
            {
                job = p->second;
                PromotePrewarmJob(job);
            }
            else
            {
                s_hitch.pipelines++;
                job = QueuePipeline(key, layout, s_shaders[key.vs].get(), s_shaders[key.ps].get(), false);
            }
            {
                HitchTimer hitchTimer(s_hitch.pipelineMs);
                CpWaitTimer wait(CP_WAIT_PIPELINE);
                std::unique_lock lock(job->mutex);
                // Waits share one budget a frame (cache hits make it); once it
                // is spent, draws just check. A per-draw wait made a pipeline
                // used by 13 draws cost 47 ms a frame while it compiled.
                if (!settings::GetBool(settings::Id::AsyncShaders) || s_frame < s_waitPipelinesUntil)
                    job->cv.wait(lock, [&] { return job->done; });
                else
                {
                    // The budget catches compiles that finish within a few
                    // milliseconds (cache hits); one that outlasted it once is
                    // only checked afterwards, not waited for every frame
                    // (that cost 3 ms a frame for as long as it compiled).
                    auto start = std::chrono::steady_clock::now();
                    int64_t budget = job->outlasted ? 0 : std::max<int64_t>(s_pipelineWaitLeftUs, 0);
                    bool ready = job->cv.wait_for(lock, std::chrono::microseconds(budget), [&] { return job->done; });
                    s_pipelineWaitLeftUs -= std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
                    if (!ready && budget > 0)
                        job->outlasted = true;
                    if (!ready)
                    {
                        pending = true;
                        s_pipelineSkips++;
                        return VK_NULL_HANDLE;
                    }
                }
            }
            if (s_logPipelines)
                fprintf(stderr, "[pipeline] vs %016llx ps %016llx: %.1f ms to compile, ready %.1f ms and %llu frames after the first draw%s\n",
                    (unsigned long long)key.vs, (unsigned long long)key.ps, job->createMs,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - job->queued).count(),
                    (unsigned long long)(s_frame - job->queuedFrame), job->prewarm ? " (queued by the prewarm)" : "");
            s_stats.pipelines++;
            s_pipelines[key] = { job->pipeline, false };
            s_pendingPipelines.erase(key);
            if (job->pipeline)
                NotePipelineKey(key, job->vs, job->ps);
            return job->pipeline;
        }

        // ---------------------------------------------------------------
        // Pipeline prewarm
        //
        // The pipeline cache makes a pipeline cheaper to create, not created:
        // every session still created each one when a draw first used it. The
        // Deck's replayed race created 19 of its ~90 while driving, each with
        // its shaders' translation on the command processor and the command
        // processor waiting for the worker: the three frames that made them
        // took 0.4-2.6 ms longer than with them made ahead, and the first
        // frame after the race's intro 6 ms longer for its 25. After a driver
        // update, or a build whose shaders translate differently, the cache
        // is cold, and the draws are skipped for the 20-80 ms each takes. So
        // the key list of the sessions before (gpu/pipeline_keys.h) is made
        // again as the game starts: from the first swap on, a slice of each
        // swap's command-processor time (kPrewarmSliceUs) translates the
        // listed shaders from their words and queues the pipelines, in the
        // order sessions first used them, and the workers make them when no
        // draw waits for one (all workers but one: s_prewarmWorkers).
        // Finished ones are taken in at the next swap, before any draw needs
        // them; a draw that needs one first gets it as it would have anyway.
        // Only keys this session's Draw can make are queued (dynamic state on
        // or off, formats the device can render to). One line in the log
        // says how many and how long. NFSMW_PIPELINE_PREWARM=0 makes nothing
        // ahead (the list is still kept and saved).
        constexpr int64_t kPrewarmSliceUs = 2000;
        const bool s_prewarmOn = [] { const char* v = std::getenv("NFSMW_PIPELINE_PREWARM"); return !v || v[0] != '0'; }();

        struct Prewarm
        {
            std::vector<PipelineKey> keys;  // to make, in the list's order
            size_t next = 0;                // the next to queue
            std::unordered_map<uint64_t, gpu::pipeline_keys::Shader> shaders;  // the list's words, by hash
            std::vector<PipelineKey> queued;  // queued, not taken in yet
            bool started = false, finished = true;
            std::chrono::steady_clock::time_point start;
            uint64_t startFrame = 0;
            double translateMs = 0, compileMs = 0;
            uint32_t listed = 0, translated = 0, made = 0, failed = 0, unusable = 0, drawnFirst = 0;
        };
        Prewarm s_prewarm;  // command processor

        // A listed key's shader: the one this session has under its hash, or
        // made from the list's words (when they hash to it).
        Shader* PrewarmShader(uint64_t hash, uint32_t type)
        {
            const ShaderStage stage = type == 0 ? ShaderStage::Vertex : ShaderStage::Pixel;
            if (type == 1 && hash == kDepthOnlyPsHash)
                return DepthOnlyPixelShader();
            if (Shader* s = FindShader(hash))
                return s->stage == stage ? s : nullptr;
            auto it = s_prewarm.shaders.find(hash);
            if (it == s_prewarm.shaders.end() || it->second.type != type)
                return nullptr;
            const std::vector<uint32_t>& words = it->second.words;
            if (gpu::pipeline_keys::ShaderHash(type, words.data(), words.size()) != hash)
                return nullptr;
            auto start = std::chrono::steady_clock::now();
            Shader* s = MakeShader(type, hash, words.data(), uint32_t(words.size()));
            s_prewarm.translateMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            s_prewarm.translated++;
            return s;
        }

        // Whether this session's Draw makes keys like `key` and the device
        // can render to its formats: the list may come from another
        // configuration (CP_OPT_DYNAMIC_STATE on or off, NFSMW_DYNAMIC_STATE)
        // or a driver that has changed.
        bool PrewarmKeyUsable(const PipelineKey& key)
        {
            if (((key.raster >> 6) & 1) != uint32_t(s_dynamicState) || (key.raster >> 7) != 0 || key.pad != 0)
                return false;
            if (s_dynamicState && ((key.raster & 7) != 0 || key.depthControl != 0))
                return false;
            // Draw's topologies: point, line and triangle lists and strips,
            // primitive restart only for strips.
            if (key.topology > VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || key.restart > 1 ||
                (key.restart && key.topology != VK_PRIMITIVE_TOPOLOGY_LINE_STRIP && key.topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP))
                return false;
            if (key.colorMask >> 16)
                return false;
            auto features = [](uint32_t format) {
                VkFormatProperties props{};
                vkGetPhysicalDeviceFormatProperties(s_vk->physical, VkFormat(format), &props);
                return props.optimalTilingFeatures;
            };
            for (int i = 0; i < 4; i++)
            {
                if (key.colorFormats[i] == 0)
                {
                    if (key.blend[i] != ~0u)
                        return false;
                    continue;
                }
                const VkFormatFeatureFlags need =
                    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | (key.blend[i] != ~0u ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0);
                if ((features(key.colorFormats[i]) & need) != need)
                    return false;
            }
            if (key.depthFormat == 0)
                return key.depthControl == 0;
            return (features(key.depthFormat) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        }

        // Initialize: the key list from the last session, kept by s_keyList
        // and, with the keys this configuration makes, by the prewarm.
        void LoadPipelineKeys()
        {
            if (!s_shaderCacheEnabled)
                return;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            std::vector<uint8_t> data;
            if (FILE* f = fopen(PipelineKeysFile().c_str(), "rb"))
            {
                fseek(f, 0, SEEK_END);
                long size = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (size > 0 && size <= (64l << 20))
                {
                    data.resize(size_t(size));
                    if (fread(data.data(), 1, data.size(), f) != data.size())
                        data.clear();
                }
                fclose(f);
            }
            gpu::pipeline_keys::Decoded decoded = gpu::pipeline_keys::Decode(data.data(), data.size(), props.vendorID, props.deviceID);
            if (decoded.verdict == gpu::pipeline_keys::Verdict::OtherGpu || decoded.verdict == gpu::pipeline_keys::Verdict::Unreadable)
                fprintf(stderr, "[renderer] pipeline key list set aside (%s): %s; it starts again\n",
                    decoded.verdict == gpu::pipeline_keys::Verdict::OtherGpu ? "another GPU's" : "not readable", decoded.why.c_str());
            gpu::pipeline_keys::List& list = decoded.list;
            gpu::pipeline_keys::Prune(list, gpu::pipeline_keys::kMaxKeys);
            Prewarm& p = s_prewarm;
            p.listed = uint32_t(list.keys.size());
            // Two pipeline workers at least: with one, a prewarm compile can
            // hold the only worker while a draw waits for its own pipeline
            // (20-80 ms with a cold driver cache), and with none
            // (NFSMW_PIPELINE_THREADS=0) every pipeline would be built on the
            // command processor itself, a whole one per swap. The list is
            // still kept and saved either way.
            const bool prewarmWorkers = s_pipelineThreads >= 2;
            if (s_prewarmOn && prewarmWorkers)
            {
                for (const gpu::pipeline_keys::Entry& e : list.keys)
                {
                    PipelineKey key;
                    std::memcpy(&key, e.key.data(), sizeof(key));
                    if (PrewarmKeyUsable(key))
                        p.keys.push_back(key);
                    else
                        p.unusable++;
                }
                p.shaders = list.shaders;
                p.finished = p.keys.empty();
            }
            if (p.listed)
                fprintf(stderr, "[renderer] pipeline key list: %u pipelines and %zu shaders from %u sessions; %s\n", p.listed,
                    list.shaders.size(), list.session,
                    !s_prewarmOn    ? "none made ahead (NFSMW_PIPELINE_PREWARM=0)"
                    : !prewarmWorkers ? std::format("none made ahead ({} pipeline worker thread{}: it takes two)", s_pipelineThreads, s_pipelineThreads == 1 ? "" : "s").c_str()
                                      : std::format("{} to make ahead ({} not for this configuration)", p.keys.size(), p.unusable).c_str());
            std::lock_guard lock(s_keyListMutex);
            s_keyListVendor = props.vendorID;
            s_keyListDevice = props.deviceID;
            s_keyList.Start(std::move(list));
            s_keyListOn = true;
        }

        // At each swap until it is done (OnSwap): queue the next listed
        // pipelines while this swap's slice lasts, and take in those the
        // workers have made.
        void PrewarmStep()
        {
            Prewarm& p = s_prewarm;
            if (p.finished)
                return;
            auto now = std::chrono::steady_clock::now();
            if (!p.started)
            {
                p.started = true;
                p.start = now;
                p.startFrame = s_frame;
            }
            const auto until = now + std::chrono::microseconds(kPrewarmSliceUs);
            while (p.next < p.keys.size() && std::chrono::steady_clock::now() < until)
            {
                const PipelineKey& key = p.keys[p.next++];
                if (s_pipelines.count(key) || s_pendingPipelines.count(key))
                {
                    p.drawnFirst++;  // a draw needed it first
                    continue;
                }
                Shader* vs = PrewarmShader(key.vs, 0);
                Shader* ps = PrewarmShader(key.ps, 1);
                if (!vs || !ps || !vs->info.ok || !ps->info.ok)
                {
                    p.unusable++;
                    continue;
                }
                QueuePipeline(key, GetTextureLayoutUncached(*vs, *ps).pipeline, vs, ps, true);
                p.queued.push_back(key);
            }
            for (size_t i = 0; i < p.queued.size();)
            {
                auto it = s_pendingPipelines.find(p.queued[i]);
                if (it == s_pendingPipelines.end())
                    p.drawnFirst++;  // a draw waited for it and took it in (GetPipeline)
                else
                {
                    std::shared_ptr<PendingPipeline> job = it->second;
                    {
                        std::lock_guard lock(job->mutex);
                        if (!job->done)
                        {
                            i++;
                            continue;
                        }
                    }
                    p.compileMs += job->createMs;
                    (job->pipeline ? p.made : p.failed)++;
                    s_stats.pipelines++;
                    s_pipelines[job->key] = { job->pipeline, job->pipeline != VK_NULL_HANDLE };
                    s_pendingPipelines.erase(it);
                }
                p.queued[i] = p.queued.back();
                p.queued.pop_back();
            }
            if (p.next < p.keys.size() || !p.queued.empty())
                return;
            p.finished = true;
            const std::string where = s_pipelineThreads == 0 ? std::string("the command processor")
                                                             : std::format("{} of the {} workers", s_prewarmWorkers, s_pipelineThreads);
            fprintf(stderr, "[renderer] pipeline prewarm: %u of %zu made ahead, ready %.0f ms (%llu frames) after the first frame "
                "(%u shaders translated in %.1f ms on the command processor, %.0f ms of compiles on %s); "
                "%u drawn with first, %u not makeable, %u failed\n",
                p.made, p.keys.size(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p.start).count(),
                (unsigned long long)(s_frame - p.startFrame), p.translated, p.translateMs, p.compileMs, where.c_str(), p.drawnFirst,
                p.unusable, p.failed);
            p.keys = {};
            p.shaders = {};
            p.queued = {};
        }

        // ---------------------------------------------------------------
        // Scissor (Xenia's draw_util GetScissor)

        struct Rect { int32_t x0, y0, x1, y1; };

        Rect GuestScissor()
        {
            auto tl = Reg<reg::PA_SC_WINDOW_SCISSOR_TL>();
            auto br = Reg<reg::PA_SC_WINDOW_SCISSOR_BR>();
            Rect r{ int32_t(tl.tl_x), int32_t(tl.tl_y), int32_t(br.br_x), int32_t(br.br_y) };
            if (!tl.window_offset_disable)
            {
                auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
                r.x0 = std::max(r.x0 + wo.window_x_offset, 0);
                r.y0 = std::max(r.y0 + wo.window_y_offset, 0);
                r.x1 = std::max(r.x1 + wo.window_x_offset, 0);
                r.y1 = std::max(r.y1 + wo.window_y_offset, 0);
            }
            auto stl = Reg<reg::PA_SC_SCREEN_SCISSOR_TL>();
            auto sbr = Reg<reg::PA_SC_SCREEN_SCISSOR_BR>();
            r.x0 = std::max(r.x0, int32_t(stl.tl_x));
            r.y0 = std::max(r.y0, int32_t(stl.tl_y));
            r.x1 = std::min(r.x1, int32_t(sbr.br_x));
            r.y1 = std::min(r.y1, int32_t(sbr.br_y));
            r.x1 = std::max(r.x1, r.x0);
            r.y1 = std::max(r.y1, r.y0);
            return r;
        }

        // NFSMW_UNTILED_AREA on for this frame (=alt: in odd 120-frame windows).
        bool UntiledAreaOn()
        {
            return s_untiledArea == 1 || (s_untiledArea == 2 && s_frame / 120 % 2 == 1);
        }

        // Whether the game is in split screen (game/split_screen.h), read
        // once a frame; its split-screen rules: gpu/split_views.h. CP thread.
        bool SplitFrame()
        {
            static uint64_t frame = ~0ull;
            static bool split = false;
            if (frame != s_frame)
            {
                frame = s_frame;
                const bool now = game::split_screen::GameSaysSplit();
                if (now != split)
                    fprintf(stderr, "[renderer] split screen %s at frame %llu%s\n", now ? "on" : "off", (unsigned long long)s_frame,
                        now ? ": untiled draws are cut to their own view, a second view restarts its pass" : "");
                split = now;
            }
            return split;
        }

        // Where an untiled draw into `targets` (4 colour, then depth; any
        // null) may draw, in guest pixels of their common w x h (the caller
        // clamps to it): tile 0 draws the whole frame, so the whole target.
        // With NFSMW_UNTILED_AREA, one clipped to its viewport (x and y
        // scaled and offset, clipping on) only the pixels whose centres lie
        // inside the viewport (a counted draw's rule, in Draw). No scissor:
        // they are tile 0's, and the rows and columns past tile 0 are drawn
        // by tile 0 alone (the window scissor is the tile: for side-by-side
        // tiles only the left one's columns, and the right half went black
        // when they were drawn to it; D3D leaves the screen scissor
        // 8192x8192). `why`: 0 cut to the viewport, 1 clipping off, 2 no
        // viewport transform (or garbage in it), 3 a target that fell back
        // to the whole target (wholeArea), 4 off.
        // In split screen (gpu/split_views.h) every untiled draw is cut, and
        // a fallback to the whole target doesn't hold.
        Rect UntiledScissor(RenderTarget* const targets[5], int32_t w, int32_t h, int* why = nullptr)
        {
            Rect r{ 0, 0, w, h };
            int reason = 4;
            const bool split = SplitFrame();
            if (gpu::split_views::CutOn(UntiledAreaOn(), split))
            {
                float ax = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_XSCALE)), ox = RegF(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
                float ay = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_YSCALE)), oy = RegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
                if (Reg<reg::PA_CL_CLIP_CNTL>().clip_disable)
                    reason = 1;
                else if ((Reg<reg::PA_CL_VTE_CNTL>().value & 0xF) != 0xF || !std::isfinite(ax + ox + ay + oy))
                    reason = 2;
                else if (std::any_of(targets, targets + 5, [&](RenderTarget* rt) { return rt && gpu::split_views::KeepsWholeArea(rt->wholeArea, split); }))
                    reason = 3;
                else
                    reason = 0;
                if (reason == 0)
                {
                    // As the host viewport: the window offset (0 for tile 0)
                    // and D3D's integer pixel centres. |scale|: y is flipped.
                    float wx = 0.0f, wy = 0.0f;
                    if (Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
                    {
                        auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
                        wx = float(wo.window_x_offset);
                        wy = float(wo.window_y_offset);
                    }
                    const bool d3dCenter = Reg<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero;
                    const gpu::split_views::GuestRect v = gpu::split_views::ViewportPixels(ax, ox, ay, oy, wx, wy, d3dCenter, w, h);
                    r = { v.x0, v.y0, v.x1, v.y1 };
                }
            }
            if (why)
                *why = reason;
            return r;
        }

        // ---------------------------------------------------------------
        // Indices

        // Index `i` of the current draw, as the vertex shader's fetchIndex.
        struct IndexSource
        {
            bool indexed;
            uint32_t base;       // physical
            bool is32;
            uint32_t endian;

            uint32_t Get(uint32_t i) const
            {
                if (!indexed)
                    return i;
                if (is32)
                    return GpuSwap(LoadPhysical(base + i * 4), endian) & 0xFFFFFF;
                uint32_t addr = base + i * 2;
                uint32_t w = GpuSwap(LoadPhysical(addr & ~3u), endian);
                bool firstInHigh = endian == 2 || endian == 3;
                bool high = ((addr >> 1) & 1) != firstInHigh;
                return high ? (w >> 16) : (w & 0xFFFF);
            }
        };

        // NFSMW_LOG_COVERS=1 (gpu/covers.h, game/cover_callers.h): the
        // current draw's quads, if it is a front-end draw (the front-end
        // matrix in c0..c3 as the hook left it, ePolys at fetch 95 read by
        // its vertex shader), as host pixels of its target after every rule
        // (`posScale`, `scissor`), against its guest viewport in host pixels:
        // ClassifyQuad's kinds, logged once per site.
        std::atomic<uint64_t> s_coverHits{ 0 };
        void LogCovers(uint32_t count, uint32_t prim, const IndexSource& src, const float posScale[4], const VkRect2D& scissor,
            float hostW, float hostH, float fx, float fy, const gpu::bars::Plan& plan)
        {
            const float feX = game::FrontEndScaleX();
            if (feX <= 0.0f || prim != 13 || count == 0 || count > 256 || count % 4)
                return;
            float c[8];
            memcpy(c, &s_regs[XE_GPU_REG_SHADER_CONSTANT_000_X], sizeof(c));
            const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 95 * 2];
            if (!(std::fabs(c[0] - feX) <= feX * 1e-5f && c[1] == 0.0f && c[2] == 0.0f && (vf[0] & 3) == 3))
                return;
            const auto& vi = s_vs->info;
            if (!(((vi.vertexFetchConstants[95 / 32] >> (95 % 32)) & 1) && (vi.constRelative || (vi.constUsed[0] & 0xFull) == 0xFull)))
                return;
            const float s = game::AspectScaleX(), sv = game::AspectScaleY();
            const float bandMidX = c[3] * (1.0f - s), bandMidY = c[7] * (1.0f - sv);
            // The guest viewport (what the presenter shows of this target), in
            // host pixels, from the raw registers.
            const float vsx = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_XSCALE)), vox = RegF(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
            const float vsy = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_YSCALE)), voy = RegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
            gpu::covers::Rect picture{ int32_t(std::lround((vox - vsx) * fx)), int32_t(std::lround((voy - vsy) * fy)),
                int32_t(std::lround((vox + vsx) * fx)), int32_t(std::lround((voy + vsy) * fy)) };
            picture = gpu::covers::Intersect(picture, { 0, 0, int32_t(hostW), int32_t(hostH) });
            const gpu::covers::Rect sc{ scissor.offset.x, scissor.offset.y, scissor.offset.x + int32_t(scissor.extent.width),
                scissor.offset.y + int32_t(scissor.extent.height) };
            const uint32_t base = vf[0] & 0x1FFFFFFC, endian = vf[1] & 3;
            const uint32_t words = (vf[1] >> 2) & 0xFFFFFF, first = s_regs[XE_GPU_REG_VGT_INDX_OFFSET];
            const uint32_t blend0 = s_regs[XE_GPU_REG_RB_BLENDCONTROL0] & 0x1FFF1FFF;
            // Blending on unless the colour's factors are one and zero
            // (RB_BLENDCONTROL bits 0-4 and 8-12; 07060706: source alpha and
            // one minus it, the front end's usual).
            const bool blended = !((blend0 & 0x1F) == 1 && ((blend0 >> 8) & 0x1F) == 0);
            xenos::xe_gpu_texture_fetch_t t0;
            memcpy(&t0, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(t0));
            const bool t0Sampled = (s_ps->info.texture2DMask & 1) != 0;
            const uint32_t t0W = uint32_t(t0.size_2d.width) + 1, t0H = uint32_t(t0.size_2d.height) + 1;
            // Its darkest texel of five (the centre and halfway to each corner),
            // for a 32-bit t0 (format 6); white for the game's 32x32 white
            // texture's size and anything unread.
            uint32_t texel = 0xFFFFFFFFu;
            const bool texelKnown = !t0Sampled || (t0W <= 32 && t0H <= 32) || uint32_t(t0.format) == 6;
            if (t0Sampled && uint32_t(t0.format) == 6 && (t0W > 32 || t0H > 32))
            {
                const uint32_t t0Base = (uint32_t(t0.base_address) << 12) & 0x1FFFFFFFu, t0Pitch = uint32_t(t0.pitch) << 5;
                uint32_t darkest = 0xFFFFFFFFu, darkestSum = 3 * 255 + 1;
                const uint32_t at[5][2] = { { t0W / 2, t0H / 2 }, { t0W / 4, t0H / 4 }, { 3 * t0W / 4, t0H / 4 }, { t0W / 4, 3 * t0H / 4 },
                    { 3 * t0W / 4, 3 * t0H / 4 } };
                for (const auto& p : at)
                    if (t0Pitch && p[0] < t0Pitch)
                    {
                        // k_8_8_8_8 as the front end's textures are stored: A in the top byte after the swap.
                        const bool tiled = t0.tiled;
                        const uint32_t texEndian = static_cast<uint32_t>(t0.endianness);
                        const uint32_t w32 = GpuSwap(LoadPhysical(t0Base + gpu::bars::TexelOffset32(p[0], p[1], t0Pitch, tiled)), texEndian);
                        const uint32_t sum = ((w32 >> 16) & 0xFF) + ((w32 >> 8) & 0xFF) + (w32 & 0xFF);
                        if (sum < darkestSum)
                        {
                            darkestSum = sum;
                            darkest = w32;
                        }
                    }
                texel = darkest;
            }
            const bool filledOutside = plan.rectCount > 0 && (plan.fill == gpu::bars::Fill::Blackout || plan.fill == gpu::bars::Fill::Backdrop);
            for (uint32_t v0 = 0; v0 < count; v0 += 4)
            {
                gpu::covers::Quad q;
                float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
                bool valid = true;
                for (uint32_t k = 0; k < 4 && valid; k++)
                {
                    const uint32_t v = src.Get(v0 + k) + first;
                    if ((v + 1) * 8 > words)
                    {
                        valid = false;
                        break;
                    }
                    float p[3];
                    for (uint32_t j = 0; j < 3; j++)
                    {
                        const uint32_t b = GpuSwap(LoadPhysical(base + (v * 8 + j) * 4), endian);
                        memcpy(&p[j], &b, 4);
                    }
                    const float x = p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + c[3];
                    const float y = p[0] * c[4] + p[1] * c[5] + p[2] * c[6] + c[7];
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                    q.argb[k] = GpuSwap(LoadPhysical(base + (v * 8 + 3) * 4), endian);
                }
                if (!valid)
                    break;
                const gpu::bars::Rect host = gpu::bars::ClipToHost(x0, x1, y0, y1, posScale, hostW, hostH);
                q.host = gpu::covers::Intersect({ host.x0, host.y0, host.x1, host.y1 }, sc);
                q.picture = picture;
                q.band[0] = (x0 - bandMidX) / s;
                q.band[1] = (x1 - bandMidX) / s;
                q.band[2] = (y0 - bandMidY) / sv;
                q.band[3] = (y1 - bandMidY) / sv;
                q.texel = t0Sampled ? texel : 0xFFFFFFFFu;
                q.texelKnown = texelKnown;
                q.blended = blended;
                q.filledOutside = filledOutside;
                const gpu::covers::Verdict verdict = gpu::covers::ClassifyQuad(q);
                if (verdict.kind == gpu::covers::Kind::None)
                    continue;
                game::covers::Chain chain;
                uint32_t quadWords[16];
                bool readable = true;
                for (uint32_t k = 0; k < 4 && readable; k++)
                {
                    const uint32_t v = src.Get(v0 + k) + first;
                    readable = (v + 1) * 8 <= words;
                    for (uint32_t j = 0; j < 4 && readable; j++)
                        quadWords[k * 4 + j] = GpuSwap(LoadPhysical(base + (v * 8 + j) * 4), endian);
                }
                const bool known = readable && game::covers::CallerOf(game::covers::QuadKey(quadWords), chain);
                // Once per site, kind, extent (to 1% of the picture), opacity.
                const float pw = float(std::max(picture.x1 - picture.x0, 1)), ph = float(std::max(picture.y1 - picture.y0, 1));
                uint64_t key = uint64_t(verdict.kind) | (uint64_t(verdict.minAlpha >= 0xF0) << 3);
                for (uint32_t i = 0; i < chain.n; i++)
                    key = key * 1000003ull ^ chain.lr[i];
                for (const int32_t e : verdict.shortBy)
                    key = key * 131ull + uint64_t(std::lround(float(e) / std::min(pw, ph) * 100.0f));
                key = key * 131ull + uint64_t(std::lround(s * 1000.0f)) * 7 + uint64_t(std::lround(sv * 1000.0f));
                static std::unordered_set<uint64_t> logged;
                if (logged.size() >= 512 || !logged.insert(key).second)
                    continue;
                s_coverHits.fetch_add(1, std::memory_order_relaxed);
                fprintf(stderr,
                    "[covers] %s: frame %llu caller %s; host [%d,%d)x[%d,%d) of the picture [%d,%d)x[%d,%d) (%.0f%% of it; short by "
                    "left %d top %d right %d bottom %d px), band x %.3f..%.3f y %.3f..%.3f, view scale x %.4f y %.4f; colour %08X %08X %08X "
                    "%08X (max RGB %02X, alpha %02X) blend %08X t0 %s; edge-bar rules: %s; vs_%016llx ps_%016llx n%u quad %u\n",
                    gpu::covers::KindName(verdict.kind), (unsigned long long)s_frame, known ? game::covers::Describe(chain).c_str()
                          : std::format("unknown (key {:016X}, {} quads noted)", readable ? game::covers::QuadKey(quadWords) : 0ull,
                                game::covers::Noted()).c_str(),
                    q.host.x0, q.host.x1, q.host.y0, q.host.y1, picture.x0, picture.x1, picture.y0, picture.y1, verdict.share * 100.0f,
                    verdict.shortBy[0], verdict.shortBy[1], verdict.shortBy[2], verdict.shortBy[3], q.band[0], q.band[1], q.band[2], q.band[3], s,
                    sv, q.argb[0], q.argb[1], q.argb[2], q.argb[3], verdict.maxRgb, verdict.minAlpha, blend0,
                    t0Sampled ? std::format("{:08X} {}x{} f{} darkest {:08X}", (uint32_t(t0.base_address) << 12) & 0x1FFFFFFFu, t0W, t0H,
                                    uint32_t(t0.format), texel).c_str()
                              : "-",
                    plan.fill == gpu::bars::Fill::None ? "none" : plan.rectCount ? "filled"
                        : plan.fill == gpu::bars::Fill::Vignette || plan.fill == gpu::bars::Fill::Layer ? "stretched" : "matched, nothing to fill",
                    (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, count, v0 / 4);
            }
        }

        // Get() for a whole draw, written straight into the ring: `out16`
        // keeps 16-bit indices (the pipeline's restart index is then 0xFFFF,
        // so only when restart is off or the guest's reset index is 0xFFFF);
        // otherwise 32-bit, with the reset index mapped to ~0 when `restart`.
        // For 16-bit indices, 8in16 and 8in32 swap each index's bytes; none
        // and 16in32 keep them (Get() picks the other half of the swapped word).
        // Returns the lowest and highest index written, restarts left out
        // (the vertices the draw runs: its vertex snapshots, gpu/vertex_snapshot.h).
        // Found in the same pass, the highest as the highest index + 1,
        // which wraps a restart (0xFFFF in 16 bits, ~0 in 32; never an index
        // there, see above) to 0: no branch keeps the loops from vectorizing.
        // 16 bits out without restart (not on Apple: see Draw), 0xFFFF is an
        // index: the highest + 1 is counted in 32 bits there.
        vsnap::IndexRange ConvertIndices(uint8_t* dst, const IndexSource& src, uint32_t count, bool out16, bool restart, uint32_t resetIndex)
        {
            auto range = [](uint32_t lo, uint32_t hiPlus1) {
                return hiPlus1 ? vsnap::IndexRange{ true, lo, hiPlus1 - 1 } : vsnap::IndexRange{ true, 1, 0 };  // (0: only restarts)
            };
            if (!src.is32)
            {
                const uint8_t* p = s_sharedHost + (src.base & 0x1FFFFFFE);
                bool swap = src.endian == 1 || src.endian == 2;
                if (out16 && !restart)
                {
                    uint32_t lo = 0xFFFFFFFFu, hiPlus1 = 0;
                    auto copy = [&](auto order) {
                        for (uint32_t i = 0; i < count; i++)
                        {
                            uint16_t v;
                            memcpy(&v, p + size_t(i) * 2, 2);
                            v = order(v);
                            memcpy(dst + size_t(i) * 2, &v, 2);
                            lo = std::min(lo, uint32_t(v));
                            hiPlus1 = std::max(hiPlus1, uint32_t(v) + 1);
                        }
                    };
                    if (swap)
                        copy([](uint16_t v) { return __builtin_bswap16(v); });
                    else
                        copy([](uint16_t v) { return v; });
                    return range(lo, hiPlus1);
                }
                if (out16)
                {
                    uint16_t lo = 0xFFFF, hiPlus1 = 0;
                    auto copy = [&](auto order) {
                        for (uint32_t i = 0; i < count; i++)
                        {
                            uint16_t v;
                            memcpy(&v, p + size_t(i) * 2, 2);
                            v = order(v);
                            memcpy(dst + size_t(i) * 2, &v, 2);
                            lo = std::min(lo, v);
                            hiPlus1 = std::max(hiPlus1, uint16_t(v + 1));
                        }
                    };
                    if (swap)
                        copy([](uint16_t v) { return __builtin_bswap16(v); });
                    else
                        copy([](uint16_t v) { return v; });
                    return range(lo, hiPlus1);
                }
                uint32_t lo = 0xFFFFFFFFu, hiPlus1 = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    uint16_t v;
                    memcpy(&v, p + size_t(i) * 2, 2);
                    uint32_t x = swap ? __builtin_bswap16(v) : v;
                    x = restart && x == resetIndex ? 0xFFFFFFFFu : x;
                    memcpy(dst + size_t(i) * 4, &x, 4);
                    lo = std::min(lo, x);
                    hiPlus1 = std::max(hiPlus1, x + 1);
                }
                return range(lo, hiPlus1);
            }
            const uint8_t* p = s_sharedHost + (src.base & 0x1FFFFFFC);
            uint32_t lo = 0xFFFFFFFFu, hiPlus1 = 0;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t w;
                memcpy(&w, p + size_t(i) * 4, 4);
                uint32_t v = GpuSwap(w, src.endian) & 0xFFFFFF;
                v = restart && v == resetIndex ? 0xFFFFFFFFu : v;
                memcpy(dst + size_t(i) * 4, &v, 4);
                lo = std::min(lo, v);
                hiPlus1 = std::max(hiPlus1, v + 1);
            }
            return range(lo, hiPlus1);
        }


        // ---------------------------------------------------------------
        // Textures: 2D, cube and 3D, with the guest's mip levels (2D, cube).
        // Validity comes from write watches (kernel/write_watch.h): a
        // texture reloads when any page it covers was written (a CPU store,
        // a file read or a resolve) since it was last loaded.

        struct TextureFormatInfo
        {
            VkFormat vk;
            uint32_t bpbLog2;   // guest bytes per block
            uint32_t block;     // block width/height in texels
            // 0 copy; 1 565, 2 1555, 3 4444 -> RGBA8; 4 24_8 -> R32F;
            // 5 10_11_11, 6 11_11_10 -> RGBA16 (blue also in alpha);
            // 7 DXT3A -> BC2 (its blocks as the alpha half, white colour)
            uint32_t convert;
        };

        // `f`: texformat::SampledAs(the fetch constant's format), so the
        // formats that hold another's data (50-56, 8) load as that one.
        // k_10_11_11 and k_11_11_10 are unsigned normalized 11/11/10 and
        // 10/11/11-bit components, red in the low bits (as the vertex
        // formats of those names: shader_translator.cpp); Xenia loads them
        // into RGBA16 too. k_DXT3A is DXT3's alpha block alone (4 bits a
        // texel) and k_DXT5A DXT5's, which BC4 is. Still placeholders:
        // k_1_REVERSE, k_1 and k_8_B (Xenia has no load for them either);
        // k_6_5_5, the YUV, EDRAM, EXPAND, MPEG and INTERLACED formats, the
        // plain 32-bit integer ones, k_32_AS_8*, k_32_32_32_FLOAT, k_CTX1
        // and k_DXT3A_AS_1_1_1_1 (each a load of its own).
        bool GetTextureFormat(uint32_t f, TextureFormatInfo& out)
        {
            switch (f)
            {
            case 2: out = { VK_FORMAT_R8_UNORM, 0, 1, 0 }; return true;
            case 3: out = { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 2 }; return true;
            case 4: out = { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1 }; return true;
            case 6: case 14: out = { VK_FORMAT_R8G8B8A8_UNORM, 2, 1, 0 }; return true;
            case 7: out = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, 2, 1, 0 }; return true;
            case 10: out = { VK_FORMAT_R8G8_UNORM, 1, 1, 0 }; return true;
            case 15: out = { VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 3 }; return true;
            case 16: out = { VK_FORMAT_R16G16B16A16_UNORM, 2, 1, 5 }; return true;
            case 17: out = { VK_FORMAT_R16G16B16A16_UNORM, 2, 1, 6 }; return true;
            case 18: out = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 3, 4, 0 }; return true;
            case 19: out = { VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 0 }; return true;
            case 20: out = { VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 0 }; return true;
            case 22: case 23: out = { VK_FORMAT_R32_SFLOAT, 2, 1, 4 }; return true;
            case 24: out = { VK_FORMAT_R16_UNORM, 1, 1, 0 }; return true;
            case 25: out = { VK_FORMAT_R16G16_UNORM, 2, 1, 0 }; return true;
            case 26: out = { VK_FORMAT_R16G16B16A16_UNORM, 3, 1, 0 }; return true;
            case 30: out = { VK_FORMAT_R16_SFLOAT, 1, 1, 0 }; return true;
            case 31: out = { VK_FORMAT_R16G16_SFLOAT, 2, 1, 0 }; return true;
            case 32: out = { VK_FORMAT_R16G16B16A16_SFLOAT, 3, 1, 0 }; return true;
            case 36: out = { VK_FORMAT_R32_SFLOAT, 2, 1, 0 }; return true;
            case 37: out = { VK_FORMAT_R32G32_SFLOAT, 3, 1, 0 }; return true;
            case 38: out = { VK_FORMAT_R32G32B32A32_SFLOAT, 4, 1, 0 }; return true;
            case 49: out = { VK_FORMAT_BC5_UNORM_BLOCK, 4, 4, 0 }; return true;
            case 58: out = { VK_FORMAT_BC2_UNORM_BLOCK, 3, 4, 7 }; return true;
            case 59: out = { VK_FORMAT_BC4_UNORM_BLOCK, 3, 4, 0 }; return true;
            default: return false;
            }
        }

        // A loaded block's bytes (log2): the guest's when copied; a
        // conversion's 32-bit texel, but for 5 and 6 (64-bit texels) and 7
        // (a 128-bit BC2 block).
        uint32_t LoadedBpbLog2(const TextureFormatInfo& fi)
        {
            return fi.convert == 0 ? fi.bpbLog2 : fi.convert == 7 ? 4 : fi.convert >= 5 ? 3 : 2;
        }

        // Stored as resolves write it: one 32-bit word a texel, unconverted
        // (the formats a scaled texture can have).
        bool ScaledFormat(const TextureFormatInfo& fi)
        {
            return fi.bpbLog2 == 2 && fi.convert == 0 && fi.block == 1;
        }

        uint32_t Tiled2D(uint32_t x, uint32_t y, uint32_t pitchAligned, uint32_t bppLog2)
        {
            uint32_t outer = (((y >> 5) * (pitchAligned >> 5)) + (x >> 5)) << 6;
            uint32_t inner = (((y >> 1) & 7) << 3) | (x & 7);
            uint32_t oi = (outer | inner) << bppLog2;
            uint32_t bank = (y >> 4) & 1;
            uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
            return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xF) | (((oi >> 4) & 1) << 5) |
                (((oi >> 5) & 7) << 8) | ((oi >> 8) << 12);
        }

        uint32_t Log2Ceil(uint32_t v)
        {
            return v <= 1 ? 0 : 32 - __builtin_clz(v - 1);
        }

        // Mip levels (Xenia's texture_util GetGuestTextureLayout and
        // GetPackedMipOffset, for 2D and cube textures). Levels 1.. are
        // stored one after another from mip_address, each (per face) with a
        // pitch and height from the base size rounded up to a power of two,
        // shifted to the level and aligned to 32x32 blocks (linear rows to
        // 256 bytes), faces 4 KB-aligned. With packed mips, the levels from
        // the one whose short side is 16 texels (packedLevel) share that
        // level's storage, each at an offset in its 32x32 tile; when the
        // base itself is that small (packedLevel 0), the mips' shared tile
        // is at mip_address.
        struct MipLayout
        {
            uint32_t width, height, block, bpbLog2, layers;
            bool tiled, packed;
            uint32_t packedLevel;

            uint32_t StorageLevel(uint32_t level) const { return packed ? std::min(level, packedLevel) : level; }
            uint32_t PitchBlocks(uint32_t storage) const  // 32-aligned
            {
                uint32_t texels = std::max(std::bit_ceil(width) >> storage, 1u);
                return (((texels + block - 1) / block) + 31) & ~31u;
            }
            uint32_t RowsBlocks(uint32_t storage) const  // 32-aligned
            {
                uint32_t texels = std::max(std::bit_ceil(height) >> storage, 1u);
                return (((texels + block - 1) / block) + 31) & ~31u;
            }
            uint32_t RowBytes(uint32_t storage) const  // linear
            {
                return ((PitchBlocks(storage) << bpbLog2) + 255) & ~255u;
            }
            uint32_t SliceStride(uint32_t storage) const
            {
                uint32_t bytes = tiled ? (PitchBlocks(storage) * RowsBlocks(storage)) << bpbLog2 : RowBytes(storage) * RowsBlocks(storage);
                return (bytes + 4095) & ~4095u;
            }
            uint32_t Offset(uint32_t storage) const  // from mip_address
            {
                uint32_t offset = 0;
                for (uint32_t s = 1; s < storage; s++)
                    offset += SliceStride(s) * layers;
                return offset;
            }
            // The level's position in its storage, in blocks.
            void PackedOffset(uint32_t level, uint32_t& x, uint32_t& y) const
            {
                x = y = 0;
                if (!packed || level < packedLevel)
                    return;
                uint32_t log2w = Log2Ceil(width), log2h = Log2Ceil(height);
                uint32_t log2Size = std::min(log2w, log2h);
                uint32_t base = log2Size > 4 ? log2Size - 4 : 0;
                uint32_t sub = level - base;
                if (sub < 3)
                {
                    if (log2w > log2h)
                        y = 16 >> sub;
                    else
                        x = 16 >> sub;
                }
                else if (log2w > log2h)
                    x = (1u << (log2w - base)) >> (sub - 2);
                else
                    y = (1u << (log2h - base)) >> (sub - 2);
                x /= block;
                y /= block;
            }
        };

        // Texture loading runs on the GPU: a compute shader untiles and
        // converts from guest memory into the ring, then a buffer->image copy.
        // Ordering against resolves is then just a barrier (no CPU wait).
        // ---------------------------------------------------------------
        // Scaled resolve memory (internal resolution above 1x)
        //
        // A resolve from a scaled render target writes each guest pixel's
        // host samples here, besides the 1x average into guest memory
        // (which the CPU, 1x draws and the auto-exposure readback keep
        // using). Storage is per 4 KB guest page: a slot of 4096 x s bytes
        // (s = sx x sy host pixels per guest pixel: 9/4 at 1.5x) in one pool
        // buffer, found through a page table (a GPU buffer and its CPU copy),
        // as MoltenVK has no sparse buffers. At a whole scale the samples of
        // the 32-bit texel at guest byte address A are at
        //   slot(A >> 12) x 4096 x s + (A & 4095) x s + (j x sx + i) x 4;
        // at 1.5x a 2x2 block of texels keeps its 3x3 samples together
        // (gpu/render_scale.h, sbSampleWord, has the layout), and below 1x
        // a block of 2x2 (0.5x), 4x4 (0.75x) or 8x8 texels (0.625x) its 1x1,
        // 3x3 or 5x5 (s 1/4, 9/16, 25/64: a slot smaller than the page).
        // A page's scaled copy is current while the last write to the page
        // was a scaled resolve: its write sequence is still the one recorded
        // right after that resolve (a CPU write, a 1x resolve or any other
        // GPU write moves it on). Draws into scaled targets then sample
        // scaled textures made from it, and the front buffer is presented at
        // host resolution. NFSMW_SCALED_RESOLVE=0 keeps everything 1x (the
        // targets are still scaled: supersampling). NFSMW_SCALED_MB sizes the
        // pool (default 512). Scaled textures are stored into straight from
        // the pool through an R32_UINT view; NFSMW_SCALED_DIRECT=0 (or a
        // device that can't) loads them through a scratch buffer and a copy.
        // Scaled resolves write the pool coalesced (see the resolve shader;
        // not on Apple GPUs); NFSMW_SCALED_COALESCE=0 has each thread store
        // its own samples, =1 coalesces on any GPU.
        constexpr uint32_t kGuestPages = uint32_t(kSharedSize >> 12);
        struct ScaledMemory
        {
            bool tried = false, enabled = false, direct = false, coalesce = false;
            gpu::RenderScale scale;  // the scale its samples are at
            uint32_t slotWords = 0, slots = 0;
            VkBuffer pool = VK_NULL_HANDLE, table = VK_NULL_HANDLE, scratch = VK_NULL_HANDLE;
            VkDeviceMemory poolMemory = VK_NULL_HANDLE, tableMemory = VK_NULL_HANDLE, scratchMemory = VK_NULL_HANDLE;
            VkDeviceSize scratchSize = 0;  // also the largest scaled texture (no buffer when direct)
            VkDeviceSize poolBytes = 0, tableBytes = 0, scratchBytes = 0;  // allocated (the [perf] line's memory)
            std::vector<uint32_t> pageSlot;  // slot + 1; 0: none
            std::vector<uint64_t> pageSeq;   // write sequence after the page's last scaled resolve; 0: none or stale
            // The cached scaled texture (a Texture) that alone holds the page's
            // samples: its last scaled resolve stored them there and not in
            // the pool (see Resolve). Null: the pool has them (if current).
            std::vector<const void*> pageHeld;
            std::vector<uint32_t> slotPage;  // page + 1; 0: free
            std::vector<uint8_t> slotRef;    // clock bit (used since the hand last passed)
            uint32_t clock = 0;
            std::vector<uint32_t> dirty;     // pages whose table entry changed
            uint64_t allocations = 0, evictions = 0, textures = 0, fallbacks = 0;
            uint64_t loads = 0, untiledBytes = 0, resolvedBytes = 0;  // for the [perf] line
            uint64_t hosted = 0;  // scaled resolves that stored into their scaled texture too
            uint32_t hostPageGuestPages = 1;  // guest pages per host page (write sequences are per host page)
            // Below 1x (ScaledWriteWanted): per destination, its scaled
            // copies left unread in a row; per page, the last scaled
            // resolve's destination (index + 1, 0: none) and whether a reader
            // took the page's scaled copy since. 1x-only resolves skipped so far.
            struct Writer { uint64_t key; uint32_t unread; };
            std::vector<Writer> writers;
            std::vector<uint16_t> pageWriter;
            std::vector<uint8_t> pageRead;
            uint64_t unreadSkips = 0;
        } s_scaled;

        bool EnsureScaledMemory();
        bool ScaledWanted(uint32_t base, uint32_t size);
        bool ScaledWanted(uint32_t base, uint32_t size, uint32_t exceptBase, uint32_t exceptSize, uint64_t exceptSig);
        void FillFreshSlots(const std::vector<std::pair<uint32_t, uint32_t>>& fresh);

        // The page's slot, allocated (clock replacement) if it has none.
        uint32_t ScaledSlot(uint32_t page, bool& fresh)
        {
            fresh = false;
            uint32_t& entry = s_scaled.pageSlot[page];
            if (entry)
            {
                s_scaled.slotRef[entry - 1] = 1;
                return entry - 1;
            }
            uint32_t slot;
            for (;;)
            {
                slot = s_scaled.clock;
                s_scaled.clock = (slot + 1) % s_scaled.slots;
                if (!s_scaled.slotPage[slot])
                    break;
                if (s_scaled.slotRef[slot])
                {
                    s_scaled.slotRef[slot] = 0;
                    continue;
                }
                // Evict: that page falls back to its 1x copy. (Command
                // buffers are ordered by full barriers, so earlier work is
                // done with the slot before the table changes.)
                uint32_t old = s_scaled.slotPage[slot] - 1;
                s_scaled.pageSlot[old] = 0;
                s_scaled.pageSeq[old] = 0;
                s_scaled.pageHeld[old] = nullptr;
                s_scaled.dirty.push_back(old);
                s_scaled.evictions++;
                break;
            }
            s_scaled.slotPage[slot] = page + 1;
            s_scaled.slotRef[slot] = 1;
            entry = slot + 1;
            s_scaled.dirty.push_back(page);
            s_scaled.allocations++;
            fresh = true;
            return slot;
        }

        // Upload changed page-table entries (in the command stream, so the
        // GPU sees them in order).
        void FlushScaledTable()
        {
            auto& dirty = s_scaled.dirty;
            if (dirty.empty())
                return;
            std::sort(dirty.begin(), dirty.end());
            dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
            EndPass(kBreakUpload);
            Begin();
            for (size_t i = 0; i < dirty.size();)
            {
                size_t j = i + 1;
                while (j < dirty.size() && dirty[j] == dirty[j - 1] + 1 && j - i < 16384)
                    j++;
                vkCmdUpdateBuffer(s_cmd, s_scaled.table, VkDeviceSize(dirty[i]) * 4, VkDeviceSize(j - i) * 4,
                    &s_scaled.pageSlot[dirty[i]]);
                i = j;
            }
            dirty.clear();
            FullBarrier();
        }

        // The page's scaled copy is current (see above) and in the pool.
        bool ScaledPageValid(uint32_t page)
        {
            uint64_t seq = s_scaled.pageSeq[page];
            return seq != 0 && s_scaled.pageSlot[page] != 0 && !s_scaled.pageHeld[page] && writewatch::WriteSequence(page << 12, 4096) == seq;
        }
        // ... current, but only in the scaled texture holding it (pageHeld).
        bool ScaledPageHeld(uint32_t page)
        {
            uint64_t seq = s_scaled.pageSeq[page];
            return seq != 0 && s_scaled.pageSlot[page] != 0 && s_scaled.pageHeld[page] && writewatch::WriteSequence(page << 12, 4096) == seq;
        }

        // Below 1x: a destination whose scaled copy went unread this many
        // times in a row keeps none (ScaledWriteWanted).
        constexpr uint32_t kUnreadSkip = 8;

        // Every page of [base, base + size) has a current scaled copy. Below
        // 1x a reader asks (a scaled texture, the front buffer): the pages
        // were read (ScaledWriteWanted), or one without a copy is wanted
        // again from its writer.
        bool ScaledRangeValid(uint32_t base, uint32_t size)
        {
            if (!s_scaled.enabled || size == 0)
                return false;
            uint32_t first = base >> 12, last = std::min<uint32_t>((base + size - 1) >> 12, kGuestPages - 1);
            const bool below = s_scaled.scale.Below() && !s_scaled.pageWriter.empty();
            for (uint32_t page = first; page <= last; page++)
                if (!ScaledPageValid(page))
                {
                    if (below)
                        for (uint32_t p = page; p <= last; p++)
                            if (uint32_t w = s_scaled.pageWriter[p]; w && s_scaled.writers[w - 1].unread >= kUnreadSkip)
                                s_scaled.writers[w - 1].unread = 0;
                    return false;
                }
            for (uint32_t page = first; page <= last; page++)
                s_scaled.slotRef[s_scaled.pageSlot[page] - 1] = 1;
            if (below)
                std::fill(s_scaled.pageRead.begin() + first, s_scaled.pageRead.begin() + last + 1, uint8_t(1));
            return true;
        }

        // A resolve's pages: [first, last], its extent (ResolveExtent), and
        // with a page-aligned base of 32-bit texels, where each page is one
        // 32x32 tile, the tiles its rectangle (destination texels [x0, x1) x
        // [y0, y1)) touches or covers whole. The extent also holds the tiles
        // of the rows between that lie beside the rectangle (the right half's
        // extent runs through the left half's tiles of every row but its last).
        struct ResolvePages
        {
            uint32_t first, last;
            bool tiles;  // page-aligned 32-bit destination: a page is a tile
            uint32_t basePage, tilesPerRow;
            int32_t x0, y0, x1, y1;

            bool Touches(uint32_t page) const
            {
                if (!tiles)
                    return true;
                const uint32_t t = page - basePage;
                const int32_t tx = int32_t(t % tilesPerRow * 32), ty = int32_t(t / tilesPerRow * 32);
                return page >= basePage && tx < x1 && tx + 32 > x0 && ty < y1 && ty + 32 > y0;
            }
            bool Covers(uint32_t page) const
            {
                if (!tiles)
                    return false;
                const uint32_t t = page - basePage;
                const int32_t tx = int32_t(t % tilesPerRow * 32), ty = int32_t(t / tilesPerRow * 32);
                return page >= basePage && tx >= x0 && tx + 32 <= x1 && ty >= y0 && ty + 32 <= y1;
            }
        };

        // Below 1x, a scaled resolve of `pages` to destination `key` (base,
        // size, pitch, format): whether to keep its scaled samples. They cost
        // about what its 1x copy does (a 1280x720 one 0.18 ms at the Steam
        // Deck's capped clock), and the game writes surfaces it never samples
        // at host resolution afterwards: its final composite goes over the
        // scene's texture, read again only once the next frame's scene is
        // there. The last destinations of the pages it writes (those its
        // rectangle touches) learn here whether a reader took their copy
        // before this overwrote it (any of their pages: a texture may read
        // part); one unread kUnreadSkip times in a row keeps the 1x copy only
        // from then on, its pages without a scaled one, so a reader uses the
        // 1x data, and that reader wants it again (ScaledRangeValid): its next
        // resolve keeps the samples, the frames in between showing the 1x
        // data. At 1x and above (and with no room for more destinations):
        // always.
        bool ScaledWriteWanted(uint64_t key, const ResolvePages& pages)
        {
            if (!s_scaled.scale.Below() || s_scaled.pageWriter.empty())
                return true;
            auto& writers = s_scaled.writers;
            uint32_t self = 0;
            while (self < writers.size() && writers[self].key != key)
                self++;
            if (self == writers.size())
            {
                if (writers.size() >= 0xFFFF)
                    return true;
                writers.push_back({ key, 0 });
            }
            // The pages' previous destinations, each once: read or not.
            uint32_t seen[8], seenCount = 0;
            bool read[8] = {};
            for (uint32_t page = pages.first; page <= pages.last; page++)
            {
                uint32_t w = s_scaled.pageWriter[page];
                if (!w || !pages.Touches(page))
                    continue;
                uint32_t i = 0;
                while (i < seenCount && seen[i] != w)
                    i++;
                if (i == seenCount)
                {
                    if (seenCount == 8)
                        continue;  // (more than 8 destinations under one resolve: the rest aren't judged)
                    seen[seenCount++] = w;
                }
                read[i] = read[i] || s_scaled.pageRead[page];
            }
            for (uint32_t i = 0; i < seenCount; i++)
            {
                auto& w = writers[seen[i] - 1];
                if (read[i])
                    w.unread = 0;
                else if (w.unread < UINT32_MAX)
                    w.unread++;
            }
            for (uint32_t page = pages.first; page <= pages.last; page++)
                if (pages.Touches(page))
                {
                    s_scaled.pageWriter[page] = uint16_t(self + 1);
                    s_scaled.pageRead[page] = 0;
                }
            return writers[self].unread < kUnreadSkip;
        }

        const char* kUntileGlsl = R"(#version 460
// 24-bit depth to a depth texture's float: as the resolve shader's D24F
// (NFSMW_RESOLVE_LAZY: SB_D24_EXACT), so a texture loaded here and one a
// resolve stored hold the same values.
#define D24F(d) (float(d) / 16777215.0)
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer SharedMemory { uint g_mem[]; };)" GLSL_SHARED_MEMORY_READ R"(
#ifdef TO_IMAGE
// 1x textures of 32-bit texels, stored into the image through an R32_UINT view
// (no staging copy): x is the texel, as the staging row's word index.
layout(set = 0, binding = 1, r32ui) writeonly uniform uimage2D o_image;
#define STORE(x, v) imageStore(o_image, ivec2(int(x), int(gy)), uvec4((v), 0u, 0u, 0u))
#else
layout(std430, set = 0, binding = 1) writeonly buffer Staging { uint o_mem[]; };
#define STORE(x, v) o_mem[row + (x)] = (v)
#endif
layout(push_constant) uniform P
{
    uint srcBase, tiled, pitch, bpbLog2;   // pitch: tiled = blocks (32-aligned), linear = bytes
    uint offX, offY, width, height;        // blocks
    uint endian, convert, dstWord, dstRowWords;
    uint z, heightAligned;                 // tiled == 2: 3D slice and aligned height (blocks)
} p;

uint tiledCombine(uint oi, uint bank, uint pipe, uint yLsb)
{
    return (yLsb << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

uint tiled3D(uint x, uint y, uint z, uint pitch, uint height, uint bppLog2)
{
    uint outer = ((((z >> 2) * (height >> 4) + (y >> 4)) * (pitch >> 5)) + (x >> 5)) << 7;
    uint inner = ((z & 3u) << 5) | (((y >> 1) & 3u) << 3) | (x & 7u);
    uint bank = ((y >> 3) ^ (z >> 2)) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (bank << 1);
    return tiledCombine((outer | inner) << bppLog2, bank, pipe, y & 1u);
}

uint tiled2D(uint x, uint y, uint pitch, uint bppLog2)
{
    uint outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outer | inner) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

uint swapWord(uint v, uint e)
{
    if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (e == 3u) return (v >> 16) | (v << 16);
    return v;
}

uint addr(uint bx, uint by)
{
    uint x = bx + p.offX, y = by + p.offY;
    if (p.tiled == 2u)
        return p.srcBase + tiled3D(x, y, p.z, p.pitch, p.heightAligned, p.bpbLog2);
    return p.srcBase + (p.tiled != 0u ? tiled2D(x, y, p.pitch, p.bpbLog2) : y * p.pitch + (x << p.bpbLog2));
}

uint word(uint a) { return swapWord(memLoad((a & 0x1FFFFFFFu) >> 2), p.endian); }

uint element(uint bx, uint by)
{
    if (bx >= p.width) return 0u;
    uint a = addr(bx, by);
    uint w = word(a & ~3u);
    return p.bpbLog2 == 1u ? (w >> ((a & 2u) * 8u)) & 0xFFFFu : (w >> ((a & 3u) * 8u)) & 0xFFu;
}

uint expand(uint v, uint bits) { uint m = (1u << bits) - 1u; return (v * 255u + m / 2u) / m; }
uint expand16(uint v, uint bits) { uint m = (1u << bits) - 1u; return (v * 65535u + m / 2u) / m; }

void main()
{
    uint gx = gl_GlobalInvocationID.x, gy = gl_GlobalInvocationID.y;
    if (gy >= p.height) return;
    uint row = p.dstWord + gy * p.dstRowWords;
    // Conversions wider than a word (never stored directly): 5 10_11_11 and
    // 6 11_11_10 -> RGBA16 (red in the low bits; blue also in alpha, the
    // last component repeated as the texture unit does); 7 a DXT3A block ->
    // a BC2 block, its 4-bit alphas as BC2's alpha half and a white colour
    // block (both endpoints 0xFFFF, every index 0).
    if (p.convert >= 5u)
    {
        if (gx >= p.width) return;
        uint a = addr(gx, gy);
        if (p.convert == 7u)
        {
            STORE(gx * 4u, word(a));
            STORE(gx * 4u + 1u, word(a + 4u));
            STORE(gx * 4u + 2u, 0xFFFFFFFFu);
            STORE(gx * 4u + 3u, 0u);
            return;
        }
        uint w = word(a);
        uint r, g, b;
        if (p.convert == 5u) { r = expand16(w & 0x7FFu, 11u); g = expand16((w >> 11) & 0x7FFu, 11u); b = expand16(w >> 22, 10u); }
        else { r = expand16(w & 0x3FFu, 10u); g = expand16((w >> 10) & 0x7FFu, 11u); b = expand16(w >> 21, 11u); }
        STORE(gx * 2u, r | (g << 16));
        STORE(gx * 2u + 1u, b | (b << 16));
        return;
    }
    if (p.convert != 0u)
    {
        if (gx >= p.width) return;
        uint o;
        if (p.convert == 4u)
        {
            o = floatBitsToUint(D24F(word(addr(gx, gy)) >> 8));
        }
        else
        {
            uint v = element(gx, gy);
            uint r, g, b, a;
            if (p.convert == 1u) { r = expand(v & 31u, 5u); g = expand((v >> 5) & 63u, 6u); b = expand((v >> 11) & 31u, 5u); a = 255u; }
            else if (p.convert == 2u) { r = expand(v & 31u, 5u); g = expand((v >> 5) & 31u, 5u); b = expand((v >> 10) & 31u, 5u); a = (v >> 15) != 0u ? 255u : 0u; }
            else { r = expand(v & 15u, 4u); g = expand((v >> 4) & 15u, 4u); b = expand((v >> 8) & 15u, 4u); a = expand(v >> 12, 4u); }
            o = r | (g << 8) | (b << 16) | (a << 24);
        }
        STORE(gx, o);
        return;
    }
    if (p.bpbLog2 >= 2u)
    {
        if (gx >= p.width) return;
        uint words = 1u << (p.bpbLog2 - 2u);
        uint a = addr(gx, gy);
        for (uint k = 0u; k < words; k++)
            STORE(gx * words + k, word(a + k * 4u));
        return;
    }
    uint perWord = 4u >> p.bpbLog2;
    uint bx = gx * perWord;
    if (bx >= p.width) return;
    uint o = 0u;
    for (uint k = 0u; k < perWord; k++)
        o |= element(bx + k, gy) << (k * (8u << p.bpbLog2));
    STORE(gx, o);
}
)";

        VkPipeline s_untilePipeline = VK_NULL_HANDLE;
        VkPipelineLayout s_untileLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_untileSetLayout = VK_NULL_HANDLE;
        // 1x textures stored directly (TO_IMAGE): 2D (or, NFSMW_DIRECT_CUBE, a
        // cube, a face at a time), one level, uncompressed, 32-bit texels; the
        // untile writes the image, with no staging slice and no copy after it
        // (on the Steam Frame a 1600x1600 shadow map's upload was ~0.7 ms,
        // about half of it the copy). NFSMW_UNTILE_DIRECT=0 keeps the staging
        // path.
        VkPipeline s_untileStorePipeline = VK_NULL_HANDLE;
        VkPipelineLayout s_untileStoreLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_untileStoreSetLayout = VK_NULL_HANDLE;
        const bool s_untileDirect = [] { const char* v = std::getenv("NFSMW_UNTILE_DIRECT"); return !v || v[0] != '0'; }();
        // NFSMW_DIRECT_CUBE=1 (default kFewerPasses; 0: cubes take the staging path): an
        // un-mipped cube is a direct texture too, each face stored through an
        // R32_UINT 2D view of its layer (Texture::storeViews), so a resolve
        // into a face stores into it as well (Resolve). The race's
        // environment cube (256x256, faces resolved every frame) was staged
        // again at its first use in the scene, on every platform: six
        // untiles, six copies and a split render pass a frame (two in heavy
        // scenes). A device that turns the image or a view down keeps the
        // staging path for that format.
        const bool s_directCube = [] { const char* v = std::getenv("NFSMW_DIRECT_CUBE"); return v ? v[0] == '1' : kFewerPasses; }();
        // fi.vk -> a storable R32_UINT view is allowed (2D; cube compatible, with six layers)
        std::unordered_map<VkFormat, bool> s_directFormats, s_directCubeFormats;

        bool DirectFormat(VkFormat format, bool cube = false)
        {
            auto [it, fresh] = (cube ? s_directCubeFormats : s_directFormats).try_emplace(format, false);
            if (fresh)
            {
                VkImageFormatProperties props;
                it->second = vkGetPhysicalDeviceImageFormatProperties(s_vk->physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
                    &props) == VK_SUCCESS && (!cube || props.maxArrayLayers >= 6);
            }
            return it->second;
        }

        struct UntileConstants
        {
            uint32_t srcBase, tiled, pitch, bpbLog2;
            uint32_t offX, offY, width, height;
            uint32_t endian, convert, dstWord, dstRowWords;
            uint32_t z, heightAligned;
        };

        struct Texture
        {
            Image image;
            VkFormat format;
            uint32_t width, height;
            uint32_t guestStart, guestSize;
            uint64_t loadedSequence = 0;  // writewatch::Current() when last loaded
            uint64_t checkedEpoch = ~0ull;  // writewatch::WriteEpoch() at the last validity check
            bool uploaded = false;
            bool cube = false;
            bool volume = false;
            uint32_t layers = 1, depth = 1;
            uint32_t levels = 1;
            uint32_t mipStart = 0, mipSize = 0;  // the mip levels' guest memory (size 0: none)
            bool scaled = false;  // host resolution, from the scaled resolve memory
            uint32_t guestWidth = 0, guestHeight = 0;  // scaled: its guest size (width, height are the host's)
            uint64_t scaledSig = 0;  // scaled: ScaledSignature
            VkImageView storeView = VK_NULL_HANDLE;  // scaled: R32_UINT, stored into by the untile (if the device can)
            // The same, a 2D view of each layer: [0] is storeView, and a
            // direct cube (NFSMW_DIRECT_CUBE) has one per face.
            VkImageView storeViews[6] = {};
            std::unordered_map<uint32_t, VkImageView> views;  // by swizzle
            // How the untile reads a direct 1x texture (a resolve can store
            // into it the same way, see Resolve); sliceStride: a cube's guest
            // bytes from one face to the next.
            bool tiled = false;
            uint32_t pitch = 0, bpbLog2 = 0, convert = 0, endian = 0, offX = 0, offY = 0, sliceStride = 0;
            // NFSMW_TEXTURE_VERIFY: a hash of each 4 KB guest page of its
            // base, then its mips, as loaded (empty: not checked), and the
            // frame it was last checked in.
            std::vector<uint64_t> verifyHashes;
            uint64_t verifyFrame = 0;
            // s_frame at its latest use: a lookup (a draw binding it), or a
            // resolve storing into it. The cache evicts the oldest
            // (TrimTextureCache), never one used since the last swap.
            uint64_t lastUsed = 0;
            // NFSMW_RESOLVE_RENAME: sampled from this image
            // instead of its own (a depth target's, which a resolve handed it
            // whole: renamedFormat, its depth aspect), whose texels are stale
            // meanwhile (MaterializeRename).
            VkImage renamedImage = VK_NULL_HANDLE;
            VkFormat renamedFormat = VK_FORMAT_UNDEFINED;
        };
        void LoadScaledTexture(Texture& t, uint32_t base, uint32_t pitchBlocks, uint32_t endian);
        bool ScaledTextureValid(uint32_t base, uint32_t size, uint64_t sig);
        // A scaled texture's surface: its guest format, size, pitch and
        // endianness (with its first byte and guest size, its cache key's
        // fields for a scaled texture).
        uint64_t ScaledSignature(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch, uint32_t endian)
        {
            uint64_t h = 0x9E3779B97F4A7C15ull;
            for (uint64_t v : { uint64_t(format), uint64_t(width), uint64_t(height), uint64_t(pitch), uint64_t(endian) })
                h = (h ^ v) * 0x100000001B3ull;
            return h | 1;  // (never 0: no texture)
        }

        // Every page of the surface [base, base + size) has current samples
        // for the scaled texture `sig` there: in the pool, or held by that
        // very texture (pageHeld) while it is current (nothing written there
        // since its load or its last resolve). A held page is never current
        // for any other reader (the pool's copy of it is old).
        bool ScaledSurfaceValid(uint32_t base, uint32_t size, uint64_t sig)
        {
            if (!s_scaled.enabled || size == 0)
                return false;
            uint32_t first = base >> 12, last = std::min<uint32_t>((base + size - 1) >> 12, kGuestPages - 1);
            const void* checked = nullptr;
            for (uint32_t page = first; page <= last; page++)
            {
                if (ScaledPageValid(page))
                    continue;
                if (!ScaledPageHeld(page))
                    return false;
                const void* holder = s_scaled.pageHeld[page];
                if (holder != checked)
                {
                    const Texture& t = *static_cast<const Texture*>(holder);
                    if (!sig || !t.scaled || t.scaledSig != sig || t.guestStart != base || t.guestSize != size || !t.uploaded ||
                        writewatch::WrittenSince(t.guestStart, t.guestSize, t.loadedSequence))
                        return false;
                    checked = holder;
                }
            }
            for (uint32_t page = first; page <= last; page++)
                s_scaled.slotRef[s_scaled.pageSlot[page] - 1] = 1;
            return true;
        }
        // A page held by `t` no longer is: its samples are gone (`t` dropped).
        void ReleaseHeldPages(const void* t, uint32_t base, uint32_t size)
        {
            if (!s_scaled.enabled || size == 0 || s_scaled.pageHeld.empty())
                return;
            uint32_t first = base >> 12, last = std::min<uint32_t>((base + size - 1) >> 12, kGuestPages - 1);
            for (uint32_t page = first; page <= last; page++)
                if (s_scaled.pageHeld[page] == t)
                {
                    s_scaled.pageHeld[page] = nullptr;
                    s_scaled.pageSeq[page] = 0;
                }
        }
        std::unordered_map<uint64_t, std::unique_ptr<Texture>> s_textures;
        // Textures the cache evicted, out of every lookup table already,
        // each with the submission after which nothing can use it: destroyed
        // once that one has completed (ReleaseRetiredTextures).
        struct RetiredTexture
        {
            std::unique_ptr<Texture> texture;
            uint64_t submission;
        };
        std::vector<RetiredTexture> s_retiredTextures;
        // Direct 1x 2D textures by guest address (the latest made there), for
        // resolves that store into them too (DestroyTexture drops its entry).
        std::unordered_map<uint32_t, Texture*> s_directByBase;
        // Resolves store into such a texture too when it is exactly their
        // surface (see Resolve); NFSMW_RESOLVE_TO_TEXTURE=0 turns it off.
        const bool s_resolveToTexture = [] { const char* v = std::getenv("NFSMW_RESOLVE_TO_TEXTURE"); return !v || v[0] != '0'; }();
        // ... also from a scaled resolve (its 1x words); NFSMW_SCALED_FUSE=0
        // turns that off (and the scaled textures' own, see Resolve).
        const bool s_scaledFuse = [] { const char* v = std::getenv("NFSMW_SCALED_FUSE"); return !v || v[0] != '0'; }();
        // ... and then only there, where no other reader asks (see Resolve);
        // NFSMW_SCALED_HOLD=0: into the pool as well, always.
        const bool s_scaledHold = [] { const char* v = std::getenv("NFSMW_SCALED_HOLD"); return !v || v[0] != '0'; }();
        // NFSMW_FUSE_PARTIAL=1 (default kFewerPasses; 0: only a resolve
        // covering the whole texture stores into it): a resolve into whole 32x32 tiles
        // inside a direct texture (or a cube's face) stores there too, at
        // those tiles' texels (Resolve). The scene's colour and depth
        // (1280x720) are each resolved in three strips, a predicated tile
        // each, and were untiled again between passes every frame (3.7 MB
        // each), on every platform.
        const bool s_fusePartial = [] { const char* v = std::getenv("NFSMW_FUSE_PARTIAL"); return v ? v[0] == '1' : kFewerPasses; }();
        // Direct textures by their first guest byte (the latest made there),
        // for resolves into part of one (Resolve): every one with
        // NFSMW_FUSE_PARTIAL, the cubes with NFSMW_DIRECT_CUBE alone; and the
        // largest one's guest size, how far before an address one holding
        // it can start.
        std::map<uint32_t, Texture*> s_directByStart;
        // Scaled textures stored into directly, by their first guest byte (the
        // latest made there), for scaled resolves into them (Resolve); the
        // largest one's guest size.
        std::map<uint32_t, Texture*> s_scaledByStart;
        uint32_t s_maxScaledSize = 0;
        uint32_t s_maxDirectSize = 0;

        // NFSMW_LOG_UPLOADS=<first>[,<last>]: in those frames (s_frame, as
        // NFSMW_PASS_PROFILE numbers them), a line per texture (re)load: its
        // path, whether it ended a render pass, and for a reload the guest
        // pages written since the last one with what last wrote each
        // (write_watch keeps that while this is set); a line per resolve
        // (stored into its cached texture or not, and why); a summary at the
        // end of each frame. Null when off.
        struct UploadLog
        {
            uint64_t first = 0, last = 0;
            bool now = false;  // s_frame is logged
            struct Resolve { uint64_t frame; const char* unfused; };  // unfused: null when it stored into the texture
            std::unordered_map<uint32_t, Resolve> resolves;  // by destination, the latest (every frame)
            uint32_t uploads = 0, endedPasses = 0, resolvesNow = 0, fusedNow = 0;
            uint64_t bytes = 0;
            std::map<std::string, uint32_t> byPath, byWriters;
        };
        UploadLog* const s_uploadLog = []() -> UploadLog* {
            const char* v = std::getenv("NFSMW_LOG_UPLOADS");
            if (!v)
                return nullptr;
            auto* log = new UploadLog();
            log->first = log->last = std::strtoull(v, nullptr, 10);
            if (const char* comma = std::strchr(v, ','))
                log->last = std::strtoull(comma + 1, nullptr, 10);
            return log;
        }();

        // Whether the latest resolve to `dest` stored into its cached texture.
        std::string ResolveNote(uint32_t dest)
        {
            auto it = s_uploadLog->resolves.find(dest);
            if (it == s_uploadLog->resolves.end())
                return "?";
            return it->second.unfused ? std::string("unfused: ") + it->second.unfused : "fused";
        }

        // The guest pages of `t` written since its load, and what last wrote
        // them (the first 4, then counts by kind, also added to `kinds`).
        std::string DescribeWrites(const Texture& t, std::set<std::string>& kinds)
        {
            auto pageCount = [](uint32_t b, uint32_t n) { return n ? ((b + n - 1) >> 12) - (b >> 12) + 1 : 0u; };
            auto inTexture = [&](uint32_t a) { return a - t.guestStart < t.guestSize || (t.mipSize && a - t.mipStart < t.mipSize); };
            std::vector<uint32_t> pages(pageCount(t.guestStart, t.guestSize) + pageCount(t.mipStart, t.mipSize));
            uint32_t n = writewatch::WrittenPages(t.guestStart, t.guestSize, t.loadedSequence, pages.data(), pageCount(t.guestStart, t.guestSize));
            uint32_t mipN = t.mipSize ? writewatch::WrittenPages(t.mipStart, t.mipSize, t.loadedSequence, pages.data() + n,
                pageCount(t.mipStart, t.mipSize)) : 0;
            std::string text = std::format("dirty {}/{} pages, mips {}/{}:", n, pageCount(t.guestStart, t.guestSize), mipN,
                pageCount(t.mipStart, t.mipSize));
            std::map<std::string, uint32_t> counts;
            for (uint32_t i = 0; i < n + mipN; i++)
            {
                writewatch::LastWrite w = writewatch::LastWriter(pages[i]);
                // (NFSMW_WATCH_SUBPAGES: compared, a store its host page's
                // snapshot found there; unverified, marked unseen as the
                // snapshot closed without a compare, at the fault that opened it.)
                std::string kind = w.kind == writewatch::Writer::kCpuFault ? (inTexture(w.tag) ? "cpu-fault" : "cpu-fault-nbr")
                    : w.kind == writewatch::Writer::kResolve ? "resolve" : w.kind == writewatch::Writer::kHostWrite ? "host-write"
                    : w.kind == writewatch::Writer::kGpuStore ? "gpu-store" : w.kind == writewatch::Writer::kRange ? "range"
                    : w.kind == writewatch::Writer::kCompared ? "compared" : w.kind == writewatch::Writer::kUnverified ? "unverified" : "none";
                counts[kind]++;
                kinds.insert(kind);
                if (i < 4)
                    text += std::format(" [{:08X} {}{} -{}f]", pages[i], kind,
                        w.kind == writewatch::Writer::kResolve ? std::format(" dest {:08X} {}", w.tag, ResolveNote(w.tag))
                        : w.kind != writewatch::Writer::kNone ? std::format(" at {:08X}{}", w.tag, w.pc ? std::format(" pc +{:X}", w.pc) : "")
                        : std::string(), (uint32_t(s_frame) - w.frame) & 0x0FFFFFFF);
            }
            text += " |";
            for (auto& [kind, count] : counts)
                text += std::format(" {} {}", kind, count);
            return text;
        }

        // Before (re)loading `t` (its loadedSequence is still the last load's).
        void LogUpload(const Texture& t, uint32_t format, bool tiled)
        {
            UploadLog& log = *s_uploadLog;
            const char* path = t.scaled ? "scaled" : t.storeView ? "direct" : "staging";
            std::set<std::string> writers;
            std::string line = std::format("[upload] frame {} base {:08X} size {} mip {:08X}/{} {}x{} {}/{} levels {} fmt {}/vk{} tiled {} {} path {} {} reason {}",
                s_frame, t.guestStart, t.guestSize, t.mipStart, t.mipSize, t.width, t.height, t.depth, t.layers, t.levels, format,
                int(t.format), int(tiled), t.cube ? "cube" : t.volume ? "volume" : "2d", path, s_passActive ? "ends-pass" : "between-passes",
                t.uploaded ? "written " + DescribeWrites(t, writers) : "first");
            if (!t.uploaded)
                writers.insert("first");
            auto it = log.resolves.find(t.guestStart);
            line += it == log.resolves.end() ? std::string(" | resolve-dest no")
                : std::format(" | resolve-dest -{}f {}", s_frame - it->second.frame, ResolveNote(t.guestStart));
            fprintf(stderr, "%s\n", line.c_str());
            std::string key;
            for (const std::string& w : writers)
                key += (key.empty() ? "" : "+") + w;
            log.byWriters[key]++;
            log.byPath[path]++;
            log.uploads++;
            log.endedPasses += s_passActive;
            log.bytes += t.guestSize + t.mipSize;
        }

        // NFSMW_TEXTURE_VERIFY=1 (debug; as NFSMW_CP_CACHE_VERIFY): that the
        // write watch sees every change to a texture's memory, which
        // NFSMW_WATCH_SUBPAGES partly finds by comparing instead of faulting.
        // A 1x texture's guest pages are hashed at its load (after Watch
        // protects them), and again at its first use each frame that finds
        // it still valid; a change with no write recorded since the load is
        // logged ([texverify], the first 100). Not checked: scaled textures,
        // pages a GPU write leaves unreadable (resolve destinations), and a
        // texture a resolve stored into (the GPU changes its memory later).
        const bool s_textureVerify = [] { const char* v = std::getenv("NFSMW_TEXTURE_VERIFY"); return v && v[0] == '1'; }();
        uint64_t s_textureMisses = 0;

        // A hash of each 4 KB guest page of `t`'s base, then its mips (the
        // part in the range), as they are now.
        void HashTexturePages(const Texture& t, std::vector<uint64_t>& hashes)
        {
            hashes.clear();
            for (auto [start, size] : { std::pair{ t.guestStart, t.guestSize }, std::pair{ t.mipStart, t.mipSize } })
                for (uint64_t page = start & ~0xFFFull; page < uint64_t(start) + size; page += 0x1000)
                {
                    uint64_t from = std::max<uint64_t>(page, start), to = std::min<uint64_t>(page + 0x1000, uint64_t(start) + size);
                    const uint8_t* p = s_sharedHost + from;
                    size_t n = size_t(to - from), i = 0;
                    // Four lanes: one multiply chain would be latency-bound.
                    uint64_t h[4] = { n, 1, 2, 3 };
                    for (; i + 32 <= n; i += 32)
                        for (int lane = 0; lane < 4; lane++)
                        {
                            uint64_t w;
                            memcpy(&w, p + i + lane * 8, 8);
                            h[lane] = (h[lane] ^ w) * 0x9E3779B97F4A7C15ull;
                            h[lane] ^= h[lane] >> 32;
                        }
                    for (; i < n; i++)
                        h[0] = (h[0] ^ p[i]) * 0x100000001B3ull;
                    hashes.push_back(h[0] ^ std::rotl(h[1], 16) ^ std::rotl(h[2], 32) ^ std::rotl(h[3], 48));
                }
        }

        // NFSMW_TEXTURE_VERIFY: after a load's Watch, what its pages hold (a
        // store from now on faults, or is compared).
        void HashLoadedTexture(Texture& t)
        {
            // Only the GPU guard (NFSMW_EAGER_WRITES, the default, or shadow
            // mode) leaves a resolve's destination unreadable until the CPU
            // next touches it: without it, one would be hashed before the
            // resolve lands, and each later check would report it.
            bool guarded = g_eagerWrites || s_shadowMode;
            LogOnce(0x2900, "%s", guarded ? "NFSMW_TEXTURE_VERIFY: texture pages hashed at each load, checked at the first use each frame"
                : "NFSMW_TEXTURE_VERIFY needs the GPU guard (NFSMW_EAGER_WRITES=0 turns it off): nothing checked");
            t.verifyFrame = s_frame;
            if (!guarded || t.scaled || !writewatch::CpuReadable(t.guestStart, t.guestSize) || !writewatch::CpuReadable(t.mipStart, t.mipSize))
                t.verifyHashes.clear();
            else
                HashTexturePages(t, t.verifyHashes);
        }

        // NFSMW_TEXTURE_VERIFY: a use found `t` valid, deciding at WriteEpoch()
        // `epoch`. Its pages must hold what they did at its load.
        void VerifyTexture(Texture& t, uint64_t epoch)
        {
            if (t.verifyHashes.empty() || t.verifyFrame == s_frame)
                return;
            t.verifyFrame = s_frame;
            if (!writewatch::CpuReadable(t.guestStart, t.guestSize) || !writewatch::CpuReadable(t.mipStart, t.mipSize))
                return;
            std::vector<uint64_t> now;
            HashTexturePages(t, now);
            if (now == t.verifyHashes)
                return;
            // A write recorded since that use decided (another thread, just
            // now) moved the epoch: the next use loads it again.
            if (writewatch::WriteEpoch() != epoch && (writewatch::WrittenSince(t.guestStart, t.guestSize, t.loadedSequence) ||
                    (t.mipSize && writewatch::WrittenSince(t.mipStart, t.mipSize, t.loadedSequence))))
                return;
            if (s_textureMisses++ < 100)
            {
                // The changed pages' addresses, in HashTexturePages' order.
                uint32_t basePages = t.guestSize ? ((t.guestStart + t.guestSize - 1) >> 12) - (t.guestStart >> 12) + 1 : 0;
                std::string pages;
                uint32_t changed = 0;
                for (size_t i = 0; i < now.size(); i++)
                    if (now[i] != t.verifyHashes[i] && changed++ < 4)
                        pages += std::format(" {:08X}", i < basePages ? ((t.guestStart >> 12) + uint32_t(i)) << 12
                            : ((t.mipStart >> 12) + uint32_t(i - basePages)) << 12);
                fprintf(stderr, "[texverify] frame %llu texture %08X+%u mips %08X+%u %ux%u%s vk%d: %u of %zu pages changed since its load "
                    "with no write recorded:%s%s\n", (unsigned long long)s_frame, t.guestStart, t.guestSize, t.mipStart, t.mipSize, t.width,
                    t.height, t.cube ? " cube" : t.volume ? " volume" : "", int(t.format), changed, now.size(), pages.c_str(),
                    changed > 4 ? " ..." : "");
            }
            t.verifyHashes = std::move(now);  // (each change reported once)
        }

        std::unordered_map<uint64_t, VkSampler> s_samplers;
        bool s_loadMips = true;  // Settings > Video > Mipmaps, as applied (ApplyLiveSettings)

        // Anisotropic filtering on linearly filtered textures: 16x on a
        // discrete GPU or an M-series iPad, 4x on other integrated ones (16x
        // cost the M1 Pro ~4 ms a frame in races at 2x2). Settings > Video (NFSMW_ANISOTROPY=<1..16>)
        // changes it live; "game" uses the game's own setting.
        float s_maxAnisotropy = 1.0f;  // device limit (1: no anisotropic filtering)
        int s_autoAnisotropy = 16;      // "Auto": set in CreateSharedMemory

        // `maxLevel`: the texture's last mip level (0: base only).
        VkSampler GetSampler(const xenos::xe_gpu_texture_fetch_t& f, uint32_t maxLevel)
        {
            // The setting is part of the key: a change makes new samplers,
            // while command buffers in flight keep theirs (never destroyed).
            int anisotropy = settings::GetInt(settings::Id::Anisotropy);
            if (anisotropy == settings::kAnisotropyAuto)
                anisotropy = s_autoAnisotropy;
            uint64_t key = uint64_t(f.clamp_x) | (uint64_t(f.clamp_y) << 3) | (uint64_t(f.clamp_z) << 6) |
                (uint64_t(f.mag_filter) << 9) | (uint64_t(f.min_filter) << 11) | (uint64_t(f.border_color) << 13) |
                (uint64_t(f.mip_filter) << 15) | (uint64_t(f.aniso_filter) << 17) | (uint64_t(f.mip_min_level) << 20) |
                (uint64_t(maxLevel) << 24) | (uint64_t(uint8_t(anisotropy)) << 32);
            auto& slot = s_samplers[key];
            if (slot)
                return slot;
            // The mirror-once modes as Xenia maps them (vulkan_texture_cache.cc,
            // NormalizeClampMode): Vulkan mirrors once only to the edge, so
            // the halfway and border variants take that too, where the
            // device has it (VK_KHR_sampler_mirror_clamp_to_edge, enabled by
            // the presenter). Without it they clamp unmirrored, to the edge
            // or the border, as before. Clamp to halfway (half edge texel,
            // half border; nothing in Direct3D 9 makes it) is clamp to edge,
            // as in Xenia.
            auto address = [](xenos::ClampMode m) {
                const bool mirrorOnce = s_vk && s_vk->mirrorClampToEdge;
                switch (m)
                {
                case xenos::ClampMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case xenos::ClampMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case xenos::ClampMode::kClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                case xenos::ClampMode::kMirrorClampToEdge:
                case xenos::ClampMode::kMirrorClampToHalfway:
                    return mirrorOnce ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                case xenos::ClampMode::kMirrorClampToBorder:
                    return mirrorOnce ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                }
            };
            VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = f.mag_filter == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            sci.minFilter = f.min_filter == xenos::TextureFilter::kPoint ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            sci.addressModeU = address(f.clamp_x);
            sci.addressModeV = address(f.clamp_y);
            sci.addressModeW = address(f.clamp_z);
            // Mips as the fetch constant says: point or linear between
            // levels, or the base level only; its level range and bias.
            sci.mipmapMode = f.mip_filter == xenos::TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            bool baseOnly = maxLevel == 0 || f.mip_filter == xenos::TextureFilter::kBaseMap;
            sci.minLod = baseOnly ? 0.0f : float(std::min<uint32_t>(f.mip_min_level, maxLevel));
            sci.maxLod = baseOnly ? 0.0f : float(maxLevel);
            // (The fetch constant's LOD bias is applied in the shader, as
            // Xenia does: MoltenVK has no sampler LOD bias.)
            // Anisotropy (the game's: 1, 2, 4, 8 or 16:1, unless overridden)
            // for linear minification.
            static const float gameAniso[8] = { 1, 1, 2, 4, 8, 16, 1, 1 };
            float aniso = anisotropy > 0 ? float(anisotropy) : gameAniso[uint32_t(f.aniso_filter) & 7];
            aniso = std::min(aniso, s_maxAnisotropy);
            if (aniso > 1.0f && sci.minFilter == VK_FILTER_LINEAR)
            {
                sci.anisotropyEnable = VK_TRUE;
                sci.maxAnisotropy = aniso;
            }
            sci.borderColor = f.border_color == xenos::BorderColor::k_ABGR_White
                ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            Check(vkCreateSampler(s_dev, &sci, nullptr, &slot), "vkCreateSampler");
            return slot;
        }

        // NFSMW_RESOLVE_RENAME: views of renamed images, by image and swizzle.
        // Kept for good: the images are the depth targets' spares, a few,
        // and command buffers in flight may still sample through any.
        std::map<std::pair<VkImage, uint32_t>, VkImageView> s_renameViews;
        uint64_t s_renames = 0, s_renameDrops = 0, s_renameMaterialized = 0;  // (counts, the [hle] line at exit)
        void ForgetTextureLookups(const Texture* t);  // the cached lookups that hold `t` decided again (its views changed)

        VkImageView TextureView(Texture& t, uint32_t swizzle)
        {
            if (t.renamedImage)
            {
                auto& view = s_renameViews[{ t.renamedImage, swizzle }];
                if (!view)
                {
                    auto comp = [](uint32_t s) {
                        static const VkComponentSwizzle c[8] = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                            VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY };
                        return c[s & 7];
                    };
                    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                    vci.image = t.renamedImage;
                    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    vci.format = t.renamedFormat;
                    vci.components = { comp(swizzle), comp(swizzle >> 3), comp(swizzle >> 6), comp(swizzle >> 9) };
                    vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                    Check(vkCreateImageView(s_dev, &vci, nullptr, &view), "vkCreateImageView(renamed texture)");
                }
                return view;
            }
            auto& view = t.views[swizzle];
            if (view)
                return view;
            auto comp = [](uint32_t s) {
                switch (s & 7)
                {
                case 0: return VK_COMPONENT_SWIZZLE_R;
                case 1: return VK_COMPONENT_SWIZZLE_G;
                case 2: return VK_COMPONENT_SWIZZLE_B;
                case 3: return VK_COMPONENT_SWIZZLE_A;
                case 4: return VK_COMPONENT_SWIZZLE_ZERO;
                case 5: return VK_COMPONENT_SWIZZLE_ONE;
                default: return VK_COMPONENT_SWIZZLE_IDENTITY;
                }
            };
            // Sampled only (a storable scaled texture's format need not support storage).
            VkImageViewUsageCreateInfo usage{ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, VK_IMAGE_USAGE_SAMPLED_BIT };
            VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vci.pNext = t.storeView ? &usage : nullptr;
            vci.image = t.image.image;
            vci.viewType = t.cube ? VK_IMAGE_VIEW_TYPE_CUBE : t.volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
            vci.format = t.format;
            vci.components = { comp(swizzle), comp(swizzle >> 3), comp(swizzle >> 6), comp(swizzle >> 9) };
            vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, t.cube ? 6u : 1u };
            Check(vkCreateImageView(s_dev, &vci, nullptr, &view), "vkCreateImageView(texture)");
            return view;
        }

        // The view for texture fetch constant `slot` sampled as 2D or as a
        // cube, or the placeholder. Base level only; each cube face is a
        // tiled 2D slice, 4 KB-aligned (Xenia's GetGuestTextureLayout).
        enum class TexDim { k2D, k3D, kCube };

        // What a lookup's result depends on beyond the fetch constant (for
        // the lookup cache below). `texture`: null when it isn't kept (a
        // placeholder, a scaled texture).
        struct TextureLookup
        {
            Texture* texture = nullptr;
            uint32_t base = 0, size = 0, mipBase = 0, mipSize = 0;
            bool scaledCandidate = false;  // a draw into scaled targets may take the scaled path
            uint64_t sig = 0;              // ScaledSignature
        };

        // The full lookup (GetTexture below keeps its results).
        VkImageView LookupTexture(uint32_t slot, TexDim dim, VkSampler& sampler, TextureLookup* out)
        {
            const bool cube = dim == TexDim::kCube, volume = dim == TexDim::k3D;
            xenos::xe_gpu_texture_fetch_t f;
            memcpy(&f, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6], sizeof(f));
            sampler = s_sampler;
            const VkImageView dummy = s_dummy[cube ? 2 : volume ? 1 : 0].view;
            if (f.type != xenos::FetchConstantType::kTexture)
                return dummy;
            bool dimensionOk = cube ? f.dimension == xenos::DataDimension::kCube
                : volume ? f.dimension == xenos::DataDimension::k3D
                : (f.dimension == xenos::DataDimension::k2DOrStacked || f.dimension == xenos::DataDimension::k1D);
            if (!dimensionOk)
            {
                LogOnce(0x2000 + uint32_t(f.dimension) + uint32_t(dim) * 8, "texture dimension %u sampled as dimension %u",
                    uint32_t(f.dimension), uint32_t(dim));
                return dummy;
            }
            uint32_t layers = cube ? 6 : 1;
            TextureFormatInfo fi;
            // (An alias keys and loads as its base format: one texture for
            // both at an address.)
            uint32_t format = texformat::SampledAs(uint32_t(f.format));
            // The view's swizzle: the fetch constant's, reading the
            // components the host format lacks as the texture unit does.
            const uint32_t swizzle = texformat::Compose(f.swizzle, texformat::HostSwizzle(format));
            if (!GetTextureFormat(format, fi))
            {
                LogOnce(0x3000 + format, "texture format %u not implemented", format);
                return dummy;
            }
            if (f.base_address == 0)
            {
                LogOnce(0x2100, "texture without a base level (mips only)");
                return dummy;
            }
            uint32_t width = (f.dimension == xenos::DataDimension::k1D ? f.size_1d.width
                : volume ? f.size_3d.width : f.size_2d.width) + 1;
            uint32_t height = f.dimension == xenos::DataDimension::k1D ? 1 : (volume ? f.size_3d.height : f.size_2d.height) + 1;
            uint32_t depth = volume ? f.size_3d.depth + 1 : 1;

            // Guest layout (Xenia's texture_util).
            uint32_t bw = (width + fi.block - 1) / fi.block, bh = (height + fi.block - 1) / fi.block;
            uint32_t offX = 0, offY = 0;
            uint32_t log2w = Log2Ceil(width), log2h = Log2Ceil(height);
            if (f.packed_mips && std::min(log2w, log2h) <= 4)
            {
                // The base level itself sits in the packed-mip tile.
                if (log2w > log2h)
                    offY = 16 / fi.block;
                else
                    offX = 16 / fi.block;
            }
            uint32_t pitchTexels = f.pitch ? f.pitch << 5 : width;
            uint32_t pitchBlocks = (pitchTexels + fi.block - 1) / fi.block;
            uint32_t base = (f.base_address << 12) & 0x1FFFFFFF;
            if (base == s_logTextureUse)
            {
                s_drawUsesLoggedTexture = true;
                s_loggedTextureFetch = std::format("slot {} dim {} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", slot, int(dim),
                    s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6], s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 1],
                    s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 2], s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 3],
                    s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 4], s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6 + 5]);
            }
            uint32_t pitchAligned = 0, rowBytes = 0, size;
            if (f.tiled)
            {
                pitchAligned = (std::max(pitchBlocks, bw + offX) + 31) & ~31u;
                uint32_t heightAligned = (bh + offY + 31) & ~31u;
                size = (pitchAligned * heightAligned) << fi.bpbLog2;
            }
            else
            {
                rowBytes = ((pitchBlocks << fi.bpbLog2) + 255) & ~255u;
                size = rowBytes * (bh + offY);
            }
            // Faces after the first follow at the 4 KB-aligned slice stride.
            uint32_t sliceStride = f.tiled ? (((pitchAligned * ((bh + offY + 31) & ~31u)) << fi.bpbLog2) + 4095) & ~4095u
                                           : ((rowBytes * ((bh + offY + 31) & ~31u)) + 4095) & ~4095u;
            size += sliceStride * (layers - 1);
            uint32_t heightAligned3D = (bh + 31) & ~31u;
            if (volume)
            {
                // Tiled: one Tiled3D volume (depth in groups of 4); linear:
                // slices at the 2D slice stride.
                size = f.tiled ? (pitchAligned * heightAligned3D * ((depth + 3) & ~3u)) << fi.bpbLog2
                               : sliceStride * depth;
            }
            size = std::min(size, uint32_t(kSharedSize) - base);

            // Mip levels (2D and cube): up to mip_max_level, when the texture
            // has a mip address. Settings > Video > Mipmaps off
            // (NFSMW_TEXTURE_MIPS=0) loads the base only; a change drops the
            // cache (ApplyLiveSettings), so every texture is loaded again.
            const bool loadMips = s_loadMips;
            uint32_t maxLevel = 0, mipBase = 0, mipSize = 0;
            MipLayout mips{ width, height, fi.block, fi.bpbLog2, layers, f.tiled != 0, f.packed_mips != 0, 0 };
            if (loadMips && !volume && !f.stacked && f.dimension != xenos::DataDimension::k1D && f.mip_address != 0)
            {
                uint32_t sizeMaxLevel = 31 - uint32_t(__builtin_clz(std::max(width, height)));
                maxLevel = std::min<uint32_t>(f.mip_max_level, sizeMaxLevel);
                if (maxLevel > 0)
                {
                    uint32_t log2Short = Log2Ceil(std::min(width, height));
                    mips.packedLevel = log2Short > 4 ? log2Short - 4 : 0;
                    mipBase = (f.mip_address << 12) & 0x1FFFFFFF;
                    uint32_t lastStorage = mips.StorageLevel(maxLevel);
                    mipSize = mips.Offset(lastStorage) + mips.SliceStride(lastStorage) * layers;
                    mipSize = std::min(mipSize, uint32_t(kSharedSize) - mipBase);
                }
            }
            // Drawn into scaled targets: a texture that scaled resolves wrote
            // (and nothing since) is sampled at host resolution, base level
            // only (resolved surfaces have no mips).
            // (Mipmapped textures keep their levels on the 1x path.)
            // At 1.5x only a texture of whole 2x2 blocks (even width and
            // height; gpu/render_scale.h), below 1x of whole 2x2, 4x4 or
            // 8x8 ones (the scene's 1280x720 and 640x720 are).
            bool scaledCandidate = !cube && !volume && f.tiled && layers == 1 && !f.stacked &&
                ScaledFormat(fi) && offX == 0 && offY == 0 && maxLevel == 0 && s_scaled.scale.Exact(width, height) &&
                s_scaled.scale.HostPixels(width, height) * 4 <= s_scaled.scratchSize;
            // Which scaled texture this is (its cache key's surface fields):
            // pages only a scaled texture holds are current for it alone.
            const uint64_t sig = ScaledSignature(format, width, height, pitchAligned, uint32_t(f.endianness));
            bool scaled = s_drawScaled && s_scaled.enabled && scaledCandidate && ScaledTextureValid(base, size, sig);
            sampler = GetSampler(f, maxLevel);
            if (out)
                *out = { nullptr, base, size, mipBase, mipSize, scaledCandidate, sig };

            uint64_t key = scaled ? 0x5CA1ED5CA1EDull : 0xCBF29CE484222325ull;
            for (uint64_t v : { uint64_t(base), uint64_t(format), uint64_t(width), uint64_t(height), uint64_t(f.tiled),
                     uint64_t(pitchTexels), uint64_t(f.endianness), uint64_t(f.packed_mips), uint64_t(layers), uint64_t(depth),
                     uint64_t(mipBase), uint64_t(maxLevel) })
                key = (key ^ v) * 0x100000001B3ull;
            auto& slotRef = s_textures[key];
            if (!slotRef)
            {
                slotRef = std::make_unique<Texture>();
                Texture& t = *slotRef;
                t.lastUsed = s_frame;  // (before its image: a failed allocation evicts only older ones)
                t.format = fi.vk;
                t.width = width;
                t.height = height;
                t.guestStart = base;
                t.guestSize = size;
                t.cube = cube;
                t.volume = volume;
                t.layers = layers;
                t.depth = depth;
                t.levels = maxLevel + 1;
                t.mipStart = mipBase;
                t.mipSize = mipSize;
                t.scaled = scaled;
                if (scaled)
                {
                    t.width = s_scaled.scale.x.Size(width);
                    t.height = s_scaled.scale.y.Size(height);
                    t.guestWidth = width;
                    t.guestHeight = height;
                    t.scaledSig = sig;
                    s_scaled.textures++;
                }
                // A 1x texture the untile can store directly (see
                // s_untileStorePipeline): 2D, or a cube (NFSMW_DIRECT_CUBE).
                bool direct1x = !scaled && s_untileDirect && !volume && maxLevel == 0 && fi.block == 1 &&
                    LoadedBpbLog2(fi) == 2 && (cube ? s_directCube && layers == 6 && !f.stacked : layers == 1) &&
                    DirectFormat(fi.vk, cube);
                if ((scaled && s_scaled.direct) || direct1x)
                {
                    // The untile stores its words through an R32_UINT view
                    // (every scaled format is 32 bits a texel, as is each
                    // direct 1x one), a 2D one of each layer (a cube's faces).
                    VkFormat formats[2] = { fi.vk, VK_FORMAT_R32_UINT };
                    VkImageFormatListCreateInfo list{ VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr, 2, formats };
                    VkImageViewUsageCreateInfo sampled{ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr, VK_IMAGE_USAGE_SAMPLED_BIT };
                    // A cube's image and views are untried on some drivers
                    // (MoltenVK: 2D R32_UINT views of a cube's layers): one
                    // turned down is staged instead, below.
                    t.image = CreateImage(VK_IMAGE_TYPE_2D, fi.vk, { t.width, t.height, 1 }, layers,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | (direct1x ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0),
                        cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT | (cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
                        1, &list, &sampled, cube);
                    VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                    vci.image = t.image.image;
                    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    vci.format = VK_FORMAT_R32_UINT;
                    bool made = t.image.image != VK_NULL_HANDLE;
                    for (uint32_t layer = 0; layer < layers && made; layer++)
                    {
                        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1 };
                        VkResult result = vkCreateImageView(s_dev, &vci, nullptr, &t.storeViews[layer]);
                        if (!cube)
                            Check(result, "vkCreateImageView(scaled store)");
                        if (result != VK_SUCCESS)
                        {
                            t.storeViews[layer] = VK_NULL_HANDLE;
                            made = false;
                        }
                    }
                    t.storeView = t.storeViews[0];
                    if (!made)
                    {
                        fprintf(stderr, "[renderer] cube %ux%u format %d can't be stored directly (image or view refused): staged\n",
                            width, height, int(fi.vk));
                        for (VkImageView& view : t.storeViews)
                        {
                            if (view)
                                vkDestroyImageView(s_dev, view, nullptr);
                            view = VK_NULL_HANDLE;
                        }
                        t.storeView = VK_NULL_HANDLE;
                        if (t.image.image)
                            DestroyImage(t.image);
                        s_directCubeFormats[fi.vk] = false;  // (once a format: DirectFormat says no from now on)
                    }
                    else if (direct1x)
                    {
                        t.tiled = f.tiled != 0;
                        t.pitch = f.tiled ? pitchAligned : rowBytes;
                        t.bpbLog2 = fi.bpbLog2;
                        t.convert = fi.convert;
                        t.endian = uint32_t(f.endianness);
                        t.offX = offX;
                        t.offY = offY;
                        t.sliceStride = sliceStride;
                        // (A resolve into a cube takes the face rule, see Resolve.)
                        if (!cube)
                            s_directByBase[base] = &t;
                        if (s_fusePartial || cube)
                        {
                            s_directByStart[base] = &t;
                            s_maxDirectSize = std::max(s_maxDirectSize, t.guestSize);
                        }
                    }
                    else
                    {
                        // A scaled texture a scaled resolve can store into
                        // as its untile would (Resolve; NFSMW_SCALED_FUSE).
                        t.tiled = true;
                        t.pitch = pitchAligned;
                        t.bpbLog2 = 2;
                        t.endian = uint32_t(f.endianness);
                        s_scaledByStart[base] = &t;
                        s_maxScaledSize = std::max(s_maxScaledSize, t.guestSize);
                    }
                }
                if (!t.image.image)
                    t.image = CreateImage(volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, fi.vk, { t.width, t.height, depth }, layers,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        cube ? VK_IMAGE_VIEW_TYPE_CUBE : volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT,
                        cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, t.levels);
            }
            Texture& t = *slotRef;
            t.lastUsed = s_frame;
            {
                // No page written anywhere since the last check: still valid
                // (a draw binds ~7,500 textures a frame; writes are rare).
                uint64_t epoch = writewatch::WriteEpoch();
                bool scan = !(g_cpOpt & CP_OPT_TEXTURE_EPOCH) || epoch != t.checkedEpoch;
                t.checkedEpoch = epoch;
                if (!t.uploaded || (scan && (writewatch::WrittenSince(base, size, t.loadedSequence) ||
                        (mipSize && writewatch::WrittenSince(mipBase, mipSize, t.loadedSequence)))))
                {
                    if (s_uploadLog && s_uploadLog->now)
                        LogUpload(t, format, f.tiled != 0);
                    // Snapshot before recording: a store after this faults and
                    // bumps the sequence; the GPU load itself runs later and
                    // sees anything written before. (NFSMW_WATCH_SUBPAGES: the
                    // snapshots open on its pages are compared first, else the
                    // compare Watch makes below would mark its own pages newer
                    // than this load, and it would load again.)
                    writewatch::Settle(base, size);
                    writewatch::Settle(mipBase, mipSize);
                    t.loadedSequence = writewatch::Current();
                    if (t.renamedImage)  // NFSMW_RESOLVE_RENAME: this load writes its own image whole
                    {
                        t.renamedImage = VK_NULL_HANDLE;
                        s_renameDrops++;
                        ForgetTextureLookups(&t);
                    }
                    HitchTimer hitchTimer(s_hitch.textureMs);
                    s_hitch.textures++;
                    if (t.scaled)
                    {
                        s_hitch.scaledTextures++;
                        LoadScaledTexture(t, base, pitchAligned, uint32_t(f.endianness));
                        t.uploaded = true;
                        writewatch::Watch(base, size);  // as the 1x path: a CPU write faults and retires it
                        return TextureView(t, swizzle);
                    }
                    s_hitch.textureBytes += size + mipSize;
                    EnsureShadow(base, size);
                    if (mipSize)
                        EnsureShadow(mipBase, mipSize);
                    uint32_t outBpbLog2 = LoadedBpbLog2(fi);
                    uint32_t rowWords, rowTexels;
                    if (fi.convert != 0 || fi.bpbLog2 >= 2)
                    {
                        rowWords = (bw << outBpbLog2) / 4;
                        rowTexels = bw * fi.block;
                    }
                    else
                    {
                        uint32_t perWord = 4 >> fi.bpbLog2;
                        rowWords = (bw + perWord - 1) / perWord;
                        rowTexels = rowWords * perWord;
                    }
                    for (uint32_t slice = 0; slice < layers * depth; slice++)
                    {
                        if (t.storeView)  // stored directly (one 2D slice, or a cube's face: see s_untileStorePipeline)
                        {
                            EndPass(kBreakUpload);
                            Begin();
                            UntileConstants uc{ base + slice * sliceStride, f.tiled ? 1u : 0u, f.tiled ? pitchAligned : rowBytes, fi.bpbLog2,
                                offX, offY, bw, bh, uint32_t(f.endianness), fi.convert, 0, 0, 0, 0 };
                            VkDescriptorImageInfo ii{ VK_NULL_HANDLE, t.storeViews[slice], VK_IMAGE_LAYOUT_GENERAL };
                            VkWriteDescriptorSet w[5] = {
                                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
                                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr },
                            };
                            if (!t.uploaded && slice == 0)
                                ToGeneral(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, layers);
                            ComputeWorkRecorded();
                            if (s_profiling)
                                ProfileCompute(std::format("texture {}x{} fmt {} at {:08X}, stored directly", width, height, format, base).c_str());
                            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untileStorePipeline);
                            s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untileStoreLayout, 0, AddSharedParts(w, 2), w);
                            vkCmdPushConstants(s_cmd, s_untileStoreLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uc), &uc);
                            if (!(s_exp & 4))
                                vkCmdDispatch(s_cmd, (bw + 7) / 8, (bh + 7) / 8, 1);
                            FullBarrier();
                            ProfileComputeEnd();
                            SubmitIfStarving();
                            continue;
                        }
                        VkDeviceSize staging = RingAllocate(VkDeviceSize(rowWords) * 4 * bh);
                        if (staging == ~VkDeviceSize(0))
                        {
                            LogOnce(0x2200, "texture %ux%u format %u is too large for a staging slot", width, height, format);
                            return dummy;
                        }
                        EndPass(kBreakUpload);
                        Begin();
                        uint32_t layer = volume ? 0 : slice, zSlice = volume ? slice : 0;
                        uint32_t sliceBase = base + (volume ? (f.tiled ? 0 : zSlice * sliceStride) : layer * sliceStride);
                        UntileConstants uc{ sliceBase, f.tiled ? 1u : 0u, f.tiled ? pitchAligned : rowBytes, fi.bpbLog2,
                            offX, offY, bw, bh, uint32_t(f.endianness), fi.convert, uint32_t(staging / 4), rowWords,
                            zSlice, heightAligned3D };
                        if (volume && f.tiled)
                            uc.tiled = 2;
                        VkDescriptorBufferInfo ring{ s_ring, 0, kRingSize };
                        VkWriteDescriptorSet w[5] = {
                            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
                            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ring, nullptr },
                        };
                        ComputeWorkRecorded();
                        if (s_profiling)
                            ProfileCompute(std::format("texture {}x{} fmt {} at {:08X}, staged", width, height, format, base).c_str());
                        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untilePipeline);
                        s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untileLayout, 0, AddSharedParts(w, 2), w);
                        vkCmdPushConstants(s_cmd, s_untileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uc), &uc);
                        uint32_t groupsX = fi.convert != 0 || fi.bpbLog2 >= 2 ? bw : rowWords;
                        if (!(s_exp & 4))
                            vkCmdDispatch(s_cmd, (groupsX + 7) / 8, (bh + 7) / 8, 1);
                        FullBarrier();
                        if (!t.uploaded && slice == 0)
                            ToGeneral(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, layers);
                        VkBufferImageCopy region{};
                        region.bufferOffset = staging;
                        region.bufferRowLength = rowTexels;
                        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
                        region.imageOffset = { 0, 0, int32_t(zSlice) };
                        region.imageExtent = { width, height, 1 };
                        if (!(s_exp & 16))  // NFSMW_GPU_EXP bit 16: no staging copy (upload cost experiment)
                            vkCmdCopyBufferToImage(s_cmd, s_ring, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                        FullBarrier();
                        ProfileComputeEnd();
                        SubmitIfStarving();
                    }
                    // Mip levels 1..maxLevel, each face from its storage.
                    for (uint32_t level = 1; level <= maxLevel; level++)
                    {
                        uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
                        uint32_t lbw = (lw + fi.block - 1) / fi.block, lbh = (lh + fi.block - 1) / fi.block;
                        uint32_t storage = mips.StorageLevel(level);
                        uint32_t levelBase = mipBase + mips.Offset(storage);
                        uint32_t levelOffX, levelOffY;
                        mips.PackedOffset(level, levelOffX, levelOffY);
                        uint32_t levelRowWords, levelRowTexels;
                        if (fi.convert != 0 || fi.bpbLog2 >= 2)
                        {
                            levelRowWords = (lbw << outBpbLog2) / 4;
                            levelRowTexels = lbw * fi.block;
                        }
                        else
                        {
                            uint32_t perWord = 4 >> fi.bpbLog2;
                            levelRowWords = (lbw + perWord - 1) / perWord;
                            levelRowTexels = levelRowWords * perWord;
                        }
                        for (uint32_t layer = 0; layer < layers; layer++)
                        {
                            VkDeviceSize staging = RingAllocate(VkDeviceSize(levelRowWords) * 4 * lbh);
                            if (staging == ~VkDeviceSize(0))
                                break;
                            EndPass(kBreakUpload);
                            Begin();
                            UntileConstants uc{ levelBase + layer * mips.SliceStride(storage), f.tiled ? 1u : 0u,
                                f.tiled ? mips.PitchBlocks(storage) : mips.RowBytes(storage), fi.bpbLog2,
                                levelOffX, levelOffY, lbw, lbh, uint32_t(f.endianness), fi.convert, uint32_t(staging / 4), levelRowWords,
                                0, 0 };
                            VkDescriptorBufferInfo ring{ s_ring, 0, kRingSize };
                            VkWriteDescriptorSet w[5] = {
                                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
                                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ring, nullptr },
                            };
                            ComputeWorkRecorded();
                            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untilePipeline);
                            s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_untileLayout, 0, AddSharedParts(w, 2), w);
                            vkCmdPushConstants(s_cmd, s_untileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uc), &uc);
                            uint32_t groupsX = fi.convert != 0 || fi.bpbLog2 >= 2 ? lbw : levelRowWords;
                            vkCmdDispatch(s_cmd, (groupsX + 7) / 8, (lbh + 7) / 8, 1);
                            FullBarrier();
                            VkBufferImageCopy region{};
                            region.bufferOffset = staging;
                            region.bufferRowLength = levelRowTexels;
                            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1 };
                            region.imageExtent = { lw, lh, 1 };
                            if (!(s_exp & 16))  // NFSMW_GPU_EXP bit 16: no staging copy (upload cost experiment)
                                vkCmdCopyBufferToImage(s_cmd, s_ring, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                            FullBarrier();
                        }
                    }
                    t.uploaded = true;
                    writewatch::Watch(base, size);
                    GuardGpu(base, size, false, "texture", true);  // the untile runs later
                    if (mipSize)
                    {
                        writewatch::Watch(mipBase, mipSize);
                        GuardGpu(mipBase, mipSize, false, "texture mips", true);
                    }
                    if (s_textureVerify)
                        HashLoadedTexture(t);
                    s_stats.uploads++;
                }
                else if (s_textureVerify)
                {
                    VerifyTexture(t, epoch);
                }
            }
            if (out && !t.scaled)
                out->texture = &t;
            return TextureView(t, swizzle);
        }

        // Texture lookups kept per fetch slot and dimension. Dense scenes
        // look up 5,000-10,000 textures a frame, nearly all with a fetch
        // constant seen very recently, and the full lookup (layout, three
        // hash maps, the sampler) was ~16% of the command processor (Deck
        // profile). An entry is a lookup's result, used again while the
        // fetch constant's 6 dwords are the same and nothing else it depends
        // on moved: the texture cache (DropTextures), any setting
        // (anisotropy is in the sampler) and the scaled resolve memory bump
        // `generation`; for a texture the scaled path could take, whether
        // the draw renders into scaled targets is part of the key. Validity
        // is the full lookup's: unchanged while no page anywhere was written
        // (WriteEpoch), else checked in place (the texture's write
        // sequences, and whether a scaled copy became current) instead of
        // repeating the whole lookup. Each slot keeps its last lookup; a
        // table by the fetch constant's hash keeps textures that alternate
        // in a slot (about a sixth of the lookups in the career intro).
        // Placeholders and scaled textures aren't kept. NFSMW_TEXTURE_CACHE=0
        // turns it off; NFSMW_CP_CACHE_VERIFY=1 also does the full lookup on
        // every hit and logs any difference (it then returns the full
        // lookup's result).
        // A hit reads only the entry's first 64 bytes (and the fetch
        // constant): the key, the epoch, the result and the frame it last
        // set its texture's lastUsed in; revalidating it after a write, the
        // rest of the entry. The texture itself, a cold line by the next
        // draw (the snapshot copies stream megabytes a frame through the
        // caches), is touched once a frame per entry, to set lastUsed.
        struct alignas(64) TextureCacheEntry
        {
            uint32_t fetch[6] = {};
            uint32_t generation = 0;  // TextureCache::generation when made; 0: empty
            uint8_t dim = 0;
            bool scaledCandidate = false, drawScaled = false;
            uint64_t epoch = 0;  // WriteEpoch() it was last valid at
            VkImageView view = VK_NULL_HANDLE;
            VkSampler sampler = VK_NULL_HANDLE;
            uint64_t frame = 0;  // s_frame its texture's lastUsed was last set to through it
            Texture* texture = nullptr;
            uint32_t base = 0, size = 0, mipBase = 0, mipSize = 0;
            uint64_t loadedSequence = 0;  // the texture's when the entry was made (RevalidateTextureEntry)
            uint64_t sig = 0;  // the lookup's ScaledSignature (ScaledTextureValid)
        };
        static_assert(offsetof(TextureCacheEntry, frame) + sizeof(uint64_t) <= 64, "a hit's fields share the entry's first line");

        // CP_OPT_TEXTURE_MEMO: the texture bindings of the last draw that
        // looked its textures up, served again to the draws after it while
        // nothing they depend on changed. In a race ~5,000 textures a frame
        // were looked up one by one (~0.1 us each at a hit, ~9% of the
        // command processor with the misses, Deck profile), while most draws
        // bind what the draw before them bound. Its results are
        // TextureCache::infos as LookupTextures left them (in the layout's
        // binding order), and they are what looking each binding up now
        // would give, with nothing loaded, while all of these hold:
        // - the same layout (the same object: its bindings and their
        //   dimensions);
        // - each fetch constant those read holds the same 6 dwords (each
        //   lookup's key, as an entry's);
        // - WriteEpoch() is the one read before those lookups: no page
        //   anywhere was written since (a CPU store faults and bumps it,
        //   resolves mark what they write and fused textures are current
        //   only through that, host writes mark theirs), so no texture can
        //   be stale, and no scaled copy current (ScaledTextureValid is
        //   decided per epoch). This is the rule a cache entry is valid by
        //   (GetTexture), so each lookup would be a hit at its entry, or a
        //   full lookup finding the same texture (nothing can have made or
        //   reloaded it at this epoch) with the same view (kept until the
        //   texture is dropped) and sampler (kept for good);
        // - the same cache generation (DropTextures, a setting such as
        //   anisotropy or mipmaps, the scaled resolve memory made or reset
        //   bump it) and the same s_drawScaled (the scaled path's choice);
        // - the same s_frame: those lookups set each texture's lastUsed to
        //   it, as these would (the eviction's age);
        // - no texture evicted since (EvictTextures forgets the memo, as it
        //   does the entries).
        // Each binding must have been a kept lookup (an entry's): a draw
        // binding a placeholder or a scaled texture isn't memoized. With
        // NFSMW_CP_CACHE_VERIFY=1 a draw the memo would serve looks its
        // textures up anyway and logs any difference.
        struct TextureDrawMemo
        {
            const TextureLayout* layout = nullptr;  // null: holds nothing
            uint64_t epoch = 0, frame = 0;
            uint32_t generation = 0;
            bool drawScaled = false;
            uint32_t fetch[32][6] = {};  // [n]: fetch constant n as looked up (the layout's fetchSlots)
        };
        struct TextureCache
        {
            TextureCacheEntry slots[3][32];
            // By the fetch constant's hash: two entries a hash, the newer
            // first. Direct-mapped (1,024), lookups of keys made before that
            // a colliding key had pushed out were ~130 a frame in a race (300-
            // 500 keys a frame; ~2 us each, a full lookup).
            TextureCacheEntry recent[1024][2];
            uint32_t generation = 1;
            uint64_t settingsGeneration = 0;
            bool on = false, verify = false, memoOn = false;
            uint64_t hits = 0, recentHits = 0, revalidated = 0, misses = 0, stale = 0, uncached = 0, mismatches = 0, raced = 0;
            // Draw bindings: [i] is binding i of the last layout looked up
            // (LookupTextures), what the memo serves and Draw pushes from.
            // `infosSerial` moves on whenever they are rewritten, so the
            // draw's bound set can tell they are still what it pushed.
            VkDescriptorImageInfo infos[96] = {};
            uint64_t infosSerial = 1;
            TextureDrawMemo memo;
            uint64_t evictions = 0;  // EvictTextures calls that evicted any
            uint64_t memoDraws = 0, memoBindings = 0, lookupDraws = 0;  // draws with textures served by the memo, or looked up
        };
        // On the heap (as the shader cache): the renderer's globals move by a pointer, not by the tables (4K aliasing, see NOTES).
        TextureCache* const s_textureCache = [] {
            auto* c = new TextureCache();
            const char* v = std::getenv("NFSMW_TEXTURE_CACHE");
            // (Built on the epoch rule: without it the full lookup rescans every time.)
            c->on = (g_cpOpt & CP_OPT_TEXTURE_CACHE) && (g_cpOpt & CP_OPT_TEXTURE_EPOCH) && !(v && v[0] == '0') && s_logTextureUse == 0;
            c->verify = std::getenv("NFSMW_CP_CACHE_VERIFY") != nullptr;
            // (Built on the entries: their rules are its rules.)
            c->memoOn = c->on && (g_cpOpt & CP_OPT_TEXTURE_MEMO);
            return c;
        }();

        void ForgetTextureLookups(const Texture* t)
        {
            TextureCache& c = *s_textureCache;
            for (auto& slots : c.slots)
                for (TextureCacheEntry& e : slots)
                    if (e.texture == t)
                        e.generation = 0;
            for (auto& ways : c.recent)
                for (TextureCacheEntry& e : ways)
                    if (e.texture == t)
                        e.generation = 0;
            c.memo.layout = nullptr;  // (the last draw's bindings may hold it)
        }

        // Whether entry `e` is this lookup's: its key is the fetch constant,
        // the dimension, the cache's generation and, for a texture the
        // scaled path could take, whether the draw renders into scaled
        // targets (see above).
        [[gnu::always_inline]] inline bool TextureEntryKey(const TextureCacheEntry& e, uint32_t generation, TexDim dim, const uint32_t* fetch,
            bool drawScaled)
        {
            return e.generation == generation && e.dim == uint8_t(dim) && memcmp(e.fetch, fetch, sizeof(e.fetch)) == 0 &&
                !(e.scaledCandidate && e.drawScaled != drawScaled);
        }

        // Entry `e` of this lookup's key, not valid at `epoch` yet (some page
        // was written since it last was): whether it still holds the full
        // lookup's result, checked as the full lookup checks, in its order
        // (its scaled copy first); if so it is valid at `epoch` from now on.
        // Mostly from the entry alone: it keeps its texture's load sequence
        // as of its last check, and nothing written after that is all the
        // full lookup asks, so the texture (a cold line, ~2% of the command
        // processor in a race when every check read it) isn't read. Only
        // when something was: the texture may have been loaded since (by a
        // full lookup through another entry, or a resolve storing into it,
        // which writes its pages after the entry's sequence too), and is
        // then checked against its own sequence, as the full lookup would.
        // Every texture an entry holds is loaded (the full lookup keeps none
        // before), and never unloaded while it lives.
        [[gnu::noinline]] bool RevalidateTextureEntry(TextureCacheEntry& e, bool drawScaled, uint64_t epoch)
        {
            TextureCache& c = *s_textureCache;
            // (WrittenSince, as the full lookup asks: it skips the 256 KB
            // blocks written before the load without reading their pages'
            // sequences.)
            auto writtenSince = [&](uint64_t sequence) {
                return writewatch::WrittenSince(e.base, e.size, sequence) || (e.mipSize && writewatch::WrittenSince(e.mipBase, e.mipSize, sequence));
            };
            bool valid = !(e.scaledCandidate && drawScaled && ScaledTextureValid(e.base, e.size, e.sig));
            if (valid && writtenSince(e.loadedSequence))
            {
                const uint64_t loaded = e.texture->loadedSequence;
                valid = loaded != e.loadedSequence && !writtenSince(loaded);
                if (valid)
                    e.loadedSequence = loaded;
            }
            if (!valid)
            {
                c.stale++;
                return false;
            }
            e.epoch = epoch;
            c.revalidated++;
            return true;
        }

        // The recent table's two entries for this key (the newer first).
        TextureCacheEntry* RecentTextureEntries(const uint32_t* fetch, TexDim dim)
        {
            uint64_t a, b, d;
            memcpy(&a, fetch, 8);
            memcpy(&b, fetch + 2, 8);
            memcpy(&d, fetch + 4, 8);
            uint64_t h = (a * 0x9E3779B97F4A7C15ull) ^ (b * 0xC2B2AE3D27D4EB4Full) ^ (d * 0x165667B19E3779F9ull) ^ uint64_t(dim);
            return s_textureCache->recent[(h * 0x9E3779B97F4A7C15ull) >> 54];
        }

        // `kept`: the result is an entry's (a hit, or a lookup now kept),
        // which the draw memo may serve again (TextureDrawMemo).
        VkImageView GetTexture(uint32_t slot, TexDim dim, VkSampler& sampler, bool& kept)
        {
            TextureCache& c = *s_textureCache;
            kept = false;
            if (!c.on)
                return LookupTexture(slot, dim, sampler, nullptr);
            const uint32_t* fetch = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + slot * 6];
            TextureCacheEntry& e = c.slots[uint32_t(dim)][slot & 31];
            const bool drawScaled = s_drawScaled && s_scaled.enabled;
            const uint64_t epoch = writewatch::WriteEpoch();
            bool valid = false;
            if (TextureEntryKey(e, c.generation, dim, fetch, drawScaled))
                valid = e.epoch == epoch || RevalidateTextureEntry(e, drawScaled, epoch);
            else
            {
                // (A key is in one of the two at most: see the miss below.)
                TextureCacheEntry* recent = RecentTextureEntries(fetch, dim);
                for (uint32_t way = 0; way < 2; way++)
                {
                    TextureCacheEntry& r = recent[way];
                    if (!TextureEntryKey(r, c.generation, dim, fetch, drawScaled))
                        continue;
                    if (r.epoch == epoch || RevalidateTextureEntry(r, drawScaled, epoch))
                    {
                        e = r;
                        c.recentHits++;
                        valid = true;
                    }
                    break;
                }
            }
            if (valid)
            {
                c.hits++;
                // The texture's lastUsed (the eviction's age), set through
                // each entry once a frame, not at every hit (its line is
                // cold): an entry whose frame is s_frame has set it to
                // s_frame already, for this entry's texture, and lastUsed
                // never goes back.
                if (e.frame != s_frame)
                {
                    e.frame = s_frame;
                    e.texture->lastUsed = s_frame;
                }
                if (s_textureVerify)
                    VerifyTexture(*e.texture, epoch);
                if (!c.verify)
                {
                    sampler = e.sampler;
                    kept = true;
                    return e.view;
                }
                // The full lookup as if this hit hadn't happened (a hit
                // changes nothing the full lookup checks): a reload it does
                // is one the hit would have missed, unless a page was
                // written during the check (another thread): the full
                // lookup then saw memory the hit, a moment earlier, didn't.
                uint32_t loads = s_hitch.textures;
                TextureLookup lookup;
                VkImageView view = LookupTexture(slot, dim, sampler, &lookup);
                bool reloaded = s_hitch.textures != loads;
                if (reloaded && writewatch::WriteEpoch() != epoch)
                {
                    c.raced++;
                    reloaded = false;
                }
                if (view != e.view || sampler != e.sampler || lookup.texture != e.texture || reloaded)
                {
                    if (c.mismatches++ < 50)
                        fprintf(stderr, "[cpverify] texture slot %u dim %u: cached view %p sampler %p texture %p, full lookup %p %p %p%s "
                            "(fetch %08X %08X %08X %08X %08X %08X, epoch %llu, entry epoch %llu, scaled %d/%d)\n",
                            slot, uint32_t(dim), (void*)e.view, (void*)e.sampler, (void*)e.texture, (void*)view, (void*)sampler,
                            (void*)lookup.texture, reloaded ? ", reloaded" : "", fetch[0], fetch[1], fetch[2], fetch[3],
                            fetch[4], fetch[5], (unsigned long long)epoch, (unsigned long long)e.epoch, int(e.scaledCandidate), int(drawScaled));
                    e.generation = 0;
                    for (uint32_t way = 0; way < 2; way++)
                    {
                        TextureCacheEntry& r = RecentTextureEntries(fetch, dim)[way];
                        if (TextureEntryKey(r, c.generation, dim, fetch, drawScaled))
                            r.generation = 0;
                    }
                    return view;
                }
                kept = true;
                return view;
            }
            c.misses++;
            TextureLookup lookup;
            VkImageView view = LookupTexture(slot, dim, sampler, &lookup);
            if (!lookup.texture)
            {
                c.uncached++;
                e.generation = 0;
                return view;
            }
            memcpy(e.fetch, fetch, sizeof(e.fetch));
            e.generation = c.generation;
            e.dim = uint8_t(dim);
            e.scaledCandidate = lookup.scaledCandidate;
            e.drawScaled = drawScaled;
            e.epoch = epoch;  // read before the lookup: a write meanwhile is checked next time
            e.view = view;
            e.sampler = sampler;
            e.frame = s_frame;  // (the lookup set its lastUsed)
            e.texture = lookup.texture;
            e.base = lookup.base;
            e.size = lookup.size;
            e.mipBase = lookup.mipBase;
            e.mipSize = lookup.mipSize;
            e.sig = lookup.sig;
            e.loadedSequence = lookup.texture->loadedSequence;  // (current: the lookup found its pages unwritten since, or loaded it)
            // Into the recent table as its newer entry, over this key's own
            // older one (a stale one, which missed) if either holds it, so a
            // key is never in both; else the newer one moves to second place.
            TextureCacheEntry* recent = RecentTextureEntries(fetch, dim);
            if (!TextureEntryKey(recent[0], c.generation, dim, fetch, drawScaled))
                recent[1] = recent[0];
            recent[0] = e;
            kept = true;
            return view;
        }

        // Whether the draw memo holds `layout`'s bindings as they would be
        // looked up now (TextureDrawMemo's rules); `epoch`: WriteEpoch() now.
        bool TextureMemoHolds(const TextureLayout& layout, uint64_t epoch, bool drawScaled)
        {
            const TextureCache& c = *s_textureCache;
            const TextureDrawMemo& m = c.memo;
            if (m.layout != &layout || m.epoch != epoch || m.generation != c.generation || m.frame != s_frame || m.drawScaled != drawScaled)
                return false;
            const uint32_t* regs = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
            for (uint32_t bits = layout.fetchSlots; bits; bits &= bits - 1)
            {
                const uint32_t n = uint32_t(__builtin_ctz(bits));
                if (memcmp(m.fetch[n], regs + n * 6, sizeof(m.fetch[n])) != 0)
                    return false;
            }
            return true;
        }

        // Looks `layout`'s bindings up into TextureCache::infos, one by one
        // (GetTexture); the memo then holds them if every one was kept and
        // nothing a lookup depends on moved meanwhile (the generation, an
        // eviction: a texture made here can push older ones out). `epoch`:
        // WriteEpoch() before the lookups (a write during them leaves the
        // memo behind the current epoch: the next draw looks up again).
        void LookupTextures(const TextureLayout& layout, uint64_t epoch, bool drawScaled)
        {
            TextureCache& c = *s_textureCache;
            const uint32_t generation = c.generation;
            const uint64_t evictions = c.evictions;
            c.memo.layout = nullptr;
            bool kept = c.memoOn && !(s_exp & 8);
            for (uint32_t i = 0; i < layout.bindingCount; i++)
            {
                const uint32_t b = layout.bindings[i], d = b >> 5;
                VkDescriptorImageInfo& ii = c.infos[i];
                ii = { s_sampler, s_dummy[d].view, VK_IMAGE_LAYOUT_GENERAL };
                if (!(s_exp & 8))  // NFSMW_GPU_EXP bit 8: placeholders only (texture cost experiment)
                {
                    bool k;
                    ii.imageView = GetTexture(b & 31, d == 0 ? TexDim::k2D : d == 1 ? TexDim::k3D : TexDim::kCube, ii.sampler, k);
                    kept = kept && k;
                }
            }
            c.infosSerial++;
            c.lookupDraws++;
            if (!kept || c.generation != generation || c.evictions != evictions)
                return;
            TextureDrawMemo& m = c.memo;
            m.epoch = epoch;
            m.frame = s_frame;
            m.generation = generation;
            m.drawScaled = drawScaled;
            const uint32_t* regs = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
            for (uint32_t bits = layout.fetchSlots; bits; bits &= bits - 1)
            {
                const uint32_t n = uint32_t(__builtin_ctz(bits));
                memcpy(m.fetch[n], regs + n * 6, sizeof(m.fetch[n]));
            }
            m.layout = &layout;
        }

        // ---------------------------------------------------------------
        // Resolves

        VkPipeline s_resolvePipeline = VK_NULL_HANDLE;
        VkPipeline s_resolveCoalescedPipeline = VK_NULL_HANDLE;  // scaled resolves (see COALESCED in the shader)
        // The same for a source of another scale denominator ([coalesced][x][y], each log2 den: 1.5x is 3/2,
        // 0.75x 3/4, 0.625x 5/8; a texel axis, a den above 2 that isn't 4 or 8, has its own, per exact scale:
        // ResolvePipeline), made at the first resolve that needs it; [..][0][0] are the two above.
        VkPipeline s_resolveDenPipelines[2][4][4] = {};
        // Sources above 1x: the same, built with SCALED_FUSE (and TEX_OFFSET),
        // whole scales too; [coalesced][x][y] as above.
        VkPipeline s_resolveScaledPipelines[2][4][4] = {};
        VkPipeline s_resolveTexturePipeline = VK_NULL_HANDLE;  // TEXTURE_ONLY (NFSMW_RESOLVE_DEFER; made at its first use)
        VkPipeline s_resolveWritebackPipeline = VK_NULL_HANDLE;  // TEXTURE_WRITEBACK (NFSMW_RESOLVE_DEFER_COLOR; the same)
        VkPipeline s_resolveWritebackCheckPipeline = VK_NULL_HANDLE;  // and WRITEBACK_CHECK (NFSMW_RESOLVE_DEFER_COLOR=2)
        VkPipeline ResolvePipeline(bool coalesced, const gpu::RenderScale& scale);
        void MakeResolvePipeline(bool coalesced, uint32_t denX, uint32_t denY, VkPipeline* out, bool textureOnly = false, uint32_t writeback = 0,
            bool scaled = false, const gpu::RenderScale* fixed = nullptr);
        // A coalesced group's samples fit its shared memory (64 x 16 words):
        // every scale but a texel axis's beside 4x (1x10/9 has 2 a texel,
        // 3x10/3 12).
        bool CoalescedFits(const gpu::RenderScale& scale)
        {
            const uint32_t blocks = 64 / (gpu::ScaleGlsl{}.sbLayoutDen(scale.x.den) * gpu::ScaleGlsl{}.sbLayoutDen(scale.y.den));
            return blocks * scale.SamplesPerBlock() <= 64 * 16;
        }

        // Coalesced scaled resolves: 2.8 -> 1.1 ms of scaled resolves a frame
        // on the Steam Machine (RADV), but 2.6-3.4 -> 4.1-4.4 ms on an M1 Pro
        // (MoltenVK), so not on Apple GPUs unless NFSMW_SCALED_COALESCE=1.
        bool ScaledCoalesce(const VkPhysicalDeviceProperties& props)
        {
            const char* coalesce = std::getenv("NFSMW_SCALED_COALESCE");
            return coalesce ? coalesce[0] != '0' : props.vendorID != 0x106B;
        }

        // Whether a coalesced group (32x2 pixels, see COALESCED in the shader)
        // holds whole blocks of `scale`: their rows two at most. Not below 1x
        // at all: a block of 3/4 or 5/8 is four or eight rows, which a group
        // would split, and at 1/2, where it fits, each pixel owns a quarter of
        // a sample, so the group's shared memory and barrier cost more than
        // they gather (a 1280x720 resolve 474 us coalesced, against 367 us
        // per thread at 3/4, on the Steam Deck).
        bool ScaledCoalesceFits(const gpu::RenderScale& scale)
        {
            return !scale.Below() && CoalescedFits(scale);
        }

        // The resolve pipelines a scale will use, made now (at start or at a
        // live change, with the menu open) rather than at its first resolve
        // mid-frame: a new one costs ~0.4 s on MoltenVK (once; the pipeline
        // cache keeps it), 23 ms on RADV.
        void PrewarmResolvePipelines(const gpu::RenderScale& scale)
        {
            if (scale.IsOne())
                return;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            ResolvePipeline(false, scale);
            if (ScaledCoalesce(props) && ScaledCoalesceFits(scale))
                ResolvePipeline(true, scale);
        }
        VkPipelineLayout s_resolveLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_resolveSetLayout = VK_NULL_HANDLE;

        struct ResolveConstants
        {
            int32_t originX, originY;
            uint32_t width, height;
            uint32_t destBase, destPitch;
            uint32_t format, endian;
            uint32_t flags;  // bit 0: swap red/blue; bit 1: depth source
            float expScale;
            uint32_t srcWidth, srcHeight;
            int32_t srcOriginX, srcOriginY;  // host pixels
            uint32_t scaleX, scaleY;         // host pixels per guest pixel in the source (/ the pipeline's SB_DEN_X, SB_DEN_Y)
            uint32_t scaledWrite, slotWords; // also write the samples to the scaled memory
            // A texel axis's (SB_TEXEL_X, SB_TEXEL_Y) denominator, and the
            // rectangle's first pixel in the source target in guest pixels
            // (r + the tile shift), which its host pixels count from.
            uint32_t denX, denY;
            int32_t srcGuestX, srcGuestY;
            // Also store into a cached direct texture (binding 4), as its
            // untile would have from the words (see Resolve); texX, texY:
            // where the destination's texel 0,0 is in it (NFSMW_FUSE_PARTIAL,
            // NFSMW_DIRECT_CUBE; pushed only with either, see s_resolvePush).
            uint32_t texWrite, texEndian, texConvert, texWidth, texHeight;
            uint32_t texX, texY;
            // A scaled resolve's samples also into a cached scaled texture
            // (binding 5), where its untile would have put them (see
            // Resolve): hostX, hostY: the destination's texel 0,0 in it; its
            // guest size and endianness. Pushed (and in the shader:
            // SCALED_FUSE) only to the pipelines of scaled sources.
            uint32_t hostWrite, hostX, hostY, hostWidth, hostHeight, hostEndian;
        };
        // The constants pushed to the resolve shader: without NFSMW_FUSE_PARTIAL
        // and NFSMW_DIRECT_CUBE, those before texX, as before them, and the
        // shader is built without TEX_OFFSET (the same code and push).
        const uint32_t s_resolvePush = s_fusePartial || s_directCube ? offsetof(ResolveConstants, hostWrite) : offsetof(ResolveConstants, texX);

        const char* kResolveGlsl = R"(#version 460
// 24-bit depth (in a word's low bits) to the float a depth texture holds.
// NFSMW_RESOLVE_LAZY defines SB_D24_EXACT: one correctly rounded multiply by
// 1/16777215 (0x33800001), the same value in every shader, so that the
// write-back finds the d24 it came from (a division may round otherwise in
// each pipeline: 1 in 24 bits apart on RADV).
#define D24F(d) (float(d) / 16777215.0)
#extension GL_EXT_samplerless_texture_functions : require
#ifdef TEXTURE_ONLY
layout(local_size_x = 256) in;
#else
layout(local_size_x = 8, local_size_y = 8) in;
#endif
layout(std430, set = 0, binding = 0) buffer SharedMemory { uint g_mem[]; };)" GLSL_SHARED_MEMORY_WRITE R"(
layout(set = 0, binding = 1) uniform texture2D src;
layout(std430, set = 0, binding = 2) buffer ScaledPool { uint s_pool[]; };
layout(std430, set = 0, binding = 3) readonly buffer ScaledTable { uint s_table[]; };
#ifdef TEXTURE_WRITEBACK
layout(set = 0, binding = 4, r32ui) readonly uniform uimage2D t_image;
#elif defined(FRONT_DIRECT)
layout(set = 0, binding = 4, rgba8) writeonly uniform image2D t_image;  // NFSMW_RESOLVE_PRESENT: the presenter's front image
#else
layout(set = 0, binding = 4, r32ui) writeonly uniform uimage2D t_image;
#endif
#ifdef SCALED_FUSE
layout(set = 0, binding = 5, r32ui) writeonly uniform uimage2D h_image;
#endif
layout(push_constant) uniform P
{
    ivec2 origin;
    uvec2 size;
    uint destBase, destPitch, format, endian, flags;
    float expScale;
    uvec2 srcSize;
    ivec2 srcOrigin;    // where the rectangle is in the host target (see untiled rendering), host pixels
    uvec2 scale;        // host pixels per guest pixel (internal resolution): scale / kDen
    uint scaledWrite, slotWords;
    uvec2 den;          // a texel axis's denominator (SB_TEXEL_X, SB_TEXEL_Y)
    ivec2 srcGuest;     // the rectangle's first pixel in the source target, guest pixels (texel axes)
    uint texWrite, texEndian, texConvert, texWidth, texHeight;  // also into a cached texture (binding 4)
#ifdef TEX_OFFSET
    uint texX, texY;    // the destination's texel 0,0 in it (whole tiles)
#endif
#ifdef SCALED_FUSE
    // Also each host sample into a cached scaled texture (binding 5) where
    // its untile would put it: its texel of the destination's 0,0 (whole
    // tiles), its guest size, its endianness.
    uint hostWrite, hostX, hostY, hostWidth, hostHeight, hostEndian;
#endif
} p;
)" GLSL_SCALE_CODE R"(
// The scale's denominators are the pipeline's own (CreateResolvePipeline: a
// pair of pipelines for each, made at the first resolve that needs it), not
// push constants: at a whole scale (1, 1) every block and weight expression
// folds back to the arithmetic from before 1.5x, with no integer division.
#ifndef SB_DEN_X
#define SB_DEN_X 1u
#endif
#ifndef SB_DEN_Y
#define SB_DEN_Y 1u
#endif
const uvec2 kDen = uvec2(SB_DEN_X, SB_DEN_Y);
// A texel axis (a den above 2, gpu/render_scale.h: a Steam Deck's native rows,
// 10/9): its num / den from the constants and no blocks (kDen 1 there), each
// pixel's host pixels counted from where it is in the source target
// (srcGuest), the other axis's too. Without either the code is as before.
#ifndef SB_TEXEL_X
#define SB_TEXEL_X 0
#endif
#ifndef SB_TEXEL_Y
#define SB_TEXEL_Y 0
#endif
#if SB_TEXEL_X || SB_TEXEL_Y
#define SB_TEXEL
// The scale as the pipeline's constants when it has them (SB_FIXED_*: a
// texel axis's pipelines are made for their exact scale, so every division
// by num or den is by a constant, a multiply, not a runtime division).
#ifdef SB_FIXED_NUM_X
uvec2 texelDen() { return uvec2(SB_FIXED_DEN_X, SB_FIXED_DEN_Y); }
#define TEXEL_NUM uvec2(SB_FIXED_NUM_X, SB_FIXED_NUM_Y)
#else
uvec2 texelDen() { return uvec2(SB_TEXEL_X != 0 ? p.den.x : kDen.x, SB_TEXEL_Y != 0 ? p.den.y : kDen.y); }
#define TEXEL_NUM p.scale
#endif
#endif

uint tiled2D(uint x, uint y, uint pitch, uint bppLog2)
{
    uint outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outer | inner) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

uint swapWord(uint v, uint e)
{
    if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u || e >= 4u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (e == 3u) return (v >> 16) | (v << 16);
    return v;
}

uint bppLog2(uint f)
{
    if (f == 2u) return 0u;                                  // 8
    if (f == 3u || f == 4u || f == 10u || f == 15u || f == 24u || f == 30u) return 1u;
    if (f == 26u || f == 32u || f == 34u || f == 37u) return 3u;
    return 2u;
}

// px() at 1.5x (a den of 2): the host pixels the guest pixel covers, each
// weighted by how much of it lies inside (gpu/render_scale.h's box filter:
// 2/3 and 1/3, in 1/den of a pixel, num of them a whole guest pixel), or
// (depth) the one holding its centre. The rectangle's origin is whole blocks
// (even), so its pixels' host coordinates count from srcOrigin. Below 1x
// (a den of 2, 4 or 8; its origin a multiple of den) the one host pixel
// holding the guest pixel's centre, colour as depth (below).
vec4 pxFraction(uint x, uint y)
{
    if ((p.flags & 2u) != 0u)
        return texelFetch(src, clamp(p.srcOrigin + ivec2(sbCenterHost(x, p.scale.x, kDen.x), sbCenterHost(y, p.scale.y, kDen.y)),
            ivec2(0), ivec2(p.srcSize) - 1), 0);
    vec4 v = vec4(0.0);
    if (p.scale.x < kDen.x && p.scale.y < kDen.y)
    {
        // Below 1x: the host pixel holding the guest pixel's centre, as for
        // depth. The 1x copy is the bloom's input (blurred down from 320x180)
        // and the CPU's, never what the frame shows; the box filter's two
        // to four fetches a pixel (at 0.75x 2.25 on average, with their
        // weights) made a scaled resolve of the scene 3.5 times as costly
        // as a 1x one on the Steam Deck, more than the draws saved.
        v = texelFetch(src, clamp(p.srcOrigin + ivec2(sbCenterHost(x, p.scale.x, kDen.x), sbCenterHost(y, p.scale.y, kDen.y)),
            ivec2(0), ivec2(p.srcSize) - 1), 0);
    }
    else
    {
        uint x1 = sbHostSize(x + 1u, p.scale.x, kDen.x), y1 = sbHostSize(y + 1u, p.scale.y, kDen.y);
        for (uint j = sbHostEdge(y, p.scale.y, kDen.y); j < y1; j++)
            for (uint i = sbHostEdge(x, p.scale.x, kDen.x); i < x1; i++)
                v += float(sbOverlap(x, i, p.scale.x, kDen.x) * sbOverlap(y, j, p.scale.y, kDen.y)) *
                    texelFetch(src, clamp(p.srcOrigin + ivec2(i, j), ivec2(0), ivec2(p.srcSize) - 1), 0);
        v /= float(p.scale.x * p.scale.y);
    }
    v *= p.expScale;
    if ((p.flags & 1u) != 0u) v = v.bgra;
    return v;
}

#ifdef SB_TEXEL
// px() with a texel axis: the host pixels guest pixel (x, y) of the rectangle
// covers, from its place in the source target (srcGuest + x, y), each
// weighted by how much of it lies inside (the box filter), or (depth) the
// one holding its centre.
vec4 pxTexel(uint x, uint y)
{
    uvec2 n = TEXEL_NUM, d = texelDen();
    uvec2 g = uvec2(p.srcGuest) + uvec2(x, y);
    if ((p.flags & 2u) != 0u)
        return texelFetch(src, clamp(ivec2(sbCenterHost(g.x, n.x, d.x), sbCenterHost(g.y, n.y, d.y)), ivec2(0), ivec2(p.srcSize) - 1), 0);
    vec4 v = vec4(0.0);
    uint x1 = sbHostSize(g.x + 1u, n.x, d.x), y1 = sbHostSize(g.y + 1u, n.y, d.y);
    for (uint j = g.y * n.y / d.y; j < y1; j++)
        for (uint i = g.x * n.x / d.x; i < x1; i++)
            v += float(sbOverlap(g.x, i, n.x, d.x) * sbOverlap(g.y, j, n.y, d.y)) *
                texelFetch(src, clamp(ivec2(i, j), ivec2(0), ivec2(p.srcSize) - 1), 0);
    v /= float(n.x * n.y);
    v *= p.expScale;
    if ((p.flags & 1u) != 0u) v = v.bgra;
    return v;
}
#endif

vec4 px(uint x, uint y)
{
#ifdef SB_TEXEL
    return pxTexel(x, y);
#else
    // Guest pixel (x, y) of the rectangle: its sx x sy block of host pixels,
    // averaged (colour) or its first sample (depth).
    if (kDen.x * kDen.y > 1u) return pxFraction(x, y);
    ivec2 c = clamp(p.srcOrigin + ivec2(x * p.scale.x, y * p.scale.y), ivec2(0), ivec2(p.srcSize) - 1);
    vec4 v = texelFetch(src, c, 0);
    if ((p.flags & 2u) != 0u) return v;
    if (p.scale.x * p.scale.y > 1u)
    {
        for (uint j = 0u; j < p.scale.y; j++)
            for (uint i = 0u; i < p.scale.x; i++)
                if (i + j > 0u)
                    v += texelFetch(src, clamp(c + ivec2(i, j), ivec2(0), ivec2(p.srcSize) - 1), 0);
        v /= float(p.scale.x * p.scale.y);
    }
    v *= p.expScale;
    if ((p.flags & 1u) != 0u) v = v.bgra;
    return v;
#endif
}

uint unorm(float v, float m) { return uint(clamp(v, 0.0, 1.0) * m + 0.5); }
)" GLSL_DEPTH24_PACK R"(
uint pack32(vec4 c)
{
    switch (p.format)
    {
    case 6u: case 14u: return packUnorm4x8(c);
    case 7u: return unorm(c.r, 1023.0) | (unorm(c.g, 1023.0) << 10) | (unorm(c.b, 1023.0) << 20) | (unorm(c.a, 3.0) << 30);
    case 22u: case 23u: return packDepth24(c.r) << 8;  // (not unorm: 1.0 overflows 24 bits, see depth24_glsl.h)
    case 25u: return packSnorm2x16(c.rg);
    case 31u: return packHalf2x16(c.rg);
    case 33u: case 36u: return floatBitsToUint(c.r);
    default: return packUnorm4x8(c);
    }
}

uint pack16(vec4 c)
{
    switch (p.format)
    {
    case 3u: return unorm(c.r, 31.0) | (unorm(c.g, 31.0) << 5) | (unorm(c.b, 31.0) << 10) | (unorm(c.a, 1.0) << 15);
    case 4u: return unorm(c.r, 31.0) | (unorm(c.g, 63.0) << 5) | (unorm(c.b, 31.0) << 11);
    case 15u: return unorm(c.r, 15.0) | (unorm(c.g, 15.0) << 4) | (unorm(c.b, 15.0) << 8) | (unorm(c.a, 15.0) << 12);
    case 10u: return unorm(c.r, 255.0) | (unorm(c.g, 255.0) << 8);
    case 30u: return packHalf2x16(vec2(c.r, 0.0)) & 0xFFFFu;
    default: return unorm(c.r, 65535.0);
    }
}

#ifdef COALESCED
// A scaled resolve's samples stored coalesced: a group is 32x2 pixels, its
// threads in the order of their words in the 32-bit tiled layout (64
// consecutive words when aligned), and the samples go to the pool through
// shared memory so that each store writes consecutive pool words. Each
// thread storing its own pixel's samples writes words `s` apart across the
// group, several times the memory transactions: most of a scaled resolve's
// cost. (A pipeline of its own: the shared memory would slow the others.)
// By block (gpu/render_scale.h): a pixel at a whole scale, 2x2 pixels at
// 1.5x (a group is 16 whole blocks: its rows start even, as its columns).
// Not below 1x (ScaledCoalesceFits).
shared uint g_samples[64 * 16];  // by block, then sample (up to 64 pixels' worth at 4x4)
shared uint g_offsets[64];       // each block's samples in the pool; ~0u: none
#endif

#ifdef TEXTURE_ONLY
// NFSMW_RESOLVE_DEFER: a 32-bit resolve at a whole scale, without scaled
// memory, into its cached texture only (main() below writes its guest bytes
// later, if anything reads them). The same texels as main(). A group a 32x32
// tile of the destination, each thread four texels: those of the tile's
// words t, t + 256, t + 512 and t + 768 (tiled2D's bits 2-11 taken apart: x
// 0-1, y 0, x 2, the pipe, y 1-3, the bank: y 4). On the Steam Deck that
// stored a 1600x1600 shadow map's texture in ~305 us where 8x8 groups of a
// texel a thread took ~395.
void main()
{
    uvec2 tile = (uvec2(p.origin) >> 5u) + gl_WorkGroupID.xy;
    for (uint k = 0u; k < 4u; k++)
    {
        uint a = (gl_LocalInvocationIndex + k * 256u) << 2;
        uint ly = ((a >> 4) & 1u) | (((a >> 8) & 7u) << 1) | (((a >> 11) & 1u) << 4);
        uint lx = ((a >> 2) & 3u) | (((a >> 5) & 1u) << 2) | ((((a >> 6) & 3u) ^ (((ly >> 3) & 1u) << 1)) << 3);
        uvec2 g = tile * 32u + uvec2(lx, ly) - uvec2(p.origin);  // (wraps below the origin: outside)
        if (g.x >= p.size.x || g.y >= p.size.y)
            continue;
        uint x = uint(p.origin.x) + g.x, y = uint(p.origin.y) + g.y;
#ifdef TEX_OFFSET
        if (x + p.texX >= p.texWidth || y + p.texY >= p.texHeight)
            continue;
#else
        if (x >= p.texWidth || y >= p.texHeight)
            continue;
#endif
        uint v = swapWord(swapWord(pack32(px(g.x, g.y)), p.endian), p.texEndian);
        uint o = p.texConvert == 4u ? floatBitsToUint(D24F(v >> 8)) : v;
#ifdef TEX_OFFSET
        imageStore(t_image, ivec2(int(x + p.texX), int(y + p.texY)), uvec4(o, 0u, 0u, 0u));
#else
        imageStore(t_image, ivec2(int(x), int(y)), uvec4(o, 0u, 0u, 0u));
#endif
    }
}
#elif defined(TEXTURE_WRITEBACK)
// NFSMW_RESOLVE_DEFER_COLOR: a deferred colour resolve's guest bytes, from
// the cached texture that holds its texels: raw 32-bit ones (no conversion),
// each the guest word in the texture's endianness (main() below stores
// swapWord(m, texEndian)), so swapped back it is the word again. The
// rectangle lies in the texture whole (see Resolve).
void main()
{
    uvec2 id = gl_GlobalInvocationID.xy;
    if (id.x >= p.size.x || id.y >= p.size.y)
        return;
    uint x = uint(p.origin.x) + id.x, y = uint(p.origin.y) + id.y;
    uint word = ((p.destBase + tiled2D(x, y, p.destPitch, 2u)) & 0x1FFFFFFFu) >> 2;
    // NFSMW_RESOLVE_LAZY: a rectangle past the texture by
    // its 8-pixel alignment only (D3D resolved the surface whole): those
    // words stay as they were.
#ifdef TEX_OFFSET
    if (x + p.texX >= p.texWidth || y + p.texY >= p.texHeight)
        return;
    uint v = imageLoad(t_image, ivec2(int(x + p.texX), int(y + p.texY))).x;
#else
    if (x >= p.texWidth || y >= p.texHeight)
        return;
    uint v = imageLoad(t_image, ivec2(int(x), int(y))).x;
#endif
    if (p.texConvert == 4u)
    {
        // NFSMW_RESOLVE_LAZY: a depth resolve's texel, D24F(d24) as
        // TEXTURE_ONLY stored it, back to the word main() writes (d24 << 8,
        // the stencil byte 0): the 24-bit value whose quotient is this float,
        // the nearest to packDepth24's first (0, -1, +1, ...). Where a driver
        // divides by multiplying with the reciprocal (RADV), 0xFFFFFE and
        // 0xFFFFFF (the far plane, a shadow map's clear) both give 1.0: the
        // clear's comes back, 0xFFFFFE does as 0xFFFFFF (its texture can't
        // tell them apart either).
        float f = uintBitsToFloat(v);
        uint d0 = packDepth24(f), d = d0;
        for (int k = 0; k < 7; k++)
        {
            uint c = uint(int(d0) + ((k & 1) != 0 ? -((k + 1) >> 1) : (k >> 1)));
            if (c <= 0xFFFFFFu && D24F(c) == f)
            {
                d = c;
                break;
            }
        }
        v = d << 8;
    }
#ifdef WRITEBACK_CHECK
    // NFSMW_RESOLVE_DEFER_COLOR=2: the word main() stored, compared (the
    // pool binding counts: [0] words that differ, [1] words compared, [2]
    // the largest difference of a depth word's 24 bits).
    uint g = memLoad(word);
    if (g != swapWord(v, p.texEndian))
    {
        atomicAdd(s_pool[0], 1u);
        if (p.texConvert == 4u)
            atomicMax(s_pool[2], uint(abs(int(swapWord(g, p.texEndian) >> 8) - int(v >> 8))));
        if (atomicCompSwap(s_pool[3], 0u, 1u) == 0u)
        {
#ifdef TEX_OFFSET
            s_pool[4] = imageLoad(t_image, ivec2(int(x + p.texX), int(y + p.texY))).x;
#else
            s_pool[4] = imageLoad(t_image, ivec2(int(x), int(y))).x;
#endif
            s_pool[5] = g; s_pool[6] = v; s_pool[7] = x; s_pool[8] = y; s_pool[9] = p.destBase; s_pool[10] = p.texConvert;
            s_pool[11] = p.endian | (p.texEndian << 8) | (p.format << 16);
        }
    }
    atomicAdd(s_pool[1], 1u);
#else
    memStore(word, swapWord(v, p.texEndian));
#endif
}
#else
void main()
{
    uint bl = bppLog2(p.format);
    uint perWord = bl >= 2u ? 1u : (4u >> bl);
#ifdef COALESCED
    uint l = gl_LocalInvocationIndex;
    uvec2 id = gl_WorkGroupID.xy * uvec2(32u, 2u) + uvec2((l & 3u) | ((l >> 3u) << 2u), (l >> 2u) & 1u);
#else
    uvec2 id = gl_GlobalInvocationID.xy;
#endif
    uint gx = id.x * perWord, gy = id.y;
    bool inside = gx < p.size.x && gy < p.size.y;
#ifndef COALESCED
    if (!inside) return;  // (a coalesced write needs every thread at its barrier)
#endif
    uint x = uint(p.origin.x) + gx, y = uint(p.origin.y) + gy;
    uint word = ((p.destBase + tiled2D(x, y, p.destPitch, bl)) & 0x1FFFFFFFu) >> 2;
    uint w;
    if (bl == 2u && p.scaledWrite != 0u)
    {
        // Each host sample this guest pixel owns into its block's place in
        // the page's scaled slot (see ScaledMemory, gpu/render_scale.h): at a
        // whole scale all its sx x sy, at 1.5x one, two or four of its 2x2
        // block's 3x3, from where its texel sits in the block (its word's
        // bits 0 and 2; the rectangle's origin is even, so gx and gy agree).
        // Below 1x one or none (3/4: a 4x4 block's 3x3, its first row and
        // column of texels owning none), from the word's sub-texel bits
        // (sbSubMask; the origin a multiple of den).
        uint a = word << 2;
        uint slot = inside ? s_table[a >> 12] : 0u;
#ifdef SB_TEXEL
        // With a texel axis: on it, all the texel's stride of samples (the
        // ones it owns, then its last again), from its own place in the
        // source target; on a block axis as below, from the block's corner.
        uvec2 n = TEXEL_NUM, d = texelDen();
        uint sX = sbStride(n.x, d.x);
        uint s = sX * sbStride(n.y, d.y);  // samples a block
        uint k = (a & 4095u) >> 2;
        uint o = (slot - 1u) * p.slotWords + sbBlockIndex(k, d.x, d.y) * s;
        uint subX = sbSubX(k, d.x), subY = sbSubY(k, d.y);
        uint i0 = sbSampleLo(subX, n.x, d.x), i1 = sbSampleHi(subX, n.x, d.x);
        uint j0 = sbSampleLo(subY, n.y, d.y), j1 = sbSampleHi(subY, n.y, d.y);
        uvec2 g = uvec2(p.srcGuest) + uvec2(gx, gy);
#else
        uint sX = p.scale.x;
        uint s = p.scale.x * p.scale.y;  // samples a block
        uint k = (a & 4095u) >> 2;
        uint o = (slot - 1u) * p.slotWords + sbBlockIndex(k, kDen.x, kDen.y) * s;
        uint subX = sbSubX(k, kDen.x), subY = sbSubY(k, kDen.y);
        uint i0 = sbHostEdge(subX, p.scale.x, kDen.x), i1 = sbHostEdge(subX + 1u, p.scale.x, kDen.x);
        uint j0 = sbHostEdge(subY, p.scale.y, kDen.y), j1 = sbHostEdge(subY + 1u, p.scale.y, kDen.y);
#endif
#ifdef COALESCED
        uint lb = sbBlockIndex(l, kDen.x, kDen.y);  // the block's in this group (l is the word order)
#endif
#ifdef SCALED_FUSE
        uint tx = x + p.hostX, ty = y + p.hostY;  // the texel in the cached scaled texture
#endif
        if (slot != 0u)
        {
#ifndef SB_TEXEL
            ivec2 c0 = p.srcOrigin + ivec2(sbHostEdge(gx, p.scale.x, kDen.x), sbHostEdge(gy, p.scale.y, kDen.y)) - ivec2(i0, j0);
#endif
            for (uint j = j0; j < j1; j++)
                for (uint i = i0; i < i1; i++)
                {
#ifdef SB_TEXEL
                    ivec2 c = ivec2(sbSampleHost(g.x, subX, i, n.x, d.x), sbSampleHost(g.y, subY, j, n.y, d.y));
#else
                    ivec2 c = c0 + ivec2(i, j);
#endif
                    vec4 v = texelFetch(src, clamp(c, ivec2(0), ivec2(p.srcSize) - 1), 0);
                    if ((p.flags & 2u) == 0u)
                    {
                        v *= p.expScale;
                        if ((p.flags & 1u) != 0u) v = v.bgra;
                    }
                    uint word32 = swapWord(pack32(v), p.endian);
#ifdef COALESCED
                    g_samples[lb * s + j * sX + i] = word32;
#elif defined(SCALED_FUSE)
                    if (p.hostWrite != 2u)  // (2: into the texture alone)
                        s_pool[o + j * sX + i] = word32;
#else
                    s_pool[o + j * sX + i] = word32;
#endif
#ifdef SCALED_FUSE
                    // The texture's host texel this sample is (sbReaderHost:
                    // the untile's own mapping; none for a texel axis's
                    // padding), from its texel (tx, ty).
                    if (p.hostWrite != 0u && tx < p.hostWidth && ty < p.hostHeight)
                    {
#ifdef SB_TEXEL
                        uint hx = sbReaderHost(tx, subX, i, n.x, d.x), hy = sbReaderHost(ty, subY, j, n.y, d.y);
#else
                        uint hx = sbReaderHost(tx, subX, i, p.scale.x, kDen.x), hy = sbReaderHost(ty, subY, j, p.scale.y, kDen.y);
#endif
                        if (hx != ~0u && hy != ~0u)
                            imageStore(h_image, ivec2(int(hx), int(hy)), uvec4(swapWord(word32, p.hostEndian), 0u, 0u, 0u));
                    }
#endif
                }
        }
#ifdef COALESCED
#ifdef SCALED_FUSE
        if (subX == 0u && subY == 0u)  // (every pixel at a whole scale; hostWrite 2: into the texture alone)
            g_offsets[lb] = slot != 0u && p.hostWrite != 2u ? o : ~0u;
#else
        if (subX == 0u && subY == 0u)  // (every pixel at a whole scale)
            g_offsets[lb] = slot != 0u ? o : ~0u;
#endif
        barrier();
        uint samples = 64u / (kDen.x * kDen.y) * s;
        for (uint q = l; q < samples; q += 64u)
        {
            uint po = g_offsets[q / s];
            if (po != ~0u)
                s_pool[po + q % s] = g_samples[q];
        }
#endif
    }
    if (!inside) return;
    if (bl == 3u)
    {
        vec4 c = px(gx, gy);
        uint w0, w1;
        if (p.format == 26u) { w0 = packSnorm2x16(c.rg); w1 = packSnorm2x16(c.ba); }
        else if (p.format == 32u) { w0 = packHalf2x16(c.rg); w1 = packHalf2x16(c.ba); }
        else { w0 = floatBitsToUint(c.r); w1 = floatBitsToUint(c.g); }
        if (p.endian == 4u) { uint t = w0; w0 = swapWord(w1, 2u); w1 = swapWord(t, 2u); }
        else { w0 = swapWord(w0, p.endian); w1 = swapWord(w1, p.endian); }
        memStore(word, w0);
        memStore(word + 1u, w1);
        return;
    }
    if (bl == 2u) w = pack32(px(gx, gy));
    else if (bl == 1u) w = pack16(px(gx, gy)) | (pack16(px(gx + 1u, gy)) << 16);
    else w = unorm(px(gx, gy).r, 255.0) | (unorm(px(gx + 1u, gy).r, 255.0) << 8) |
        (unorm(px(gx + 2u, gy).r, 255.0) << 16) | (unorm(px(gx + 3u, gy).r, 255.0) << 24);
    uint m = swapWord(w, p.endian);
    memStore(word, m);
#ifdef FRONT_DIRECT
    // NFSMW_RESOLVE_PRESENT: the resolve into the front
    // buffer also stores the pixel as the front buffer's conversion
    // (kFrontGlsl) would read this word: its endianness (texEndian), format
    // (texConvert) and swizzle (slotWords), within its size (texWidth,
    // texHeight).
    if (bl == 2u && x < p.texWidth && y < p.texHeight)
    {
        uint v = swapWord(m, p.texEndian);
        float c[6];
        if (p.texConvert == 7u || p.texConvert == 54u)
        {
            c[0] = float(v & 0x3FFu) / 1023.0; c[1] = float((v >> 10) & 0x3FFu) / 1023.0;
            c[2] = float((v >> 20) & 0x3FFu) / 1023.0; c[3] = float(v >> 30) / 3.0;
        }
        else
        {
            for (int i = 0; i < 4; i++) c[i] = float((v >> (8 * i)) & 0xFFu) / 255.0;
        }
        c[4] = 0.0; c[5] = 1.0;
        vec3 rgb = vec3(c[min(p.slotWords & 7u, 5u)], c[min((p.slotWords >> 3) & 7u, 5u)], c[min((p.slotWords >> 6) & 7u, 5u)]);
        imageStore(t_image, ivec2(int(x), int(y)), vec4(rgb, 1.0));
    }
#else
    // A cached direct 1x texture of this surface: its texel as the untile
    // would read the word (its endianness, D24 -> float). Or (TEX_OFFSET) one
    // holding it whole tiles in: 32-bit texels make a 4 KB tile, and
    // tiled2D's address is that tile's index in bits 12 and up, the rest from
    // x & 31 and y & 31 only, so the destination's x, y is the texture's
    // x + texX, y + texY.
#ifdef TEX_OFFSET
    if (p.texWrite != 0u && bl == 2u && x + p.texX < p.texWidth && y + p.texY < p.texHeight)
#else
    if (p.texWrite != 0u && bl == 2u && x < p.texWidth && y < p.texHeight)
#endif
    {
        uint v = swapWord(m, p.texEndian);
        // (D24: m is packDepth24's, clamped, so the far plane stores 1.0 here
        // too, not 0.)
        uint o = p.texConvert == 4u ? floatBitsToUint(D24F(v >> 8)) : v;
#ifdef TEX_OFFSET
        imageStore(t_image, ivec2(int(x + p.texX), int(y + p.texY)), uvec4(o, 0u, 0u, 0u));
#else
        imageStore(t_image, ivec2(int(x), int(y)), uvec4(o, 0u, 0u, 0u));
#endif
    }
#endif  // FRONT_DIRECT
}
#endif
)";

        uint32_t ResolveBppLog2(uint32_t f)
        {
            if (f == 2) return 0;
            if (f == 3 || f == 4 || f == 10 || f == 15 || f == 24 || f == 30) return 1;
            if (f == 26 || f == 32 || f == 34 || f == 37) return 3;
            return 2;
        }

        // The guest bytes a resolve of `r` writes, as offsets from the
        // destination base: the 4 KB blocks of the 32x32 tiles the rectangle
        // touches (Tiled2D puts the tile index, shifted by the bytes per
        // pixel, in address bits 12 and up). Not every row of the pitch down
        // to a 32-row boundary: at world entry NFSMW resolves the right
        // 640x720 half of a 1280x720 texture through a destination base 20
        // tiles into it, so whole rows from there ran 80 KB past the
        // texture, into Direct3D's command ring right after it. In shadow
        // mode (discrete GPUs) the pages a resolve marks are read back whole
        // from the GPU copy, which put old bytes over the game's newest
        // commands: a malformed indirect buffer, a hang.
        // NFSMW_RESOLVE_EXTENT=0 marks whole rows again.
        std::pair<uint32_t, uint32_t> ResolveExtent(const Rect& r, uint32_t pitchAligned, uint32_t bppLog2)
        {
            static const bool tiles = [] { const char* v = std::getenv("NFSMW_RESOLVE_EXTENT"); return !v || v[0] != '0'; }();
            if (!tiles)
                return { 0, (pitchAligned * ((uint32_t(r.y1) + 31) & ~31u)) << bppLog2 };
            uint32_t tilesPerRow = pitchAligned >> 5;
            uint32_t first = (uint32_t(r.y0) >> 5) * tilesPerRow + (uint32_t(r.x0) >> 5);
            uint32_t last = (uint32_t(r.y1 - 1) >> 5) * tilesPerRow + (uint32_t(r.x1 - 1) >> 5);
            return { ((first << (6 + bppLog2)) >> 8) << 12, (((((last + 1) << (6 + bppLog2)) - 1) >> 8) + 1) << 12 };
        }

        bool ResolveFormatSupported(uint32_t f)
        {
            switch (f)
            {
            case 2: case 3: case 4: case 6: case 7: case 10: case 14: case 15: case 22: case 23: case 24:
            case 25: case 26: case 30: case 31: case 32: case 33: case 34: case 36: case 37:
                return true;
            default:
                return false;
            }
        }

        int32_t ToFixed16p8(float f)
        {
            float v = std::clamp(f, -8388608.0f / 256.0f, 8388607.0f / 256.0f) * 256.0f;
            return int32_t(v + (v < 0 ? -0.5f : 0.5f));
        }

        // Clear a rectangle of a target (resolve with clear enabled).
        void ClearRect(RenderTarget* rt, const Rect& r)
        {
            VkClearAttachment ca{};
            if (rt->depth)
            {
                uint32_t v = s_regs[XE_GPU_REG_RB_DEPTH_CLEAR];
                ca.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
                float depth;
                if (xenos::DepthRenderTargetFormat(rt->format) == xenos::DepthRenderTargetFormat::kD24FS8)
                {
                    // 20e4 float: 4-bit exponent (bias 15), 20-bit mantissa.
                    uint32_t f24 = v >> 8;
                    uint32_t e = f24 >> 20, m = f24 & 0xFFFFF;
                    depth = e == 0 ? std::ldexp(float(m), -34) : std::ldexp(1.0f + float(m) / 1048576.0f, int(e) - 15);
                }
                else
                {
                    depth = float(v >> 8) / 16777215.0f;
                }
                ca.clearValue.depthStencil = { std::clamp(depth, 0.0f, 1.0f), v & 0xFF };
            }
            else
            {
                uint32_t lo = s_regs[XE_GPU_REG_RB_COLOR_CLEAR];
                uint32_t hi = s_regs[XE_GPU_REG_RB_COLOR_CLEAR_LO];
                ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                ca.colorAttachment = 0;
                float* c = ca.clearValue.color.float32;
                auto half = [](uint32_t h) {
                    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
                    float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? 65504.0f : std::ldexp(1.0f + m / 1024.0f, int(e) - 15);
                    return s ? -v : v;
                };
                switch (rt->vkFormat)
                {
                case VK_FORMAT_R8G8B8A8_UNORM:
                    for (int i = 0; i < 4; i++)
                        c[i] = float((lo >> (8 * i)) & 0xFF) / 255.0f;
                    break;
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
                    c[0] = float(lo & 0x3FF) / 1023.0f;
                    c[1] = float((lo >> 10) & 0x3FF) / 1023.0f;
                    c[2] = float((lo >> 20) & 0x3FF) / 1023.0f;
                    c[3] = float(lo >> 30) / 3.0f;
                    break;
                case VK_FORMAT_R16G16_SFLOAT:
                    c[0] = half(lo & 0xFFFF);
                    c[1] = half(lo >> 16);
                    break;
                case VK_FORMAT_R16G16B16A16_SFLOAT:
                    c[0] = half(lo & 0xFFFF);
                    c[1] = half(lo >> 16);
                    c[2] = half(hi & 0xFFFF);
                    c[3] = half(hi >> 16);
                    break;
                case VK_FORMAT_R32_SFLOAT:
                    memcpy(&c[0], &lo, 4);
                    break;
                case VK_FORMAT_R32G32_SFLOAT:
                    memcpy(&c[0], &lo, 4);
                    memcpy(&c[1], &hi, 4);
                    break;
                default:
                    for (int i = 0; i < 4; i++)
                        c[i] = 0.0f;
                    break;
                }
            }
            // `r` is in guest pixels; the clear covers its host pixels.
            int32_t x0 = rt->scale.x.EdgeAt(std::clamp(r.x0, 0, int32_t(rt->guestWidth)));
            int32_t y0 = rt->scale.y.EdgeAt(std::clamp(r.y0, 0, int32_t(rt->guestHeight)));
            int32_t x1 = rt->scale.x.EdgeAt(std::clamp(r.x1, 0, int32_t(rt->guestWidth)));
            int32_t y1 = rt->scale.y.EdgeAt(std::clamp(r.y1, 0, int32_t(rt->guestHeight)));
            x1 = std::max(x1, x0);
            y1 = std::max(y1, y0);
            if (x1 <= x0 || y1 <= y0)
                return;
            // Deferred (see s_pendingClears); a pass open on the target now
            // must end so the clear lands before its next use.
            if (s_passActive && (s_passDepth == rt || std::find(s_passColor, s_passColor + 4, rt) != s_passColor + 4))
                EndPass(kBreakClear);
            VkRect2D rect{ { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
            // NFSMW_CLEAR_FLUSH_RECT: tile bands left pending by the resolves
            // between them become one clear (a load-op clear when it covers the
            // next pass). Only with the target's newest pending clear, so no
            // clear of the target lies between the two (values compared as
            // bytes: `ca` is zeroed, so the bytes a format leaves unused agree).
            if (s_clearFlushRect)
                for (size_t i = s_pendingClears.size(); i-- > 0;)
                    if (s_pendingClears[i].rt == rt)
                    {
                        if (!memcmp(&s_pendingClears[i].value, &ca.clearValue, sizeof(VkClearValue)) &&
                            Adjoins(s_pendingClears[i].rect, rect))
                        {
                            rect = Union(s_pendingClears[i].rect, rect);
                            s_pendingClears.erase(s_pendingClears.begin() + i);
                        }
                        break;
                    }
            // A newer clear covering an older one replaces it.
            std::erase_if(s_pendingClears, [&](const PendingClear& pc) { return pc.rt == rt && Contains(rect, pc.rect); });
            s_pendingClears.push_back({ rt, rect, ca.clearValue });
        }

        // NFSMW_LOG_SHADOWMAP=1 (diagnostic): every 5 s, the first depth
        // resolve from a surface of pitch 1600 (the sun's shadow map) is
        // copied back after it, and a [shadowmap] line gives the share of its
        // texels that are exactly 0 (nearest) and exactly 0xFFFFFF (far: the
        // clear, no caster there). Before d24-fix every far texel read 0
        // (depth24_glsl.h). A GPU copy into host memory: guest memory may be
        // the GPU's copy in VRAM (shadow mode), uncached to the CPU. Once a
        // swap finds the copy's submission done, a thread of its own counts
        // the 2.56M texels. (Counted at the swap, on the command processor,
        // they made the next frame late: after every sample on the Steam
        // Machine, 3-5 ms more CP time and a ~20 ms frame.) The command
        // processor still records the copy (10 MB) and its two barriers
        // every 5 s, so time perf with the probe off. Off (the default): a
        // constant tested per resolve and per swap, nothing allocated.
        const bool s_logShadowMap = [] { const char* v = std::getenv("NFSMW_LOG_SHADOWMAP"); return v && v[0] && v[0] != '0'; }();
        struct ShadowMapProbe
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            const uint8_t* host = nullptr;
            bool coherent = true;
            VkDeviceSize size = 0;
            bool failed = false;
            uint64_t submission = 0, frame = 0;  // the copy's submission, and the frame
            double seconds = 0;
            uint32_t rangeBase = 0, rangeBytes = 0, destBase = 0, destPitch = 0, endian = 0, format = 0, srcBase = 0;
            Rect rect{};
            std::chrono::steady_clock::time_point next{};
        };
        std::unique_ptr<ShadowMapProbe> s_shadowMapProbe;
        std::chrono::steady_clock::time_point s_shadowMapClock{};  // the first swap
        // Idle: a resolve may record a copy. Recorded: the copy waits for its
        // submission. Counting: the counting thread reads the buffer, and the
        // command processor neither refills nor reallocates it until the
        // thread sets idle (release; the resolve's check acquires). Static
        // storage: a count still running at exit outlives s_shadowMapProbe.
        enum : uint8_t { kProbeIdle, kProbeRecorded, kProbeCounting };
        std::atomic<uint8_t> s_shadowMapState{ kProbeIdle };

        // What the counting thread needs, copied: it reads the buffer only.
        struct ShadowMapSample
        {
            const uint8_t* host;
            uint32_t rangeBase, rangeBytes, destBase, destPitch, endian, format, srcBase;
            Rect rect;
            uint64_t frame;
            double seconds;
        };

        void ShadowMapCount(ShadowMapSample p)
        {
            SetHostThreadName("nfsmw-shadowmap");
            hostcpu::LeaveReservedCore();  // started by the command processor
#ifdef __APPLE__
            pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);  // (not the CP's interactive class)
#endif
            // By 32x32 tiles: 32-bit texels make a tile 4 KB of consecutive
            // bytes (Tiled2D), so a tile wholly inside the rectangle is read
            // in order; the rest texel by texel.
            // (farTexels, not "far": <windows.h> defines far and near as nothing.)
            uint64_t texels = 0, zero = 0, farTexels = 0, outside = 0;
            auto count = [&](uint32_t offset) {
                if (uint64_t(offset) + 4 > p.rangeBytes)
                {
                    outside++;
                    return;
                }
                uint32_t word;
                memcpy(&word, p.host + offset, 4);
                uint32_t d = GpuSwap(word, p.endian >= 4 ? 2 : p.endian) >> 8;
                texels++;
                zero += d == 0;
                farTexels += d == 0xFFFFFF;
            };
            auto offsetOf = [&](uint32_t x, uint32_t y) { return ((p.destBase + Tiled2D(x, y, p.destPitch, 2)) & 0x1FFFFFFF) - p.rangeBase; };
            const uint32_t x0 = uint32_t(p.rect.x0), y0 = uint32_t(p.rect.y0), x1 = uint32_t(p.rect.x1), y1 = uint32_t(p.rect.y1);
            for (uint32_t ty = y0 >> 5; ty <= (y1 - 1) >> 5; ty++)
                for (uint32_t tx = x0 >> 5; tx <= (x1 - 1) >> 5; tx++)
                {
                    uint32_t ax = std::max(x0, tx * 32), bx = std::min(x1, tx * 32 + 32);
                    uint32_t ay = std::max(y0, ty * 32), by = std::min(y1, ty * 32 + 32);
                    if (bx - ax == 32 && by - ay == 32)
                        for (uint32_t i = 0, base = offsetOf(tx * 32, ty * 32); i < 1024; i++)
                            count(base + i * 4);
                    else
                        for (uint32_t y = ay; y < by; y++)
                            for (uint32_t x = ax; x < bx; x++)
                                count(offsetOf(x, y));
                }
            auto pct = [&](uint64_t n) { return texels ? 100.0 * double(n) / double(texels) : 0.0; };
            fprintf(stderr, "[shadowmap] frame %llu (%.1f s): depth resolve b%u p1600 fmt %u rect (%d,%d)-(%d,%d) -> %08X: %llu texels, "
                "zero %.2f%%, far %.2f%%%s\n", (unsigned long long)p.frame, p.seconds, p.srcBase, p.format, p.rect.x0, p.rect.y0,
                p.rect.x1, p.rect.y1, p.destBase, (unsigned long long)texels, pct(zero), pct(farTexels),
                outside ? std::format(" ({} texels outside the copy)", outside).c_str() : "");
            s_shadowMapState.store(kProbeIdle, std::memory_order_release);
        }

        void ShadowMapProbeRecord(uint32_t rangeBase, uint32_t rangeBytes, const ResolveConstants& rc, const Rect& r, uint32_t srcBase)
        {
            if (!s_shadowMapProbe)
                s_shadowMapProbe = std::make_unique<ShadowMapProbe>();
            ShadowMapProbe& p = *s_shadowMapProbe;
            auto now = std::chrono::steady_clock::now();
            if (p.failed || s_shadowMapState.load(std::memory_order_acquire) != kProbeIdle || now < p.next || rangeBytes == 0)
                return;
            if (p.size < rangeBytes)
            {
                if (p.buffer)
                {
                    vkDestroyBuffer(s_dev, p.buffer, nullptr);
                    vkFreeMemory(s_dev, p.memory, nullptr);
                    p.buffer = VK_NULL_HANDLE;
                    p.memory = VK_NULL_HANDLE;
                    p.host = nullptr;
                    p.size = 0;
                }
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = rangeBytes;
                bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VkMemoryRequirements req{};
                VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                void* mapped = nullptr;
                const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
                bool ok = vkCreateBuffer(s_dev, &bci, nullptr, &p.buffer) == VK_SUCCESS;
                if (ok)
                {
                    vkGetBufferMemoryRequirements(s_dev, p.buffer, &req);
                    mai.allocationSize = req.size;
                    // Cached host memory when there is some (the CPU reads the
                    // copy), else any host-visible coherent type. (Inline: the
                    // memory plan, gpu/memory_plan.h, replaced the renderer's
                    // FindMemoryTypePreferring; the windows branch does the same.)
                    const VkMemoryPropertyFlags cached = visible | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
                    mai.memoryTypeIndex = UINT32_MAX;
                    for (uint32_t i = 0; i < s_memProps.memoryTypeCount && mai.memoryTypeIndex == UINT32_MAX; i++)
                        if ((req.memoryTypeBits & (1u << i)) && (s_memProps.memoryTypes[i].propertyFlags & cached) == cached)
                            mai.memoryTypeIndex = i;
                    if (mai.memoryTypeIndex == UINT32_MAX)
                        mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, visible | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                    ok = (s_memProps.memoryTypes[mai.memoryTypeIndex].propertyFlags & visible) == visible &&
                        vkAllocateMemory(s_dev, &mai, nullptr, &p.memory) == VK_SUCCESS;
                }
                ok = ok && vkBindBufferMemory(s_dev, p.buffer, p.memory, 0) == VK_SUCCESS &&
                    vkMapMemory(s_dev, p.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS;
                if (!ok)
                {
                    fprintf(stderr, "[shadowmap] no host-visible buffer for the copy (%u bytes): off\n", rangeBytes);
                    if (p.buffer)
                        vkDestroyBuffer(s_dev, p.buffer, nullptr);
                    if (p.memory)
                        vkFreeMemory(s_dev, p.memory, nullptr);
                    p.buffer = VK_NULL_HANDLE;
                    p.memory = VK_NULL_HANDLE;
                    p.failed = true;
                    return;
                }
                p.host = static_cast<const uint8_t*>(mapped);
                p.size = rangeBytes;
                p.coherent = (s_memProps.memoryTypes[mai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            }
            // The resolve's writes -> the copy -> the host's reads (after
            // the submission's fence).
            VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            VkBufferCopy copy{ rangeBase, 0, rangeBytes };
            vkCmdCopyBuffer(s_cmd, s_shared, p.buffer, 1, &copy);
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            s_shadowMapState.store(kProbeRecorded, std::memory_order_relaxed);
            p.submission = s_submissionCounter;
            p.frame = s_frame;
            p.seconds = s_shadowMapClock.time_since_epoch().count()
                ? std::chrono::duration<double>(now - s_shadowMapClock).count() : 0.0;
            p.rangeBase = rangeBase;
            p.rangeBytes = rangeBytes;
            p.destBase = rc.destBase;
            p.destPitch = rc.destPitch;
            p.endian = rc.endian;
            p.format = rc.format;
            p.srcBase = srcBase;
            p.rect = r;
            p.next = now + std::chrono::seconds(5);
        }

        // At a swap: the recorded copy, once its submission is done, goes to
        // a counting thread (ShadowMapCount).
        void ShadowMapProbeOnSwap()
        {
            if (!s_shadowMapClock.time_since_epoch().count())
                s_shadowMapClock = std::chrono::steady_clock::now();
            if (!s_shadowMapProbe || s_shadowMapState.load(std::memory_order_relaxed) != kProbeRecorded)
                return;
            ShadowMapProbe& p = *s_shadowMapProbe;
            {
                std::lock_guard lock(s_submitMutex);
                bool done = p.submission < s_submissionCounter &&
                    (s_inFlight.empty() || s_submissions[s_inFlight.front()].number > p.submission);
                if (!done)
                    return;
            }
            if (!p.coherent)
            {
                VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, p.memory, 0, VK_WHOLE_SIZE };
                vkInvalidateMappedMemoryRanges(s_dev, 1, &range);
            }
            s_shadowMapState.store(kProbeCounting, std::memory_order_relaxed);
            std::thread(ShadowMapCount, ShadowMapSample{ p.host, p.rangeBase, p.rangeBytes, p.destBase, p.destPitch, p.endian, p.format,
                p.srcBase, p.rect, p.frame, p.seconds }).detach();
        }

        // NFSMW_RESOLVE_DEFER=1 (shadow mode; on by default on x86-64 Linux,
        // where the Steam Deck measured it with the fewer and lighter passes,
        // see kFewerPasses; NFSMW_RESOLVE_DEFER=0 turns it off): a depth resolve
        // into a cached texture of exactly its surface (Resolve's exact
        // fusion), at 1x, whose target the same resolve then clears whole,
        // stores its texels into that texture only (TEXTURE_ONLY). Its guest
        // bytes go into the shadow later, from the depth as resolved, and only
        // if something reads them: GPU work (EnsureShadow), the CPU (a fault
        // on its pages asks for them, see writewatch::DeferWrite), another
        // resolve into part of them. The same resolve again (the next frame's)
        // replaces them unwritten. The depth as resolved is the target's
        // image: the record keeps it, and the target goes on in another of its
        // format and size (the clear covers all of it). The race resolves the
        // second of its two 1600x1600 sun shadow maps so each frame, and
        // nothing reads its guest bytes (the first isn't: copy control 0x004,
        // no clear, the second is drawn over its depth; 0x204 clears). On the
        // Steam Deck, where the shadow is host memory, that resolve's dispatch
        // took ~500 us, deferred ~305.
        // NFSMW_RESOLVE_DEFER=2 (a check): that, and a thread of its own reads a
        // page of a deferred resolve's guest bytes every second as the CPU
        // would (a fault: the write watch asks for them and waits) and logs
        // what it found ([defer-check]: words whose stencil byte isn't 0 were
        // never a depth resolve's).
#if defined(__linux__) && defined(__x86_64__)
        constexpr int kResolveDeferDefault = 1;
#else
        constexpr int kResolveDeferDefault = 0;
#endif
        const int s_resolveDeferMode = [] {
            const char* v = std::getenv("NFSMW_RESOLVE_DEFER");
            return v ? std::clamp(std::atoi(v), 0, 2) : kResolveDeferDefault;
        }();
        const bool s_resolveDefer = s_resolveDeferMode != 0;
        // NFSMW_RESOLVE_DEFER_COLOR=1 (shadow mode; on by default on x86-64
        // Linux as NFSMW_RESOLVE_DEFER: a Steam Deck at 5 W, 5 alternated
        // rounds, +1.7% fps, GPU busy -0.43 ms a frame, the pictures within
        // compare_shots.py --strict; =0 turns it off): a colour
        // resolve into a cached texture (the exact fusion or a part of one,
        // NFSMW_FUSE_PARTIAL, NFSMW_DIRECT_CUBE), at 1x, of raw 32-bit texels
        // (no conversion), whose rectangle lies in the texture whole, stores
        // into the texture only too. Its guest bytes are written back from the
        // texture when something reads them (TEXTURE_WRITEBACK: a texel is the
        // guest word in the texture's endianness), so the target needs no
        // keeping. A later colour resolve into the same texture leaves the
        // owed ones owed (the texture holds the latest texels of all of
        // them), and those its rectangle covers no longer owe anything. The
        // texture stays cached while it holds owed texels. In the race 90% of
        // the resolved bytes were written over unread (NFSMW_RESOLVE_USE),
        // ~9.3 MB a frame of them such colour resolves: the scene into its
        // texture (whole, in strips or halves), the six environment cube
        // faces, the bloom chain.
        // NFSMW_RESOLVE_DEFER_COLOR=2 (a check): such resolves aren't deferred,
        // but after each (guest bytes and texels stored as usual) a
        // WRITEBACK_CHECK pass compares every word with the one written back
        // from the texture would be, and the write-back runs too (the same
        // words again): [defer-check] colour lines count the words that differ.
        const int s_resolveDeferColorMode = [] {
            const char* v = std::getenv("NFSMW_RESOLVE_DEFER_COLOR");
            return v ? std::clamp(std::atoi(v), 0, 2) : kResolveDeferDefault;
        }();
        const bool s_resolveDeferColor = s_resolveDeferColorMode == 1;
        // NFSMW_RESOLVE_LAZY=1 (by default on x86-64 Linux, as
        // NFSMW_RESOLVE_DEFER; 0 turns it off): more resolves into cached
        // textures store the texture only, their guest bytes written back
        // from it when something reads them, as NFSMW_RESOLVE_DEFER_COLOR's,
        // where the CPU hasn't locked that texture lately (gpu/cpu_locks.h:
        // a read every frame, the exposure's 64x64, would only wait):
        // - depth ones without a clear (shadow map 1, which map 2 is drawn
        //   over; the scene's depth strips): the texture holds float(d24) /
        //   16777215, the word main() writes is d24 << 8 (its stencil byte
        //   0), so the write-back recovers it (in the same endianness only);
        // - colour ones whose rectangle runs past the texture by its 8-pixel
        //   alignment only (the bloom chain's 320x180 and 160x90 at 184 and
        //   96 rows: D3D resolves those surfaces whole, the hardware's
        //   rectangle is 8-pixel aligned): the words past it, outside the
        //   texture, stay as they were. Nothing reads them in the race
        //   (NFSMW_RESOLVE_USE: those resolves' bytes are all dead); the
        //   check below doesn't compare them.
        // NFSMW_RESOLVE_LAZY=2 checks it as NFSMW_RESOLVE_DEFER_COLOR=2 does:
        // stored as before, then compared with what the write-back writes.
        const int s_hleLazyMode = [] {
            const char* v = std::getenv("NFSMW_RESOLVE_LAZY");
            return v ? std::clamp(std::atoi(v), 0, 2) : kResolveDeferDefault;
        }();
        const bool s_hleLazy = s_hleLazyMode == 1;
        // NFSMW_RESOLVE_RENAME=1 (with NFSMW_RESOLVE_DEFER; by default on x86-64
        // Linux, 0 turns it off): a
        // depth resolve of a whole target with its clear, into a cached
        // texture exactly the target's size (shadow map 2), renames the
        // target's image instead of copying it: NFSMW_RESOLVE_DEFER already
        // hands the target a fresh image (the clear's) and keeps the old one
        // for the guest bytes; now the texture samples that one too (its
        // depth aspect), and the resolve's store into the texture is gone.
        // The texture's own texels are stale meanwhile: a load of it drops
        // the rename, a resolve into it that doesn't rename first copies
        // them over (MaterializeRename). Its depth is the target's as drawn,
        // not rounded to 24 bits as the store rounds it. Its views change
        // each frame: the cached lookups of that texture alone are made
        // again (ForgetTextureLookups).
        const bool s_hleRename = [] { const char* v = std::getenv("NFSMW_RESOLVE_RENAME"); return v ? v[0] == '1' : kResolveDeferDefault == 1; }();
        // A depth image going back to the spares (its deferred resolve
        // written, replaced or out of room): a texture renamed to it gets its
        // own texels first, else the next defer to take that spare as its
        // target would draw into what the texture samples. `except`: the
        // texture this very resolve renames again.
        void ReleaseRenamedImage(VkImage image, const Texture* except = nullptr);
        struct ColourCheck
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            volatile uint32_t* counts = nullptr;  // [0] words that differ, [1] words compared (coherent)
        };
        ColourCheck s_colourCheck;
        struct DeferCheckRange { uint32_t base, bytes, endian; };
        std::mutex s_deferCheckMutex;
        std::vector<DeferCheckRange> s_deferCheckRanges;  // the destinations deferred so far (8 at most)
        void DeferCheck()
        {
            SetHostThreadName("nfsmw-defercheck");
            hostcpu::LeaveReservedCore();
            for (uint32_t round = 0;; round++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                DeferCheckRange c;
                {
                    std::lock_guard lock(s_deferCheckMutex);
                    c = s_deferCheckRanges[round % s_deferCheckRanges.size()];
                }
                // A page from the middle of the range: the shadow map's caster
                // area is there more often than its edges.
                uint32_t base = (c.base + c.bytes / 2) & ~4095u;
                auto start = std::chrono::steady_clock::now();
                uint32_t words[1024];
                for (uint32_t i = 0; i < 1024; i++)
                    words[i] = *reinterpret_cast<volatile const uint32_t*>(s_sharedHost + base + i * 4);
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                uint32_t stencil = 0, farWords = 0;  // (not "far": <windows.h> defines it as nothing)
                for (uint32_t w : words)
                {
                    uint32_t d = GpuSwap(w, c.endian >= 4 ? 2 : c.endian);
                    stencil += (d & 0xFF) != 0;
                    farWords += (d >> 8) == 0xFFFFFF;
                }
                fprintf(stderr, "[defer-check] read %08X in %.2f ms: %u of 1024 words far, %u with a stencil byte%s\n", base, ms,
                    farWords, stencil, stencil ? " (NOT a depth resolve's bytes)" : "");
            }
        }
        struct DeferredResolve
        {
            uint32_t rangeBase, rangeBytes;  // the guest bytes it owes (DeferWrite'd)
            ResolveConstants rc;             // as resolved
            Image image;                      // the target's image as resolved
            VkImageView sampleView;           // its depth aspect
            VkFormat format;                  // the target's format and size
            uint32_t width, height;
            // A colour resolve's (NFSMW_RESOLVE_DEFER_COLOR; none of the
            // above): the cached texture, and the face, holding its texels.
            Texture* texture = nullptr;
            uint32_t layer = 0;
            // NFSMW_RESOLVE_PRESENT=2: the image is a front ring slot's (the
            // presenter shows it): the slot returns it to the spares, not this record.
            bool borrowed = false;
        };
        std::vector<DeferredResolve> s_deferredResolves;
        constexpr size_t kMaxDeferred = 64;  // more: the oldest is written (32 ran over ~0.5 times a frame once NFSMW_RESOLVE_PRESENT=2 added its two)
        constexpr size_t kMaxDeferredDepth = 4;  // of them depth ones (each keeps a target's image)
        // Images of depth targets that went into records and came back, for the
        // next target of their format and size that needs one.
        struct SpareTarget
        {
            Image image;
            VkImageView sampleView;
            VkFormat format;
            uint32_t width, height;
        };
        std::vector<SpareTarget> s_spareTargets;
        // Resolves deferred; replaced unwritten; written for GPU work, for the
        // CPU, for want of room (any thread reads them at exit).
        std::atomic<uint64_t> s_deferredCount{ 0 }, s_deferReplaced{ 0 }, s_deferForGpu{ 0 }, s_deferForCpu{ 0 }, s_deferForRoom{ 0 };
        std::atomic<uint64_t> s_deferredColour{ 0 }, s_deferForResolve{ 0 };  // (colour ones; written for a resolve not deferred)
        std::atomic<uint64_t> s_deferredHle{ 0 };  // (NFSMW_RESOLVE_LAZY ones)

        // Whether deferred colour resolve `b`'s rectangle in its texture holds
        // all of `a`'s (the same texture and face).
        bool CoversTexels(const ResolveConstants& b, const ResolveConstants& a)
        {
            const int64_t bx = int64_t(b.originX) + b.texX, by = int64_t(b.originY) + b.texY;
            const int64_t ax = int64_t(a.originX) + a.texX, ay = int64_t(a.originY) + a.texY;
            return bx <= ax && by <= ay && bx + b.width >= ax + a.width && by + b.height >= ay + a.height;
        }

        // A resolve shader pass over `rc`'s rectangle (8x8 groups): `src` its
        // source, `store` its texture binding, `pool` its pool binding.
        void DispatchResolvePass(VkPipeline pipeline, const ResolveConstants& rc, VkImageView src, VkImageView store, VkDescriptorBufferInfo pool,
            bool tiles = false)  // (tiles: TEXTURE_ONLY's groups, a 32x32 tile each)
        {
            VkDescriptorImageInfo ii{ VK_NULL_HANDLE, src, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo ti{ VK_NULL_HANDLE, store, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorBufferInfo table = DummyStorage(s_scaled.table);
            VkWriteDescriptorSet w[8]{};
            w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr };
            w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
                VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii, nullptr, nullptr };
            w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pool, nullptr };
            w[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 3, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &table, nullptr };
            w[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 4, 0, 1,
                VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ti, nullptr, nullptr };
            ComputeWorkRecorded();
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_resolveLayout, 0, AddSharedParts(w, 5), w);
            vkCmdPushConstants(s_cmd, s_resolveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, s_resolvePush, &rc);
            if (tiles)
                vkCmdDispatch(s_cmd, ((rc.originX + int32_t(rc.width) - 1) >> 5) - (rc.originX >> 5) + 1,
                    ((rc.originY + int32_t(rc.height) - 1) >> 5) - (rc.originY >> 5) + 1, 1);
            else
                vkCmdDispatch(s_cmd, (rc.width + 7) / 8, (rc.height + 7) / 8, 1);
        }

        // Records deferred resolve `i`'s guest bytes and takes it off the
        // list: a depth one's from its depth (the main resolve shader, guest
        // memory only; its image becomes a spare), a colour one's from its
        // texture (TEXTURE_WRITEBACK).
        void WriteDeferred(size_t i)
        {
            DeferredResolve d = s_deferredResolves[i];
            s_deferredResolves.erase(s_deferredResolves.begin() + ptrdiff_t(i));
            EndPass(kBreakResolve);
            Begin();
            const bool colour = d.texture != nullptr;
            ResolveConstants rc = d.rc;
            rc.texWrite = 0;
            if (colour && !s_resolveWritebackPipeline)
                MakeResolvePipeline(false, 1, 1, &s_resolveWritebackPipeline, false, 1);
            if (s_profiling)
                ProfileCompute(std::format("deferred resolve {}x{} to {:08X}: its guest bytes{}", rc.width, rc.height, rc.destBase,
                    colour ? ", from its texture" : "").c_str());
            DispatchResolvePass(colour ? s_resolveWritebackPipeline : s_resolvePipeline,  // (1x only are deferred)
                rc, colour ? s_dummy[0].view : d.sampleView, colour ? d.texture->storeViews[d.layer] : s_dummyStorage.view,
                DummyStorage(s_scaled.pool));
            ResolveBarrier(d.rangeBase, d.rangeBytes, 0);
            if (s_hleCounting)
                s_hle.writebackBytes += (uint64_t(rc.width) * rc.height) << ResolveBppLog2(rc.format);
            ProfileComputeEnd();
            // Guarded as the resolve's own write would have been (its pages
            // stayed GPU-owned), and only then no longer owed: a CPU access
            // waiting for it (WaitForDeferred) waits for this write next.
            GuardGpu(d.rangeBase, d.rangeBytes, true, "resolve");
            writewatch::MarkGpuOwned(d.rangeBase, d.rangeBytes);
            writewatch::DeferredWritten(d.rangeBase, d.rangeBytes);
            if (!colour && !d.borrowed)
            {
                ReleaseRenamedImage(d.image.image);
                s_spareTargets.push_back({ d.image, d.sampleView, d.format, d.width, d.height });
            }
            LogOnce(0x3300, "deferred resolve to %08X (%ux%u) written: something reads its guest bytes", rc.destBase, rc.width, rc.height);
        }

        // NFSMW_RESOLVE_DEFER_COLOR=2: after a colour resolve that could have
        // been deferred, its words compared with the write-back's
        // (WRITEBACK_CHECK, into s_colourCheck), and the write-back itself.
        void CheckColourWriteback(const ResolveConstants& resolved, Texture* t, uint32_t layer, uint32_t rangeBase, uint32_t rangeBytes)
        {
            if (!s_colourCheck.buffer)
            {
                VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
                bci.size = 256;
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                Check(vkCreateBuffer(s_dev, &bci, nullptr, &s_colourCheck.buffer), "colour check buffer");
                VkMemoryRequirements req;
                vkGetBufferMemoryRequirements(s_dev, s_colourCheck.buffer, &req);
                VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                mai.allocationSize = req.size;
                mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                Check(vkAllocateMemory(s_dev, &mai, nullptr, &s_colourCheck.memory), "colour check memory");
                Check(vkBindBufferMemory(s_dev, s_colourCheck.buffer, s_colourCheck.memory, 0), "colour check bind");
                void* mapped = nullptr;
                Check(vkMapMemory(s_dev, s_colourCheck.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "colour check map");
                s_colourCheck.counts = static_cast<volatile uint32_t*>(mapped);
                s_colourCheck.counts[0] = 0;
                s_colourCheck.counts[1] = 0;
                for (int i = 2; i < 12; i++)
                    s_colourCheck.counts[i] = 0;
                MakeResolvePipeline(false, 1, 1, &s_resolveWritebackCheckPipeline, false, 2);
                if (!s_resolveWritebackPipeline)
                    MakeResolvePipeline(false, 1, 1, &s_resolveWritebackPipeline, false, 1);
                LogOnce(0x3500, "NFSMW_RESOLVE_DEFER_COLOR=2: colour resolves into cached textures compared with their write-back");
            }
            EndPass(kBreakResolve);
            Begin();
            ResolveConstants rc = resolved;
            rc.texWrite = 0;
            DispatchResolvePass(s_resolveWritebackCheckPipeline, rc, s_dummy[0].view, t->storeViews[layer], { s_colourCheck.buffer, 0, 256 });
            FullBarrier();
            DispatchResolvePass(s_resolveWritebackPipeline, rc, s_dummy[0].view, t->storeViews[layer], DummyStorage(s_scaled.pool));
            ResolveBarrier(rangeBase, rangeBytes, t->guestStart);
        }

        void ReportColourCheck(const char* when)
        {
            if (s_colourCheck.counts)
                fprintf(stderr, "[defer-check] colour %s: %u words compared with their write-back from the texture, %u differ%s; "
                    "largest depth difference %u (24-bit units)\n", when, s_colourCheck.counts[1], s_colourCheck.counts[0],
                    s_colourCheck.counts[0] ? " (NOT exact)" : "", s_colourCheck.counts[2]);
            if (s_colourCheck.counts && s_colourCheck.counts[3])
                fprintf(stderr, "[defer-check] first: texel %08X guest %08X written back %08X at %u,%u dest %08X convert %u endian/tex/format %06X\n",
                    s_colourCheck.counts[4], s_colourCheck.counts[5], s_colourCheck.counts[6], s_colourCheck.counts[7], s_colourCheck.counts[8],
                    s_colourCheck.counts[9], s_colourCheck.counts[10], s_colourCheck.counts[11]);
        }

        // Before work that reads guest bytes [base, base + size): the deferred
        // resolves owing any of them write them.
        void WriteDeferredIn(uint32_t base, uint32_t size)
        {
            for (size_t i = s_deferredResolves.size(); i-- > 0;)
            {
                const DeferredResolve& d = s_deferredResolves[i];
                if (uint64_t(d.rangeBase) < uint64_t(base) + size && uint64_t(base) < uint64_t(d.rangeBase) + d.rangeBytes)
                {
                    s_deferForGpu++;
                    WriteDeferred(i);
                }
            }
        }

        // A CPU access waits for deferred bytes (writewatch::g_writeDeferred):
        // all are written (which pages it wants isn't known here).
        void WriteDeferredForCpu()
        {
            if (!writewatch::g_writeDeferred.exchange(false))
                return;
            while (!s_deferredResolves.empty())
            {
                s_deferForCpu++;
                WriteDeferred(s_deferredResolves.size() - 1);
            }
        }

        // The target goes on in another image of its format and size (a spare,
        // or a new one); its old one is returned. Undefined contents: the
        // caller's clear covers all of it.
        std::pair<Image, VkImageView> SwapTargetImage(RenderTarget& rt)
        {
            std::pair<Image, VkImageView> old{ rt.image, rt.sampleView };
            auto it = std::find_if(s_spareTargets.begin(), s_spareTargets.end(), [&](const SpareTarget& s) {
                return s.format == rt.vkFormat && s.width == rt.width && s.height == rt.height;
            });
            if (it != s_spareTargets.end())
            {
                rt.image = it->image;
                rt.sampleView = it->sampleView;
                s_spareTargets.erase(it);
                return old;
            }
            // (A colour target too, NFSMW_RESOLVE_PRESENT=2: its view samples it as is.)
            const VkImageAspectFlags aspect = rt.depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            rt.image = CreateImage(VK_IMAGE_TYPE_2D, rt.vkFormat, { rt.width, rt.height, 1 }, 1, VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (rt.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
                VK_IMAGE_VIEW_TYPE_2D, aspect);
            if (!rt.depth)
                rt.sampleView = rt.image.view;
            else
            {
                VkImageViewCreateInfo vci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                vci.image = rt.image.image;
                vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
                vci.format = rt.vkFormat;
                vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                Check(vkCreateImageView(s_dev, &vci, nullptr, &rt.sampleView), "vkCreateImageView(depth)");
            }
            EndPass(kBreakResolve);
            Begin();  // (the resolve may have submitted: SubmitIfStarving)
            ToGeneral(rt.image.image, aspect, 1);
            return old;
        }

        // NFSMW_RESOLVE_RENAME: texture `t` samples its own image again, its
        // texels stored first from the renamed one as the resolve would have
        // stored them (TEXTURE_ONLY with that resolve's constants).
        struct RenameSource { ResolveConstants rc; VkImageView depthView; };
        std::unordered_map<Texture*, RenameSource> s_renameSources;
        void MaterializeRename(Texture& t)
        {
            if (!t.renamedImage)
                return;
            if (auto it = s_renameSources.find(&t); it != s_renameSources.end())
            {
                if (!s_resolveTexturePipeline)
                    MakeResolvePipeline(false, 1, 1, &s_resolveTexturePipeline, true);
                EndPass(kBreakResolve);
                Begin();
                const ResolveConstants& rc = it->second.rc;
                if (s_profiling)
                    ProfileCompute(std::format("renamed {}x{} at {:08X}: its texels stored back (NFSMW_RESOLVE_RENAME)", rc.width, rc.height,
                        t.guestStart).c_str());
                DispatchResolvePass(s_resolveTexturePipeline, rc, it->second.depthView, t.storeViews[0], DummyStorage(s_scaled.pool), true);
                ResolveBarrier(0, 0, t.guestStart);
                ProfileComputeEnd();
                s_renameMaterialized++;
                s_renameSources.erase(it);
            }
            t.renamedImage = VK_NULL_HANDLE;
            ForgetTextureLookups(&t);
        }

        void ReleaseRenamedImage(VkImage image, const Texture* except)
        {
            std::vector<Texture*> renamed;
            for (auto& [t, source] : s_renameSources)
                if (t != except && t->renamedImage == image)
                    renamed.push_back(t);
            for (Texture* t : renamed)
                MaterializeRename(*t);
        }

        void ForgetRenamedImage(VkImage image)
        {
            if (!image)
                return;
            for (auto it = s_renameViews.begin(); it != s_renameViews.end();)
                if (it->first.first == image)
                {
                    vkDestroyImageView(s_dev, it->second, nullptr);
                    it = s_renameViews.erase(it);
                }
                else
                    ++it;
            for (auto it = s_renameSources.begin(); it != s_renameSources.end();)
                if (it->first->renamedImage == image)
                {
                    // (Its own texels are stale: the next resolve into it
                    // stores them, every frame for shadow map 2.)
                    it->first->renamedImage = VK_NULL_HANDLE;
                    ForgetTextureLookups(it->first);
                    s_renameDrops++;
                    it = s_renameSources.erase(it);
                }
                else
                    ++it;
        }

        // NFSMW_RESOLVE_PRESENT=1 (=2 by default on x86-64 Linux, see
        // s_hlePresentMode below; 0 turns it off; Steam Deck at 5 W, the three
        // together: -1.4 ms GPU a frame, +6% fps, the same pictures): the resolve into the front
        // buffer, a whole 1x one of 32-bit texels into a front buffer a swap
        // presented lately, also stores the picture into the presenter's
        // front image, as the swap's conversion would from those guest bytes
        // (FRONT_DIRECT): the swap then presents that image as it is, without
        // its conversion (a read of the front buffer's guest bytes and a
        // dispatch; its full barrier moves to the resolve). The guest bytes
        // are still written: the front image can't give them back (its alpha
        // is 1). Claimed at the resolve, the image takes back a frame waiting
        // for the presenter a moment before the swap would. A front buffer
        // written again before the swap (a resolve, the CPU), or presented
        // in another format, takes the conversion as before.
        // NFSMW_RESOLVE_PRESENT=2 (the default where =1 is): a resolve into
        // the front buffer that also clears its target, of a whole 8888
        // target at least the front image's size, runs nothing: the target's
        // image itself goes to the presenter (the front ring's slot holds it
        // until the presenter is done with it, then it becomes a spare
        // target image), the target goes on in another image (its clear
        // covers the frame), and the guest bytes are owed as a deferred
        // resolve from that image (written only if something reads them).
        // Saves the resolve's read, the front image's store and the guest
        // write, ~3 x 3.7 MB a frame: on a Steam Deck at 5 W -0.58 ms of GPU
        // time a frame (3 alternated pairs: -0.58, -0.65, -0.50), the same
        // pictures (compare_shots --strict) and trace hash.
        const int s_hlePresentMode = [] {
            const char* v = std::getenv("NFSMW_RESOLVE_PRESENT");
            return v ? std::clamp(std::atoi(v), 0, 2) : kResolveDeferDefault ? 2 : 0;
        }();
        const bool s_hlePresent = s_hlePresentMode != 0;
        struct FrontSeen { uint32_t base = 0, pitch = 0, width = 0, height = 0, endian = 0, format = 0, swizzle = 0; uint64_t frame = 0; };
        FrontSeen s_frontSeen[4];  // the last front buffers presented (tiled, 1x), by base
        // (And what it was stored as: a front buffer written since, by a
        // resolve or the CPU, or presented otherwise, takes the conversion.)
        struct FrontDirect
        {
            bool pending = false;
            uint32_t base = 0;
            uint64_t frame = 0;
            VkImage image = VK_NULL_HANDLE;
            uint64_t written = 0;  // the resolve's write sequence
            uint32_t rangeBase = 0, rangeBytes = 0;
            FrontSeen as;
        };
        FrontDirect s_frontDirect;  // a front image the resolve stored this frame's picture into
        VkPipeline s_resolveFrontPipeline = VK_NULL_HANDLE;  // FRONT_DIRECT (made at its first use)
        // The unscaled front image ring's next image, claimed now for a
        // resolve's FRONT_DIRECT store (false: no ring of that size yet).
        bool ClaimFrontForResolve(uint32_t width, uint32_t height, VkImage& image, VkImageView& view);
        // NFSMW_RESOLVE_PRESENT=2: the slot of the unscaled front ring the next
        // swap presents, its previous hand-over recycled (-1: no ring of that
        // size yet); then the image handed over into it.
        int ClaimFrontSlotForHandover(uint32_t width, uint32_t height);
        void HandOverFront(int slot, const Image& image, VkImageView view, VkFormat format, uint32_t width, uint32_t height);

        void Resolve()
        {
            s_stats.resolves++;
            s_hle.resolves += s_hleCounting;
            auto copyControl = Reg<reg::RB_COPY_CONTROL>();
            auto surfaceInfo = Reg<reg::RB_SURFACE_INFO>();
            uint32_t pitch = surfaceInfo.surface_pitch;
            if (pitch == 0)
                return;

            // The rectangle comes from the three vertices in vertex fetch 0
            // (Direct3D 9 writes them with the CPU).
            uint32_t fetch0 = s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0];
            uint32_t fetch1 = s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_1];
            uint32_t vertexAddress = fetch0 & 0x1FFFFFFC;
            uint32_t vertexEndian = fetch1 & 3;
            if ((fetch0 & 3) != 3 || ((fetch1 >> 2) & 0xFFFFFF) != 6)
            {
                LogOnce(0x100, "resolve with unexpected vertex fetch constant %08X %08X", fetch0, fetch1);
                return;
            }
            float halfPixel = Reg<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
            int32_t v[6];
            for (int i = 0; i < 6; i++)
            {
                uint32_t bits = GpuSwap(LoadPhysical(vertexAddress + i * 4), vertexEndian);
                float f;
                memcpy(&f, &bits, 4);
                v[i] = ToFixed16p8(f + halfPixel);
            }
            Rect r;
            r.x0 = (std::min({ v[0], v[2], v[4] }) + 127) >> 8;
            r.y0 = (std::min({ v[1], v[3], v[5] }) + 127) >> 8;
            r.x1 = (std::max({ v[0], v[2], v[4] }) + 127) >> 8;
            r.y1 = (std::max({ v[1], v[3], v[5] }) + 127) >> 8;
            if (Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
            {
                auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
                r.x0 += wo.window_x_offset;
                r.x1 += wo.window_x_offset;
                r.y0 += wo.window_y_offset;
                r.y1 += wo.window_y_offset;
            }
            Rect sc = GuestScissor();
            r.x0 = std::clamp(r.x0, sc.x0, sc.x1);
            r.x1 = std::clamp(r.x1, sc.x0, sc.x1);
            r.y0 = std::clamp(r.y0, sc.y0, sc.y1);
            r.y1 = std::clamp(r.y1, sc.y0, sc.y1);
            r.x0 &= ~7;
            r.y0 &= ~7;
            r.x1 = (r.x1 + 7) & ~7;
            r.y1 = (r.y1 + 7) & ~7;
            r.x1 = std::min(r.x1, int32_t(pitch & ~7u));
            r.x0 = std::min(r.x0, r.x1);
            if (r.x1 <= r.x0 || r.y1 <= r.y0)
                return;

            bool depthSource = copyControl.copy_src_select >= 4;
            // NFSMW_LOG_RESOLVES=1: describe the first resolves in detail.
            // NFSMW_LOG_RESOLVES_PITCH=<p>: only resolves from surfaces of that pitch.
            static const bool logResolves = std::getenv("NFSMW_LOG_RESOLVES") != nullptr;
            static const int logPitch = [] { const char* v = std::getenv("NFSMW_LOG_RESOLVES_PITCH"); return v ? std::atoi(v) : -1; }();
            static uint32_t logged = 0;
            if ((logResolves || (logPitch >= 0 && pitch == uint32_t(logPitch))) && logged < 200)
            {
                logged++;
                auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
                fprintf(stderr, "[resolve] rect (%d,%d)-(%d,%d) wo (%d,%d) woEn %u src %u pitch %u msaa %u | dest %08X pitch %u h %u info %08X "
                    "(fmt %u endian %u swap %u bias %d) | ctl %08X color %08X depth %08X clear c%08X d%08X\n",
                    r.x0, r.y0, r.x1, r.y1, wo.window_x_offset, wo.window_y_offset,
                    uint32_t(Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable), copyControl.copy_src_select, pitch,
                    uint32_t(surfaceInfo.msaa_samples), s_regs[XE_GPU_REG_RB_COPY_DEST_BASE],
                    Reg<reg::RB_COPY_DEST_PITCH>().copy_dest_pitch, Reg<reg::RB_COPY_DEST_PITCH>().copy_dest_height,
                    s_regs[XE_GPU_REG_RB_COPY_DEST_INFO], uint32_t(Reg<reg::RB_COPY_DEST_INFO>().copy_dest_format),
                    uint32_t(Reg<reg::RB_COPY_DEST_INFO>().copy_dest_endian), uint32_t(Reg<reg::RB_COPY_DEST_INFO>().copy_dest_swap),
                    int(Reg<reg::RB_COPY_DEST_INFO>().copy_dest_exp_bias), copyControl.value, s_regs[XE_GPU_REG_RB_COLOR_INFO],
                    s_regs[XE_GPU_REG_RB_DEPTH_INFO], s_regs[XE_GPU_REG_RB_COLOR_CLEAR], s_regs[XE_GPU_REG_RB_DEPTH_CLEAR]);
                if (logPitch >= 0)
                    for (int i = 0; i < 6; i++)
                    {
                        uint32_t bits = GpuSwap(LoadPhysical(vertexAddress + i * 4), vertexEndian);
                        float f;
                        memcpy(&f, &bits, 4);
                        fprintf(stderr, "[resolve]   v%d.%c = %.1f\n", i / 2, "xy"[i & 1], f);
                    }
            }
            RenderTarget* source;
            if (depthSource)
            {
                auto di = Reg<reg::RB_DEPTH_INFO>();
                source = GetTarget(di.depth_base, uint32_t(di.depth_format), true, pitch);
            }
            else
            {
                static const uint32_t infoRegs[4] = { XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO,
                    XE_GPU_REG_RB_COLOR2_INFO, XE_GPU_REG_RB_COLOR3_INFO };
                auto ci = RegAt<reg::RB_COLOR_INFO>(infoRegs[copyControl.copy_src_select & 3]);
                source = GetTarget(ci.color_base, uint32_t(ci.color_format), false, pitch);
            }

            // Copy.
            auto destInfo = Reg<reg::RB_COPY_DEST_INFO>();
            auto destPitch = Reg<reg::RB_COPY_DEST_PITCH>();
            uint32_t destBase = s_regs[XE_GPU_REG_RB_COPY_DEST_BASE];
            uint32_t format = depthSource
                ? (xenos::DepthRenderTargetFormat(Reg<reg::RB_DEPTH_INFO>().depth_format) == xenos::DepthRenderTargetFormat::kD24FS8 ? 23u : 22u)
                : uint32_t(destInfo.copy_dest_format);
            bool copy = copyControl.copy_command == xenos::CopyCommand::kRaw ||
                copyControl.copy_command == xenos::CopyCommand::kConvert;
            if (copy && destBase != 0)
            {
                auto [extentStart, extentEnd] = ResolveExtent(r, (destPitch.copy_dest_pitch + 31) & ~31u, ResolveBppLog2(format));
                uint32_t rangeBase = (destBase & 0x1FFFFFFF) + extentStart, rangeBytes = extentEnd - extentStart;
                // NFSMW_RESOLVE_DEFER: a deferred resolve of these very bytes
                // (the same destination and rectangle, 32-bit: the next
                // frame's) is replaced unwritten. Its pages stay owed until
                // this one's write is guarded or owed itself (below), so a CPU
                // access in between waits for one of them.
                std::optional<DeferredResolve> replaced;
                if (ResolveFormatSupported(format) && ResolveBppLog2(format) == 2)
                    for (size_t i = 0; i < s_deferredResolves.size(); i++)
                    {
                        const ResolveConstants& o = s_deferredResolves[i].rc;
                        if (o.destBase == (destBase & 0x1FFFFFFF) && o.destPitch == ((destPitch.copy_dest_pitch + 31) & ~31u) &&
                            o.originX == r.x0 && o.originY == r.y0 && o.width == uint32_t(r.x1 - r.x0) && o.height == uint32_t(r.y1 - r.y0))
                        {
                            replaced = s_deferredResolves[i];
                            s_deferredResolves.erase(s_deferredResolves.begin() + ptrdiff_t(i));
                            break;
                        }
                    }
                // NFSMW_RESOLVE_DEFER_COLOR: deferred colour resolves this one
                // overlaps, whose texture holds this one's range too (the same
                // pitch): kept from EnsureShadow below. Should this one be
                // deferred into the same texture and face, they stay owed
                // (those it covers no longer owe anything once it does);
                // otherwise they are written before it writes guest memory.
                std::vector<DeferredResolve> held;
                if ((s_resolveDeferColor || s_hleLazy) && ResolveFormatSupported(format) && ResolveBppLog2(format) == 2)
                    for (size_t i = s_deferredResolves.size(); i-- > 0;)
                    {
                        const DeferredResolve& d = s_deferredResolves[i];
                        const Texture* t = d.texture;
                        if (t && uint64_t(d.rangeBase) < uint64_t(rangeBase) + rangeBytes && uint64_t(rangeBase) < uint64_t(d.rangeBase) + d.rangeBytes &&
                            rangeBase >= t->guestStart && uint64_t(rangeBase) + rangeBytes <= uint64_t(t->guestStart) + t->guestSize &&
                            d.rc.destPitch == ((destPitch.copy_dest_pitch + 31) & ~31u))
                        {
                            held.push_back(d);
                            s_deferredResolves.erase(s_deferredResolves.begin() + ptrdiff_t(i));
                        }
                    }
                ResolveUse* use = s_resolveUse && s_shadowMode && ResolveFormatSupported(format)
                    ? NoteResolveUse(rangeBase, rangeBytes, uint32_t(r.x1 - r.x0), uint32_t(r.y1 - r.y0), format, depthSource,
                        (uint64_t(r.x1 - r.x0) * uint32_t(r.y1 - r.y0)) << ResolveBppLog2(format))
                    : nullptr;
                // Shadow mode: pages the resolve covers only in part keep
                // the CPU's latest data around it.
                s_resolveUseQuiet = true;
                EnsureShadow(rangeBase, rangeBytes);
                s_resolveUseQuiet = false;
                if (destInfo.copy_dest_array)
                    LogOnce(0x200, "resolve to a 3D/array destination is not implemented (copying slice 0)");
                if (!ResolveFormatSupported(format))
                {
                    LogOnce(0x300 + format, "resolve to texture format %u not implemented", format);
                }
                else
                {
                    EndPass(kBreakResolve);
                    Begin();
                    // NFSMW_UNTILED_AREA: a resolve reading, inside what the
                    // untiled draws cut to their viewports could reach (their
                    // attachments' common size), past what they drew in the
                    // last frame untiled draws drew its source (a resolve
                    // after the swap reads that frame's), reads pixels the
                    // host used to overdraw (and the console never drew).
                    // None seen in menus, loading, pre-race or races; should
                    // one appear, that target's draws keep the whole target
                    // from then on, so it differs from before for this one
                    // frame. Blind: a cut inside another (their union is the
                    // larger). Needless (costing only the saving): a band only
                    // a clear covered, a target drawn since by non-untiled draws.
                    if (s_untiledArea && source->cut && !source->wholeArea && gpu::split_views::FallsBack(SplitFrame()))
                    {
                        auto [shiftX, shiftY] = TileShift();
                        int32_t x0 = source->scale.x.EdgeAt(r.x0 + shiftX);
                        int32_t y0 = source->scale.y.EdgeAt(r.y0 + shiftY);
                        int32_t x1 = std::min({ source->scale.x.EdgeAt(r.x1 + shiftX), int32_t(source->width), int32_t(source->cutReach.width) });
                        int32_t y1 = std::min({ source->scale.y.EdgeAt(r.y1 + shiftY), int32_t(source->height), int32_t(source->cutReach.height) });
                        VkRect2D read{ { x0, y0 }, { uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0)) } };
                        if (read.extent.width && read.extent.height && !Contains(source->cutDrawn, read))
                        {
                            source->wholeArea = true;
                            s_ua.resolvesPast++;
                            LogOnce((0x31ull << 56) | (uint64_t(source->base) << 24) | (uint64_t(source->pitch) << 1) | uint64_t(source->depth),
                                "untiled area: frame %llu resolve of target b%u p%u%s reads %ux%u@%d,%d, past what its cut draws drew (%ux%u@%d,%d "
                                "in frame %llu; dest %08X): its draws keep the whole target from now on",
                                (unsigned long long)s_frame, source->base, source->pitch, source->depth ? " (depth)" : "", read.extent.width,
                                read.extent.height, read.offset.x, read.offset.y, source->cutDrawn.extent.width, source->cutDrawn.extent.height,
                                source->cutDrawn.offset.x, source->cutDrawn.offset.y, (unsigned long long)source->areaFrame, destBase);
                        }
                    }
                    if (s_clearFlushRect)
                    {
                        // What the shader reads (px() and the scaled path:
                        // the rectangle's host pixels from rc.srcOrigin, each
                        // axis clamped to the target; at 1.5x its edges are
                        // even, so whole), so the clears it doesn't see stay
                        // pending.
                        auto [shiftX, shiftY] = TileShift();
                        auto texel = [](int32_t v, uint32_t size) { return std::clamp(v, 0, int32_t(size) - 1); };
                        int32_t x0 = texel(source->scale.x.EdgeAt(r.x0 + shiftX), source->width);
                        int32_t y0 = texel(source->scale.y.EdgeAt(r.y0 + shiftY), source->height);
                        int32_t x1 = texel(source->scale.x.EdgeAt(r.x1 + shiftX) - 1, source->width) + 1;
                        int32_t y1 = texel(source->scale.y.EdgeAt(r.y1 + shiftY) - 1, source->height) + 1;
                        VkRect2D read{ { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
                        FlushPendingClears(source, &read);
                    }
                    else
                    {
                        FlushPendingClears(source);
                    }
                    EndPass(kBreakResolve);
                    Begin();
                    ResolveConstants rc{};
                    rc.originX = r.x0;
                    rc.originY = r.y0;
                    rc.width = uint32_t(r.x1 - r.x0);
                    rc.height = uint32_t(r.y1 - r.y0);
                    rc.destBase = destBase & 0x1FFFFFFF;
                    rc.destPitch = (destPitch.copy_dest_pitch + 31) & ~31u;
                    rc.format = format;
                    rc.endian = uint32_t(destInfo.copy_dest_endian);
                    rc.flags = (destInfo.copy_dest_swap ? 1u : 0u) | (depthSource ? 2u : 0u);
                    rc.expScale = std::ldexp(1.0f, destInfo.copy_dest_exp_bias);
                    rc.srcWidth = source->width;
                    rc.srcHeight = source->height;
                    auto [shiftX, shiftY] = TileShift();
                    rc.srcOriginX = source->scale.x.EdgeAt(r.x0 + shiftX);
                    rc.srcOriginY = source->scale.y.EdgeAt(r.y0 + shiftY);
                    rc.scaleX = source->scale.x.num;
                    rc.scaleY = source->scale.y.num;
                    rc.denX = source->scale.x.den;
                    rc.denY = source->scale.y.den;
                    rc.srcGuestX = std::max(r.x0 + shiftX, 0);
                    rc.srcGuestY = std::max(r.y0 + shiftY, 0);
                    // At 1.5x the shader counts host pixels from srcOrigin
                    // and the destination's blocks from its origin: both
                    // need even coordinates (resolves are 8-pixel aligned
                    // and the tile offsets even, so they are). Below 1x
                    // multiples of 2, 4 or 8 (the 8-pixel alignment, and
                    // tile offsets of 256 rows and 640 columns). A texel
                    // axis counts each pixel from its place in the target
                    // (srcGuest), so any origin will do (Exact).
                    const bool wholeBlocks = (r.x0 + shiftX) >= 0 && (r.y0 + shiftY) >= 0 &&
                        source->scale.x.Exact(uint32_t(r.x0 + shiftX)) && source->scale.y.Exact(uint32_t(r.y0 + shiftY)) &&
                        source->scale.x.Exact(uint32_t(r.x0)) && source->scale.y.Exact(uint32_t(r.y0));
                    if (!wholeBlocks)
                        LogOnce(0x2500, "resolve at (%d,%d) (tile shift %d,%d) from a %s target isn't whole blocks: kept at 1x only, "
                            "its 1x average up to half a host pixel off", r.x0, r.y0, shiftX, shiftY, gpu::ScaleName(source->scale).c_str());
                    uint32_t bl = ResolveBppLog2(format);
                    uint32_t firstPage = rangeBase >> 12;
                    uint32_t lastPage = std::min<uint32_t>((rangeBase + rangeBytes - 1) >> 12, kGuestPages - 1);
                    // A 32-bit resolve from a scaled target keeps its samples.
                    // Below 1x not a depth one's: depth is sampled through a
                    // conversion (D24 to float), which scaled textures don't
                    // have (ScaledFormat), so its samples were written and
                    // never read, 70 us a 1280x256 tile band on the Steam
                    // Deck. A read of one in another format would find its
                    // pages without a scaled copy and use the 1x data.
                    bool scaledWrite = !source->scale.IsOne() && wholeBlocks && bl == 2 && !destInfo.copy_dest_array &&
                        !(depthSource && source->scale.Below()) && EnsureScaledMemory() && source->scale == s_scaled.scale &&
                        ScaledWanted(rangeBase, rangeBytes);
                    bool freshSlots = false;  // a page here had no current scaled copy before this
                    // Pages whose samples only a scaled texture holds (see
                    // below): their slots are filled from the 1x data too
                    // unless that texture takes this resolve and keeps them.
                    std::vector<std::pair<uint32_t, uint32_t>> heldSlots;
                    // Below 1x only while a reader takes its samples (ScaledWriteWanted).
                    const ResolvePages pages{ firstPage, lastPage, (rc.destBase & 0xFFFu) == 0 && bl == 2, rc.destBase >> 12,
                        std::max(rc.destPitch >> 5, 1u), r.x0, r.y0, r.x1, r.y1 };
                    if (scaledWrite && source->scale.Below())
                    {
                        const uint64_t key = (uint64_t(rc.destBase) << 32) ^ (uint64_t(rc.destPitch) << 20) ^ (uint64_t(rc.width) << 8) ^
                            (uint64_t(rc.height) * 0x9E3779B1ull) ^ (uint64_t(format) << 58);
                        if (!ScaledWriteWanted(key, pages))
                        {
                            scaledWrite = false;
                            s_scaled.unreadSkips++;
                            LogOnce((0x35ull << 56) ^ key, "below 1x: resolves of %ux%u to %08X (pitch %u, format %u) keep no scaled copy: "
                                "not read at host resolution %u times in a row (the 1x copy only, until a reader wants one)", rc.width,
                                rc.height, rc.destBase, rc.destPitch, format, kUnreadSkip);
                        }
                    }
                    // (Scaled: no fusion, so held colour records, see above,
                    // are written now, before the fill below reads the 1x data.)
                    if (scaledWrite)
                    {
                        for (const DeferredResolve& h : held)
                        {
                            s_deferredResolves.push_back(h);
                            s_deferForResolve++;
                            WriteDeferred(s_deferredResolves.size() - 1);
                        }
                        held.clear();
                    }
                    if (scaledWrite)
                    {
                        // Below 1x a page the rectangle covers whole (its
                        // 32x32 tile: 32-bit texels, a page-aligned base) gets
                        // every sample from the resolve: no fill first. The
                        // scene's tile bands, 1x-resolved over by the frame's
                        // composite (ScaledWriteWanted), went stale each frame
                        // and were filled from the 1x data before every band
                        // (0.3 ms a frame on the Steam Deck).
                        const bool coverSkips = source->scale.Below();
                        std::vector<std::pair<uint32_t, uint32_t>> fresh;
                        for (uint32_t page = firstPage; page <= lastPage; page++)
                        {
                            // A slot whose copy went stale (a CPU write, a 1x
                            // resolve) starts over from the 1x data too: the
                            // resolve may not cover all of it.
                            const bool held = ScaledPageHeld(page);
                            bool stale = s_scaled.pageSlot[page] != 0 && !ScaledPageValid(page) && !held;
                            bool isFresh;
                            uint32_t slot = ScaledSlot(page, isFresh);
                            if ((isFresh || stale) && !(coverSkips && pages.Covers(page)))
                                fresh.push_back({ page, slot });
                            else if (held)
                                heldSlots.push_back({ page, slot });
                        }
                        FlushScaledTable();
                        FillFreshSlots(fresh);
                        freshSlots = !fresh.empty();
                        rc.scaledWrite = 1;
                        rc.slotWords = s_scaled.slotWords;
                        s_scaled.resolvedBytes += s_scaled.scale.HostPixels(rc.width, rc.height) * 4;
                    }

                    // A cached direct 1x texture of exactly this surface (its
                    // address, tiling, pitch and 32-bit texels), current until
                    // now, whose memory this resolve rewrites: the resolve
                    // stores its texels too, and the texture stays current. The
                    // Steam Frame uploaded each 1600x1600 shadow map again after
                    // its resolve (~0.4 ms, twice a frame).
                    // Else (NFSMW_FUSE_PARTIAL, NFSMW_DIRECT_CUBE) one holding
                    // the destination whole 32x32 tiles in, and all this resolve
                    // writes (a strip of the scene, a cube's face): the same,
                    // at those tiles' texels (see the shader). Its other texels
                    // must still be what it loaded: checked again after
                    // MarkWritten, below.
                    Texture* fused = nullptr;
                    Texture* direct = nullptr;
                    bool partial = false;
                    uint32_t layer = 0, texX = 0, texY = 0;  // partial: the face, and the destination's texel 0,0 in it
                    // Why not (NFSMW_LOG_UPLOADS).
                    // A scaled resolve writes the same 1x words too, so it
                    // stores them into a 1x texture of its surface as well
                    // (NFSMW_SCALED_FUSE=0: not, as before): the scene's depth
                    // (a D24 texture, never scaled) and its colour where a
                    // draw into a 1x target (the bloom chain) samples it were
                    // loaded again every frame at native rows.
                    const bool fuseHere = !scaledWrite || s_scaledFuse;
                    const char* unfused = !s_resolveToTexture ? "off" : !fuseHere ? "scaled resolve" : bl != 2 ? "not 32-bit"
                        : "no direct texture there";
                    // Whether `t` can take this resolve's words as its texels at all.
                    auto unfit = [&](const Texture& t) -> const char* {
                        return !(t.uploaded && t.storeView && !t.scaled) ? "texture not loaded"
                            : !t.tiled || t.bpbLog2 != 2 || (t.convert != 0 && t.convert != 4) ? "texture layout"
                            : t.pitch != rc.destPitch ? "pitch differs"
                            : t.offX != 0 || t.offY != 0 ? "texture offset"
                            : nullptr;
                    };
                    if (fuseHere && bl == 2 && s_resolveToTexture)
                        if (auto it = s_directByBase.find(rc.destBase); it != s_directByBase.end())
                        {
                            Texture& t = *(direct = it->second);
                            unfused = unfit(t);
                            if (!unfused)
                                unfused = !(t.guestStart >= rangeBase && uint64_t(t.guestStart) + t.guestSize <= uint64_t(rangeBase) + rangeBytes)
                                    ? "texture beyond the resolve's range"
                                    : writewatch::WrittenSince(t.guestStart, t.guestSize, t.loadedSequence) ? "texture written since its load"
                                    : nullptr;
                            if (!unfused)
                                fused = &t;
                        }
                    if (!fused && fuseHere && bl == 2 && s_resolveToTexture && (s_fusePartial || s_directCube))
                    {
                        // Each direct texture holding the destination, the
                        // nearest start first, until one takes it: textures
                        // are kept (dead ones too) until the cache is dropped,
                        // so an older one may start inside a larger one. Why
                        // not: the nearest's reason, or the nearest's that
                        // could take its words at all (unfit).
                        Texture* why = nullptr;
                        bool whyFits = false;
                        for (auto it = s_directByStart.upper_bound(rc.destBase); !fused && it != s_directByStart.begin();)
                        {
                            if (rc.destBase - (--it)->first >= s_maxDirectSize)
                                break;  // none starting earlier reaches it
                            Texture& t = *it->second;
                            if (rc.destBase - t.guestStart >= t.guestSize)
                                continue;
                            // Its face holding the destination (a cube's: its
                            // layers are the guest's faces in order, as the
                            // staging path copies them), and the tile there:
                            // 32-bit texels make a tile 4 KB, so a destination
                            // whole tiles in is the texture's texels from
                            // (tile % tiles a row, tile / tiles a row) x 32,
                            // as the untile reads them.
                            uint32_t face = t.cube ? (rc.destBase - t.guestStart) / t.sliceStride : 0;
                            uint32_t sliceStart = t.guestStart + face * t.sliceStride;
                            uint64_t sliceEnd = uint64_t(t.guestStart) + t.guestSize;
                            if (t.cube)
                                sliceEnd = std::min<uint64_t>(sliceEnd, uint64_t(sliceStart) + t.sliceStride);
                            uint32_t rel = rc.destBase - sliceStart, tilesPerRow = std::max(t.pitch >> 5, 1u);
                            uint32_t x = (rel >> 12) % tilesPerRow * 32, y = (rel >> 12) / tilesPerRow * 32;
                            const char* reason = unfit(t);
                            bool fits = !reason;
                            if (fits)
                                reason = face >= t.layers ? "texture layout"
                                    : rel % 4096 != 0 ? "not whole tiles into the texture"
                                    : (rel != 0 || !t.cube) && !s_fusePartial ? "partial fusion off"
                                    : x + uint32_t(r.x1) > rc.destPitch ? "beyond the texture's columns"
                                    : !(rangeBase >= sliceStart && uint64_t(rangeBase) + rangeBytes <= sliceEnd)
                                    ? (t.cube ? "beyond the face" : "beyond the texture")
                                    : writewatch::WrittenSince(t.guestStart, t.guestSize, t.loadedSequence) ? "texture written since its load"
                                    : nullptr;
                            if (!reason)
                            {
                                fused = direct = &t;
                                unfused = nullptr;
                                partial = true;
                                layer = face;
                                texX = x;
                                texY = y;
                            }
                            else if (!why || (fits && !whyFits))
                            {
                                why = direct = &t;
                                whyFits = fits;
                                unfused = reason;
                            }
                        }
                    }
                    // A scaled resolve stores its samples into a cached scaled
                    // texture holding its destination too, where that
                    // texture's untile would put them (sbReaderHost), so it
                    // stays current instead of being untiled from the pool
                    // again before its next use: the scene, every frame. The
                    // texture as the 1x rule above, by its first byte: this
                    // surface's pitch, the destination whole 32x32 tiles in
                    // and all this resolve writes inside it, nothing written
                    // there since its load, and every page of it current in
                    // the pool before this resolve (so its texels are the
                    // pool's, and none of its slots is filled afresh now).
                    // Its other texels must still be what it loaded: checked
                    // again after MarkWritten. NFSMW_SCALED_FUSE=0: not.
                    Texture* hosted = nullptr;
                    const char* unhosted = !scaledWrite ? nullptr : !s_scaledFuse || !s_resolveToTexture ? "off"
                        : freshSlots ? "fresh slots" : "no scaled texture there";
                    uint32_t hostX = 0, hostY = 0;
                    if (scaledWrite && s_scaledFuse && s_resolveToTexture && !freshSlots)
                        for (auto it = s_scaledByStart.upper_bound(rc.destBase); !hosted && it != s_scaledByStart.begin();)
                        {
                            if (rc.destBase - (--it)->first >= s_maxScaledSize)
                                break;
                            Texture& t = *it->second;
                            if (rc.destBase - t.guestStart >= t.guestSize)
                                continue;
                            uint32_t rel = rc.destBase - t.guestStart, tilesPerRow = std::max(t.pitch >> 5, 1u);
                            uint32_t x = (rel >> 12) % tilesPerRow * 32, y = (rel >> 12) / tilesPerRow * 32;
                            const char* reason = !(t.uploaded && t.storeView && t.scaled) ? "texture not loaded"
                                : t.pitch != rc.destPitch ? "pitch differs"
                                : rel % 4096 != 0 ? "not whole tiles into the texture"
                                : x + uint32_t(r.x1) > rc.destPitch ? "beyond the texture's columns"
                                : !(rangeBase >= t.guestStart && uint64_t(rangeBase) + rangeBytes <= uint64_t(t.guestStart) + t.guestSize)
                                ? "beyond the texture"
                                : writewatch::WrittenSince(t.guestStart, t.guestSize, t.loadedSequence) ? "texture written since its load"
                                : !ScaledSurfaceValid(t.guestStart, t.guestSize, t.scaledSig) ? "texture's pages not all current"
                                : nullptr;
                            if (!reason)
                            {
                                hosted = &t;
                                hostX = x;
                                hostY = y;
                            }
                            unhosted = reason;
                        }
                    // And when no other scaled reader (another texture, the
                    // front buffer) asked for this memory lately, the samples
                    // go only into that texture, not the pool too: its pages
                    // are then held by it (pageHeld), current for it alone;
                    // any other reader sees them as 1x until a resolve
                    // writes the pool again (one now asking makes the next
                    // one do so). NFSMW_SCALED_HOLD=0: always the pool too.
                    const bool hold = hosted && s_scaledHold &&
                        !ScaledWanted(rangeBase, rangeBytes, hosted->guestStart, hosted->guestSize, hosted->scaledSig);
                    // Held pages this resolve doesn't keep held by the same
                    // texture: their slots start over from the 1x data, as
                    // stale ones (the pool's copy is old).
                    if (scaledWrite)
                    {
                        std::erase_if(heldSlots, [&](const std::pair<uint32_t, uint32_t>& ps) {
                            return hold && s_scaled.pageHeld[ps.first] == hosted;
                        });
                        if (!heldSlots.empty())
                        {
                            FillFreshSlots(heldSlots);
                            for (const auto& ps : heldSlots)
                                s_scaled.pageHeld[ps.first] = nullptr;
                        }
                    }
                    if (hosted)
                    {
                        rc.hostWrite = hold ? 2 : 1;
                        rc.hostX = hostX;
                        rc.hostY = hostY;
                        rc.hostWidth = hosted->guestWidth;
                        rc.hostHeight = hosted->guestHeight;
                        rc.hostEndian = hosted->endian;
                        LogOnce(0x2900, "scaled resolves store into cached scaled textures too (the first: %ux%u host at %08X, texels from %u,%u)",
                            hosted->width, hosted->height, rc.destBase, hostX, hostY);
                    }
                    if (s_uploadLog)
                    {
                        UploadLog& log = *s_uploadLog;
                        log.resolves[rc.destBase] = { s_frame, unfused };
                        if (log.now)
                        {
                            log.resolvesNow++;
                            log.fusedNow += fused != nullptr;
                            std::string part = !partial ? std::string()
                                : fused->cube ? std::format(" (face {}, texels from {},{})", layer, texX, texY)
                                : std::format(" (texels from {},{})", texX, texY);
                            fprintf(stderr, "[upload] frame %llu resolve dest %08X range %08X+%u rect %ux%u fmt %u pitch %u %s %s%s%s%s",
                                (unsigned long long)s_frame, rc.destBase, rangeBase, rangeBytes, rc.width, rc.height, format, rc.destPitch,
                                depthSource ? "depth" : "color", fused ? "fused" : "unfused: ", fused ? part.c_str() : unfused,
                                !scaledWrite ? "" : hosted ? ", scaled: stored into its scaled texture" : ", scaled: not stored into a scaled texture: ",
                                scaledWrite && !hosted ? unhosted : "");
                            std::set<std::string> kinds;
                            if (direct)
                                fprintf(stderr, " | texture %08X+%u %ux%u pitch %u%s%s", direct->guestStart, direct->guestSize, direct->width,
                                    direct->height, direct->pitch, direct->uploaded ? " " : "",
                                    direct->uploaded ? DescribeWrites(*direct, kinds).c_str() : "");
                            fprintf(stderr, "\n");
                        }
                    }
                    if (fused)
                    {
                        fused->lastUsed = s_frame;  // (this recording now stores into it: see Texture::lastUsed)
                        rc.texWrite = 1;
                        rc.texEndian = fused->endian;
                        rc.texConvert = fused->convert;
                        rc.texWidth = fused->width;
                        rc.texHeight = fused->height;
                        rc.texX = texX;
                        rc.texY = texY;
                        LogOnce(0x2400, "resolves store into cached textures too (the first: %ux%u at %08X)", fused->width,
                            fused->height, rc.destBase);
                        if (partial)
                            LogOnce(0x2800, "resolves store into part of cached textures too (the first: %ux%u%s at %08X, texels from %u,%u)",
                                fused->width, fused->height, fused->cube ? " cube" : "", rc.destBase, texX, texY);
                    }

                    VkDescriptorImageInfo ii{ VK_NULL_HANDLE, source->sampleView, VK_IMAGE_LAYOUT_GENERAL };
                    VkDescriptorImageInfo ti{ VK_NULL_HANDLE, fused ? fused->storeViews[layer] : s_dummyStorage.view, VK_IMAGE_LAYOUT_GENERAL };
                    VkDescriptorImageInfo hi{ VK_NULL_HANDLE, hosted ? hosted->storeView : s_dummyStorage.view, VK_IMAGE_LAYOUT_GENERAL };
                    VkDescriptorBufferInfo pool = DummyStorage(s_scaled.pool);
                    VkDescriptorBufferInfo table = DummyStorage(s_scaled.table);
                    VkWriteDescriptorSet w[9]{};
                    w[0] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr };
                    w[1] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1,
                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii, nullptr, nullptr };
                    w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pool, nullptr };
                    w[3] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 3, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &table, nullptr };
                    w[4] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 4, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ti, nullptr, nullptr };
                    w[5] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 5, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &hi, nullptr, nullptr };
                    s_stateEpoch++;
                    ComputeWorkRecorded();
                    bool coalesced = scaledWrite && s_scaled.coalesce && ScaledCoalesceFits(source->scale);  // 32x2 pixels a group
                    // NFSMW_RESOLVE_DEFER (see s_resolveDefer): the texture
                    // only, now. The target must come out of this resolve
                    // cleared whole (it goes on in another image).
                    const bool defer = s_resolveDefer && s_shadowMode && depthSource && fused && !partial && !scaledWrite && bl == 2 &&
                        source->scale.IsOne() && copyControl.depth_clear_enable && !s_logShadowMap && r.x0 + shiftX <= 0 &&
                        r.y0 + shiftY <= 0 && r.x1 + shiftX >= int32_t(source->guestWidth) && r.y1 + shiftY >= int32_t(source->guestHeight);
                    // NFSMW_RESOLVE_DEFER_COLOR (see s_resolveDeferColor).
                    const bool colourFits = s_shadowMode && !depthSource && fused && fused->convert == 0 && !scaledWrite && bl == 2 &&
                        source->scale.IsOne() && r.x0 >= 0 && r.y0 >= 0 && uint64_t(r.x1) + rc.texX <= fused->width &&
                        uint64_t(r.y1) + rc.texY <= fused->height;
                    // NFSMW_RESOLVE_LAZY (see s_hleLazy): a texture the CPU
                    // hasn't locked lately (gpu/cpu_locks.h).
                    const bool hleFits = s_hleLazyMode && !cpulocks::LockedLately(fused ? fused->guestStart : rc.destBase) && s_shadowMode && fused &&
                        !scaledWrite &&
                        bl == 2 && source->scale.IsOne() && r.x0 >= 0 && r.y0 >= 0 && !defer;
                    const bool hleDepthFits = hleFits && depthSource && fused->convert == 4 && !copyControl.depth_clear_enable &&
                        rc.endian == fused->endian && uint64_t(r.x1) + rc.texX <= fused->width && uint64_t(r.y1) + rc.texY <= fused->height;
                    const bool hleColourFits = hleFits && !depthSource && !colourFits && fused->convert == 0 &&
                        uint64_t(r.x1) + rc.texX < uint64_t(fused->width) + 8 && uint64_t(r.y1) + rc.texY < uint64_t(fused->height) + 8;
                    const bool deferHle = s_hleLazy && (hleDepthFits || hleColourFits);
                    const bool deferColour = s_resolveDeferColor && colourFits;
                    const bool deferTexture = deferColour || deferHle;  // a record with its texture (written back from it)
                    // (Split memory too: WRITEBACK_CHECK reads through memLoad.)
                    const bool checkColour = (s_resolveDeferColorMode == 2 && colourFits) || (s_hleLazyMode == 2 && (hleDepthFits || hleColourFits));
                    std::vector<DeferredResolve> covered;  // held ones this one's texels cover whole
                    for (const DeferredResolve& h : held)
                    {
                        const bool same = deferTexture && h.texture == fused && h.layer == layer;
                        if (same && CoversTexels(rc, h.rc))
                            covered.push_back(h);
                        else
                        {
                            s_deferredResolves.push_back(h);
                            if (!same)
                            {
                                s_deferForResolve++;
                                WriteDeferred(s_deferredResolves.size() - 1);
                            }
                        }
                    }
                    const bool textureOnly = defer || deferTexture;
                    // NFSMW_RESOLVE_RENAME (see s_hleRename): the texture samples
                    // the target's image as it stands (made a spare by the
                    // defer below), so nothing is stored now. Depth sampled
                    // through a filter only where its format allows it.
                    static const bool renameSampleable = [] {
                        VkFormatProperties fp;
                        vkGetPhysicalDeviceFormatProperties(s_vk->physical, VK_FORMAT_D32_SFLOAT_S8_UINT, &fp);
                        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
                        return (fp.optimalTilingFeatures & need) == need;
                    }();
                    const bool rename = s_hleRename && defer && !cpulocks::LockedLately(fused->guestStart) && rc.endian == fused->endian &&
                        fused->levels == 1 && !fused->cube && !fused->volume && fused->convert == 4 &&
                        fused->width == source->width && fused->height == source->height && source->vkFormat == VK_FORMAT_D32_SFLOAT_S8_UINT &&
                        renameSampleable;
                    // A renamed texture this resolve stores into after all:
                    // its own texels again, the store's (a whole one, the
                    // defer's), or the renamed image's first (any other).
                    if (fused && fused->renamedImage && !rename)
                    {
                        if (defer)
                        {
                            fused->renamedImage = VK_NULL_HANDLE;
                            s_renameDrops++;
                            ForgetTextureLookups(fused);
                        }
                        else
                            MaterializeRename(*fused);
                    }
                    if (s_hleCounting && !rename)
                    {
                        const uint64_t px = (uint64_t(rc.width) * rc.height) << bl;
                        s_hle.srcBytes += px;
                        s_hle.guestBytes += textureOnly ? 0 : px;
                        s_hle.textureBytes += fused ? px : 0;
                    }
                    if (use)
                    {
                        std::lock_guard lock(s_resolveUseMutex);
                        use->fused = fused != nullptr;
                        use->partial = partial;
                        use->deferred = textureOnly;
                    }
                    if (textureOnly && !s_resolveTexturePipeline)
                        MakeResolvePipeline(false, 1, 1, &s_resolveTexturePipeline, true);
                    // NFSMW_RESOLVE_PRESENT (see s_hlePresent).
                    const FrontSeen* front = nullptr;
                    if (s_hlePresent && !cpulocks::LockedLately(rc.destBase) && !depthSource && bl == 2 && !scaledWrite && !fused && !textureOnly &&
                        !coalesced && source->scale.IsOne() && (rc.destBase & 0xFFFu) == 0 && r.x0 == 0 && r.y0 == 0)
                        for (const FrontSeen& f : s_frontSeen)
                            if (f.base == rc.destBase && f.pitch == rc.destPitch && f.frame + 4 >= s_frame && uint32_t(r.x1) >= f.width &&
                                uint32_t(r.y1) >= f.height)
                                front = &f;
                    VkImage frontImage = VK_NULL_HANDLE;
                    VkDescriptorImageInfo fi{ VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL };
                    // NFSMW_RESOLVE_PRESENT=2 (see s_hlePresentMode): the
                    // target's image itself, when this resolve clears the whole
                    // target and it is the front image's size and format.
                    bool handover = false;
                    int handoverSlot = -1;
                    const auto [handX, handY] = TileShift();
                    // (The target's image is as tall as EDRAM allows, taller
                    // than the frame: the presenter shows its top-left part.)
                    if (front && s_hlePresentMode == 2 && copyControl.color_clear_enable && !partial && !rename && !defer &&
                        source->vkFormat == VK_FORMAT_R8G8B8A8_UNORM && source->width >= front->width && source->height >= front->height &&
                        r.x0 + handX <= 0 && r.y0 + handY <= 0 && r.x1 + handX >= int32_t(front->width) &&
                        r.y1 + handY >= int32_t(front->height))
                        handoverSlot = ClaimFrontSlotForHandover(front->width, front->height);
                    if (front && s_hlePresentMode == 2 && handoverSlot < 0)
                        LogOnce(0x3450, "no hand-over of the front buffer's resolve: clear %d partial %d rename %d defer %d format %u (want %u) "
                            "size %ux%u (front %ux%u) rect %d,%d-%d,%d shift %d,%d guest %ux%u slot %d",
                            int(copyControl.color_clear_enable), int(partial), int(rename), int(defer), unsigned(source->vkFormat),
                            unsigned(VK_FORMAT_R8G8B8A8_UNORM), source->width, source->height, front->width, front->height, r.x0, r.y0, r.x1, r.y1,
                            handX, handY, source->guestWidth, source->guestHeight, handoverSlot);
                    if (handoverSlot >= 0)
                    {
                        // (A hand-over's image is the ring's: the limit on
                        // records keeping a target image doesn't count it.)
                        if (s_deferredResolves.size() >= kMaxDeferred)
                        {
                            s_deferForRoom++;
                            WriteDeferred(0);
                        }
                        auto [image, view] = SwapTargetImage(*source);
                        HandOverFront(handoverSlot, image, view, source->vkFormat, source->width, source->height);
                        frontImage = image.image;
                        handover = true;
                        s_deferredResolves.push_back({ rangeBase, rangeBytes, rc, image, view, source->vkFormat, source->width, source->height,
                            nullptr, 0, true });
                        s_deferredCount++;
                        s_hle.presented += s_hleCounting;
                        LogOnce(0x3440, "the resolve into the front buffer hands the target's image to the presenter (NFSMW_RESOLVE_PRESENT=2; "
                            "the first: %ux%u at %08X)", front->width, front->height, rc.destBase);
                    }
                    else if (front && ClaimFrontForResolve(front->width, front->height, frontImage, fi.imageView))
                    {
                        if (!s_resolveFrontPipeline)
                            MakeResolvePipeline(false, 1, 1, &s_resolveFrontPipeline, false, 3);
                        rc.texEndian = front->endian;
                        rc.texConvert = front->format;
                        rc.texWidth = front->width;
                        rc.texHeight = front->height;
                        rc.slotWords = front->swizzle;
                        w[4].pImageInfo = &fi;
                        FullBarrier();  // (as the swap's conversion: after the presenter's last read of the image)
                        s_hle.presented += s_hleCounting;
                        LogOnce(0x3420, "the resolve into the front buffer stores the picture into the front image too (NFSMW_RESOLVE_PRESENT; "
                            "the first: %ux%u at %08X)", front->width, front->height, rc.destBase);
                    }
                    else
                        front = nullptr;
                    if (s_profiling)
                    {
                        char desc[160];
                        snprintf(desc, sizeof(desc), "resolve %ux%u@%d,%d p%u fmt %u to %08X %s%s%s%s%s", rc.width, rc.height, rc.originX,
                            rc.originY, pitch, format, rc.destBase, depthSource ? "depth " : "",
                            scaledWrite ? std::format("scaled {}", gpu::ScaleName(source->scale)).c_str()
                            : !source->scale.IsOne() ? "1x (from a scaled target)" : "1x", fused ? " fused" : "",
                            rename ? ", renamed (NFSMW_RESOLVE_RENAME)" : textureOnly ? ", texture only (deferred)" : "",
                            handover ? ", its image handed to the presenter" : front ? ", to the presenter" : "");
                        ProfileCompute(desc);
                    }
                    if (!rename && !handover)
                    {
                        vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, textureOnly ? s_resolveTexturePipeline
                            : front ? s_resolveFrontPipeline : ResolvePipeline(coalesced, source->scale));
                        s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_resolveLayout, 0, AddSharedParts(w, 6), w);
                        vkCmdPushConstants(s_cmd, s_resolveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            source->scale.IsOne() ? s_resolvePush : uint32_t(sizeof(ResolveConstants)), &rc);
                        uint32_t perWord = bl >= 2 ? 1 : (4 >> bl);
                        if (!(s_exp & 2) && textureOnly)  // a group a 32x32 tile of the destination
                            vkCmdDispatch(s_cmd, ((r.x1 - 1) >> 5) - (r.x0 >> 5) + 1, ((r.y1 - 1) >> 5) - (r.y0 >> 5) + 1, 1);
                        else if (!(s_exp & 2))
                            vkCmdDispatch(s_cmd, coalesced ? (rc.width + 31) / 32 : (rc.width / perWord + 7) / 8,
                                coalesced ? (rc.height + 1) / 2 : (rc.height + 7) / 8, 1);
                        // Below 1x a scaled resolve's barrier is a 1x one's: its
                        // pool writes are compute shader writes too, read by
                        // compute (untiles, the front buffer) and fragment work,
                        // never by a vertex stage. (A narrower one only where 1x
                        // resolves get one, NFSMW_PASS_BARRIER 2 and up: Apple's
                        // default; elsewhere ResolveBarrier is a full one too.)
                        if (scaledWrite && !source->scale.Below())
                            FullBarrier();  // (above 1x: as before)
                        else
                            ResolveBarrier(rangeBase, rangeBytes, fused ? fused->guestStart : 0);
                    }
                    else if (rename)
                        ResolveBarrier(0, 0, fused->guestStart);  // (its texture now holds the pass's depth writes)
                    ProfileComputeEnd();
                    if (front)
                        s_frontDirect = { true, rc.destBase, s_frame, frontImage, 0, rangeBase, rangeBytes, *front };
                    else if (s_frontDirect.pending && uint64_t(rangeBase) < uint64_t(s_frontDirect.rangeBase) + s_frontDirect.rangeBytes &&
                        uint64_t(s_frontDirect.rangeBase) < uint64_t(rangeBase) + rangeBytes)
                        s_frontDirect.pending = false;  // another resolve into it
                    if (checkColour)
                        CheckColourWriteback(rc, fused, layer, rangeBase, rangeBytes);
                    if (s_logShadowMap && depthSource && pitch == 1600 && bl == 2)
                        ShadowMapProbeRecord(rangeBase, rangeBytes, rc, r, source->base);
                    SubmitIfStarving();
                    // Write sequences are per host page: guest pages of the
                    // edge host pages outside this resolve whose scaled copy
                    // was current stay current (this write isn't theirs).
                    std::vector<uint32_t> siblings;
                    if (s_scaled.enabled && s_scaled.hostPageGuestPages > 1)
                    {
                        uint32_t g = s_scaled.hostPageGuestPages;
                        for (uint32_t edge : { firstPage, lastPage })
                            for (uint32_t q = edge / g * g; q < edge / g * g + g && q < kGuestPages; q++)
                                if ((q < firstPage || q > lastPage) && (ScaledPageValid(q) || ScaledPageHeld(q)))
                                    siblings.push_back(q);
                    }
                    uint64_t written = writewatch::MarkWritten(rangeBase, rangeBytes, writewatch::Writer::kResolve, rc.destBase);
                    if (front)
                        s_frontDirect.written = written;
                    // The fused texture holds this write: its pages' sequence
                    // now, and any write after it is newer (0: per-page
                    // sequences, NFSMW_WATCH_RANGES=0, so it is loaded again).
                    // A partial one's pages outside this range keep theirs,
                    // which the check above found no newer than its load: a
                    // CPU store there since (a fault racing this) is older
                    // than `written`, and would never be loaded. Then it stays
                    // as it was (its range is newer now: it loads again). The
                    // exact rule's texture is all in the range. Quiesce first:
                    // a fault that took its sequence before `written` may not
                    // have stored it on its pages yet.
                    uint32_t rangeEnd = rangeBase + rangeBytes;
                    if (partial)
                        writewatch::Quiesce();
                    bool keep = !partial || (!writewatch::WrittenSince(fused->guestStart, rangeBase - fused->guestStart, fused->loadedSequence) &&
                        !writewatch::WrittenSince(rangeEnd, fused->guestStart + fused->guestSize - rangeEnd, fused->loadedSequence));
                    if (fused && written && keep)
                    {
                        fused->loadedSequence = written;
                        fused->verifyHashes.clear();  // (NFSMW_TEXTURE_VERIFY: its memory is the GPU's to change now)
                    }
                    // The scaled texture likewise: current now, unless a
                    // page of it outside this resolve was written meanwhile
                    // (then it loads again, as without this).
                    bool hostKept = false;
                    if (hosted && written)
                    {
                        if (!partial)
                            writewatch::Quiesce();  // (as above)
                        if (!writewatch::WrittenSince(hosted->guestStart, rangeBase - hosted->guestStart, hosted->loadedSequence) &&
                            !writewatch::WrittenSince(rangeEnd, hosted->guestStart + hosted->guestSize - rangeEnd, hosted->loadedSequence))
                        {
                            hosted->loadedSequence = written;
                            s_scaled.hosted++;
                            hostKept = true;
                        }
                    }
                    if (fused && !keep && s_uploadLog)
                    {
                        s_uploadLog->resolves[rc.destBase].unfused = "texture written outside it meanwhile";
                        if (s_uploadLog->now)
                        {
                            s_uploadLog->fusedNow--;
                            fprintf(stderr, "[upload] frame %llu resolve dest %08X: stored into %08X, but the texture was written outside "
                                "it meanwhile: loaded again\n", (unsigned long long)s_frame, rc.destBase, fused->guestStart);
                        }
                    }
                    // CPU writes must fault to move the sequence on (and so
                    // retire the scaled copy), whatever else protects the pages.
                    if (scaledWrite)
                        writewatch::Watch(rangeBase, rangeBytes);
                    if (s_scaled.enabled)
                    {
                        // Held (only the texture has this resolve's samples)
                        // while the texture took them and stays current; a
                        // held resolve whose texture didn't: lost (1x).
                        for (uint32_t page = firstPage; page <= lastPage; page++)
                        {
                            const bool lost = hold && !hostKept;
                            s_scaled.pageSeq[page] = scaledWrite && !lost ? writewatch::WriteSequence(page << 12, 4096) : 0;
                            s_scaled.pageHeld[page] = hold && hostKept ? hosted : nullptr;
                        }
                        for (uint32_t q : siblings)
                            s_scaled.pageSeq[q] = writewatch::WriteSequence(q << 12, 4096);
                    }
                    // Deferred: its bytes owed first, so that no CPU access
                    // finds the pages guarded and not owed.
                    if (textureOnly || handover)
                        writewatch::DeferWrite(rangeBase, rangeBytes);
                    GuardGpu(rangeBase, rangeBytes, true, "resolve");
                    writewatch::MarkGpuOwned(rangeBase, rangeBytes);
                    if (replaced)
                    {
                        s_deferReplaced++;
                        writewatch::DeferredWritten(replaced->rangeBase, replaced->rangeBytes);
                        if (!replaced->texture && !replaced->borrowed)
                        {
                            ReleaseRenamedImage(replaced->image.image, rename ? fused : nullptr);
                            s_spareTargets.push_back({ replaced->image, replaced->sampleView, replaced->format, replaced->width, replaced->height });
                        }
                    }
                    for (const DeferredResolve& c : covered)
                    {
                        s_deferReplaced++;
                        writewatch::DeferredWritten(c.rangeBase, c.rangeBytes);
                    }
                    if (deferTexture)
                    {
                        if (s_deferredResolves.size() >= kMaxDeferred)
                        {
                            s_deferForRoom++;
                            WriteDeferred(0);
                        }
                        s_deferredResolves.push_back({ rangeBase, rangeBytes, rc, Image{}, VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, 0, 0, fused, layer });
                        s_deferredCount++;
                        s_deferredColour += deferColour;
                        s_deferredHle += deferHle;
                        if (deferColour)
                            LogOnce(0x3400, "colour resolves into cached textures defer their guest bytes (NFSMW_RESOLVE_DEFER_COLOR; the first: "
                                "%ux%u at %08X)", rc.width, rc.height, rc.destBase);
                        else
                            LogOnce(0x3410 + depthSource, "%s resolves into cached textures the CPU never locked defer their guest bytes "
                                "(NFSMW_RESOLVE_LAZY; the first: %ux%u at %08X)", depthSource ? "depth" : "padded colour", rc.width, rc.height, rc.destBase);
                    }
                    if (defer)
                    {
                        auto depthRecord = [](const DeferredResolve& d) { return d.texture == nullptr && !d.borrowed; };
                        if (s_deferredResolves.size() >= kMaxDeferred ||
                            size_t(std::count_if(s_deferredResolves.begin(), s_deferredResolves.end(), depthRecord)) >= kMaxDeferredDepth)
                        {
                            s_deferForRoom++;
                            auto oldest = std::find_if(s_deferredResolves.begin(), s_deferredResolves.end(), depthRecord);
                            WriteDeferred(s_deferredResolves.size() >= kMaxDeferred || oldest == s_deferredResolves.end()
                                ? 0 : size_t(oldest - s_deferredResolves.begin()));
                        }
                        auto [image, view] = SwapTargetImage(*source);
                        s_deferredResolves.push_back({ rangeBase, rangeBytes, rc, image, view, source->vkFormat, source->width, source->height });
                        s_deferredCount++;
                        if (rename)
                        {
                            // (The image goes back to the spares with the
                            // record; only a defer like this one takes a
                            // spare, and it renames the texture to its own old
                            // image, or drops the rename, first: see above.)
                            fused->renamedImage = image.image;
                            fused->renamedFormat = source->vkFormat;
                            s_renameSources[fused] = { rc, view };
                            ForgetTextureLookups(fused);
                            s_renames++;
                            s_hle.renamed += s_hleCounting;
                            LogOnce(0x3430, "depth resolves of a whole surface with its clear rename the target's image into their texture "
                                "(NFSMW_RESOLVE_RENAME; the first: %ux%u at %08X)", rc.width, rc.height, rc.destBase);
                        }
                        if (s_resolveDeferMode == 2)
                        {
                            std::lock_guard lock(s_deferCheckMutex);
                            bool first = s_deferCheckRanges.empty();
                            if (s_deferCheckRanges.size() < 8 && std::none_of(s_deferCheckRanges.begin(), s_deferCheckRanges.end(),
                                [&](const DeferCheckRange& c) { return c.base == rangeBase; }))
                                s_deferCheckRanges.push_back({ rangeBase, rangeBytes, rc.endian });
                            if (first)
                                std::thread(DeferCheck).detach();
                        }
                        LogOnce(0x3200, "resolves into cached textures defer their guest bytes (NFSMW_RESOLVE_DEFER; the first: %ux%u depth "
                            "at %08X)", rc.width, rc.height, rc.destBase);
                    }
                }
            }

            // Clears (in host target coordinates, see TileShift).
            {
                auto [shiftX, shiftY] = TileShift();
                r.x0 += shiftX;
                r.x1 += shiftX;
                r.y0 += shiftY;
                r.y1 += shiftY;
            }
            if (copyControl.color_clear_enable && !depthSource)
                ClearRect(source, r);
            if (copyControl.depth_clear_enable)
            {
                auto di = Reg<reg::RB_DEPTH_INFO>();
                ClearRect(GetTarget(di.depth_base, uint32_t(di.depth_format), true, pitch), r);
            }
            if (copyControl.color_clear_enable && depthSource)
                LogOnce(0x400, "color clear requested with a depth resolve source");
        }

        void CreateComputePipeline(const char* glsl, const VkDescriptorSetLayoutBinding* bindings, uint32_t bindingCount,
            uint32_t pushSize, VkDescriptorSetLayout& setLayout, VkPipelineLayout& layout, VkPipeline& pipeline,
            const char* preamble = "")
        {
            std::string log;
            auto spirv = CompileGlsl(glsl, GLSLANG_STAGE_COMPUTE, log, preamble);
            if (spirv.empty())
            {
                fprintf(stderr, "[renderer] compute shader failed:\n%s\n", log.c_str());
                abort();
            }
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = bindingCount;
            dslci.pBindings = bindings;
            Check(vkCreateDescriptorSetLayout(s_dev, &dslci, nullptr, &setLayout), "compute set layout");
            VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, pushSize };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &setLayout;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &range;
            Check(vkCreatePipelineLayout(s_dev, &plci, nullptr, &layout), "compute pipeline layout");
            VkShaderModule module = CreateModule(spirv);
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                module, "main", nullptr };
            cpci.layout = layout;
            Check(vkCreateComputePipelines(s_dev, s_pipelineCache, 1, &cpci, nullptr, &pipeline), "compute pipeline");
        }

        // Scaled memory's GPU side (see ScaledMemory): made on the first
        // scaled resolve.
        const char* kScaledFillGlsl = R"(#version 460
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer SharedMemory { uint g_mem[]; };)" GLSL_SHARED_MEMORY_READ R"(
layout(std430, set = 0, binding = 1) buffer ScaledPool { uint s_pool[]; };
layout(std430, set = 0, binding = 2) readonly buffer List { uint l_pairs[]; };
layout(push_constant) uniform P { uint count, slotWords, listWord, pad; uvec2 scale, den; } p;
)" GLSL_SCALE_CODE R"(
// A new slot starts as its page's 1x data, each word in the host samples its
// texel owns (all s at a whole scale; at 1.5x one, two or four of its
// block's; below 1x one or none; on a texel axis all its stride): a resolve
// covering the page only in part leaves no garbage around it.
void main()
{
    uint n = gl_GlobalInvocationID.x;
    uint pair = n >> 10, k = n & 1023u;
    if (pair >= p.count) return;
    uint page = l_pairs[p.listWord + pair * 2u], slot = l_pairs[p.listWord + pair * 2u + 1u];
    uint v = memLoad(page * 1024u + k);
    uint sX = sbStride(p.scale.x, p.den.x);
    uint o = slot * p.slotWords + sbBlockIndex(k, p.den.x, p.den.y) * (sX * sbStride(p.scale.y, p.den.y));
    uint subX = sbSubX(k, p.den.x), subY = sbSubY(k, p.den.y);
    uint i1 = sbSampleHi(subX, p.scale.x, p.den.x), j1 = sbSampleHi(subY, p.scale.y, p.den.y);
    for (uint j = sbSampleLo(subY, p.scale.y, p.den.y); j < j1; j++)
        for (uint i = sbSampleLo(subX, p.scale.x, p.den.x); i < i1; i++)
            s_pool[o + j * sX + i] = v;
}
)";
        VkPipeline s_scaledFillPipeline = VK_NULL_HANDLE;
        VkPipelineLayout s_scaledFillLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_scaledFillSetLayout = VK_NULL_HANDLE;

        // A scaled texture's texels from the scaled memory (the 32-bit tiled
        // layout of the guest texture, each texel's samples): stored into the
        // texture through its R32_UINT view, or (TO_BUFFER) linear into the
        // scratch buffer for a copy.
        const char* kScaledUntileGlsl = R"(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer ScaledPool { uint s_pool[]; };
layout(std430, set = 0, binding = 1) readonly buffer ScaledTable { uint s_table[]; };
#ifdef TO_BUFFER
layout(std430, set = 0, binding = 2) writeonly buffer Out { uint o_mem[]; };
#else
layout(set = 0, binding = 2, r32ui) writeonly uniform uimage2D o_image;
#endif
// width, height: the texture's host size (whole blocks); scale / den: host
// pixels per guest pixel (a den above 2: a texel axis).
layout(push_constant) uniform P { uint base, pitch, width, height, endian, slotWords; uvec2 scale, den; } p;
)" GLSL_SCALE_CODE R"(
uint tiled2D(uint x, uint y, uint pitch, uint bppLog2)
{
    uint outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outer | inner) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

uint swapWord(uint v, uint e)
{
    if (e == 1u) return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u) return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    if (e == 3u) return (v >> 16) | (v << 16);
    return v;
}

void main()
{
    uvec2 h = gl_GlobalInvocationID.xy;
    uint w = p.width;
    if (h.x >= w || h.y >= p.height) return;
    // Host texel h: sample (h mod scale) of the block (h / scale), whose
    // first texel is guest (block x den); on a texel axis, of the texel
    // owning it (gpu/render_scale.h).
    uvec2 t = uvec2(sbBlockTexel(h.x, p.scale.x, p.den.x), sbBlockTexel(h.y, p.scale.y, p.den.y));
    uint a = (p.base + tiled2D(t.x, t.y, p.pitch, 2u)) & 0x1FFFFFFFu;
    uint slot = s_table[a >> 12];
    uint v = 0u;
    if (slot != 0u)
        v = s_pool[(slot - 1u) * p.slotWords + sbSampleWord((a & 4095u) >> 2, sbBlockSample(h.x, p.scale.x, p.den.x),
            sbBlockSample(h.y, p.scale.y, p.den.y), p.scale.x, p.scale.y, p.den.x, p.den.y)];
#ifdef TO_BUFFER
    o_mem[h.y * w + h.x] = swapWord(v, p.endian);
#else
    imageStore(o_image, ivec2(h), uvec4(swapWord(v, p.endian)));
#endif
}
)";
        VkPipeline s_scaledUntilePipeline = VK_NULL_HANDLE;  // TO_BUFFER
        VkPipelineLayout s_scaledUntileLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_scaledUntileSetLayout = VK_NULL_HANDLE;
        VkPipeline s_scaledStorePipeline = VK_NULL_HANDLE;  // into the image
        VkPipelineLayout s_scaledStoreLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_scaledStoreSetLayout = VK_NULL_HANDLE;

        bool CreateDeviceBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer& buffer, VkDeviceMemory& memory, const char* what,
            VkDeviceSize* allocated = nullptr)
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = size;
            bci.usage = usage;
            if (vkCreateBuffer(s_dev, &bci, nullptr, &buffer) != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] %s: vkCreateBuffer failed\n", what);
                return false;
            }
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_dev, buffer, &req);
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (vkAllocateMemory(s_dev, &mai, nullptr, &memory) != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] %s: vkAllocateMemory (%llu MB) failed\n", what, (unsigned long long)(size >> 20));
                vkDestroyBuffer(s_dev, buffer, nullptr);
                buffer = VK_NULL_HANDLE;
                return false;
            }
            vkBindBufferMemory(s_dev, buffer, memory, 0);
            if (allocated)
                *allocated = req.size;
            return true;
        }

        // Scaled-texture checks by range, redone only when something was
        // written since (the write epoch moved).
        struct ScaledCheck
        {
            uint64_t epoch = ~0ull;
            bool valid = false;
            uint64_t wanted = 0;  // s_frame + 1 when a scaled reader last asked for it
            uint32_t base = 0, size = 0;
            uint64_t sig = 0;     // the scaled texture's (ScaledSignature); 0: the front buffer
        };
        std::unordered_map<uint64_t, ScaledCheck> s_scaledChecks;
        uint64_t ScaledCheckKey(uint32_t base, uint32_t size, uint64_t sig)
        {
            return ((uint64_t(base) << 32) | size) ^ (sig * 0x9E3779B97F4A7C15ull);
        }

        // Scaled resolves keep their samples only where a scaled reader (a
        // draw into scaled targets sampling a texture there, the front
        // buffer) asked for that memory in the last kScaledWantFrames
        // frames; elsewhere they write the 1x words alone, as at 1x (the
        // pages then read as 1x, as after any 1x write). The scene's depth
        // strips (D24: never a scaled texture) and the 1x-only targets'
        // sources were stored at host resolution every frame for no reader.
        // A reader that starts asking reads 1x for the frame its memory
        // takes to be resolved again. NFSMW_SCALED_WANTED=0: every scaled
        // resolve keeps its samples, as before.
        constexpr uint64_t kScaledWantFrames = 120;
        const bool s_scaledWantedOnly = [] { const char* v = std::getenv("NFSMW_SCALED_WANTED"); return !v || v[0] != '0'; }();
        // (`except`: a scaled texture's surface not to count, ScaledSignature.)
        bool ScaledWanted(uint32_t base, uint32_t size, uint32_t exceptBase, uint32_t exceptSize, uint64_t exceptSig)
        {
            for (const auto& [key, check] : s_scaledChecks)
            {
                if (!check.wanted || check.wanted + kScaledWantFrames <= s_frame + 1)
                    continue;
                if (exceptSig && check.sig == exceptSig && check.base == exceptBase && check.size == exceptSize)
                    continue;
                const uint64_t b = check.base, e = b + check.size;
                if (b < uint64_t(base) + size && uint64_t(base) < e)
                    return true;
            }
            return false;
        }
        bool ScaledWanted(uint32_t base, uint32_t size)
        {
            return !s_scaledWantedOnly || ScaledWanted(base, size, 0, 0, 0);
        }

        // The pool, page table and scratch buffer (the caller has the GPU idle).
        void FreeScaledBuffers()
        {
            for (auto [buffer, memory] : { std::pair{ &s_scaled.pool, &s_scaled.poolMemory }, std::pair{ &s_scaled.table, &s_scaled.tableMemory },
                     std::pair{ &s_scaled.scratch, &s_scaled.scratchMemory } })
            {
                if (*buffer)
                    vkDestroyBuffer(s_dev, *buffer, nullptr);
                if (*memory)
                    vkFreeMemory(s_dev, *memory, nullptr);
                *buffer = VK_NULL_HANDLE;
                *memory = VK_NULL_HANDLE;
            }
            s_scaled.poolBytes = s_scaled.tableBytes = s_scaled.scratchBytes = 0;
        }

        // The pool for `scale`: NFSMW_SCALED_MB (default 512).
        // The whole budget, whatever the scale: kept across live changes
        // of the internal resolution (ResetScaledMemory). Bound whole, so
        // within the device's storage buffer range (NFSMW_SCALED_MB=4096
        // is past a 4 GB - 1 limit). Below 1x a slot is smaller than its
        // page, and a slot for every guest page fits in less than the budget
        // (288 MB at 0.75x, 200 at 0.625x, 128 at 0.5x; above 1x the budget
        // is never enough for all of them): no more than that, for a
        // handheld's memory.
        VkDeviceSize ScaledPoolSize(const gpu::RenderScale& scale, const VkPhysicalDeviceProperties& props)
        {
            uint64_t megabytes = gpu::ScaledPoolMegabytes(scale, props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
            if (const char* v = std::getenv("NFSMW_SCALED_MB"))
                megabytes = std::clamp<uint64_t>(std::strtoull(v, nullptr, 10), 16, 4096);
            VkDeviceSize poolSize = std::min<VkDeviceSize>(VkDeviceSize(megabytes) << 20, props.limits.maxStorageBufferRange) & ~VkDeviceSize(0xFFFF);
            return std::min(poolSize, (VkDeviceSize(kGuestPages) * scale.SlotWords() * 4 + 0xFFFF) & ~VkDeviceSize(0xFFFF));
        }

        // Back to "not made yet" (a new internal resolution; the GPU idle):
        // the next scaled resolve makes it again at the new scale. Every page
        // falls back to its 1x copy until a scaled resolve writes it again.
        // `keepBuffers` (the new scale is above 1x too): the pool and the
        // page table stay allocated, as their sizes don't depend on the
        // scale; only the slots are laid out again. (Below 1x they do: the
        // caller keeps a pool only when it is big enough for the new scale.)
        void ResetScaledMemory(bool keepBuffers)
        {
            if (!keepBuffers)
                FreeScaledBuffers();
            s_scaled.tried = s_scaled.enabled = false;
            s_scaled.scale = {};
            s_scaled.slotWords = s_scaled.slots = 0;
            s_scaled.scratchSize = 0;
            std::vector<uint32_t>().swap(s_scaled.pageSlot);
            std::vector<uint64_t>().swap(s_scaled.pageSeq);
            std::vector<const void*>().swap(s_scaled.pageHeld);
            std::vector<uint32_t>().swap(s_scaled.slotPage);
            std::vector<uint8_t>().swap(s_scaled.slotRef);
            std::vector<uint16_t>().swap(s_scaled.pageWriter);
            std::vector<uint8_t>().swap(s_scaled.pageRead);
            s_scaled.writers.clear();
            s_scaled.dirty.clear();
            s_scaled.clock = 0;
            s_scaledChecks.clear();
            s_textureCache->generation++;  // lookups decided with the old scale
        }

        bool EnsureScaledMemory()
        {
            if (s_scaled.tried)
                return s_scaled.enabled;
            s_scaled.tried = true;
            if (const char* v = std::getenv("NFSMW_SCALED_RESOLVE"); v && v[0] == '0')
                return false;
            auto start = std::chrono::steady_clock::now();
            s_scaled.scale = s_renderScale;
            if (s_scaled.scale.IsOne())
                return false;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            VkDeviceSize poolSize = ScaledPoolSize(s_scaled.scale, props);
            // Kept across live changes of the internal resolution
            // (ResetScaledMemory), and made again larger for a scale that
            // wants more (below). Bound whole, so within the device's storage
            // buffer range (NFSMW_SCALED_MB=4096 is past a 4 GB - 1 limit).
            if (s_scaled.pool && s_scaled.poolBytes < poolSize)
            {
                // (Commands in flight may still bind it, as a stand-in too.)
                Flush();
                video::WaitGpuIdle();
                vkDestroyBuffer(s_dev, s_scaled.pool, nullptr);
                vkFreeMemory(s_dev, s_scaled.poolMemory, nullptr);
                s_scaled.pool = VK_NULL_HANDLE;
                s_scaled.poolMemory = VK_NULL_HANDLE;
                s_scaled.poolBytes = 0;
            }
            s_scaled.slotWords = s_scaled.scale.SlotWords();
            // (A pool kept from another scale is at least this big: ApplyRenderScale.)
            if (s_scaled.pool && s_scaled.poolBytes < poolSize)
                poolSize = s_scaled.poolBytes & ~VkDeviceSize(0xFFFF);
            s_scaled.slots = uint32_t(std::min<uint64_t>(poolSize / (4ull * s_scaled.slotWords), kGuestPages));
            // Scaled textures up to 1280x1024 (at scale). The untile stores
            // into them through an R32_UINT view when the device can for every
            // scaled format (a mutable-format image with extended usage: the
            // format itself needn't be storable); otherwise they load through
            // a scratch buffer that size (the ring's slots are too small) and a
            // copy, one more full write and read of the texture.
            s_scaled.scratchSize = VkDeviceSize(s_scaled.scale.HostPixels(1280, 1024)) * 4;
            {
                // (The image's format list is Vulkan 1.2.)
                const char* v = std::getenv("NFSMW_SCALED_DIRECT");
                VkFormatProperties fp;
                vkGetPhysicalDeviceFormatProperties(s_vk->physical, VK_FORMAT_R32_UINT, &fp);
                s_scaled.direct = !(v && v[0] == '0') && props.apiVersion >= VK_API_VERSION_1_2 &&
                    (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
                for (uint32_t f = 0; f < 64 && s_scaled.direct; f++)
                {
                    TextureFormatInfo fi;
                    VkImageFormatProperties ifp;
                    if (GetTextureFormat(f, fi) && ScaledFormat(fi))
                        s_scaled.direct = vkGetPhysicalDeviceImageFormatProperties(s_vk->physical, fi.vk, VK_IMAGE_TYPE_2D,
                            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                            VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, &ifp) == VK_SUCCESS;
                }
            }
            s_scaled.coalesce = ScaledCoalesce(props);
            // A scratch buffer kept from a smaller scale is too small.
            if (s_scaled.scratch && s_scaled.scratchBytes < s_scaled.scratchSize)
            {
                vkDestroyBuffer(s_dev, s_scaled.scratch, nullptr);
                vkFreeMemory(s_dev, s_scaled.scratchMemory, nullptr);
                s_scaled.scratch = VK_NULL_HANDLE;
                s_scaled.scratchMemory = VK_NULL_HANDLE;
                s_scaled.scratchBytes = 0;
            }
            if ((!s_scaled.pool && !CreateDeviceBuffer(poolSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, s_scaled.pool, s_scaled.poolMemory,
                     "scaled pool", &s_scaled.poolBytes)) ||
                (!s_scaled.table && !CreateDeviceBuffer(VkDeviceSize(kGuestPages) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    s_scaled.table, s_scaled.tableMemory, "scaled page table", &s_scaled.tableBytes)) ||
                (!s_scaled.direct && !s_scaled.scratch && !CreateDeviceBuffer(s_scaled.scratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    s_scaled.scratch, s_scaled.scratchMemory, "scaled scratch", &s_scaled.scratchBytes)))
            {
                fprintf(stderr, "[renderer] scaled resolve memory unavailable: textures stay 1x\n");
                FreeScaledBuffers();
                return false;
            }
            s_scaled.pageSlot.assign(kGuestPages, 0);
            s_scaled.pageSeq.assign(kGuestPages, 0);
            s_scaled.pageHeld.assign(kGuestPages, nullptr);
            s_scaled.slotPage.assign(s_scaled.slots, 0);
            s_scaled.slotRef.assign(s_scaled.slots, 0);
            s_scaled.writers.clear();
            if (s_scaled.scale.Below())
            {
                s_scaled.pageWriter.assign(kGuestPages, 0);
                s_scaled.pageRead.assign(kGuestPages, 0);
            }
            long hostPage = sysconf(_SC_PAGESIZE);
            s_scaled.hostPageGuestPages = hostPage > 4096 ? uint32_t(hostPage / 4096) : 1;
            // The pipelines take the scale in their push constants: made
            // once, kept when the memory is made again at another scale.
            VkDescriptorSetLayoutBinding b[3] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            if (!s_scaledFillPipeline)
            {
                auto bindings = WithSharedParts(b, 3);
                CreateComputePipeline(kScaledFillGlsl, bindings.data(), uint32_t(bindings.size()), 8 * sizeof(uint32_t),
                    s_scaledFillSetLayout, s_scaledFillLayout, s_scaledFillPipeline);
            }
            if (s_scaled.direct)
            {
                b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                if (!s_scaledStorePipeline)
                    CreateComputePipeline(kScaledUntileGlsl, b, 3, 10 * sizeof(uint32_t), s_scaledStoreSetLayout, s_scaledStoreLayout,
                        s_scaledStorePipeline);
            }
            else if (!s_scaledUntilePipeline)
                CreateComputePipeline(kScaledUntileGlsl, b, 3, 10 * sizeof(uint32_t), s_scaledUntileSetLayout, s_scaledUntileLayout,
                    s_scaledUntilePipeline, "#define TO_BUFFER\n");
            EndPass(kBreakUpload);
            Begin();
            vkCmdFillBuffer(s_cmd, s_scaled.table, 0, VK_WHOLE_SIZE, 0);
            FullBarrier();
            s_scaled.enabled = true;
            s_textureCache->generation++;  // lookups decided without the scaled path
            fprintf(stderr, "[renderer] scaled resolve memory: %u slots of %u KB (%llu MB), scale %s, %s writes, textures %s (%.1f ms)\n",
                s_scaled.slots, s_scaled.slotWords / 256, (unsigned long long)(poolSize >> 20), gpu::ScaleName(s_scaled.scale).c_str(),
                s_scaled.coalesce && ScaledCoalesceFits(s_scaled.scale) ? "coalesced" : "per-thread", s_scaled.direct ? "stored directly" : "through a scratch buffer",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            return true;
        }

        bool ScaledTextureValid(uint32_t base, uint32_t size, uint64_t sig)
        {
            uint64_t epoch = writewatch::WriteEpoch();
            ScaledCheck& check = s_scaledChecks[ScaledCheckKey(base, size, sig)];
            check.base = base;
            check.size = size;
            check.sig = sig;
            check.wanted = s_frame + 1;
            if (check.epoch != epoch)
            {
                check.epoch = epoch;
                check.valid = ScaledSurfaceValid(base, size, sig);
                if (!check.valid)
                    s_scaled.fallbacks++;
            }
            return check.valid;
        }

        void LoadScaledTexture(Texture& t, uint32_t base, uint32_t pitchBlocks, uint32_t endian)
        {
            VkDeviceSize bytes = VkDeviceSize(t.width) * t.height * 4;
            if (bytes > s_scaled.scratchSize)
            {
                LogOnce(0x2300, "scaled texture %ux%u is larger than the scratch buffer", t.width, t.height);
                return;
            }
            EndPass(kBreakUpload);
            Begin();
            s_scaled.loads++;
            s_scaled.untiledBytes += bytes;
            VkDescriptorBufferInfo bi[3] = { { s_scaled.pool, 0, VK_WHOLE_SIZE }, { s_scaled.table, 0, VK_WHOLE_SIZE },
                { s_scaled.scratch, 0, VK_WHOLE_SIZE } };
            VkWriteDescriptorSet w[3];
            for (uint32_t k = 0; k < 3; k++)
                w[k] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, k, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    nullptr, &bi[k], nullptr };
            const gpu::RenderScale& sc = s_scaled.scale;
            struct { uint32_t base, pitch, width, height, endian, slotWords, scaleX, scaleY, denX, denY; } pc = { base, pitchBlocks,
                t.width, t.height, endian, s_scaled.slotWords, sc.x.num, sc.y.num, sc.x.den, sc.y.den };
            ComputeWorkRecorded();
            if (s_profiling)
                ProfileCompute(std::format("scaled texture {}x{} at {:08X}", t.width, t.height, base).c_str());
            if (t.storeView)
            {
                // Straight into the image. Everything before is behind a full
                // barrier already (as every pass and dispatch is; with
                // NFSMW_PASS_BARRIER a pass's narrower one still holds compute
                // work), including the draws that sampled the last load and
                // the resolves that wrote the pool, so only the stores need
                // ordering after.
                if (!t.uploaded)
                    ToGeneral(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
                VkDescriptorImageInfo ii{ VK_NULL_HANDLE, t.storeView, VK_IMAGE_LAYOUT_GENERAL };
                w[2] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                    &ii, nullptr, nullptr };
                vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledStorePipeline);
                s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledStoreLayout, 0, 3, w);
                vkCmdPushConstants(s_cmd, s_scaledStoreLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
                vkCmdDispatch(s_cmd, (t.width + 7) / 8, (t.height + 7) / 8, 1);
                ComputeBarrier();
                ProfileComputeEnd();
                return;
            }
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledUntilePipeline);
            s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledUntileLayout, 0, 3, w);
            vkCmdPushConstants(s_cmd, s_scaledUntileLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(s_cmd, (t.width + 7) / 8, (t.height + 7) / 8, 1);
            FullBarrier();
            if (!t.uploaded)
                ToGeneral(t.image.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            VkBufferImageCopy region{};
            region.bufferRowLength = t.width;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { t.width, t.height, 1 };
            vkCmdCopyBufferToImage(s_cmd, s_scaled.scratch, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            FullBarrier();
            ProfileComputeEnd();
        }

        void FillFreshSlots(const std::vector<std::pair<uint32_t, uint32_t>>& fresh)
        {
            for (size_t first = 0; first < fresh.size(); first += 4096)
            {
                uint32_t count = uint32_t(std::min<size_t>(4096, fresh.size() - first));
                VkDeviceSize list = RingAllocate(VkDeviceSize(count) * 8);
                if (list == ~VkDeviceSize(0))
                    return;
                uint32_t* out = reinterpret_cast<uint32_t*>(s_ringHost + list);
                for (uint32_t i = 0; i < count; i++)
                {
                    out[i * 2] = fresh[first + i].first;
                    out[i * 2 + 1] = fresh[first + i].second;
                    EnsureShadow(fresh[first + i].first << 12, 4096);
                }
                EndPass(kBreakUpload);
                Begin();
                VkDescriptorBufferInfo bi[3] = { s_sharedInfo[0], { s_scaled.pool, 0, VK_WHOLE_SIZE }, { s_ring, 0, kRingSize } };
                VkWriteDescriptorSet w[6];
                for (uint32_t k = 0; k < 3; k++)
                    w[k] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, k, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        nullptr, &bi[k], nullptr };
                const gpu::RenderScale& sc = s_scaled.scale;
                struct { uint32_t count, slotWords, listWord, pad, scaleX, scaleY, denX, denY; } pc = { count, s_scaled.slotWords,
                    uint32_t(list / 4), 0, sc.x.num, sc.y.num, sc.x.den, sc.y.den };
                ComputeWorkRecorded();
                if (s_profiling)
                    ProfileCompute(std::format("scaled memory: {} fresh slots filled", count).c_str());
                vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledFillPipeline);
                s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_scaledFillLayout, 0, AddSharedParts(w, 3), w);
                vkCmdPushConstants(s_cmd, s_scaledFillLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
                vkCmdDispatch(s_cmd, (count * 1024 + 63) / 64, 1, 1);
                FullBarrier();
                ProfileComputeEnd();
            }
        }

        // Front buffer -> RGBA8 image on the GPU (the presenter blits it), so a
        // swap needs neither a CPU wait nor a CPU untile.
        const char* kFrontGlsl = R"(#version 460
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer SharedMemory { uint g_mem[]; };)" GLSL_SHARED_MEMORY_READ R"(
layout(set = 0, binding = 1, rgba8) writeonly uniform image2D outImage;
layout(std430, set = 0, binding = 2) readonly buffer ScaledPool { uint s_pool[]; };
layout(std430, set = 0, binding = 3) readonly buffer ScaledTable { uint s_table[]; };
// width, height: the output (host pixels when scaled: from the scaled memory,
// at sx / denX x sy / denY host pixels per guest pixel).
layout(push_constant) uniform P { uint base, pitch, tiled, endian, format, swizzle, width, height, sx, sy, slotWords, scaled, denX, denY; } p;
)" GLSL_SCALE_CODE R"(
uint tiled2D(uint x, uint y, uint pitch, uint bppLog2)
{
    uint outer = (((y >> 5) * (pitch >> 5)) + (x >> 5)) << 6;
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint oi = (outer | inner) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (oi & 0xFu) | (((oi >> 4) & 1u) << 5) |
        (((oi >> 5) & 7u) << 8) | ((oi >> 8) << 12);
}

void main()
{
    uvec2 g = gl_GlobalInvocationID.xy;
    if (g.x >= p.width || g.y >= p.height) return;
    uint v;
    if (p.scaled != 0u)
    {
        // Host pixel g: sample (g mod scale) of the block (g / scale), whose
        // first texel is guest (block x den); on a texel axis, of the texel
        // owning it (gpu/render_scale.h).
        // (SB_FIXED_*: the scale as constants, a pipeline made for it: no
        // runtime divisions, see the resolve shader.)
#ifdef SB_FIXED_NUM_X
        const uint sx = SB_FIXED_NUM_X, sy = SB_FIXED_NUM_Y, denX = SB_FIXED_DEN_X, denY = SB_FIXED_DEN_Y;
#else
        uint sx = p.sx, sy = p.sy, denX = p.denX, denY = p.denY;
#endif
        uint tx = sbBlockTexel(g.x, sx, denX), ty = sbBlockTexel(g.y, sy, denY);
        uint a = (p.base + tiled2D(tx, ty, p.pitch, 2u)) & 0x1FFFFFFFu;
        uint slot = s_table[a >> 12];
        v = slot != 0u ? s_pool[(slot - 1u) * p.slotWords + sbSampleWord((a & 4095u) >> 2, sbBlockSample(g.x, sx, denX),
            sbBlockSample(g.y, sy, denY), sx, sy, denX, denY)] : 0u;
    }
    else
    {
        uint addr = p.base + (p.tiled != 0u ? tiled2D(g.x, g.y, p.pitch, 2u) : (g.y * p.pitch + g.x) * 4u);
        v = memLoad((addr & 0x1FFFFFFFu) >> 2);
    }
    if (p.endian == 1u) v = ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    else if (p.endian == 2u) v = (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    else if (p.endian == 3u) v = (v >> 16) | (v << 16);
    float c[6];
    if (p.format == 7u || p.format == 54u)
    {
        c[0] = float(v & 0x3FFu) / 1023.0; c[1] = float((v >> 10) & 0x3FFu) / 1023.0;
        c[2] = float((v >> 20) & 0x3FFu) / 1023.0; c[3] = float(v >> 30) / 3.0;
    }
    else
    {
        for (int i = 0; i < 4; i++) c[i] = float((v >> (8 * i)) & 0xFFu) / 255.0;
    }
    c[4] = 0.0; c[5] = 1.0;
    vec3 rgb = vec3(c[min(p.swizzle & 7u, 5u)], c[min((p.swizzle >> 3) & 7u, 5u)], c[min((p.swizzle >> 6) & 7u, 5u)]);
    imageStore(outImage, ivec2(g), vec4(rgb, 1.0));
}
)";

        VkPipeline s_frontPipeline = VK_NULL_HANDLE;
        VkPipelineLayout s_frontLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_frontSetLayout = VK_NULL_HANDLE;
        // The same for a texel axis's scale, made for that exact scale
        // (SB_FIXED_*) at its first scaled present (Steam Deck 1x10/9: the
        // divisions by the push constants were most of the front's cost).
        VkPipeline s_frontFixedPipeline = VK_NULL_HANDLE;
        VkPipelineLayout s_frontFixedLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout s_frontFixedSetLayout = VK_NULL_HANDLE;
        gpu::RenderScale s_frontFixedScale;
        // The front buffer as presented: 1x, and at host resolution when the
        // scaled resolve memory has it (two rings: no stall when the game
        // switches between them, e.g. around movies). Each is a ring of
        // images of one size, and a swap writes one the presenter doesn't
        // hold (gpu/front_ring.h, video::ClaimFrontImage): one image written
        // at every swap was written again, now and then, before the
        // presenter had queued its read of the frame before.
        struct FrontRing
        {
            std::array<Image, gpu::kMaxFrontImages> images;
            // NFSMW_RESOLVE_PRESENT=2: a slot shows a target's image handed
            // over (external) instead of its own; `current` is the image the
            // presenter is given for the slot (its own when none is handed over).
            std::array<SpareTarget, gpu::kMaxFrontImages> external{};
            std::array<VkImage, gpu::kMaxFrontImages> current{};
            uint32_t width = 0, height = 0;
        };
        FrontRing s_front, s_frontScaled;
        // The same for in-between pictures (game/hfr.h), so the real
        // picture right after one doesn't overwrite it before the
        // presenter has read it.
        FrontRing s_frontBetween, s_frontBetweenScaled;
        // NFSMW_FRONT_IMAGES=<1..3>: the images a ring has (default 2; 1 is
        // v0.1.4's single image, to compare: the presenter logs the frames
        // it loses then).
        const int s_frontImages = [] {
            const char* v = std::getenv("NFSMW_FRONT_IMAGES");
            return v ? std::clamp(std::atoi(v), 1, gpu::kMaxFrontImages) : gpu::kFrontImages;
        }();

        // The slot of `ring` the next swap presents (gpu/front_ring.h): the
        // presenter neither holds its image nor waits to take it, so its read
        // of that image is queued already: a hand-over the slot holds goes
        // back to the spare target images (or stays with the record owing
        // its guest bytes), and the slot shows its own image again.
        int ClaimFrontSlot(FrontRing& ring)
        {
            std::array<VkImage, gpu::kMaxFrontImages> handles{};
            for (int i = 0; i < s_frontImages; i++)
                handles[i] = ring.current[i] ? ring.current[i] : ring.images[i].image;
            const int slot = video::ClaimFrontImage(handles.data(), s_frontImages);
            SpareTarget& external = ring.external[size_t(slot)];
            if (external.image.image)
            {
                // A record still owing the image's guest bytes (the frame was
                // never shown, taken back for a newer one, and the next
                // resolve into its front buffer hasn't replaced it yet) keeps
                // the image as its own: it goes to the spares with the record.
                bool owed = false;
                for (DeferredResolve& d : s_deferredResolves)
                    if (d.borrowed && d.image.image == external.image.image)
                    {
                        d.borrowed = false;
                        owed = true;
                        break;
                    }
                if (!owed)
                    s_spareTargets.push_back(external);
                external = {};
            }
            ring.current[size_t(slot)] = ring.images[size_t(slot)].image;
            return slot;
        }

        bool ClaimFrontForResolve(uint32_t width, uint32_t height, VkImage& image, VkImageView& view)
        {
            FrontRing& ring = s_front;
            if (ring.width != width || ring.height != height || !ring.images[0].image)
                return false;
            const Image& front = ring.images[size_t(ClaimFrontSlot(ring))];
            image = front.image;
            view = front.view;
            return true;
        }

        int ClaimFrontSlotForHandover(uint32_t width, uint32_t height)
        {
            FrontRing& ring = s_front;
            if (ring.width != width || ring.height != height || !ring.images[0].image)
                return -1;
            return ClaimFrontSlot(ring);
        }

        void HandOverFront(int slot, const Image& image, VkImageView view, VkFormat format, uint32_t width, uint32_t height)
        {
            FrontRing& ring = s_front;
            ring.external[size_t(slot)] = { image, view, format, width, height };
            ring.current[size_t(slot)] = image.image;
        }
        // The image the last PresentFrontBuffer wrote, for ReadBackFrontImage
        // (the timedemo's shots); null once destroyed.
        struct LastFront
        {
            VkImage image = VK_NULL_HANDLE;
            uint32_t width = 0, height = 0;
        };
        LastFront s_lastFront;
        // Shot copies whose thread hasn't finished with their buffer yet
        // (Shutdown waits for them a moment).
        std::atomic<int> s_frontReadbacks{ 0 };

        // A ring of front images the presenter may still show or be reading:
        // taken back from it, then destroyed once the queue is idle.
        void DestroyFrontImages(FrontRing& ring)
        {
            Flush();
            for (size_t i = 0; i < ring.images.size(); i++)
            {
                if (ring.current[i])
                    video::ReleaseFrontImage(ring.current[i]);
                if (ring.images[i].image && ring.images[i].image != ring.current[i])
                    video::ReleaseFrontImage(ring.images[i].image);
            }
            video::WaitGpuIdle();
            for (size_t i = 0; i < ring.images.size(); i++)
            {
                Image& front = ring.images[i];
                if (front.image == s_lastFront.image || ring.current[i] == s_lastFront.image)
                    s_lastFront = {};
                if (ring.external[i].image.image)
                    s_spareTargets.push_back(ring.external[i]);
                ring.external[i] = {};
                ring.current[i] = VK_NULL_HANDLE;
                DestroyImage(front);
            }
            ring.width = ring.height = 0;
        }

        void CreateUntilePipeline()
        {
            VkDescriptorSetLayoutBinding b[2] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            auto bindings = WithSharedParts(b, 2);
            CreateComputePipeline(kUntileGlsl, bindings.data(), uint32_t(bindings.size()), sizeof(UntileConstants), s_untileSetLayout,
                s_untileLayout, s_untilePipeline);
            b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings = WithSharedParts(b, 2);
            CreateComputePipeline(kUntileGlsl, bindings.data(), uint32_t(bindings.size()), sizeof(UntileConstants), s_untileStoreSetLayout,
                s_untileStoreLayout, s_untileStorePipeline, "#define TO_IMAGE\n");
        }

        // The resolve shader for sources whose scale has these denominators
        // (1, 2, 4 or 8 each; the shader's SB_DEN_X, SB_DEN_Y; 0 for a
        // texel axis, SB_TEXEL_X, SB_TEXEL_Y: its exact scale from `fixed`,
        // SB_FIXED_*), into `out`; `textureOnly`: its TEXTURE_ONLY variant,
        // `writeback` 1 its TEXTURE_WRITEBACK one, 2 that with WRITEBACK_CHECK
        // (whole scales), 3 FRONT_DIRECT (NFSMW_RESOLVE_PRESENT); `scaled`: a
        // source above 1x (SCALED_FUSE).
        void MakeResolvePipeline(bool coalesced, uint32_t denX, uint32_t denY, VkPipeline* out, bool textureOnly, uint32_t writeback,
            bool scaled, const gpu::RenderScale* fixed)
        {
            std::string log;
            std::string preamble = coalesced ? "#define COALESCED\n" : textureOnly ? "#define TEXTURE_ONLY\n"
                : writeback == 2 ? "#define TEXTURE_WRITEBACK\n#define WRITEBACK_CHECK\n" : writeback == 3 ? "#define FRONT_DIRECT\n"
                : writeback ? "#define TEXTURE_WRITEBACK\n" : "";
            if (fixed)
                preamble += std::format("#define SB_FIXED_NUM_X {}u\n#define SB_FIXED_DEN_X {}u\n#define SB_FIXED_NUM_Y {}u\n#define SB_FIXED_DEN_Y {}u\n",
                    fixed->x.num, fixed->x.den, fixed->y.num, fixed->y.den);
            if (s_resolvePush != offsetof(ResolveConstants, texX) || scaled)
                preamble += "#define TEX_OFFSET\n";
            if (scaled)
                preamble += "#define SCALED_FUSE\n";
            if (denX == 0)
                preamble += "#define SB_TEXEL_X 1\n";
            else if (denX != 1)
                preamble += std::format("#define SB_DEN_X {}u\n", denX);
            if (denY == 0)
                preamble += "#define SB_TEXEL_Y 1\n";
            else if (denY != 1)
                preamble += std::format("#define SB_DEN_Y {}u\n", denY);
            auto spirv = CompileGlsl(kResolveGlsl, GLSLANG_STAGE_COMPUTE, log, preamble);
            if (spirv.empty())
            {
                fprintf(stderr, "[renderer] resolve shader failed:\n%s\n", log.c_str());
                abort();
            }
            VkShaderModule module = CreateModule(spirv);
            VkComputePipelineCreateInfo cpci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
            cpci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                module, "main", nullptr };
            cpci.layout = s_resolveLayout;
            Check(vkCreateComputePipelines(s_dev, s_pipelineCache, 1, &cpci, nullptr, out), "resolve pipeline");
        }

        // The resolve pipeline for a source at `scale` (see s_resolveDenPipelines).
        VkPipeline ResolvePipeline(bool coalesced, const gpu::RenderScale& scale)
        {
            // (1, 2, 4 or 8 a block axis's den, gpu/render_scale.h; 0: a
            // texel axis.)
            const uint32_t dx = scale.x.Texel() ? 0 : std::clamp(scale.x.den, 1u, 8u), dy = scale.y.Texel() ? 0 : std::clamp(scale.y.den, 1u, 8u);
            // Above 1x: SCALED_FUSE (below 1x, as before, not).
            const bool scaled = !scale.IsOne() && !scale.Below();
            // A texel axis's: one for its exact scale (SB_FIXED_*), kept per
            // scale (a live change to another makes its own).
            const bool fixed = scaled && (dx == 0 || dy == 0);
            static std::map<std::tuple<bool, uint32_t, uint32_t, uint32_t, uint32_t>, VkPipeline> s_fixedPipelines;
            VkPipeline& slot = fixed ? s_fixedPipelines[{ coalesced, scale.x.num, scale.x.den, scale.y.num, scale.y.den }]
                : (scaled ? s_resolveScaledPipelines : s_resolveDenPipelines)[coalesced][std::countr_zero(std::max(dx, 1u))]
                    [std::countr_zero(std::max(dy, 1u))];
            if (!slot)
            {
                auto start = std::chrono::steady_clock::now();
                MakeResolvePipeline(coalesced, dx, dy, &slot, false, 0, scaled, fixed ? &scale : nullptr);
                fprintf(stderr, "[renderer] resolve pipeline for scale denominators %sx%s%s%s made (%.1f ms)\n",
                    dx ? std::to_string(dx).c_str() : "texel", dy ? std::to_string(dy).c_str() : "texel", coalesced ? ", coalesced" : "",
                    scaled ? ", scaled source" : "",
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            return slot;
        }

        void CreateResolvePipeline()
        {
            VkDescriptorSetLayoutBinding b[6] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },  // scaled pool
                { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },  // scaled page table
                { 4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },   // a cached texture (or a dummy)
                { 5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },   // a cached scaled texture (or a dummy)
            };
            auto bindings = WithSharedParts(b, 6);
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
            dslci.bindingCount = uint32_t(bindings.size());
            dslci.pBindings = bindings.data();
            Check(vkCreateDescriptorSetLayout(s_dev, &dslci, nullptr, &s_resolveSetLayout), "resolve set layout");
            VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, uint32_t(sizeof(ResolveConstants)) };
            VkPipelineLayoutCreateInfo plci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            plci.setLayoutCount = 1;
            plci.pSetLayouts = &s_resolveSetLayout;
            plci.pushConstantRangeCount = 1;
            plci.pPushConstantRanges = &range;
            Check(vkCreatePipelineLayout(s_dev, &plci, nullptr, &s_resolveLayout), "resolve pipeline layout");
            for (bool coalesced : { false, true })
                MakeResolvePipeline(coalesced, 1, 1, coalesced ? &s_resolveCoalescedPipeline : &s_resolvePipeline);
            s_resolveDenPipelines[0][0][0] = s_resolvePipeline;
            s_resolveDenPipelines[1][0][0] = s_resolveCoalescedPipeline;
            PrewarmResolvePipelines(s_renderScale);
        }

        // ---------------------------------------------------------------

        // Where guest memory and the upload ring go: gpu/memory_plan.h, which
        // lists the places to try in order. A place the driver refuses (out of
        // memory, say) is logged and the next one tried; only when none is
        // left does the renderer give up.
        memplan::Memory s_planMemory;  // the plan's view of the device's memory
        uint32_t s_guestType = memplan::kNoType;  // the copy's memory type (kNoType: imported)
        std::string s_guestChoice;  // for the [renderer] memory line
        memplan::Gpu s_planGpu = memplan::Gpu::kOther;  // the kind the memory was planned for (memplan::OverPcieNote)

        // Test hooks for the plan's fallbacks, which the machines we test on
        // never need:
        //   NFSMW_TEST_BAR_HEAP_MB=<MB>: the heaps holding CPU-mapped VRAM read
        //     as that size to the plan, and the copy's and the ring's
        //     allocations from those types fail (out of device memory) once
        //     they would pass it, as on a GPU without resizable BAR;
        //   NFSMW_TEST_ALLOC_FAIL=<places>: those places fail as out of device
        //     memory (comma-separated: import, copy-vram, copy-system,
        //     copy-cached, ring-vram, ring-cached, ring-system);
        //   NFSMW_TEST_GPU_KIND=integrated|discrete|other: the plan takes the
        //     GPU for that kind (the Deck's choice on the Steam Machine, say).
        struct MemoryTest
        {
            uint64_t barBytes = 0, barUsed = 0;
            std::string fail;  // ",a,b,"
            const char* kind = nullptr;
        };
        MemoryTest s_memoryTest = [] {
            MemoryTest t;
            if (const char* v = std::getenv("NFSMW_TEST_BAR_HEAP_MB"); v && std::atoi(v) > 0)
                t.barBytes = uint64_t(std::atoi(v)) << 20;
            if (const char* v = std::getenv("NFSMW_TEST_ALLOC_FAIL"); v && v[0])
                t.fail = std::string(",") + v + ",";
            t.kind = std::getenv("NFSMW_TEST_GPU_KIND");
            return t;
        }();

        // NFSMW_TEST_ALLOC_FAIL names this place.
        bool MemoryTestRefuses(memplan::Place place, bool ring)
        {
            const char* name = memplan::Name(place, ring);
            if (s_memoryTest.fail.find(std::string(",") + name + ",") == std::string::npos)
                return false;
            fprintf(stderr, "[renderer] NFSMW_TEST_ALLOC_FAIL: %s refused (out of device memory)\n", name);
            return true;
        }

        // vkAllocateMemory for a planned place, through the test hooks.
        VkResult AllocatePlanned(const memplan::Candidate& c, bool ring, VkDeviceSize size, VkDeviceMemory& memory)
        {
            if (MemoryTestRefuses(c.place, ring))
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            const VkMemoryPropertyFlags bar = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            bool mappedVram = (s_memProps.memoryTypes[c.type].propertyFlags & bar) == bar;
            if (mappedVram && s_memoryTest.barBytes && s_memoryTest.barUsed + size > s_memoryTest.barBytes)
            {
                fprintf(stderr, "[renderer] NFSMW_TEST_BAR_HEAP_MB: %llu MB of CPU-mapped VRAM refused (%llu of %llu MB taken)\n",
                    (unsigned long long)(size >> 20), (unsigned long long)(s_memoryTest.barUsed >> 20),
                    (unsigned long long)(s_memoryTest.barBytes >> 20));
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = size;
            mai.memoryTypeIndex = c.type;
            VkResult r = vkAllocateMemory(s_dev, &mai, nullptr, &memory);
            if (r == VK_SUCCESS && mappedVram)
                s_memoryTest.barUsed += size;
            return r;
        }

        // The device's memory as the plan sees it (NFSMW_TEST_BAR_HEAP_MB
        // applied), logged once: bug reports then show what a GPU offers.
        memplan::Memory PlanMemory()
        {
            memplan::Memory m;
            std::string line;
            for (uint32_t h = 0; h < s_memProps.memoryHeapCount; h++)
            {
                m.heaps.push_back(s_memProps.memoryHeaps[h].size);
                line += std::format("{}{} {}{}", h ? ", " : "", h, memplan::Size(m.heaps.back()),
                    (s_memProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " device-local" : "");
            }
            line += "; types (type:heap/flags):";
            for (uint32_t i = 0; i < s_memProps.memoryTypeCount; i++)
            {
                m.types.push_back({ s_memProps.memoryTypes[i].propertyFlags, s_memProps.memoryTypes[i].heapIndex });
                line += std::format(" {}:{}/{:X}", i, m.types.back().heap, m.types.back().flags);
            }
            if (s_memoryTest.barBytes)
            {
                memplan::ShrinkMappedVram(m, s_memoryTest.barBytes);
                line += std::format(" (NFSMW_TEST_BAR_HEAP_MB: CPU-mapped VRAM's heaps read as {} MB)", s_memoryTest.barBytes >> 20);
            }
            fprintf(stderr, "[renderer] memory heaps: %s\n", line.c_str());
            return m;
        }

        uint32_t DriverId(const VkPhysicalDeviceProperties& props)
        {
            if (props.apiVersion < VK_API_VERSION_1_2)
                return 0;
            VkPhysicalDeviceDriverProperties driver{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            props2.pNext = &driver;
            vkGetPhysicalDeviceProperties2(s_vk->physical, &props2);
            return uint32_t(driver.driverID);
        }

        // Guest physical memory imported without a copy (unified memory, or
        // a discrete GPU reading system memory over PCIe).
        bool ImportSharedMemory()
        {
            if (!s_vk->getMemoryHostPointerProperties)  // (no host-memory import)
                return false;
            if (MemoryTestRefuses(memplan::Place::kImport, false))
            {
                fprintf(stderr, "[renderer] importing guest memory failed (%d)\n", int(VK_ERROR_OUT_OF_DEVICE_MEMORY));
                return false;
            }
            VkMemoryHostPointerPropertiesEXT hpp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            if (s_vk->getMemoryHostPointerProperties(s_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                    s_sharedHost, &hpp) != VK_SUCCESS)
                return false;

            VkExternalMemoryBufferCreateInfo embci{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
            embci.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.pNext = &embci;
            bci.size = kSharedSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            Check(vkCreateBuffer(s_dev, &bci, nullptr, &s_shared), "vkCreateBuffer(shared)");
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_dev, s_shared, &req);

            VkImportMemoryHostPointerInfoEXT import{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
            import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
            import.pHostPointer = s_sharedHost;
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.pNext = &import;
            mai.allocationSize = kSharedSize;
            mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits & hpp.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkResult r = vkAllocateMemory(s_dev, &mai, nullptr, &s_sharedMemory);
            if (r == VK_SUCCESS && (r = vkBindBufferMemory(s_dev, s_shared, s_sharedMemory, 0)) != VK_SUCCESS)
            {
                vkFreeMemory(s_dev, s_sharedMemory, nullptr);
                s_sharedMemory = VK_NULL_HANDLE;
            }
            if (r != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] importing guest memory failed (%d)\n", int(r));
                vkDestroyBuffer(s_dev, s_shared, nullptr);
                s_shared = VK_NULL_HANDLE;
                return false;
            }
            return true;
        }

        // The buffer of guest memory's copy (an unbound one: a place that
        // fails before binding leaves it for the next).
        VkBuffer NewCopyBuffer(VkMemoryRequirements& req)
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = kSharedSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkBuffer buffer = VK_NULL_HANDLE;
            Check(vkCreateBuffer(s_dev, &bci, nullptr, &buffer), "vkCreateBuffer(shadow)");
            vkGetBufferMemoryRequirements(s_dev, buffer, &req);
            return buffer;
        }

        // Guest memory's copy for the GPU in `c`'s memory type, kept current
        // page by page from the write watch (writewatch::SyncShadow): on a
        // discrete GPU in VRAM the CPU writes through resizable BAR when it
        // holds the copy, else in system memory; on an integrated one in
        // system memory (memory_plan.h). False, with nothing kept, when the
        // driver refuses it; `buffer` is then still unbound, or replaced.
        bool CreateShadowSharedMemory(VkBuffer& buffer, const VkMemoryRequirements& req, const memplan::Candidate& c)
        {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void* p = nullptr;
            const char* what = "vkAllocateMemory(shadow)";
            VkResult r = AllocatePlanned(c, false, req.size, memory);
            if (r == VK_SUCCESS && (r = vkMapMemory(s_dev, memory, 0, VK_WHOLE_SIZE, 0, &p)) != VK_SUCCESS)
                what = "vkMapMemory(shadow)";
            else if (r == VK_SUCCESS && (r = vkBindBufferMemory(s_dev, buffer, memory, 0)) != VK_SUCCESS)
            {
                what = "vkBindBufferMemory(shadow)";
                vkDestroyBuffer(s_dev, buffer, nullptr);  // (a buffer binds once)
                VkMemoryRequirements again;
                buffer = NewCopyBuffer(again);
            }
            if (r != VK_SUCCESS)
            {
                fprintf(stderr, "[renderer] %s failed: %d (%s, memory type %u): trying the next place\n", what, int(r),
                    memplan::Name(c.place, false), c.type);
                if (memory)
                    vkFreeMemory(s_dev, memory, nullptr);
                return false;
            }
            s_shared = buffer;
            s_sharedMemory = memory;
            // Write every page of the mapping once, before anything uses it.
            // The first CPU write to a page of mapped VRAM faults it into the
            // page tables; left to the first sync, a burst of new textures
            // paid it mid-frame (on the Steam Machine, copying 7.9 MB of
            // never-copied pages took 3.9 ms instead of 2.9, a scene load's
            // 44 MB 22 ms instead of 14-17), while all 512 MB in order take
            // ~100 ms here. The zeros are never read: SyncShadow copies a page
            // before its first use. NFSMW_SHADOW_PREFAULT=0 leaves the faults
            // to the first sync.
            static const bool prefault = [] { const char* v = std::getenv("NFSMW_SHADOW_PREFAULT"); return !v || v[0] != '0'; }();
            auto prefaultStart = std::chrono::steady_clock::now();
            if (prefault)
                for (VkDeviceSize offset = 0; offset < kSharedSize; offset += 4096)
                    static_cast<volatile uint8_t*>(p)[offset] = 0;
            double prefaultMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prefaultStart).count();
            writewatch::EnableShadow(static_cast<uint8_t*>(p));
            s_shadowMode = true;
            s_shadowInVram = (s_memProps.memoryTypes[c.type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
            fprintf(stderr, "[renderer] guest memory: GPU copy in %s (memory type %u, flags %X), updated from the write watch%s\n",
                s_shadowInVram ? "VRAM (CPU-mapped)" : "host memory", c.type, s_memProps.memoryTypes[c.type].propertyFlags,
                prefault ? std::format(" (pages touched in {:.0f} ms)", prefaultMs).c_str() : "");
            return true;
        }

        // `failed`, a list of place names, as the log's ", after ... failed".
        std::string AfterFailed(const std::string& failed)
        {
            return failed.empty() ? std::string() : " after " + failed + " failed";
        }

        void CreateSharedMemory()
        {
            s_sharedHost = static_cast<uint8_t*>(g_memory.Translate(vmem::Physical().Base()));
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            VkPhysicalDeviceFeatures features;
            vkGetPhysicalDeviceFeatures(s_vk->physical, &features);  // the presenter enables anisotropy when supported
            s_maxAnisotropy = features.samplerAnisotropy ? props.limits.maxSamplerAnisotropy : 1.0f;
            s_autoAnisotropy = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 16 : 4;
#if defined(__APPLE__) && TARGET_OS_IOS
            // An M-series iPad renders at 1x (ReadRenderScale), where 16x
            // cost nothing measurable (iPad Pro M2, scripted race: 60 fps in
            // the race and in the heavy windows, as at 4x). The A-series
            // GPUs of iPhones are unmeasured: 4x there.
            if (std::string_view(props.deviceName).starts_with("Apple M"))
                s_autoAnisotropy = 16;
#endif
            {
                int anisotropy = settings::GetInt(settings::Id::Anisotropy);
                if (anisotropy == settings::kAnisotropyAuto)
                    anisotropy = s_autoAnisotropy;
                fprintf(stderr, "[renderer] anisotropic filtering: %s\n",
                    anisotropy < 0 ? "the game's" : std::format("{}x", std::min(float(anisotropy), s_maxAnisotropy)).c_str());
            }

            s_planMemory = PlanMemory();
            memplan::GuestInput in;
            in.gpu = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? memplan::Gpu::kDiscrete
                : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? memplan::Gpu::kIntegrated : memplan::Gpu::kOther;
            if (const char* kind = s_memoryTest.kind)
            {
                std::string_view k(kind);
                in.gpu = k == "integrated" ? memplan::Gpu::kIntegrated : k == "discrete" ? memplan::Gpu::kDiscrete
                    : k == "other" ? memplan::Gpu::kOther : in.gpu;
                fprintf(stderr, "[renderer] NFSMW_TEST_GPU_KIND=%s: memory planned as for %s GPU\n", kind,
                    in.gpu == memplan::Gpu::kDiscrete ? "a discrete" : in.gpu == memplan::Gpu::kIntegrated ? "an integrated" : "another");
            }
            in.importable = s_vk->getMemoryHostPointerProperties != nullptr;
            in.importRefusal = memplan::ImportRefusal(DriverId(props));
            in.overrides = memplan::ParseOverrides(std::getenv("NFSMW_SHARED_MEMORY"), std::getenv("NFSMW_SHADOW_MEMORY"),
                std::getenv("NFSMW_RING_MEMORY"));
            VkMemoryRequirements req;
            VkBuffer copy = NewCopyBuffer(req);
            in.bits = req.memoryTypeBits;
            s_planGpu = in.gpu;
            memplan::Plan plan = memplan::PlanGuestMemory(s_planMemory, in);
            std::string failed;
            const memplan::Candidate* placed = nullptr;
            for (const memplan::Candidate& c : plan.order)
            {
                if (c.place == memplan::Place::kImport ? ImportSharedMemory() : CreateShadowSharedMemory(copy, req, c))
                {
                    placed = &c;
                    break;
                }
                failed += std::string(failed.empty() ? "" : ", ") + memplan::Name(c.place, false);
            }
            if (!placed)
            {
                fprintf(stderr, "[renderer] guest memory: no place could hold it (%s failed; %s)\n", failed.c_str(), plan.why.c_str());
                abort();
            }
            if (placed->place == memplan::Place::kImport)
            {
                vkDestroyBuffer(s_dev, copy, nullptr);
                fprintf(stderr, "[renderer] guest memory: imported (zero-copy)\n");
                s_guestChoice = "imported (zero-copy)";
            }
            else
            {
                s_guestType = placed->type;
                s_guestChoice = std::format("as a GPU copy in {}, memory type {}", placed->place == memplan::Place::kVram ? "CPU-mapped VRAM"
                    : placed->place == memplan::Place::kCached ? "cached host memory" : "host memory", placed->type);
            }
            s_guestChoice += " (" + plan.why + ")" + AfterFailed(failed);
        }

        void CreateRing()
        {
            VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = kRingSize;
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            Check(vkCreateBuffer(s_dev, &bci, nullptr, &s_ring), "vkCreateBuffer(ring)");
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(s_dev, s_ring, &req);
            // Device-local when the host can map it (resizable BAR) and it
            // fits, so the GPU reads constants and indices from VRAM; else
            // cached system memory (memory_plan.h). NFSMW_RING_MEMORY=cached
            // prefers cached system memory anyway: the CPU's ~40 MB of
            // writes a frame then aren't write-combined PCIe stores (an A/B
            // experiment: on the Steam Machine 2% less CPU, twice the GPU
            // time); =vram|system name the other places.
            memplan::Overrides o = memplan::ParseOverrides(nullptr, nullptr, std::getenv("NFSMW_RING_MEMORY"));
            memplan::Plan plan = memplan::PlanRing(s_planMemory, req.memoryTypeBits, s_guestType, o.ring);
            std::string failed;
            const memplan::Candidate* placed = nullptr;
            for (const memplan::Candidate& c : plan.order)
            {
                VkDeviceMemory memory = VK_NULL_HANDLE;
                void* p = nullptr;
                const char* what = "vkAllocateMemory(ring)";
                VkResult r = AllocatePlanned(c, true, req.size, memory);
                if (r == VK_SUCCESS && (r = vkMapMemory(s_dev, memory, 0, VK_WHOLE_SIZE, 0, &p)) != VK_SUCCESS)
                    what = "vkMapMemory(ring)";
                else if (r == VK_SUCCESS && (r = vkBindBufferMemory(s_dev, s_ring, memory, 0)) != VK_SUCCESS)
                {
                    what = "vkBindBufferMemory(ring)";
                    vkDestroyBuffer(s_dev, s_ring, nullptr);  // (a buffer binds once)
                    Check(vkCreateBuffer(s_dev, &bci, nullptr, &s_ring), "vkCreateBuffer(ring)");
                }
                if (r != VK_SUCCESS)
                {
                    fprintf(stderr, "[renderer] %s failed: %d (%s, memory type %u): trying the next place\n", what, int(r),
                        memplan::Name(c.place, true), c.type);
                    if (memory)
                        vkFreeMemory(s_dev, memory, nullptr);
                    failed += std::string(failed.empty() ? "" : ", ") + memplan::Name(c.place, true);
                    continue;
                }
                fprintf(stderr, "[renderer] upload ring: memory type %u (flags %X)\n", c.type, s_memProps.memoryTypes[c.type].propertyFlags);
                s_ringMemory = memory;
                s_ringHost = static_cast<uint8_t*>(p);
                placed = &c;
                break;
            }
            if (!placed)
            {
                fprintf(stderr, "[renderer] upload ring: no place could hold it (%s failed; %s)\n", failed.c_str(), plan.why.c_str());
                abort();
            }
            // The choice and why, in one line.
            fprintf(stderr, "[renderer] memory: guest memory %s; upload ring in %s, memory type %u (%s)%s\n", s_guestChoice.c_str(),
                placed->place == memplan::Place::kVram ? "CPU-mapped VRAM" : placed->place == memplan::Place::kCached ? "cached host memory"
                    : "host memory", placed->type, plan.why.c_str(), AfterFailed(failed).c_str());
            // Issue #40: say so when the GPU reads guest memory across PCIe.
            if (const char* note = memplan::OverPcieNote(s_planGpu, s_shadowMode && s_shadowInVram,
                    (s_memProps.memoryTypes[placed->type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0))
                fprintf(stderr, "[renderer] note: %s\n", note);
        }

        void CreateSet0()
        {
            VkDescriptorSetLayoutBinding b[3] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            };
            auto bindings = WithSharedParts(b, 3);
            VkDescriptorSetLayoutCreateInfo dslci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dslci.bindingCount = uint32_t(bindings.size());
            dslci.pBindings = bindings.data();
            Check(vkCreateDescriptorSetLayout(s_dev, &dslci, nullptr, &s_set0Layout), "set 0 layout");
            VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, s_splitMemory ? 5u : 2u },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 } };
            VkDescriptorPoolCreateInfo dpci{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpci.maxSets = 1;
            dpci.poolSizeCount = 2;
            dpci.pPoolSizes = sizes;
            Check(vkCreateDescriptorPool(s_dev, &dpci, nullptr, &s_descriptorPool), "descriptor pool");
            VkDescriptorSetAllocateInfo dsai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dsai.descriptorPool = s_descriptorPool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &s_set0Layout;
            Check(vkAllocateDescriptorSets(s_dev, &dsai, &s_set0), "set 0");
            VkDescriptorBufferInfo constants{ s_ring, 0, kConstantsSize };
            VkDescriptorBufferInfo ring{ s_ring, 0, kRingSize };
            VkWriteDescriptorSet w[6] = {
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_set0, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_set0, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nullptr, &constants, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, s_set0, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &ring, nullptr },
            };
            vkUpdateDescriptorSets(s_dev, AddSharedParts(w, 3, s_set0), w, 0, nullptr);
        }

        void CreateSamplerAndDummies()
        {
            VkSamplerCreateInfo sci{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.maxLod = VK_LOD_CLAMP_NONE;
            Check(vkCreateSampler(s_dev, &sci, nullptr, &s_sampler), "vkCreateSampler");

            VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            s_dummy[0] = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 1, 1, 1 }, 1, usage,
                VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            s_dummy[1] = CreateImage(VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, { 1, 1, 1 }, 1, usage,
                VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_ASPECT_COLOR_BIT);
            s_dummy[2] = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 1, 1, 1 }, 6, usage,
                VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
            s_dummyStorage = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R32_UINT, { 1, 1, 1 }, 1, VK_IMAGE_USAGE_STORAGE_BIT,
                VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
            Begin();
            ToGeneral(s_dummyStorage.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            VkClearColorValue white{ { 1.0f, 1.0f, 1.0f, 1.0f } };
            for (int i = 0; i < 3; i++)
            {
                uint32_t layers = i == 2 ? 6 : 1;
                ToGeneral(s_dummy[i].image, VK_IMAGE_ASPECT_COLOR_BIT, layers);
                VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers };
                vkCmdClearColorImage(s_cmd, s_dummy[i].image, VK_IMAGE_LAYOUT_GENERAL, &white, 1, &range);
            }
            FullBarrier();
        }
    }

    namespace
    {
        // ---------------------------------------------------------------
        // Live settings. Internal Resolution and Mipmaps change between two
        // frames (OnSwap, on this thread), with the GPU idle (Flush: command
        // buffers in flight are the only other users of what is replaced).
        // What was built for the old value is made again from what the guest
        // has:
        //   - Internal Resolution: the render targets of the scaled pitches
        //     at the new scale (their EDRAM contents are lost; the game clears
        //     and redraws them every frame, and resolve clears still waiting
        //     move over), the scaled resolve memory (every page back to its
        //     1x copy until a scaled resolve writes it again) and the scaled
        //     textures. The front images follow at the next swap
        //     (PresentFrontBuffer replaces one of a new size), and the
        //     presenter's AA image with them.
        //   - Mipmaps: the whole texture cache, each texture loaded again
        //     from guest memory when a draw next uses it.
        // Auto, and a setting's native rows or columns (video::NativeShape),
        // follow the picture's size (the window, fullscreen, Aspect Ratio)
        // once it has held for kAutoSettleFrames, so a window being dragged
        // doesn't rebuild at every step; a change of the setting itself
        // applies at the next swap.
        uint64_t s_liveGeneration = ~0ull;
        int32_t s_resolutionSetting = 0;  // Internal Resolution as applied
        constexpr uint32_t kAutoSettleFrames = 30;
        gpu::RenderScale s_autoWant;
        uint32_t s_autoSeen = 0;

        // Resolves no longer find `t` to store into (if they did).
        void ForgetDirectTexture(const Texture& t)
        {
            if (auto it = s_directByBase.find(t.guestStart); it != s_directByBase.end() && it->second == &t)
                s_directByBase.erase(it);
            if (auto it = s_directByStart.find(t.guestStart); it != s_directByStart.end() && it->second == &t)
                s_directByStart.erase(it);
        }

        void DestroyTexture(Texture& t)
        {
            ForgetDirectTexture(t);
            if (auto it = s_scaledByStart.find(t.guestStart); it != s_scaledByStart.end() && it->second == &t)
                s_scaledByStart.erase(it);
            if (t.scaled)
                ReleaseHeldPages(&t, t.guestStart, t.guestSize);
            for (auto& [swizzle, view] : t.views)
                vkDestroyImageView(s_dev, view, nullptr);
            t.views.clear();
            for (uint32_t layer = 1; layer < 6; layer++)  // ([0] is storeView)
                if (t.storeViews[layer])
                    vkDestroyImageView(s_dev, t.storeViews[layer], nullptr);
            std::fill(std::begin(t.storeViews), std::end(t.storeViews), VK_NULL_HANDLE);
            if (t.storeView)
                vkDestroyImageView(s_dev, t.storeView, nullptr);
            t.storeView = VK_NULL_HANDLE;
            DestroyImage(t.image);
        }

        // Drops the cached textures `which` picks (the GPU idle); returns how many.
        template <typename Which>
        size_t DropTextures(Which which)
        {
            // Deferred colour resolves' bytes from those textures first, and
            // their reads done before the textures go.
            bool owed = false;
            for (size_t i = s_deferredResolves.size(); i-- > 0;)
                if (s_deferredResolves[i].texture && which(*s_deferredResolves[i].texture))
                {
                    s_deferForGpu++;
                    WriteDeferred(i);
                    owed = true;
                }
            if (owed)
                Flush();
            s_textureCache->generation++;
            size_t dropped = 0;
            for (auto it = s_textures.begin(); it != s_textures.end();)
            {
                if (!which(*it->second))
                {
                    ++it;
                    continue;
                }
                DestroyTexture(*it->second);
                it = s_textures.erase(it);
                dropped++;
            }
            return dropped;
        }

        // ---------------------------------------------------------------
        // Texture cache limits (gpu/texture_budget.h has the policy and
        // why). Checked at every 30th swap (TrimTextureCache), and when an
        // image allocation fails (ReclaimTextureMemory). An evicted texture
        // leaves every table that hands textures out at once (the cache, the
        // lookups kept per fetch slot, the direct textures resolves store
        // into), so nothing recorded after it can use it, and is destroyed
        // once the last submission that could have used it has completed:
        // no wait for the GPU at a check. The next draw that samples one
        // loads it again from guest memory, as a new texture.
        // NFSMW_TEXTURE_BUDGET_MB (default: half the largest device-local
        // heap, 512 MB to 2 GB; 1 GB at most on iOS; 0: no cap),
        // NFSMW_TEXTURE_MAX (8192 textures; 0: no cap) and
        // NFSMW_TEXTURE_EVICT_AGE (600 swaps) set the limits;
        // NFSMW_TEXTURE_EVICT=0 never evicts at a swap (A/B: the cache as
        // up to v0.1.4, but for a failed allocation). The [perf] line counts
        // what was evicted.
        struct TextureEvictions
        {
            uint64_t textures = 0, bytes = 0, reclaims = 0, batches = 0;
        };
        TextureEvictions s_textureEvictions;

        const texbudget::Limits& TextureLimits()
        {
            static const texbudget::Limits limits = [] {
                texbudget::Limits l;
                uint64_t heap = 0;
                for (uint32_t h = 0; h < s_memProps.memoryHeapCount; h++)
                    if (s_memProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                        heap = std::max<uint64_t>(heap, s_memProps.memoryHeaps[h].size);
#if defined(__APPLE__) && TARGET_OS_IOS
                l.bytes = texbudget::DefaultBytes(heap, texbudget::kDefaultCeilingIos);
#else
                l.bytes = texbudget::DefaultBytes(heap);
#endif
                l.count = texbudget::kDefaultCount;
                if (const char* v = std::getenv("NFSMW_TEXTURE_BUDGET_MB"))
                    l.bytes = uint64_t(std::max(0ll, std::atoll(v))) * texbudget::kMB;
                if (const char* v = std::getenv("NFSMW_TEXTURE_MAX"))
                    l.count = uint32_t(std::clamp(std::atoll(v), 0ll, 0x7FFFFFFFll));
                if (const char* v = std::getenv("NFSMW_TEXTURE_EVICT_AGE"))
                    l.minAge = uint64_t(std::max(1ll, std::atoll(v)));
                fprintf(stderr, "[renderer] texture cache: %llu MB and %u textures at most (0: no cap; largest device-local heap %llu MB); "
                    "over either or the device's memory budget, textures unused for %llu swaps are evicted\n",
                    (unsigned long long)(l.bytes / texbudget::kMB), l.count, (unsigned long long)(heap / texbudget::kMB),
                    (unsigned long long)l.minAge);
                return l;
            }();
            return limits;
        }

        // The device-local heaps' use and budget as the driver counts them
        // (VK_EXT_memory_budget); 0 and 0 without it.
        void DeviceMemoryBudget(uint64_t& used, uint64_t& budget)
        {
            used = budget = 0;
            if (!s_vk || !s_memoryBudget)
                return;
            VkPhysicalDeviceMemoryBudgetPropertiesEXT b{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
            VkPhysicalDeviceMemoryProperties2 props{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b };
            vkGetPhysicalDeviceMemoryProperties2(s_vk->physical, &props);
            for (uint32_t h = 0; h < props.memoryProperties.memoryHeapCount; h++)
                if (props.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                {
                    used += b.heapUsage[h];
                    budget += b.heapBudget[h];
                }
        }

        // Every submission up to this one has completed (they complete in
        // order).
        uint64_t CompletedSubmission()
        {
            std::lock_guard lock(s_submitMutex);
            return s_inFlight.empty() ? s_submissionCounter - 1 : s_submissions[s_inFlight.front()].number - 1;
        }

        // Destroys the evicted textures whose last possible use has completed;
        // returns their bytes.
        uint64_t ReleaseRetiredTextures()
        {
            if (s_retiredTextures.empty())
                return 0;
            const uint64_t done = CompletedSubmission();
            uint64_t bytes = 0;
            std::erase_if(s_retiredTextures, [&](RetiredTexture& r) {
                if (r.submission > done)
                    return false;
                bytes += r.texture->image.bytes;
                DestroyTexture(*r.texture);
                return true;
            });
            return bytes;
        }

        // The cache's textures: their keys and, for texbudget::Pick, sizes
        // and last uses, in the same order; and their totals.
        void GatherTextures(std::vector<uint64_t>& keys, std::vector<texbudget::Candidate>& candidates, texbudget::Usage& usage)
        {
            keys.reserve(s_textures.size());
            candidates.reserve(s_textures.size());
            for (const auto& [key, t] : s_textures)
            {
                keys.push_back(key);
                // (One a deferred colour resolve's bytes are to come from
                // counts as used now: never evicted.)
                const bool owed = std::any_of(s_deferredResolves.begin(), s_deferredResolves.end(),
                    [&](const DeferredResolve& d) { return d.texture == t.get(); });
                candidates.push_back({ t->image.bytes, owed ? s_frame : t->lastUsed });
                usage.bytes += t->image.bytes;
            }
            usage.count = uint32_t(s_textures.size());
        }

        // Moves the textures Pick chose (`picks`, into `keys`) out of the
        // cache, to be destroyed once submission `lastUse` has completed:
        // none can be used by anything recorded after it. Returns their bytes.
        uint64_t EvictTextures(const std::vector<uint64_t>& keys, const std::vector<size_t>& picks, uint64_t lastUse)
        {
            std::unordered_set<const Texture*> gone;
            uint64_t bytes = 0;
            for (size_t i : picks)
            {
                auto it = s_textures.find(keys[i]);
                if (it == s_textures.end())
                    continue;
                ForgetDirectTexture(*it->second);
                bytes += it->second->image.bytes;
                gone.insert(it->second.get());
                s_retiredTextures.push_back({ std::move(it->second), lastUse });
                s_textures.erase(it);
            }
            // The lookups kept per fetch slot that hold one (only an entry of
            // the current generation is ever read, and those point at live
            // textures: the cache's, or these until now).
            TextureCache& c = *s_textureCache;
            auto forget = [&](TextureCacheEntry& e) {
                if (e.generation == c.generation && gone.count(e.texture))
                    e.generation = 0;
            };
            for (auto& slots : c.slots)
                for (TextureCacheEntry& e : slots)
                    forget(e);
            for (auto& ways : c.recent)
                for (TextureCacheEntry& e : ways)
                    forget(e);
            // And the draw memo, whatever it holds (TextureDrawMemo); a
            // draw looking its textures up meanwhile then keeps none.
            if (!gone.empty())
            {
                c.memo.layout = nullptr;
                c.evictions++;
            }
            s_textureEvictions.textures += gone.size();
            s_textureEvictions.bytes += bytes;
            return bytes;
        }

        // OnSwap, s_frame just moved on: destroys what earlier evictions
        // left once the GPU is done with it, and at every 30th swap checks
        // the cache against its limits, evicting the textures unused longest
        // (none drawn in the last minAge swaps) when over one.
        void TrimTextureCache()
        {
            ReleaseRetiredTextures();
            static const bool off = [] { const char* v = std::getenv("NFSMW_TEXTURE_EVICT"); return v && v[0] == '0'; }();
            if (off || s_frame % 30 != 0 || s_textures.empty())
                return;
            const texbudget::Limits& limits = TextureLimits();
            std::vector<uint64_t> keys;
            std::vector<texbudget::Candidate> candidates;
            texbudget::Usage usage;
            GatherTextures(keys, candidates, usage);
            DeviceMemoryBudget(usage.deviceUsed, usage.deviceBudget);
            const texbudget::Excess excess = texbudget::Over(limits, usage);
            const std::vector<size_t> picks = texbudget::Pick(candidates, s_frame, limits.minAge, excess);
            if (picks.empty())
                return;  // (nothing over, or nothing old enough: the scene's textures stay)
            // Each use so far is in the submission being recorded, if any,
            // or one before it.
            const uint64_t bytes = EvictTextures(keys, picks, s_recording ? s_submissionCounter : s_submissionCounter - 1);
            s_textureEvictions.batches++;
            if (s_textureEvictions.batches <= 20 || s_textureEvictions.batches % 50 == 0)
            {
                std::string why;
                if (excess.overBytes)
                    why += std::format(", its {} MB cap", limits.bytes / texbudget::kMB);
                if (excess.overCount)
                    why += std::format(", its {} textures cap", limits.count);
                if (excess.overDevice)
                    why += std::format(", the device's memory budget ({} of {} MB in use)", usage.deviceUsed / texbudget::kMB,
                        usage.deviceBudget / texbudget::kMB);
                fprintf(stderr, "[renderer] texture cache: %zu textures (%.0f MB) unused for %llu+ swaps evicted, over%s; %.0f MB in %zu "
                    "kept (eviction %llu)\n", picks.size(), double(bytes) / 1048576.0, (unsigned long long)limits.minAge,
                    why.empty() ? "" : why.c_str() + 1, double(usage.bytes - bytes) / 1048576.0, s_textures.size(),
                    (unsigned long long)s_textureEvictions.batches);
            }
        }

        bool ReclaimTextureMemory(VkDeviceSize need, uint32_t round)
        {
            if (round > 1)
                return false;
            if (round == 0)
                s_textureEvictions.reclaims++;
            // Round 0: the oldest, twice what was asked for or a quarter of
            // the cache; round 1: everything Pick may take. Never a texture
            // used since the last swap: the one being made and every one the
            // draw being recorded binds (lastUsed is s_frame for those).
            std::vector<uint64_t> keys;
            std::vector<texbudget::Candidate> candidates;
            texbudget::Usage usage;
            GatherTextures(keys, candidates, usage);
            texbudget::Excess excess;
            excess.bytes = round == 0 ? std::max<uint64_t>(need * 2, usage.bytes / 4) : usage.bytes;
            excess.count = round == 0 ? 0 : usage.count;
            const std::vector<size_t> picks = texbudget::Pick(candidates, s_frame, 1, excess);
            // The recording (s_submissionCounter) holds no use of these:
            // their uses are all submitted (the swap submits its frame's
            // work, PresentFrontBuffer, before s_frame moves on). So once
            // everything in flight has completed nothing uses them, and that
            // wait drains the queue without submitting the recording, safe
            // wherever an image is made (a pass being set up, say).
            const uint64_t evicted = EvictTextures(keys, picks, s_submissionCounter - 1);
            if (!s_retiredTextures.empty())
            {
                CpWaitTimer wait(CP_WAIT_FLUSH);
                std::unique_lock lock(s_submitMutex);
                s_submitCv.wait(lock, [] { return s_inFlight.empty(); });
            }
            const uint64_t freed = ReleaseRetiredTextures();
            // Then the empty blocks (the spares, and any the textures just
            // emptied) go back to the device now, not after kSpareKeep.
            std::vector<VkDeviceMemory> spares;
            {
                std::lock_guard lock(s_spareMutex);
                for (std::vector<Spare>& list : s_spares)
                {
                    for (const Spare& spare : list)
                        spares.push_back(spare.memory);
                    list.clear();
                }
                s_imageTotals.blocksFreed += spares.size();
            }
            for (VkDeviceMemory memory : spares)
                vkFreeMemory(s_dev, memory, nullptr);
            fprintf(stderr, "[renderer] out of memory for an image (%.1f MB; 0: making it, round %u): %zu textures evicted (%.0f MB), %.0f MB of "
                "textures and %zu spare image blocks freed; %.0f MB in %zu textures kept\n", double(need) / 1048576.0, round, picks.size(),
                double(evicted) / 1048576.0, double(freed) / 1048576.0, spares.size(), double(usage.bytes - evicted) / 1048576.0,
                s_textures.size());
            return freed > 0 || !spares.empty();
        }

        void ApplyRenderScale(const gpu::RenderScale& scale, const std::string& how)
        {
            auto start = std::chrono::steady_clock::now();
            const gpu::RenderScale old = s_renderScale;
            Flush();
            s_renderScale = scale;
            PrewarmResolvePipelines(scale);
            size_t targets = 0;
            for (auto& [key, slot] : s_targets)
            {
                RenderTarget& was = *slot;  // (keyed as before: the key has no scale)
                if (!ScaledPitch(was.pitch) || was.scale == scale)
                    continue;
                // Clears still waiting for their target's next pass, in guest
                // pixels (their host edges came from guest ones: GuestEdge
                // gives those back; from below 1x the first of the guest
                // edges sharing one, which the game's 8-aligned clears are).
                std::vector<PendingClear> moved;
                std::erase_if(s_pendingClears, [&](const PendingClear& pc) {
                    if (pc.rt != &was)
                        return false;
                    const uint32_t x0 = was.scale.x.GuestEdge(uint32_t(pc.rect.offset.x)), y0 = was.scale.y.GuestEdge(uint32_t(pc.rect.offset.y));
                    const uint32_t x1 = was.scale.x.GuestEdge(uint32_t(pc.rect.offset.x) + pc.rect.extent.width);
                    const uint32_t y1 = was.scale.y.GuestEdge(uint32_t(pc.rect.offset.y) + pc.rect.extent.height);
                    moved.push_back({ nullptr, { { int32_t(x0), int32_t(y0) }, { x1 - x0, y1 - y0 } }, pc.value });
                    return true;
                });
                uint32_t base = was.base, format = was.format, pitch = was.pitch;
                bool depth = was.depth, wide = was.wide;
                DestroyTarget(was);
                slot = CreateTarget(base, format, depth, pitch, wide);
                for (PendingClear& pc : moved)
                {
                    pc.rt = slot.get();
                    pc.rect = HostRect(scale, pc.rect.offset.x, pc.rect.offset.y, pc.rect.offset.x + int32_t(pc.rect.extent.width),
                        pc.rect.offset.y + int32_t(pc.rect.extent.height));
                    s_pendingClears.push_back(pc);
                }
                targets++;
            }
            for (RecentTarget& recent : s_recentTargets)
                recent = {};
            size_t textures = DropTextures([](const Texture& t) { return t.scaled; });
            // The buffers are kept for another scale above or below 1x, unless
            // the pool, made for a smaller scale below 1x, is too small for it,
            // or below 1x larger than it needs (from 2x to 0.75x: 512 MB where
            // 288 do) (the GPU is idle here: Flush, and nothing uses them until
            // the next scaled resolve makes the memory again).
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            const VkDeviceSize poolNeeded = ScaledPoolSize(scale, props);
            const bool poolFits = !s_scaled.pool || (s_scaled.poolBytes >= poolNeeded && (!scale.Below() || s_scaled.poolBytes < poolNeeded + 0x10000));
            ResetScaledMemory(!scale.IsOne() && poolFits);
            fprintf(stderr, "[renderer] internal resolution %s -> %s (%ux%u, %s): %zu targets and %zu scaled textures replaced in %.1f ms\n",
                gpu::ScaleName(old).c_str(), gpu::ScaleName(scale).c_str(), scale.x.Size(1280), scale.y.Size(720), how.c_str(), targets, textures,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }

        void ApplyMipmaps(bool mips)
        {
            auto start = std::chrono::steady_clock::now();
            Flush();
            s_loadMips = mips;
            size_t textures = DropTextures([](const Texture&) { return true; });
            fprintf(stderr, "[renderer] mipmaps %s: %zu textures dropped in %.1f ms (loaded again as they are drawn)\n",
                mips ? "on" : "off", textures, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }

        void ApplyLiveSettings()
        {
            uint64_t generation = settings::Generation();
            if (generation != s_liveGeneration)
            {
                s_liveGeneration = generation;
                bool mips = settings::GetBool(settings::Id::Mipmaps);
                if (mips != s_loadMips)
                    ApplyMipmaps(mips);
            }
            if (s_scaleFixed)
                return;
            int32_t setting = settings::GetInt(settings::Id::InternalResolution);
            uint32_t regionW = 0, regionH = 0;
            // Auto, and a setting's native rows or columns, follow the picture.
            if (setting <= 0 || s_nativeShape)
            {
                video::FrameRegion(regionW, regionH);
                if (regionW == 0 || regionH == 0)
                    return;  // no picture to size it for: keep the scale
            }
            const gpu::RenderScale want = WantedRenderScale(setting, regionW, regionH);
            if (want == s_renderScale)
            {
                s_resolutionSetting = setting;
                s_autoSeen = 0;
                return;
            }
            // Only the picture's size moved (Auto, native rows or columns):
            // once it has settled.
            if (setting == s_resolutionSetting)
            {
                if (!(want == s_autoWant))
                {
                    s_autoWant = want;
                    s_autoSeen = 0;
                }
                if (++s_autoSeen < kAutoSettleFrames)
                    return;
            }
            s_resolutionSetting = setting;
            s_autoSeen = 0;
            ApplyRenderScale(want, ScaleReason(setting, regionW, regionH));
        }

        // ---------------------------------------------------------------
        // Occlusion queries counted on the GPU (NFSMW_OCCLUSION=1; see
        // ZpassDone).

        // Before a counted draw, inside its pass: the active segment while
        // the weight is the same, else a new one. None when the slot's
        // queries are used up (the draw isn't counted).
        void ZpdBeginSegment(uint32_t num, uint32_t den)
        {
            if (s_zpdQuery >= 0 && (num != s_zpdNum || den != s_zpdDen))
                ZpdEndSegment();
            if (s_zpdQuery >= 0)
                return;
            if (s_zpdUsed[s_slot] >= kZpdQueries)
            {
                s_zpdCounts.overflow++;
                LogOnce((0x2Aull << 56) | 2, "occlusion query: %u segments in one command buffer: draws past them aren't counted", kZpdQueries);
                return;
            }
            uint32_t q = s_zpdUsed[s_slot]++;
            vkCmdBeginQuery(s_cmd, s_zpdPool[s_slot], q, VK_QUERY_CONTROL_PRECISE_BIT);
            s_zpdQuery = int32_t(q);
            s_zpdNum = num;
            s_zpdDen = den;
            s_submissions[s_slot].zpdSegments.push_back({ s_zpdSegNext++, q, num, den });
            s_zpdCounts.segments++;
        }

        // At each swap. The game's BEGIN, quad and END follow each other in
        // one view, so a bracket still open came from an earlier frame: it
        // is dropped (an open bracket counts every draw and turns off
        // NFSMW_PASS_MERGE bit 1). Every 120 frames, before the [perf] line,
        // the [zpd] line.
        void ZpdOnSwap(uint64_t frame)
        {
            zpd::Bracket first;
            if (uint32_t dropped = s_zpdOpen.DropBefore(frame, &first))
            {
                s_zpdCounts.dropped += dropped;
                LogOnce((0x2Bull << 56) | first.beginAddress, "occlusion query: BEGIN %08X (frame %llu) still open at the swap: dropped (%u in all)",
                    first.beginAddress, (unsigned long long)first.frame, dropped);
                ZpdLog("f%llu swap: %u brackets dropped (the first: BEGIN %08X)", (unsigned long long)frame, dropped, first.beginAddress);
            }
            if (frame % 120 != 0)
                return;
            static ZpdCounts last{};
            static ZpdStores lastStores{};
            ZpdStores stores;
            {
                std::lock_guard lock(s_submitMutex);
                stores = s_zpdStores;
                s_zpdStores.delayMax = 0;
            }
            const ZpdCounts& c = s_zpdCounts;
            uint64_t stored = stores.stored - lastStores.stored;
            fprintf(stderr, "[zpd] per frame: %.1f ZPDs, %.1f ENDs (%.1f with samples), %.1f counted draws in %.1f segments"
                " | CP -> store %.2f ms avg, %.2f ms max | totals: %llu unmatched ENDs, %llu dropped and %llu replaced brackets,"
                " %llu other reports, %llu not available, %llu over the queries, %llu lost, %llu outside the viewport, %llu not clamped\n",
                double(c.zpds - last.zpds) / 120, double(stored) / 120, double(stores.nonZero - lastStores.nonZero) / 120,
                double(c.draws - last.draws) / 120, double(c.segments - last.segments) / 120,
                stored ? (stores.delaySum - lastStores.delaySum) / double(stored) : 0.0, stores.delayMax,
                (unsigned long long)c.unmatched, (unsigned long long)c.dropped, (unsigned long long)c.replaced,
                (unsigned long long)c.unknown, (unsigned long long)s_zpdUnavailable.load(), (unsigned long long)c.overflow,
                (unsigned long long)stores.lost, (unsigned long long)c.clamped, (unsigned long long)c.unclamped);
            last = c;
            lastStores = stores;
        }
    }

    bool Initialize(const uint32_t* regs)
    {
        s_vk = video::GetVulkan();
        if (!s_vk)
            return false;
        s_dev = s_vk->device;
        s_regs = regs;
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            ReadRenderScale(props);
            const char* fmulz = std::getenv("NFSMW_FMULZ");
            s_fmulz = fmulz ? fmulz[0] == '1' : DriverId(props) == VK_DRIVER_ID_MESA_RADV;
            if (s_fmulz)
                fprintf(stderr, "[renderer] shader multiplications in the fmulz form (NFSMW_FMULZ=0: the select form)\n");
            const char* split = std::getenv("NFSMW_SPLIT_MEMORY");
            s_splitMemory = split ? split[0] == '1' : props.limits.maxStorageBufferRange < kSharedSize;
            if (s_splitMemory)
                fprintf(stderr, "[renderer] guest memory bound as 4 parts of %llu MB (storage buffers bind up to %u MB here)\n",
                    (unsigned long long)(kSharedPart >> 20), props.limits.maxStorageBufferRange >> 20);
            s_resolutionSetting = settings::GetInt(settings::Id::InternalResolution);
            s_loadMips = settings::GetBool(settings::Id::Mipmaps);
            s_liveGeneration = settings::Generation();
            uint32_t count = 0;
            vkEnumerateDeviceExtensionProperties(s_vk->physical, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> extensions(count);
            vkEnumerateDeviceExtensionProperties(s_vk->physical, nullptr, &count, extensions.data());
            for (const VkExtensionProperties& e : extensions)
                s_memoryBudget |= strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
        }
        vkGetPhysicalDeviceMemoryProperties(s_vk->physical, &s_memProps);
        s_pushDescriptorSet = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(s_dev, "vkCmdPushDescriptorSetKHR"));
        if (!s_pushDescriptorSet)
        {
            fprintf(stderr, "[renderer] VK_KHR_push_descriptor is missing; rendering disabled\n");
            s_vk = nullptr;
            return false;
        }
#ifdef VK_EXT_extended_dynamic_state
        s_dynamicState = (g_cpOpt & CP_OPT_DYNAMIC_STATE) && s_vk->cmdSetCullMode;
#endif
        glslang_initialize_process();
        CreatePipelineCache();
        LoadPipelineKeys();

        VkCommandPoolCreateInfo cpci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = s_vk->queueFamily;
        Check(vkCreateCommandPool(s_dev, &cpci, nullptr, &s_commandPool), "vkCreateCommandPool");
        for (Submission& sub : s_submissions)
        {
            VkCommandBufferAllocateInfo cbai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            cbai.commandPool = s_commandPool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            Check(vkAllocateCommandBuffers(s_dev, &cbai, &sub.cmd), "vkAllocateCommandBuffers");
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            Check(vkCreateFence(s_dev, &fci, nullptr, &sub.fence), "vkCreateFence");
        }
        {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(s_vk->physical, &props);
            s_timestampPeriod = props.limits.timestampPeriod;
            VkQueryPoolCreateInfo qpci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpci.queryCount = kSlots * 2;
            // HFR (game/hfr.h) needs the GPU's execution time for its load
            // check: the busy time (submit to seen done) counts the queue's
            // waits too, and read 9-12 ms a frame where execution took 5-8.
            if (const char* v = std::getenv("NFSMW_GPU_TIMING"); (v && v[0] == '1') || (!(v && v[0] == '0') && hfr::Enabled()))
                if (vkCreateQueryPool(s_dev, &qpci, nullptr, &s_timestamps) != VK_SUCCESS)
                    s_timestamps = VK_NULL_HANDLE;
            if (s_profileFrame >= 0)
            {
                qpci.queryCount = 512;
                Check(vkCreateQueryPool(s_dev, &qpci, nullptr, &s_passQueries), "pass query pool");
                if (s_vk->pipelineStatistics)
                {
                    VkQueryPoolCreateInfo sci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
                    sci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
                    sci.queryCount = 256;
                    sci.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                        VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
                    if (vkCreateQueryPool(s_dev, &sci, nullptr, &s_passStats) != VK_SUCCESS)
                        s_passStats = VK_NULL_HANDLE;
                    const char* v = std::getenv("NFSMW_PASS_STATS");
                    if (v && v[0] == '2' && !s_vk->occlusionCounting)
                    {
                        sci.queryType = VK_QUERY_TYPE_OCCLUSION;
                        sci.pipelineStatistics = 0;
                        if (vkCreateQueryPool(s_dev, &sci, nullptr, &s_passSamples) != VK_SUCCESS)
                            s_passSamples = VK_NULL_HANDLE;
                    }
                    fprintf(stderr, "[renderer] NFSMW_PASS_STATS: shader invocations per pass in the profile%s\n",
                        s_passStats ? "" : " unavailable (no query pool)");
                }
            }
        }
        // NFSMW_OCCLUSION=1: a pool of occlusion queries per slot, so a reset
        // never touches queries a submission in flight uses. Each is reset
        // before its first use: here with host reset, else by the slot's
        // first Begin.
        if (s_vk->occlusionCounting)
        {
            VkQueryPoolCreateInfo qpci{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            qpci.queryType = VK_QUERY_TYPE_OCCLUSION;
            qpci.queryCount = kZpdQueries;
            s_zpdOn = true;
            for (uint32_t k = 0; k < kSlots && s_zpdOn; k++)
            {
                s_zpdOn = vkCreateQueryPool(s_dev, &qpci, nullptr, &s_zpdPool[k]) == VK_SUCCESS;
                if (!s_zpdOn)
                    s_zpdPool[k] = VK_NULL_HANDLE;
                else if (s_vk->resetQueryPool)
                    s_vk->resetQueryPool(s_dev, s_zpdPool[k], 0, kZpdQueries);
                else
                    s_zpdUsed[k] = kZpdQueries;
            }
            if (s_zpdOn)
                s_zpdResults = std::make_unique<zpd::ResultRing>();
            else
            {
                for (VkQueryPool& pool : s_zpdPool)
                    if (pool)
                        vkDestroyQueryPool(s_dev, pool, nullptr);
                std::fill(std::begin(s_zpdPool), std::end(s_zpdPool), VK_NULL_HANDLE);
                std::fill(std::begin(s_zpdUsed), std::end(s_zpdUsed), 0u);
            }
            fprintf(stderr, s_zpdOn ? "[renderer] occlusion queries counted: %u per command buffer, %u command buffers\n"
                                    : "[renderer] occlusion query pools (%u x %u) couldn't be made: queries report zero samples\n",
                kZpdQueries, kSlots);
        }
        if (s_passBarrierLevel)
            fprintf(stderr, "[renderer] NFSMW_PASS_BARRIER=%u: %s leave the vertex stages out of their barriers%s\n", s_passBarrierLevel,
                s_passBarrierLevel == 1 ? "pass ends" : s_passBarrierLevel == 2 ? "pass ends and 1x resolves" : "1x resolves",
                s_passBarrierAlt ? " (every other [perf] window: NFSMW_PASS_BARRIER_ALT)" : "");
        if (s_untiledArea)
            fprintf(stderr, "[renderer] NFSMW_UNTILED_AREA%s: untiled passes load and store their draws' guest viewport, not the whole target\n",
                s_untiledArea == 2 ? "=alt (every other [perf] window)" : "");
        std::thread(CompletionThread).detach();
        if (s_imagePool)
            std::thread(MemoryThread).detach();

        CreateSharedMemory();
        SetSharedInfo();
        CreateRing();
        CreateSet0();
        CreateSamplerAndDummies();
        CreateResolvePipeline();
        CreateUntilePipeline();
        {
            VkDescriptorSetLayoutBinding b[4] = {
                { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            };
            auto bindings = WithSharedParts(b, 4);
            CreateComputePipeline(kFrontGlsl, bindings.data(), uint32_t(bindings.size()), 14 * sizeof(uint32_t), s_frontSetLayout,
                s_frontLayout, s_frontPipeline);
        }
        Flush();
        fprintf(stderr, "[renderer] ready: guest physical memory %s (%llu MB)\n",
            !s_shadowMode ? "imported" : s_shadowInVram ? "copied for the GPU, in VRAM" : "copied for the GPU, in host memory",
            (unsigned long long)(kSharedSize >> 20));
        return true;
    }

    uint64_t CoverHits()
    {
        return s_coverHits.load(std::memory_order_relaxed);
    }

    bool Enabled()
    {
        return s_vk != nullptr;
    }

    void LoadShader(uint32_t type, const uint32_t* words, uint32_t dwords)
    {
        if (!s_vk || dwords == 0 || type > 1)
            return;
        auto hash = [type](const uint32_t* w, uint32_t n) { return gpu::pipeline_keys::ShaderHash(type, w, n); };
        ShaderCache& c = *s_shaderCache;
        ShaderCacheEntry* e = nullptr;
        if (c.on)
        {
            c.loads++;
            e = &c.entries[(uint64_t(reinterpret_cast<uintptr_t>(words)) * 0x9E3779B97F4A7C15ull) >> 54];
            if (e->words == words && e->dwords == dwords && e->type == type)
            {
                c.comparedBytes += size_t(dwords) * 4;
                if (memcmp(e->copy.data(), words, size_t(dwords) * 4) == 0)
                {
                    c.hits++;
                    if (c.verify)
                    {
                        auto it = s_shaders.find(hash(words, dwords));
                        Shader* full = it == s_shaders.end() ? nullptr : it->second.get();
                        if (full != e->shader && c.mismatches++ < 50)
                            fprintf(stderr, "[cpverify] shader at %p (%u dwords, type %u): cached %016llx, hash finds %016llx\n",
                                (const void*)words, dwords, type, (unsigned long long)e->shader->hash,
                                (unsigned long long)(full ? full->hash : 0));
                    }
                    (type == 0 ? s_vs : s_ps) = e->shader;
                    return;
                }
            }
            // Hashed from the copy, so the entry's words and shader agree
            // even if the guest writes them meanwhile.
            e->words = words;
            e->dwords = dwords;
            e->type = type;
            e->copy.assign(words, words + dwords);
            words = e->copy.data();
        }
        uint64_t h = hash(words, dwords);
        auto& slot = s_shaders[h];
        if (!slot)  // (the pipeline prewarm may have made it already, from the key list)
        {
            HitchTimer hitchTimer(s_hitch.shaderMs);
            s_hitch.shaders++;
            MakeShader(type, h, words, dwords);  // (into `slot`)
        }
        if (e)
            e->shader = slot.get();
        (type == 0 ? s_vs : s_ps) = slot.get();
    }

    // DIAG (v0.1.5 HFR scoping): NFSMW_HFR_DRAWMATCH=<from s>,<frames>. For <frames> consecutive frames from <from s>
    // after launch, each draw's identity (shaders, primitive type and count, render/depth target, textures) and hashes
    // of the float constants its shaders read; at each swap one [drawmatch] line compares the frame with the one before:
    // draws whose identity is unique in both frames, repeated identities (matched in order), and identities the
    // previous frame lacks; for the matched draws, how many kept their constants, and how many read a vertex buffer at
    // another address (dynamic vertex data: particles, text).
    namespace
    {
        struct HfrConst { uint16_t index; uint32_t v[4]; };
        struct HfrDraw { uint64_t key, vsc, psc, vb; bool rel; std::vector<HfrConst> vs; };
        // The first draw of the frame that reads VS c26 (the per-render toggle the study found): its shader and value.
        struct HfrC26 { bool seen = false; uint64_t vs = 0; float v[4] = {}; };
        HfrC26 s_hfrC26;
        struct HfrDrawMatch
        {
            double fromS = -1;
            uint32_t frames = 0, done = 0;
            bool active = false;
            std::vector<HfrDraw> cur, prev;
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            HfrDrawMatch()
            {
                if (const char* v = std::getenv("NFSMW_HFR_DRAWMATCH"))
                {
                    fromS = std::atof(v);
                    const char* c = std::strchr(v, ',');
                    frames = c ? uint32_t(std::atoi(c + 1)) : 30u;
                    fprintf(stderr, "[drawmatch] DIAGNOSTIC: %u frames from %.1f s\n", frames, fromS);
                }
            }
        } s_hfrDm;  // command processor thread only

        bool HfrDrawMatchActive()
        {
            return s_hfrDm.active;
        }

        void HfrDrawMatchRecord(uint64_t key, uint64_t vsc, uint64_t psc, uint64_t vb, bool rel, std::vector<HfrConst>&& vs)
        {
            if (!s_hfrC26.seen)
                for (const HfrConst& c : vs)
                    if (c.index == 26)
                    {
                        s_hfrC26.seen = true;
                        memcpy(s_hfrC26.v, c.v, 16);
                    }
            s_hfrDm.cur.push_back({ key, vsc, psc, vb, rel, std::move(vs) });
        }

        void HfrDrawMatchSwap(uint64_t frame)
        {
            HfrDrawMatch& d = s_hfrDm;
            if (d.fromS < 0 || d.done > d.frames)
                return;
            if (!d.active)
            {
                if (std::chrono::duration<double>(std::chrono::steady_clock::now() - d.t0).count() >= d.fromS)
                {
                    d.active = true;
                    d.cur.clear();
                }
                return;
            }
            if (!d.prev.empty())
            {
                std::unordered_map<uint64_t, std::vector<const HfrDraw*>> before, now;
                for (auto& x : d.prev)
                    before[x.key].push_back(&x);
                for (auto& x : d.cur)
                    now[x.key].push_back(&x);
                uint32_t unique = 0, ordered = 0, unmatched = 0, countChanged = 0, same = 0, vsChanged = 0, psOnly = 0,
                         vbMoved = 0, rel = 0, relChanged = 0;
                for (auto& [key, list] : now)
                {
                    auto it = before.find(key);
                    if (it == before.end())
                    {
                        unmatched += uint32_t(list.size());
                        continue;
                    }
                    if (it->second.size() != list.size())
                        countChanged += uint32_t(list.size());
                    const size_t n = std::min(list.size(), it->second.size());
                    unmatched += uint32_t(list.size() - n);
                    for (size_t i = 0; i < n; i++)
                    {
                        const HfrDraw &a = *it->second[i], &b = *list[i];
                        (list.size() == 1 && it->second.size() == 1 ? unique : ordered)++;
                        if (a.vsc == b.vsc && a.psc == b.psc)
                            same++;
                        else if (a.vsc != b.vsc)
                            vsChanged++;
                        else
                            psOnly++;
                        if (a.vb != b.vb)
                            vbMoved++;
                        if (b.rel)
                        {
                            rel++;
                            if (a.vsc != b.vsc)
                                relChanged++;
                        }
                    }
                }
                // Which VS constants changed between matched draws, and by how much (largest |difference|).
                {
                    std::map<uint16_t, std::pair<uint32_t, uint32_t>> seenChanged;  // index -> (draws reading it, changed)
                    std::map<uint16_t, float> maxDiff;
                    for (auto& [key, list] : now)
                    {
                        auto it = before.find(key);
                        if (it == before.end())
                            continue;
                        const size_t n = std::min(list.size(), it->second.size());
                        for (size_t i = 0; i < n; i++)
                        {
                            const HfrDraw &a = *it->second[i], &b = *list[i];
                            if (a.vs.size() != b.vs.size())
                                continue;
                            for (size_t k = 0; k < b.vs.size(); k++)
                            {
                                auto& e = seenChanged[b.vs[k].index];
                                e.first++;
                                if (memcmp(a.vs[k].v, b.vs[k].v, 16) != 0)
                                {
                                    e.second++;
                                    for (int c = 0; c < 4; c++)
                                    {
                                        float fa, fb;
                                        memcpy(&fa, &a.vs[k].v[c], 4);
                                        memcpy(&fb, &b.vs[k].v[c], 4);
                                        if (std::isfinite(fa) && std::isfinite(fb))
                                            maxDiff[b.vs[k].index] = std::max(maxDiff[b.vs[k].index], std::abs(fa - fb));
                                    }
                                }
                            }
                        }
                    }
                    std::vector<std::pair<uint32_t, uint16_t>> top;
                    for (auto& [idx, e] : seenChanged)
                        if (e.second)
                            top.push_back({ e.second, idx });
                    std::sort(top.rbegin(), top.rend());
                    std::string t;
                    for (size_t i = 0; i < std::min<size_t>(top.size(), 12); i++)
                    {
                        const auto& e = seenChanged[top[i].second];
                        t += std::format(" c{} {}/{} (max {:.3g})", top[i].second, e.second, e.first, maxDiff[top[i].second]);
                    }
                    fprintf(stderr, "[drawmatch] frame %llu VS constants changed most:%s | c26 %s\n", (unsigned long long)frame, t.c_str(),
                        s_hfrC26.seen ? std::format("{:.6g},{:.6g},{:.6g},{:.6g}", s_hfrC26.v[0], s_hfrC26.v[1], s_hfrC26.v[2], s_hfrC26.v[3]).c_str() : "-");
                }
                const double total = double(d.cur.size());
                auto pct = [&](uint32_t v) { return total > 0 ? 100.0 * v / total : 0.0; };
                fprintf(stderr, "[drawmatch] frame %llu: %zu draws (previous %zu), %zu identities | matched unique %.1f%%, "
                                "repeated identities matched in order %.1f%% (identity count changed: %.1f%%), unmatched %.1f%% | of "
                                "the matched: constants unchanged %.1f%%, VS constants changed %.1f%%, PS only %.1f%%, vertex "
                                "buffer moved %.1f%%; relative-constant (palette) VS %u, %u changed\n",
                    (unsigned long long)frame, d.cur.size(), d.prev.size(), now.size(), pct(unique), pct(ordered), pct(countChanged),
                    pct(unmatched), pct(same), pct(vsChanged), pct(psOnly), pct(vbMoved), rel, relChanged);
            }
            // NFSMW_HFR_C26SCAN=1 (DIAGNOSTIC, M2): in the first two frames,
            // where in guest memory c26's x, y, z sit (big-endian floats, in a
            // row): the variable the game sets it from.
            static const bool scan = [] { const char* v = std::getenv("NFSMW_HFR_C26SCAN"); return v && v[0] == '1'; }();
            if (scan && s_hfrC26.seen && d.done < 2 && (s_hfrC26.v[0] != 0.0f || s_hfrC26.v[1] != 0.0f))
            {
                uint8_t pattern[12];
                for (int k = 0; k < 3; k++)
                {
                    uint32_t bits;
                    memcpy(&bits, &s_hfrC26.v[k], 4);
                    bits = __builtin_bswap32(bits);
                    memcpy(pattern + k * 4, &bits, 4);
                }
                std::string hits;
                uint32_t count = 0;
                auto scanRange = [&](uint32_t from, uint32_t size, const char* where) {
                    const uint8_t* p = g_memory.base + from;
                    for (uint32_t i = 0; i + 12 <= size && count < 16; i += 4)
                        if (!memcmp(p + i, pattern, 12))
                        {
                            hits += std::format(" {:08X} ({})", from + i, where);
                            count++;
                        }
                };
                for (vmem::PageHeap* heap : { &vmem::Virtual4K(), &vmem::Virtual64K(), &vmem::Physical() })
                    for (uint32_t a = heap->Base(); a < heap->End();)
                    {
                        vmem::RegionInfo info{};
                        if (!heap->Query(a, &info) || !info.regionSize)
                            break;
                        if (info.state & vmem::X_MEM_COMMIT)
                            scanRange(info.baseAddress, info.regionSize, heap == &vmem::Physical() ? "phys" : "virt");
                        a = info.baseAddress + info.regionSize;
                    }
                scanRange(0x82000000u, 0x00CD0000u, "xex");
                fprintf(stderr, "[drawmatch] frame %llu: c26's x, y, z in guest memory at:%s\n", (unsigned long long)frame,
                    hits.empty() ? " nowhere" : hits.c_str());
            }
            d.prev.swap(d.cur);
            d.cur.clear();
            s_hfrC26 = {};
            if (++d.done > d.frames)
                d.active = false;
        }
    }

    void Draw(uint32_t initiatorValue, uint32_t dmaBase, uint32_t dmaSizeValue)
    {
        if (!s_vk)
            return;
        reg::VGT_DRAW_INITIATOR initiator;
        initiator.value = initiatorValue;
        auto edramMode = Reg<reg::RB_MODECONTROL>().edram_mode;
        if (edramMode == xenos::EdramMode::kCopy)
        {
            Resolve();
            return;
        }
        if (edramMode != xenos::EdramMode::kColorDepth && edramMode != xenos::EdramMode::kDepthOnly)
            return;
        // Depth-only: no pixel shader runs (depth_only.h); the guest's stays
        // bound for the draws after this one.
        struct RestorePs
        {
            Shader* ps;
            ~RestorePs() { s_ps = ps; }
        } restorePs{ s_ps };
        if (!gpu::RunsGuestPixelShader(edramMode, s_depthOnlyGuestPs))
            s_ps = DepthOnlyPixelShader();
        else if (s_exp & 64)  // (NFSMW_GPU_EXP: every draw's)
            s_ps = DepthOnlyPixelShader();
        s_stats.draws++;
        auto skip = [&](uint64_t key, const char* why) {
            s_stats.skipped++;
            LogOnce(key, "draw skipped: %s", why);
        };
        if (!s_vs || !s_ps)
            return skip(1, "no shaders loaded");
        if (!s_vs->info.ok || !s_ps->info.ok)
            return skip(2, "a shader failed to translate");
        uint32_t count = initiator.num_indices;
        if (count == 0)
            return;
        if (initiator.source_select == xenos::SourceSelect::kImmediate)
            return skip(3, "immediate indices");
        auto viz = Reg<reg::PA_SC_VIZ_QUERY>();
        if (viz.viz_query_ena && viz.kill_pix_post_hi_z)
            return;

        auto surface = Reg<reg::RB_SURFACE_INFO>();
        uint32_t pitch = surface.surface_pitch;
        if (pitch == 0)
            return skip(4, "zero surface pitch");
        auto [tileShiftX, tileShiftY] = TileShift();
        if (tileShiftX != 0 || tileShiftY != 0)
        {
            s_stats.tileSkips++;
            return;  // drawn with tile 0 (untiled rendering)
        }
        const bool untiledPass = s_untiled && surface.msaa_samples != xenos::MsaaSamples::k1X &&
            Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable;

        // Render targets.
        RenderTarget* color[4] = {};
        auto colorMask = Reg<reg::RB_COLOR_MASK>();
        uint32_t writeMask = 0;
        if (edramMode == xenos::EdramMode::kColorDepth)
        {
            static const uint32_t infoRegs[4] = { XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO,
                XE_GPU_REG_RB_COLOR2_INFO, XE_GPU_REG_RB_COLOR3_INFO };
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t mask = (colorMask.value >> (4 * i)) & 0xF;
                if (!(s_ps->info.colorTargetMask & (1u << i)) || mask == 0)
                    continue;
                auto ci = RegAt<reg::RB_COLOR_INFO>(infoRegs[i]);
                color[i] = GetTarget(ci.color_base, uint32_t(ci.color_format), false, pitch);
                writeMask |= mask << (4 * i);
            }
        }
        auto depthControl = Reg<reg::RB_DEPTHCONTROL>();
        RenderTarget* depth = nullptr;
        if (depthControl.z_enable || depthControl.stencil_enable)
        {
            auto di = Reg<reg::RB_DEPTH_INFO>();
            depth = GetTarget(di.depth_base, uint32_t(di.depth_format), true, pitch);
        }
        if (!depth && writeMask == 0)
            return;  // nothing visible (memexport-only draws are not supported yet)
        // NFSMW_OCCLUSION=1, an occlusion query bracket open: this draw is
        // counted (ZpassDone).
        const bool zpdCount = s_zpdOn && !s_zpdOpen.Empty();
        // NFSMW_PASS_MERGE bit 1: nor is a depth-tested draw that writes no
        // colour, depth or stencil (the sun's occlusion quad: LEQUAL, no
        // writes), unless a query counts it: then it is what the query
        // counts, and bit 2 draws it in the view's open pass (depth tested
        // there, its colour masked). Otherwise the command processor answers
        // the queries (EVENT_WRITE_ZPD stores zero samples, VIZ_QUERY sets
        // its bit), so the draw changes nothing; its pass of its own does.
        if ((s_passMerge & 1) && !zpdCount && writeMask == 0 && !(depthControl.z_enable && depthControl.z_write_enable) &&
            !depthControl.stencil_enable)
        {
            LogOnce(s_vs->hash ^ (s_ps->hash << 1) ^ 0x2600, "pass merge: vs_%016llx ps_%016llx writes nothing (depth control %08X): skipped",
                (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, depthControl.value);
            return;
        }
        // NFSMW_PASS_MERGE bit 2: a draw whose targets are some of the open
        // pass's (the sun's glow, colour only and additive, right after the
        // view's colour and depth draws) is drawn in that pass, with the
        // others attached but untouched: their colour writes masked, depth
        // and stencil tests off (the pipeline must have the pass's formats).
        // On its own it was a pass that loaded and stored its target (an
        // untiled one whole, 1280x2048) behind a full barrier, and the
        // view's pass started again after it. Only when no pixel anything
        // reads is lost: its area with its own targets is its area with the
        // pass's (the same pixels; untiled with NFSMW_UNTILED_AREA, both are
        // its viewport), or, untiled (that area is then the whole
        // target, and the pass's is shorter: the depth target's), with
        // clipping on and its viewport, the frame, inside the pass's
        // targets. What it loses then lies outside its viewport (the host
        // clips to the target, not the viewport) and at or beyond the pass's
        // extent: right only while nothing resolves rows at or beyond the
        // pass's extent (the frame's resolves read inside it). With clipping
        // off a draw may mean to reach past its viewport: it keeps its pass.
        uint32_t adoptedColor = 0;  // colour targets attached for the pass only
        bool adoptedDepth = false;
        if ((s_passMerge & 2) && s_passActive && (!depth || depth == s_passDepth))
        {
            bool subset = true, same = depth == s_passDepth;
            for (int i = 0; i < 4; i++)
            {
                subset = subset && (!color[i] || color[i] == s_passColor[i]);
                same = same && color[i] == s_passColor[i];
            }
            // As the scissor below (keep the two alike), in guest pixels,
            // with these targets (the pass's area is never larger: its
            // targets include these).
            auto areaWith = [&](RenderTarget* const* targets) {
                int32_t w = INT32_MAX, h = INT32_MAX;
                for (int i = 0; i < 5; i++)
                    if (targets[i])
                    {
                        w = std::min(w, int32_t(targets[i]->guestWidth));
                        h = std::min(h, int32_t(targets[i]->guestHeight));
                    }
                Rect sc = untiledPass ? UntiledScissor(targets, w, h) : GuestScissor();
                int32_t x0 = std::clamp(sc.x0, 0, w), y0 = std::clamp(sc.y0, 0, h);
                return Rect{ x0, y0, std::clamp(sc.x1, x0, w), std::clamp(sc.y1, y0, h) };
            };
            bool keeps = false;
            if (subset && !same)
            {
                RenderTarget* const own[5] = { color[0], color[1], color[2], color[3], depth };
                RenderTarget* const pass[5] = { s_passColor[0], s_passColor[1], s_passColor[2], s_passColor[3], s_passDepth };
                Rect a = areaWith(own), b = areaWith(pass);
                keeps = a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
                // (The viewport only where x and y are scaled and offset.)
                auto vte = Reg<reg::PA_CL_VTE_CNTL>();
                if (!keeps && untiledPass && (vte.value & 0xF) == 0xF && !Reg<reg::PA_CL_CLIP_CNTL>().clip_disable)
                {
                    float sx = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_XSCALE)), ox = RegF(XE_GPU_REG_PA_CL_VPORT_XOFFSET);
                    float sy = std::fabs(RegF(XE_GPU_REG_PA_CL_VPORT_YSCALE)), oy = RegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET);
                    keeps = ox - sx >= float(b.x0) && ox + sx <= float(b.x1) && oy - sy >= float(b.y0) && oy + sy <= float(b.y1);
                }
            }
            if (keeps)
            {
                for (int i = 0; i < 4; i++)
                    if (!color[i] && s_passColor[i])
                    {
                        color[i] = s_passColor[i];
                        adoptedColor |= 1u << i;
                    }
                adoptedDepth = !depth && s_passDepth;
                depth = s_passDepth;
                LogOnce(s_vs->hash ^ (s_ps->hash << 1) ^ 0x2601, "pass merge: vs_%016llx ps_%016llx drawn in the open pass (%s attached)",
                    (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash,
                    adoptedDepth ? (adoptedColor ? "colour and depth" : "depth") : "colour");
            }
        }
        // Before the textures are looked up: a draw into scaled targets
        // samples scaled textures (every attachment shares the pitch, so the scale).
        s_drawScaled = false;
        for (RenderTarget* rt : { color[0], color[1], color[2], color[3], depth })
            if (rt && !rt->scale.IsOne())
                s_drawScaled = true;

        // NFSMW_LOG_TILES=1: register state of the first draws under MSAA (predicated tiling).
        static const bool logTiles = std::getenv("NFSMW_LOG_TILES") != nullptr;
        if (logTiles && surface.msaa_samples != xenos::MsaaSamples::k1X)
        {
            static uint32_t logged = 0;
            static uint32_t lastKey = 0;
            auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
            auto wtl = Reg<reg::PA_SC_WINDOW_SCISSOR_TL>();
            auto wbr = Reg<reg::PA_SC_WINDOW_SCISSOR_BR>();
            auto stl = Reg<reg::PA_SC_SCREEN_SCISSOR_TL>();
            auto sbr = Reg<reg::PA_SC_SCREEN_SCISSOR_BR>();
            uint32_t key = wo.value ^ (wtl.value * 3) ^ (wbr.value * 7) ^ (stl.value * 11) ^ (sbr.value * 13);
            if (key != lastKey && logged < 60)
            {
                logged++;
                lastKey = key;
                fprintf(stderr, "[tiles] draw %llu: msaa %u pitch %u wo (%d,%d) woEn %u | window scissor (%u,%u)-(%u,%u) offDis %u | screen scissor (%d,%d)-(%d,%d) | color %08X depth %08X vp y %.1f+%.1f\n",
                    (unsigned long long)s_stats.draws, uint32_t(surface.msaa_samples), pitch, wo.window_x_offset, wo.window_y_offset,
                    uint32_t(Reg<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable), wtl.tl_x, wtl.tl_y, wbr.br_x, wbr.br_y,
                    wtl.window_offset_disable, stl.tl_x, stl.tl_y, sbr.br_x, sbr.br_y, s_regs[XE_GPU_REG_RB_COLOR_INFO],
                    s_regs[XE_GPU_REG_RB_DEPTH_INFO], RegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET), RegF(XE_GPU_REG_PA_CL_VPORT_YSCALE));
            }
        }

        // Primitive assembly.
        auto modeCntl = Reg<reg::PA_SU_SC_MODE_CNTL>();
        auto dmaSize = RegAt<reg::VGT_DMA_SIZE>(0);
        dmaSize.value = dmaSizeValue;
        IndexSource src{ initiator.source_select == xenos::SourceSelect::kDMA, dmaBase & 0x1FFFFFFF,
            initiator.index_size == xenos::IndexFormat::kInt32, uint32_t(dmaSize.swap_mode) };
        bool restartEnabled = modeCntl.multi_prim_ib_ena && src.indexed;
        uint32_t resetIndex = s_regs[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX] & 0xFFFFFF;

        DrawConstants pc{};
        VkPrimitiveTopology topology;
        bool restart = false;
        bool polygonal = true;
        std::vector<uint32_t> indices;  // converted/expanded (empty: plain vkCmdDraw)
        bool useIndices = false;
        bool directIndices = false;     // converted by ConvertIndices into the ring instead
        uint32_t vertexCount = count;
        auto convertAll = [&](bool allowRestart) {
            if ((g_cpOpt & CP_OPT_FAST_INDICES) &&
                uint64_t(src.base & 0x1FFFFFFF) + uint64_t(count) * (src.is32 ? 4 : 2) <= kSharedSize)
            {
                directIndices = true;
                useIndices = true;
                return;
            }
            indices.resize(count);
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t v = src.Get(i);
                indices[i] = allowRestart && v == resetIndex ? 0xFFFFFFFFu : v;
            }
            useIndices = true;
        };
        // Splits the draw at reset indices and hands each run to `emit`.
        auto forEachRun = [&](auto&& emit) {
            std::vector<uint32_t> run;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t v = src.Get(i);
                if (restartEnabled && v == resetIndex)
                {
                    emit(run);
                    run.clear();
                    continue;
                }
                run.push_back(v);
            }
            emit(run);
            useIndices = true;
        };

        // NFSMW_VS_RECT=1 (an A/B switch): every draw with the rectangle list
        // variant of its vertex shader, as before it had one.
        static const bool s_vsRectAll = [] { const char* v = std::getenv("NFSMW_VS_RECT"); return v && v[0] == '1'; }();
        bool rectList = s_vsRectAll;  // a rectangle list: the vertex shader's SB_RECT variant
        switch (initiator.prim_type)
        {
        case xenos::PrimitiveType::kPointList:
            topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            polygonal = false;
            if (src.indexed) convertAll(false);
            break;
        case xenos::PrimitiveType::kLineList:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            polygonal = false;
            if (src.indexed) convertAll(false);
            break;
        case xenos::PrimitiveType::kLineStrip:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            polygonal = false;
            restart = restartEnabled;
            if (src.indexed) convertAll(restart);
            break;
        case xenos::PrimitiveType::kTriangleList:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            if (src.indexed) convertAll(false);
            break;
        case xenos::PrimitiveType::kTriangleStrip:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            restart = restartEnabled;
            if (src.indexed) convertAll(restart);
            break;
        case xenos::PrimitiveType::kTriangleFan:
        case xenos::PrimitiveType::kPolygon:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            forEachRun([&](const std::vector<uint32_t>& r) {
                for (size_t k = 2; k < r.size(); k++)
                    indices.insert(indices.end(), { r[0], r[k - 1], r[k] });
            });
            break;
        case xenos::PrimitiveType::kQuadList:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            forEachRun([&](const std::vector<uint32_t>& r) {
                for (size_t k = 0; k + 3 < r.size(); k += 4)
                    indices.insert(indices.end(), { r[k], r[k + 1], r[k + 2], r[k], r[k + 2], r[k + 3] });
            });
            break;
        case xenos::PrimitiveType::kQuadStrip:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            forEachRun([&](const std::vector<uint32_t>& r) {
                for (size_t k = 0; k + 3 < r.size(); k += 2)
                    indices.insert(indices.end(), { r[k], r[k + 1], r[k + 3], r[k], r[k + 3], r[k + 2] });
            });
            break;
        case xenos::PrimitiveType::kLineLoop:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            polygonal = false;
            forEachRun([&](const std::vector<uint32_t>& r) {
                for (size_t k = 0; k + 1 < r.size(); k++)
                    indices.insert(indices.end(), { r[k], r[k + 1] });
                if (r.size() > 2)
                    indices.insert(indices.end(), { r.back(), r[0] });
            });
            break;
        case xenos::PrimitiveType::kRectangleList:
            // Expanded by the vertex shader (4th corner = v1 + v2 - v0).
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            vertexCount = count / 3 * 6;
            pc.indexInfo = 1u << 3;
            rectList = true;
            if (src.indexed)
            {
                pc.indexAddress = src.base;
                pc.indexInfo |= (src.is32 ? 1u : 0u) | (src.endian << 1);
            }
            break;
        default:
            s_stats.skipped++;
            LogOnce(0x1000 + uint32_t(initiator.prim_type), "primitive type %u not supported",
                uint32_t(initiator.prim_type));
            return;
        }
        if (useIndices)
            vertexCount = directIndices ? count : uint32_t(indices.size());
        // 16-bit indices when 0xFFFF means what the pipeline says: restart
        // with the guest's reset index 0xFFFF, or no restart (an index
        // 0xFFFF is then a vertex like any other). Metal can't turn primitive
        // restart off: there, without restart, only when no index is 0xFFFF
        // (a scan of them; elsewhere it was ~8% of the command processor's
        // time on a Steam Deck at 5 W, its hottest loop with the conversion).
        bool indices16 = false;
        if (directIndices && !src.is32)
        {
            if (restart)
                indices16 = resetIndex == 0xFFFF;
            else
            {
#ifdef __APPLE__
                const uint16_t* p = reinterpret_cast<const uint16_t*>(s_sharedHost + (src.base & 0x1FFFFFFE));
                indices16 = std::find(p, p + count, uint16_t(0xFFFF)) == p + count;  // byte order doesn't matter
#else
                indices16 = true;
#endif
            }
        }
        const VkDeviceSize indexBytes = directIndices ? VkDeviceSize(count) * (indices16 ? 2 : 4) : indices.size() * 4;
        if (vertexCount == 0)
            return;

        // Pipeline state.
        const TextureLayout& layout = GetTextureLayout(*s_vs, *s_ps);
        PipelineKey key{};
        key.vs = s_vs->hash;
        key.ps = s_ps->hash;
        key.topology = topology;
        key.restart = restart;
        static const uint32_t blendRegs[4] = { XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1,
            XE_GPU_REG_RB_BLENDCONTROL2, XE_GPU_REG_RB_BLENDCONTROL3 };
        for (int i = 0; i < 4; i++)
        {
            key.blend[i] = ~0u;
            if (!color[i])
                continue;
            key.colorFormats[i] = color[i]->vkFormat;
            uint32_t bc = s_regs[blendRegs[i]] & 0x1FFF1FFF;
            // (One, Zero, Add) for both colour and alpha is "blending off".
            if (bc != 0x00010001 && color[i]->blendable && !(adoptedColor & (1u << i)))
                key.blend[i] = bc;
        }
        key.colorMask = writeMask;  // (none for adopted targets)
        if (depth)
        {
            key.depthFormat = depth->vkFormat;
            key.depthControl = adoptedDepth ? 0u : depthControl.value;  // adopted: no depth or stencil test or write
        }
        auto clipCntl = Reg<reg::PA_CL_CLIP_CNTL>();
        // Polygon offset (gpu/poly_offset.h): the face's that is on and
        // drawn, the front's first, as Xenia chooses (Vulkan has one per
        // draw); until v0.1.4 always the front registers, also for a pass
        // culling front faces and offsetting the back ones.
        const gpu::PolyOffset polyOffset = gpu::PreferredPolyOffset({ .polygonal = polygonal,
            .frontEnable = modeCntl.poly_offset_front_enable != 0,
            .backEnable = modeCntl.poly_offset_back_enable != 0,
            .cullFront = modeCntl.cull_front != 0,
            .cullBack = modeCntl.cull_back != 0,
            .frontScale = RegF(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE),
            .frontOffset = RegF(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET),
            .backScale = RegF(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE),
            .backOffset = RegF(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET) });
        const bool depthBias = polyOffset.enable;
        uint32_t cull = 0;
        if (polygonal && initiator.prim_type != xenos::PrimitiveType::kRectangleList)
            cull = (modeCntl.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) | (modeCntl.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
        auto colorControlKey = Reg<reg::RB_COLORCONTROL>();
        // (The alpha test tests the pixel shader's alpha: none depth-only.)
        bool alphaTest = colorControlKey.alpha_test_enable && colorControlKey.alpha_func != xenos::CompareFunction::kAlways &&
            gpu::RunsGuestPixelShader(edramMode, s_depthOnlyGuestPs);
        // (Raster bit 9: the rectangle list variant of the vertex shader.)
        key.raster = (rectList ? 512u : 0u) | cull | (modeCntl.face ? 4u : 0u) | (clipCntl.clip_disable ? 8u : 0u) | (depthBias ? 16u : 0u) |
            (alphaTest ? 32u : 0u);
        // CP_OPT_DYNAMIC_STATE: cull mode, front face and the depth and
        // stencil tests are set per draw (below, from these), so draws that
        // differ only in them share a pipeline. In a race 1 draw in 20 bound
        // another pipeline only for them (the front face of mirrored meshes,
        // depth writes on and off), and a pipeline bind costs RADV about
        // 1 us (it emits the whole pipeline's state again at the next draw).
        const uint32_t rasterState = key.raster, depthState = key.depthControl;
        if (s_dynamicState)
        {
            key.raster = (key.raster & ~7u) | 64u;
            key.depthControl = 0;
        }

        bool pipelinePending;
        VkPipeline pipeline = GetPipeline(key, layout.pipeline, pipelinePending);
        if (pipelinePending)
            return;  // still compiling (counted in OnSwap's log)
        if (pipeline == VK_NULL_HANDLE)
            return skip(5, "pipeline creation failed");

        // Textures (may upload through the ring and so flush: before the
        // constants are allocated): into TextureCache::infos, [i] binding i
        // of the layout, unless the draw memo still holds them
        // (CP_OPT_TEXTURE_MEMO: in a race ~2 in 3 draws with textures).
        TextureCache& textureCache = *s_textureCache;
        if (textureCache.on && settings::Generation() != textureCache.settingsGeneration)
        {
            textureCache.settingsGeneration = settings::Generation();  // the anisotropy setting, in the samplers
            textureCache.generation++;
        }
        const uint32_t textureCount = layout.bindingCount;
        if (textureCount)
        {
            const uint64_t epoch = writewatch::WriteEpoch();
            const bool drawScaled = s_drawScaled && s_scaled.enabled;
            if (!textureCache.memoOn || !TextureMemoHolds(layout, epoch, drawScaled))
                LookupTextures(layout, epoch, drawScaled);
            else if (!textureCache.verify)
            {
                textureCache.memoDraws++;
                textureCache.memoBindings += textureCount;
            }
            else
            {
                // NFSMW_CP_CACHE_VERIFY: the lookups the memo stands for,
                // done anyway, must give what it holds.
                VkDescriptorImageInfo memo[96];
                std::copy(textureCache.infos, textureCache.infos + textureCount, memo);
                LookupTextures(layout, epoch, drawScaled);
                for (uint32_t i = 0; i < textureCount; i++)
                {
                    const VkDescriptorImageInfo& now = textureCache.infos[i];
                    if (now.imageView == memo[i].imageView && now.sampler == memo[i].sampler && now.imageLayout == memo[i].imageLayout)
                        continue;
                    if (textureCache.mismatches++ < 50)
                        fprintf(stderr, "[cpverify] texture draw memo, binding %u: view %p sampler %p, looked up %p %p (epoch %llu)\n",
                            uint32_t(layout.bindings[i]), (void*)memo[i].imageView, (void*)memo[i].sampler, (void*)now.imageView,
                            (void*)now.sampler, (unsigned long long)epoch);
                }
            }
        }
        const VkDescriptorImageInfo* const textureInfos = textureCache.infos;

        // Vertex data the guest may reuse before the GPU runs this draw (its
        // fences are published early, see g_eagerWrites): small ranges are
        // copied into the ring now and the shader reads the copy (vmem);
        // larger ones (static meshes, the triple-buffered dynamic buffers)
        // are guarded by the write watch. A range the GPU itself writes
        // first (a resolve into vertex memory) is never copied from the CPU.
        // Vertex data snapshots, copied into the ring with this draw's
        // constants and indices (one allocation: a submit between two would
        // leave the snapshot in the previous submission's slot). Each holds
        // only what the fetches through its range can read (gpu/vertex_snapshot.h:
        // `use`, spanned once the indices are converted; a range two
        // constants or both shaders read is copied once, for both), so a
        // draw of a few vertices of a buffer no longer copies all of it.
        // NFSMW_CP_OPT without CP_OPT_VERTEX_SPANS: whole ranges, as before.
        struct SnapshotRange { uint32_t base, size; vsnap::FetchUse use; };
        SnapshotRange copies[4];
        uint32_t copyCount = 0;
        VkDeviceSize copyBytes = 0;  // whole ranges: the most the copies take
        if (g_eagerWrites || s_shadowMode)
        {
            constexpr uint32_t kSnapshotMax = 64 * 1024;
            using Range = SnapshotRange;
            // HFR M3: the draw's vertex indices (with VGT_INDX_OFFSET), for a
            // large buffer the CPU rewrites (a triple-buffered dynamic one:
            // the sprite batch's three 128,000-byte slots under one fetch
            // constant), so only this draw's vertices' records are copied or
            // guarded and the CPU can fill the next slot while the GPU reads
            // this one (the guard stalled sub_82445198 there 2.3 ms a second
            // at 120 fps on the M1 Pro: M3's run d4a). Indexed draws are
            // scanned up to kNarrowScan indices; static buffers (no CPU write
            // since their last draw) keep the whole range.
            constexpr uint32_t kNarrowScan = 4096;
            int64_t vertexMin = -1, vertexMax = -1;  // -2: unknown
            auto drawVertices = [&]() {
                if (vertexMin != -1)
                    return vertexMin >= 0;
                vertexMin = vertexMax = -2;
                const uint32_t first = s_regs[XE_GPU_REG_VGT_INDX_OFFSET];
                if (count == 0)
                    return false;
                if (!src.indexed)
                {
                    vertexMin = int64_t(first);
                    vertexMax = int64_t(first) + count - 1;
                    return true;
                }
                if (count > kNarrowScan || uint64_t(src.base) + uint64_t(count) * (src.is32 ? 4 : 2) > kSharedSize)
                    return false;
                uint32_t lo = ~0u, hi = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    const uint32_t v = src.Get(i);
                    if (restartEnabled && v == resetIndex)
                        continue;
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                if (lo > hi)
                    return false;
                vertexMin = int64_t(lo) + first;
                vertexMax = int64_t(hi) + first;
                return true;
            };
            auto narrow = [&](Range& r, uint32_t constant) {
                const auto& info = s_vs->info;
                const uint32_t stride = info.vertexStride[constant] * 4u, extent = info.vertexExtent[constant] * 4u;
                if (((info.vertexIndexOther[constant / 32] >> (constant % 32)) & 1) || stride == 0 || extent == 0)
                    return;
                // Static buffers keep the whole range (no scan for them).
                if (src.indexed && !DynamicVertexRange(r.base, r.size))
                    return;
                if (!drawVertices())
                    return;
                const uint64_t lo = uint64_t(vertexMin) * stride, hi = uint64_t(vertexMax) * stride + std::max(stride, extent);
                if (lo >= r.size)
                    return;
                const uint32_t newBase = r.base + uint32_t(lo), newEnd = r.base + uint32_t(std::min<uint64_t>(hi, r.size));
                if (newEnd > newBase && newEnd - newBase < r.size)
                {
                    s_stats.narrowedRanges++;
                    r = { newBase, newEnd - newBase };
                }
            };
            for (const Shader* shader : { s_vs, s_ps })
                for (const vsnap::FetchUse& use : shader->info.vertexFetches)
                {
                    const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + use.constant * 2];
                    Range r{ vf[0] & 0x1FFFFFFC, ((vf[1] >> 2) & 0xFFFFFF) * 4, use };
                    r.size = std::min<uint32_t>(r.size, uint32_t(kSharedSize) - r.base);
                    if (!(g_cpOpt & CP_OPT_VERTEX_SPANS))
                        r.use.vertexIndexed = false;
                    if (r.size == 0)
                        continue;
                    Range* same = std::find_if(copies, copies + copyCount, [&](const Range& c) { return c.base == r.base && c.size == r.size; });
                    if (same != copies + copyCount)
                    {
                        vsnap::Merge(same->use, r.use);
                        continue;
                    }
                    if (g_eagerWrites && r.size <= kSnapshotMax && copyCount < 4 &&
                        !writewatch::GpuWritePending(r.base, r.size) && !writewatch::AnyGpuOwned(r.base, r.size))
                    {
                        copies[copyCount++] = r;
                        copyBytes += (r.size + 255) & ~255u;
                    }
                    else
                    {
                        EnsureShadow(r.base, r.size);
                        GuardGpu(r.base, r.size, false, "vertices");
                    }
                }
            // Indices the shader fetches itself.
            if (pc.indexAddress)
            {
                EnsureShadow(pc.indexAddress, count << (src.is32 ? 2 : 1));
                GuardGpu(pc.indexAddress, count << (src.is32 ? 2 : 1), false, "indices");
            }
        }

        // NFSMW_LOG_TEXTURE_USE=<hex base>: dump the shaders of draws that
        // sample the texture at that address (once per shader pair).
        if (s_drawUsesLoggedTexture)
        {
            s_drawUsesLoggedTexture = false;
            static std::unordered_set<uint64_t> dumped;
            if (dumped.insert(s_vs->hash ^ (s_ps->hash << 1)).second)
            {
                std::string path = std::format("build/texuse_vs_{:016x}.glsl", s_vs->hash);
                if (FILE* f = fopen(path.c_str(), "w")) { fputs(s_vs->info.glsl.c_str(), f); fclose(f); }
                path = std::format("build/texuse_ps_{:016x}.glsl", s_ps->hash);
                if (FILE* f = fopen(path.c_str(), "w")) { fputs(s_ps->info.glsl.c_str(), f); fclose(f); }
                fprintf(stderr, "[texuse] %08X (%s) sampled by vs %016llx ps %016llx; vertex fetch constants:", s_logTextureUse, s_loggedTextureFetch.c_str(),
                    (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash);
                for (uint32_t word = 0; word < 3; word++)
                    for (uint32_t bits = s_vs->info.vertexFetchConstants[word]; bits; bits &= bits - 1)
                    {
                        uint32_t c = word * 32 + uint32_t(__builtin_ctz(bits));
                        const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + c * 2];
                        fprintf(stderr, " [%u: %08X %08X]", c, vf[0], vf[1]);
                        uint32_t words = std::min<uint32_t>((vf[1] >> 2) & 0xFFFFFF, 128);
                        for (uint32_t i = 0; i < words; i++)
                        {
                            uint32_t bits = GpuSwap(LoadPhysical((vf[0] & 0x1FFFFFFC) + i * 4), vf[1] & 3);
                            float fv;
                            memcpy(&fv, &bits, 4);
                            fprintf(stderr, "%s%.3f", i % 20 == 0 ? "\n[texuse]   v: " : " ", fv);
                        }
                    }
                fprintf(stderr, "\n");
            }
        }

        // Per-draw data in the ring (may submit when full, so before
        // recording). Constants are uploaded only when the game wrote any
        // constant register since the last upload (g_constantsDirty) or the
        // last copy is in an older submission's slot; one allocation holds
        // both, since a submit between two would recycle the first.
        VkDeviceSize constantsOffset = s_lastConstantsOffset;
        VkDeviceSize indexOffset = 0;
        // A shader with packed constants reads its used ones from slots
        // 0..N-1 of its stage's range, so the upload's layout is the shader's
        // (null: the full file). ~20 vec4s instead of 256 per stage: constants
        // were 9 KB an upload, ~2,500 uploads a frame into the BAR ring.
        const Shader* constLayoutVs = s_vs->info.constPacked ? s_vs : nullptr;
        const Shader* constLayoutPs = s_ps->info.constPacked ? s_ps : nullptr;
        while (true)
        {
            bool uploadConstants = g_constantsDirty.load(std::memory_order_relaxed) ||
                s_lastConstantsSubmission != s_submissionCounter || !s_recording ||
                constLayoutVs != s_lastConstLayoutVs || constLayoutPs != s_lastConstLayoutPs;
            VkDeviceSize need = (uploadConstants ? kConstantsSize : 0) + (useIndices ? 256 + indexBytes : 0) +
                (copyCount ? 256 + copyBytes : 0);
            if (need == 0)
                break;
            uint64_t before = s_submissionCounter;
            VkDeviceSize at = RingAllocate(need);
            if (!uploadConstants && s_submissionCounter != before)
                continue;  // submitted meanwhile: the cached constants are gone
            if (uploadConstants)
            {
                g_constantsDirty.store(false, std::memory_order_relaxed);
                constantsOffset = at;
                uint8_t* c = s_ringHost + at;
                auto copyStage = [&](const Shader* packed, uint32_t base) {
                    const uint32_t* src = &s_regs[XE_GPU_REG_SHADER_CONSTANT_000_X + base * 4];
                    if (!packed)
                    {
                        memcpy(c + base * 16, src, 256 * 16);
                        return;
                    }
                    uint32_t slot = 0;
                    for (uint32_t q = 0; q < 4; q++)
                        for (uint64_t bits = packed->info.constUsed[q]; bits; bits &= bits - 1, slot++)
                            memcpy(c + (base + slot) * 16, src + (q * 64 + uint32_t(__builtin_ctzll(bits))) * 4, 16);
                };
                copyStage(constLayoutVs, 0);    // pc.vsConstBase
                copyStage(constLayoutPs, 256);  // pc.psConstBase
                s_lastConstLayoutVs = constLayoutVs;
                s_lastConstLayoutPs = constLayoutPs;
                memcpy(c + 512 * 16, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 48 * 16);
                memcpy(c + 560 * 16, &s_regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 2 * 16);
                memcpy(c + 562 * 16, &s_regs[XE_GPU_REG_SHADER_CONSTANT_LOOP_00], 8 * 16);
                memset(c + 570 * 16, 0, 4 * 16);
                s_lastConstantsOffset = at;
                s_lastConstantsSubmission = s_submissionCounter;
                s_stats.constantUploads++;
            }
            // The vertex indices the vertex shader runs for, as its index
            // buffer holds them: the draw's own count without one; not
            // scanned when the shader fetches its indices itself (pc.indexAddress).
            vsnap::IndexRange vertices;
            if (!useIndices && !pc.indexAddress)
                vertices = { true, 0, count - 1 };
            if (useIndices)
            {
                indexOffset = (at + (uploadConstants ? kConstantsSize : 0) + 255) & ~VkDeviceSize(255);
                if (directIndices)
                {
                    const IndexEntry* cached = nullptr;
                    if (g_cpOpt & CP_OPT_INDEX_CACHE)
                    {
                        // (Only a restart index the output's own: the bits below say the rest.)
                        const uint32_t how = (src.is32 ? 1u : 0u) | (src.endian & 3u) << 1 | (indices16 ? 8u : 0u) | (restart ? 16u : 0u);
                        if (!restart || resetIndex == (src.is32 ? 0xFFFFFFu : 0xFFFFu))
                            cached = CachedIndices(src.base & (src.is32 ? 0x1FFFFFFCu : 0x1FFFFFFEu), uint32_t(count) * (src.is32 ? 4 : 2), how,
                                [&](uint8_t* dst) { return ConvertIndices(dst, src, count, indices16, restart, resetIndex); });
                    }
                    if (cached)
                    {
                        indexOffset = cached->offset;
                        vertices = cached->vertices;
                    }
                    else
                        vertices = ConvertIndices(s_ringHost + indexOffset, src, count, indices16, restart, resetIndex);
                    // NFSMW_CP_OPT_VERIFY=1: compare with the plain conversion
                    // (and its range). It reads the ring back, slowly: =spans
                    // checks only the vertex snapshots' spans (below).
                    static const bool verify = [] { const char* v = std::getenv("NFSMW_CP_OPT_VERIFY"); return v && strcmp(v, "spans") != 0; }();
                    if (verify)
                    {
                        static uint64_t checked = 0, bad = 0;
                        uint32_t lo = UINT32_MAX, hi = 0;
                        for (uint32_t i = 0; i < count; i++)
                        {
                            uint32_t v = src.Get(i);
                            uint32_t want = restart && v == resetIndex ? 0xFFFFFFFFu : v, got;
                            if (want != 0xFFFFFFFFu)
                            {
                                lo = std::min(lo, want);
                                hi = std::max(hi, want);
                            }
                            if (indices16)
                            {
                                uint16_t g16;
                                memcpy(&g16, s_ringHost + indexOffset + size_t(i) * 2, 2);
                                got = restart && g16 == 0xFFFF ? 0xFFFFFFFFu : g16;
                            }
                            else
                                memcpy(&got, s_ringHost + indexOffset + size_t(i) * 4, 4);
                            if (got != want && bad++ < 10)
                                fprintf(stderr, "[cpverify] index mismatch at %u: %08X, want %08X (32-bit %d, endian %u, restart %d)\n",
                                    i, got, want, int(src.is32), src.endian, int(restart));
                        }
                        bool none = lo > hi;
                        if ((none ? vertices.lo <= vertices.hi : vertices.lo != lo || vertices.hi != hi) && bad++ < 10)
                            fprintf(stderr, "[cpverify] index range %u..%u, want %u..%u (%u indices, 32-bit %d, restart %d)\n",
                                vertices.lo, vertices.hi, lo, hi, count, int(src.is32), int(restart));
                        if (++checked % 100000 == 0)
                            fprintf(stderr, "[cpverify] %llu indexed draws checked, %llu mismatches\n", (unsigned long long)checked, (unsigned long long)bad);
                    }
                }
                else
                {
                    memcpy(s_ringHost + indexOffset, indices.data(), indices.size() * 4);
                    if (copyCount)
                    {
                        // (As ConvertIndices: a restart, ~0, wraps to 0.)
                        uint32_t lo = UINT32_MAX, hiPlus1 = 0;
                        for (uint32_t v : indices)
                        {
                            lo = std::min(lo, v);
                            hiPlus1 = std::max(hiPlus1, v + 1);
                        }
                        vertices = hiPlus1 ? vsnap::IndexRange{ true, lo, hiPlus1 - 1 } : vsnap::IndexRange{ true, 1, 0 };
                    }
                }
            }
            if (copyCount)
            {
                // As the shader sees them (+ VGT_INDX_OFFSET, pc.indexOffset).
                vertices = vsnap::WithOffset(vertices, s_regs[XE_GPU_REG_VGT_INDX_OFFSET]);
                // NFSMW_CP_OPT_VERIFY=1 or =spans: the whole range stays
                // redirected, its bytes outside the span set to 0xFF (NaN in
                // the float formats): a fetch the span missed draws visibly
                // wrong (compare the frames with a run without: timedemo
                // shots, docs/timedemo.md).
                static const bool poison = std::getenv("NFSMW_CP_OPT_VERIFY") != nullptr;
                VkDeviceSize snapshot = (at + (uploadConstants ? kConstantsSize : 0) + (useIndices ? 256 + indexBytes : 0) + 255) & ~VkDeviceSize(255);
                for (uint32_t i = 0; i < copyCount; i++)
                {
                    const vsnap::Span span = vsnap::Needed(copies[i].use, vertices, copies[i].size);
                    uint32_t start = span.start, size = span.Size();
                    if (poison)
                    {
                        memset(s_ringHost + snapshot, 0xFF, copies[i].size);
                        memcpy(s_ringHost + snapshot + start, s_sharedHost + copies[i].base + start, size);
                        start = 0;
                        size = copies[i].size;
                    }
                    else if (g_cpOpt & CP_OPT_SNAPSHOT_CACHE)
                    {
                        const SnapshotEntry* e = CachedSnapshot(copies[i].base, copies[i].size, start, size);
                        if (e && !s_snapshotShadow)
                        {
                            pc.vfRedirect[i][0] = copies[i].base + e->start;
                            pc.vfRedirect[i][1] = e->length;
                            pc.vfRedirect[i][2] = uint32_t(e->offset);
                            continue;
                        }
                        memcpy(s_ringHost + snapshot, s_sharedHost + copies[i].base + start, size);
                    }
                    else
                        memcpy(s_ringHost + snapshot, s_sharedHost + copies[i].base + start, size);
                    pc.vfRedirect[i][0] = copies[i].base + start;
                    pc.vfRedirect[i][1] = size;
                    pc.vfRedirect[i][2] = uint32_t(snapshot);
                    snapshot += (size + 255) & ~255u;
                    s_stats.snapshotBytes += (size + 255) & ~255u;
                }
                // The allocation (this draw's last) made room for whole
                // ranges: what the spans left goes back to the slot.
                s_ringOffset = std::min(s_ringOffset, snapshot - VkDeviceSize(s_slot) * kSlotSize);
            }
            break;
        }

        // The attachments' common size (the pass extent) and this draw's
        // scissor, which is also the area the pass must cover (NFSMW_PASS_MERGE
        // repeats it before adopting the open pass's targets: areaWith).
        // Untiled, tile 0 draws the whole frame: the whole target, or with
        // NFSMW_UNTILED_AREA its viewport (UntiledScissor).
        // In guest pixels; the host viewport and scissor are x (scaleX, scaleY)
        // (every attachment of a pass shares the surface pitch, so its scale),
        // the scissor's edges as gpu/render_scale.h maps them (HostRect).
        uint32_t targetW = UINT32_MAX, targetH = UINT32_MAX;
        gpu::RenderScale scale;
        for (RenderTarget* rt : { color[0], color[1], color[2], color[3], depth })
            if (rt)
            {
                targetW = std::min(targetW, rt->guestWidth);
                targetH = std::min(targetH, rt->guestHeight);
                scale = rt->scale;
            }
        float w = float(targetW), h = float(targetH);
        const float scaleX = scale.x.Value(), scaleY = scale.y.Value();
        RenderTarget* const attached[5] = { color[0], color[1], color[2], color[3], depth };
        Rect sc = GuestScissor();
        int untiledWhy = 4;
        if (untiledPass)
            sc = UntiledScissor(attached, int32_t(w), int32_t(h), &untiledWhy);
        int32_t x0 = std::clamp(sc.x0, 0, int32_t(w)), y0 = std::clamp(sc.y0, 0, int32_t(h));
        int32_t x1 = std::clamp(sc.x1, x0, int32_t(w)), y1 = std::clamp(sc.y1, y0, int32_t(h));
        // NFSMW_UNTILED_AREA: a draw whose area its targets' open pass doesn't
        // cover (a whole-target draw after cut ones, another viewport) would
        // restart that pass over the union, a pass more and the union
        // reloaded, which no untiled draw did before (each one's area was the
        // whole target): those targets keep the whole target from now on, as
        // this draw does, so they restart this once. (Checked before a vertex
        // barrier may end the pass: then it gives way a draw early.)
        // In split screen (gpu/split_views.h) the second view's first draw
        // restarts the pass over both instead (BeginPass), once a frame.
        if (untiledPass && untiledWhy != 4 && x1 > x0 && y1 > y0 && s_passActive && depth == s_passDepth &&
            std::equal(color, color + 4, s_passColor) &&
            !Contains(s_passArea, HostRect(scale, x0, y0, x1, y1)) && !gpu::split_views::FallsBack(SplitFrame()))
        {
            const VkRect2D want = HostRect(scale, x0, y0, x1, y1);
            LogOnce(0x35ull << 56, "split screen: frame %llu: a draw cut to %ux%u@%d,%d restarts its targets' pass (area %ux%u@%d,%d) over "
                "both (scale %.2fx%.2f)", (unsigned long long)s_frame, want.extent.width, want.extent.height, want.offset.x, want.offset.y,
                s_passArea.extent.width, s_passArea.extent.height, s_passArea.offset.x, s_passArea.offset.y, scaleX, scaleY);
        }
        else if (untiledPass && untiledWhy != 4 && x1 > x0 && y1 > y0 && s_passActive && depth == s_passDepth &&
            std::equal(color, color + 4, s_passColor) &&
            !Contains(s_passArea, HostRect(scale, x0, y0, x1, y1)))
        {
            uint64_t key = 0;
            for (RenderTarget* rt : attached)
                if (rt)
                {
                    rt->wholeArea = true;
                    key = key * 0x9E3779B1ull + ((uint64_t(rt->base) << 20) ^ (uint64_t(rt->pitch) << 1) ^ uint64_t(rt->depth));
                }
            const VkRect2D want = HostRect(scale, x0, y0, x1, y1);
            LogOnce((0x33ull << 56) | (key & 0xFFFFFFFFFFFFFFull), "untiled area: frame %llu vs_%016llx ps_%016llx would restart its targets' "
                "pass (area %ux%u@%d,%d) for %ux%u@%d,%d: they keep the whole target from now on", (unsigned long long)s_frame,
                (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, s_passArea.extent.width, s_passArea.extent.height,
                s_passArea.offset.x, s_passArea.offset.y, want.extent.width, want.extent.height, want.offset.x, want.offset.y);
            if (untiledWhy == 0)
            {
                x0 = y0 = 0;
                x1 = int32_t(w);
                y1 = int32_t(h);
                untiledWhy = 3;
            }
        }
        if (untiledPass && untiledWhy != 4 && s_logUntiledArea)
        {
            if (untiledWhy != 0)
                s_ua.whole++;
            else if (x1 <= x0 || y1 <= y0)
                s_ua.empty++;
            else
                s_ua.cut++;
            if (untiledWhy == 1)
                s_ua.clipOff++;
            else if (untiledWhy == 2)
                s_ua.noViewport++;
            else if (untiledWhy == 3)
                s_ua.wholeArea++;
            if (untiledWhy == 1 || untiledWhy == 2)
                LogOnce((0x30ull << 56) ^ s_vs->hash ^ (s_ps->hash << 1), "untiled area: vs_%016llx ps_%016llx keeps the whole target (%s; VTE %03X, clip control %08X)",
                    (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, untiledWhy == 1 ? "clipping off" : "no viewport transform",
                    Reg<reg::PA_CL_VTE_CNTL>().value & 0xFFF, Reg<reg::PA_CL_CLIP_CNTL>().value);
            // Each distinct cut per surface (pitch, MSAA, window scissor),
            // once (the map looked up only when it differs from the last).
            if (untiledWhy == 0)
            {
                static uint64_t lastKey = 0;
                auto wtl = Reg<reg::PA_SC_WINDOW_SCISSOR_TL>();
                auto wbr = Reg<reg::PA_SC_WINDOW_SCISSOR_BR>();
                uint64_t key = (0x32ull << 56) ^ (uint64_t(pitch) << 40) ^ (uint64_t(surface.msaa_samples) << 38) ^ (uint64_t(wbr.br_x) << 50) ^
                    (uint64_t(x0) << 26) ^ (uint64_t(y0) << 14) ^ (uint64_t(x1) << 2) ^ (uint64_t(y1) * 0x9E3779B1ull);
                if (key != lastKey)
                    LogOnce(key, "untiled area: pitch %u msaa %u target %dx%d, window scissor (%u,%u)-(%u,%u): draws cut to (%d,%d)-(%d,%d)%s",
                        pitch, uint32_t(surface.msaa_samples), int32_t(w), int32_t(h), wtl.tl_x, wtl.tl_y, wbr.br_x, wbr.br_y, x0, y0, x1, y1,
                        x1 <= x0 || y1 <= y0 ? " (nothing: not drawn)" : "");
                lastKey = key;
            }
        }
        // NFSMW_UNTILED_AREA, for Resolve's check: the untiled draws cut short
        // of the attachments' common size (a smaller attachment cuts the
        // others as before), even to nothing, what they drew and that size.
        // Whole-target draws lose nothing, nor do the targets a draw adopts
        // (never written). Two cuts whose union isn't exact (disjoint
        // viewports: the gap would count as drawn) fall back to the whole
        // target from the next draw.
        if (untiledPass && s_untiledArea)
        {
            const bool cut = untiledWhy == 0 && (x0 > 0 || y0 > 0 || x1 < int32_t(w) || y1 < int32_t(h));
            const VkRect2D drew = HostRect(scale, x0, y0, x1, y1);
            for (int i = 0; i < 5; i++)
                if (RenderTarget* rt = attached[i]; rt && !(i < 4 ? (adoptedColor >> i) & 1 : adoptedDepth))
                {
                    if (rt->areaFrame != s_frame)
                    {
                        rt->areaFrame = s_frame;
                        rt->cut = false;
                    }
                    if (!cut)
                        continue;
                    if (!rt->cut)
                    {
                        rt->cutDrawn = {};
                        rt->cutReach = {};
                    }
                    rt->cut = true;
                    rt->cutReach = { std::max(rt->cutReach.width, scale.x.Size(uint32_t(w))), std::max(rt->cutReach.height, scale.y.Size(uint32_t(h))) };
                    // (Below 1x a cut narrower than a host pixel draws nothing
                    // too, as the scissor check below finds; a rectangle of
                    // zero size here would end the target's saving.)
                    if (x1 <= x0 || y1 <= y0 || drew.extent.width == 0 || drew.extent.height == 0)
                        continue;
                    const VkRect2D& was = rt->cutDrawn;
                    if (was.extent.width && !Contains(was, drew) && !Contains(drew, was) && !Adjoins(was, drew) &&
                        gpu::split_views::FallsBack(SplitFrame()))
                    {
                        rt->wholeArea = true;
                        LogOnce((0x34ull << 56) | (uint64_t(rt->base) << 24) | (uint64_t(rt->pitch) << 1) | uint64_t(rt->depth),
                            "untiled area: frame %llu target b%u p%u%s cut to %ux%u@%d,%d and %ux%u@%d,%d: its draws keep the whole target from now on",
                            (unsigned long long)s_frame, rt->base, rt->pitch, rt->depth ? " (depth)" : "", was.extent.width, was.extent.height,
                            was.offset.x, was.offset.y, drew.extent.width, drew.extent.height, drew.offset.x, drew.offset.y);
                    }
                    rt->cutDrawn = was.extent.width ? Union(was, drew) : drew;
                }
        }
        if (x1 <= x0 || y1 <= y0)
            return;
        VkRect2D scissor = HostRect(scale, x0, y0, x1, y1);
        // Below 1x a scissor narrower than a host pixel holds none (at 0.75x
        // one guest column at a block's start): nothing to draw, and a pass
        // can't have an empty area. (At 1x and above every guest pixel has
        // one or more: never.)
        if (scissor.extent.width == 0 || scissor.extent.height == 0)
            return;

        // Viewport: the guest transform is folded into the vertex shader
        // (pc.posScale) and the host viewport covers the whole target.
        auto vte = Reg<reg::PA_CL_VTE_CNTL>();
        float sx = vte.vport_x_scale_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
        float sy = vte.vport_y_scale_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
        float sz = vte.vport_z_scale_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_ZSCALE) : 1.0f;
        float ox = vte.vport_x_offset_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
        float oy = vte.vport_y_offset_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
        float oz = vte.vport_z_offset_ena ? RegF(XE_GPU_REG_PA_CL_VPORT_ZOFFSET) : 0.0f;
        if (modeCntl.vtx_window_offset_enable)
        {
            auto wo = Reg<reg::PA_SC_WINDOW_OFFSET>();
            ox += float(wo.window_x_offset);
            oy += float(wo.window_y_offset);
        }
        if (Reg<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero)
        {
            ox += 0.5f;
            oy += 0.5f;
        }
        // A counted draw stops at its viewport, as the console's does: the
        // host clips to the target, and a cube face's 256x256 viewport lies
        // in a 280-pitch target up to 896 rows tall, so a quad past the
        // face's edge would count samples the console never draws. A pixel
        // is in when its centre is. The edges are clamped to the scissor
        // while still floats (garbage in the viewport registers never
        // converts out of range; NaN lands on an edge). Wholly outside, the
        // draw is not drawn and begins no pass; else the pass is begun with
        // the unclamped area (pass decisions are those of any other draw).
        VkRect2D drawScissor = scissor;
        if (zpdCount)
        {
            if ((vte.value & 0xF) == 0xF && !clipCntl.clip_disable)  // (x and y scaled and offset)
            {
                const float fx = float(scaleX), fy = float(scaleY), ax = std::fabs(sx), ay = std::fabs(sy);
                const float vx0 = (ox - ax) * fx, vx1 = (ox + ax) * fx, vy0 = (oy - ay) * fy, vy1 = (oy + ay) * fy;
                const float lx = float(scissor.offset.x), hx = lx + float(scissor.extent.width);
                const float ly = float(scissor.offset.y), hy = ly + float(scissor.extent.height);
                auto edge = [](float v, float lo, float hi) { return int32_t(std::fmax(lo, std::fmin(std::ceil(v - 0.5f), hi))); };
                const int32_t cx0 = edge(vx0, lx, hx), cx1 = edge(vx1, lx, hx), cy0 = edge(vy0, ly, hy), cy1 = edge(vy1, ly, hy);
                if (cx1 <= cx0 || cy1 <= cy0)
                {
                    s_zpdCounts.clamped++;
                    ZpdLog("f%llu draw vs_%016llx ps_%016llx: nothing inside its viewport %.1f..%.1f x %.1f..%.1f (host px): not drawn",
                        (unsigned long long)s_frame, (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, vx0, vx1, vy0, vy1);
                    return;
                }
                drawScissor = { { cx0, cy0 }, { uint32_t(cx1 - cx0), uint32_t(cy1 - cy0) } };
            }
            else
            {
                s_zpdCounts.unclamped++;
                LogOnce((0x2Aull << 56) | 1, "occlusion query: a counted draw without its viewport transform or with clipping off "
                    "(VTE %03X, clip control %08X): not clamped to its viewport", vte.value & 0xFFF, clipCntl.value);
            }
        }
        if (s_passBarrier >= 2)
            VertexBarrierIfNeeded(layout.vsMasks, pc.indexAddress, pc.indexAddress ? count << (src.is32 ? 2 : 1) : 0);
        BeginPass(color, depth, scissor);
        if (s_profiling && s_passEndPending)  // (this pass's entry: none with the pool full)
        {
            PassInfo& pi = s_passInfos.back();
            if (pi.draws++ < 3)
                pi.desc += std::format(" [vs_{:016x} ps_{:016x} n{}]", s_vs->hash, s_ps->hash, vertexCount);
        }
        scissor = drawScissor;
        pc.posScale[0] = 2.0f * sx / w;
        pc.posScale[1] = 2.0f * sy / h;
        pc.posScale[2] = 2.0f * ox / w - 1.0f;
        pc.posScale[3] = 2.0f * oy / h - 1.0f;
        pc.flags = 1u | (vte.vtx_w0_fmt ? 2u : 0u) | (vte.vtx_xy_fmt ? 4u : 0u) | (vte.vtx_z_fmt ? 8u : 0u);
        // Ultrawide (game/ultrawide): the 16:9 guest frame is stretched to the
        // screen and the front-end (HUD and menu) matrix is narrowed by s to
        // match. Full-screen front-end layers (fades to black, the dims behind
        // dialogs and menus, the loading screen's backdrop and band) are
        // authored ~29% past the 16:9 edges; narrowed by s (0.744 at 3440x1440)
        // they stopped ~1.8% short of each screen edge and the world showed
        // through. A front-end draw whose every primitive spans the whole 16:9
        // width keeps its 16:9 extent (still past both screen edges).
        // Vert+ (a screen narrower than 16:9) is the same in y: the front-end
        // matrix's y scale is narrowed by sv (0.75 on a 4:3 iPad), so a layer
        // that ended at the 16:9 frame's top and bottom ends at the 16:9
        // band's. One that spans the whole width (as above) and reaches both
        // band edges is stretched in y to the screen's top and bottom; the
        // loading band, short of them, keeps its height. Any other layer that
        // covers the whole band (a menu backdrop picture, made of one quad or
        // several) is scaled up in x and y alike about the centre until it
        // reaches the screen's top and bottom, its sides cropped ("cover"):
        // stretched in y alone it would be distorted by up to 1/sv. The rest
        // of the front end is clipped to the band (below).
        // Edge bars (gpu/edge_bars.h): the copies of a police vid-cam side
        // bar that fill the screen past the 16:9 band, and of a black frame
        // (the intro's cuts, the Blacklist bio's panel) that fill everything
        // outside it, at 16:9 too, drawn right after it; the black around a
        // full-band backdrop (the results screen), cleared right after it,
        // at 16:9 too; and the Showcase's vignette, stretched to the screen's
        // edges. NFSMW_BAR_FILL=0 turns all the edge fills off (an A/B
        // switch).
        // planEdgeFills: the plan for a front-end draw (its registers, what
        // the rules below did, a reader for its ePolys): PlanFill's bars and
        // black frames first, then PlanBackdrop's full-band picture, then
        // PlanVignette's overlay (every screen shape; a bar fills nothing at
        // 16:9).
        // logEdgeFill: what a plan does, once per kind, screen shape and
        // outcome, in every log (a tester's too).
        static const bool barFill = [] { const char* v = std::getenv("NFSMW_BAR_FILL"); return !(v && v[0] == '0'); }();
        auto planEdgeFills = [&](const float* c, float feY, float aspectX, float aspectY, bool feShader, bool leftAlone) {
            xenos::xe_gpu_texture_fetch_t t0;
            memcpy(&t0, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(t0));
            const gpu::bars::Draw draw{ .prim = uint32_t(initiator.prim_type),
                .count = count,
                .c = c,
                .frontEndY = feY,
                .scaleX = aspectX,
                .scaleY = aspectY,
                .frontEndShader = feShader,
                .leftAlone = leftAlone,
                .occlusion = zpdCount,
                .vte = vte.value,
                .blend0 = s_regs[XE_GPU_REG_RB_BLENDCONTROL0] & 0x1FFF1FFF,
                .t0Sampled = (s_ps->info.texture2DMask & 1) != 0,
                .t0Width = uint32_t(t0.size_2d.width) + 1,
                .t0Height = uint32_t(t0.size_2d.height) + 1,
                .t0Format = uint32_t(t0.format),
                .posScale = { pc.posScale[0], pc.posScale[1], pc.posScale[2], pc.posScale[3] },
                .hostW = w * float(scaleX),
                .hostH = h * float(scaleY),
                .scissor = { scissor.offset.x, scissor.offset.y, scissor.offset.x + int32_t(scissor.extent.width),
                    scissor.offset.y + int32_t(scissor.extent.height) } };
            const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 95 * 2];
            const uint32_t base = vf[0] & 0x1FFFFFFC, endian = vf[1] & 3;
            const uint32_t words = (vf[1] >> 2) & 0xFFFFFF, first = s_regs[XE_GPU_REG_VGT_INDX_OFFSET];
            auto read = [&](uint32_t k, uint32_t j, uint32_t& word) {
                const uint32_t v = src.Get(k) + first;  // an ePoly: 8 words
                if ((v + 1) * 8 > words)
                    return false;
                word = GpuSwap(LoadPhysical(base + (v * 8 + j) * 4), endian);
                return true;
            };
            gpu::bars::Plan plan = gpu::bars::PlanFill(draw, read);
            // The results screen's backdrop (and any opaque picture of that
            // shape: the title's key art): what lies outside the picture,
            // past the band and in the band's overscan rows above and below
            // it, is cleared to black after it, where the world drawn behind
            // it showed (gpu/edge_bars.h).
            if (plan.fill == gpu::bars::Fill::None)
                plan = gpu::bars::PlanBackdrop(draw, read);
            // The Showcase's vignette (and any front-end overlay of its
            // shape, clear in the middle): stretched to the screen's edges,
            // where the garage showed undimmed round it (GitHub #19). Its
            // texels as the GPU reads them: 32-bit, tiled or linear rows.
            if (plan.fill == gpu::bars::Fill::None)
            {
                const uint32_t t0Base = (uint32_t(t0.base_address) << 12) & 0x1FFFFFFFu;
                const uint32_t t0Pitch = uint32_t(t0.pitch) << 5;
                auto texel = [&](uint32_t x, uint32_t y, uint32_t& word) {
                    if (!t0Pitch || x >= draw.t0Width || y >= draw.t0Height || x >= t0Pitch)
                        return false;
                    word = GpuSwap(LoadPhysical(t0Base + gpu::bars::TexelOffset32(x, y, t0Pitch, t0.tiled != 0)), uint32_t(t0.endianness));
                    return true;
                };
                plan = gpu::bars::PlanVignette(draw, read, texel);
            }
            // A flat full-screen layer short of the screen's edges (the fade
            // to black before a load): stretched to them (gpu/edge_bars.h).
            if (plan.fill == gpu::bars::Fill::None)
                plan = gpu::bars::PlanLayer(draw, read);
            return plan;
        };
        auto logEdgeFill = [&](const gpu::bars::Plan& barPlan, float aspectX, float aspectY) {
            if (barPlan.fill == gpu::bars::Fill::None)
                return;
            static const bool logFrontEnd = std::getenv("NFSMW_LOG_FRONTEND") != nullptr;
            // A match each frame skips building the line once its key is
            // logged.
            const uint32_t kind = uint32_t(barPlan.fill);
            const bool blackout = barPlan.fill == gpu::bars::Fill::Blackout;
            const bool backdrop = barPlan.fill == gpu::bars::Fill::Backdrop;
            const bool vignette = barPlan.fill == gpu::bars::Fill::Vignette;
            const bool layer = barPlan.fill == gpu::bars::Fill::Layer;
            const gpu::bars::Box& box = barPlan.box;
            const uint64_t key = (0x36ull << 56) | (uint64_t(kind) << 40) | (uint64_t(barPlan.rectCount) << 36) |
                (uint64_t(std::lround(aspectX * 4096.0f)) << 18) | uint64_t(std::lround(aspectY * 4096.0f));
            static uint64_t loggedKey[7] = { ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull };
            if (loggedKey[kind] != key)
            {
                loggedKey[kind] = key;
                char fills[192];
                if (barPlan.rectCount)
                    snprintf(fills, sizeof(fills), "fills %u rectangle(s) %s, first [%d,%d)x[%d,%d) (host px)", barPlan.rectCount,
                        backdrop ? "outside it (past the 16:9 band, and the band's overscan rows)"
                            : blackout ? "outside it (past the 16:9 band, and the band's border round it)" : "past the 16:9 band", barPlan.rects[0].x0,
                        barPlan.rects[0].x1, barPlan.rects[0].y0, barPlan.rects[0].y1);
                else
                    snprintf(fills, sizeof(fills), "%s", vignette || layer ? "stretched to the screen's edges"
                        : blackout || backdrop ? "fills nothing" : "reaches the screen edge: nothing to fill");
                LogOnce(key,
                    "edge bars: frame %llu vs_%016llx ps_%016llx: %s (clip x %.3f..%.3f y %.3f..%.3f, colour %08X; view scale x %.4f y %.4f) %s",
                    (unsigned long long)s_frame, (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash,
                    vignette ? "the Showcase's vignette (clear in the middle)" : layer ? "a full-screen layer short of the screen's edges (a fade)"
                        : backdrop ? "a full-band backdrop (black outside it)"
                        : blackout ? "a black frame" : kind == uint32_t(gpu::bars::Fill::RightBar) ? "the right vid-cam bar" : "the left vid-cam bar",
                    box.x0, box.x1, box.y0, box.y1, barPlan.argb, aspectX, aspectY, fills);
            }
            // NFSMW_LOG_FRONTEND=1: the first frame of each run of frames
            // with a match, bars and black frames apart (one run per vid-cam
            // scene and per cut; any other is a draw this should not take).
            static uint64_t lastBarFrame[5] = { ~0ull - 1, ~0ull - 1, ~0ull - 1, ~0ull - 1, ~0ull - 1 };
            uint64_t& last = lastBarFrame[layer ? 4 : vignette ? 3 : backdrop ? 2 : blackout ? 1 : 0];
            if (logFrontEnd && s_frame != last && s_frame != last + 1)
            {
                xenos::xe_gpu_texture_fetch_t t0;
                memcpy(&t0, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(t0));
                fprintf(stderr, "[frontend] edge bars: matching from frame %llu: %s x %.3f..%.3f y %.3f..%.3f colour %08X blend %08X t0 %08X %ux%u, %u rect(s)\n",
                    (unsigned long long)s_frame, layer ? "layer" : vignette ? "vignette" : backdrop ? "backdrop" : blackout ? "black frame"
                        : kind == uint32_t(gpu::bars::Fill::RightBar) ? "right bar" : "left bar",
                    box.x0, box.x1, box.y0, box.y1, barPlan.argb, s_regs[XE_GPU_REG_RB_BLENDCONTROL0] & 0x1FFF1FFF,
                    (uint32_t(t0.base_address) << 12) & 0x1FFFFFFFu, uint32_t(t0.size_2d.width) + 1, uint32_t(t0.size_2d.height) + 1,
                    barPlan.rectCount);
            }
            last = s_frame;
        };
        gpu::bars::Plan barPlan;
        if (const float s = game::AspectScaleX(), sv = game::AspectScaleY(); s < 1.0f || sv < 1.0f)
        {
            if (const float feX = game::FrontEndScaleX(); feX > 0.0f)
            {
                // A front-end draw: c0..c3 hold the front-end matrix (x scale
                // as the hook left it, no rotation, no projection), and its
                // vertices are ePolys (32 bytes: x, y, z, colour, uv0, uv1;
                // written at 0x82445DC8) at vertex fetch constant 95.
                float c[16];
                memcpy(c, &s_regs[XE_GPU_REG_SHADER_CONSTANT_000_X], sizeof(c));
                const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 95 * 2];
                if (std::fabs(c[0] - feX) <= feX * 1e-5f && c[1] == 0.0f && c[2] == 0.0f &&
                    c[12] == 0.0f && c[13] == 0.0f && c[14] == 0.0f && c[15] == 1.0f && ((vf[0] & 3) == 3))
                {
                    // NFSMW_LOG_FRONTEND=1: each distinct full-width layer,
                    // and in Vert+ each draw that covers the band, sits at
                    // one of its edges or reaches past it (below), once, with
                    // its extents in the frame's clip space before any
                    // widening (to check the thresholds); 4096 lines at most.
                    static const bool logFrontEnd = std::getenv("NFSMW_LOG_FRONTEND") != nullptr;
                    // At most 32 lines per shader pair (an animated draw makes
                    // a new line each frame), so a long session still logs
                    // the screen it was played to.
                    auto logOnce = [](uint64_t vs, uint64_t ps, const std::string& line) {
                        static std::unordered_set<std::string> logged;
                        static std::unordered_map<std::string, uint32_t> perPair;
                        if (logged.size() >= 4096 || !logged.insert(line).second)
                            return;
                        uint32_t n = ++perPair[std::format("{:016x}{:016x}", vs, ps)];
                        if (n <= 32)
                            fprintf(stderr, "[frontend] %s\n", line.c_str());
                        else if (n == 33)
                            fprintf(stderr, "[frontend] vs_%016llx ps_%016llx: 32 lines, no more\n", (unsigned long long)vs, (unsigned long long)ps);
                    };
                    // c4..c7 the y column: 0, the y scale as the hook left it
                    // (its sign unverified), 0, the y translation.
                    const float feY = game::FrontEndScaleY();
                    // Vert+ acts on every draw it matches (Hor+ only on
                    // full-width layers): the vertex shader must read the
                    // matrix (registers c0..c3: the 16 floats above) and the
                    // ePolys (fetch constant 95), not just inherit them from
                    // the HUD drawn before it (the mirror's picture, a movie
                    // quad, a post-process pass).
                    const auto& vi = s_vs->info;
                    const bool feShader = ((vi.vertexFetchConstants[95 / 32] >> (95 % 32)) & 1) &&
                        (vi.constRelative || (vi.constUsed[0] & 0xFull) == 0xFull);
                    const bool tall = feShader && sv < 1.0f && feY > 0.0f && c[4] == 0.0f && c[6] == 0.0f &&
                        std::fabs(std::fabs(c[5]) - feY) <= feY * 1e-5f;
                    const bool textured = (s_ps->info.texture2DMask | s_ps->info.texture3DMask | s_ps->info.textureCubeMask) != 0;
                    // Quad, triangle and rectangle lists are measured: up to
                    // 16 quads to act on (layers are a few), more for the log
                    // (which says so). Hor+ stops at the first primitive short
                    // of the 16:9 width; Vert+ needs them all (the band's
                    // cover is their union).
                    const uint32_t prim = uint32_t(initiator.prim_type);
                    const uint32_t per = prim == 13 ? 4 : (prim == 4 || prim == 8) ? 3 : 0;
                    const bool small = count <= 64;
                    const bool scan = per && count % per == 0 && (small || (logFrontEnd && count <= 4096));
                    const bool measureY = tall || logFrontEnd;
                    bool valid = scan, fullWidth = scan, fullHeight = scan && tall;
                    float reach = 1e30f, reachY = 1e30f;  // the nearest screen edge any primitive reaches
                    float allLo = 1e30f, allHi = -1e30f, allLoY = 1e30f, allHiY = -1e30f;
                    // The primitives' areas inside the band (x +-1, s being 1
                    // in Vert+; y +-sv), in clip units: a quad's or rectangle's
                    // box, half a triangle's (a quad's half). The first eight
                    // primitives' extents for the log.
                    float bandArea = 0.0f, minPrimArea = 1e30f;
                    // The band this draw shows on a 16:9 frame: clip y of
                    // its translation's share outside the narrowing, +-sv.
                    const float bandMid = c[7] * (1.0f - sv), bandLo = bandMid - sv, bandHi = bandMid + sv;
                    bool outside = false;  // a primitive wholly above or below the band
                    uint32_t prims = 0;
                    std::string primList;
                    // NFSMW_LOG_FRONTEND=1: up to four primitives that cross
                    // an edge of the 16:9 band (past it by more than half a
                    // percent of the band) and stop short of the screen's
                    // edge, the shape that leaves a strip of the world (the
                    // vid cam's mask did): logged below as "edge candidate"
                    // with what the rules here did to them. And ("inset
                    // candidate") one centred in the band, covering most of
                    // it in x and y and stopping short of its edges in one
                    // or both, the shape of the intro's black frame between
                    // two shots, which the world showed all round.
                    struct EdgeHit
                    {
                        uint32_t prim;
                        const char* edge;  // "+y", "-y", "+x" or "-x" (clip space), or "in" (inset)
                        float lo, hi, past, end;  // band units along that axis; the outer end in clip units
                                                  // (inset: band x, then its reach in clip x and y)
                        float acrossLo, acrossHi;  // band units along the other axis (inset: band y)
                        uint32_t colours[4];
                        uint32_t colourCount;
                    };
                    EdgeHit edgeHits[4];
                    uint32_t edgeHitCount = 0;
                    const float bandMidX = c[3] * (1.0f - s), bandLoX = bandMidX - s, bandHiX = bandMidX + s;
                    if (scan)
                    {
                        const uint32_t base = vf[0] & 0x1FFFFFFC, endian = vf[1] & 3;
                        const uint32_t words = (vf[1] >> 2) & 0xFFFFFF, first = s_regs[XE_GPU_REG_VGT_INDX_OFFSET];
                        for (uint32_t v0 = 0; v0 < count && valid && (fullWidth || measureY); v0 += per)
                        {
                            float lo = 1e30f, hi = -1e30f, loY = 1e30f, hiY = -1e30f;
                            for (uint32_t k = 0; k < per; k++)
                            {
                                uint32_t v = src.Get(v0 + k) + first;
                                if ((v + 1) * 8 > words)
                                {
                                    valid = false;
                                    break;
                                }
                                float p[3];
                                for (uint32_t j = 0; j < 3; j++)
                                {
                                    uint32_t b = GpuSwap(LoadPhysical(base + (v * 8 + j) * 4), endian);
                                    memcpy(&p[j], &b, 4);
                                }
                                float x = p[0] * c[0] + p[1] * c[1] + p[2] * c[2] + c[3];
                                lo = std::min(lo, x);
                                hi = std::max(hi, x);
                                if (measureY)
                                {
                                    float y = p[0] * c[4] + p[1] * c[5] + p[2] * c[6] + c[7];
                                    loY = std::min(loY, y);
                                    hiY = std::max(hiY, y);
                                }
                            }
                            if (!valid)
                                break;
                            // Only layers authored well past both 16:9 edges (the
                            // fades, dims and loading band at +-1.29): the title /
                            // Controls key art (+-1.105) keeps its shape.
                            fullWidth = fullWidth && lo <= -1.2f * s && hi >= 1.2f * s;
                            reach = std::min(reach, std::min(-lo, hi));
                            // Vert+: and down to both edges of the 16:9 band.
                            fullHeight = fullHeight && loY <= -0.99f * sv && hiY >= 0.99f * sv;
                            reachY = std::min(reachY, std::min(-loY, hiY));
                            allLo = std::min(allLo, lo);
                            allHi = std::max(allHi, hi);
                            allLoY = std::min(allLoY, loY);
                            allHiY = std::max(allHiY, hiY);
                            const float primArea = std::max(0.0f, std::min(hi, 1.0f) - std::max(lo, -1.0f)) *
                                std::max(0.0f, std::min(hiY, sv) - std::max(loY, -sv)) * (prim == 4 ? 0.5f : 1.0f);
                            bandArea += primArea;
                            minPrimArea = std::min(minPrimArea, primArea);
                            outside = outside || (measureY && (hiY < bandLo || loY > bandHi));
                            if (logFrontEnd && prims < 8)
                                primList += std::format(" [{:.3f}..{:.3f} {:.3f}..{:.3f}]", lo, hi, loY, hiY);
                            // (The front-end shader's draws only: one that
                            // inherits the constants reads no ePolys.)
                            if (logFrontEnd && feShader && edgeHitCount < 4)
                            {
                                auto hit = [&](const char* edge, float a, float b, float mid, float scale, float past, float end) {
                                    if (edgeHitCount >= 4)
                                        return;
                                    EdgeHit& e = edgeHits[edgeHitCount++];
                                    const bool y = edge[1] == 'y';
                                    const float c0 = y ? lo : loY, c1 = y ? hi : hiY, cMid = y ? bandMidX : bandMid, cScale = y ? s : sv;
                                    e = { prims, edge, (a - mid) / scale, (b - mid) / scale, past / scale, end, (c0 - cMid) / cScale,
                                        (c1 - cMid) / cScale, {}, 0 };
                                    for (uint32_t k = 0; k < per && k < 4; k++)
                                    {
                                        const uint32_t v = src.Get(v0 + k) + first;  // (in range: read above)
                                        e.colours[e.colourCount++] = GpuSwap(LoadPhysical(base + (v * 8 + 3) * 4), endian);
                                    }
                                };
                                const float my = 0.005f * sv, mx = 0.005f * s;
                                if (sv < 1.0f)
                                {
                                    if (hiY > bandHi + my && hiY < 1.0f && loY < bandHi)
                                        hit("+y", loY, hiY, bandMid, sv, hiY - bandHi, hiY);
                                    if (loY < bandLo - my && loY > -1.0f && hiY > bandLo)
                                        hit("-y", loY, hiY, bandMid, sv, bandLo - loY, loY);
                                }
                                if (s < 1.0f)
                                {
                                    if (hi > bandHiX + mx && hi < 1.0f && lo < bandHiX)
                                        hit("+x", lo, hi, bandMidX, s, hi - bandHiX, hi);
                                    if (lo < bandLoX - mx && lo > -1.0f && hi > bandLoX)
                                        hit("-x", lo, hi, bandMidX, s, bandLoX - lo, lo);
                                }
                                // Inset: centred within 2% of the band,
                                // reaching 80% of it or more each way, short
                                // of its edge (99%) in x or y.
                                const float xl = (lo - bandMidX) / s, xh = (hi - bandMidX) / s, yl = (loY - bandMid) / sv, yh = (hiY - bandMid) / sv;
                                const float bandReachX = std::min(-xl, xh), bandReachY = std::min(-yl, yh);
                                if (edgeHitCount < 4 && std::fabs(xl + xh) <= 0.04f && std::fabs(yl + yh) <= 0.04f && bandReachX >= 0.8f &&
                                    bandReachY >= 0.8f && std::min(bandReachX, bandReachY) < 0.99f)
                                {
                                    EdgeHit& e = edgeHits[edgeHitCount++];
                                    e = { prims, "in", xl, xh, std::min(-lo, hi), std::min(-loY, hiY), yl, yh, {}, 0 };
                                    for (uint32_t k = 0; k < per && k < 4; k++)
                                    {
                                        const uint32_t v = src.Get(v0 + k) + first;  // (in range: read above)
                                        e.colours[e.colourCount++] = GpuSwap(LoadPhysical(base + (v * 8 + 3) * 4), endian);
                                    }
                                }
                            }
                            prims++;
                        }
                        fullWidth = fullWidth && valid;
                        fullHeight = fullHeight && valid;
                    }
                    // Saved before any widening: the band below is this
                    // draw's own transform of clip y = +-sv.
                    const float ps1 = pc.posScale[1], ps3 = pc.posScale[3];
                    float kX = 1.0f, kY = 1.0f, kCover = 1.0f;
                    if (s < 1.0f && small && fullWidth && reach < 1.0f)
                    {
                        // Just past the screen edges, not the full 1/s: a
                        // textured layer (the band's stripes) stays ~5% off.
                        kX = std::min(1.0f / s, 1.01f / reach);
                        pc.posScale[0] *= kX;
                        pc.posScale[2] *= kX;
                    }
                    // A flat full-screen layer (a fade or dim) is stretched in
                    // y alone; a textured one (a backdrop picture) is not, it
                    // would be distorted: it is a cover below.
                    const bool extended = sv < 1.0f && small && fullWidth && fullHeight && !textured;
                    if (extended && reachY < 1.0f)
                    {
                        // posScale.y alone: NDC y = y * posScale.y +
                        // posScale.w * w scales about posScale.w, the
                        // viewport's centre, also with a tile's window offset
                        // (NFSMW_TILING=1).
                        kY = std::min(1.0f / sv, 1.01f / reachY);
                        pc.posScale[1] *= kY;
                    }
                    // Cover: the draw's primitives together reach both band
                    // edges in x and y (within 1%) and fill 95% of its area
                    // (corner pieces, a frame's four sides or a list's rows
                    // don't). Scaled by up to 1/sv (just past the screen's
                    // top and bottom, as the fades), the same in x so it keeps
                    // its shape; flat ones too (nothing to distort, and they
                    // must not be clipped: the strips would show undimmed).
                    const bool coversBand = allLo <= -0.99f && allHi >= 0.99f && allLoY <= -0.99f * sv && allHiY >= 0.99f * sv;
                    // And nothing else in the batch: the primitives' areas add
                    // up to the band's (no overlap, no extra sprite drawn with
                    // the same texture) and each is a sixteenth of it at
                    // least (a backdrop in up to 4x4 tiles), so a dialog's
                    // pieces batched with a dim are never zoomed.
                    const bool cover = tall && small && valid && !extended && coversBand && bandArea >= 0.95f * 4.0f * sv &&
                        bandArea <= 1.05f * 4.0f * sv && minPrimArea >= 4.0f * sv / 16.0f;
                    if (const float reachAllY = std::min(-allLoY, allHiY); cover && reachAllY < 1.0f)
                    {
                        // posScale.x and .y alone: about the viewport's centre
                        // (clip 0,0), as above.
                        kCover = std::min(1.0f / sv, 1.01f / reachAllY);
                        pc.posScale[0] *= kCover;
                        pc.posScale[1] *= kCover;
                    }
                    // Vert+: everything else is clipped to the 16:9 band, the
                    // rows a 16:9 frame shows. Menus park elements just past
                    // the frame's top and bottom (clip y beyond +-1, never on
                    // a 16:9 screen), and the narrowed matrix brings them into
                    // the strips above and below the band (the autosave
                    // dialog's white box and warning triangle, top right).
                    // The band is clip y = +-sv through this draw's viewport
                    // transform, in the target's host pixels like the
                    // scissor: posScale covers the render scale (the host
                    // viewport is the target's size x scale) and a tile's
                    // window offset (NFSMW_TILING=1: posScale.w moves with
                    // it). A row is in it when its centre is, as for a quad
                    // that ends there (the edges snapped to 1/256 pixel, as
                    // the rasteriser snaps vertices, so float noise doesn't
                    // decide a tie): at 1x, 720 rows and sv 0.75, rows
                    // 90..629.
                    // A measured draw whose every primitive meets the band
                    // isn't clipped: a bar or gradient authored past the
                    // frame's edge (as the fades) reaches into the strips as
                    // before. One with a primitive wholly outside (parked)
                    // is, and so is one too big to measure (text batches).
                    const bool meetsBand = small && scan && valid && !outside;
                    bool clipped = false;
                    if (tall && !extended && !cover && !meetsBand && vte.vport_y_scale_ena && vte.vport_y_offset_ena)
                    {
                        // (The rows the edge-bar fills leave, by the same rule.)
                        const gpu::bars::Rows band = gpu::bars::BandRows(bandLo, bandHi, ps1, ps3, h * float(scaleY));
                        const int32_t top = std::max(scissor.offset.y, band.top);
                        const int32_t bottom = std::min(scissor.offset.y + int32_t(scissor.extent.height), band.bottom);
                        scissor.offset.y = top;
                        scissor.extent.height = uint32_t(std::max(bottom - top, 0));
                        clipped = true;
                    }
                    // The police vid-cam's mask (career intro) ends with two
                    // solid black side bars that reach ~3% past the 16:9
                    // frame's top and bottom: under Vert+ the world shows in
                    // the rows above and below them (and the CRT bezel's
                    // quadrants, which stop at the same place), under Hor+
                    // past a 2.50:1 screen beside them. The intro's cut
                    // between two shots (~95 s) is a few frames of one black
                    // quad 0.970 x 0.939 of the 16:9 frame: the world shows
                    // all round it (and the Blacklist bio's panel, 0.976 x
                    // 0.985, for as long as it is up). Such a quad
                    // (gpu::bars::PlanFill: one black rectangle of the
                    // front-end shader the rules above left alone; a bar a
                    // little past both band edges in y, from inside the band
                    // to well past one side; a black frame centred, short of
                    // the band's edges) is drawn again after itself,
                    // stretched over the frame and scissored to (its half of)
                    // what lies outside the band (a bar: nothing inside the
                    // band changes) or to everything outside itself (a black
                    // frame). The Showcase's vignette
                    // (gpu::bars::PlanVignette) is drawn stretched to the
                    // screen's edges instead (its own posScale).
                    // NFSMW_BAR_FILL=0 turns it off (an A/B switch: barFill).
                    const uint32_t blend0 = s_regs[XE_GPU_REG_RB_BLENDCONTROL0] & 0x1FFF1FFF;
                    if (barFill)
                    {
                        barPlan = planEdgeFills(c, feY, s, sv, feShader, kX == 1.0f && kY == 1.0f && kCover == 1.0f && !clipped);
                        logEdgeFill(barPlan, s, sv);
                        // A layer short of the screen's edges, stretched to them.
                        if (barPlan.fill == gpu::bars::Fill::Layer)
                            memcpy(pc.posScale, barPlan.posScale, sizeof(pc.posScale));
                        if (barPlan.fill == gpu::bars::Fill::Vignette)
                            memcpy(pc.posScale, barPlan.posScale, sizeof(pc.posScale));
                    }
                    // The edge candidates (above) that still stop short of
                    // the screen's edge after the rules, each distinct line
                    // once, at most 4 per (t0, edge, kind) and 256 per run:
                    // a sliding or fading element makes a new line each
                    // frame, so the frame number is not part of the line.
                    // Their own cap, not the per-shader-pair one (every
                    // front-end draw shares one shader pair, and the title
                    // screen and menus use it up).
                    for (uint32_t i = 0; i < edgeHitCount; i++)
                    {
                        static std::unordered_set<std::string> logged;
                        static std::unordered_map<uint64_t, uint32_t> perKey;
                        static uint32_t lines = 0;
                        if (lines > 256)
                            break;
                        const EdgeHit& e = edgeHits[i];
                        const bool inset = e.edge[0] == 'i';
                        // One the rules above take past the screen's edge
                        // (widened, stretched or zoomed about the centre: the
                        // fades, dims and loading band) leaves no strip; an
                        // inset one, none when they take it past both.
                        if (inset ? e.past * kX * kCover >= 1.0f && e.end * kY * kCover >= 1.0f
                                  : std::fabs(e.end * (e.edge[1] == 'x' ? kX : kY) * kCover) >= 1.0f)
                            continue;
                        auto fetch = [](uint32_t n) {
                            xenos::xe_gpu_texture_fetch_t f;
                            memcpy(&f, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + n * 6], sizeof(f));
                            return f;
                        };
                        const xenos::xe_gpu_texture_fetch_t f0 = fetch(0), f1 = fetch(1);
                        const uint32_t t0 = (s_ps->info.texture2DMask & 1) ? (uint32_t(f0.base_address) << 12) & 0x1FFFFFFFu : 0;
                        const uint32_t t1 = (s_ps->info.texture2DMask & 2) ? (uint32_t(f1.base_address) << 12) & 0x1FFFFFFFu : 0;
                        bool same = true;
                        uint32_t minAlpha = 0xFF;
                        for (uint32_t k = 0; k < e.colourCount; k++)
                        {
                            same = same && e.colours[k] == e.colours[0];
                            minAlpha = std::min(minAlpha, e.colours[k] >> 24);
                        }
                        // At most 4 lines per (t0, edge, kind: opaque black,
                        // opaque, translucent): checked before the line is
                        // made, so a capped element costs nothing more. (By
                        // kind too: the game's white texture is under every
                        // flat panel, and a fading glow would use them up.)
                        const uint64_t kind = (minAlpha >= 0xF0 ? 1u : 0u) | (same && (e.colours[0] & 0xFFFFFFu) == 0 ? 2u : 0u);
                        const uint64_t edgeCode = inset ? 4u : (uint64_t(e.edge[0] == '+') << 1) | uint64_t(e.edge[1] == 'x');
                        uint32_t& perT0Edge = perKey[(uint64_t(t0) << 32) | (kind << 3) | edgeCode];
                        if (perT0Edge >= 4)
                            continue;
                        const std::string action = barPlan.fill == gpu::bars::Fill::Vignette ? std::string("vignette stretched to the screen's edges")
                            : barPlan.fill == gpu::bars::Fill::Layer ? std::string("layer stretched to the screen's edges")
                            : barPlan.rectCount
                            ? std::string(barPlan.fill == gpu::bars::Fill::Backdrop ? "backdrop fill"
                                    : barPlan.fill == gpu::bars::Fill::Blackout ? "black frame fill" : "bar fill")
                            : clipped ? std::format("clipped to rows {}..{}", scissor.offset.y, scissor.offset.y + int32_t(scissor.extent.height))
                            : kCover != 1.0f ? std::format("cover x{:.3f}", kCover)
                            : (kX != 1.0f || kY != 1.0f) ? std::format("widened x{:.3f} y{:.3f}", kX, kY)
                            : std::string("left alone");
                        const std::string textures = std::format("t0 {} t1 {}",
                            t0 ? std::format("{:08X} {}x{} f{}", t0, uint32_t(f0.size_2d.width) + 1, uint32_t(f0.size_2d.height) + 1, uint32_t(f0.format)) : std::string("-"),
                            t1 ? std::format("{:08X}", t1) : std::string("-"));
                        const std::string colour = same ? std::format("{:08X}", e.colours[0]) : std::string("varies");
                        const std::string line = inset
                            ? std::format("vs_{:016x} ps_{:016x} n{} prim {}/{} {}: band x {:.3f}..{:.3f} y {:.3f}..{:.3f}, short of the band's edges by "
                                "{:.3f} in x and {:.3f} in y, reaches clip x {:.3f} y {:.3f}; colour {} alpha {:02X} blend {:08X}: {}",
                                s_vs->hash, s_ps->hash, count, e.prim, prims, textures, e.lo, e.hi, e.acrossLo, e.acrossHi,
                                1.0f - std::min(-e.lo, e.hi), 1.0f - std::min(-e.acrossLo, e.acrossHi), e.past, e.end, colour, minAlpha, blend0, action)
                            : std::format("vs_{:016x} ps_{:016x} n{} prim {}/{} {} {}: band {:.3f}..{:.3f} ({} {:.3f}..{:.3f}), "
                                "{:.3f} past its edge, ends at clip {:.3f} (short of the screen edge); colour {} alpha {:02X} blend {:08X}: {}",
                                s_vs->hash, s_ps->hash, count, e.prim, prims, textures, e.edge, e.lo, e.hi, e.edge[1] == 'y' ? "x" : "y", e.acrossLo,
                                e.acrossHi, e.past, e.end, colour, minAlpha, blend0, action);
                        if (!logged.insert(line).second)
                            continue;
                        perT0Edge++;
                        if (++lines > 256)
                        {
                            fprintf(stderr, "[frontend] edge candidate: cap reached (256 lines), no more\n");
                            break;
                        }
                        fprintf(stderr, "[frontend] %s candidate: frame %llu %s\n", inset ? "inset" : "edge", (unsigned long long)s_frame, line.c_str());
                    }
                    if (logFrontEnd && fullWidth)
                    {
                        logOnce(s_vs->hash, s_ps->hash, std::format("vs_{:016x} ps_{:016x} n{} x {:.3f}..{:.3f} y {:.3f}..{:.3f} c5 {:.4f} (front end y {:.4f}{}{}) "
                            "view scale x {:.4f} y {:.4f}: widened x{:.3f} y{:.3f}{}",
                            s_vs->hash, s_ps->hash, count, allLo, allHi, allLoY, allHiY, c[5], feY, tall ? "" : ", not matched",
                            textured ? ", textured" : "", s, sv, kX, kY, small ? "" : " (n > 64: not acted on)"));
                    }
                    if (logFrontEnd && sv < 1.0f)
                    {
                        // The union's share of the band's area, and the
                        // primitives' summed (overlaps count twice). Logged:
                        // a draw that covers the band, one of a tenth of it
                        // at either edge (a backdrop in tiles drawn one by
                        // one, a header or button strip), and what is clipped
                        // because it reaches past the band.
                        const float boxShare = std::max(0.0f, std::min(allHi, 1.0f) - std::max(allLo, -1.0f)) *
                            std::max(0.0f, std::min(allHiY, sv) - std::max(allLoY, -sv)) / (4.0f * sv);
                        const bool atPlus = allHiY >= 0.99f * sv, atMinus = allLoY <= -0.99f * sv;
                        const bool pastBand = allLoY < -1.001f * sv || allHiY > 1.001f * sv;
                        const std::string action = cover ? std::format("cover x{:.3f}", kCover)
                            : extended ? std::format("widened y{:.3f}", kY)
                            : clipped ? std::format("clipped to rows {}..{}", scissor.offset.y, scissor.offset.y + int32_t(scissor.extent.height))
                            : tall ? "left alone (no viewport y)" : "not matched, left alone";
                        if (!scan || !valid)
                        {
                            if (tall)
                                logOnce(s_vs->hash, s_ps->hash, std::format("band vs_{:016x} ps_{:016x} prim {} n{} (translation c3 {:.3f} c7 {:.3f}): not measured{}: {}",
                                    s_vs->hash, s_ps->hash, prim, count, c[3], c[7], textured ? ", textured" : "", action));
                        }
                        else if (boxShare >= 0.95f || (boxShare >= 0.1f && (atPlus || atMinus)) || (tall && pastBand))
                        {
                            // A cover's 2D textures (address, size): a picture
                            // loaded with the menu, or a copy of the frame
                            // (a resolve: then the cover would show a zoomed
                            // copy of the picture under it).
                            std::string textures;
                            for (uint32_t n = 0; n < 32; n++)
                                if (cover && (s_ps->info.texture2DMask & (1u << n)))
                                {
                                    xenos::xe_gpu_texture_fetch_t f;
                                    memcpy(&f, &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + n * 6], sizeof(f));
                                    textures += std::format(" t{} {:08X} {}x{}", n, (uint32_t(f.base_address) << 12) & 0x1FFFFFFFu,
                                        uint32_t(f.size_2d.width) + 1, uint32_t(f.size_2d.height) + 1);
                                }
                            logOnce(s_vs->hash, s_ps->hash, std::format("band vs_{:016x} ps_{:016x} prim {} n{} ({} primitives) x {:.3f}..{:.3f} y {:.3f}..{:.3f} "
                                "(band y +-{:.3f}; translation c3 {:.3f} c7 {:.3f}): box {:.0f}%, primitives {:.0f}%{}{}{}{}: {}{}{}{}",
                                s_vs->hash, s_ps->hash, prim, count, prims, allLo, allHi, allLoY, allHiY, sv, c[3], c[7], boxShare * 100.0f,
                                bandArea / (4.0f * sv) * 100.0f, textured ? ", textured" : "", atPlus ? ", at +y edge" : "",
                                atMinus ? ", at -y edge" : "", pastBand ? ", past it" : "", action, small ? "" : " (n > 64: not acted on)",
                                textures, primList));
                        }
                    }
                    // Nothing of the band inside the game's own scissor.
                    if (clipped && scissor.extent.height == 0)
                        return;
                }
            }
        }
        else if (barFill)
        {
            // 16:9 (or a 16:9 picture letterboxed on another shape): none of
            // the rules above has anything to do, and no bar needs a fill,
            // but a black frame leaves a thin border of what is behind it all
            // round it: filled with its copies (gpu::bars::BlackoutRects); a
            // full-band backdrop (the results screen) leaves the race showing
            // in the band's overscan rows above and below it: those are
            // cleared to black (gpu::bars::OverscanRows); and the Showcase's
            // vignette leaves a thin border of the garage undimmed all round
            // it: it is stretched to the screen's edges
            // (gpu::bars::PlanVignette).
            // A front-end draw as above (the front-end matrix in c0..c3,
            // ePolys at fetch constant 95, read by the vertex shader); nothing
            // is measured, and only one quad gets as far as reading its
            // vertices: over a small t0 its first colour only (a black frame
            // needs black, gpu::bars::Candidate), over a large t0
            // (gpu::bars::BackdropCandidate, VignetteCandidate) all four.
            if (const float feX = game::FrontEndScaleX(); feX > 0.0f)
            {
                float c[16];
                memcpy(c, &s_regs[XE_GPU_REG_SHADER_CONSTANT_000_X], sizeof(c));
                const uint32_t* vf = &s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 95 * 2];
                if (std::fabs(c[0] - feX) <= feX * 1e-5f && c[1] == 0.0f && c[2] == 0.0f &&
                    c[12] == 0.0f && c[13] == 0.0f && c[14] == 0.0f && c[15] == 1.0f && ((vf[0] & 3) == 3))
                {
                    const auto& vi = s_vs->info;
                    const bool feShader = ((vi.vertexFetchConstants[95 / 32] >> (95 % 32)) & 1) &&
                        (vi.constRelative || (vi.constUsed[0] & 0xFull) == 0xFull);
                    barPlan = planEdgeFills(c, game::FrontEndScaleY(), s, sv, feShader, true);
                    logEdgeFill(barPlan, s, sv);
                    if (barPlan.fill == gpu::bars::Fill::Layer)
                        memcpy(pc.posScale, barPlan.posScale, sizeof(pc.posScale));
                    if (barPlan.fill == gpu::bars::Fill::Vignette)
                        memcpy(pc.posScale, barPlan.posScale, sizeof(pc.posScale));
                }
            }
        }
        // NFSMW_LOG_COVERS=1 (gpu/covers.h): each quad of a front-end draw
        // (any screen shape), as drawn after the rules above, against the
        // guest viewport the presenter shows: a dark box inside it, a dark
        // layer short of its edges, or a layer authored to cover the 16:9
        // frame that stops short of the picture's edges. Once per draw site
        // (the guest code that built the quad, game/cover_callers.h), kind,
        // extent (to 1% of the picture) and opacity; 512 lines at most.
        if (game::covers::Enabled())
            LogCovers(count, uint32_t(initiator.prim_type), src, pc.posScale, scissor, w * float(scaleX), h * float(scaleY), float(scaleX),
                float(scaleY), barPlan);
        // NFSMW_LOG_DRAWS=<seconds>: every draw of one frame (target, viewport,
        // scissor, textures, shaders), consecutive duplicates folded.
        if (s_logDrawsNow)
        {
            std::string line = std::format("{}x{} vp s({:.1f},{:.1f}) o({:.1f},{:.1f}) sc({},{})-({},{}) vte{:03X} vs_{:016x} ps_{:016x} tex",
                targetW, targetH, sx, sy, ox, oy, x0, y0, x1, y1, vte.value & 0xFFF, s_vs->hash, s_ps->hash);
            for (uint32_t n = 0; n < 32; n++)
                if ((s_ps->info.texture2DMask | s_ps->info.texture3DMask | s_ps->info.textureCubeMask) & (1u << n))
                    line += std::format(" {}:{:08X}", n, (s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + n * 6 + 1] & 0x1FFFF000u));
            if (barPlan.rectCount)
                line += std::format(" +edge {} fill x{}", barPlan.fill == gpu::bars::Fill::Backdrop ? "backdrop"
                    : barPlan.fill == gpu::bars::Fill::Blackout ? "black frame" : "bar", barPlan.rectCount);
            else if (barPlan.fill == gpu::bars::Fill::Vignette)
                line += " +vignette stretched";
            else if (barPlan.fill == gpu::bars::Fill::Layer)
                line += " +layer stretched";
            static std::string last;
            static uint32_t repeats = 0;
            if (line == last)
                repeats++;
            else
            {
                if (repeats)
                    fprintf(stderr, "[draws]   (x%u more)\n", repeats);
                fprintf(stderr, "[draws] %s\n", line.c_str());
                last = line;
                repeats = 0;
            }
        }
        // DIAG (v0.1.5 HFR scoping): NFSMW_HFR_DRAWMATCH=<from s>,<frames>: draw-list stability between consecutive
        // frames ([drawmatch] at each swap), for judging interpolation at the GPU command level.
        if (HfrDrawMatchActive())
        {
            auto mix = [](uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0x100000001B3ull; };
            uint64_t key = mix(mix(mix(s_vs->hash, s_ps->hash), uint64_t(initiator.prim_type) << 32 | count),
                uint64_t(s_regs[XE_GPU_REG_RB_SURFACE_INFO]) << 32 | s_regs[XE_GPU_REG_RB_COLOR_INFO]);
            key = mix(key, s_regs[XE_GPU_REG_RB_DEPTH_INFO]);
            for (uint32_t n = 0; n < 32; n++)
                if ((s_ps->info.texture2DMask | s_ps->info.texture3DMask | s_ps->info.textureCubeMask) & (1u << n))
                    key = mix(key, s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + n * 6 + 1] & 0x1FFFF000u);
            auto constHash = [&](const Shader* sh, uint32_t reg0) {
                uint64_t h = 0;
                for (uint32_t i = 0; i < 256; i++)
                    if (sh->info.constRelative || (sh->info.constUsed[i >> 6] >> (i & 63) & 1))
                        for (uint32_t k = 0; k < 4; k++)
                            h = mix(h, uint64_t(i) << 40 | uint64_t(k) << 32 | s_regs[reg0 + i * 4 + k]);
                return h;
            };
            uint64_t vb = 0;
            for (uint32_t word = 0; word < 3; word++)
                for (uint32_t bits = s_vs->info.vertexFetchConstants[word]; bits; bits &= bits - 1)
                    vb = mix(vb, s_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + (word * 32 + uint32_t(__builtin_ctz(bits))) * 2] & 0x1FFFFFFC);
            std::vector<HfrConst> vsConsts;
            for (uint32_t i = 0; i < 256; i++)
                if (!s_vs->info.constRelative && (s_vs->info.constUsed[i >> 6] >> (i & 63) & 1))
                {
                    HfrConst c{ uint16_t(i), {} };
                    for (uint32_t k = 0; k < 4; k++)
                        c.v[k] = s_regs[0x4000 + i * 4 + k];
                    vsConsts.push_back(c);
                }
            HfrDrawMatchRecord(key, constHash(s_vs, 0x4000), constHash(s_ps, 0x4400), vb, s_vs->info.constRelative, std::move(vsConsts));
        }
        pc.vsConstBase = 0;
        pc.psConstBase = 256;
        pc.indexOffset = s_regs[XE_GPU_REG_VGT_INDX_OFFSET];
        auto colorControl = Reg<reg::RB_COLORCONTROL>();
        pc.alphaTest = colorControl.alpha_test_enable ? uint32_t(colorControl.alpha_func) : 7u;
        pc.alphaRef = RegF(XE_GPU_REG_RB_ALPHA_REF);
        // Below 1x a host pixel spans 1/scale guest pixels, and the implicit
        // LOD the GPU takes from the host derivatives sits log2(1/scale)
        // above the 360's: blurrier textures, and the fonts' empty lower
        // mips fade the race HUD's text out (its smaller lines gone already
        // at 0.75x; with this bias all of it stays down to 0.5x, and the
        // Deck shots' PSNR against 1x rose 1.7-1.9 dB). The pixel shaders'
        // computed-LOD fetches add it back (TextureFetch in
        // gpu/shader_translator.cpp). Of two different axes the smaller
        // correction: no axis gets finer mips than at 1x. 0 at 1x and above,
        // where the supersampled LODs stay as they were. (Textures the scaled
        // resolves wrote are at host resolution already and don't need it;
        // they have one level, where it can only pick the magnification
        // filter for a fetch minified by less than 1/scale.)
        pc.lodBias = scale.Below() ? std::min(std::log2(std::max(scaleX, scaleY)), 0.0f) : 0.0f;

        float zmin = std::clamp(oz, 0.0f, 1.0f), zmax = std::clamp(oz + sz, 0.0f, 1.0f);
        VkViewport viewport{ 0.0f, 0.0f, w * float(scaleX), h * float(scaleY), zmin, zmax };

        // What this command buffer already has: commands that would set the
        // same value again are skipped (most draws change only a few). Set 0
        // stays bound and push constants stay valid across the texture
        // layouts (same set-0 layout and push-constant range); set 1 is
        // pushed again whenever the layout changes. A new command buffer
        // resets it (s_stateEpoch), and so does compute work without
        // CP_OPT_KEEP_BINDINGS (with it, only the push constants are pushed
        // whole again: s_pushEpoch).
        struct BoundState
        {
            uint64_t epoch = 0, pushEpoch = 0;
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkViewport viewport{};
            VkRect2D scissor{};
            uint32_t stencil[6] = {};
            float blend[4] = {}, bias[3] = {};
            bool unormBias = false;  // the bias set in unorm24's steps (FORCE_UNORM)
            uint32_t cullMode = 0, frontFace = 0;  // CP_OPT_DYNAMIC_STATE, with the depth and stencil tests
            gpu::draw_state::DepthStencil tests{};
            int32_t indexType = -1;  // CP_OPT_KEEP_BINDINGS: the ring's index type bound, or none
            uint32_t setOffset = ~0u;
            VkPipelineLayout textureLayout = VK_NULL_HANDLE;
            uint32_t textureCount = ~0u;
            uint64_t textureSerial = 0;  // TextureCache::infosSerial when `textures` were last its infos (0: never)
            VkDescriptorImageInfo textures[96];
            DrawConstants pc{};
        };
        static BoundState bound;
        // The first draw after a reset sets everything: dynamic state must be
        // set before a draw uses it, whatever the values.
        const bool fresh = !(g_cpOpt & CP_OPT_SKIP_REDUNDANT) || bound.epoch != s_stateEpoch;
        if (fresh)
        {
            bound = BoundState{};
            bound.epoch = s_stateEpoch;
        }
        if (fresh || bound.pipeline != pipeline)
        {
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            bound.pipeline = pipeline;
        }
        if (fresh || memcmp(&bound.viewport, &viewport, sizeof(viewport)) != 0)
        {
            vkCmdSetViewport(s_cmd, 0, 1, &viewport);
            bound.viewport = viewport;
        }
        // The scissor and the push constants are recorded through these, so
        // the bound state always follows what was recorded: the edge-bar
        // copies (below) change both, and a stale cache would skip the next
        // draw's own scissor and clip it to a fill rectangle.
        auto setScissor = [&](const VkRect2D& r) {
            vkCmdSetScissor(s_cmd, 0, 1, &r);
            bound.scissor = r;
        };
        // `whole` after a reset; else only when something changed, and with
        // CP_OPT_KEEP_BINDINGS only the 16-byte rows from the first changed
        // one to the last (nearly every race draw changes the vertex
        // snapshots' rows, the last four of seven, and little else). Every
        // layout has the same push-constant range, so the rows not pushed
        // keep their values (Vulkan's incremental push-constant updates).
        auto pushConstants = [&](const DrawConstants& c, bool whole) {
            static_assert(sizeof(DrawConstants) % 16 == 0);
            uint32_t lo = 0, hi = sizeof(c);
            if (!whole)
            {
                if (!gpu::draw_state::ChangedRows(&bound.pc, &c, sizeof(c), lo, hi))
                    return;
                if (!(g_cpOpt & CP_OPT_KEEP_BINDINGS))
                    lo = 0, hi = sizeof(c);
            }
            vkCmdPushConstants(s_cmd, layout.pipeline, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, lo, hi - lo,
                reinterpret_cast<const uint8_t*>(&c) + lo);
            bound.pc = c;
        };
        if (s_exp & 128)
            scissor = VkRect2D{ { 0, 0 }, { 1, 1 } };
        if (fresh || memcmp(&bound.scissor, &scissor, sizeof(scissor)) != 0)
            setScissor(scissor);
        auto front = Reg<reg::RB_STENCILREFMASK>();
        auto back = depthControl.backface_enable ? RegAt<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF) : front;
        uint32_t stencil[6] = { front.stencilmask, back.stencilmask, front.stencilwritemask, back.stencilwritemask,
            front.stencilref, back.stencilref };
        if (fresh || memcmp(bound.stencil, stencil, sizeof(stencil)) != 0)
        {
            vkCmdSetStencilCompareMask(s_cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilmask);
            vkCmdSetStencilCompareMask(s_cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilmask);
            vkCmdSetStencilWriteMask(s_cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilwritemask);
            vkCmdSetStencilWriteMask(s_cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilwritemask);
            vkCmdSetStencilReference(s_cmd, VK_STENCIL_FACE_FRONT_BIT, front.stencilref);
            vkCmdSetStencilReference(s_cmd, VK_STENCIL_FACE_BACK_BIT, back.stencilref);
            memcpy(bound.stencil, stencil, sizeof(stencil));
        }
        float blendConstants[4] = { RegF(XE_GPU_REG_RB_BLEND_RED), RegF(XE_GPU_REG_RB_BLEND_GREEN),
            RegF(XE_GPU_REG_RB_BLEND_BLUE), RegF(XE_GPU_REG_RB_BLEND_ALPHA) };
        if (fresh || memcmp(bound.blend, blendConstants, sizeof(blendConstants)) != 0)
        {
            vkCmdSetBlendConstants(s_cmd, blendConstants);
            memcpy(bound.blend, blendConstants, sizeof(blendConstants));
        }
        // The face's polygon offset (chosen with the pipeline above): the
        // constant in unorm24's steps (the register holds depth units).
        float bias[3] = { 0.0f, 0.0f, 0.0f };
        if (depthBias)
        {
            bias[0] = polyOffset.offset * gpu::kUnorm24Steps;
            // The slope factor is per host pixel: scaled with the resolution (Xenia).
            bias[2] = polyOffset.scale / 16.0f * std::max(scaleX, scaleY);
        }
        // The host depth is float32 for both guest formats, and Vulkan's
        // constant bias on a float depth buffer is in steps of 2^(e - 23),
        // e the exponent of the primitive's largest depth: 2^24 steps are
        // the register's depth only for depths 0.5..1, half of it below 0.5,
        // a quarter below 0.25. A D24S8 target (unorm24: one step 2^-24 over
        // the whole range, on the console too) has its bias given in
        // unorm24's steps where the device can (VK_EXT_depth_bias_control,
        // FORCE_UNORM: presenter), so a shadow caster near the light gets
        // all of it; a D24FS8 one keeps the float steps, already its own
        // (gpu/poly_offset.h).
#ifdef VK_EXT_depth_bias_control
        const bool unormBias = gpu::BiasInUnorm24Steps(depthBias, depth != nullptr,
            depth ? xenos::DepthRenderTargetFormat(depth->format) : xenos::DepthRenderTargetFormat::kD24S8, s_vk->cmdSetDepthBias2 != nullptr);
#else
        const bool unormBias = false;
#endif
        if (fresh || memcmp(bound.bias, bias, sizeof(bias)) != 0 || bound.unormBias != unormBias)
        {
#ifdef VK_EXT_depth_bias_control
            if (unormBias)
            {
                VkDepthBiasRepresentationInfoEXT representation{ VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT };
                representation.depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
                representation.depthBiasExact = s_vk->depthBiasExact ? VK_TRUE : VK_FALSE;
                VkDepthBiasInfoEXT info{ VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT, &representation };
                info.depthBiasConstantFactor = bias[0];
                info.depthBiasClamp = bias[1];
                info.depthBiasSlopeFactor = bias[2];
                s_vk->cmdSetDepthBias2(s_cmd, &info);
            }
            else
#endif
                // (The format's steps: as vkCmdSetDepthBias2EXT with no
                // representation, so it also undoes FORCE_UNORM.)
                vkCmdSetDepthBias(s_cmd, bias[0], bias[1], bias[2]);
            memcpy(bound.bias, bias, sizeof(bias));
            bound.unormBias = unormBias;
        }
        // NFSMW_PREPASS (see s_prepassOn): this draw in the pre-pass too?
        VkPipeline prePipeline = VK_NULL_HANDLE;
        bool prepassDiscards = false;  // with its pixel shader (NFSMW_PREPASS=2)
        if (s_passSecondary && !(s_exp & 1))
        {
            s_prepassCounts.draws++;
            const gpu::draw_state::DepthStencil t = gpu::draw_state::FromDepthControl(depthState);
            bool blends = false;
            for (int i = 0; i < 4; i++)
                blends = blends || (color[i] && ((writeMask >> (4 * i)) & 0xF) && key.blend[i] != ~0u);
            const bool discards = (rasterState & 32u) || (s_ps && s_ps->info.usesKill);
            if (depth && !adoptedDepth && t.depthTest && t.depthWrite && t.depthCompareOp == VK_COMPARE_OP_LESS_OR_EQUAL && !t.stencilTest &&
                !blends && (!discards || s_prepassLevel >= 2) && s_ps && !s_ps->info.writesDepth && !barPlan.rectCount && !zpdCount)
            {
                PipelineKey pk = key;
                for (uint32_t& b : pk.blend)
                    b = ~0u;
                pk.colorMask = 0;
                if (discards)
                {
                    pk.raster = key.raster | 128u | 256u;  // (the alpha test bit kept)
                    prepassDiscards = true;
                }
                else
                {
                    pk.ps = DepthOnlyPixelShader()->hash;
                    pk.raster = (key.raster & ~32u) | 128u;
                }
                prePipeline = PrepassPipeline(pk, layout.pipeline);
                if (prePipeline)
                    s_prepassCounts.prepassed++;
                else
                    s_prepassCounts.waiting++;
            }
        }
#ifdef VK_EXT_extended_dynamic_state
        // CP_OPT_DYNAMIC_STATE: what the pipeline key left out (above), as
        // BuildPipeline would bake it (gpu/draw_state.h; no depth target, or
        // one attached for the pass only: no depth or stencil test or
        // write). Each command only when its value changed (every pipeline
        // has these dynamic, so a pipeline bind leaves them as set).
        if (s_dynamicState)
        {
            const uint32_t cullMode = rasterState & 3u, frontFace = (rasterState >> 2) & 1u;
            gpu::draw_state::DepthStencil tests = gpu::draw_state::FromDepthControl(depthState);
            if (prePipeline)
            {
                // (The pre-pass wrote its depth: what passes is what ends up there.)
                tests.depthCompareOp = VK_COMPARE_OP_EQUAL;
                tests.depthWrite = 0;
            }
            if (fresh || bound.cullMode != cullMode)
                s_vk->cmdSetCullMode(s_cmd, VkCullModeFlags(cullMode));
            if (fresh || bound.frontFace != frontFace)
                s_vk->cmdSetFrontFace(s_cmd, frontFace ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
            if (fresh || bound.tests.depthTest != tests.depthTest)
                s_vk->cmdSetDepthTestEnable(s_cmd, tests.depthTest);
            if (fresh || bound.tests.depthWrite != tests.depthWrite)
                s_vk->cmdSetDepthWriteEnable(s_cmd, tests.depthWrite);
            if (fresh || bound.tests.depthCompareOp != tests.depthCompareOp)
                s_vk->cmdSetDepthCompareOp(s_cmd, VkCompareOp(tests.depthCompareOp));
            if (fresh || bound.tests.stencilTest != tests.stencilTest)
                s_vk->cmdSetStencilTestEnable(s_cmd, tests.stencilTest);
            if (fresh || !(bound.tests.front == tests.front) || !(bound.tests.back == tests.back))
            {
                auto setOps = [&](VkStencilFaceFlags faces, const gpu::draw_state::StencilFace& f) {
                    s_vk->cmdSetStencilOp(s_cmd, faces, VkStencilOp(f.failOp), VkStencilOp(f.passOp), VkStencilOp(f.depthFailOp),
                        VkCompareOp(f.compareOp));
                };
                if (tests.front == tests.back)
                    setOps(VK_STENCIL_FACE_FRONT_AND_BACK, tests.front);
                else
                {
                    setOps(VK_STENCIL_FACE_FRONT_BIT, tests.front);
                    setOps(VK_STENCIL_FACE_BACK_BIT, tests.back);
                }
            }
            bound.cullMode = cullMode;
            bound.frontFace = frontFace;
            bound.tests = tests;
        }
#endif

        if (s_profiling && s_passStats && s_passActive && !s_passInfos.empty())
        {
            const gpu::draw_state::DepthStencil t = gpu::draw_state::FromDepthControl(depthState);
            bool blends = false;
            for (int i = 0; i < 4; i++)
                blends = blends || (color[i] && ((writeMask >> (4 * i)) & 0xF) && key.blend[i] != ~0u);
            PrepassReason why = !depth || adoptedDepth ? kPrepassNoDepth
                : !t.depthTest || !t.depthWrite ? kPrepassNoWrite
                : t.depthCompareOp != VK_COMPARE_OP_LESS_OR_EQUAL ? kPrepassFunc
                : t.stencilTest ? kPrepassStencil
                : blends ? kPrepassBlend
                : (rasterState & 32u) || (s_ps && (s_ps->info.usesKill || s_ps->info.writesDepth)) ? kPrepassDiscard
                : barPlan.rectCount || zpdCount ? kPrepassOther : kPrepassOk;
            s_passInfos.back().prepass[why]++;
        }
        uint32_t dynamicOffset = uint32_t(constantsOffset);
        if (fresh || bound.setOffset != dynamicOffset)
        {
            vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline, 0, 1, &s_set0, 1, &dynamicOffset);
            bound.setOffset = dynamicOffset;
        }

        // The textures (set 1), pushed when they differ from the bound ones.
        // The bound set notes the infos' serial it was last pushed from or
        // found equal to: while the infos keep it (no lookup rewrote them:
        // the memo served the draws since), they are the bound ones still,
        // and aren't compared again. The writes are made only for a push.
        if (textureCount)
        {
            bool same = !fresh && bound.textureLayout == layout.pipeline && bound.textureCount == textureCount;
            if (same && bound.textureSerial != textureCache.infosSerial)
                for (uint32_t i = 0; same && i < textureCount; i++)  // (not memcmp: the struct has padding)
                    same = bound.textures[i].sampler == textureInfos[i].sampler && bound.textures[i].imageView == textureInfos[i].imageView &&
                        bound.textures[i].imageLayout == textureInfos[i].imageLayout;
            if (!same)
            {
                VkWriteDescriptorSet writes[96];
                for (uint32_t i = 0; i < textureCount; i++)
                {
                    VkWriteDescriptorSet& wds = writes[i];
                    wds = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    wds.dstBinding = layout.bindings[i];
                    wds.descriptorCount = 1;
                    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    wds.pImageInfo = &textureInfos[i];
                }
                s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline, 1, textureCount, writes);
                bound.textureLayout = layout.pipeline;
                bound.textureCount = textureCount;
                std::copy(textureInfos, textureInfos + textureCount, bound.textures);
            }
            bound.textureSerial = textureCache.infosSerial;
        }
        pushConstants(pc, fresh || bound.pushEpoch != s_pushEpoch);
        bound.pushEpoch = s_pushEpoch;

        // Guest memory this draw reads when the GPU runs it: vertex data, and
        // indices when the shader fetches them itself.


        if (g_eagerWrites && (++s_drawsSinceCheck & 127) == 0)
        {
            // Every 128 draws: submit when another thread waits on recorded
            // work (write-watch guard), or when the GPU is starving and the
            // recording is at least NFSMW_STARVE_SUBMIT_US old (default
            // 1000). Each submit costs the CP ~20 us on RADV plus a pass
            // restart; every 128 draws was ~25 submits a frame in races.
            static const int64_t starveUs = [] { const char* v = std::getenv("NFSMW_STARVE_SUBMIT_US"); return v ? int64_t(std::atoll(v)) : 1000; }();
            bool submit = writewatch::g_submitRequested.exchange(false);
            if (!submit && std::chrono::steady_clock::now() - s_recordStart >= std::chrono::microseconds(starveUs))
            {
                std::lock_guard lock(s_submitMutex);
                submit = s_inFlight.size() < 2;
            }
            if (submit)
            {
                // Record this draw first: submit right after it.
                s_submitAfterDraw = true;
            }
        }

        if (s_exp & 1)
            return;
        bool skipPassDraw = false;  // NFSMW_PREPASS: a depth-only draw the pre-pass took whole
        if (prePipeline)
        {
            // NFSMW_PREPASS: the same draw into the pre-pass buffer, its own
            // bound state (the pass buffer's above), depth LESS_OR_EQUAL with
            // writes, no stencil test.
            struct PreBound
            {
                uint64_t epoch = 0;
                VkPipeline pipeline = VK_NULL_HANDLE;
                VkViewport viewport{};
                VkRect2D scissor{};
                float bias[3] = {};
                bool unormBias = false;
                uint32_t cullMode = ~0u, frontFace = ~0u;
                uint32_t setOffset = ~0u;
                int32_t indexType = -1;
                VkPipelineLayout textureLayout = VK_NULL_HANDLE;
                uint64_t textureSerial = 0;
                DrawConstants pc{};
            };
            static PreBound pre;
            const VkCommandBuffer cmd = s_preCmd;
            const bool first = pre.epoch != s_preEpoch;
            if (first)
            {
                pre = PreBound{};
                pre.epoch = s_preEpoch;
                s_vk->cmdSetDepthTestEnable(cmd, VK_TRUE);
                s_vk->cmdSetDepthWriteEnable(cmd, VK_TRUE);
                s_vk->cmdSetDepthCompareOp(cmd, VK_COMPARE_OP_LESS_OR_EQUAL);
                s_vk->cmdSetStencilTestEnable(cmd, VK_FALSE);
                const float none[4] = {};
                vkCmdSetBlendConstants(cmd, none);  // (dynamic in every pipeline: set before a draw)
                vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
                vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
                vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
                s_vk->cmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                    VK_COMPARE_OP_ALWAYS);
            }
            if (first || pre.pipeline != prePipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prePipeline);
                pre.pipeline = prePipeline;
            }
            if (first || memcmp(&pre.viewport, &viewport, sizeof(viewport)) != 0)
            {
                vkCmdSetViewport(cmd, 0, 1, &viewport);
                pre.viewport = viewport;
            }
            if (first || memcmp(&pre.scissor, &scissor, sizeof(scissor)) != 0)
            {
                vkCmdSetScissor(cmd, 0, 1, &scissor);
                pre.scissor = scissor;
            }
            if (first || memcmp(pre.bias, bias, sizeof(bias)) != 0 || pre.unormBias != unormBias)
            {
#ifdef VK_EXT_depth_bias_control
                if (unormBias)
                {
                    VkDepthBiasRepresentationInfoEXT representation{ VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT };
                    representation.depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT;
                    representation.depthBiasExact = s_vk->depthBiasExact ? VK_TRUE : VK_FALSE;
                    VkDepthBiasInfoEXT info{ VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT, &representation };
                    info.depthBiasConstantFactor = bias[0];
                    info.depthBiasClamp = bias[1];
                    info.depthBiasSlopeFactor = bias[2];
                    s_vk->cmdSetDepthBias2(cmd, &info);
                }
                else
#endif
                    vkCmdSetDepthBias(cmd, bias[0], bias[1], bias[2]);
                memcpy(pre.bias, bias, sizeof(bias));
                pre.unormBias = unormBias;
            }
            const uint32_t cullMode = rasterState & 3u, frontFace = (rasterState >> 2) & 1u;
            if (first || pre.cullMode != cullMode)
                s_vk->cmdSetCullMode(cmd, VkCullModeFlags(cullMode));
            if (first || pre.frontFace != frontFace)
                s_vk->cmdSetFrontFace(cmd, frontFace ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
            pre.cullMode = cullMode;
            pre.frontFace = frontFace;
            if (first || pre.setOffset != dynamicOffset)
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline, 0, 1, &s_set0, 1, &dynamicOffset);
                pre.setOffset = dynamicOffset;
            }
            // The textures only where the vertex shader fetches some.
            if (textureCount && (prepassDiscards || (s_vs && (s_vs->info.texture2DMask | s_vs->info.texture3DMask | s_vs->info.textureCubeMask))) &&
                (first || pre.textureLayout != layout.pipeline || pre.textureSerial != textureCache.infosSerial))
            {
                VkWriteDescriptorSet writes[96];
                for (uint32_t i = 0; i < textureCount; i++)
                {
                    writes[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    writes[i].dstBinding = layout.bindings[i];
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    writes[i].pImageInfo = &textureInfos[i];
                }
                s_pushDescriptorSet(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout.pipeline, 1, textureCount, writes);
                pre.textureLayout = layout.pipeline;
                pre.textureSerial = textureCache.infosSerial;
            }
            {
                uint32_t lo = 0, hi = sizeof(pc);
                if (first || gpu::draw_state::ChangedRows(&pre.pc, &pc, sizeof(pc), lo, hi))
                {
                    if (first)
                        lo = 0, hi = sizeof(pc);
                    vkCmdPushConstants(cmd, layout.pipeline, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, lo, hi - lo,
                        reinterpret_cast<const uint8_t*>(&pc) + lo);
                    pre.pc = pc;
                }
            }
            if (useIndices)
            {
                const VkIndexType indexType = indices16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
                if (first || pre.indexType != int32_t(indexType))
                {
                    vkCmdBindIndexBuffer(cmd, s_ring, 0, indexType);
                    pre.indexType = int32_t(indexType);
                }
                vkCmdDrawIndexed(cmd, vertexCount, 1, uint32_t(indexOffset >> (indices16 ? 1 : 2)), 0, 0);
            }
            else
                vkCmdDraw(cmd, vertexCount, 1, 0, 0);
            // A depth-only draw: the pre-pass did all it does (no colour, no
            // stencil, no occlusion count); the pass needn't draw it again.
            if (!writeMask)
            {
                s_prepassCounts.depthOnly++;
                skipPassDraw = true;
            }
        }
        if (zpdCount)
        {
            // Weighted by the guest's samples per pixel (its MSAA; the host
            // renders 1) over host pixels per guest pixel (the render scale).
            const uint64_t segments = s_zpdSegNext;
            // (At 1.5x a host pixel is 4/9 of a guest one: msaa x 4 / 9.)
            ZpdBeginSegment((1u << uint32_t(surface.msaa_samples)) * scale.PixelsDen(), scale.PixelsNum());
            s_zpdCounts.draws++;
            if (ZpdLogging())
            {
                std::string targets = depth ? std::format("D[b{} p{}]", depth->base, depth->pitch) : "no depth";
                for (int i = 0; i < 4; i++)
                    if (color[i])
                        targets += std::format(" C{}[b{} p{}]{}", i, color[i]->base, color[i]->pitch, (adoptedColor >> i) & 1 ? " (pass's)" : "");
                ZpdLog("f%llu draw vs_%016llx ps_%016llx n%u: %s, x %u/%u, %s, scissor %ux%u@%d,%d (host px)",
                    (unsigned long long)s_frame, (unsigned long long)s_vs->hash, (unsigned long long)s_ps->hash, vertexCount,
                    s_zpdQuery < 0 ? "not counted (this command buffer's queries are used up)"
                        : std::format("{} {}", s_zpdSegNext != segments ? "new segment" : "segment", s_zpdSegNext - 1).c_str(),
                    (1u << uint32_t(surface.msaa_samples)) * scale.PixelsDen(), scale.PixelsNum(), targets.c_str(), scissor.extent.width,
                    scissor.extent.height, scissor.offset.x, scissor.offset.y);
            }
        }
        // CP_OPT_KEEP_BINDINGS: the ring stays bound as the index buffer
        // (again only when the index type changes) and firstIndex finds the
        // draw's indices in it: their offset is 256-byte aligned, so a whole
        // number of indices, and the ring (192 MB) keeps it in 32 bits.
        // Before, every indexed draw (all of a race's) bound it at its own
        // offset.
        uint32_t firstIndex = 0;
        if (useIndices)
        {
            const VkIndexType indexType = indices16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            if (!(g_cpOpt & CP_OPT_KEEP_BINDINGS))
                vkCmdBindIndexBuffer(s_cmd, s_ring, indexOffset, indexType);
            else
            {
                static_assert(kRingSize / 2 <= UINT32_MAX);
                if (fresh || bound.indexType != int32_t(indexType))
                {
                    vkCmdBindIndexBuffer(s_cmd, s_ring, 0, indexType);
                    bound.indexType = int32_t(indexType);
                }
                firstIndex = uint32_t(indexOffset >> (indices16 ? 1 : 2));
            }
            if (s_exp & 32)
                vertexCount = 0;
            if (!skipPassDraw)
                vkCmdDrawIndexed(s_cmd, vertexCount, 1, firstIndex, 0, 0);
        }
        else
        {
            if (s_exp & 32)
                vertexCount = 0;
            if (!skipPassDraw)
                vkCmdDraw(s_cmd, vertexCount, 1, 0, 0);
        }
        // Edge bars (above): the same draw again per rectangle, with the
        // stretched posScale and the rectangle as its scissor (inside this
        // draw's own, so inside the pass); through setScissor and
        // pushConstants, so the bound state follows.
        if (barPlan.rectCount && barPlan.fill == gpu::bars::Fill::Backdrop)
        {
            // A backdrop: its rectangles (inside this draw's scissor, so
            // inside the pass) cleared to opaque black in the colour targets
            // it writes; nothing else changes.
            VkClearAttachment ca[4]{};
            uint32_t attachments = 0;
            for (uint32_t i = 0; i < 4; i++)
                if (color[i] && ((writeMask >> (4 * i)) & 0xF))
                {
                    ca[attachments].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    ca[attachments].colorAttachment = i;
                    ca[attachments].clearValue.color = { { 0.0f, 0.0f, 0.0f, 1.0f } };
                    attachments++;
                }
            VkClearRect cr[std::extent_v<decltype(gpu::bars::Plan::rects)>]{};
            for (uint32_t i = 0; i < barPlan.rectCount; i++)
            {
                const gpu::bars::Rect& r = barPlan.rects[i];
                cr[i] = { { { r.x0, r.y0 }, { uint32_t(r.x1 - r.x0), uint32_t(r.y1 - r.y0) } }, 0, 1 };
            }
            if (attachments)
                vkCmdClearAttachments(s_cmd, attachments, ca, barPlan.rectCount, cr);
        }
        else if (barPlan.rectCount)
        {
            DrawConstants fill = pc;
            memcpy(fill.posScale, barPlan.posScale, sizeof(barPlan.posScale));
            pushConstants(fill, false);
            for (uint32_t i = 0; i < barPlan.rectCount; i++)
            {
                const gpu::bars::Rect& r = barPlan.rects[i];
                setScissor(VkRect2D{ { r.x0, r.y0 }, { uint32_t(r.x1 - r.x0), uint32_t(r.y1 - r.y0) } });
                if (useIndices)
                    vkCmdDrawIndexed(s_cmd, vertexCount, 1, firstIndex, 0, 0);
                else
                    vkCmdDraw(s_cmd, vertexCount, 1, 0, 0);
            }
        }
        if (s_submitAfterDraw)
        {
            s_submitAfterDraw = false;
            WriteDeferredForCpu();
            SubmitRecorded(8);
        }
    }

    void Submit()
    {
        if (!s_vk)
            return;
        WriteDeferredForCpu();  // (a CPU access may be what asks for this submit)
        SubmitRecorded(2);
    }

    void SubmitIfIdle()
    {
        if (!s_vk || !s_recording)
            return;
        size_t inFlight;
        {
            std::lock_guard lock(s_submitMutex);
            inFlight = s_inFlight.size();
        }
        if (inFlight < (g_eagerWrites ? 2u : 1u))
            SubmitRecorded(3);
    }

    namespace
    {
        const int64_t s_submitAgeUs = [] { const char* v = std::getenv("NFSMW_SUBMIT_AGE_US"); return v ? int64_t(std::atoll(v)) : 2000; }();
    }

    void SubmitIfDue()
    {
        // With fences published mid-buffer (events), batching more work per
        // command buffer costs the game nothing while the GPU is busy anyway;
        // each Metal command buffer has a large fixed cost. Submit when the
        // queue is shallow, or when the recording has aged.
        // Eager writes: nothing waits on the recording, so keep the GPU fed.
        static const uint32_t depth = [] { const char* v = std::getenv("NFSMW_SUBMIT_DEPTH"); return v ? uint32_t(std::atoi(v)) : g_eagerWrites ? 2u : 1u; }();
        const int64_t ageUs = s_submitAgeUs;
        if (!s_vk || !s_recording)
            return;
        if (!s_fenceEvents)
        {
            SubmitRecorded(4);
            return;
        }
        size_t inFlight;
        {
            std::lock_guard lock(s_submitMutex);
            inFlight = s_inFlight.size();
        }
        if (inFlight < depth || std::chrono::steady_clock::now() - s_recordStart > std::chrono::microseconds(ageUs))
            SubmitRecorded(4);
    }

    std::chrono::steady_clock::time_point SubmitDueAt()
    {
        // (Without fence events SubmitIfDue submits at once: nothing waits.)
        if (!s_vk || !s_recording || !s_fenceEvents)
            return std::chrono::steady_clock::time_point::max();
        return s_recordStart + std::chrono::microseconds(s_submitAgeUs + 1);
    }

    void Flush()
    {
        if (!s_vk)
            return;
        SubmitRecorded(5);
        CpWaitTimer wait(CP_WAIT_FLUSH);
        std::unique_lock lock(s_submitMutex);
        s_submitCv.wait(lock, [] { return s_inFlight.empty(); });
    }

    void QueueWrite(std::function<void()> write)
    {
        if (!s_vk || g_eagerWrites)
        {
            write();
            return;
        }
        // Attach to the newest submission: the one being recorded (it then
        // becomes visible when that completes, a little later than strictly
        // needed, which is always safe for fences), else the last in flight.
        std::unique_lock lock(s_submitMutex);
        if (s_recording)
        {
            s_submissions[s_slot].writes.push_back({ std::move(write) });
            bool inPass = s_passActive;
            // With the GPU idle nothing would carry this write until the next
            // submit: send the work now so whoever waits on it isn't stalled.
            bool idle = s_inFlight.empty();
            lock.unlock();
            if (idle)
                SubmitRecorded(6);
            else if (!inPass)
                SignalWrites();  // in a pass, the pass end signals it
            return;
        }
        if (s_inFlight.empty())
        {
            lock.unlock();
            write();
            return;
        }
        s_submissions[s_inFlight.back()].writes.push_back({ std::move(write) });
    }

    // Occlusion queries, counted (NFSMW_OCCLUSION=1). The game brackets its
    // sun quads with them and turns the share of samples that passed into
    // the glare's strength (gpu/zpd_report.h has the reports). A bracket
    // runs from its BEGIN ZPD to its END ZPD; the draws in it are counted
    // in segments, each a precise occlusion query begun right before a
    // draw inside its pass (no pass is split for a query) and ended at the
    // pass's end, at the next ZPD or when the weight changes, so a segment
    // belongs to every bracket open while it ran (they may overlap; Vulkan
    // allows one active query). The END's report is stored when the newest
    // submission completes (every segment's count is in by then: the
    // completion thread harvests in submission order, never waiting), in
    // stream order, so a predicated-tiling query's last tile, which makes
    // it ready, lands last. Tile 0 holds the whole count (untiled
    // rendering: later tiles' draws are skipped, their brackets count 0),
    // as GetData sums the tiles. A result lands a frame or two after its
    // draw; the game polls without waiting and keeps its value meanwhile,
    // as on the console. Nothing here waits.
    bool ZpassDone(uint32_t address)
    {
        if (!s_zpdOn)
            return false;
        address &= 0x1FFFFFFF;
        if (address > kSharedSize - 32)
            return false;
        // Every ZPD ends the segment (inside the open pass: legal), so no
        // segment spans a bracket's edge.
        ZpdEndSegment();
        s_zpdCounts.zpds++;
        switch (zpd::Classify(address))
        {
        case zpd::Kind::Begin:
        {
            zpd::Bracket old;
            auto opened = s_zpdOpen.Open(address, s_zpdSegNext, s_frame, &old);
            s_zpdCounts.begins++;
            if (opened == zpd::BracketBook::Opened::Replaced)
            {
                s_zpdCounts.replaced++;
                LogOnce((0x2Cull << 56) | address, "occlusion query: BEGIN %08X again before its END (open since frame %llu): replaced",
                    address, (unsigned long long)old.frame);
            }
            else if (opened == zpd::BracketBook::Opened::DroppedOldest)
            {
                s_zpdCounts.dropped++;
                LogOnce((0x2Aull << 56) | 3, "occlusion query: more than %u brackets open: the oldest (BEGIN %08X, frame %llu) dropped",
                    zpd::BracketBook::kMax, old.beginAddress, (unsigned long long)old.frame);
            }
            ZpdLog("f%llu BEGIN %08X: segments from %llu, %u open", (unsigned long long)s_frame, address,
                (unsigned long long)s_zpdSegNext, s_zpdOpen.Size());
            return false;  // its zeros are stored as without counting
        }
        case zpd::Kind::End:
        {
            ZpdEnd end{ address, s_zpdSegNext, s_zpdSegNext, s_frame, std::chrono::steady_clock::now() };
            zpd::Bracket closed;
            if (s_zpdOpen.Close(address, &closed))
                end.s0 = closed.s0;
            else
            {
                s_zpdCounts.unmatched++;
                LogOnce((0x2Dull << 56) | address, "occlusion query: END %08X without its BEGIN: zero samples", address);
            }
            s_zpdCounts.ends++;
            ZpdLog("f%llu END %08X queued: segments %llu..%llu", (unsigned long long)s_frame, address,
                (unsigned long long)end.s0, (unsigned long long)end.s1);
            // With the newest submission, as QueueWrite: the one being
            // recorded, else the last in flight, else now (nothing in
            // flight: every count is in, harvested before its submission
            // left s_inFlight under this lock).
            std::lock_guard lock(s_submitMutex);
            if (s_recording)
                s_submissions[s_slot].zpdEnds.push_back(end);
            else if (!s_inFlight.empty())
                s_submissions[s_inFlight.back()].zpdEnds.push_back(end);
            else
                ZpdStore(end);
            return true;
        }
        default:
            s_zpdCounts.unknown++;
            LogOnce((0x2Eull << 56) | address, "occlusion query: report at %08X is neither a BEGIN (+0x20) nor an END (+0) of a 64-byte block: zero samples",
                address);
            return false;
        }
    }

    void LogOcclusionCounts(const char* what)
    {
        if (!s_zpdOn)  // (also when the device counts but the pools couldn't be made)
            return;
        std::string line;
        {
            std::lock_guard lock(s_zpdLatestMutex);
            for (const auto& [address, latest] : s_zpdLatest)
                line += std::format(" {:08X} {} (frame {})", address, latest.first, latest.second);
        }
        fprintf(stderr, "[zpd] %s: latest END counts:%s\n", what, line.empty() ? " none yet" : line.c_str());
    }

    bool PresentFrontBuffer(const uint32_t fetch[6], uint32_t width, uint32_t height, float between, float shift, int64_t target)
    {
        if (!s_vk || width == 0 || height == 0)
            return false;
        // NFSMW_SHOW_TEXTURE=<hex base>,<width>,<height>: present that tiled
        // 32-bit guest texture instead of the front buffer (debugging).
        static const std::array<uint32_t, 3> showTexture = [] {
            std::array<uint32_t, 3> t{};
            if (const char* v = std::getenv("NFSMW_SHOW_TEXTURE"))
                sscanf(v, "%x,%u,%u", &t[0], &t[1], &t[2]);
            return t;
        }();
        uint32_t debugFetch[6];
        if (showTexture[0])
        {
            memcpy(debugFetch, fetch, sizeof(debugFetch));
            debugFetch[0] = (debugFetch[0] & ~(0x1FFu << 22)) | (((showTexture[1] + 31) / 32) << 22) | 0x80000000u;
            debugFetch[1] = (debugFetch[1] & 0xFFFu) | (showTexture[0] & 0x1FFFF000u);
            fetch = debugFetch;
            width = showTexture[1];
            height = showTexture[2];
        }
        uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) << 5;
        struct { uint32_t base, pitch, tiled, endian, format, swizzle, width, height, sx, sy, slotWords, scaled, denX, denY; } pc = {
            fetch[1] & 0x1FFFF000u, pitch ? pitch : (width + 31) & ~31u, (fetch[0] >> 31) & 1, (fetch[1] >> 6) & 3,
            fetch[1] & 0x3F, (fetch[3] >> 1) & 0xFFF, width, height, 1, 1, 0, 0, 1, 1 };
        uint32_t frontBytes = pc.pitch * ((height + 31) & ~31u) * 4;
        // At host resolution when a scaled resolve wrote it (the game's frames;
        // movies and loading screens drawn at 1x use the 1x image). At 1.5x
        // only a front buffer of whole blocks (even; 1280x720 is), below 1x
        // of whole blocks of 2, 4 or 8 (1280x720 is): a smaller image than
        // the frame's, which the presenter scales up (its FSR 1).
        if (pc.tiled && !showTexture[0] && s_scaled.enabled)
        {
            ScaledCheck& check = s_scaledChecks[ScaledCheckKey(pc.base, frontBytes, 0)];  // (ScaledWanted)
            check.base = pc.base;
            check.size = frontBytes;
            check.wanted = s_frame + 1;
        }
        bool scaled = pc.tiled && !showTexture[0] && s_scaled.enabled && s_scaled.scale.Exact(width, height) &&
            ScaledRangeValid(pc.base, frontBytes);
        uint32_t outW = scaled ? s_scaled.scale.x.Size(width) : width, outH = scaled ? s_scaled.scale.y.Size(height) : height;
        const bool b = between != 0.0f;  // an in-between picture (game/hfr.h)
        FrontRing& ring = scaled ? (b ? s_frontBetweenScaled : s_frontScaled) : (b ? s_frontBetween : s_front);
        if (outW != ring.width || outH != ring.height)
        {
            // A new size: wait for any use of the old images before replacing them.
            DestroyFrontImages(ring);
            for (int i = 0; i < s_frontImages; i++)
            {
                Image& image = ring.images[i];
                image = CreateImage(VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { outW, outH, 1 }, 1,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,  // sampled: presenter post-processing
                    VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT);
                ring.current[size_t(i)] = image.image;
                Begin();
                ToGeneral(image.image, VK_IMAGE_ASPECT_COLOR_BIT, 1);
            }
            ring.width = outW;
            ring.height = outH;
            s_frontDirect.pending = false;  // (its image is gone)
            fprintf(stderr, "[renderer] front images %ux%u (%s%s), %d of them\n", outW, outH, scaled ? "host resolution" : "1x",
                b ? ", in-between pictures" : "", s_frontImages);
        }
        if (scaled)
        {
            pc.width = outW;
            pc.height = outH;
            pc.sx = s_scaled.scale.x.num;
            pc.sy = s_scaled.scale.y.num;
            pc.denX = s_scaled.scale.x.den;
            pc.denY = s_scaled.scale.y.den;
            pc.slotWords = s_scaled.slotWords;
            pc.scaled = 1;
            s_scaled.untiledBytes += uint64_t(outW) * outH * 4;
        }
        // NFSMW_RESOLVE_PRESENT: this front buffer as a
        // resolve may store it into a front image (FRONT_DIRECT, see
        // s_hlePresent), and whether one did this frame: then that image is
        // the picture, written already.
        int directImage = -1;
        if (!scaled && !b && pc.tiled && !showTexture[0] && s_hlePresent)
        {
            FrontSeen* slot = &s_frontSeen[0];
            for (FrontSeen& f : s_frontSeen)
                if (f.frame < slot->frame)
                    slot = &f;
            for (FrontSeen& f : s_frontSeen)
                if (f.base == pc.base)
                    slot = &f;
            *slot = { pc.base, pc.pitch, width, height, pc.endian, pc.format, pc.swizzle, s_frame };
            const FrontSeen& as = s_frontDirect.as;
            if (s_frontDirect.pending && s_frontDirect.base == pc.base && s_frontDirect.frame == s_frame && as.pitch == pc.pitch &&
                as.width == width && as.height == height && as.endian == pc.endian && as.format == pc.format && as.swizzle == pc.swizzle &&
                s_frontDirect.written && !writewatch::WrittenSince(pc.base, frontBytes, s_frontDirect.written))
                for (int i = 0; i < s_frontImages; i++)
                    if (ring.current[size_t(i)] == s_frontDirect.image)
                        directImage = i;
        }
        s_frontDirect.pending = false;
        if (!scaled && directImage < 0)
            EnsureShadow(pc.base, frontBytes);
        EndPass(kBreakPresent);
        Begin();
        FullBarrier();  // after earlier work, including the presenter's last read
        // The image this swap writes: not the one the presenter took last,
        // whose read it may queue only after waiting for the display (up to a
        // refresh), nor (while another is free) the frame waiting for it,
        // which stays takeable until here. The presenter can't take the
        // image chosen until SubmitFrontImage hands it over, after the write.
        // (NFSMW_RESOLVE_PRESENT: the one the resolve claimed and wrote,
        // directImage, written already: handed over as it is.)
        Image* written = nullptr;
        if (directImage < 0)
        {
            Image& front = ring.images[size_t(ClaimFrontSlot(ring))];
            VkDescriptorImageInfo ii{ VK_NULL_HANDLE, front.view, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorBufferInfo pool = DummyStorage(s_scaled.pool);
            VkDescriptorBufferInfo table = DummyStorage(s_scaled.table);
            VkWriteDescriptorSet w[7] = {
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &s_sharedInfo[0], nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, nullptr, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &pool, nullptr },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &table, nullptr },
            };
            ComputeWorkRecorded();
            if (s_profiling)
                ProfileCompute(std::format("front buffer {}x{} at {:08X}{}", outW, outH, pc.base, scaled ? " (scaled memory)" : "").c_str());
            s_stateEpoch++;
            const bool fixedFront = scaled && (s_scaled.scale.x.Texel() || s_scaled.scale.y.Texel());
            if (fixedFront && !(s_frontFixedPipeline && s_frontFixedScale == s_scaled.scale))
            {
                // (A live change's old one may still be in flight: kept, not destroyed. Rare.)
                VkDescriptorSetLayoutBinding fb[4] = {
                    { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                    { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                    { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                    { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
                };
                auto bindings = WithSharedParts(fb, 4);
                const gpu::RenderScale& sc = s_scaled.scale;
                CreateComputePipeline(kFrontGlsl, bindings.data(), uint32_t(bindings.size()), 14 * sizeof(uint32_t), s_frontFixedSetLayout,
                    s_frontFixedLayout, s_frontFixedPipeline,
                    std::format("#define SB_FIXED_NUM_X {}u\n#define SB_FIXED_DEN_X {}u\n#define SB_FIXED_NUM_Y {}u\n#define SB_FIXED_DEN_Y {}u\n",
                        sc.x.num, sc.x.den, sc.y.num, sc.y.den).c_str());
                s_frontFixedScale = sc;
            }
            VkPipelineLayout frontLayout = fixedFront ? s_frontFixedLayout : s_frontLayout;
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fixedFront ? s_frontFixedPipeline : s_frontPipeline);
            s_pushDescriptorSet(s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, frontLayout, 0, AddSharedParts(w, 4), w);
            vkCmdPushConstants(s_cmd, frontLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            vkCmdDispatch(s_cmd, (outW + 7) / 8, (outH + 7) / 8, 1);
            GuardGpu(pc.base, frontBytes, false, "front buffer");
            FullBarrier();
            ProfileComputeEnd();
            SubmitRecorded(7);  // the presenter's blit comes later in queue order
            video::SubmitFrontImage(front.image, outW, outH, width, height, between, shift, target);
            s_lastFront = { front.image, outW, outH };
            written = &front;
        }
        else
        {
            Image& front = ring.images[directImage];
            const VkImage shown = ring.current[size_t(directImage)];  // (the resolve's store, or a target's image handed over)
            if (s_profiling)
            {
                ProfileCompute(std::format("front buffer {}x{} at {:08X}: the resolve's (NFSMW_RESOLVE_PRESENT{})", outW, outH, pc.base,
                    shown != front.image ? "=2, its target's image" : "").c_str());
                ProfileComputeEnd();
            }
            SubmitRecorded(7);
            const SpareTarget& ext = ring.external[size_t(directImage)];
            video::SubmitFrontImage(shown, outW, outH, width, height, between, shift, target, ext.image.image ? ext.width : 0,
                ext.image.image ? ext.height : 0);
            s_lastFront = { shown, outW, outH };
            written = &front;
        }
        Image& front = *written;
        // Back at 1x (a live change): the host-resolution images are no
        // longer needed. The presenter has this frame to show instead.
        if (!scaled && s_renderScale.IsOne())
        {
            if (!b && s_frontScaled.width)
                DestroyFrontImages(s_frontScaled);
            if (b && s_frontBetweenScaled.width)
                DestroyFrontImages(s_frontBetweenScaled);
        }
        if (g_eagerWrites)
        {
            // With fences published early nothing else keeps the GPU from
            // falling behind: allow one frame in flight, as the console's
            // GPU never lags more. The ring then fills and the game waits.
            static uint64_t previousSwap = 0;
            uint64_t thisSwap = s_submissionCounter - 1;
            if (previousSwap)
            {
                CpWaitTimer wait(CP_WAIT_THROTTLE);
                std::unique_lock lock(s_submitMutex);
                s_submitCv.wait(lock, [] { return s_inFlight.empty() || s_submissions[s_inFlight.front()].number > previousSwap; });
            }
            previousSwap = thisSwap;
        }

        // NFSMW_CHECK_FRONT=<n>: at the n-th GPU present, read the image back
        // into build/front_check.ppm (verifies this path without a screen).
        // NFSMW_CHECK_FRONT_SEC=<s>: from s seconds after launch, 6 captures
        // 20 presents apart, into build/front_check_<k>.ppm.
        static const int checkAt = [] { const char* v = std::getenv("NFSMW_CHECK_FRONT"); return v ? std::atoi(v) : -1; }();
        static const double checkSec = [] { const char* v = std::getenv("NFSMW_CHECK_FRONT_SEC"); return v ? std::atof(v) : -1.0; }();
        static const auto launch = std::chrono::steady_clock::now();
        static int presents = 0, seriesStart = -1, seriesTaken = 0;
        presents++;
        std::string checkPath;
        if (presents == checkAt)
            checkPath = "build/front_check.ppm";
        else if (checkSec >= 0 && seriesTaken < 6)
        {
            if (seriesStart < 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - launch).count() >= checkSec)
                seriesStart = presents;
            if (seriesStart >= 0 && (presents - seriesStart) % 20 == 0)
                checkPath = "build/front_check_" + std::to_string(seriesTaken++) + ".ppm";
        }
        if (!checkPath.empty())
            width = outW, height = outH;  // what was presented
        VkDeviceSize at = checkPath.empty() ? 0 : RingAllocate(VkDeviceSize(width) * height * 4);
        if (at == ~VkDeviceSize(0))
        {
            LogOnce(0x2400, "front buffer capture %ux%u doesn't fit a staging slot", width, height);
            checkPath.clear();
        }
        if (!checkPath.empty())
        {
            Begin();
            FullBarrier();
            VkBufferImageCopy region{};
            region.bufferOffset = at;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.imageExtent = { width, height, 1 };
            vkCmdCopyImageToBuffer(s_cmd, s_lastFront.image, VK_IMAGE_LAYOUT_GENERAL, s_ring, 1, &region);  // (the image shown: a hand-over's too)
            Flush();
            if (FILE* f = fopen(checkPath.c_str(), "wb"))
            {
                // One bulk copy out of the ring first: on a discrete GPU it
                // is uncached VRAM, where per-pixel reads took ~20 s a frame.
                std::vector<uint8_t> rgba(s_ringHost + at, s_ringHost + at + size_t(width) * height * 4), rgb(size_t(width) * height * 3);
                for (size_t i = 0; i < size_t(width) * height; i++)
                    memcpy(&rgb[i * 3], &rgba[i * 4], 3);
                fprintf(f, "P6\n%u %u\n255\n", width, height);
                fwrite(rgb.data(), 1, rgb.size(), f);
                fclose(f);
                fprintf(stderr, "[renderer] front buffer check written\n");
                LogOcclusionCounts(checkPath.c_str());  // NFSMW_OCCLUSION=1: the counts behind it
            }
        }
        return true;
    }

    void ReadBackFrontImage(FrontPixels done)
    {
        const LastFront front = s_lastFront;
        if (!s_vk || !front.image)
        {
            done(nullptr, 0, 0);
            return;
        }
        // A buffer of its own per copy, made here and freed by the thread
        // that reads it: shots are a handful a run, and nothing the game's
        // frames use (the upload ring, its slots) waits on them. Host-cached
        // memory where there is some, as the shadow map's copy takes it: the
        // CPU reads every byte.
        struct Readback
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void* host = nullptr;
            bool coherent = true;
        };
        auto rb = std::make_shared<Readback>();
        auto release = [](Readback& r) {
            if (r.host)
                vkUnmapMemory(s_dev, r.memory);
            if (r.buffer)
                vkDestroyBuffer(s_dev, r.buffer, nullptr);
            if (r.memory)
                vkFreeMemory(s_dev, r.memory, nullptr);
            r = {};
        };
        VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = VkDeviceSize(front.width) * front.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bool ok = vkCreateBuffer(s_dev, &bci, nullptr, &rb->buffer) == VK_SUCCESS;
        if (ok)
        {
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(s_dev, rb->buffer, &req);
            const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            const VkMemoryPropertyFlags cached = visible | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            VkMemoryAllocateInfo mai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = req.size;
            mai.memoryTypeIndex = UINT32_MAX;
            for (uint32_t i = 0; i < s_memProps.memoryTypeCount && mai.memoryTypeIndex == UINT32_MAX; i++)
                if ((req.memoryTypeBits & (1u << i)) && (s_memProps.memoryTypes[i].propertyFlags & cached) == cached)
                    mai.memoryTypeIndex = i;
            if (mai.memoryTypeIndex == UINT32_MAX)
                mai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, visible | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            ok = (s_memProps.memoryTypes[mai.memoryTypeIndex].propertyFlags & visible) == visible &&
                vkAllocateMemory(s_dev, &mai, nullptr, &rb->memory) == VK_SUCCESS &&
                vkBindBufferMemory(s_dev, rb->buffer, rb->memory, 0) == VK_SUCCESS &&
                vkMapMemory(s_dev, rb->memory, 0, VK_WHOLE_SIZE, 0, &rb->host) == VK_SUCCESS;
            rb->coherent = (s_memProps.memoryTypes[mai.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        }
        if (!ok)
        {
            fprintf(stderr, "[renderer] no host-visible buffer for a front image copy (%ux%u)\n", front.width, front.height);
            release(*rb);
            done(nullptr, 0, 0);
            return;
        }
        // In the command stream after the swap's conversion and before any
        // later write of the image (the next swaps' conversions begin with a
        // full barrier too), so the copy is this swap's picture whatever the
        // GPU's pace. The guest sees nothing of it.
        EndPass(kBreakPresent);
        Begin();
        FullBarrier();
        VkBufferImageCopy region{};
        region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        region.imageExtent = { front.width, front.height, 1 };
        vkCmdCopyImageToBuffer(s_cmd, front.image, VK_IMAGE_LAYOUT_GENERAL, rb->buffer, 1, &region);
        VkMemoryBarrier toHost{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr, 0, nullptr);
        s_frontReadbacks.fetch_add(1, std::memory_order_relaxed);
        // Once the GPU is past the copy: on the completion thread, under its
        // lock, when the GPU passes the event SignalWrites records after the
        // copy, else at the submission's completion. Not QueueWrite: with
        // eager writes (the default) that runs at once, and the reader found
        // the buffer still empty, or half copied. The reading (and whatever
        // `done` writes) goes to a thread of its own, so neither the
        // completions nor the game wait for it.
        auto landed = [rb, release, front, done = std::move(done)]() mutable {
            std::thread([rb, release, front, done = std::move(done)] {
                if (!rb->coherent)
                {
                    VkMappedMemoryRange range{ VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
                    range.memory = rb->memory;
                    range.size = VK_WHOLE_SIZE;
                    vkInvalidateMappedMemoryRanges(s_dev, 1, &range);
                }
                done(static_cast<const uint8_t*>(rb->host), front.width, front.height);
                release(*rb);
                s_frontReadbacks.fetch_sub(1, std::memory_order_relaxed);
            }).detach();
        };
        {
            std::lock_guard lock(s_submitMutex);
            s_submissions[s_slot].writes.push_back({ std::move(landed) });
        }
        SignalWrites();  // (outside a pass: this recording has none open)
    }

    void OnSwap(uint64_t frame)
    {
        HfrDrawMatchSwap(frame);  // DIAG (v0.1.5 HFR scoping)
        if (s_vk)
            ApplyLiveSettings();
        if (frame % 600 == 0)
            ReportColourCheck("so far");
        if (s_resolveUseFrom && frame >= s_resolveUseFrom)
        {
            s_resolveUse = true;
            if (++s_resolveUseFrames % 600 == 0)
                ReportResolveUse("so far");
        }
        if (s_uploadLog)
        {
            UploadLog& log = *s_uploadLog;
            if (log.now)  // s_frame just ended
            {
                std::string paths, writers;
                for (auto& [k, n] : log.byPath)
                    paths += std::format(" {} {}", k, n);
                for (auto& [k, n] : log.byWriters)
                    writers += std::format(" {} {},", k, n);
                fprintf(stderr, "[upload] frame %llu summary: %u uploads (%llu KB, %u ending a pass) | paths:%s | writers:%s | resolves %u (%u fused)\n",
                    (unsigned long long)s_frame, log.uploads, (unsigned long long)(log.bytes / 1024), log.endedPasses, paths.c_str(),
                    writers.c_str(), log.resolvesNow, log.fusedNow);
                log.uploads = log.endedPasses = log.resolvesNow = log.fusedNow = 0;
                log.bytes = 0;
                log.byPath.clear();
                log.byWriters.clear();
            }
            log.now = frame >= log.first && frame <= log.last;
            writewatch::SetWriterFrame(frame);
        }
        s_cacheFrame = frame;
        s_frame = frame;
        if (s_vk)
            TrimTextureCache();
        // NFSMW_RESOLVE_STATS's window.
        s_hleCounting = s_hleWindow.second > s_hleWindow.first && frame >= s_hleWindow.first && frame < s_hleWindow.second;
        s_hle.frames += s_hleCounting;
        if (s_vk)
            PrewarmStep();  // (until the key list's pipelines are made)
        if (s_zpdOn)
            ZpdOnSwap(frame);
        if (s_logShadowMap)
            ShadowMapProbeOnSwap();
        // NFSMW_PASS_BARRIER: per frame of the window ending here, with the
        // level it ran at, before its [perf] line. Vertex barriers cost a
        // pass's overlap (a split: a pass too).
        if (s_prepassOn && frame % 120 == 0)
        {
            static PrepassCounts last{};
            const PrepassCounts& c = s_prepassCounts;
            fprintf(stderr, "[prepass] per frame: %.1f passes, %.0f of their %.0f draws in a pre-pass (%.0f of them only there: depth only), "
                "%.0f waiting for its pipeline\n", double(c.passes - last.passes) / 120, double(c.prepassed - last.prepassed) / 120,
                double(c.draws - last.draws) / 120, double(c.depthOnly - last.depthOnly) / 120, double(c.waiting - last.waiting) / 120);
            last = c;
        }
        if (frame % 120 == 0 && std::getenv("NFSMW_CLEAR_STATS"))
        {
            static ClearCounts last{};
            const ClearCounts& c = s_clearCounts;
            auto k = [&](int d, int i, double& n, double& mp) {
                n = double(c.kind[d][i].count - last.kind[d][i].count) / 120;
                mp = double(c.kind[d][i].pixels - last.kind[d][i].pixels) / 120 / 1e6;
            };
            double n[4], mp[4];
            k(0, 0, n[0], mp[0]); k(0, 1, n[1], mp[1]); k(1, 0, n[2], mp[2]); k(1, 1, n[3], mp[3]);
            fprintf(stderr, "[clears] per frame: colour %.1f load ops (%.2f Mpx) %.1f in passes (%.2f Mpx) | depth %.1f load ops (%.2f Mpx) "
                "%.1f in passes (%.2f Mpx)\n", n[0], mp[0], n[1], mp[1], n[2], mp[2], n[3], mp[3]);
            last = c;
        }
        if (s_passBarrierLevel && frame % 120 == 0)
        {
            static BarrierCounts last{};
            const BarrierCounts& c = s_barrierCounts;
            fprintf(stderr, "[barrier] level %u per frame: %.1f pass ends and %.1f resolves narrowed, %.2f full at the cap | vertex barriers %.2f"
                " (vertex data %.2f, indices %.2f, a texture %.2f), %.2f splitting a pass\n", s_passBarrier,
                double(c.passEnds - last.passEnds) / 120, double(c.resolves - last.resolves) / 120, double(c.capped - last.capped) / 120,
                double(c.vertex[0] + c.vertex[1] + c.vertex[2] - last.vertex[0] - last.vertex[1] - last.vertex[2]) / 120,
                double(c.vertex[0] - last.vertex[0]) / 120, double(c.vertex[1] - last.vertex[1]) / 120,
                double(c.vertex[2] - last.vertex[2]) / 120, double(c.splits - last.splits) / 120);
            last = c;
        }
        // NFSMW_PASS_BARRIER_ALT: the next window's level. The swap's front
        // buffer has just put a full barrier behind everything recorded; a
        // CPU present's Flush (frame dumps) only idles the GPU, so what a
        // resolve left for the vertex stages gets one here before level 0.
        if (s_passBarrierAlt && frame % 120 == 0)
        {
            s_passBarrier = (frame / 120) % 2 ? s_passBarrierLevel : 0u;
            if (!s_passBarrier && (s_vertexPendingLo < s_vertexPendingHi || !s_vertexPendingTextures.empty()))
            {
                Begin();
                FullBarrier();
            }
            fprintf(stderr, "[barrier] alt: frames %llu-%llu run with level %u\n", (unsigned long long)(frame + 1),
                (unsigned long long)(frame + 120), s_passBarrier);
        }
        // NFSMW_LOG_UNTILED_AREA=1: with each [perf] line, whether the toggle
        // was on in its window (=alt: every other one) and what it did.
        if (s_logUntiledArea && frame % 120 == 0)
        {
            static UntiledAreaCounts last;
            const UntiledAreaCounts& n = s_ua;
            auto per = [](uint64_t now, uint64_t before) { return double(now - before) / 120.0; };
            const bool on = s_untiledArea == 1 || (s_untiledArea == 2 && (frame - 1) / 120 % 2 == 1);
            fprintf(stderr, "[untiled-area] %s per frame: %.1f untiled draws cut to their viewport, %.1f whole target (%.1f clipping off, "
                "%.1f no viewport transform, %.1f read past), %.1f nothing inside; %.2f pass restarts | %.1f passes, %.2f Mpx, %.1f MB loaded+stored; "
                "untiled %.1f passes, %.2f Mpx, %.1f MB | %llu resolves read past (total)\n",
                on ? "on" : "off", per(n.cut, last.cut), per(n.whole, last.whole), per(n.clipOff, last.clipOff), per(n.noViewport, last.noViewport),
                per(n.wholeArea, last.wholeArea), per(n.empty, last.empty), per(n.restarts, last.restarts), per(n.passes, last.passes),
                per(n.passPixels, last.passPixels) / 1e6, per(n.passBytes, last.passBytes) / 1e6, per(n.untiledPasses, last.untiledPasses),
                per(n.untiledPixels, last.untiledPixels) / 1e6, per(n.untiledBytes, last.untiledBytes) / 1e6, (unsigned long long)n.resolvesPast);
            last = n;
        }
        // NFSMW_LOG_CP_CACHES=1 (or NFSMW_CP_CACHE_VERIFY): the command
        // processor's lookup caches, with each [perf] line. (A separate
        // check from NFSMW_CP_OPT_VERIFY, whose index check reads the ring
        // back: uncached VRAM on the Deck and the Steam Machine, seconds a
        // frame.)
        static const bool logCaches = std::getenv("NFSMW_LOG_CP_CACHES") != nullptr || s_textureCache->verify;
        if (logCaches && frame % 120 == 0)
        {
            const TextureCache& t = *s_textureCache;
            const ShaderCache& sh = *s_shaderCache;
            const uint64_t now[12] = { t.hits, t.misses, t.revalidated, t.stale, t.uncached, t.recentHits, sh.loads, sh.hits,
                sh.comparedBytes, t.memoDraws, t.memoBindings, t.lookupDraws };
            static uint64_t last[12] = {};
            uint64_t d[12];
            for (int i = 0; i < 12; i++)
                d[i] = now[i] - last[i];
            std::copy(std::begin(now), std::end(now), last);
            auto pct = [](uint64_t part, uint64_t whole) { return whole ? 100.0 * double(part) / double(whole) : 0.0; };
            // (The lookups are those the draw memo didn't serve; it serves
            // a draw's textures without any.)
            fprintf(stderr, "[cpcache] textures: %.0f lookups/frame, %.1f%% kept (%.1f%% of them checked after a write, %.1f%% by hash), "
                "%.1f%% stale, %.1f%% not keepable; draw memo: %.1f%% of %.0f draws/frame with textures (%.0f bindings/frame) | "
                "shaders: %.0f loads/frame, %.1f%% found by address (%.0f KB compared) | "
                "mismatches: %llu textures, %llu shaders (%llu texture checks raced a write)\n",
                double(d[0] + d[1]) / 120, pct(d[0], d[0] + d[1]), pct(d[2], d[0]), pct(d[5], d[0]), pct(d[3], d[0] + d[1]),
                pct(d[4], d[0] + d[1]), pct(d[9], d[9] + d[11]), double(d[9] + d[11]) / 120, double(d[10]) / 120,
                double(d[6]) / 120, pct(d[7], d[6]), double(d[8]) / 1024 / 120,
                (unsigned long long)t.mismatches, (unsigned long long)sh.mismatches, (unsigned long long)t.raced);
        }
        s_pipelineWaitLeftUs = s_pipelineWaitUs;
        // A loading screen (or movie): wait for pipelines in full for 2 s.
        if (s_stats.draws - s_drawsAtSwap < 64)
            s_waitPipelinesUntil = frame + 120;
        s_drawsAtSwap = s_stats.draws;
        if (s_pipelineSkips)
        {
            static uint32_t logged = 0;
            if (logged++ < 300)
                fprintf(stderr, "[pipelines] frame %llu: %u draws skipped, %zu pipelines compiling\n",
                    (unsigned long long)frame, s_pipelineSkips, s_pendingPipelines.size());
            s_stats.pipelineSkips += s_pipelineSkips;
            s_pipelineSkips = 0;
        }
        // New pipelines are saved every 10 s, on a thread of its own: getting a
        // large cache's data and writing it took ~70 ms, a visible stutter
        // when it ran here (the pipeline cache is internally synchronized).
        if (s_vk && frame % 600 == 0 && s_stats.pipelines != s_pipelinesAtSave)
        {
            s_pipelinesAtSave = uint32_t(s_stats.pipelines);
            static std::atomic<bool> saving{ false };
            if (!saving.exchange(true))
                std::thread([] {
                    SetHostThreadName("nfsmw-cachesave");
                    hostcpu::LeaveReservedCore();
                    SavePipelineCache();
                    SavePipelineKeys();
                    saving = false;
                }).detach();
        }
        // NFSMW_LOG_DRAWS=<seconds>: the first frame after that time.
        static const double logDrawsSec = [] { const char* v = std::getenv("NFSMW_LOG_DRAWS"); return v ? std::atof(v) : -1.0; }();
        static const auto launch = std::chrono::steady_clock::now();
        static bool loggedDraws = false;
        s_logDrawsNow = false;
        if (logDrawsSec >= 0 && !loggedDraws &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - launch).count() >= logDrawsSec)
        {
            loggedDraws = true;
            s_logDrawsNow = true;
            fprintf(stderr, "[draws] frame %llu\n", (unsigned long long)frame);
        }
        if (!s_vk || s_profileFrame < 0)
            return;
        auto startProfile = [] {
            s_profiling = true;
            s_passInfos.clear();
            s_gapLast = { s_stats.resolves, s_stats.uploads, s_stats.submits, writewatch::GetGuardStats().uploadBytes };
            s_passQueryNext = 0;
            s_passEndPending = false;
        };
        if (frame == uint64_t(s_profileFrame))
        {
            startProfile();
            return;
        }
        if (!s_profiling)
            return;
        s_profiling = false;
        Flush();  // presentation no longer waits: make the results ready
        // Per pass, also the GPU time since the previous pass ended: the
        // resolves, untiles, copies and barriers between them (and idle time
        // where a submission waited).
        double total = 0, between = 0;
        uint64_t lastEnd = 0;
        fprintf(stderr, "[profile] frame %llu: %zu passes\n", (unsigned long long)(frame - 1), s_passInfos.size());
        for (const PassInfo& pi : s_passInfos)
        {
            uint64_t ts[2] = {};
            vkGetQueryPoolResults(s_dev, s_passQueries, pi.query, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            double us = ts[1] > ts[0] ? double(ts[1] - ts[0]) * s_timestampPeriod / 1000.0 : 0.0;
            double gap = lastEnd && ts[0] > lastEnd ? double(ts[0] - lastEnd) * s_timestampPeriod / 1000.0 : 0.0;
            total += us;
            between += gap;
            if (ts[1] > ts[0])
                lastEnd = ts[1];
            // A pass also says why it began: "| after <what ended the one
            // before>[ (submit site)], same targets|new targets".
            std::string why;
            if (pi.desc.starts_with("area"))
                why = std::format("  | after {}{}, {} targets", kPassBreakNames[std::min<size_t>(pi.why, std::size(kPassBreakNames) - 1)],
                    pi.why == kBreakSubmit ? std::format(" ({})", pi.submitWhy) : std::string(), pi.same ? "same" : "new");
            std::string stats;
            if (pi.stats)
            {
                uint64_t st[2] = {};
                if (vkGetQueryPoolResults(s_dev, s_passStats, pi.query / 2, 1, sizeof(st), st, sizeof(st), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    stats = std::format("  | VS {:.0f}k, FS {:.2f} Mpx = {:.2f} a pixel", double(st[0]) / 1e3, double(st[1]) / 1e6,
                        pi.areaPx ? double(st[1]) / double(pi.areaPx) : 0.0);
                uint64_t kept = 0;
                if (s_passSamples &&
                    vkGetQueryPoolResults(s_dev, s_passSamples, pi.query / 2, 1, sizeof(kept), &kept, sizeof(kept), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    stats += std::format(", samples kept {:.2f} M = {:.2f} a pixel", double(kept) / 1e6,
                        pi.areaPx ? double(kept) / double(pi.areaPx) : 0.0);
                stats += " | pre-pass:";
                for (int r = 0; r < 8; r++)
                    if (pi.prepass[r])
                        stats += std::format(" {} {}", kPrepassReasonNames[r], pi.prepass[r]);
            }
            fprintf(stderr, "[profile] %8.1f us  %4u draws  (+%7.1f us before: %u resolves, %u uploads, %u submits, %llu KB synced)  %s%s%s\n",
                us, pi.draws, gap, pi.resolves, pi.uploads, pi.submits, (unsigned long long)pi.shadowKB, pi.desc.c_str(), why.c_str(),
                stats.c_str());
        }
        fprintf(stderr, "[profile] total %.1f us in passes, %.1f us between them\n", total, between);
        if (frame < uint64_t(s_profileFrame) + uint64_t(s_profileFrames))
            startProfile();
    }

    std::vector<uint32_t> CompileShader(const std::string& glsl, int stage, std::string& log)
    {
        static std::once_flag initialized;
        std::call_once(initialized, [] { glslang_initialize_process(); });
        glslang_stage_t stages[3] = { GLSLANG_STAGE_VERTEX, GLSLANG_STAGE_FRAGMENT, GLSLANG_STAGE_COMPUTE };
        return CompileGlsl(glsl, stages[std::clamp(stage, 0, 2)], log);
    }

    void Shutdown()
    {
        if (!s_vk)
            return;
        {
            // Let running compiles finish (their pipelines go into the
            // cache being saved); drop the queued ones.
            std::unique_lock lock(s_compileMutex);
            s_compileQueue.clear();
            s_prewarmQueue.clear();
            s_compileCv.wait_for(lock, std::chrono::seconds(2), [] { return s_compileActive == 0; });
            s_compileStopping = true;
        }
        s_compileCv.notify_all();
        SavePipelineCache();
        SavePipelineKeys();
        ReportResolveUse("at exit");
        if (s_hle.frames)  // NFSMW_RESOLVE_STATS
        {
            const double n = double(s_hle.frames), mb = 1048576.0;
            fprintf(stderr, "[resolve-stats] %llu frames (%llu..%llu), a frame: %.1f resolves, read %.2f MB, "
                "guest bytes %.2f MB now + %.2f MB written back later, texture stores %.2f MB; %.1f passes, %.1f full barriers, "
                "%.1f narrow barriers, %.1f submits; renamed %.1f, to the presenter %.1f\n",
                (unsigned long long)s_hle.frames, (unsigned long long)s_hleWindow.first, (unsigned long long)s_hleWindow.second,
                double(s_hle.resolves) / n, double(s_hle.srcBytes) / n / mb, double(s_hle.guestBytes) / n / mb,
                double(s_hle.writebackBytes) / n / mb, double(s_hle.textureBytes) / n / mb, double(s_hle.passes) / n,
                double(s_hle.fullBarriers) / n, double(s_hle.narrowBarriers) / n, double(s_hle.submits) / n, double(s_hle.renamed) / n,
                double(s_hle.presented) / n);
        }
        if (s_hleRename)
            fprintf(stderr, "[renderer] renamed (NFSMW_RESOLVE_RENAME): %llu, dropped by a load or a whole store %llu, stored back %llu\n",
                (unsigned long long)s_renames, (unsigned long long)s_renameDrops, (unsigned long long)s_renameMaterialized);
        ReportColourCheck("at exit");
        if (s_resolveDefer || s_resolveDeferColor || s_hleLazy)
        {
            auto ds = writewatch::GetDeferStats();
            fprintf(stderr, "[renderer] deferred resolves (NFSMW_RESOLVE_DEFER): %llu (%llu colour, %llu NFSMW_RESOLVE_LAZY), replaced unwritten %llu, written for GPU "
                "work %llu, for the CPU %llu, for a resolve %llu, for room %llu; CPU accesses that waited %llu, stale read-backs %llu\n",
                (unsigned long long)s_deferredCount.load(), (unsigned long long)s_deferredColour.load(), (unsigned long long)s_deferredHle.load(), (unsigned long long)s_deferReplaced.load(),
                (unsigned long long)s_deferForGpu.load(), (unsigned long long)s_deferForCpu.load(), (unsigned long long)s_deferForResolve.load(),
                (unsigned long long)s_deferForRoom.load(), (unsigned long long)ds.waits, (unsigned long long)ds.staleReadbacks);
        }
        // A shot's copy still being read and written out (ReadBackFrontImage):
        // a moment for it, so its file is whole and its buffer freed.
        for (int i = 0; i < 200 && s_frontReadbacks.load(std::memory_order_relaxed) > 0; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    void HitchReport(uint64_t frame, double frameMs, uint32_t vblanks)
    {
        // A frame is late when it took NFSMW_HITCH_LATE_MS (default 3; 0 off)
        // longer than the median of the last 63, so a 20 ms frame among 16.7
        // ms ones is, unless it continues a cadence (as long as the frame
        // before, a whole number of vblanks: 30 fps in movies and parts of
        // the front end); or when it took NFSMW_HITCH_MS (default 50) or
        // more. At most 8 are logged per 120-frame [perf] window (its "late"
        // count has them all), with where the command processor's time went
        // and what the game's threads were doing meanwhile. A fixed 50 ms
        // threshold hid the 19-25 ms frames that stutter while driving.
        static const double lateMs = [] { const char* v = std::getenv("NFSMW_HITCH_LATE_MS"); return v ? std::atof(v) : 3.0; }();
        static const double threshold = [] { const char* v = std::getenv("NFSMW_HITCH_MS"); return v ? std::atof(v) : 50.0; }();
        static std::array<double, 63> recent{};
        static uint32_t recentCount = 0;
        double median = 0, previous = recentCount ? recent[(recentCount - 1) % recent.size()] : 0;
        if (recentCount >= 15)
        {
            std::array<double, 63> sorted = recent;
            uint32_t n = std::min<uint32_t>(recentCount, uint32_t(sorted.size()));
            std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.begin() + n);
            median = sorted[n / 2];
        }
        recent[recentCount++ % recent.size()] = frameMs;

        struct Totals
        {
            uint64_t busyNs, waitNs[CP_WAIT_KINDS], draws, tileSkips, resolves, submits, snapshotBytes, gpuExecNs, gameWaitNs, gamePresentNs,
                allocations, allocationNs;
            writewatch::GuardStats guard;
            uint64_t cpuNs = 0;
            long preemptions = 0;
        };
        static Totals last{};
        Totals now{ g_cpBusyNs, {}, s_stats.draws, s_stats.tileSkips, s_stats.resolves, s_stats.submits, s_stats.snapshotBytes,
            s_gpuExecNs.load(), timeline::g_gameWaitNs.load(), timeline::g_gamePresentNs.load(), s_imageTotals.allocations,
            s_imageTotals.allocationNs, writewatch::GetGuardStats() };
        std::copy(std::begin(g_cpWaitKindNs), std::end(g_cpWaitKindNs), now.waitNs);
        // This (the command processor's) thread's CPU time, and on Linux the
        // times the scheduler took its core away: busy time the thread wasn't
        // on a core is time it was descheduled, not work.
        struct timespec cpu{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
        now.cpuNs = uint64_t(cpu.tv_sec) * 1000000000ull + uint64_t(cpu.tv_nsec);
#ifdef RUSAGE_THREAD
        struct rusage usage{};
        getrusage(RUSAGE_THREAD, &usage);
        now.preemptions = usage.ru_nivcsw;
#endif
        double gameFrameMs = double(timeline::g_gameLongestFrameNs.exchange(0)) / 1e6;
        double vblanksLong = std::round(frameMs / 16.667);
        bool cadence = std::abs(frameMs - previous) < 1 && vblanksLong >= 2 && std::abs(frameMs - vblanksLong * 16.667) < 1;
        bool late = frameMs >= threshold || (lateMs > 0 && median > 0 && frameMs > median + lateMs && !cadence);
        static uint64_t window = 0;
        static uint32_t logged = 0;
        if ((frame + 119) / 120 != window)  // the [perf] line at frame % 120 == 0 closes a window
        {
            window = (frame + 119) / 120;
            logged = 0;
        }
        if (late)
            s_stats.lateFrames++;
        if (late && logged++ < 8)
        {
            auto ms = [](uint64_t ns) { return double(ns) / 1e6; };
            double waits[CP_WAIT_KINDS], waitMs = 0;
            for (uint32_t k = 0; k < CP_WAIT_KINDS; k++)
                waitMs += waits[k] = ms(now.waitNs[k] - last.waitNs[k]);
            // The frame's time is guest time, which stood still while the
            // game was suspended: that wait is shown apart, outside the
            // frame's waits and idle time.
            waitMs -= waits[CP_WAIT_SUSPENDED];
            char suspended[48] = "";
            if (waits[CP_WAIT_SUSPENDED] > 0)
                snprintf(suspended, sizeof(suspended), ", suspended %.1f ms", waits[CP_WAIT_SUSPENDED]);
            double busyMs = ms(now.busyNs - last.busyNs);
            const writewatch::GuardStats& g = now.guard;
            const writewatch::GuardStats& lg = last.guard;
            // idle: the ring was empty (the command processor waited for the game).
            char preempted[32] = "";
#ifdef RUSAGE_THREAD
            snprintf(preempted, sizeof(preempted), ", preempted %ld times", now.preemptions - last.preemptions);
#endif
            // Textures' time includes their shadow sync; mprotect counts all
            // threads (the guard's fault handler too).
            fprintf(stderr, "[hitch] frame %llu took %.1f ms (median %.1f, %u vblanks) | command processor: busy %.1f ms, "
                "waits %.1f ms (throttle %.1f, regmem %.1f, shadow %.1f, flush %.1f, pipeline %.1f, slot %.1f)%s, idle %.1f ms, "
                "on a core %.1f ms%s | "
                "shadow sync %.1f ms (%.0f KB, %.0f KB of it new; copying %.1f ms), textures %u (%.1f ms, %.0f KB, %u scaled), images created %u (%.1f ms), "
                "image memory allocations %llu (%.1f ms), shaders %u (%.1f ms), pipelines %u (%.1f ms), submits %llu (%.1f ms), draws %llu (%llu later tiles), resolves %llu, "
                "snapshots %.0f KB | guard: %llu faults, %llu stalls (%.1f ms), %llu mprotect (%.1f ms), %.0f KB read back, "
                "%.0f KB file reads | "
                "game: longest frame %.1f ms, present %.1f ms, D3D waits %.1f ms | GPU executing %.1f ms\n",
                (unsigned long long)frame, frameMs, median, vblanks, busyMs, waitMs, waits[CP_WAIT_THROTTLE], waits[CP_WAIT_REGMEM],
                waits[CP_WAIT_SHADOW], waits[CP_WAIT_FLUSH], waits[CP_WAIT_PIPELINE], waits[CP_WAIT_SLOT], suspended,
                std::max(0.0, frameMs - busyMs - waitMs), ms(now.cpuNs - last.cpuNs), preempted, s_hitch.shadowMs, double(g.uploadBytes - lg.uploadBytes) / 1024,
                double(g.firstUploadBytes - lg.firstUploadBytes) / 1024, ms(g.uploadNs - lg.uploadNs), s_hitch.textures, s_hitch.textureMs, double(s_hitch.textureBytes) / 1024,
                s_hitch.scaledTextures, s_hitch.images, s_hitch.imageMs, (unsigned long long)(now.allocations - last.allocations),
                ms(now.allocationNs - last.allocationNs), s_hitch.shaders, s_hitch.shaderMs, s_hitch.pipelines,
                s_hitch.pipelineMs, (unsigned long long)(now.submits - last.submits), s_hitch.submitMs,
                (unsigned long long)(now.draws - last.draws), (unsigned long long)(now.tileSkips - last.tileSkips),
                (unsigned long long)(now.resolves - last.resolves), double(now.snapshotBytes - last.snapshotBytes) / 1024,
                (unsigned long long)(g.faults - lg.faults), (unsigned long long)(g.stalls - lg.stalls), double(g.stallUs - lg.stallUs) / 1000,
                (unsigned long long)(g.protects - lg.protects), ms(g.protectNs - lg.protectNs), double(g.readbackBytes - lg.readbackBytes) / 1024, double(g.hostWriteBytes - lg.hostWriteBytes) / 1024,
                gameFrameMs, ms(now.gamePresentNs - last.gamePresentNs), ms(now.gameWaitNs - last.gameWaitNs),
                ms(now.gpuExecNs - last.gpuExecNs));
        }
        last = now;
        s_hitch = {};
    }

    namespace
    {
        std::mutex s_perfMutex;
        PerfNumbers s_perfNumbers;
    }

    PerfNumbers GetPerfNumbers()
    {
        std::lock_guard lock(s_perfMutex);
        return s_perfNumbers;
    }

    void PublishPerfNumbers(const PerfNumbers& numbers)
    {
        std::lock_guard lock(s_perfMutex);
        s_perfNumbers = numbers;
    }

    MemoryStats GetMemoryStats()
    {
        MemoryStats m{};
        for (const auto& [key, rt] : s_targets)
        {
            m.targets += rt->image.bytes;
            m.targetCount++;
        }
        for (const auto& [key, t] : s_textures)
        {
            m.textures += t->image.bytes;
            m.textureCount++;
        }
        for (const RetiredTexture& r : s_retiredTextures)  // (evicted, held until the GPU is done with them)
        {
            m.textures += r.texture->image.bytes;
            m.textureCount++;
        }
        m.textureEvictions = s_textureEvictions.textures;
        m.textureEvictedBytes = s_textureEvictions.bytes;
        m.textureReclaims = s_textureEvictions.reclaims;
        m.scaled = s_scaled.poolBytes + s_scaled.tableBytes + s_scaled.scratchBytes;
        for (const FrontRing* ring : { &s_front, &s_frontScaled, &s_frontBetween, &s_frontBetweenScaled })
            for (const Image& image : ring->images)
                m.front += image.bytes;
        auto own = [&m](const Image& image) {
            if (image.memory && !image.block)
            {
                m.own++;
                m.ownBytes += image.bytes;
            }
        };
        for (const auto& [key, rt] : s_targets)
            own(rt->image);
        for (const auto& [key, t] : s_textures)
            own(t->image);
        for (const RetiredTexture& r : s_retiredTextures)
            own(r.texture->image);
        for (const FrontRing* ring : { &s_front, &s_frontScaled, &s_frontBetween, &s_frontBetweenScaled })
            for (const Image& image : ring->images)
                own(image);
        for (const Image* image : { &s_dummy[0], &s_dummy[1], &s_dummy[2], &s_dummyStorage })
            own(*image);
        for (const auto& block : s_blocks)
        {
            m.blocks++;
            m.blockBytes += block->ranges.Size();
            m.rangeBytes += block->ranges.Used();
            m.ranges += block->ranges.Ranges();
        }
        {
            std::lock_guard lock(s_spareMutex);
            for (const std::vector<Spare>& spares : s_spares)
                m.spares += uint32_t(spares.size());
            m.spareBlocks = s_imageTotals.spareBlocks;
            m.spareNs = s_imageTotals.spareNs;
            m.blocksFreed = s_imageTotals.blocksFreed;
        }
        m.spareBytes = m.spares * s_blockSize;
        m.images = s_imageTotals.images;
        m.imageNs = s_imageTotals.imageNs;
        m.allocations = s_imageTotals.allocations;
        m.allocationNs = s_imageTotals.allocationNs;
        m.fromSpare = s_imageTotals.fromSpare;
        m.blocksHere = s_imageTotals.blocksHere;
        DeviceMemoryBudget(m.deviceUsed, m.deviceBudget);
        return m;
    }

    ScaledStats GetScaledStats()
    {
        return { s_scaled.loads, s_scaled.untiledBytes, s_scaled.resolvedBytes, s_scaled.allocations, s_scaled.evictions,
            s_scaled.fallbacks, s_scaled.hosted };
    }

    Stats GetStats()
    {
        s_stats.shaderCacheHits = s_shaderCacheHits.load(std::memory_order_relaxed);
        s_stats.gpuBusyUs = s_gpuBusyUs.load();
        s_stats.gpuExecUs = s_gpuExecNs.load() / 1000;
        return s_stats;
    }
}
