// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See fetch.h.
//
// Windows: WinHTTP (winhttp.dll is part of every Windows, nothing to bundle).
//   - The system's proxy settings apply (WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY),
//     as they do on macOS.
//   - No cookies, no redirects followed (the API answers 200 or an error),
//     no Windows credentials sent (autologon policy high).
//   - WinHttpSetTimeouts is per phase, so the whole exchange also has its own
//     deadline: a watchdog thread closes the request handle at timeoutSeconds,
//     which makes the blocking call in progress fail (ERROR_WINHTTP_OPERATION_
//     CANCELLED, taken as Timeout).
//   - The status line and headers (WINHTTP_QUERY_RAW_HEADERS_CRLF) and the
//     body are joined as curl prints them, so ParseHttpResponse reads them
//     as it does on Linux (x-ratelimit-* included).
#ifdef _WIN32
#include "fetch.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>

namespace update
{
    namespace
    {
        std::wstring Widen(const std::string& text)
        {
            if (text.empty())
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
            std::wstring out(size_t(n > 0 ? n : 0), L'\0');
            if (n > 0)
                MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
            return out;
        }

        std::string Narrow(const wchar_t* text, size_t length)
        {
            if (!length)
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, text, int(length), nullptr, 0, nullptr, nullptr);
            std::string out(size_t(n > 0 ? n : 0), '\0');
            if (n > 0)
                WideCharToMultiByte(CP_UTF8, 0, text, int(length), out.data(), n, nullptr, nullptr);
            return out;
        }

        Transport FromError(DWORD error)
        {
            switch (error)
            {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            case ERROR_WINHTTP_CANNOT_CONNECT:
            case ERROR_WINHTTP_CONNECTION_ERROR:
                return Transport::Offline;
            case ERROR_WINHTTP_TIMEOUT:
            case ERROR_WINHTTP_OPERATION_CANCELLED:
                return Transport::Timeout;
            case ERROR_WINHTTP_SECURE_FAILURE:
                return Transport::Secure;
            case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
                return Transport::Garbled;
            default:
                return Transport::Failed;
            }
        }

        struct Handle
        {
            HINTERNET h = nullptr;
            ~Handle()
            {
                if (h)
                    WinHttpCloseHandle(h);
            }
        };
    }

    Fetched Fetch(const Request& request)
    {
        Fetched result;
        auto fail = [&](const char* step) {
            const DWORD error = GetLastError();
            result.transport = FromError(error);
            result.detail = std::format("{}: WinHTTP error {}", step, error);
        };

        const std::wstring url = Widen(request.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = DWORD(-1);
        parts.dwUrlPathLength = DWORD(-1);
        parts.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts))
            return fail("the URL"), result;
        const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        if (!secure && !(request.allowHttp && parts.nScheme == INTERNET_SCHEME_HTTP))
        {
            result.transport = Transport::Failed;
            result.detail = "not an https URL";
            return result;
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        if (path.empty())
            path = L"/";

        Handle session{ WinHttpOpen(Widen(request.userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0) };
        if (!session.h)
            return fail("WinHttpOpen"), result;
        const int ms = int(request.timeoutSeconds * 1000.0);
        WinHttpSetTimeouts(session.h, ms, ms, ms, ms);
        Handle connection{ WinHttpConnect(session.h, host.c_str(), parts.nPort, 0) };
        if (!connection.h)
            return fail("WinHttpConnect"), result;
        Handle req{ WinHttpOpenRequest(connection.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0) };
        if (!req.h)
            return fail("WinHttpOpenRequest"), result;
        DWORD option = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &option, sizeof(option));
        option = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &option, sizeof(option));
        option = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        WinHttpSetOption(req.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &option, sizeof(option));
        for (const auto& [name, value] : request.headers)
        {
            const std::wstring line = Widen(name + ": " + value);
            WinHttpAddRequestHeaders(req.h, line.c_str(), DWORD(line.size()), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
        }

        // The whole exchange's deadline: closing the handle ends the call in
        // progress. The handle is then null, so ~Handle doesn't close it twice.
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false, cancelled = false;
        std::thread watchdog([&] {
            std::unique_lock lock(mutex);
            if (!done.wait_for(lock, std::chrono::duration<double>(request.timeoutSeconds), [&] { return finished; }))
            {
                cancelled = true;
                WinHttpCloseHandle(req.h);
            }
        });
        auto exchange = [&] {
            if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
                return fail("WinHttpSendRequest");
            if (!WinHttpReceiveResponse(req.h, nullptr))
                return fail("WinHttpReceiveResponse");
            DWORD bytes = 0;
            WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
            std::wstring headers(bytes / sizeof(wchar_t), L'\0');
            if (!bytes || !WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, headers.data(), &bytes, WINHTTP_NO_HEADER_INDEX))
                return fail("WinHttpQueryHeaders");
            headers.resize(bytes / sizeof(wchar_t));
            std::string raw = Narrow(headers.data(), headers.size());
            const size_t headerBytes = raw.size();
            for (;;)
            {
                char buffer[16384];
                DWORD read = 0;
                if (!WinHttpReadData(req.h, buffer, sizeof(buffer), &read))
                    return fail("WinHttpReadData");
                if (!read)
                    break;
                raw.append(buffer, read);
                if (raw.size() - headerBytes > request.maxBytes)
                {
                    result.transport = Transport::TooLarge;
                    result.detail = std::format("over {} bytes", request.maxBytes);
                    return;
                }
            }
            if (!ParseHttpResponse(raw, result.response))
            {
                result.transport = Transport::Garbled;
                result.detail = std::format("WinHTTP's answer isn't an HTTP answer ({} bytes)", raw.size());
                return;
            }
            result.transport = Transport::Ok;
        };
        exchange();
        {
            std::lock_guard lock(mutex);
            finished = true;
            if (cancelled)
            {
                req.h = nullptr;
                if (result.transport != Transport::Ok)
                {
                    result.transport = Transport::Timeout;
                    result.detail = std::format("no whole answer within {} s", request.timeoutSeconds);
                }
            }
        }
        done.notify_one();
        watchdog.join();
        return result;
    }
}
#endif
