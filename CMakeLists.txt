cmake_minimum_required (VERSION 3.29)

set(LIBERTY_RECOMP_THIRDPARTY_ROOT ${CMAKE_SOURCE_DIR}/thirdparty)
set(LIBERTY_RECOMP_TOOLS_ROOT ${CMAKE_SOURCE_DIR}/tools)
# C++ standard for first-party targets. The RexGlue SDK pins its own standard
# (it uses C++23 library features); generated and vendored code is untouched.
set(LIBERTY_RECOMP_CXX_STANDARD 23 CACHE STRING "C++ standard for first-party targets")
set(CMAKE_CXX_STANDARD ${LIBERTY_RECOMP_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_SHARED_LIBS OFF)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Check source prerequisites before project() enters the vcpkg toolchain.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/DependencyPatches.cmake")
liberty_require_dependency_sources("${CMAKE_CURRENT_SOURCE_DIR}")
if(APPLE AND NOT CMAKE_TOOLCHAIN_FILE)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindHostLLVM.cmake")
endif()
project("LibertyRecomp-ALL")

# Reproduce the source fixes carried for pinned upstream dependencies.
liberty_apply_dependency_patches("${CMAKE_CURRENT_SOURCE_DIR}")

set(LIBERTY_RECOMP_GRAINE_MAC OFF)
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" AND
   (NOT DEFINED LIBERTY_RECOMP_TARGET_PLATFORM OR
    LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "desktop"))
    set(LIBERTY_RECOMP_GRAINE_MAC ON)
endif()

if(LIBERTY_RECOMP_GRAINE_MAC)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckMacOSToolchain.cmake")
endif()

# Android consumes the same pure Graine RexGlue application as macOS; the
# legacy LibertyRecomp/ tree is not built for it.
set(LIBERTY_RECOMP_GRAINE_ANDROID OFF)
if(ANDROID)
    set(LIBERTY_RECOMP_GRAINE_ANDROID ON)
endif()

if(NOT LIBERTY_RECOMP_GRAINE_MAC)
    add_compile_definitions(FMT_USE_CONSTEVAL=0)
endif()

# ─── System dependencies (no package manager) ──────────────────────────────
# Third-party libraries come from the OS package manager (zypper/dnf/apt) and
# Linuxbrew, found via standard find_package/pkg-config paths:
#   zypper: libcurl-devel, libopenssl-devel, freetype2-devel, fmt-devel,
#           spdlog-devel, discord-rpc-devel (optional)
#   brew:   protobuf (or any libprotobuf + protoc install)
# No VCPKG_ROOT, toolchain file, or bundled ports are needed or used on Linux.
# ─── Homebrew prefix (protobuf etc.) ───────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/home/linuxbrew/.linuxbrew")
    list(APPEND CMAKE_PREFIX_PATH "/home/linuxbrew/.linuxbrew")
endif()
# ─── Position-independent code ─────────────────────────────────────────────
# The old vcpkg toolchain injected -fPIC globally; keep that behavior so
# shared objects (librexruntime.so, plugins) keep linking.
if(NOT MSVC)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
# ────────────────────────────────────────────────────────────────────────────

# ─── vcpkg check (desktop platforms only, non-Linux) ───────────────────────
# Mobile/console platforms use system SDKs and don't need vcpkg.
# Linux desktop uses system/brew deps, never vcpkg.
if(NOT LIBERTY_RECOMP_GRAINE_MAC AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   (NOT DEFINED LIBERTY_RECOMP_TARGET_PLATFORM OR
    LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "desktop"))
    if(NOT DEFINED ENV{VCPKG_ROOT})
        message(FATAL_ERROR "VCPKG_ROOT is not defined! Set it or use a platform-specific preset.")
    endif()
endif()
# ────────────────────────────────────────────────────────────────────────────

# ─── Cross-Platform Target Selection ───────────────────────────────────────
# Set LIBERTY_RECOMP_TARGET_PLATFORM to one of: desktop, ios, android, ps4, switch
# The toolchain files (toolchains/*.cmake) set this automatically.
if(NOT DEFINED LIBERTY_RECOMP_TARGET_PLATFORM)
    set(LIBERTY_RECOMP_TARGET_PLATFORM "desktop" CACHE STRING
        "Target platform: desktop | ios | android | ps4 | switch")
endif()
message(STATUS "Target platform: ${LIBERTY_RECOMP_TARGET_PLATFORM}")

if(APPLE)
    set(LIBERTY_IOS_DEVELOPMENT_TEAM "" CACHE STRING "Apple development team ID for iOS signing.")
    set(LIBERTY_IOS_BUNDLE_IDENTIFIER "com.libertyrecomp" CACHE STRING "Bundle identifier for iOS builds.")
    set(LIBERTY_IOS_PROVISIONING_PROFILE "" CACHE STRING "Optional iOS provisioning profile specifier.")
    set(LIBERTY_MACOS_BUNDLE_IDENTIFIER "OZORDI.LibertyRecomp" CACHE STRING "Bundle identifier for macOS builds.")
    set(LIBERTY_MACOS_CODE_SIGN_IDENTITY "" CACHE STRING "Optional macOS codesign identity for entitlement signing.")
endif()

# ─── Build-time Asset Path ──────────────────────────────────────────────────
# For non-desktop platforms that cannot use a file-picker installer, all game
# assets must be available at configure time and bundled into the package.
#
# Priority: $GTA4_GAME_PATH env var (if set) overrides everything,
#           then -DLIBERTY_RECOMP_EMBEDDED_GAME_PATH=... (CMake cache),
#           then tools/local_game_payload/ (default).
set(LIBERTY_RECOMP_EMBEDDED_GAME_PATH
    "${CMAKE_SOURCE_DIR}/tools/local_game_payload"
    CACHE PATH "Path to the staged GTA IV Xbox 360 game files for embedded builds")
if(DEFINED ENV{GAME_PATH})
    set(LIBERTY_RECOMP_EMBEDDED_GAME_PATH "$ENV{GAME_PATH}")
    message(STATUS "GAME_PATH env var set — using: ${LIBERTY_RECOMP_EMBEDDED_GAME_PATH}")
endif()

# Android-only opt-in: game files are delivered at runtime via
# LibertySDLActivity's folder-picker flow (see docs/ANDROID_BUILDING.md),
# so the CMake-time payload check is a blocker rather than a safety net.
# Defaults ON for Android (inherited from `android-*` CMake presets), OFF
# for every other embedded target (PS4 / Switch / iOS).
option(LIBERTY_RECOMP_ANDROID_RUNTIME_ASSETS
    "Android: expect the game payload to be supplied at runtime (skip CMake-time validation)"
    OFF)

if(NOT LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "desktop")
    message(STATUS "Embedded game path: ${LIBERTY_RECOMP_EMBEDDED_GAME_PATH}")

    if(LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "android" AND LIBERTY_RECOMP_ANDROID_RUNTIME_ASSETS)
        message(STATUS
            "Android: LIBERTY_RECOMP_ANDROID_RUNTIME_ASSETS=ON — skipping build-time\n"
            "         game-file validation. The Activity's folder-picker flow is\n"
            "         expected to hand the path to the native runtime via\n"
            "         Java_com_libertyrecomp_LibertySDLActivity_nativeSetGameRoot.")
    else()
        if(NOT EXISTS "${LIBERTY_RECOMP_EMBEDDED_GAME_PATH}")
            message(FATAL_ERROR
                "LIBERTY_RECOMP_EMBEDDED_GAME_PATH does not exist:\n"
                "  ${LIBERTY_RECOMP_EMBEDDED_GAME_PATH}\n"
                "Set the GAME_PATH environment variable to your extracted game directory,\n"
                "or populate tools/local_game_payload/, or pass "
                "-DLIBERTY_RECOMP_EMBEDDED_GAME_PATH=... to CMake.\n"
                "See tools/local_game_payload/README.md for the required layout.")
        endif()

        # Validate required files
        foreach(_req_file default.xex common.rpf xbox360.rpf audio.rpf)
            if(NOT EXISTS "${LIBERTY_RECOMP_EMBEDDED_GAME_PATH}/${_req_file}")
                message(FATAL_ERROR
                    "Required game file missing from embedded payload:\n"
                    "  ${LIBERTY_RECOMP_EMBEDDED_GAME_PATH}/${_req_file}\n"
                    "See tools/local_game_payload/README.md for the required layout.")
            endif()
        endforeach()

        message(STATUS "Embedded payload validated successfully.")
    endif()

    # ── Detect optional DLC ───────────────────────────────────────────────
    set(LIBERTY_PAYLOAD_HAS_TLAD  "false")
    set(LIBERTY_PAYLOAD_HAS_TBOGT "false")
    if(EXISTS "${LIBERTY_RECOMP_EMBEDDED_GAME_PATH}/dlc/TLAD")
        set(LIBERTY_PAYLOAD_HAS_TLAD "true")
        message(STATUS "  Found embedded DLC: TLAD")
    endif()
    if(EXISTS "${LIBERTY_RECOMP_EMBEDDED_GAME_PATH}/dlc/TBOGT")
        set(LIBERTY_PAYLOAD_HAS_TBOGT "true")
        message(STATUS "  Found embedded DLC: TBOGT")
    endif()

    # ── Manifest variables ────────────────────────────────────────────────
    set(LIBERTY_PAYLOAD_GAME_VERSION "1.0.0")
    set(LIBERTY_PAYLOAD_TU_VERSION   "")
    string(TIMESTAMP LIBERTY_PAYLOAD_BUILD_TIMESTAMP "%s")

    configure_file(
        "${CMAKE_SOURCE_DIR}/LibertyRecomp/install/manifest.json.in"
        "${CMAKE_BINARY_DIR}/manifest.json"
        @ONLY)
endif()
# ────────────────────────────────────────────────────────────────────────────



if (APPLE AND NOT LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "ps4")
    enable_language(OBJC OBJCXX)
endif()

if (CMAKE_OSX_ARCHITECTURES)
    set(LIBERTY_RECOMP_ARCHITECTURE ${CMAKE_OSX_ARCHITECTURES})
elseif(CMAKE_SYSTEM_PROCESSOR)
    set(LIBERTY_RECOMP_ARCHITECTURE ${CMAKE_SYSTEM_PROCESSOR})
else()
    set(LIBERTY_RECOMP_ARCHITECTURE ${CMAKE_HOST_SYSTEM_PROCESSOR})
endif()
string(TOLOWER "${LIBERTY_RECOMP_ARCHITECTURE}" LIBERTY_RECOMP_ARCHITECTURE)
message(STATUS "Detected architecture: ${LIBERTY_RECOMP_ARCHITECTURE}")

if (LIBERTY_RECOMP_ARCHITECTURE STREQUAL "x86_64" OR LIBERTY_RECOMP_ARCHITECTURE STREQUAL "amd64")
    # Baseline x86-64-v3 (AVX2, ~2013). Plain x86-64 (SSE2) does not compile
    # (the SDK uses SSSE3 intrinsics without per-function targets), and v2
    # proved too narrow in practice; v3 is still a generic baseline with no
    # chip name. No AVX intrinsics in first-party code (verified); SDL3 and
    # snappy gate their own per-file AVX paths. Override with
    # -DLIBERTY_RECOMP_ARCH=native (fastest local builds, non-portable).
    set(LIBERTY_RECOMP_ARCH "x86-64-v3" CACHE STRING "Value for -march on x86-64 (x86-64-v2/v3/v4, native, ...)")
    add_compile_options(
        -march=${LIBERTY_RECOMP_ARCH}
    )
endif()

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Normally only defined by Visual Studio, added for consistency
    add_compile_definitions(_DEBUG)
endif()

option(USE_REXGLUE_RUNTIME "Use RexGlue SDK for runtime infrastructure" ON)
option(LIBERTY_RECOMP_STRICT_WARNINGS "Strict warnings (-Wall -Wextra -pedantic-errors) for first-party targets (GCC/Clang only; vendored code keeps its own flags)" OFF)

# Strict warnings helper for first-party targets. Per-target (not directory
# scope) so generated or vendored sources sharing a directory are never
# affected. Nothing is silenced: warnings are fixed in the source instead.
# Usage: liberty_strict_warnings(<target>) after the target is created.
macro(liberty_strict_warnings TARGET)
    if(LIBERTY_RECOMP_STRICT_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(${TARGET} PRIVATE -Wall -Wextra -pedantic-errors)
    endif()
endmacro()

# Marks explicit vendored directories as -isystem for one first-party target,
# so strict warnings never fire inside headers we must not touch. Explicit
# directories (not target-property queries: those may hold generator
# expressions that split on ';'). Grows one call at a time, only when a build
# proves a new directory warns.
# Usage: liberty_systemize(<target> <dir> [<dir>...]).
macro(liberty_systemize TARGET)
    if(LIBERTY_RECOMP_STRICT_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        foreach(_d ${ARGN})
            # One SHELL item per directory (compile-step only via
            # COMPILE_LANGUAGE, so nothing leaks onto the link line).
            # SHELL items must stay unique per directory: CMake deduplicates
            # identical flag strings, which would leave all but one -isystem
            # without its directory.
            target_compile_options(${TARGET} PRIVATE
                "SHELL:$<$<COMPILE_LANGUAGE:C,CXX>:-isystem ${_d}>")
        endforeach()
    endif()
endmacro()

if(LIBERTY_RECOMP_GRAINE_MAC)
    if(NOT USE_REXGLUE_RUNTIME)
        message(FATAL_ERROR "The macOS build requires the Graine RexGlue runtime")
    endif()

    message(STATUS "macOS: building the pure Graine RexGlue consumer")
    add_subdirectory("glue")
    return()
endif()

if(LIBERTY_RECOMP_GRAINE_ANDROID)
    message(STATUS "Android: building the pure Graine RexGlue consumer")
    add_subdirectory("glue")
    return()
endif()

add_subdirectory(${LIBERTY_RECOMP_THIRDPARTY_ROOT})
# xbox_support: XEX/XDBF/big-endian helpers used by both desktop and console
# builds (previously sourced from the now-removed XenonRecomp tool).
set(THIRDPARTY_ROOT ${CMAKE_SOURCE_DIR}/third_party)
if (NOT TARGET disasm)
    add_subdirectory(${THIRDPARTY_ROOT}/disasm ${CMAKE_BINARY_DIR}/_xbox_disasm)
endif()
if (NOT TARGET xbox_support)
    add_subdirectory(${THIRDPARTY_ROOT}/xbox_support ${CMAKE_BINARY_DIR}/_xbox_support)
endif()

# Dev tools (u8extract, x_decompress, etc.) are host-only — skip for console cross-compiles
if(NOT DEFINED LIBERTY_RECOMP_TARGET_PLATFORM OR LIBERTY_RECOMP_TARGET_PLATFORM STREQUAL "desktop")
    add_subdirectory(${LIBERTY_RECOMP_TOOLS_ROOT})
else()
    # Console builds still need liberty_debugger (static lib linked into LibertyRecomp)
    add_subdirectory(${LIBERTY_RECOMP_TOOLS_ROOT}/liberty_debugger)
    # xbox_support + disasm already added above (desktop + console shared path).
    # Console builds also need zstd, xxHash — normally provided through XenosRecomp thirdparty chain
    # (fmt is provided by glue/rexglue-sdk thirdparty for console builds)
    set(XENOS_THIRDPARTY_ROOT ${LIBERTY_RECOMP_TOOLS_ROOT}/XenosRecomp/thirdparty)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "" FORCE)
    set(ZSTD_DISABLE_ASM ON CACHE BOOL "" FORCE)
    if (NOT TARGET libzstd_static)
        add_subdirectory(${XENOS_THIRDPARTY_ROOT}/zstd/build/cmake ${CMAKE_BINARY_DIR}/_zstd)
    endif()
    if (NOT TARGET xxHash::xxhash)
        set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
        add_subdirectory(${XENOS_THIRDPARTY_ROOT}/xxHash/cmake_unofficial ${CMAKE_BINARY_DIR}/_xxhash)
    endif()
endif()

# RexGlue SDK integration
if(USE_REXGLUE_RUNTIME)
    add_subdirectory("glue")
endif()

# GCC 15 miscompiles the vendored FFmpeg fork's NEG_SSR32/NEG_USR32 inline asm
# (it folds the shift count and emits `shr $-N`, which GAS rejects). The fork
# pins that code, so the GCC portability config builds libavcodec at -O0
# instead: identical semantics, slower DSP paths. Clang/default configs keep
# their flags. Applies to the C compiler actually used for those sources.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND TARGET libavcodec)
    target_compile_options(libavcodec PRIVATE -O0)
endif()

# Include sub-projects.
add_subdirectory("LibertyRecompLib")
add_subdirectory("LibertyRecomp")
