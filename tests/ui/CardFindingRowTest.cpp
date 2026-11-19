// tests/ui/CardFindingRowTest.cpp
// UX-1: a card's Explain / Audition / Fix row for its own top open finding.
#include <catch2/catch_test_macros.hpp>
#include "support/FakeAppController.h"
#include "ui/shared/CardFindingRow.h"
#include "ui/shared/PanelCard.h"

using namespace khensu;

namespace {
struct IssuesController final : test::FakeAppController {
    std::vector<IIssues::Issue> issues;
    std::string fixed;
    std::pair<double, double> looped{-1.0, -1.0};
    ScreenType shown = ScreenType::Dashboard;
    std::function<void()> notify;

    std::vector<IIssues::Issue> list() const override { return issues; }
    void fix(const std::string& recId, IIssues::Done onDone) override
    {
        fixed = recId;
        if (onDone) onDone(true, {});
    }
    void addIssuesListener(void*, std::function<void()> cb) override { notify = std::move(cb); }
    void removeIssuesListener(void*) override { notify = nullptr; }
    void loopRegion(double s, double e) override { looped = {s, e}; }
    void showScreen(ScreenType s) override { shown = s; }
};

IIssues::Issue issue(const char* code, float severity, bool renderable, bool timed = false)
{
    IIssues::Issue i;
    i.code = code;
    i.recId = std::string(code) + "_f1";
    i.rec.what = std::string("what ") + code;
    i.rec.severity = severity;
    i.renderable = renderable;
    if (timed) {
        TimeRange t;
        t.start_sec = 30.0f;
        t.end_sec = 34.0f;
        i.rec.time_range = t;
    }
    return i;
}

struct Card final : PanelCard {
    explicit Card(IAppController& c) : PanelCard(c, "Loudness") {}
    juce::Rectangle<int> body() const { return bodyArea(); }
};
} // namespace

TEST_CASE("a card's finding row shows its own top open finding (UX-1)", "[ux1][card]")
{
    IssuesController c;
    c.issues = {issue("stereo_too_wide", 90.0f, true), issue("lufs_too_quiet", 40.0f, true, true),
                issue("true_peak_over", 60.0f, false)};
    c.issues[2].state = IIssues::State::Done; // settled: not shown

    Card card(c);
    card.setBounds(0, 0, 400, 300);
    const auto fullBody = card.body();
    card.setFindingCodes(card_findings::codesFor("loudness"));
    auto* row = card.findingRowForTesting();
    REQUIRE(row != nullptr);
    CHECK(row->isVisible());
    REQUIRE(row->topIssue().has_value());
    CHECK(row->topIssue()->code == "lufs_too_quiet");      // stereo is not this card's; the done one is settled
    CHECK(card.body().getHeight() < fullBody.getHeight()); // the body leaves room for the row

    // The queue changes: nothing open for this card -> the row goes, the body grows back.
    c.issues[1].state = IIssues::State::Done;
    c.notify();
    CHECK_FALSE(row->isVisible());
    CHECK(card.body().getHeight() == fullBody.getHeight());
}

TEST_CASE("the row's Fix goes through the issue queue, advice opens Issues (UX-1)", "[ux1][card]")
{
    IssuesController c;
    c.issues = {issue("dc_offset", 70.0f, false)};
    CardFindingRow row(c, card_findings::codesFor("artifact"));
    row.refresh();
    REQUIRE(row.isVisible());
    auto* fix = dynamic_cast<juce::TextButton*>(row.getChildComponent(2));
    REQUIRE(fix != nullptr);
    fix->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    CHECK(c.shown == ScreenType::Issues); // advice: no render

    c.issues = {issue("clipping_detected", 70.0f, true, true)};
    row.refresh();
    fix->triggerClick();
    auto* hear = dynamic_cast<juce::TextButton*>(row.getChildComponent(1));
    REQUIRE(hear != nullptr);
    hear->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    CHECK(c.fixed == "clipping_detected_f1");
    CHECK(c.looped.first >= 0.0);
}

TEST_CASE("cards without findings of their own get no row (UX-1)", "[ux1][card]")
{
    CHECK(card_findings::codesFor("song_info").empty());
    CHECK_FALSE(card_findings::codesFor("artifact").empty());
}
