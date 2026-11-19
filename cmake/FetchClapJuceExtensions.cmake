# cmake/FetchClapJuceExtensions.cmake - free-audio/clap-juce-extensions (MIT)
# adds a CLAP format next to the VST3 (CE-DAW-2).
#
# Sets KHENSU_CLAP_AVAILABLE. Like the ARA SDK, a failed fetch never fails the
# configure: it prints why and the CLAP target is skipped.
#
# Pinned to a commit, not a tag: the last tag (0.26.0, 2022) predates JUCE 8's
# juce_audio_processors_headless split; JUCE 8.0.11+ support landed in
# 645ed2f (2025-12). 6bde108 (2026-11-06) also makes the wrapper request a
# parameter rescan after state load, which clap-validator's
# state-reproducibility tests require.

option(KHENSU_BUILD_CLAP "Build the CLAP format of the Copilot (clap-juce-extensions)" ON)
set(KHENSU_CLAP_JUCE_EXT_COMMIT "6bde108329ff9fc24404b79b8b397261a75cf6a8" CACHE STRING
    "clap-juce-extensions commit")
set(KHENSU_CLAP_AVAILABLE OFF)

if(NOT KHENSU_BUILD_CLAP)
    message(STATUS "CLAP: KHENSU_BUILD_CLAP is OFF - no .clap is built")
    return()
endif()

include(FetchContent)

set(_khensu_clap_url "https://github.com/free-audio/clap-juce-extensions.git")
if(FETCHCONTENT_BASE_DIR)
    set(_khensu_clap_cached "${FETCHCONTENT_BASE_DIR}/clap_juce_extensions-src")
else()
    set(_khensu_clap_cached "${CMAKE_BINARY_DIR}/_deps/clap_juce_extensions-src")
endif()

set(_khensu_clap_can_populate OFF)
if(FETCHCONTENT_SOURCE_DIR_CLAP_JUCE_EXTENSIONS AND EXISTS "${FETCHCONTENT_SOURCE_DIR_CLAP_JUCE_EXTENSIONS}/CMakeLists.txt")
    set(_khensu_clap_can_populate ON)
elseif(EXISTS "${_khensu_clap_cached}/clap-libs/clap/include")
    set(_khensu_clap_can_populate ON)
elseif(FETCHCONTENT_FULLY_DISCONNECTED)
    message(WARNING "CLAP: FETCHCONTENT_FULLY_DISCONNECTED is set and no cached copy exists - CLAP skipped.")
else()
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        message(WARNING "CLAP: git was not found - CLAP skipped.")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" ls-remote "${_khensu_clap_url}" HEAD
            RESULT_VARIABLE _khensu_clap_probe
            OUTPUT_QUIET ERROR_QUIET
            TIMEOUT 30)
        if(_khensu_clap_probe EQUAL 0)
            set(_khensu_clap_can_populate ON)
        else()
            message(WARNING "CLAP: could not reach ${_khensu_clap_url} (offline?) - CLAP skipped. "
                            "Re-run configure when online.")
        endif()
    endif()
endif()

if(_khensu_clap_can_populate)
    FetchContent_Declare(
        clap_juce_extensions
        GIT_REPOSITORY "${_khensu_clap_url}"
        GIT_TAG        "${KHENSU_CLAP_JUCE_EXT_COMMIT}"
        GIT_SUBMODULES "clap-libs/clap" "clap-libs/clap-helpers"
        UPDATE_DISCONNECTED TRUE)
    FetchContent_MakeAvailable(clap_juce_extensions)
    if(COMMAND clap_juce_extensions_plugin)
        set(KHENSU_CLAP_AVAILABLE ON)
        message(STATUS "CLAP: clap-juce-extensions ${KHENSU_CLAP_JUCE_EXT_COMMIT}")
    else()
        message(WARNING "CLAP: clap-juce-extensions did not define clap_juce_extensions_plugin - CLAP skipped.")
    endif()
endif()
