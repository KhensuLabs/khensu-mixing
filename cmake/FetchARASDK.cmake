# cmake/FetchARASDK.cmake - Celemony ARA SDK (Apache-2.0) for the ARA2 Copilot
# variant (CE-DAW-1, target KhensuCopilotARA).
#
# Sets KHENSU_ARA_AVAILABLE to ON when the SDK is present and registered with
# JUCE (juce_set_ara_sdk_path). Configure never fails because of the SDK: when
# the fetch cannot run (offline, git missing, FETCHCONTENT_FULLY_DISCONNECTED
# without a cached copy) it prints why and the ARA target is skipped. The
# regular KhensuCopilot target never depends on any of this.
#
# Only the ARA_API and ARA_Library submodules are needed by JUCE; the large
# ARA_Examples submodule is not cloned.

option(KHENSU_BUILD_ARA "Build the ARA2 Copilot variant (KhensuCopilotARA)" ON)
set(KHENSU_ARA_SDK_TAG "releases/2.3.0" CACHE STRING "ARA SDK git tag (Celemony/ARA_SDK)")
set(KHENSU_ARA_AVAILABLE OFF)

if(NOT KHENSU_BUILD_ARA)
    message(STATUS "ARA: KHENSU_BUILD_ARA is OFF - KhensuCopilotARA is not configured")
    return()
endif()

include(FetchContent)

set(_khensu_ara_url "https://github.com/Celemony/ARA_SDK.git")
if(FETCHCONTENT_BASE_DIR)
    set(_khensu_ara_cached "${FETCHCONTENT_BASE_DIR}/ara_sdk-src")
else()
    set(_khensu_ara_cached "${CMAKE_BINARY_DIR}/_deps/ara_sdk-src")
endif()

set(_khensu_ara_can_populate OFF)
if(FETCHCONTENT_SOURCE_DIR_ARA_SDK AND EXISTS "${FETCHCONTENT_SOURCE_DIR_ARA_SDK}/ARA_API")
    set(_khensu_ara_can_populate ON)
elseif(EXISTS "${_khensu_ara_cached}/ARA_API/ARAInterface.h" AND EXISTS "${_khensu_ara_cached}/ARA_Library")
    set(_khensu_ara_can_populate ON)
elseif(FETCHCONTENT_FULLY_DISCONNECTED)
    message(WARNING "ARA: FETCHCONTENT_FULLY_DISCONNECTED is set and no cached ARA SDK exists - "
                    "KhensuCopilotARA is skipped.")
else()
    # Probe first: a FetchContent clone failure is a fatal configure error,
    # and an offline machine must still configure the rest of the project.
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        message(WARNING "ARA: git was not found - KhensuCopilotARA is skipped.")
    else()
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" ls-remote --tags "${_khensu_ara_url}" "${KHENSU_ARA_SDK_TAG}"
            RESULT_VARIABLE _khensu_ara_probe
            OUTPUT_VARIABLE _khensu_ara_probe_out
            ERROR_QUIET
            TIMEOUT 30)
        if(_khensu_ara_probe EQUAL 0 AND _khensu_ara_probe_out MATCHES "refs/tags/")
            set(_khensu_ara_can_populate ON)
        else()
            message(WARNING "ARA: could not reach ${_khensu_ara_url} (offline?) - "
                            "KhensuCopilotARA is skipped. Re-run configure when online.")
        endif()
    endif()
endif()

if(_khensu_ara_can_populate)
    FetchContent_Declare(
        ara_sdk
        GIT_REPOSITORY "${_khensu_ara_url}"
        GIT_TAG        "${KHENSU_ARA_SDK_TAG}"
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES "ARA_API" "ARA_Library"
        UPDATE_DISCONNECTED TRUE
        # The SDK root has no CMakeLists; point SOURCE_SUBDIR at a path that
        # never exists so MakeAvailable only populates (no add_subdirectory).
        SOURCE_SUBDIR  "_khensu_populate_only")
    FetchContent_MakeAvailable(ara_sdk)
    # releases/2.3.0's commit (SHIP-5); another tag needs its own reviewed pin.
    if(KHENSU_ARA_SDK_TAG STREQUAL "releases/2.3.0")
        khensu_verify_pin(ARA_SDK "${ara_sdk_SOURCE_DIR}" 0005c532bb03eb210e7bb490329fc6d7ba1792c7)
    endif()

    if(EXISTS "${ara_sdk_SOURCE_DIR}/ARA_API/ARAInterface.h")
        juce_set_ara_sdk_path("${ara_sdk_SOURCE_DIR}")
        set(KHENSU_ARA_AVAILABLE ON)
        message(STATUS "ARA: SDK ${KHENSU_ARA_SDK_TAG} at ${ara_sdk_SOURCE_DIR}")
    else()
        message(WARNING "ARA: the fetched SDK at ${ara_sdk_SOURCE_DIR} is incomplete "
                        "(ARA_API submodule missing) - KhensuCopilotARA is skipped.")
    endif()
endif()
