// tests/ui/RecommendationCardTest.cpp
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/shared/KhensuTheme.h"
#include "../../desktop-app/Source/ui/shared/RecommendationCard.h"
#include "../../desktop-app/Source/session/RecommendationChain.h"
#include "support/FakeAppController.h"

TEST_CASE("RecommendationCard collapsed height", "[reccard]")
{
    REQUIRE(khensu::RecommendationCard::collapsedHeight() == 44);
}

TEST_CASE("RecommendationCard priority badge colour", "[reccard]")
{
    using namespace khensu;
    REQUIRE(RecommendationCard::badgeColourForPriority("priority") == KhensuTheme::accentRed);
    REQUIRE(RecommendationCard::badgeColourForPriority("recommended") == KhensuTheme::accentAmber);
    REQUIRE(RecommendationCard::badgeColourForPriority("polish") == KhensuTheme::accentBlue);
    REQUIRE(RecommendationCard::badgeColourForPriority("strength") == KhensuTheme::accentGreen);
}

TEST_CASE("RecommendationCard explains measurement precision without overstating intervention certainty", "[reccard]")
{
    using namespace khensu;
    Recommendation rec;
    rec.diagnostic_evidence = "True peak exceeded the configured ceiling.";
    rec.measurement_confidence = 0.73f;
    rec.confidence_reason = "Measured from the decoded source file.";

    const auto summary = RecommendationCard::evidenceSummaryFor(rec);
    REQUIRE(summary.contains("Evidence:"));
    REQUIRE(summary.contains("73%"));
    REQUIRE(summary.contains("not proof that this fix will help"));
}

TEST_CASE("RecommendationCard surfaces abstention instead of a numeric precision", "[reccard]")
{
    using namespace khensu;
    Recommendation rec;
    rec.measurement_confidence = 0.73f;
    rec.abstained_reason = "The underlying loudness measurement is unreliable.";

    const auto summary = RecommendationCard::evidenceSummaryFor(rec);
    REQUIRE(summary.contains("Caution"));
    REQUIRE(summary.contains("unreliable"));
    REQUIRE_FALSE(summary.contains("73%"));
}

// P0-2: "Apply" on a Dashboard finding renders; an advice-only finding says
// "Mark done" instead of pretending to apply something.
TEST_CASE("RecommendationCard Apply renders a renderable finding, else says Mark done", "[reccard][p0-2]")
{
    using namespace khensu;
    test::FakeAppController fake;

    Recommendation eq;
    eq.id = "eq-1";
    eq.suggested_parameters = {{"eq_freq_hz", 3000.0f}, {"eq_gain_db", -2.5f}};
    Recommendation advice;
    advice.id = "advice-1";
    advice.suggested_parameters = {{"reverb_wet", 0.2f}}; // a send setting: advice only

    CHECK(hasRenderableChain(eq));
    CHECK_FALSE(hasRenderableChain(advice));
    REQUIRE(buildRecommendationChain(eq).size() == 1);
    CHECK(buildRecommendationChain(eq)[0].effect == "PeakFilter");

    RecommendationCard card(fake);
    card.setRecommendation(eq);
    CHECK(card.applyButtonText() == "Mark done"); // no owner hook: nothing can render
    card.onApplyClicked = [](const Recommendation&) {};
    card.setApplyInFlight(false); // re-reads the hook
    CHECK(card.appliesByRendering());
    CHECK(card.applyButtonText() == "Apply");
    card.setApplyInFlight(true);
    CHECK(card.applyButtonText() == "Rendering...");
    card.setApplyResult(false);
    CHECK(card.applyButtonText() == "Apply");
    card.setApplyResult(true);
    CHECK(card.applyButtonText() == "Applied");

    card.setRecommendation(advice);
    CHECK_FALSE(card.appliesByRendering());
    CHECK(card.applyButtonText() == "Mark done");
}
