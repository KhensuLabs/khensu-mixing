// tests/ui/ScreensCoverageTest.cpp
// VER-6: the AI Detection, Reference Library and Settings screens had no
// test at all. Each is built against the fake controller, laid out at the
// app's minimum and a large size, shown and hidden, and torn down; every
// interactive control must end up with a spoken name once
// AccessibilityCoverage has run, as on the real screen change.
#include <catch2/catch_test_macros.hpp>
#include "support/FakeAppController.h"
#include "ui/shared/AccessibilityCoverage.h"
#include "ui/ProvenanceSource/AiDetectionScreen.h"
#include "ui/ReferenceLibrary/ReferenceLibraryScreen.h"
#include "ui/Settings/SettingsScreen.h"

namespace AC = khensu::AccessibilityCoverage;

namespace {

/// Lays the screen out at two sizes, toggles its visibility (the screens
/// fetch on first show) and checks every interactive control is named.
void exerciseScreen(juce::Component& screen)
{
    for (const auto& size : {juce::Point<int>{900, 600}, juce::Point<int>{1600, 1000}}) {
        screen.setSize(size.x, size.y);
        for (auto* child : screen.getChildren()) {
            CHECK(child->getWidth() >= 0);
            CHECK(child->getHeight() >= 0);
        }
    }
    screen.setVisible(true);
    screen.setVisible(false);
    AC::apply(screen);
    const auto missing = AC::audit(screen);
    for (const auto& gap : missing) UNSCOPED_INFO("unnamed control: " << gap.describe().toStdString());
    CHECK(missing.empty());
}

} // namespace

TEST_CASE("AI Detection screen builds, lays out and refuses to scan without a session", "[ver6][screens]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::test::FakeAppController controller;
    {
        khensu::AiDetectionScreen screen(controller);
        exerciseScreen(screen);

        const int before = controller.runInBackgroundCallCount;
        screen.runScan();
        CHECK(screen.statusTextForTesting() == "No active session.");
        CHECK(controller.runInBackgroundCallCount == before); // nothing sent to the service
    }
}

TEST_CASE("Reference Library screen builds, lays out and tears down", "[ver6][screens]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::test::FakeAppController controller;
    {
        khensu::ReferenceLibraryScreen screen(controller);
        exerciseScreen(screen);
    }
}

TEST_CASE("Settings screen builds, lays out and tears down", "[ver6][screens]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::test::FakeAppController controller;
    {
        khensu::SettingsScreen screen(controller);
        exerciseScreen(screen);
    }
}

#include "ui/ProvenanceSource/SourceSummaryView.h"

TEST_CASE("Provenance & Source: the Source view reads the session's source scans (CON-8)", "[con8][screens]")
{
    juce::DynamicObject::Ptr triage = new juce::DynamicObject();
    triage->setProperty("verdict", "fix in the box");
    triage->setProperty("dc_offset", 0.004);
    triage->setProperty("clipping", false);
    juce::DynamicObject::Ptr scans = new juce::DynamicObject();
    scans->setProperty("source_triage", juce::var(triage.get()));

    const auto sections = khensu::SourceSummaryView::sectionsFor(juce::var(scans.get()));
    REQUIRE(sections.size() == 4);
    CHECK(sections[0].title == "Source triage");
    CHECK(sections[0].lines.contains("Verdict: fix in the box"));
    CHECK(sections[0].lines.contains("Dc offset: 0.004"));
    CHECK(sections[0].lines.contains("Clipping: no"));
    CHECK(sections[1].lines.isEmpty()); // forensics not run yet
    CHECK(khensu::SourceSummaryView::sectionsFor({}).size() == 4);
}
