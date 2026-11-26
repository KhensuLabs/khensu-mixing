// tests/ui/DecisionNoticesTest.cpp
// The single-action decisions that moved from modal dialogs to the app-wide
// notice bar (ui/shared/DecisionNotices.h): each posts under a stable id with
// one action, a repeat replaces rather than stacks, the action does what the
// dialog's OK did and dismissing does what its Cancel / Skip did.
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/AppNoticeBar.h"
#include "ui/shared/DecisionNotices.h"

using namespace khensu;
namespace dn = khensu::decision_notices;

TEST_CASE("Decision notices: the gave-up notice offers Restart Service and runs it", "[decision-notice]")
{
    AppNoticeBar bar;
    int restarts = 0;
    dn::postReplacing(bar, dn::serviceGaveUp("Port 47239 is in use by another process.", [&] { ++restarts; }));
    REQUIRE(bar.current() != nullptr);
    CHECK(bar.current()->id == dn::kEngineStateId);
    CHECK(bar.current()->actionLabel == "Restart Service");
    CHECK(bar.current()->text.contains("failed to start"));
    CHECK(bar.current()->text.contains("Port 47239"));

    bar.triggerAction(); // the dialog's "Restart Service"
    CHECK(restarts == 1);
    CHECK_FALSE(bar.hasNotice());
    CHECK_FALSE(bar.isVisible());

    // Dismiss is the dialog's "Dismiss": nothing restarts.
    dn::postReplacing(bar, dn::serviceGaveUp("cause", [&] { ++restarts; }));
    bar.triggerDismiss();
    CHECK(restarts == 1);
}

TEST_CASE("Decision notices: a repeat replaces the queued notice instead of stacking", "[decision-notice]")
{
    AppNoticeBar bar;
    int first = 0, second = 0;
    // The plain engine-state notice the status banner posts...
    bar.post({dn::kEngineStateId, "The analysis engine stopped.", "Restart", [&] { ++first; }, {}});
    // ...is replaced by the gave-up notice with its cause, and a second
    // gave-up replaces that one too.
    dn::postReplacing(bar, dn::serviceGaveUp("old cause", [&] { ++first; }));
    dn::postReplacing(bar, dn::serviceGaveUp("new cause", [&] { ++second; }));
    REQUIRE(bar.current() != nullptr);
    CHECK(bar.current()->text.contains("new cause"));
    bar.triggerAction();
    CHECK(first == 0);
    CHECK(second == 1);
    CHECK_FALSE(bar.hasNotice()); // only one was queued

    // The service recovering withdraws it (updateServiceBanner, same id).
    dn::postReplacing(bar, dn::serviceGaveUp("cause", [&] { ++second; }));
    bar.withdraw(dn::kEngineStateId);
    CHECK_FALSE(bar.hasNotice());
    CHECK(second == 1);
}

TEST_CASE("Decision notices: a missing session file offers Locate File only for its own session",
          "[decision-notice]")
{
    AppNoticeBar bar;
    juce::String openSession = "session-a";
    int locates = 0;
    auto post = [&] {
        dn::postReplacing(bar, dn::missingSessionFile(
                                   "D:/Music/mix.wav", [&, id = openSession] { return openSession == id; },
                                   [&] { ++locates; }));
    };

    post();
    REQUIRE(bar.current() != nullptr);
    CHECK(bar.current()->id == dn::kMissingSessionFileId);
    CHECK(bar.current()->actionLabel == "Locate File...");
    CHECK(bar.current()->text.contains("D:/Music/mix.wav"));
    bar.triggerAction(); // the dialog's "Locate File..."
    CHECK(locates == 1);

    post();
    post(); // the same session reopened: still one notice
    bar.triggerAction();
    CHECK_FALSE(bar.hasNotice());
    CHECK(locates == 2);

    // The user switched sessions before acting: the old session's file is not relinked.
    post();
    openSession = "session-b";
    bar.triggerAction();
    CHECK(locates == 2);

    // Dismiss is the dialog's "Skip".
    openSession = "session-a";
    post();
    bar.triggerDismiss();
    CHECK(locates == 2);
}

TEST_CASE("Decision notices: a session bundle import is one line and Import runs it", "[decision-notice]")
{
    const auto withAudio = dn::bundleSummary("Song", 2, 5, 3);
    CHECK(withAudio.contains("\"Song\""));
    CHECK(withAudio.contains("2 audio file(s)"));
    CHECK(withAudio.contains("5 comment(s)"));
    CHECK(withAudio.contains("3 version(s)"));
    CHECK(withAudio.contains("new session"));
    CHECK_FALSE(withAudio.containsChar('\n')); // the notice bar shows one line
    CHECK(dn::bundleSummary("Notes", 0, 1, 0).contains("open session"));

    AppNoticeBar bar;
    int imports = 0;
    dn::postReplacing(bar, dn::importBundle(withAudio, [&] { ++imports; }));
    REQUIRE(bar.current() != nullptr);
    CHECK(bar.current()->id == dn::kBundleImportId);
    CHECK(bar.current()->actionLabel == "Import");
    CHECK(bar.current()->text == withAudio);

    bar.triggerDismiss(); // the dialog's Cancel
    CHECK(imports == 0);

    dn::postReplacing(bar, dn::importBundle(withAudio, [&] { ++imports; }));
    bar.triggerAction(); // the dialog's Import
    CHECK(imports == 1);
}
