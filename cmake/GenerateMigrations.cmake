# GenerateMigrations.cmake (audit ARCH-07)
#
# Makes storage/schema/NNN_*.sql the SINGLE source of truth for the SQLite
# schema. Previously the runtime migration list was a hand-maintained C++
# array in SessionManager.cpp AND the .sql files were hand-copied to match,
# kept in sync only by a parity test. This generates the C++ array *from* the
# .sql files at configure time, so the two can no longer drift - editing a
# .sql file is the only place a migration is defined.
#
# Output: ${CMAKE_BINARY_DIR}/generated/khensu_migrations.inc, a sequence of
#   { <version>, R"SQL(<file contents>)SQL" },
# entries that SessionManager.cpp #includes into its migrations() vector.

set(_KHENSU_SCHEMA_DIR "${CMAKE_SOURCE_DIR}/storage/schema")
set(_KHENSU_MIGRATIONS_INC "${CMAKE_BINARY_DIR}/generated/khensu_migrations.inc")

# CONFIGURE_DEPENDS: re-glob (pick up added/removed files) when the build runs.
file(GLOB _KHENSU_MIGRATION_SQL CONFIGURE_DEPENDS "${_KHENSU_SCHEMA_DIR}/*.sql")
list(SORT _KHENSU_MIGRATION_SQL)

if(_KHENSU_MIGRATION_SQL STREQUAL "")
    message(FATAL_ERROR "ARCH-07: no migration files found in ${_KHENSU_SCHEMA_DIR}")
endif()

set(_gen "// AUTO-GENERATED from storage/schema/*.sql by cmake/GenerateMigrations.cmake.\n")
string(APPEND _gen "// DO NOT EDIT - edit the .sql files instead (audit ARCH-07: single source of truth).\n")

set(_expected 1)
foreach(_sqlfile ${_KHENSU_MIGRATION_SQL})
    get_filename_component(_fname "${_sqlfile}" NAME)
    if(NOT _fname MATCHES "^([0-9]+)_")
        message(FATAL_ERROR "ARCH-07: migration file '${_fname}' must start with NNN_ (a version number).")
    endif()
    # Strip leading zeros so 001 -> 1, 010 -> 10.
    string(REGEX REPLACE "^0*([0-9]+)_.*$" "\\1" _ver "${_fname}")
    if(NOT _ver EQUAL _expected)
        message(FATAL_ERROR
            "ARCH-07: migrations must be contiguous starting at 1 - expected version ${_expected} "
            "but '${_fname}' is version ${_ver}.")
    endif()
    math(EXPR _expected "${_expected} + 1")

    # Re-run configure when a file's CONTENTS change (glob only tracks the set).
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sqlfile}")

    file(READ "${_sqlfile}" _sqlcontent)
    # Raw string literal: no escaping needed. The )SQL" delimiter is safe as
    # long as no .sql file contains that exact sequence (none do, and any that
    # did would be a build error here rather than silent corruption).
    if(_sqlcontent MATCHES "\\)SQL\"")
        message(FATAL_ERROR "ARCH-07: '${_fname}' contains the raw-string delimiter )SQL\" - rename it.")
    endif()
    string(APPEND _gen "{ ${_ver}, R\"SQL(\n${_sqlcontent})SQL\" },\n")
endforeach()

# file(GENERATE) writes at the end of configure, before any compilation, and
# only rewrites when the content actually changes (no spurious rebuilds).
file(GENERATE OUTPUT "${_KHENSU_MIGRATIONS_INC}" CONTENT "${_gen}")

# The directory the .inc lands in, for target include paths.
set(KHENSU_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated" CACHE INTERNAL "ARCH-07 generated headers")
