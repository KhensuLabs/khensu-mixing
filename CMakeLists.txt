cmake_minimum_required(VERSION 3.24)

# macOS universal binary (v1.1). These MUST be set before project() so CMake's
# compiler/ABI detection runs for both slices; the APPLE variable isn't defined
# until after project(), so gate on the host instead. Ignored on non-Apple hosts.
# Left as overridable defaults (no FORCE) so a single-arch dev build is possible
# via -DCMAKE_OSX_ARCHITECTURES=arm64.
if(CMAKE_HOST_APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build universal binary")
endif()

project(KhensuMixingSuite VERSION 1.0.5 LANGUAGES C CXX)  # C: SQLite and other bundled C sources

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# BLD-2: a compiler cache where the generator can use one (Ninja / Makefiles;
# the Visual Studio generator ignores compiler launchers). sccache first, then
# ccache; nothing changes when neither is installed.
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio" AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(KHENSU_COMPILER_CACHE NAMES sccache ccache)
    if(KHENSU_COMPILER_CACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${KHENSU_COMPILER_CACHE}")
        message(STATUS "Compiler cache: ${KHENSU_COMPILER_CACHE}")
    endif()
endif()

# SHIP-1: every target, fetched dependencies included, links the C runtime
# statically, so neither the app nor the plugin needs the VC++
# redistributable on the user's machine. A plugin especially: its DLL is
# loaded by a host whose folder (not ours) is searched for vcruntime140.dll.
# Set before any target exists (policy CMP0091 is NEW from 3.15).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# SHIP-7: a packaging build can skip the three Catch2 suites (no Catch2
# fetch, no test copy of JUCE): -DKHENSU_BUILD_TESTS=OFF.
option(KHENSU_BUILD_TESTS "Build the Catch2 test suites" ON)
option(KHENSU_CONVERSION_WARNINGS "Warn on narrowing conversions (C4244/C4267) in project code (SHIP-6)" ON)

# Fetch all external dependencies
include(cmake/VerifyPin.cmake)  # SHIP-5: fetched dependencies are checked against reviewed commits
include(cmake/FetchJUCE.cmake)
include(cmake/FetchSQLiteCpp.cmake)
if(KHENSU_BUILD_TESTS)
    include(cmake/FetchCatch2.cmake)
endif()
include(cmake/CompilerFlags.cmake)
include(cmake/KhensuJuce.cmake)   # JUCE compiled once per family (BLD-1)

# Generate the SQLite migration array from storage/schema/*.sql (audit ARCH-07:
# single source of truth). Must run before shared-core is configured so the
# generated .inc exists for SessionManager.cpp.
include(cmake/GenerateMigrations.cmake)

# Windows: modern API + Unicode (must precede all target definitions)
if(WIN32)
    add_compile_definitions(
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00   # Windows 10+
    )
endif()

add_subdirectory(shared-core)
add_subdirectory(desktop-app)
add_subdirectory(plugin-copilot)
if(KHENSU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
