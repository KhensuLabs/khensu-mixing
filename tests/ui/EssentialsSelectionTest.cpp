// tests/ui/EssentialsSelectionTest.cpp
// UX-1 / VER-8: the Dashboard's Essentials view and the experimental flag.
#include <catch2/catch_test_macros.hpp>
#include "ui/Dashboard/DashboardCategory.h"

#include <algorithm>

#include "support/FakeAppController.h"
#include "ui/Dashboard/EssentialsSelection.h"
#include "ui/shared/FeatureMaturity.h"
#include "ui/shared/PanelCard.h"

namespace ess = khensu::essentials;
namespace mat = khensu::maturity;

namespace {

khensu::Recommendation finding(const std::string& code, float severity)
{
    khensu::Recommendation r;
    r.id = code + "_file1";
    r.severity = severity;
    return r;
}

bool contains(const std::vector<std::string>& v, const std::string& id)
{
    return std::find(v.begin(), v.end(), id) != v.end();
}

} // namespace

TEST_CASE("Essentials: Mix Notes leads, then the worst finding's card", "[ux1][essentials]")
{
    const auto picked = ess::select({finding("phase_issue", 20.0f), finding("dc_offset", 80.0f),
                                     finding("resonance_hotspot", 50.0f)});
    REQUIRE(picked.size() == ess::kEssentialsCount);
    CHECK(picked[0] == "mix_notes");
    CHECK(picked[1] == "artifact"); // dc_offset, severity 80
    CHECK(picked[2] == "eq_curve"); // resonance_hotspot, 50
    CHECK(picked[3] == "phase");    // phase_issue, 20
}

TEST_CASE("Essentials: no findings gives Mix Notes and the core set", "[ux1][essentials]")
{
    const auto picked = ess::select({});
    REQUIRE(picked.size() == ess::kEssentialsCount);
    CHECK(picked[0] == "mix_notes");
    CHECK(picked[1] == "reference");
}

TEST_CASE("Essentials: one card per panel, capped, never an experimental card", "[ux1][essentials][ver8]")
{
    std::vector<khensu::Recommendation> many;
    for (const char* code : {"ai_artifacts", "hook_strength_low", "memorability_low", "heaviness_weight_low",
                             "structural_flow_poor", "centroid_too_bright", "tilt_too_bright", "low_end_heavy",
                             "clipping_detected", "noise_floor_high", "mono_compat_warning", "sub_bass_too_wide",
                             "kick_bass_collision", "flat_loudness_arc", "true_peak_over", "lra_too_compressed"})
        many.push_back(finding(code, 90.0f));
    const auto picked = ess::select(many);
    CHECK(picked.size() == ess::kEssentialsCount);
    for (const auto& id : picked) {
        INFO(id);
        CHECK_FALSE(mat::isExperimental(id));
        CHECK(std::count(picked.begin(), picked.end(), id) == 1);
    }
    CHECK(contains(picked, "tonal_balance")); // three tonal findings, one card
}

TEST_CASE("Essentials: findings shown above Zone 3 add no card", "[ux1][essentials]")
{
    CHECK(ess::panelForFinding("stereo_too_wide_file1") == "phase");
    CHECK(ess::panelForFinding("some_future_code_file1").empty());
}

TEST_CASE("Experimental flag: the table and the card badge", "[ver8]")
{
    CHECK(mat::isExperimental("ai_artifacts"));
    CHECK(mat::isExperimental("structural"));
    CHECK_FALSE(mat::isExperimental("mix_notes"));
    for (const auto& f : mat::kExperimentalPanels)
        CHECK(juce::String(f.outstandingCheck).isNotEmpty());

    khensu::test::FakeAppController controller;
    khensu::PanelCard card(controller, "Hooks");
    CHECK_FALSE(card.isExperimental());
    card.setExperimental(true, "Not yet checked on real audio.");
    card.setBounds(0, 0, 400, 200);
    CHECK(card.isExperimental());
    auto* badge = card.experimentalBadgeForTesting();
    REQUIRE(badge != nullptr);
    CHECK(badge->isVisible());
    CHECK(badge->getTitle().isNotEmpty());
    CHECK(card.getHeaderBoundsForTesting().contains(badge->getBounds()));
    card.setExperimental(false, {});
    CHECK_FALSE(badge->isVisible());
}

TEST_CASE("Song insights show only on their own Dashboard tab (CUT-3)", "[dashboard][cut3]")
{
    using namespace khensu::dashboard_category;
    for (const char* id : {"hooks", "memorability", "emotional_arc", "heaviness", "recreate_sound"}) {
        INFO(id);
        CHECK(forPanel(id) == kSongInsights);
        CHECK(shownInTab("Song insights", id));
        CHECK_FALSE(shownInTab("All", id));
    }
    CHECK(shownInTab("All", "tonal_balance"));
    CHECK(shownInTab("Spectrum", "tonal_balance"));
    CHECK_FALSE(shownInTab("Loudness", "tonal_balance"));
    bool tabListed = false;
    for (const char* tab : kTabs)
        tabListed = tabListed || juce::String(tab) == kSongInsights;
    CHECK(tabListed);
}

TEST_CASE("Advanced cards leave All and Essentials unless shown (OVH-7)", "[dashboard][ovh7]")
{
    using namespace khensu::dashboard_category;
    // Every experimental card outside the song insights is advanced - except
    // a member of a Dashboard group, which moves with its group (AI evidence
    // sits in Provenance & source beside the evidence-backed Artifacts).
    for (const auto& f : khensu::maturity::kExperimentalPanels) {
        const std::string id(f.id);
        INFO(id);
        if (id == "ai_artifacts") continue;
        CHECK((forPanel(id) == kSongInsights || isAdvanced(id)));
    }
    for (const char* id : {"depth_ladder", "reverb_tail", "groove_timing", "drum_pattern", "harmony_timeline",
                           "tuning_ref", "benchmark", "genre_dist", "spatial_audio", "structural"}) {
        INFO(id);
        CHECK(isAdvanced(id));
        CHECK(shownInTab(kAdvanced, id));
        CHECK_FALSE(shownInTab("All", id));
        CHECK(shownInTab("All", id, /*showAdvanced*/ true));
    }
    // Cards that back an evidence-measured finding stay.
    for (const char* code : {"lufs_too_loud", "true_peak_over", "low_end_light", "tilt_too_dark", "dc_offset",
                             "lra_too_compressed", "stereo_too_wide", "ms_brightness_imbalance"}) {
        const std::string panel(khensu::essentials::panelForFinding(code));
        INFO(code << " -> " << panel);
        CHECK_FALSE(isAdvanced(panel));
    }
    CHECK_FALSE(shownInTab("All", "hooks", true)); // the song insights stay on their own tab
    // Essentials never picks an advanced card, even for its finding.
    khensu::Recommendation flow;
    flow.id = "structural_flow_poor_f1";
    flow.severity = 90.0f;
    for (const auto& id : khensu::essentials::select({flow}))
        CHECK_FALSE(isAdvanced(id));
    bool tabListed = false;
    for (const char* tab : kTabs)
        tabListed = tabListed || juce::String(tab) == kAdvanced;
    CHECK(tabListed);
}
