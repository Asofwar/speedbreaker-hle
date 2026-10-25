// SpeedBreaker Windows port: the POSIX odds and ends the runtime uses that
// mingw-w64 lacks, as stubs or thin wrappers. Force-included (-include) into
// every runtime source on Windows. Deliberately does NOT include <windows.h>:
// its macros (FILE_OPEN, DUPLICATE_CLOSE_SOURCE, RtlCompareMemory, ...)
// collide with the Xbox 360 kernel names the runtime implements.
// GPL-3.0-or-later.
#pragma once
#ifdef _WIN32
// Since upstream's Windows port (v0.1.9 Windows beta 1) the runtime takes
// <windows.h> in every source, as xbox.h includes it: no longer kept out.
#ifdef __cplusplus
#include <csignal>
#include <cerrno>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef SIGBUS
#define SIGBUS 7
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGTRAP
#define SIGTRAP 5
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO 4
#define SA_RESTART 0x10000000
#define SA_RESETHAND 0x80000000
#endif
#ifndef SIG_BLOCK
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif
#ifndef ESRCH
#define ESRCH 3
#endif

// A bit of its own (Linux's value), for sockets' fcntl (compat/win/sb_net.h);
// open() leaves it out.
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
#ifndef EDQUOT
#define EDQUOT 122
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGURG
#define SIGURG 23
#endif
#define F_GETFL 3
#define F_SETFL 4
#define F_SETFD 2
#define FD_CLOEXEC 1
#define F_DUPFD_CLOEXEC 1030

// 64-bit file sizes: mingw's plain struct stat has a 32-bit st_size, which
// can't describe a disc image.
#define stat _stat64
#define fstat _fstat64

#ifdef __cplusplus
#include <cstdarg>
#include <cwchar>
#include <string>
extern "C++" {
#ifndef _NTDEF_
typedef long NTSTATUS;
#endif
typedef unsigned long long sigset_t;
typedef struct siginfo_t { void* si_addr; int si_code; int si_signo; } siginfo_t;
struct sigaction
{
    union { void (*sa_handler)(int); void (*sa_sigaction)(int, siginfo_t*, void*); };
    int sa_flags;
    sigset_t sa_mask;
};
inline int sigemptyset(sigset_t* s) { *s = 0; return 0; }
inline int sigaddset(sigset_t*, int) { return 0; }
inline int sigaction(int, const struct sigaction*, struct sigaction* old)
{
    if (old) { old->sa_handler = SIG_DFL; old->sa_flags = 0; old->sa_mask = 0; }
    return 0;
}
inline int kill(int, int) { errno = ESRCH; return -1; }
inline int fsync(int fd) { return _commit(fd); }
inline int pipe(int fds[2]) { return _pipe(fds, 1 << 16, _O_BINARY); }
namespace sbnet
{
    int Fcntl(int fd, int cmd, long arg);  // compat/win/win_net.cpp
}
inline int fcntl(int fd, int cmd, ...)
{
    long arg = 0;
    if (cmd == F_SETFL || cmd == F_DUPFD_CLOEXEC)
    {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, int);
        va_end(ap);
    }
    if (cmd == F_GETFL || cmd == F_SETFL)
        return sbnet::Fcntl(fd, cmd, arg);
    if (cmd == F_DUPFD_CLOEXEC) return dup(fd);
    return 0;
}
// Windows paths are wide: std::filesystem::path::c_str() is a wchar_t*.
inline int _stat64(const wchar_t* p, struct _stat64* st) { return _wstat64(p, st); }
inline int open(const wchar_t* p, int flags, int mode = 0) { return _wopen(p, (flags & ~O_NONBLOCK) | _O_BINARY, mode); }
inline int open(const char* p, int flags, int mode = 0) { return _open(p, (flags & ~O_NONBLOCK) | _O_BINARY, mode); }
inline FILE* fopen(const wchar_t* p, const char* mode)
{
    std::wstring w;
    for (const char* c = mode; *c; ++c) w.push_back(wchar_t(*c));
    return _wfopen(p, w.c_str());
}
inline int access(const wchar_t* p, int mode) { return _waccess(p, mode & 6); }
inline int mkdir(const wchar_t* p, int) { return _wmkdir(p); }
inline int unlink(const wchar_t* p) { return _wunlink(p); }
// POSIX's rename: replaces `b` if it is there (_wrename fails then, and every
// atomic save after the first would). compat/win/win_compat.cpp.
int rename(const wchar_t* a, const wchar_t* b);
inline int remove(const wchar_t* p) { return _wremove(p); }
ssize_t pread(int fd, void* buf, size_t count, long long offset);
ssize_t pwrite(int fd, const void* buf, size_t count, long long offset);
#define LOCK_EX 2
#define LOCK_NB 4
inline int flock(int, int) { return 0; }
struct ucontext_t { int unused; };
inline struct tm* localtime_r(const time_t* t, struct tm* out) { return localtime_s(out, t) == 0 ? out : nullptr; }
inline struct tm* gmtime_r(const time_t* t, struct tm* out) { return gmtime_s(out, t) == 0 ? out : nullptr; }
inline int setenv(const char* name, const char* value, int overwrite)
{
    if (!overwrite && std::getenv(name))
        return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
inline int unsetenv(const char* name) { return _putenv_s(name, "") == 0 ? 0 : -1; }
}
#endif
#endif
