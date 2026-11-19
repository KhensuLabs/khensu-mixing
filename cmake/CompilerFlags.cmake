# Debug information goes into the object files (/Z7), not a shared .pdb (/Zi).
# With /Zi every cl.exe of a project writes one .pdb through mspdbsrv.exe, and
# under /MP that is where a Debug build hung for about four hours on
# 2026-11-09 (VER-2). Release carries no debug information and is unchanged.
# The cache is rewritten (FORCE) so that an existing tree, the fetched
# dependencies and a tree configured with -DCMAKE_CXX_FLAGS_DEBUG=... all get
# it. Include this file after the Fetch*.cmake files, which enable C.
if(MSVC)
    foreach(_khensu_cfg DEBUG RELWITHDEBINFO)
        foreach(_khensu_lang C CXX)
            set(_khensu_var CMAKE_${_khensu_lang}_FLAGS_${_khensu_cfg})
            if(DEFINED ${_khensu_var} AND "${${_khensu_var}}" MATCHES "[/-]Z[iI]")
                string(REGEX REPLACE "[/-]Z[iI]" "/Z7" _khensu_flags "${${_khensu_var}}")
                set(${_khensu_var} "${_khensu_flags}" CACHE STRING
                    "Flags used by the compiler during ${_khensu_cfg} builds (/Z7: VER-2)" FORCE)
            endif()
        endforeach()
    endforeach()
    unset(_khensu_var)
    unset(_khensu_flags)

    # SHIP-2: Release carries debug information too, so the minidumps
    # CrashReporter / PluginCrashHandler write can be read. /Z7 keeps it in the
    # objects (no mspdbsrv, as above); /DEBUG makes every linked binary write
    # its .pdb next to it, and /OPT:REF /OPT:ICF keep the binary as small as a
    # build without /DEBUG. The installer excludes *.pdb;
    # scripts/archive_symbols.ps1 keeps each build's PDBs.
    foreach(_khensu_lang C CXX)
        set(_khensu_var CMAKE_${_khensu_lang}_FLAGS_RELEASE)
        if(DEFINED ${_khensu_var} AND NOT "${${_khensu_var}}" MATCHES "/Z7")
            set(${_khensu_var} "${${_khensu_var}} /Z7" CACHE STRING
                "Flags used by the compiler during RELEASE builds (/Z7: SHIP-2)" FORCE)
        endif()
    endforeach()
    foreach(_khensu_kind EXE SHARED MODULE)
        set(_khensu_var CMAKE_${_khensu_kind}_LINKER_FLAGS_RELEASE)
        if(NOT "${${_khensu_var}}" MATCHES "/DEBUG")
            set(${_khensu_var} "${${_khensu_var}} /DEBUG /OPT:REF /OPT:ICF" CACHE STRING
                "Flags used by the linker during RELEASE builds (/DEBUG: SHIP-2)" FORCE)
        endif()
    endforeach()
    unset(_khensu_var)
endif()

# khensu_harden_link(<target>): a linked binary opts in to Control Flow Guard
# and CET shadow stacks (x64 only) (SHIP-6). No-op for libraries that are not
# linked. JUCE's plugin format targets (<name>_VST3, _CLAP, ...) link the
# shipped binary but never pass through khensu_apply_compiler_flags, so the
# plugin CMakeLists calls this for each of them.
function(khensu_harden_link target)
    if(NOT MSVC)
        return()
    endif()
    get_target_property(_khensu_type ${target} TYPE)
    if(_khensu_type STREQUAL "STATIC_LIBRARY" OR _khensu_type STREQUAL "INTERFACE_LIBRARY"
       OR _khensu_type STREQUAL "OBJECT_LIBRARY" OR _khensu_type STREQUAL "UTILITY")
        return()
    endif()
    target_link_options(${target} PRIVATE /guard:cf)
    if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
        target_link_options(${target} PRIVATE /CETCOMPAT)
    endif()
endfunction()

# khensu_conversion_warnings(<source>...)
# Turns C4244 / C4267 back on for our own sources inside a target that also
# compiles JUCE's (the plugin and its tests), where the target-wide flags keep
# them off (SHIP-6). Call it from the directory that adds the sources.
function(khensu_conversion_warnings)
    if(MSVC AND KHENSU_CONVERSION_WARNINGS)
        set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS
            /w44244 /w44267 /external:anglebrackets /external:W0)
    endif()
endfunction()

# khensu_apply_compiler_flags(<target> [COMPILES_JUCE])
# COMPILES_JUCE marks a target that compiles JUCE's module sources (and the
# VST3 SDK inside them): /sdl makes the SDK's strcpy / sscanf deprecations
# errors there, so those targets define _CRT_SECURE_NO_WARNINGS (SHIP-6).
function(khensu_apply_compiler_flags target)
    cmake_parse_arguments(ARG "COMPILES_JUCE" "" "" ${ARGN})
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(MSVC AND ARG_COMPILES_JUCE)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /WX              # Treat warnings as errors
            /permissive-     # Strict standard conformance
            /Zc:__cplusplus  # Report correct __cplusplus value (JUCE feature detection needs this)
            # Multi-processor compilation. Debug is capped at 4 compilers per
            # project: a bare /MP starts one per logical core in every project
            # MSBuild is building, and Debug objects are the large ones (VER-2).
            # Release keeps /MP so its command lines, and so its objects, are
            # unchanged.
            $<IF:$<CONFIG:Debug>,/MP4,/MP>
            /utf-8           # Treat source AND execution charset as UTF-8 so literal em-dashes,
                             # ellipses, etc. survive to juce::String (else they render as "â…")
            # Suppress JUCE-specific noisy warnings
            /wd4100          # unreferenced formal parameter
            /wd4127          # conditional expression is constant
            /wd4458          # declaration hides class member (noisy in Steinberg VST3 SDK)
            # SHIP-6 hardening: Control Flow Guard and the SDL checks (extra
            # warnings as errors, pointer sanitisation, /GS).
            /guard:cf
            /sdl
        )
        # Narrowing conversions (C4244 double/float/int, C4267 size_t/int).
        # SHIP-6: on for project code; JUCE's own sources and the SDKs they
        # compile are not ours to sweep, and JUCE's headers (included with
        # angle brackets) are external, so they stay quiet everywhere.
        if(ARG_COMPILES_JUCE OR NOT KHENSU_CONVERSION_WARNINGS)
            target_compile_options(${target} PRIVATE /wd4244 /wd4267)
        else()
            target_compile_options(${target} PRIVATE /external:anglebrackets /external:W0)
        endif()
        khensu_harden_link(${target})
    else()
        # GCC/Clang — macOS v1.1, Linux future
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-unused-parameter
        )
        if(APPLE)
            target_compile_options(${target} PRIVATE -Wno-deprecated-declarations)
        endif()
    endif()
endfunction()
