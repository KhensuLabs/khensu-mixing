// UX-15: the first session's three steps tick and finish once.
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/CoachMarkCard.h"

using khensu::CoachMarkCard;

TEST_CASE("The first-session steps tick off and finish once (UX-15)", "[coach][ux15]")
{
    juce::ScopedJuceInitialiser_GUI init;
    CoachMarkCard card;
    std::vector<CoachMarkCard::Step> ran;
    int finished = 0;
    card.onRunStep = [&ran](CoachMarkCard::Step s) { ran.push_back(s); };
    card.onFinished = [&finished] { ++finished; };

    // Pressing a step's button runs it and ticks it.
    for (auto* c : card.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(c); b != nullptr && b->getButtonText() == "Fix (F)")
            b->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    REQUIRE(ran.size() == 1);
    CHECK(ran[0] == CoachMarkCard::FixTopIssue);
    CHECK(card.isDone(CoachMarkCard::FixTopIssue));

    // A step done another way (the B key) ticks too, without running it again.
    card.markDone(CoachMarkCard::HearAb);
    CHECK(card.isDone(CoachMarkCard::HearAb));
    CHECK(ran.size() == 1);
    CHECK(finished == 0);

    card.markDone(CoachMarkCard::Export);
    CHECK(card.allDone());
    CHECK(finished == 1);
    card.markDone(CoachMarkCard::Export); // already done: nothing more
    CHECK(finished == 1);
}
