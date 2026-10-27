// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING). See platform/android_files.h.
#ifdef __ANDROID__
#include <platform/android_files.h>

#include <SDL3/SDL.h>
#include <jni.h>

#include <cerrno>
#include <cstdio>
#include <map>
#include <mutex>
#include <string_view>

#include <fcntl.h>

namespace platform::android
{
    namespace
    {
        std::mutex s_namesMutex;
        std::map<std::string, std::string> s_names;

        int HexDigit(char c)
        {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        }

        // The URI's last part with its %XX escapes undone, after the last '/' or
        // ':' in that (a document ID such as "primary:Download/NFSMW.iso").
        std::string NameInUri(std::string_view uri)
        {
            std::string_view last = uri.substr(uri.find_last_of('/') + 1);
            std::string decoded;
            for (size_t i = 0; i < last.size(); i++)
            {
                if (last[i] == '%' && i + 2 < last.size() && HexDigit(last[i + 1]) >= 0 && HexDigit(last[i + 2]) >= 0)
                {
                    decoded += char(HexDigit(last[i + 1]) * 16 + HexDigit(last[i + 2]));
                    i += 2;
                }
                else
                    decoded += last[i];
            }
            size_t cut = decoded.find_last_of("/:");
            return cut == std::string::npos ? decoded : decoded.substr(cut + 1);
        }

        // SpeedBreakerActivity.documentName: the provider's display name, or
        // empty (none, or no Java to ask).
        std::string AskProvider(const std::string& uri)
        {
            auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
            auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
            if (!env || !activity)
                return {};
            std::string name;
            jclass activityClass = env->GetObjectClass(activity);
            jmethodID method = env->GetStaticMethodID(activityClass, "documentName", "(Ljava/lang/String;)Ljava/lang/String;");
            if (method)
            {
                jstring text = env->NewStringUTF(uri.c_str());
                auto answer = static_cast<jstring>(env->CallStaticObjectMethod(activityClass, method, text));
                if (answer && !env->ExceptionCheck())
                {
                    if (const char* chars = env->GetStringUTFChars(answer, nullptr))
                    {
                        name = chars;
                        env->ReleaseStringUTFChars(answer, chars);
                    }
                }
                if (answer)
                    env->DeleteLocalRef(answer);
                env->DeleteLocalRef(text);
            }
            if (env->ExceptionCheck())
                env->ExceptionClear();
            env->DeleteLocalRef(activityClass);
            env->DeleteLocalRef(activity);
            return name;
        }
    }

    bool IsDocument(const std::filesystem::path& path)
    {
        return path.native().starts_with("content://");
    }

    int OpenDocument(const std::filesystem::path& path)
    {
        // SDL opens it through the activity's ContentResolver (openFileDescriptor,
        // "r"); the stream's descriptor, copied, outlives the stream.
        SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "rb");
        if (!stream)
        {
            fprintf(stderr, "[files] can't open %s: %s\n", path.c_str(), SDL_GetError());
            errno = EACCES;
            return -1;
        }
        const int fd = int(SDL_GetNumberProperty(SDL_GetIOProperties(stream), SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1));
        const int copy = fd >= 0 ? fcntl(fd, F_DUPFD_CLOEXEC, 0) : -1;
        const int error = fd >= 0 ? errno : EBADF;
        SDL_CloseIO(stream);
        if (copy < 0)
        {
            fprintf(stderr, "[files] no descriptor for %s\n", path.c_str());
            errno = error;
        }
        return copy;
    }

    std::string DocumentName(const std::filesystem::path& path)
    {
        std::lock_guard lock(s_namesMutex);
        auto it = s_names.find(path.native());
        if (it == s_names.end())
        {
            std::string name = AskProvider(path.native());
            it = s_names.emplace(path.native(), name.empty() ? NameInUri(path.native()) : name).first;
        }
        return it->second;
    }
}
#endif
