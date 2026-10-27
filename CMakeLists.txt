# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
#   cmake -S . -B build/main -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
#         -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++
#   ninja -C build/main -j6
#
# Needs ppc/ (XenonRecomp output from your own default.xex, made by
# scripts/recompile.sh) and tools/ (scripts/setup_tools.sh). -DSB_TESTS=ON
# adds the unit tests (tests/; run them with ctest from the build folder).
# -DSB_TESTS_ONLY=ON configures nothing but the unit tests that need neither
# ppc/ nor tools/, SDL3 or Vulkan: a fresh clone with only Clang and CMake
# builds and runs them (CI):
#
#   cmake -S . -B build/tests -G Ninja -DSB_TESTS_ONLY=ON \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   ninja -C build/tests && ctest --test-dir build/tests -LE timing
#
# Windows: upstream's build (clang-cl, lld-link, the static C runtime; see
# CMakePresets.json):
#   cmake --preset windows-clang -B <build dir> -DCMAKE_PREFIX_PATH=<deps> -DFFMPEG_XMA=<deps>
# or this fork's cross-build from Linux with MinGW-w64 and Clang
# (scripts/windows/build.sh; docs/windows.md).
cmake_minimum_required(VERSION 3.20)

# macOS (not iOS, which scripts/ios/build.sh sets to 17.0): the oldest macOS
# the game runs on, 15.0 (Sequoia), which the Mac app declares
# (scripts/macos/make_app.py reads it from the binaries and refuses a library
# that needs a newer one). The libraries the app bundles must be built for it
# too: scripts/macos/build_deps.sh builds them from source (Homebrew's
# bottles are built for the macOS they were made on, 26.0 on macOS 26), and
# the build takes them with -DCMAKE_PREFIX_PATH=<its prefix>
# -DFFMPEG_XMA=<its prefix>/ffmpeg-xma. The target sets only which macOS
# APIs and libc++ features the code may use: the CPU code is the same for
# any target (clang's default for arm64 Macs, apple-m1), and
# scripts/macos/check_codegen.py proves it for a release's commit (its game
# code for this target against the same code for macOS 26.0, function by
# function). Before project(): it picks the SDK's settings.
#
# A build folder keeps the target it was first configured with, so a folder
# still holding one of our earlier defaults (26.0, up to v0.1.0) moves to
# this one, with a line saying so. SB_OSX_TARGET_FROM remembers where the
# folder's target came from: "default <version>" (ours, which follows this
# file), or "chosen" (given with -DCMAKE_OSX_DEPLOYMENT_TARGET=..., or
# changed in the cache: kept; scripts/macos/make_app.py warns that such an
# app isn't the release's).
set(SB_OSX_DEFAULT_TARGET "15.0")
set(SB_OSX_EARLIER_DEFAULTS "26.0")  # in folders configured before SB_OSX_TARGET_FROM
if(CMAKE_HOST_APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_sb_follow OFF)
    if(NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
        set(_sb_follow ON)
    elseif(DEFINED CACHE{SB_OSX_TARGET_FROM})
        # Ours, unless it has been changed since.
        set(_sb_follow OFF)
        if(SB_OSX_TARGET_FROM STREQUAL "default ${CMAKE_OSX_DEPLOYMENT_TARGET}")
            set(_sb_follow ON)
        endif()
    elseif(DEFINED CACHE{SpeedBreaker_BINARY_DIR} AND (CMAKE_OSX_DEPLOYMENT_TARGET IN_LIST SB_OSX_EARLIER_DEFAULTS
           OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL SB_OSX_DEFAULT_TARGET))
        # Configured before SB_OSX_TARGET_FROM (project() has run in this
        # folder before, so it isn't a -D of a first configure) with one of
        # our defaults.
        set(_sb_follow ON)
    endif()
    if(_sb_follow)
        if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL SB_OSX_DEFAULT_TARGET)
            message(STATUS "macOS deployment target: ${CMAKE_OSX_DEPLOYMENT_TARGET} (an earlier default) -> ${SB_OSX_DEFAULT_TARGET}")
        endif()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${SB_OSX_DEFAULT_TARGET}" CACHE STRING "The oldest macOS the game runs on" FORCE)
        set(SB_OSX_TARGET_FROM "default ${SB_OSX_DEFAULT_TARGET}" CACHE INTERNAL "Where CMAKE_OSX_DEPLOYMENT_TARGET came from")
    else()
        set(SB_OSX_TARGET_FROM "chosen" CACHE INTERNAL "Where CMAKE_OSX_DEPLOYMENT_TARGET came from")
    endif()
endif()

# The version: the one place it is set. Everything else comes from it: the
# log's [build] line, Settings > Advanced, bug reports' names, the Mac and iOS
# apps' versions, the SteamOS bundles' VERSION file and the release tag
# (v<version>). CHANGELOG.md has an entry for each version (the in-game
# "What's new" note is built from it).
project(SpeedBreaker VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# clang-cl (the MSVC front end): its spellings of the flags the other
# platforms pass GCC-style.
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(SB_MSVC_FRONTEND ON)
else()
    set(SB_MSVC_FRONTEND OFF)
endif()

# The compiler: Clang 18 or newer, as README.md asks, or Apple's clang from
# Xcode 16 or newer (scripts/ios/build.sh builds the iOS libraries with
# Xcode's own compiler; the Mac build uses Homebrew's LLVM). Checked before
# anything is built: GCC, the default c++ on Fedora and Ubuntu, or an older
# Clang would otherwise fail far into the build, deep in the generated code
# or the runtime (clang's vector extensions, its flags, C++20 library parts),
# with errors that never say the compiler is the cause. A build folder keeps
# the compiler it was first configured with, so the message says to make a
# new one.
foreach(_sb_lang C CXX)
    if(SB_MSVC_FRONTEND)
        break()
    endif()
    set(_sb_id "${CMAKE_${_sb_lang}_COMPILER_ID}")
    set(_sb_version "${CMAKE_${_sb_lang}_COMPILER_VERSION}")
    if(NOT ((_sb_id STREQUAL "Clang" AND _sb_version VERSION_GREATER_EQUAL 18)
            OR (_sb_id STREQUAL "AppleClang" AND _sb_version VERSION_GREATER_EQUAL 16)))
        message(FATAL_ERROR "SpeedBreaker needs Clang 18 or newer (or Apple's clang from Xcode 16 or newer), "
            "but the ${_sb_lang} compiler is ${_sb_id} ${_sb_version} (${CMAKE_${_sb_lang}_COMPILER}). Configure a new "
            "build folder with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ (a clang 18 or newer; on a Mac, "
            "Homebrew's: /opt/homebrew/opt/llvm/bin/clang and clang++). README.md, \"Building from source\".")
    endif()
endforeach()

if(WIN32 AND SB_MSVC_FRONTEND)
    # This fork builds Windows with MinGW-w64 only: its online play, the GPU
    # thread's doorbells and its save files take compat/win, which clang-cl's
    # build (upstream's) doesn't have.
    message(FATAL_ERROR "This fork's Windows build is the MinGW-w64 cross-build (scripts/windows/build.sh, docs/windows.md); "
        "upstream's clang-cl preset is not supported here")
    # One self-contained exe: the static C/C++ runtime (/MT) in every
    # configuration, which every dependency must match (lld-link refuses a
    # mix).
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    # xbox.h includes <windows.h> in every translation unit.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    # C++ exceptions with /EHs, not CMake's /EHsc: the c lets the compiler
    # assume extern "C" functions never throw, and every recompiled function
    # is extern "C" (PPC_FUNC_IMPL) with GuestThreadExit thrown through it
    # (ExTerminateThread). Under /EHsc that throw ends in std::terminate.
    string(REPLACE "/EHsc" "/EHs" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    if(NOT CMAKE_CXX_FLAGS MATCHES "/EHs")
        string(APPEND CMAKE_CXX_FLAGS " /EHs")
    endif()
elseif(WIN32)
    # The MinGW-w64 cross-build (scripts/windows/build.sh): as upstream's,
    # without the MSVC front end.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
endif()

# Binaries name their sources from the repository root (assert messages,
# debug information), never by the folders they were built in: a published
# build carries no one's home folder (a build folder inside the checkout
# reads ./build/...). The real path too, for a checkout reached through a
# symlink; both map to the same name, so their order doesn't matter.
# clang-cl takes the option through /clang:, and only C and C++ get it there
# (the resource compiler, platform/win32/speedbreaker.rc, doesn't know it).
file(REAL_PATH "${CMAKE_SOURCE_DIR}" SB_SOURCE_REAL)
if(SB_MSVC_FRONTEND)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/clang:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.>")
    if(NOT SB_SOURCE_REAL STREQUAL CMAKE_SOURCE_DIR)
        add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/clang:-ffile-prefix-map=${SB_SOURCE_REAL}=.>")
    endif()
else()
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
    if(NOT SB_SOURCE_REAL STREQUAL CMAKE_SOURCE_DIR)
        add_compile_options("-ffile-prefix-map=${SB_SOURCE_REAL}=.")
    endif()
endif()

# macOS: a call to an API newer than the deployment target is an error unless
# it's checked for (@available / __builtin_available), since it would crash
# on an older macOS the app says it runs on. A diagnostic only: the code
# generated is the same.
if(APPLE AND NOT IOS)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Werror=unguarded-availability-new>")
endif()

option(SB_TESTS_ONLY "Configure only the unit tests that need no ppc/, tools/, SDL3 or Vulkan (CI)" OFF)
if(SB_TESTS_ONLY)
    # No game: recomp/ would stop without ppc/, runtime/ without SDL3,
    # Vulkan, glslang and tools/. tests/ leaves out what needs them.
    enable_testing()
    add_subdirectory(tests)
else()
    # Compiler-level optimisation of the game's code: all off by default, and
    # without them the build is the same as before they existed. They apply to
    # what the game itself runs, the recompiled code (NfsmwRecompLib) and the
    # runtime with the libraries it links statically, never to the unit tests.
    # Linux only (ELF, lld, LLVM's profile runtime): the SteamOS build is
    # where they were measured (docs/timedemo.md, "Faster builds"); Apple's
    # linkers and toolchains are untested. They change how the code is laid
    # out and inlined, never what it computes: a timedemo replays the same
    # frames on them (the same trace hash).
    #   -DSB_PGO_GENERATE=ON: an instrumented build that counts how often each
    #     branch, call and loop runs, for training (scripts/pgo_train.sh).
    #   -DSB_PGO_PROFILE=<file.profdata>: a build optimised with the counts of
    #     such a training run, merged by llvm-profdata (scripts/pgo_train.sh).
    #   -DSB_LTO=ON: ThinLTO, linked with lld. The compiler sees across source
    #     files at link time, so the runtime's small functions inline into
    #     their callers in other files (cpu/indirect.cpp's lookup into every
    #     guest function with an indirect call). Best with the profile. Its
    #     builds' timedemo replays of the default race go out of step where
    #     the intro ends more often than others' (docs/timedemo.md): replay
    #     those again.
    option(SB_PGO_GENERATE "An instrumented build that writes a profile for SB_PGO_PROFILE (Linux, Android; scripts/pgo_train.sh)" OFF)
    set(SB_PGO_PROFILE "" CACHE FILEPATH "A profile from scripts/pgo_train.sh (.profdata) to optimise the game's code with (Linux, Android)")
    option(SB_LTO "ThinLTO for the game's code, linked with lld (Linux, Android)" OFF)
    if((SB_PGO_GENERATE OR SB_PGO_PROFILE OR SB_LTO) AND NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID))
        message(FATAL_ERROR "SB_PGO_GENERATE, SB_PGO_PROFILE and SB_LTO are for Linux and Android builds only")
    endif()
    if(SB_PGO_GENERATE AND SB_PGO_PROFILE)
        message(FATAL_ERROR "SB_PGO_GENERATE (training) or SB_PGO_PROFILE (the optimised build), not both")
    endif()
    # The build's identity names them after the build type (runtime/CMakeLists.txt,
    # report/build_info.cpp: the log's [build] line, bug reports, a timedemo's
    # summary.json): "Release, ThinLTO, PGO".
    set(SB_BUILD_FLAVOR "")
    if(SB_LTO)
        string(APPEND SB_BUILD_FLAVOR ", ThinLTO")
    endif()
    if(SB_PGO_GENERATE)
        string(APPEND SB_BUILD_FLAVOR ", PGO-instrumented")
    endif()
    if(SB_PGO_PROFILE)
        string(APPEND SB_BUILD_FLAVOR ", PGO")
    endif()

    add_subdirectory(recomp)
    add_subdirectory(runtime)

    set(_sb_opt_targets NfsmwRecompLib SpeedBreaker XenonUtilsLite o1heap imgui)
    set(_sb_opt_compile "")
    set(_sb_opt_link "")
    if(SB_LTO)
        # The ThinLTO cache keeps each file's optimised code by a hash of what
        # went into it, so a relink after an edit redoes only what it touched.
        list(APPEND _sb_opt_compile -flto=thin)
        list(APPEND _sb_opt_link -flto=thin -fuse-ld=lld "LINKER:--thinlto-cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
    endif()
    if(SB_PGO_GENERATE)
        # The profile runtime writes its counters from an atexit handler,
        # which the game's _Exit skips: runtime/debug/pgo_exit.c writes them
        # first, in place of every call to _Exit (the linker's --wrap).
        list(APPEND _sb_opt_compile -fprofile-generate)
        list(APPEND _sb_opt_link -fprofile-generate "LINKER:--wrap=_Exit")
        target_sources(SpeedBreaker PRIVATE ${CMAKE_SOURCE_DIR}/runtime/debug/pgo_exit.c)
    endif()
    if(SB_PGO_PROFILE)
        get_filename_component(_sb_profile "${SB_PGO_PROFILE}" ABSOLUTE)
        if(NOT EXISTS "${_sb_profile}")
            message(FATAL_ERROR "SB_PGO_PROFILE: no file ${_sb_profile} (scripts/pgo_train.sh makes one)")
        endif()
        list(APPEND _sb_opt_compile "-fprofile-use=${_sb_profile}")
        # Except gpu/renderer.cpp, which the profile made slower: Draw inlined
        # what it calls (50 KB of code grew to 159 KB) and its hottest loop,
        # the scan of a draw's indices for 0xFFFF, came out at 6 instructions
        # an index instead of 4, so the command processor (the frame's
        # critical path) took 4% longer. Without the profile there its other
        # code (command_processor.cpp) still gains, -2% in all. Worth trying
        # again when that scan is gone.
        set_property(SOURCE ${CMAKE_SOURCE_DIR}/runtime/gpu/renderer.cpp TARGET_DIRECTORY SpeedBreaker
            APPEND PROPERTY COMPILE_OPTIONS -fno-profile-instr-use)
        # A new profile in the same file recompiles everything it guides.
        foreach(_sb_target IN LISTS _sb_opt_targets)
            get_target_property(_sb_sources ${_sb_target} SOURCES)
            set_source_files_properties(${_sb_sources} TARGET_DIRECTORY ${_sb_target}
                PROPERTIES OBJECT_DEPENDS "${_sb_profile}")
        endforeach()
    endif()
    foreach(_sb_target IN LISTS _sb_opt_targets)
        target_compile_options(${_sb_target} PRIVATE ${_sb_opt_compile})
    endforeach()
    target_link_options(SpeedBreaker PRIVATE ${_sb_opt_link})

    option(SB_TESTS "Build the unit tests (tests/)" OFF)
    if(SB_TESTS)
        enable_testing()
        add_subdirectory(tests)
    endif()
endif()
