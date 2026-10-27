// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
//
// Android: the system's file picker's side of installing (android_files.cpp).
// SDL's file dialog there is the system's document picker, and what it answers
// is a content:// URI, not a path: the provider (the phone's storage, Downloads,
// a USB stick, a cloud service) hands the app the file itself, read where it
// is, 7 GB never copied, with no permission to read its folder. The app may
// open it while it runs. The installer keeps the URI as the image's "path";
// platform::OpenFile opens such a path through the provider, and these give
// the name the player knows it by.
//
// Any thread.
#pragma once
#include <filesystem>
#include <string>

namespace platform::android
{
    // A document the file picker answered with (content://...), not a path.
    bool IsDocument(const std::filesystem::path& path);

    // Opens a picked document read-only: its file descriptor (close-on-exec),
    // or -1 with errno set (EACCES when the provider refuses, say the grant is
    // gone: the app was restarted).
    int OpenDocument(const std::filesystem::path& path);

    // The document's name as the provider shows it ("NFSMW.iso"), asked once
    // and remembered; the URI's last part if the provider gives none.
    std::string DocumentName(const std::filesystem::path& path);
}
