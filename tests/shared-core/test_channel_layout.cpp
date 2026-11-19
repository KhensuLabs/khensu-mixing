#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/analysis/ChannelLayout.h>

#include <string>

using namespace khensu;

TEST_CASE("ChannelLayout: supported channel counts resolve", "[layout]")
{
    struct Case {
        int ch;
        LayoutId id;
        const char* name;
    };
    const Case cases[] = {
        {1, LayoutId::Mono, "Mono"},
        {2, LayoutId::Stereo, "Stereo"},
        {6, LayoutId::Surround51, "5.1"},
        {8, LayoutId::Surround71, "7.1"},
    };
    for (const auto& c : cases) {
        auto r = ChannelLayout::fromChannelCount(c.ch);
        INFO("channel count " << c.ch);
        REQUIRE(r.is_ok());
        CHECK(r.value().numChannels == c.ch);
        CHECK(r.value().id == c.id);
        CHECK(std::string(r.value().name) == std::string(c.name));
    }
}

TEST_CASE("ChannelLayout: unsupported channel counts are rejected", "[layout]")
{
    for (int ch : {0, -1, 3, 4, 5, 7, 9, 64}) {
        auto r = ChannelLayout::fromChannelCount(ch);
        INFO("channel count " << ch);
        REQUIRE(r.is_err());
        // The message must name the count so the user can act on it.
        CHECK(r.error().find(std::to_string(ch)) != std::string::npos);
    }
}

TEST_CASE("ChannelLayout: 5.1 role order is SMPTE (L R C LFE Ls Rs)", "[layout]")
{
    auto r = ChannelLayout::fromChannelCount(6);
    REQUIRE(r.is_ok());
    const auto& L = r.value();
    CHECK(L.roles[0] == ChannelRole::Left);
    CHECK(L.roles[1] == ChannelRole::Right);
    CHECK(L.roles[2] == ChannelRole::Centre);
    CHECK(L.roles[3] == ChannelRole::LFE);
    CHECK(L.roles[4] == ChannelRole::LeftSurround);
    CHECK(L.roles[5] == ChannelRole::RightSurround);
    CHECK(L.hasLFE());
    CHECK(L.isLFE(3));
    CHECK_FALSE(L.isLFE(0));
}

TEST_CASE("ChannelLayout: 7.1 adds the back pair", "[layout]")
{
    auto r = ChannelLayout::fromChannelCount(8);
    REQUIRE(r.is_ok());
    const auto& L = r.value();
    CHECK(L.roles[6] == ChannelRole::LeftBack);
    CHECK(L.roles[7] == ChannelRole::RightBack);
    CHECK(L.isLFE(3));
}

TEST_CASE("ChannelLayout: BS.1770 weights are power-domain", "[layout]")
{
    auto r = ChannelLayout::fromChannelCount(6);
    REQUIRE(r.is_ok());
    const auto& L = r.value();
    // L, R, C are unity.
    CHECK_THAT(L.bs1770Weight(0), Catch::Matchers::WithinAbs(1.0, 1e-12));
    CHECK_THAT(L.bs1770Weight(1), Catch::Matchers::WithinAbs(1.0, 1e-12));
    CHECK_THAT(L.bs1770Weight(2), Catch::Matchers::WithinAbs(1.0, 1e-12));
    // LFE is excluded outright.
    CHECK_THAT(L.bs1770Weight(3), Catch::Matchers::WithinAbs(0.0, 1e-12));
    // Surround is +1.5 dB in the power domain: 10^(1.5/10).
    CHECK_THAT(L.bs1770Weight(4), Catch::Matchers::WithinAbs(1.4125375446, 1e-9));
    CHECK_THAT(L.bs1770Weight(5), Catch::Matchers::WithinAbs(1.4125375446, 1e-9));
}

TEST_CASE("ChannelLayout: mono and stereo are all-unity, no LFE", "[layout]")
{
    for (int ch : {1, 2}) {
        auto r = ChannelLayout::fromChannelCount(ch);
        INFO("channel count " << ch);
        REQUIRE(r.is_ok());
        const auto& L = r.value();
        CHECK_FALSE(L.hasLFE());
        for (int c = 0; c < ch; ++c)
            CHECK_THAT(L.bs1770Weight(c), Catch::Matchers::WithinAbs(1.0, 1e-12));
    }
}

TEST_CASE("ChannelLayout: front pair indices", "[layout]")
{
    CHECK(ChannelLayout::fromChannelCount(1).value().frontLeft() == 0);
    CHECK(ChannelLayout::fromChannelCount(1).value().frontRight() == 0); // mono folds
    CHECK(ChannelLayout::fromChannelCount(6).value().frontLeft() == 0);
    CHECK(ChannelLayout::fromChannelCount(6).value().frontRight() == 1);
}

TEST_CASE("ChannelLayout: unspecified fallback preserves legacy unity weighting", "[layout]")
{
    // Used only by the backward-compatible count-based reset overloads for a
    // count AudioAnalyzer would already have rejected. All-unity, no LFE, so
    // it reproduces the pre-contract behaviour exactly.
    const auto L = ChannelLayout::unspecified(4);
    CHECK(L.id == LayoutId::Unspecified);
    CHECK(L.numChannels == 4);
    CHECK_FALSE(L.hasLFE());
    for (int c = 0; c < 4; ++c)
        CHECK_THAT(L.bs1770Weight(c), Catch::Matchers::WithinAbs(1.0, 1e-12));
}
