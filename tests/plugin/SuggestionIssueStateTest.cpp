// tests/plugin/SuggestionIssueStateTest.cpp
// The plugin's Suggestions tab shares the desktop issue queue's state
// (CON-1): the analysis controller keeps open / done / dismissed per finding,
// a row shows it, and a row's marker and Auto-Apply report the next state.
#include <catch2/catch_test_macros.hpp>

#include "../../plugin-copilot/Source/CopilotAnalysisController.h"
#include "../../plugin-copilot/Source/ui/SuggestionFeedPanel.h"

#include <atomic>
#include <chrono>
#include <thread>

using khensu::CopilotAnalysisController;
using khensu::Recommendation;
using khensu::SuggestionFeedPanel;
using khensu::SuggestionRow;
using State = khensu::issue_state::State;

namespace {
bool waitUntil(const std::function<bool()>& done, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

Recommendation rec(const std::string& id)
{
    Recommendation r;
    r.id = id;
    r.what = "Finding " + id;
    return r;
}

void clickMarker(SuggestionRow& row)
{
    const juce::Point<float> pos(static_cast<float>(row.getWidth() - 10), 20.0f);
    const auto now = juce::Time::getCurrentTime();
    juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(), 0.0f, 0.0f, 0.0f,
                       0.0f, 0.0f, &row, &row, now, pos, now, 1, false);
    row.mouseUp(e);
}
} // namespace

TEST_CASE("The plugin keeps each finding's issue-queue state from the load", "[plugin][con1]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    CopilotAnalysisController controller(
        [](const CopilotAnalysisController::CancelCheck&) -> std::optional<CopilotAnalysisController::LoadedSnapshot> {
            CopilotAnalysisController::LoadedSnapshot snapshot;
            snapshot.recs = {rec("a_f1"), rec("b_f1"), rec("c_f1")};
            snapshot.states = {{"a_f1", State::Done}, {"b_f1", State::Dismissed}};
            snapshot.sessionId = "s1";
            return snapshot;
        });
    controller.loadLastSessionAsync({});
    REQUIRE(waitUntil([&] { return controller.getNumRunningWorkers() == 0; }, 3000));
    REQUIRE(controller.getSessionId() == "s1");

    CHECK(controller.issueState("a_f1") == State::Done);
    CHECK(controller.issueState("b_f1") == State::Dismissed);
    CHECK(controller.issueState("c_f1") == State::Open);

    controller.setIssueState("b_f1", State::Open);
    controller.setIssueState("c_f1", State::Done);
    CHECK(controller.issueState("b_f1") == State::Open);
    CHECK(controller.issueState("c_f1") == State::Done);
}

TEST_CASE("A Suggestions row shows the queue state and its marker toggles it", "[plugin][con1]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    SuggestionRow row;
    row.setRecommendation(rec("a_f1"));
    row.setBounds(0, 0, 500, SuggestionRow::kRowHeight);
    std::vector<State> reported;
    row.setOnIssueStateChanged([&reported](State s) { reported.push_back(s); });

    row.setIssueState(State::Dismissed); // dismissed on the desktop
    clickMarker(row);                    // reopens it
    CHECK(row.getIssueState() == State::Open);
    clickMarker(row); // marks it done
    CHECK(row.getIssueState() == State::Done);
    clickMarker(row); // reopens it
    CHECK(row.getIssueState() == State::Open);
    CHECK(reported == std::vector<State>{State::Open, State::Done, State::Open});
}

TEST_CASE("The Suggestions panel seeds rows from the state lookup and reports by id", "[plugin][con1]")
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    SuggestionFeedPanel panel;
    panel.setBounds(0, 0, 500, 300);
    std::vector<std::pair<std::string, State>> reported;
    panel.setOnIssueStateChanged([&reported](const std::string& id, State s) { reported.emplace_back(id, s); });
    panel.setData({rec("a_f1"), rec("b_f1")},
                  [](const std::string& id) { return id == "b_f1" ? State::Dismissed : State::Open; });

    std::vector<SuggestionRow*> rows;
    std::function<void(juce::Component&)> collect = [&](juce::Component& c) {
        for (auto* child : c.getChildren()) {
            if (auto* row = dynamic_cast<SuggestionRow*>(child))
                rows.push_back(row);
            else
                collect(*child);
        }
    };
    collect(panel);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0]->getIssueState() == State::Open);
    CHECK(rows[1]->getIssueState() == State::Dismissed);

    clickMarker(*rows[1]);
    REQUIRE(reported.size() == 1);
    CHECK(reported[0] == std::make_pair(std::string("b_f1"), State::Open));
}
