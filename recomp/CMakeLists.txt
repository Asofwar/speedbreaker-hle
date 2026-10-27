# Compiles XenonRecomp's output (../ppc, generated from the user's own XEX and
# never committed) into a static library. Regenerate ppc/ with
# scripts/recompile.sh, or by hand (then without ppc/.inputs, below):
#   tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/nfsmw.toml tools/XenonRecomp/XenonUtils/ppc_context.h
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Built from the top-level CMakeLists.txt. CONFIGURE_DEPENDS: a run that makes
# more or fewer ppc_recomp.<n>.cpp configures again, rather than linking
# without the new ones or failing on a vanished one.
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${ROOT}/ppc/*.cpp)
if(NOT PPC_SOURCES)
    message(FATAL_ERROR "No generated sources in ppc/. Run scripts/recompile.sh first (README.md, \"Building from source\").")
endif()

# ppc/ against what it was made from. scripts/recompile.sh writes ppc/.inputs:
# the executable's SHA-256, each config/*.toml's and
# patches/xenonrecomp/*.patch's, and tools/XenonRecomp's commit, one
# "<input> <value>" per line. Here they are worked out again from the
# checkout: a pull that changed the config (v0.1.3 changed nfsmw.toml and
# switch_tables.toml) or the patches would otherwise build the old ppc/ into
# the new runtime without a word. Ninja configures again when one of these
# files changes (CMAKE_CONFIGURE_DEPENDS), so `ninja` after a pull checks too.
#   - A difference stops the configure: run scripts/recompile.sh (and first
#     scripts/setup_tools.sh, when the patches changed), or configure with
#     -DSB_ALLOW_STALE_PPC=ON to build anyway (bisecting, an experiment with
#     the config), which only warns.
#   - No ppc/.inputs only warns: ppc/ was made by README.md's manual command
#     or before recompile.sh existed, and stopping every such checkout would
#     break builds that may well be current. What it was made from is unknown.
option(SB_ALLOW_STALE_PPC "Build when ppc/.inputs doesn't match config/, patches/xenonrecomp or tools/XenonRecomp (only warn)" OFF)
set(SB_PPC_INPUTS ${ROOT}/ppc/.inputs)
file(GLOB _sb_configs CONFIGURE_DEPENDS ${ROOT}/config/*.toml)
file(GLOB _sb_patches CONFIGURE_DEPENDS ${ROOT}/patches/xenonrecomp/*.patch)
set(_sb_want "")
set(_sb_depends ${_sb_configs} ${_sb_patches})
# The supported executable's SHA-256, from config/nfsmw.toml's header (the one
# recompile.sh checks game/files/default.xex against).
file(STRINGS ${ROOT}/config/nfsmw.toml _sb_xex REGEX "^# default\\.xex SHA-256 [0-9a-f]+")
if(_sb_xex MATCHES "SHA-256 ([0-9a-f]+)")
    list(APPEND _sb_want "default.xex ${CMAKE_MATCH_1}")
endif()
foreach(_sb_file IN LISTS _sb_configs _sb_patches)
    file(SHA256 ${_sb_file} _sb_hash)
    file(RELATIVE_PATH _sb_name ${ROOT} ${_sb_file})
    list(APPEND _sb_want "${_sb_name} ${_sb_hash}")
endforeach()
# XenonRecomp's commit, when git can tell (no git, or tools/XenonRecomp not a
# checkout: that line isn't compared). Its branch and its binary are
# dependencies too: setup_tools.sh remakes both when the patches change.
find_package(Git QUIET)
set(_sb_tool "")
if(GIT_FOUND AND EXISTS ${ROOT}/tools/XenonRecomp/.git)
    execute_process(COMMAND ${GIT_EXECUTABLE} -C ${ROOT}/tools/XenonRecomp rev-parse HEAD
        OUTPUT_VARIABLE _sb_tool OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _sb_git)
    if(NOT _sb_git EQUAL 0)
        set(_sb_tool "")
    endif()
    foreach(_sb_ref .git/HEAD .git/refs/heads/nfsmw)
        if(EXISTS ${ROOT}/tools/XenonRecomp/${_sb_ref})
            list(APPEND _sb_depends ${ROOT}/tools/XenonRecomp/${_sb_ref})
        endif()
    endforeach()
endif()
if(_sb_tool)
    list(APPEND _sb_want "xenonrecomp ${_sb_tool}")
endif()
if(EXISTS ${ROOT}/tools/XenonRecomp/build/XenonRecomp/XenonRecomp)
    list(APPEND _sb_depends ${ROOT}/tools/XenonRecomp/build/XenonRecomp/XenonRecomp)
endif()
if(EXISTS ${SB_PPC_INPUTS})
    # Only when it exists: a missing file among these would make ninja
    # configure again on every build.
    list(APPEND _sb_depends ${SB_PPC_INPUTS})
    file(STRINGS ${SB_PPC_INPUTS} _sb_have REGEX "^[^#]")
    # Each input whose line differs, or is on one side only (a patch added
    # or removed). The tool's line only when git could tell it here.
    set(_sb_stale "")
    foreach(_sb_line IN LISTS _sb_want _sb_have)
        string(REGEX REPLACE " .*" "" _sb_key "${_sb_line}")
        if(_sb_key STREQUAL "xenonrecomp" AND NOT _sb_tool)
            continue()
        endif()
        if(NOT _sb_line IN_LIST _sb_want OR NOT _sb_line IN_LIST _sb_have)
            list(APPEND _sb_stale ${_sb_key})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _sb_stale)
    if(_sb_stale)
        list(JOIN _sb_stale ", " _sb_stale)
        set(_sb_level FATAL_ERROR)
        if(SB_ALLOW_STALE_PPC)
            set(_sb_level WARNING)
        endif()
        message(${_sb_level} "ppc/ was generated from other inputs than this checkout's (${_sb_stale} differ from ppc/.inputs): "
            "run scripts/setup_tools.sh if patches/xenonrecomp changed, then scripts/recompile.sh. "
            "-DSB_ALLOW_STALE_PPC=ON builds the old ppc/ anyway.")
    endif()
else()
    message(WARNING "ppc/ has no .inputs (it was generated by hand, or before scripts/recompile.sh existed), so whether "
        "it matches config/, patches/xenonrecomp and tools/XenonRecomp is unknown. scripts/recompile.sh regenerates it "
        "and records them, and from then on a configure checks them.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_sb_depends})

add_library(NfsmwRecompLib STATIC ${PPC_SOURCES})
target_compile_features(NfsmwRecompLib PUBLIC cxx_std_20)
target_include_directories(NfsmwRecompLib PUBLIC
    ${ROOT}/ppc
    ${ROOT}/tools/XenonRecomp/XenonUtils
    ${ROOT}/tools/XenonRecomp/thirdparty/simde)
target_compile_options(NfsmwRecompLib PRIVATE
    -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable
    -fno-strict-aliasing)
if(SB_MSVC_FRONTEND)
    # clang-cl has no -include: /FI, in the same order. The third, on
    # Windows only, aligns the guest's jmp_bufs for the CRT's setjmp
    # (runtime/platform/win32/guest_setjmp.h).
    target_compile_options(NfsmwRecompLib PRIVATE
        "/FI${ROOT}/runtime/cpu/indirect.h"
        "/FI${ROOT}/runtime/cpu/guest_time.h"
        "/FI${ROOT}/runtime/platform/win32/guest_setjmp.h")
else()
    target_compile_options(NfsmwRecompLib PRIVATE
        # Checked indirect calls (runtime/cpu/indirect.cpp). SHELL: keeps each
        # -include with its file (CMake drops a repeated plain -include).
        "SHELL:-include \"${ROOT}/runtime/cpu/indirect.h\""
        # mftb reads the game's clock, which stands still while it's suspended.
        "SHELL:-include \"${ROOT}/runtime/cpu/guest_time.h\"")
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    if(SB_MSVC_FRONTEND)
        target_compile_options(NfsmwRecompLib PRIVATE /clang:-march=x86-64-v3)
    else()
        target_compile_options(NfsmwRecompLib PRIVATE -march=x86-64-v3)
    endif()
endif()
if(IOS)
    target_compile_options(NfsmwRecompLib PRIVATE -mcpu=apple-a15)
endif()

# The European (PAL, Spanish) release's code (../ppc-pal-es: XenonRecomp with
# config/pal-es/nfsmw.toml on that disc's default.xex, then
# scripts/region_ppc.py ppc-pal-es pal_es), linked beside the USA's under
# namespace pal_es; the runtime picks one at startup (runtime/region/region.h).
# SB_PAL_ES: AUTO (when ppc-pal-es/ is there), ON (required) or OFF.
set(SB_PAL_ES AUTO CACHE STRING "Link the PAL (Spanish) release's recompiled code: AUTO, ON or OFF")
file(GLOB PPC_PAL_ES_SOURCES ${ROOT}/ppc-pal-es/*.cpp)
if(SB_PAL_ES STREQUAL "ON" AND NOT PPC_PAL_ES_SOURCES)
    message(FATAL_ERROR "SB_PAL_ES=ON, but ppc-pal-es/ has no generated sources")
endif()
if(PPC_PAL_ES_SOURCES AND NOT SB_PAL_ES STREQUAL "OFF")
    file(READ ${ROOT}/ppc-pal-es/ppc_recomp_shared.h _sb_pal_head LIMIT 64)
    if(NOT _sb_pal_head MATCHES "^// Namespaced by scripts/region_ppc.py: pal_es")
        message(FATAL_ERROR "ppc-pal-es/ isn't namespaced: run python3 scripts/region_ppc.py ppc-pal-es pal_es")
    endif()
    add_library(NfsmwRecompPalEsLib STATIC ${PPC_PAL_ES_SOURCES})
    target_compile_features(NfsmwRecompPalEsLib PUBLIC cxx_std_20)
    target_include_directories(NfsmwRecompPalEsLib PRIVATE
        ${ROOT}/ppc-pal-es
        ${ROOT}/tools/XenonRecomp/XenonUtils
        ${ROOT}/tools/XenonRecomp/thirdparty/simde)
    get_target_property(_sb_ppc_options NfsmwRecompLib COMPILE_OPTIONS)
    target_compile_options(NfsmwRecompPalEsLib PRIVATE ${_sb_ppc_options})
    set(SB_HAVE_PAL_ES ON CACHE INTERNAL "ppc-pal-es is linked")
    message(STATUS "PAL (Spanish) release: linked (ppc-pal-es/)")
else()
    set(SB_HAVE_PAL_ES OFF CACHE INTERNAL "ppc-pal-es is linked")
    message(STATUS "PAL (Spanish) release: not linked (no ppc-pal-es/, or SB_PAL_ES=OFF)")
endif()
