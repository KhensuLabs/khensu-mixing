# SHIP-5: a fetched dependency must be the commit we reviewed, not whatever
# its tag points at today. The Fetch*.cmake files keep fetching by tag (a
# shallow clone needs a ref name) and then call this, which fails configure
# when the checkout is any other commit - a moved or re-pushed tag stops the
# build instead of compiling unreviewed code.
#
#   khensu_verify_pin(<name> <source dir> <full commit sha>)
#
# A source supplied with FETCHCONTENT_SOURCE_DIR_<NAME> that is not a git
# checkout only warns: that is a deliberate local override.
function(khensu_verify_pin name source_dir expected_sha)
    find_package(Git QUIET)
    if(NOT GIT_FOUND OR NOT EXISTS "${source_dir}/.git")
        message(WARNING "${name}: cannot verify the pinned commit ${expected_sha} "
                        "(no git checkout at ${source_dir})")
        return()
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
        OUTPUT_VARIABLE _khensu_head
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _khensu_rc
        ERROR_QUIET)
    if(NOT _khensu_rc EQUAL 0)
        message(WARNING "${name}: git rev-parse failed in ${source_dir}; pin not verified")
        return()
    endif()
    if(NOT _khensu_head STREQUAL expected_sha)
        message(FATAL_ERROR "${name}: ${source_dir} is at ${_khensu_head}, but the reviewed pin is "
                            "${expected_sha}. The tag moved or the checkout is stale. Review the "
                            "change and update the pin in cmake/, or delete the source directory "
                            "and configure again.")
    endif()
endfunction()
