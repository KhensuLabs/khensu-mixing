include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.3
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(Catch2)
# v3.5.3's commit (SHIP-5).
khensu_verify_pin(Catch2 "${catch2_SOURCE_DIR}" 8ac8190e494a381072c89f5e161b92a08d98b37b)
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
