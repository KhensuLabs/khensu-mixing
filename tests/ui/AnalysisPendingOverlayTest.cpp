// tests/ui/AnalysisPendingOverlayTest.cpp
// The results gate's overlay: stage labels and an overall progress figure
// that only moves forward through the pipeline's stages.
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/AnalysisPendingOverlay.h"

using khensu::AnalysisPendingOverlay;

TEST_CASE("Results gate overlay: stage labels name each pipeline stage", "[resultsgate]")
{
    CHECK(AnalysisPendingOverlay::stageLabel("fast").contains("loudness"));
    CHECK(AnalysisPendingOverlay::stageLabel("deep_analysis:3:8") == "Deep analysis (3/8)");
    CHECK(AnalysisPendingOverlay::stageLabel("deep_analysis") == "Deep analysis");
    CHECK(AnalysisPendingOverlay::stageLabel("deep_analysis:failed").contains("unavailable"));
    CHECK(AnalysisPendingOverlay::stageLabel("sections") == "Finding sections");
    CHECK(AnalysisPendingOverlay::stageLabel("chords").contains("chords"));
    CHECK(AnalysisPendingOverlay::stageLabel("reference").contains("reference"));
}

TEST_CASE("Results gate overlay: progress rises through the stages and stops short of done", "[resultsgate]")
{
    const float fast = AnalysisPendingOverlay::overallFraction("fast", 1, 6);
    const float deepEarly = AnalysisPendingOverlay::overallFraction("deep_analysis:1:8", 2, 6);
    const float deepLate = AnalysisPendingOverlay::overallFraction("deep_analysis:7:8", 2, 6);
    const float sections = AnalysisPendingOverlay::overallFraction("sections", 3, 6);
    const float chords = AnalysisPendingOverlay::overallFraction("chords", 5, 6);
    CHECK(fast > 0.0f);
    CHECK(deepEarly > fast);
    CHECK(deepLate > deepEarly);
    CHECK(sections >= deepLate);
    CHECK(chords > sections);
    // The bar never reads 100% while results are still withheld.
    CHECK(chords < 1.0f);
    CHECK(AnalysisPendingOverlay::overallFraction("fast", 0, 0) == 0.0f);
}

TEST_CASE("Results gate overlay: running modes offer cancel, ended modes offer a re-run", "[resultsgate]")
{
    AnalysisPendingOverlay overlay;
    overlay.setSize(800, 600);
    int cancels = 0, reruns = 0;
    overlay.onCancel = [&] { ++cancels; };
    overlay.onRerun = [&] { ++reruns; };

    auto visibleButton = [&](const juce::String& text) -> juce::Button* {
        for (auto* c : overlay.getChildren())
            if (auto* b = dynamic_cast<juce::Button*>(c))
                if (b->isVisible() && b->getButtonText() == text) return b;
        return nullptr;
    };

    overlay.setMode(AnalysisPendingOverlay::Mode::Analyzing);
    REQUIRE(visibleButton("Cancel analysis") != nullptr);
    CHECK(visibleButton("Analyze again") == nullptr);
    visibleButton("Cancel analysis")->triggerClick();

    overlay.setMode(AnalysisPendingOverlay::Mode::Cancelled);
    CHECK(visibleButton("Cancel analysis") == nullptr);
    REQUIRE(visibleButton("Analyze again") != nullptr);
    visibleButton("Analyze again")->triggerClick();

    overlay.setMode(AnalysisPendingOverlay::Mode::Failed, "Could not decode the file");
    CHECK(visibleButton("Analyze again") != nullptr);

    // triggerClick posts asynchronously; pump the message loop briefly.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    CHECK(cancels == 1);
    CHECK(reruns == 1);
}

TEST_CASE("Results gate overlay: the fast pass shows at once, marked provisional (PERF-1)", "[resultsgate]")
{
    const auto text = AnalysisPendingOverlay::provisionalSummary(-9.84f, -0.42f, true);
    CHECK(text.contains("-9.8 LUFS"));
    CHECK(text.contains("-0.4 dBTP"));
    CHECK(text.contains("Clipping found"));
    CHECK(AnalysisPendingOverlay::provisionalSummary(-14.0f, -1.0f, false).contains("No clipping"));

    AnalysisPendingOverlay overlay;
    overlay.setSize(800, 600);
    overlay.setMode(AnalysisPendingOverlay::Mode::Analyzing);
    CHECK_FALSE(overlay.hasProvisional());
    overlay.setProvisional(-9.84f, -0.42f, false);
    CHECK(overlay.hasProvisional());
    // A new run forgets the previous run's numbers.
    overlay.setMode(AnalysisPendingOverlay::Mode::Cancelled);
    overlay.setMode(AnalysisPendingOverlay::Mode::Analyzing);
    CHECK_FALSE(overlay.hasProvisional());
}
