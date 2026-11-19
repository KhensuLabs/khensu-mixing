// tests/ui/GuardrailTextTest.cpp
// The guardrail one-liners shared by the mastering-agent and section-intensity
// panels (ui/shared/GuardrailText.h), read through the generated
// api::fields::RenderGuardrail constants.
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/GuardrailText.h"

using namespace khensu;

namespace {

juce::var parse(const char* json) { return juce::JSON::parse(juce::String(json)); }

} // namespace

TEST_CASE("guardrail_text: a missing guardrail never passes", "[guardrail]")
{
    CHECK_FALSE(guardrail_text::passed(juce::var()));
    CHECK(guardrail_text::summary(juce::var()).startsWith("Safety result was not returned"));
    CHECK(guardrail_text::measurements(juce::var()).isEmpty());
}

TEST_CASE("guardrail_text: a passing post-render guardrail", "[guardrail]")
{
    const auto g = parse(R"({"ok": true, "matched_gain_db": -1.5,
        "before": {"true_peak_dbtp": -1.2, "clip_samples": 0, "mono_low_corr": 0.98, "crest_db": 12.0, "vocal_presence_proxy": 0.31},
        "after":  {"true_peak_dbtp": -1.0, "clip_samples": 0, "mono_low_corr": 0.97, "crest_db": 11.0, "vocal_presence_proxy": 0.30},
        "blocks": [], "warnings": ["Crest factor fell"], "artifact_score_delta": null})");
    CHECK(guardrail_text::passed(g));
    CHECK(guardrail_text::summary(g) == "Safety checks passed: Crest factor fell");
    const auto m = guardrail_text::measurements(g);
    CHECK(m.startsWith("Matched -1.50 dB | TP -1.20 -> -1.00 dBTP"));
    CHECK(m.contains("crest 12.00 -> 11.00 dB"));
}

TEST_CASE("guardrail_text: a pre-render refusal lists its blocks", "[guardrail]")
{
    const auto g = parse(R"({"pre_render": {"ok": false, "blocks": ["Unsupported channel layout"]}})");
    CHECK_FALSE(guardrail_text::passed(g));
    CHECK(guardrail_text::summary(g) == "Safety guardrail blocked promotion: Unsupported channel layout");
    CHECK(guardrail_text::measurements(g).isEmpty());
}
