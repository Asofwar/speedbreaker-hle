// Adapted from Unleashed Recompiled (https://github.com/hedge-dev/UnleashedRecomp),
// GPL-3.0-or-later. Modified for SpeedBreaker: silent stubs and the input (hid/SDL)
// exports are removed; input returns in Phase 6 on our own SDL3 layer.
#include <stdafx.h>
#include "xam.h"
#include <online/online.h>
#include "vfs.h"
#include "xdm.h"
#include <cpu/guest_thread.h>
#include <ranges>
#include <unordered_set>
#include "xxHashMap.h"
#include <user/paths.h>
#include "overlapped.h"
#include <kernel/function.h>
#include <fstream>

namespace
{
    constexpr uint32_t XAM_ERROR_IO_PENDING = 0x3E5;
    constexpr uint32_t XAM_ERROR_ALREADY_EXISTS = 0xB7;
    constexpr uint32_t XAM_ERROR_FUNCTION_FAILED = 0x65B;

    // Asynchronous XAM calls (Xenia's CompleteOverlappedImmediate): with an
    // XOVERLAPPED the result goes there, its event is signalled, and the
    // call itself returns ERROR_IO_PENDING.
    uint32_t XamComplete(XXOVERLAPPED* overlapped, uint32_t result, uint32_t extended = 0, uint32_t length = 0)
    {
        if (!overlapped)
            return result;
        CompleteOverlapped(overlapped, result, extended, length);
        return XAM_ERROR_IO_PENDING;
    }
}

struct XamListener : KernelObject
{
    uint32_t id{};
    uint64_t areas{};
    std::vector<std::tuple<uint32_t, uint32_t>> notifications;

    XamListener(const XamListener&) = delete;
    XamListener& operator=(const XamListener&) = delete;

    XamListener();
    ~XamListener();
};

struct XamEnumeratorBase : KernelObject
{
    virtual uint32_t Next(void* buffer)
    {
        return -1;
    }
};

// Content enumeration hands out a copy of the registry taken when the
// enumerator is created. It used to keep iterators into the registry itself
// (an unordered_map), which dangled as soon as a save created before the
// enumerator was closed made the map rehash, and which another thread could
// change under it. `fetch` items per XamEnumerate call.
struct XamContentEnumerator : XamEnumeratorBase
{
    std::vector<XCONTENT_DATA> items;
    size_t next = 0;
    uint32_t fetch = 0;

    uint32_t Next(void* buffer) override
    {
        if (next == items.size())
            return -1;
        size_t count = std::min<size_t>(fetch, items.size() - next);
        // No buffer: skip the items (the old enumerator then went on to copy
        // them to the null buffer).
        if (buffer)
            memcpy(buffer, &items[next], count * sizeof(XCONTENT_DATA));
        next += count;
        return uint32_t(count);
    }
};

// The content packages known (saves found on the disk or created), by type,
// and the roots (save:, ...) open on them. Guest threads create, enumerate,
// open, delete and close content concurrently, and vfs::Resolve looks the
// roots up on every file open: all of it under gContentMutex.
std::mutex gContentMutex;
std::array<xxHashMap<XHOSTCONTENT_DATA>, 3> gContentRegistry{};
static void ScanSavedContentLocked();
std::unordered_set<XamListener*> gListeners{};
// Listeners and their queues: guest threads, and the main thread (the
// runtime's settings menu announces itself as system UI).
std::mutex gListenersMutex;
xxHashMap<std::string> gRootMap;

namespace
{
    constexpr uint32_t XAM_ERROR_INVALID_PARAMETER = 0x57;

    // The registry of a content type (XCONTENTTYPE_SAVEDATA 1, DLC 2,
    // PUBLISHER 3), or null for any other: the type comes from the guest,
    // and a 0 or a 4 indexed past the array.
    xxHashMap<XHOSTCONTENT_DATA>* ContentRegistry(uint32_t type)
    {
        return type >= 1 && type <= gContentRegistry.size() ? &gContentRegistry[type - 1] : nullptr;
    }

    // A package's file name, which has no terminating NUL when it fills all
    // of szFileName.
    std::string_view ContentName(const XCONTENT_DATA& data)
    {
        return std::string_view(data.szFileName, strnlen(data.szFileName, sizeof(data.szFileName)));
    }

    // Whether the guest's package name can name a package. An empty one made
    // ContentRoot the save folder itself, so deleting that "package" deleted
    // every save; "." and ".." name no package either. Separators and colons
    // can't leave the save folder (ContentRoot turns them into '_'), so
    // names with them keep working as they always have.
    bool ValidContentName(std::string_view name)
    {
        return !name.empty() && name != "." && name != "..";
    }

    void RegisterContentLocked(const XCONTENT_DATA& data, const std::string_view& root)
    {
        auto* registry = ContentRegistry(data.dwContentType);
        if (registry == nullptr)
            return;
        registry->emplace(StringHash(ContentName(data)), XHOSTCONTENT_DATA{ data }).first->second.szRoot = root;
    }
}

std::string XamGetRootPath(const std::string_view& root)
{
    std::lock_guard lock(gContentMutex);
    const auto result = gRootMap.find(StringHash(root));

    if (result == gRootMap.end())
        return "";

    return result->second;
}

void XamRootCreate(const std::string_view& root, const std::string_view& path)
{
    std::lock_guard lock(gContentMutex);
    gRootMap.emplace(StringHash(root), path);
}

XamListener::XamListener()
{
    std::lock_guard lock(gListenersMutex);
    gListeners.insert(this);
}

XamListener::~XamListener()
{
    std::lock_guard lock(gListenersMutex);
    gListeners.erase(this);
}

XCONTENT_DATA XamMakeContent(uint32_t type, const std::string_view& name)
{
    XCONTENT_DATA data{ 1, type };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    return data;
}

void XamRegisterContent(const XCONTENT_DATA& data, const std::string_view& root)
{
    std::lock_guard lock(gContentMutex);
    RegisterContentLocked(data, root);
}

void XamRegisterContent(uint32_t type, const std::string_view name, const std::string_view& root)
{
    XCONTENT_DATA data{ 1, type, {}, "" };

    strncpy(data.szFileName, name.data(), sizeof(data.szFileName));

    XamRegisterContent(data, root);
}

const char* XamGamertag()
{
    return online::Gamertag().c_str();
}

uint64_t XamXuid()
{
    static const uint64_t xuid = [] {
        const std::string name = XamGamertag();
        if (name == "Player")
            return 0xB13EBABEBABEBABEull;
        uint64_t h = 0xCBF29CE484222325ull;  // FNV-1a of the gamertag
        for (char c : name)
            h = (h ^ uint8_t(std::tolower(uint8_t(c)))) * 0x100000001B3ull;
        return 0xB13E000000000000ull | (h & 0x0000FFFFFFFFFFFFull);
    }();
    return xuid;
}

uint32_t XamNotifyCreateListener(uint64_t qwAreas)
{
    auto* listener = CreateKernelObject<XamListener>();

    listener->areas = qwAreas;

    static const bool logNotify = std::getenv("NFSMW_NET_LOG") != nullptr;
    if (logNotify)
        fprintf(stderr, "[net] XamNotifyCreateListener(areas %llX) = %08X\n", (unsigned long long)qwAreas, GetKernelHandle(listener));
    return GetKernelHandle(listener);
}

// NFSMW_FAKE_LIVE=1 (a probe of the game's own online flow, see kernel/imports_user.cpp): called when the game asks for the multiplayer privilege,
// i.e. when the player opens Xbox Live. The listeners that cover the right areas are told, as a console's are at connection, that the user
// signed in and that Xbox Live is connected and linked. (The message's area is bits 25-30 of its id.)
void XamNotifyLiveConnected()
{
    static bool done = false;
    if (done)
        return;
    std::lock_guard lock(gListenersMutex);
    int delivered = 0;
    auto send = [&](uint32_t id, uint32_t param) {
        int n = 0;
        for (const auto& listener : gListeners)
            if ((1ull << ((id >> 25) & 0x3F)) & listener->areas)
            {
                listener->notifications.emplace_back(id, param);
                n++;
            }
        delivered += n;
        fprintf(stderr, "[live] notification %08X (%08X) queued for %d listeners\n", id, param, n);
    };
    send(0x0000000A, 1);          // XN_SYS_SIGNINCHANGED: one user signed in
    send(0x02000001, 0x001510F0);  // XN_LIVE_CONNECTIONCHANGED: XONLINE_S_LOGON_CONNECTION_ESTABLISHED
    send(0x02000002, 1);          // XN_LIVE_LINK_STATE_CHANGED: linked
    done = delivered > 0;          // until a listener exists to hear it, try again at the next call
}

void XamNotifyEnqueueEvent(uint32_t dwId, uint32_t dwParam)
{
    std::lock_guard lock(gListenersMutex);
    for (const auto& listener : gListeners)
    {
        if (((1 << MSG_AREA(dwId)) & listener->areas) == 0)
            continue;

        listener->notifications.emplace_back(dwId, dwParam);
    }
}

bool XNotifyGetNext(uint32_t hNotification, uint32_t dwMsgFilter, be<uint32_t>* pdwId, be<uint32_t>* pParam)
{
    static const bool logNotify = std::getenv("NFSMW_NET_LOG") != nullptr;
    std::lock_guard lock(gListenersMutex);
    // Only a live listener's handle is looked at: one that is not (0, or one
    // read from memory something wrote over) has nothing to deliver, where it
    // used to be dereferenced.
    XamListener* found = nullptr;
    if (hNotification && hNotification != GUEST_INVALID_HANDLE_VALUE)
        for (auto* l : gListeners)
            if (GetKernelHandle(l) == hNotification)
                found = l;
    if (!found)
    {
        static std::atomic<bool> warned{ false };
        if (!warned.exchange(true))
            fprintf(stderr, "[xam] XNotifyGetNext(handle %08X): not a listener, nothing to deliver\n", hNotification);
        return false;
    }
    auto& listener = *found;

    if (dwMsgFilter)
    {
        for (size_t i = 0; i < listener.notifications.size(); i++)
        {
            if (std::get<0>(listener.notifications[i]) == dwMsgFilter)
            {
                if (pdwId)
                    *pdwId = std::get<0>(listener.notifications[i]);

                if (pParam)
                    *pParam = std::get<1>(listener.notifications[i]);

                listener.notifications.erase(listener.notifications.begin() + i);
                if (logNotify)
                    fprintf(stderr, "[net] XNotifyGetNext(filter %08X) delivers a notification\n", dwMsgFilter);
                return true;
            }
        }
    }
    else
    {
        if (listener.notifications.empty())
            return false;

        if (pdwId)
            *pdwId = std::get<0>(listener.notifications[0]);

        if (pParam)
            *pParam = std::get<1>(listener.notifications[0]);

        if (logNotify)
            fprintf(stderr, "[net] XNotifyGetNext(listener %08X) delivers %08X (%08X)\n", hNotification, std::get<0>(listener.notifications[0]),
                std::get<1>(listener.notifications[0]));
        listener.notifications.erase(listener.notifications.begin());

        return true;
    }

    return false;
}

uint32_t XamShowMessageBoxUI(uint32_t dwUserIndex, be<uint16_t>* wszTitle, be<uint16_t>* wszText, uint32_t cButtons,
    xpointer<be<uint16_t>>* pwszButtons, uint32_t dwFocusButton, uint32_t dwFlags, be<uint32_t>* pResult, XXOVERLAPPED* pOverlapped)
{
    *pResult = cButtons ? cButtons - 1 : 0;

#if _DEBUG
    assert("XamShowMessageBoxUI encountered!" && false);
#elif _WIN32
    // SpeedBreaker: the 360 shows a message box (no UI for one yet): log it.
    // Its strings are big-endian UTF-16, which Windows' wchar_t holds after a
    // swap (elsewhere wchar_t is 4 bytes, so only Windows logs them).
    auto text = [](const be<uint16_t>* guest) {
        std::wstring wide;
        for (; guest && guest->get() != 0; guest++)
            wide.push_back(wchar_t(guest->get()));
        int n = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), nullptr, 0, nullptr, nullptr);
        std::string out(size_t(std::max(n, 0)), '\0');
        if (n > 0)
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), out.data(), n, nullptr, nullptr);
        return out;
    };
    std::string buttons;
    for (size_t i = 0; i < cButtons; i++)
        buttons += (i ? " | " : "") + text(pwszButtons[i].get());
    fprintf(stderr, "[xam] the game showed a message box, answered with button %u: \"%s\": \"%s\" [%s]\n",
        uint32_t(pResult->get()), text(wszTitle).c_str(), text(wszText).c_str(), buttons.c_str());
#endif

    XamNotifyEnqueueEvent(9, 0);

    return XamComplete(pOverlapped, ERROR_SUCCESS);
}

uint32_t XamContentCreateEnumerator(uint32_t dwUserIndex, uint32_t DeviceID, uint32_t dwContentType,
    uint32_t dwContentFlags, uint32_t cItem, be<uint32_t>* pcbBuffer, be<uint32_t>* phEnum)
{
    if (dwUserIndex != 0)
    {
        GuestThread::SetLastError(ERROR_NO_SUCH_USER);
        return 0xFFFFFFFF;
    }

    std::vector<XCONTENT_DATA> items;
    {
        std::lock_guard lock(gContentMutex);
        ScanSavedContentLocked();
        // Another content type has no packages: an empty enumeration.
        if (const auto* registry = ContentRegistry(dwContentType))
            for (const auto& content : *registry | std::views::values)
                items.push_back(content);
    }
    auto* enumerator = CreateKernelObject<XamContentEnumerator>();
    enumerator->items = std::move(items);
    enumerator->fetch = cItem;

    if (pcbBuffer)
        *pcbBuffer = sizeof(_XCONTENT_DATA) * cItem;

    *phEnum = GetKernelHandle(enumerator);

    return 0;
}

uint32_t XamEnumerate(uint32_t hEnum, uint32_t dwFlags, void* pvBuffer, uint32_t cbBuffer, be<uint32_t>* pcItemsReturned, XXOVERLAPPED* pOverlapped)
{
    auto* enumerator = GetKernelObject<XamEnumeratorBase>(hEnum);
    const auto count = enumerator->Next(pvBuffer);

    if (count == -1)
    {
        if (pcItemsReturned)
            *pcItemsReturned = 0;
        return pOverlapped ? XamComplete(pOverlapped, XAM_ERROR_FUNCTION_FAILED, 0x80070000 | ERROR_NO_MORE_FILES)
                           : ERROR_NO_MORE_FILES;
    }

    if (pcItemsReturned)
        *pcItemsReturned = count;

    return XamComplete(pOverlapped, ERROR_SUCCESS, 0, count);
}

// Content packages (saves) live in one directory each under the save path,
// with the package's XCONTENT_DATA beside it as <name>.xcontent so saves
// are found again (and enumerated with their display names) after a restart.
static std::filesystem::path ContentRoot(const XCONTENT_DATA& data)
{
    if (data.dwContentType == XCONTENTTYPE_SAVEDATA)
    {
        std::string name(data.szFileName, strnlen(data.szFileName, sizeof(data.szFileName)));
        for (char& c : name)
            if (c == '/' || c == '\\' || c == ':' || (c == '.' && &c == &name[0]))
                c = '_';
        return GetSavePath(true) / name;
    }
    if (data.dwContentType == XCONTENTTYPE_DLC)
        return GetGamePath() / "dlc";
    return GetGamePath();
}

// Under gContentMutex.
static void ScanSavedContentLocked()
{
    static bool scanned = false;
    if (scanned)
        return;
    scanned = true;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(GetSavePath(true), ec))
    {
        if (entry.path().extension() != ".xcontent")
            continue;
        XCONTENT_DATA data{};
        std::ifstream f(entry.path(), std::ios::binary);
        if (!f.read(reinterpret_cast<char*>(&data), sizeof(data)) || data.dwContentType != XCONTENTTYPE_SAVEDATA)
            continue;
        // (Not a name XamContentCreateEx refuses now: the guest couldn't
        // open or delete it anyway.)
        if (!ValidContentName(ContentName(data)))
            continue;
        std::filesystem::path root = ContentRoot(data);
        if (!std::filesystem::is_directory(root, ec))
            continue;
        RegisterContentLocked(data, (const char*)root.u8string().c_str());
        fprintf(stderr, "[xam] found save \"%.*s\"\n", int(strnlen(data.szFileName, sizeof(data.szFileName))), data.szFileName);
    }
}

// XamContentCreateEx's work, under gContentMutex: the check for the package
// and its creation under one lock, so two threads creating the same package
// can't both make it. The type and name are valid. Returns the result.
static uint32_t CreateContentLocked(const char* szRootName, const XCONTENT_DATA* pContentData, uint32_t mode,
    be<uint32_t>* pdwDisposition)
{
    ScanSavedContentLocked();
    auto& registry = *ContentRegistry(pContentData->dwContentType);
    const auto existing = registry.find(StringHash(ContentName(*pContentData)));
    const bool exists = existing != registry.end();

    if (exists && mode == 1)
        return XAM_ERROR_ALREADY_EXISTS;
    if (!exists && (mode == OPEN_EXISTING || mode == 5))
    {
        if (pdwDisposition)
            *pdwDisposition = XCONTENT_NEW;
        return ERROR_PATH_NOT_FOUND;
    }
    if (mode < 1 || mode > 5)
        return ERROR_PATH_NOT_FOUND;

    if (exists)
    {
        if (pdwDisposition)
            *pdwDisposition = XCONTENT_EXISTING;
        gRootMap.emplace(StringHash(szRootName), existing->second.szRoot);
        return ERROR_SUCCESS;
    }

    std::filesystem::path root = ContentRoot(*pContentData);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (pContentData->dwContentType == XCONTENTTYPE_SAVEDATA)
    {
        // A save package that can't be made on the disk (full, read only)
        // is an error the game reports, not a package that is gone after a
        // restart. The .xcontent file is written atomically (kernel/vfs.h),
        // so a crash leaves no cut-short one behind.
        // (From a copy: the kernel can't fault in a guest page the GPU guard
        // protects, it fails the write with EFAULT; a plain copy faults and
        // is handled.)
        std::filesystem::path meta = root;
        meta += ".xcontent";
        const XCONTENT_DATA data = *pContentData;
        std::string error = ec ? "can't create " + root.string() + ": " + ec.message()
                               : vfs::WriteAtomically(meta, &data, sizeof(data));
        if (!error.empty())
        {
            fprintf(stderr, "[xam] creating save \"%.*s\" failed: %s\n", int(ContentName(*pContentData).size()),
                pContentData->szFileName, error.c_str());
            return XAM_ERROR_FUNCTION_FAILED;
        }
    }
    const std::string rootString = (const char*)root.u8string().c_str();
    RegisterContentLocked(*pContentData, rootString);
    gRootMap.emplace(StringHash(szRootName), rootString);
    if (pdwDisposition)
        *pdwDisposition = XCONTENT_NEW;
    return ERROR_SUCCESS;
}

uint32_t XamContentCreateEx(uint32_t dwUserIndex, const char* szRootName, const XCONTENT_DATA* pContentData,
    uint32_t dwContentFlags, be<uint32_t>* pdwDisposition, be<uint32_t>* pdwLicenseMask,
    uint32_t dwFileCacheSize, uint64_t uliContentSize, PXXOVERLAPPED pOverlapped)
{
    const auto mode = dwContentFlags & 0xF;  // CREATE_NEW 1, CREATE_ALWAYS 2, OPEN_EXISTING 3, OPEN_ALWAYS 4, TRUNCATE_EXISTING 5

    if (pdwLicenseMask)
        *pdwLicenseMask = 0xFFFFFFFF;

    if (ContentRegistry(pContentData->dwContentType) == nullptr || !ValidContentName(ContentName(*pContentData)))
        return XamComplete(pOverlapped, XAM_ERROR_INVALID_PARAMETER);

    uint32_t result;
    {
        std::lock_guard lock(gContentMutex);
        result = CreateContentLocked(szRootName, pContentData, mode, pdwDisposition);
    }
    return XamComplete(pOverlapped, result);
}

uint32_t XamContentClose(const char* szRootName, XXOVERLAPPED* pOverlapped)
{
    std::string root;
    {
        std::lock_guard lock(gContentMutex);
        if (auto it = gRootMap.find(StringHash(szRootName)); it != gRootMap.end())
        {
            root = std::move(it->second);
            gRootMap.erase(it);
        }
    }
    // Files of the package the guest still has open are put in place now,
    // since the guest is done with the package (outside gContentMutex: this
    // flushes to the disk, which nothing else should wait for).
    if (!root.empty())
        vfs::CommitStagedFiles(root);
    return XamComplete(pOverlapped, ERROR_SUCCESS);
}

uint32_t XamContentGetDeviceData(uint32_t DeviceID, XDEVICE_DATA* pDeviceData)
{
    pDeviceData->DeviceID = DeviceID;
    pDeviceData->DeviceType = XCONTENTDEVICETYPE_HDD;
    pDeviceData->ulDeviceBytes = 0x10000000;
    pDeviceData->ulDeviceFreeBytes = 0x10000000;
    const char* name = "Hard Drive";
    for (size_t i = 0; i <= strlen(name); i++)
        pDeviceData->wszName[i] = uint16_t(name[i]);

    return 0;
}




// ---------------------------------------------------------------------------
// More content exports (SpeedBreaker; semantics from Xenia's xam_content.cc).
// The only device is the hard drive, id 1.

namespace
{
    constexpr uint32_t XAM_ERROR_FILE_NOT_FOUND = 0x2;
    constexpr uint32_t XAM_ERROR_INSUFFICIENT_BUFFER = 0x7A;
    constexpr uint32_t XAM_ERROR_DEVICE_NOT_CONNECTED = 0x48F;

    std::filesystem::path ThumbnailPath(const XCONTENT_DATA& data)
    {
        std::filesystem::path p = ContentRoot(data);
        p += ".thumbnail.png";
        return p;
    }
}

uint32_t XamContentGetDeviceName(uint32_t deviceId, be<uint16_t>* name, uint32_t capacity)
{
    if ((deviceId & 0xFFFF) != 1)
        return XAM_ERROR_DEVICE_NOT_CONNECTED;
    const char* text = "Hard Drive";
    size_t n = strlen(text);
    if (capacity < n + 1)
        return XAM_ERROR_INSUFFICIENT_BUFFER;
    for (size_t i = 0; i < n; i++)
        name[i] = uint16_t(text[i]);
    name[n] = 0;
    return ERROR_SUCCESS;
}

uint32_t XamContentCreate(uint32_t userIndex, const char* rootName, const XCONTENT_DATA* data, uint32_t flags,
    be<uint32_t>* disposition, be<uint32_t>* licenseMask, XXOVERLAPPED* overlapped)
{
    return XamContentCreateEx(userIndex, rootName, data, flags, disposition, licenseMask, 0, 0, overlapped);
}

// XamContentDelete's work, under gContentMutex. False: no such package.
static bool DeleteContentLocked(xxHashMap<XHOSTCONTENT_DATA>& registry, const XCONTENT_DATA* data)
{
    ScanSavedContentLocked();
    auto it = registry.find(StringHash(ContentName(*data)));
    if (it == registry.end())
        return false;
    std::error_code ec;
    std::filesystem::path root = ContentRoot(*data);
    if (data->dwContentType == XCONTENTTYPE_SAVEDATA)
    {
        std::filesystem::remove_all(root, ec);
        std::filesystem::path meta = root;
        meta += ".xcontent";
        std::filesystem::remove(meta, ec);
        std::filesystem::remove(ThumbnailPath(*data), ec);
    }
    registry.erase(it);
    return true;
}

uint32_t XamContentDelete(uint32_t userIndex, const XCONTENT_DATA* data, XXOVERLAPPED* overlapped)
{
    // An empty name's root is the save folder itself: remove_all deleted
    // every save.
    auto* registry = ContentRegistry(data->dwContentType);
    if (registry == nullptr || !ValidContentName(ContentName(*data)))
        return XamComplete(overlapped, XAM_ERROR_INVALID_PARAMETER);
    bool found;
    {
        std::lock_guard lock(gContentMutex);
        found = DeleteContentLocked(*registry, data);
    }
    if (!found)
        return XamComplete(overlapped, XAM_ERROR_FILE_NOT_FOUND);
    fprintf(stderr, "[xam] deleted content \"%.*s\"\n", int(strnlen(data->szFileName, sizeof(data->szFileName))), data->szFileName);
    return XamComplete(overlapped, ERROR_SUCCESS);
}

// Puts the files of the package the guest still has open in place (closed
// ones were when they were closed): what a flush promises, the package's
// writes on the disk.
uint32_t XamContentFlush(const char* rootName, XXOVERLAPPED* overlapped)
{
    if (std::string root = XamGetRootPath(rootName); !root.empty())
        vfs::CommitStagedFiles(root);
    return XamComplete(overlapped, ERROR_SUCCESS);
}

uint32_t XamContentGetLicenseMask(be<uint32_t>* mask, XXOVERLAPPED* overlapped)
{
    if (mask)
        *mask = 0xFFFFFFFF;
    return XamComplete(overlapped, ERROR_SUCCESS);
}

uint32_t XamContentGetThumbnail(uint32_t userIndex, const XCONTENT_DATA* data, uint8_t* buffer, be<uint32_t>* bufferSize,
    XXOVERLAPPED* overlapped)
{
    if (ContentRegistry(data->dwContentType) == nullptr || !ValidContentName(ContentName(*data)))
        return XamComplete(overlapped, XAM_ERROR_INVALID_PARAMETER);
    std::ifstream f(ThumbnailPath(*data), std::ios::binary | std::ios::ate);
    if (!f)
        return XamComplete(overlapped, XAM_ERROR_FILE_NOT_FOUND);
    uint32_t size = uint32_t(f.tellg());
    uint32_t capacity = bufferSize ? uint32_t(*bufferSize) : 0;
    if (bufferSize)
        *bufferSize = size;
    if (!buffer || capacity < size)
        return XamComplete(overlapped, buffer ? XAM_ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buffer), size);
    return XamComplete(overlapped, ERROR_SUCCESS);
}

uint32_t XamContentSetThumbnail(uint32_t userIndex, const XCONTENT_DATA* data, const uint8_t* buffer, uint32_t size,
    XXOVERLAPPED* overlapped)
{
    if (ContentRegistry(data->dwContentType) == nullptr || !ValidContentName(ContentName(*data)))
        return XamComplete(overlapped, XAM_ERROR_INVALID_PARAMETER);
    // Atomically (kernel/vfs.h), and a failure reported: the unchecked
    // ofstream left an empty or cut-short image on a full disk or a crash.
    // From a copy, as the .xcontent file (CreateContentLocked).
    const std::vector<uint8_t> image(buffer, buffer + size);
    if (std::string error = vfs::WriteAtomically(ThumbnailPath(*data), image.data(), image.size()); !error.empty())
    {
        fprintf(stderr, "[xam] saving a thumbnail failed: %s\n", error.c_str());
        return XamComplete(overlapped, XAM_ERROR_FUNCTION_FAILED);
    }
    return XamComplete(overlapped, ERROR_SUCCESS);
}

// Voice: no headset, no voice objects (Xenia).
uint32_t XamVoiceHeadsetPresent(uint32_t voice) { return 0; }
uint32_t XamVoiceCreate(uint32_t unk1, uint32_t unk2, be<uint32_t>* out) { if (out) *out = 0; return 5; /* ERROR_ACCESS_DENIED */ }
uint32_t XamVoiceClose(uint32_t voice) { return 0; }
uint32_t XamVoiceSubmitPacket(uint32_t voice, uint32_t size, uint32_t data) { return 0; }

// Sign-in UI: user 0 is always signed in; show and close the "UI" at once.
uint32_t XamShowSigninUI(uint32_t paneCount, uint32_t flags)
{
    XamNotifyEnqueueEvent(9, 1);
    XamNotifyEnqueueEvent(9, 0);
    return ERROR_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__XamContentGetDeviceName, XamContentGetDeviceName);
GUEST_FUNCTION_HOOK(__imp__XamContentCreate, XamContentCreate);
GUEST_FUNCTION_HOOK(__imp__XamContentDelete, XamContentDelete);
GUEST_FUNCTION_HOOK(__imp__XamContentFlush, XamContentFlush);
GUEST_FUNCTION_HOOK(__imp__XamContentGetLicenseMask, XamContentGetLicenseMask);
GUEST_FUNCTION_HOOK(__imp__XamContentGetThumbnail, XamContentGetThumbnail);
GUEST_FUNCTION_HOOK(__imp__XamContentSetThumbnail, XamContentSetThumbnail);
GUEST_FUNCTION_HOOK(__imp__XamVoiceHeadsetPresent, XamVoiceHeadsetPresent);
GUEST_FUNCTION_HOOK(__imp__XamVoiceCreate, XamVoiceCreate);
GUEST_FUNCTION_HOOK(__imp__XamVoiceClose, XamVoiceClose);
GUEST_FUNCTION_HOOK(__imp__XamVoiceSubmitPacket, XamVoiceSubmitPacket);
GUEST_FUNCTION_HOOK(__imp__XamShowSigninUI, XamShowSigninUI);
