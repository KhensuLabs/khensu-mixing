include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    # Pin the exact stable tag for reproducible local and CI builds. JUCE
    # 8.0.11 introduced the high-level UMP Session/Input/Output API used by the
    # desktop MIDI 2.0 path; 8.0.14 includes subsequent MIDI fixes.
    GIT_TAG        8.0.14
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(JUCE)
# 8.0.14's commit (SHIP-5): a moved tag fails configure.
khensu_verify_pin(JUCE "${juce_SOURCE_DIR}" 2cdfca8feb300fb424002ba2c2751569e5bacb64)
