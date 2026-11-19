#include <catch2/catch_test_macros.hpp>
#include <khensu/common/Version.h>
#include <string>

TEST_CASE("Version constants are defined and correct", "[version]")
{
    REQUIRE(KHENSU_VERSION_MAJOR == 1);
    REQUIRE(KHENSU_VERSION_MINOR == 0);
    REQUIRE(KHENSU_VERSION_PATCH == 0);
    REQUIRE(std::string(KHENSU_VERSION_STRING) == "1.0.0");
}

TEST_CASE("Version string matches major.minor.patch", "[version]")
{
    const std::string expected = std::to_string(KHENSU_VERSION_MAJOR) + "." + std::to_string(KHENSU_VERSION_MINOR) +
                                 "." + std::to_string(KHENSU_VERSION_PATCH);
    REQUIRE(expected == KHENSU_VERSION_STRING);
}
