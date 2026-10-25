// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Guest path -> host path.
//   \Device\Cdrom0\..., game:\..., d:\...     disc (GetGamePath()), read-only
//   cache:\..., \Device\Harddisk0\Cache0\...  <user>/cache
//   hdd:\..., \Device\Harddisk0\Partition1\...<user>/hdd
//   <root>:\...                               a root registered through XAM
//                                             (XamContentCreateEx: saves, ...)
// Lookups are case-insensitive, component by component, because the disc's
// names are upper case and Linux file systems are case-sensitive.
#pragma once
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace vfs
{
    // A guest file the game finds at `hostPath` instead of where its path
    // would lead (the dev start's online.cfg, game/online_cfg.cpp, from
    // AndhikaFW's fork). Case-insensitive; one at a time.
    void SetOverride(std::string_view guestPath, std::filesystem::path hostPath);

    struct Resolved
    {
        std::filesystem::path host;
        bool readOnly;   // the disc
        // The host folder of the XAM content root the path is under (a save
        // package), else empty. Files there are written atomically (below).
        std::filesystem::path contentRoot{};
    };

    // `mustExist` = false resolves the parent directory and keeps the last
    // component as given (for creating files).
    std::optional<Resolved> Resolve(std::string_view guestPath, bool mustExist);

    // Case-insensitive wildcard match (* and ?), as used by NtQueryDirectoryFile.
    bool WildcardMatch(std::string_view pattern, std::string_view name);

    // -----------------------------------------------------------------------
    // Saves are written atomically.
    //
    // A file in a content package (a save) is never rewritten where it is:
    // the guest's writes go to a temporary file beside it, which is flushed
    // to the disk and renamed over the old file once the guest is done with
    // it (its last handle closed; a copy when the package is flushed or
    // closed with it still open: StagedFile, below), and then the folder is
    // flushed so the rename itself survives a power cut. Until that rename
    // the old file is untouched, so a crash, a kill, a quit that ends the
    // process while the game is still saving, or a Steam Deck's battery
    // running out mid-save leaves the previous save rather than an empty or
    // cut-short one. (A rewrite in place after O_TRUNC has no such
    // guarantee: ext4's auto_da_alloc only narrows the window, and exFAT,
    // portable mode on an SD card, has nothing like it.) .xcontent files,
    // thumbnails and profile_settings.bin are written whole the same way
    // (WriteAtomically), as settings.toml is (user/settings.cpp).

    // The temporary files' suffix: <file>.<pid>.sbtmp.
    inline constexpr std::string_view kStagingSuffix = ".sbtmp";

    // The temporary file process `pid` writes `target`'s new contents into:
    // one per process, so two copies of the game saving the same file at
    // once never write into the same temporary file (the last rename wins,
    // a whole file either way).
    inline std::filesystem::path StagingPath(const std::filesystem::path& target, long pid)
    {
        std::filesystem::path temp = target;
        temp += "." + std::to_string(pid) + std::string(kStagingSuffix);
        return temp;
    }

    // Whether the file name `name` is such a temporary file, and the pid in
    // it. They are hidden from the guest's directory listings, where they
    // would show up as saves of their own.
    inline bool IsStagingName(std::string_view name, long* pid = nullptr)
    {
        if (!name.ends_with(kStagingSuffix))
            return false;
        name.remove_suffix(kStagingSuffix.size());
        size_t dot = name.rfind('.');
        if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size() || name.size() - dot - 1 > 10)
            return false;
        long value = 0;
        for (char c : name.substr(dot + 1))
        {
            if (c < '0' || c > '9')
                return false;
            value = value * 10 + (c - '0');
        }
        if (pid)
            *pid = value;
        return true;
    }

    // Writes all `size` bytes at `offset`, again after a short write or
    // EINTR, where a single pwrite would report the shortfall as success.
    // Returns 0 or the errno that stopped it (ENOSPC for a write that stores
    // nothing, a full disk).
    inline int WriteAll(int fd, const void* data, size_t size, uint64_t offset)
    {
        const char* p = static_cast<const char*>(data);
        size_t done = 0;
        while (done < size)
        {
            ssize_t n = pwrite(fd, p + done, size - done, off_t(offset + done));
            if (n > 0)
                done += size_t(n);
            else if (n < 0 && errno == EINTR)
                continue;
            else
                return n == 0 ? ENOSPC : errno;
        }
        return 0;
    }

    // Copies all of `from` into `to`, both from the start: a save opened for
    // writing without being truncated starts out as the old one, which the
    // guest may update a part of. Returns 0 or the errno.
    inline int CopyContents(int from, int to)
    {
        std::vector<char> buffer(size_t(1) << 16);
        uint64_t offset = 0;
        for (;;)
        {
            ssize_t n = pread(from, buffer.data(), buffer.size(), off_t(offset));
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                return errno;
            if (n == 0)
                return 0;
            if (int error = WriteAll(to, buffer.data(), size_t(n), offset))
                return error;
            offset += uint64_t(n);
        }
    }

    // Flushes `fd`'s data to the disk. fsync on macOS and iOS leaves it in
    // the drive's cache; F_FULLFSYNC doesn't (not every file system
    // supports it: then fsync). Returns 0 or the errno.
    inline int SyncFile(int fd)
    {
#ifdef __APPLE__
        if (fcntl(fd, F_FULLFSYNC) == 0)
            return 0;
#endif
        return fsync(fd) == 0 ? 0 : errno;
    }

    // Flushes a folder's entries (a rename or a new file in it). Best
    // effort: some file systems refuse fsync on a directory. Windows has no
    // such call: its rename (compat/win) writes through instead.
    inline void SyncDirectory(const std::filesystem::path& dir)
    {
#ifdef _WIN32
        (void)dir;
#else
        if (int fd = open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_CLOEXEC); fd >= 0)
        {
            fsync(fd);
            close(fd);
        }
#endif
    }

    // Puts `temp`, flushed already, in place of `target` and flushes the
    // folder. Returns 0 or the errno; on failure `temp` is still there, for
    // the caller to remove.
    inline int ReplaceDurably(const std::filesystem::path& temp, const std::filesystem::path& target)
    {
        if (rename(temp.c_str(), target.c_str()) != 0)
            return errno;
        SyncDirectory(target.parent_path());
        return 0;
    }

    // Removes the temporary files crashed writes left in `dir`: another
    // process's once a minute old (a save takes milliseconds, so a younger
    // one may be another copy of the game saving now). This process's own
    // are files it is writing right now, however long the guest keeps them
    // open; one left by a crashed process with the same pid is replaced by
    // the next write of that file.
    inline void RemoveStaleStaging(const std::filesystem::path& dir)
    {
        auto now = std::filesystem::file_time_type::clock::now();
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            std::string name = it->path().filename().string();
            long pid = 0;
            if (!IsStagingName(name, &pid) || pid == long(getpid()))
                continue;
            std::error_code timeError;
            auto written = std::filesystem::last_write_time(it->path(), timeError);
            if (timeError || now - written < std::chrono::minutes(1))
                continue;
            std::error_code removeError;
            if (std::filesystem::remove(it->path(), removeError))
                fprintf(stderr, "[vfs] removed %s, left by a save that didn't finish\n", name.c_str());
        }
    }

    // Writes the whole file `path` atomically: it is always either the old
    // file or all of the new one. Returns "" or why it failed (the old file
    // is kept then).
    inline std::string WriteAtomically(const std::filesystem::path& path, const void* data, size_t size)
    {
        // One write at a time: the temporary file's name is per process, so
        // two threads writing the same file would write into one temporary
        // file and rename a mix of both into place.
        static std::mutex mutex;
        std::lock_guard lock(mutex);
        std::filesystem::path dir = path.parent_path();
        if (!dir.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            if (ec)
                return "can't create " + dir.string() + ": " + ec.message();
        }
        RemoveStaleStaging(dir.empty() ? std::filesystem::path(".") : dir);
        std::filesystem::path temp = StagingPath(path, long(getpid()));
        int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
            return "can't write " + temp.string() + ": " + strerror(errno);
        int error = WriteAll(fd, data, size, 0);
        if (!error)
            error = SyncFile(fd);
        if (close(fd) != 0 && !error)
            error = errno;
        if (error)
        {
            unlink(temp.c_str());
            return "can't write " + temp.string() + ": " + strerror(error);
        }
        if (int failed = ReplaceDurably(temp, path))
        {
            unlink(temp.c_str());
            return "can't replace " + path.string() + ": " + strerror(failed);
        }
        return "";
    }

    // A file in a content package (a save) being rewritten. It was opened
    // with O_TRUNC and rewritten in place, so a quit, a crash or a power cut
    // mid-save left an empty or cut-short save. Now every handle the guest
    // has open on the file (imports_file.cpp's FileObject) reads and writes
    // this one temporary file through a dup of `fd`, so they all see the new
    // contents, and the last handle's close flushes it and renames it over
    // the old file. A flush or close of the package with the file still open
    // puts a copy of it in place, and the rewrite goes on, so writes after
    // that are as safe as the first ones.
    struct StagedFile
    {
        std::filesystem::path target;       // the file the guest opened
        std::filesystem::path temp;         // its new contents until the commit
        std::filesystem::path contentRoot;  // the package it is in
        int fd = -1;                        // the temporary file
        uint32_t handles = 0;
        // The guest was told a write failed: the old file is kept rather
        // than one with a hole in it or cut short.
        bool failed = false;
    };

    // The rewrites in progress, by target, from the first handle's open to
    // the last one's close.
    class Staging
    {
    public:
        // Held from looking a target up to opening it (NtCreateFile), so the
        // rewrite found can't be committed in between.
        std::mutex& Mutex() { return mutex_; }

        // Under Mutex(): the rewrite of `target` in progress, or null.
        std::shared_ptr<StagedFile> FindLocked(const std::filesystem::path& target) const
        {
            auto it = files_.find(target);
            return it != files_.end() ? it->second : nullptr;
        }

        // Under Mutex(): all of them, by target (new saves, which directory
        // listings show before they are on the disk).
        const std::map<std::filesystem::path, std::shared_ptr<StagedFile>>& FilesLocked() const { return files_; }

        // Under Mutex(): a new handle on the rewrite `staged` or, when that
        // is null, on a new rewrite of `target` in the package `contentRoot`,
        // which starts as a copy of the old file when `keep` (an open that
        // neither truncates nor creates it may update a part of it). Emptied
        // first when `truncate`. Returns the handle's descriptor (its own,
        // for it to close), or -1 with `error` set; `staged` is the rewrite.
        int OpenLocked(std::shared_ptr<StagedFile>& staged, const std::filesystem::path& target,
            const std::filesystem::path& contentRoot, bool keep, bool truncate, int& error)
        {
            error = 0;
            bool started = false;
            if (!staged)
            {
                staged = Start(target, contentRoot, keep, error);
                started = staged != nullptr;
            }
            else if (truncate)
            {
                if (ftruncate(staged->fd, 0) != 0)
                    error = errno;
                else
                    // Nothing a failed write left is in it any more: a
                    // rewrite whose writes all succeed from here commits.
                    staged->failed = false;
            }
            int fd = -1;
            if (staged && !error && (fd = fcntl(staged->fd, F_DUPFD_CLOEXEC, 0)) < 0)
                error = errno;
            if (error)
            {
                if (started)
                {
                    close(staged->fd);
                    unlink(staged->temp.c_str());
                    staged = nullptr;
                }
                return -1;
            }
            if (started)
                files_[target] = staged;
            staged->handles++;
            return fd;
        }

        // A handle on `staged` closed: the last one commits the rewrite, and
        // the target's next open opens the file at its path again.
        void Release(StagedFile& staged)
        {
            std::lock_guard lock(mutex_);
            if (--staged.handles != 0)
                return;
            CommitLocked(staged, true);
            if (auto it = files_.find(staged.target); it != files_.end() && it->second.get() == &staged)
                files_.erase(it);
            close(staged.fd);
            staged.fd = -1;
            closed_.notify_all();
        }

        // A write through a handle on `staged` failed (the guest was told).
        void MarkFailed(StagedFile& staged)
        {
            std::lock_guard lock(mutex_);
            staged.failed = true;
        }

        // Puts copies of the files of the package at `contentRoot` the guest
        // still has open in place of the old ones now (XamContentFlush,
        // XamContentClose). Their rewrites go on.
        void Commit(const std::filesystem::path& contentRoot)
        {
            std::lock_guard lock(mutex_);
            for (const auto& [target, staged] : files_)
                if (staged->contentRoot == contentRoot)
                    CommitLocked(*staged, false);
        }

        // Waits up to `timeout` for the last handles to close. True if none
        // is open; files still open keep their old contents.
        bool Wait(std::chrono::milliseconds timeout)
        {
            std::unique_lock lock(mutex_);
            if (files_.empty())
                return true;
            fprintf(stderr, "[vfs] waiting for the game to finish writing %zu save file(s)\n", files_.size());
            if (closed_.wait_for(lock, timeout, [this] { return files_.empty(); }))
                return true;
            fprintf(stderr, "[vfs] %zu save file(s) still open: their previous contents are kept\n", files_.size());
            return false;
        }

    private:
        // The temporary file of a new rewrite. Null, with `error` set, if it
        // can't be made.
        static std::shared_ptr<StagedFile> Start(const std::filesystem::path& target,
            const std::filesystem::path& contentRoot, bool keep, int& error)
        {
            auto staged = std::make_shared<StagedFile>();
            staged->target = target;
            staged->temp = StagingPath(target, long(getpid()));
            staged->contentRoot = contentRoot;
            RemoveStaleStaging(target.parent_path());
            staged->fd = open(staged->temp.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (staged->fd < 0)
            {
                error = errno;
                return nullptr;
            }
            if (keep)
            {
                int from = open(target.c_str(), O_RDONLY | O_CLOEXEC);
                error = from < 0 ? errno : CopyContents(from, staged->fd);
                if (from >= 0)
                    close(from);
                if (error)
                {
                    close(staged->fd);
                    unlink(staged->temp.c_str());
                    return nullptr;
                }
            }
            return staged;
        }

        // Puts the rewrite's contents in place of the target: flushed to the
        // disk, renamed over it, the folder flushed (ReplaceDurably). `last`
        // (the last handle closed): the temporary file itself, which is gone
        // afterwards either way. Else a copy of it (named by StagingPath
        // from the temporary file's name, as hidden as it is), and the
        // rewrite goes on. A rewrite with a failed write is never put in
        // place.
        static void CommitLocked(StagedFile& staged, bool last)
        {
            std::filesystem::path from = staged.temp;
            int error = staged.failed ? EIO : 0;
            if (!error && !last)
            {
                from = StagingPath(staged.temp, long(getpid()));
                int copy = open(from.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                error = copy < 0 ? errno : CopyContents(staged.fd, copy);
                if (!error)
                    error = SyncFile(copy);
                if (copy >= 0)
                    close(copy);
            }
            else if (!error)
            {
                error = SyncFile(staged.fd);
            }
            if (!error)
                error = ReplaceDurably(from, staged.target);
            if (error)
            {
                fprintf(stderr, "[vfs] %s not saved (%s): the previous file is kept\n", staged.target.string().c_str(),
                    staged.failed ? "a write to it failed" : strerror(error));
                if (last || !staged.failed)
                    unlink(from.c_str());  // (not the rewrite itself while it goes on)
            }
        }

        std::mutex mutex_;
        std::condition_variable closed_;
        std::map<std::filesystem::path, std::shared_ptr<StagedFile>> files_;
    };

    // The rewrites of the guest's saves. Never destroyed: guest threads can
    // still be closing files while the process exits.
    inline Staging& Saves()
    {
        static Staging* staging = new Staging;
        return *staging;
    }

    // Copies of the files of the content package at `contentRoot` that the
    // guest still has open put in place (XamContentFlush, XamContentClose).
    inline void CommitStagedFiles(const std::filesystem::path& contentRoot)
    {
        Saves().Commit(contentRoot);
    }

    // Waits up to `timeout` for the guest to close the files it is writing in
    // content packages, so that quitting during a save keeps the save the
    // game was about to finish. True if none is left open; the files of any
    // still open keep their old contents.
    inline bool WaitForStagedFiles(std::chrono::milliseconds timeout)
    {
        return Saves().Wait(timeout);
    }
}
