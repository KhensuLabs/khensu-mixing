# Compile JUCE once per configuration family (BLD-1, CE-ENG-3).
#
# JUCE modules are CMake INTERFACE libraries: every target that links one
# compiles the module's sources itself. Before this file, KhensuSharedCore
# linked the modules PUBLIC, so JUCE was compiled into seven targets per
# configuration (shared-core, the app, every test target and both plugin
# variants) - each copy with slightly different JUCE_* config macros.
#
# Now:
#   - KhensuSharedCore sees JUCE *headers only* (khensu_juce_headers).
#   - KhensuJuceApp   compiles the modules once for the desktop app.
#   - KhensuJuceTest  compiles them once for KhensuTests + KhensuCoreTests.
#   - The plugin targets (and KhensuPluginTests, which links the plugin's
#     shared code) keep linking the modules themselves: juce_add_plugin needs
#     the plugin-client config (JucePlugin_*), which a shared library can't
#     carry.
#
# The two desktop-family libraries differ in exactly one macro,
# JUCE_MODAL_LOOPS_PERMITTED. It is not just extra API: with it on, JUCE runs
# some menus/dialogs synchronously (e.g. a hosted VST3's context menu). The
# tests need it for MessageManager::runDispatchLoopUntil(); the shipped app
# keeps JUCE's default (off). One library for both would silently change the
# app's behaviour.

# The modules the desktop app and its tests link directly; their JUCE
# dependencies are added by _khensu_juce_closure (walked from JUCE's own link
# graph, so a JUCE update that adds a dependency - as 8.0 did with
# juce_audio_processors_headless - needs no edit here).
set(KHENSU_JUCE_ROOT_MODULES
    juce_gui_basics
    juce_gui_extra
    juce_audio_basics
    juce_audio_devices
    juce_audio_formats
    juce_audio_processors
    juce_audio_utils
    juce_dsp
    juce_opengl
    juce_osc
    juce_midi_ci  # STU-13: MIDI-CI property exchange (MidiCiResponder)
)

# ${out} = the given modules plus every JUCE module they depend on.
function(_khensu_juce_closure out)
    set(todo ${ARGN})
    set(seen)
    while(todo)
        list(POP_FRONT todo module)
        string(REGEX REPLACE "^juce::" "" module "${module}")
        if(module IN_LIST seen OR NOT TARGET juce::${module})
            continue()
        endif()
        list(APPEND seen ${module})
        get_target_property(deps juce::${module} INTERFACE_LINK_LIBRARIES)
        if(deps)
            foreach(dep IN LISTS deps)
                if(dep MATCHES "^(juce::)?juce_[a-z0-9_]+$"
                   AND NOT dep MATCHES "juce_recommended_")
                    list(APPEND todo ${dep})
                endif()
            endforeach()
        endif()
    endwhile()
    set(${out} ${seen} PARENT_SCOPE)
endfunction()

_khensu_juce_closure(KHENSU_JUCE_MODULES ${KHENSU_JUCE_ROOT_MODULES})
message(STATUS "KhensuJuce modules: ${KHENSU_JUCE_MODULES}")

# JUCE config shared by the app and its tests (was KhensuDesktop's set).
set(KHENSU_JUCE_CONFIG_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_LAME_AUDIO_FORMAT=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=1
)

# Give ${target} the headers and JUCE_MODULE_AVAILABLE_* definitions of the
# given modules and their dependencies - but not the module sources - under
# ${scope} (PUBLIC / PRIVATE / INTERFACE).
function(khensu_juce_headers target scope)
    _khensu_juce_closure(modules ${ARGN})
    foreach(module IN LISTS modules)
        target_include_directories(${target} ${scope}
            $<TARGET_PROPERTY:juce::${module},INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_definitions(${target} ${scope}
            $<TARGET_PROPERTY:juce::${module},INTERFACE_COMPILE_DEFINITIONS>)
    endforeach()
    target_compile_features(${target} ${scope} cxx_std_17)
endfunction()

# A static library holding one compiled copy of KHENSU_JUCE_MODULES.
function(khensu_add_juce_library name modal_loops)
    add_library(${name} STATIC)

    set(module_targets)
    foreach(module IN LISTS KHENSU_JUCE_MODULES)
        list(APPEND module_targets juce::${module})
    endforeach()

    # PRIVATE: the module sources are compiled here, once. Consumers only get
    # the link-time part ($<LINK_ONLY:...>), never the sources again.
    target_link_libraries(${name}
        PRIVATE
            ${module_targets}
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags)

    khensu_juce_headers(${name} INTERFACE ${KHENSU_JUCE_MODULES})

    # PUBLIC so every translation unit that includes JUCE headers in a
    # consumer sees the same config the library was compiled with.
    target_compile_definitions(${name} PUBLIC
        ${KHENSU_JUCE_CONFIG_DEFINITIONS}
        JUCE_MODAL_LOOPS_PERMITTED=${modal_loops})

    # Same flags the consumers compiled JUCE with before (/permissive-,
    # /Zc:__cplusplus and /utf-8 change JUCE's feature detection and string
    # handling, so they must match).
    khensu_apply_compiler_flags(${name} COMPILES_JUCE)
endfunction()

khensu_add_juce_library(KhensuJuceApp 0)
if(KHENSU_BUILD_TESTS)
    khensu_add_juce_library(KhensuJuceTest 1)
endif()
