// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See cp_sampler.h.
#include <stdafx.h>
#include "cp_sampler.h"

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace cpsampler
{
    void StartForCurrentThread()
    {
        static std::atomic<bool> started{ false };
        const char* v = std::getenv("NFSMW_CP_SAMPLE");
        if (!v || !*v || started.exchange(true))
            return;
        std::string spec(v), file = spec;
        double fromS = 60, seconds = 10;
        if (size_t c = spec.find(','); c != std::string::npos)
        {
            file = spec.substr(0, c);
            fromS = std::atof(spec.c_str() + c + 1);
            if (size_t c2 = spec.find(',', c + 1); c2 != std::string::npos)
                seconds = std::atof(spec.c_str() + c2 + 1);
        }
        HANDLE thread = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
            return;
        const auto launch = std::chrono::steady_clock::now();
        std::thread([=] {
            std::this_thread::sleep_until(launch + std::chrono::duration<double>(fromS));
            constexpr int kFrames = 16;
            std::map<std::vector<uint64_t>, uint32_t> stacks;
            uint64_t samples = 0;
            const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
            while (std::chrono::steady_clock::now() < end)
            {
                // Nothing allocates while the thread is stopped (it may hold the heap's lock).
                uint64_t frames[kFrames];
                int count = 0;
                if (SuspendThread(thread) != DWORD(-1))
                {
                    CONTEXT ctx{};
                    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                    if (GetThreadContext(thread, &ctx))
                        while (count < kFrames && ctx.Rip)
                        {
                            frames[count++] = ctx.Rip;
                            DWORD64 base = 0;
                            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                            if (!fn)
                            {
                                // A leaf without unwind data: its return address is on top of the stack.
                                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                                ctx.Rsp += 8;
                                continue;
                            }
                            void* handlerData = nullptr;
                            DWORD64 establisher = 0;
                            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
                        }
                    ResumeThread(thread);
                }
                if (count)
                {
                    stacks[std::vector<uint64_t>(frames, frames + count)]++;
                    samples++;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
            // Addresses as module+offset: the modules are those loaded now.
            std::map<HMODULE, std::string> names;
            auto where = [&](uint64_t address) {
                HMODULE module = nullptr;
                if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>(address), &module))
                    return std::string("?+0x") + std::to_string(address);
                auto it = names.find(module);
                if (it == names.end())
                {
                    char path[MAX_PATH] = "?";
                    GetModuleFileNameA(module, path, MAX_PATH);
                    std::string name(path);
                    if (size_t slash = name.find_last_of("\\/"); slash != std::string::npos)
                        name = name.substr(slash + 1);
                    it = names.emplace(module, name).first;
                }
                char text[64];
                snprintf(text, sizeof(text), "+0x%llx", (unsigned long long)(address - reinterpret_cast<uint64_t>(module)));
                return it->second + text;
            };
            if (FILE* f = fopen(file.c_str(), "w"))
            {
                fprintf(f, "# %llu samples of the command processor, every 0.5 ms for %.1f s from %.1f s\n",
                    (unsigned long long)samples, seconds, fromS);
                for (const auto& [frames, n] : stacks)
                {
                    fprintf(f, "%u", n);
                    for (uint64_t a : frames)
                        fprintf(f, " %s", where(a).c_str());
                    fprintf(f, "\n");
                }
                fclose(f);
                fprintf(stderr, "[cp-sample] %llu samples, %zu stacks: %s\n", (unsigned long long)samples, stacks.size(), file.c_str());
            }
            CloseHandle(thread);
        }).detach();
    }
}
#else
namespace cpsampler
{
    void StartForCurrentThread() {}
}
#endif
