include(FetchContent)

FetchContent_Declare(
    SQLiteCpp
    GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
    GIT_TAG        3.3.1
    GIT_SHALLOW    TRUE
)

set(SQLITECPP_RUN_CPPLINT   OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK  OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SQLiteCpp)
# 3.3.1's commit (SHIP-5).
khensu_verify_pin(SQLiteCpp "${sqlitecpp_SOURCE_DIR}" fe6086c12679d8744c02fa03e83e75feca2413d8)
