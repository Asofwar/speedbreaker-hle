// SpeedBreaker Windows port. GPL-3.0-or-later.
#undef _INC_WINDOWS
#include <windows.h>
#include <io.h>
#include <cstdio>
#include <memoryapi.h>
#include "win_sys.h"
#include "dlfcn.h"
#include "sys/mman.h"
#include "sys/resource.h"
#include <psapi.h>
#include <cerrno>
#include <csignal>
#include <exception>
#include <string>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace
{
    std::wstring Wide(const char* utf8)
    {
        if (!utf8)
            return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        std::wstring w(n > 0 ? size_t(n - 1) : 0, L'\0');
        if (n > 1)
            MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
        return w;
    }

    std::string Utf8(const wchar_t* w)
    {
        if (!w)
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? size_t(n - 1) : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    int ErrnoOf(DWORD error)
    {
        switch (error)
        {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND: return ENOENT;
        case ERROR_ACCESS_DENIED:
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION: return EACCES;
        case ERROR_NOT_SAME_DEVICE: return EXDEV;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return ENOSPC;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS: return EEXIST;
        case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
        default: return EIO;
        }
    }

    thread_local std::string t_dlerror;
}

int rename(const wchar_t* a, const wchar_t* b)
{
    // Write-through: the move is on disk when it returns, as the callers'
    // fsync of the folder makes it elsewhere.
    if (MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;
    errno = ErrnoOf(GetLastError());
    return -1;
}

int getrusage(int, struct rusage* usage)
{
    PROCESS_MEMORY_COUNTERS c{};
    c.cb = sizeof(c);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c)))
        return -1;
    usage->ru_maxrss = long(c.PeakWorkingSetSize / 1024);
    return 0;
}

void* dlopen(const char* file, int)
{
    const std::wstring w = Wide(file);
    // A path's own folder first for the library's dependencies, as on Linux.
    const bool path = w.find_first_of(L"\\/") != std::wstring::npos;
    HMODULE m = LoadLibraryExW(w.c_str(), nullptr, path ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
    if (!m)
        t_dlerror = "LoadLibrary failed, error " + std::to_string(GetLastError());
    return reinterpret_cast<void*>(m);
}

void* dlsym(void* lib, const char* name)
{
    void* f = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
    if (!f)
        t_dlerror = std::string("no ") + name;
    return f;
}

int dlclose(void* lib)
{
    return FreeLibrary(static_cast<HMODULE>(lib)) ? 0 : -1;
}

const char* dlerror()
{
    return t_dlerror.c_str();
}

namespace winsys
{
    void* CreatePreciseTimer()
    {
        return CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }

    void ClosePreciseTimer(void* timer)
    {
        if (timer)
            CloseHandle(timer);
    }

    bool WaitPrecise(void* timer, int64_t ns)
    {
        // A negative due time is relative, in 100 ns units.
        LARGE_INTEGER due;
        due.QuadPart = -(ns / 100 > 0 ? ns / 100 : 1);
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            return false;
        return WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
    }

    void PreciseTimers()
    {
        RaiseTimerResolution();
        // Windows 11: keep the 1 ms timer while the window is occluded or
        // minimized, and no EcoQoS (slower cores, lower clocks) for a game
        // that is running in the background.
        PROCESS_POWER_THROTTLING_STATE state{};
        state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        state.ControlMask = 0x1 | 0x4;  // ..._EXECUTION_SPEED, ..._IGNORE_TIMER_RESOLUTION
        state.StateMask = 0;            // both off: full speed, the timer request honoured
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
    }

    bool WaitOnValue(void* address, uint32_t expected, int64_t timeoutNs)
    {
        const DWORD ms = timeoutNs <= 0 ? 0 : DWORD((timeoutNs + 999999) / 1000000);
        return WaitOnAddress(address, &expected, sizeof(expected), ms) != FALSE;
    }

    void WakeOne(void* address)
    {
        WakeByAddressSingle(address);
    }

    void WakeAll(void* address)
    {
        WakeByAddressAll(address);
    }

    bool RaiseTimerResolution()
    {
        // Loaded, not linked: the exe needn't import winmm.
        if (HMODULE winmm = LoadLibraryW(L"winmm.dll"))
        {
            using Fn = UINT(WINAPI*)(UINT);
            if (auto begin = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(winmm, "timeBeginPeriod"))))
                return begin(1) == 0;
        }
        return false;
    }

    bool JoinMmcssAudio()
    {
        if (HMODULE avrt = LoadLibraryW(L"avrt.dll"))
        {
            using Fn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
            if (auto set = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW"))))
            {
                DWORD task = 0;
                if (set(L"Pro Audio", &task))
                    return true;
                task = 0;
                return set(L"Audio", &task) != nullptr;
            }
        }
        return false;
    }

    void RaiseThreadPriority()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }

    namespace
    {
        std::string RegistryString(HKEY root, const char* key, const char* value)
        {
            const std::wstring k = Wide(key), v = Wide(value);
            DWORD size = 0;
            if (RegGetValueW(root, k.c_str(), v.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t))
                return {};
            std::wstring out(size / sizeof(wchar_t), L'\0');
            if (RegGetValueW(root, k.c_str(), v.c_str(), RRF_RT_REG_SZ, nullptr, out.data(), &size) != ERROR_SUCCESS)
                return {};
            out.resize(wcsnlen(out.c_str(), out.size()));
            return Utf8(out.c_str());
        }
    }

    std::string UserRegistryString(const char* key, const char* value)
    {
        return RegistryString(HKEY_CURRENT_USER, key, value);
    }

    std::string MachineRegistryString(const char* key, const char* value)
    {
        return RegistryString(HKEY_LOCAL_MACHINE, key, value);
    }

    std::string OsDescription()
    {
        // The real version (GetVersionEx lies to an exe without a manifest
        // that names Windows 10).
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof(v);
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
            if (auto get = reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion"))))
                get(&v);
        constexpr const char* kKey = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        std::string edition = MachineRegistryString(kKey, "EditionID");  // Professional, Core, ...
        if (edition == "Professional")
            edition = "Pro";
        else if (edition == "Core")
            edition = "Home";
        const std::string release = MachineRegistryString(kKey, "DisplayVersion");  // 24H2
        DWORD ubr = 0, size = sizeof(ubr);
        RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
        std::string text = v.dwMajorVersion == 10 ? (v.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10")
                                                  : "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion);
        if (!edition.empty())
            text += " " + edition;
        if (!release.empty())
            text += " " + release;
        text += " (build " + std::to_string(v.dwBuildNumber) + (ubr ? "." + std::to_string(ubr) : std::string()) + ")";
        return text;
    }

    unsigned PhysicalCores()
    {
        DWORD size = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
        std::string buffer(size, '\0');
        if (!size || !GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()), &size))
            return 0;
        unsigned cores = 0;
        for (DWORD at = 0; at < size;)
        {
            auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + at);
            cores += info->Relationship == RelationProcessorCore;
            at += info->Size;
        }
        return cores;
    }

    std::vector<std::string> LocalDriveRoots()
    {
        std::vector<std::string> out;
        const DWORD drives = GetLogicalDrives();
        for (int i = 0; i < 26; i++)
        {
            if (!(drives & (1u << i)))
                continue;
            const std::string root = std::string(1, char('A' + i)) + ":\\";
            const UINT type = GetDriveTypeA(root.c_str());
            if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE)
                out.push_back(root);
        }
        return out;
    }

    Memory MemoryStatus()
    {
        MEMORYSTATUSEX m{};
        m.dwLength = sizeof(m);
        Memory out;
        if (GlobalMemoryStatusEx(&m))
        {
            out.total = m.ullTotalPhys;
            out.available = m.ullAvailPhys;
            out.pageFile = m.ullTotalPageFile > m.ullTotalPhys ? m.ullTotalPageFile - m.ullTotalPhys : 0;
        }
        return out;
    }
}

// (sysconf, mprotect, the guest memory and the fault handlers: upstream's,
// platform/win32.)


ssize_t pread(int fd, void* buf, size_t count, long long offset)
{
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset & 0xFFFFFFFF);
    ov.OffsetHigh = DWORD(uint64_t(offset) >> 32);
    DWORD got = 0;
    if (!ReadFile(h, buf, DWORD(count), &got, &ov))
    {
        if (GetLastError() == ERROR_HANDLE_EOF)
            return 0;
        return -1;
    }
    return ssize_t(got);
}

ssize_t pwrite(int fd, const void* buf, size_t count, long long offset)
{
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    OVERLAPPED ov{};
    ov.Offset = DWORD(offset & 0xFFFFFFFF);
    ov.OffsetHigh = DWORD(uint64_t(offset) >> 32);
    DWORD put = 0;
    if (!WriteFile(h, buf, DWORD(count), &put, &ov))
        return -1;
    return ssize_t(put);
}

