// tests/shared-core/test_vocal_macro_mapper.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/mixerchain/VocalMacroMapper.h>

using namespace khensu::mixerchain;
using Catch::Matchers::WithinAbs;

namespace {
const khensu::ipc::HttpServiceBridge::EffectSpec*
find(const std::vector<khensu::ipc::HttpServiceBridge::EffectSpec>& chain, const char* name)
{
    for (auto& e : chain)
        if (e.effect == name) return &e;
    return nullptr;
}
} // namespace

TEST_CASE("buildVocalMacroChain omits zero-valued macros", "[vocal_macro_mapper]")
{
    VocalMacroValues v{}; // all zero
    auto chain = buildVocalMacroChain(v);
    REQUIRE(chain.empty());
}

TEST_CASE("buildVocalMacroChain emits one effect per non-zero macro, in signal order", "[vocal_macro_mapper]")
{
    VocalMacroValues v{};
    v.compression = 50;
    v.presence = 50;
    v.width = 50;
    v.deEssing = 50;
    v.saturation = 50;
    v.reverb = 50;
    v.delay = 50;
    auto chain = buildVocalMacroChain(v);

    REQUIRE(chain.size() == 7);
    const char* expectedOrder[7] = {"DeEsser",       "Compressor", "PeakFilter", "TapeSaturation",
                                    "StereoWidener", "Reverb",     "Delay"};
    for (int i = 0; i < 7; ++i)
        CHECK(chain[(size_t)i].effect == juce::String(expectedOrder[i]));
}

TEST_CASE("buildVocalMacroChain: Compressor threshold/ratio match the label formula", "[vocal_macro_mapper]")
{
    VocalMacroValues v{};
    v.compression = 100; // norm = 1.0
    auto chain = buildVocalMacroChain(v);
    auto* comp = find(chain, "Compressor");
    REQUIRE(comp != nullptr);
    // threshold_db = -10 - norm^1.5*20 = -30 at norm=1
    CHECK_THAT(comp->parameters.at("threshold_db"), WithinAbs(-30.0f, 0.01f));
    // ratio = 2 + norm^1.2*6 = 8 at norm=1
    CHECK_THAT(comp->parameters.at("ratio"), WithinAbs(8.0f, 0.01f));
}

TEST_CASE("buildVocalMacroChain: all emitted parameter values stay within dsp.py's documented clamp ranges",
          "[vocal_macro_mapper]")
{
    VocalMacroValues v{};
    v.compression = 100;
    v.presence = 100;
    v.width = 100;
    v.deEssing = 100;
    v.saturation = 100;
    v.reverb = 100;
    v.delay = 100;
    auto chain = buildVocalMacroChain(v);

    auto* comp = find(chain, "Compressor");
    CHECK(comp->parameters.at("threshold_db") >= -60.0f);
    CHECK(comp->parameters.at("threshold_db") <= 0.0f);
    CHECK(comp->parameters.at("ratio") >= 1.0f);
    CHECK(comp->parameters.at("ratio") <= 30.0f);

    auto* peak = find(chain, "PeakFilter");
    CHECK(peak->parameters.at("gain_db") >= -24.0f);
    CHECK(peak->parameters.at("gain_db") <= 24.0f);
    CHECK(peak->parameters.at("cutoff_frequency_hz") >= 20.0f);
    CHECK(peak->parameters.at("cutoff_frequency_hz") <= 20000.0f);

    auto* de = find(chain, "DeEsser");
    CHECK(de->parameters.at("threshold_db") >= -60.0f);
    CHECK(de->parameters.at("threshold_db") <= 0.0f);
    CHECK(de->parameters.at("ratio") >= 1.0f);
    CHECK(de->parameters.at("ratio") <= 20.0f);
    CHECK(de->parameters.at("frequency_hz") >= 3000.0f);
    CHECK(de->parameters.at("frequency_hz") <= 10000.0f);

    auto* sat = find(chain, "TapeSaturation");
    CHECK(sat->parameters.at("drive_db") >= 0.0f);
    CHECK(sat->parameters.at("drive_db") <= 24.0f);
    CHECK(sat->parameters.at("warmth") >= 0.0f);
    CHECK(sat->parameters.at("warmth") <= 1.0f);
    CHECK(sat->parameters.at("mix") >= 0.0f);
    CHECK(sat->parameters.at("mix") <= 1.0f);

    auto* wide = find(chain, "StereoWidener");
    CHECK(wide->parameters.at("width") >= 0.0f);
    CHECK(wide->parameters.at("width") <= 2.0f);

    auto* rev = find(chain, "Reverb");
    for (const char* p : {"room_size", "damping", "wet_level", "dry_level"}) {
        CHECK(rev->parameters.at(p) >= 0.0f);
        CHECK(rev->parameters.at(p) <= 1.0f);
    }

    auto* del = find(chain, "Delay");
    CHECK(del->parameters.at("delay_seconds") >= 0.001f);
    CHECK(del->parameters.at("delay_seconds") <= 4.0f);
    CHECK(del->parameters.at("feedback") >= 0.0f);
    CHECK(del->parameters.at("feedback") <= 0.95f);
    CHECK(del->parameters.at("mix") >= 0.0f);
    CHECK(del->parameters.at("mix") <= 1.0f);
}
