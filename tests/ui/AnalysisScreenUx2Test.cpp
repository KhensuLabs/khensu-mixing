// tests/ui/AnalysisScreenUx2Test.cpp
// UX2-1 / UX2-3 / UX2-4: the sidebar's line icons and session block, the
// rollup strip that drops a figure rather than truncate one, the engine
// status worded short enough for a header, and the readiness breakdown's
// layout.
#include <catch2/catch_test_macros.hpp>
#include <set>
#include "ui/Dashboard/MixCraftPanel.h"
#include "ui/shared/SessionStateStrip.h"
#include "ui/shared/NavIcons.h"
#include "ui/shared/ServiceHealthWidget.h"
#include "ui/shared/ServiceStatusText.h"
#include "ui/shared/SidebarComponent.h"

using namespace khensu;

TEST_CASE("Every sidebar row has its own line icon on the 24 grid (UX2-4)", "[ux2][sidebar]")
{
    juce::ScopedJuceInitialiser_GUI init;
    std::set<int> seen;
    const juce::Rectangle<float> grid(0.0f, 0.0f, nav_icons::kGrid, nav_icons::kGrid);
    for (int i = 0; i < SidebarComponent::kNumScreens; ++i) {
        const auto icon = SidebarComponent::iconForVisualIndex(i);
        INFO("row " << i);
        CHECK(icon != nav_icons::Icon::None);
        CHECK(seen.insert(static_cast<int>(icon)).second); // one icon per screen
        const auto path = nav_icons::path(icon);
        CHECK_FALSE(path.isEmpty());
        CHECK(grid.contains(path.getBounds())); // inside the grid, so a stroke never clips
    }
    CHECK(nav_icons::path(nav_icons::Icon::None).isEmpty());

    SidebarComponent sidebar;
    for (int i = 0; i < SidebarComponent::kNumScreens; ++i) {
        CHECK(sidebar.navItemForTesting(i).icon() == SidebarComponent::iconForVisualIndex(i));
        CHECK(sidebar.navItemForTesting(i).getTitle().isNotEmpty()); // a11y title kept
    }
}

TEST_CASE("The sidebar's session block carries the readiness number (UX2-4)", "[ux2][sidebar]")
{
    juce::ScopedJuceInitialiser_GUI init;
    SidebarComponent sidebar;
    sidebar.setBounds(0, 0, 200, 840);
    CHECK_FALSE(sidebar.readinessKnown());
    sidebar.setSessionName("Track", "Pop");
    sidebar.setReadiness(73.4f, true);
    CHECK(sidebar.readinessKnown());
    CHECK(sidebar.readiness() == 73.4f);
    sidebar.setReadiness(0.0f, false);
    CHECK_FALSE(sidebar.readinessKnown());

    // The groups never overlap, and the rows keep a usable height.
    for (int i = 1; i < SidebarComponent::kNumScreens; ++i) {
        const auto& prev = sidebar.navItemForTesting(i - 1).getBounds();
        const auto& row = sidebar.navItemForTesting(i).getBounds();
        CHECK(row.getY() >= prev.getBottom());
        CHECK(row.getHeight() >= 28);
    }
    // Short window (the minimum): still no overlap.
    sidebar.setBounds(0, 0, 200, 584);
    for (int i = 1; i < SidebarComponent::kNumScreens; ++i)
        CHECK(sidebar.navItemForTesting(i).getY() >= sidebar.navItemForTesting(i - 1).getBottom());
}

TEST_CASE("The session strip drops the least useful figure instead of truncating (UX2-3, VIZ-P1)", "[ux2][rollup]")
{
    juce::ScopedJuceInitialiser_GUI init;
    SessionStateStrip::State st;
    st.hasSession = true;
    st.versionNode = 2;
    st.versionCount = 4;
    st.severe = 2;
    st.moderate = 5;
    st.checked = true;
    st.resolved = 2;
    st.worse = 1;
    st.target = "Glitch Hop, Streaming";
    st.analysis = SessionStateStrip::State::Analysis::Ready;
    SessionStateStrip strip;
    strip.setState(st);

    strip.setSize(1400, 40);
    auto cells = strip.layout();
    for (const auto& c : cells)
        CHECK_FALSE(c.isEmpty()); // room for all five

    // Narrow: analysis goes first, then target; open issues always stay, whole.
    int lastShown = SessionStateStrip::kNumFigures;
    for (int width = 1000; width >= 200; width -= 40) {
        strip.setSize(width, 40);
        cells = strip.layout();
        int shown = 0;
        int right = 0;
        for (const auto& c : cells)
            if (!c.isEmpty()) {
                ++shown;
                CHECK(c.getX() >= right); // no overlap
                right = c.getRight();
            }
        CHECK(right <= width);
        CHECK(shown <= lastShown); // narrowing never brings a figure back
        lastShown = shown;
        if (shown < SessionStateStrip::kNumFigures) CHECK(cells[SessionStateStrip::kAnalysis].isEmpty());
        if (shown > 0) CHECK_FALSE(cells[SessionStateStrip::kIssues].isEmpty());
    }
    CHECK(strip.figures()[SessionStateStrip::kIssues].value == "2 severe, 5 moderate");
}

TEST_CASE("Engine status is short enough for a header (UX2-3)", "[ux2][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    using service_status::shortStateLine;
    for (auto s : {PythonStatus::Running, PythonStatus::Starting, PythonStatus::GaveUp, PythonStatus::Stopped}) {
        CHECK(shortStateLine(s).startsWith("Engine "));
        CHECK(shortStateLine(s).length() < service_status::stateLine(s).length());
    }
    // The header reserves what the longest wording needs.
    CHECK(ServiceHealthWidget::preferredWidth() > 60);
    CHECK(ServiceHealthWidget::preferredWidth() < 200);
}

TEST_CASE("The readiness breakdown lays its rows and weighting inside the card (UX2-1)", "[ux2][breakdown]")
{
    juce::ScopedJuceInitialiser_GUI init;
    MixCraftPanel panel;
    panel.setBounds(0, 0, 700, panel.preferredHeight());
    const auto control = panel.weightControlArea();
    CHECK(panel.getLocalBounds().contains(control));
    CHECK(control.getWidth() >= 120);

    MixScorecard card;
    card.overall_score = 71.0f;
    for (int i = 0; i < 7; ++i) {
        ScorecardCategory c;
        c.name = "Category " + std::to_string(i);
        c.weight = 1.0f / 7.0f;
        c.score = 40.0f + 8.0f * static_cast<float>(i);
        card.categories.push_back(c);
    }
    panel.setScorecard(card);
    juce::Image img(juce::Image::ARGB, panel.getWidth(), panel.getHeight(), true);
    juce::Graphics g(img);
    panel.paint(g); // draws without asserting at its preferred height
    SUCCEED();
}
