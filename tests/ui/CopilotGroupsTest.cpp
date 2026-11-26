// UX-13 / UXR-3: the Copilot screen's one switch; ENH-8 / CUT-8: the
// mastering view's link to Export > Finish and its section intensity.
#include <catch2/catch_test_macros.hpp>
#include "ui/Copilot/CopilotScreen.h"

TEST_CASE("A Copilot group shows one view at a time (UX-13)", "[copilot][ux13]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Component chat, feedback, taste;
    khensu::CopilotGroup group;
    group.addView("Chat", chat);
    group.addView("Client feedback", feedback);
    group.addView("Taste profile", taste);
    group.setSize(800, 600);

    CHECK(group.viewCount() == 3);
    CHECK(chat.isVisible());
    CHECK_FALSE(feedback.isVisible());
    group.show(1);
    CHECK_FALSE(chat.isVisible());
    CHECK(feedback.isVisible());
    CHECK(feedback.getHeight() > 500); // the view fills the group under the switch
    group.show(7);                     // out of range: nothing changes
    CHECK(group.activeView() == 1);
}

TEST_CASE("The Copilot's views share one switch (UXR-3)", "[copilot][uxr3]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Component a, b;
    khensu::CopilotGroup group;
    group.addView("Chat", a);
    CHECK_FALSE(group.switcher().isVisible()); // one view: no switch
    group.addView("Mastering", b);
    group.setSize(1200, 600);
    CHECK(group.switcher().isVisible());
    CHECK(group.switcher().getWidth() <= 220); // sized to its views, not the whole row
    CHECK(a.getY() == b.getY());               // every view under the one switch
}

TEST_CASE("The mastering view links to Export > Finish and keeps section intensity (ENH-8)", "[copilot][enh8]")
{
    juce::ScopedJuceInitialiser_GUI init;
    int went = 0;
    khensu::FeatureHomeLink link("Mastering lives in Export > Finish", "body", "Go to Finish", [&went] { ++went; });
    juce::Component sections;
    khensu::MasteringView view(link, sections);
    view.setSize(800, 600);

    CHECK(link.isVisible());
    CHECK(sections.isVisible());
    CHECK(link.getHeight() < 120);       // a compact card
    CHECK(sections.getY() > link.getBottom());
    CHECK(sections.getHeight() > 400);   // the rest is the section panel
    link.button().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    CHECK(went == 1);
}
