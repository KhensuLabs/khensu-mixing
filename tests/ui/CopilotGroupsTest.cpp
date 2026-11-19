// UX-13 / CUT-8: the Copilot screen's groups and the mastering view's
// section-intensity option.
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

TEST_CASE("Section intensity is an option inside the mastering view (CUT-8)", "[copilot][cut8]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Component agent, sections;
    khensu::MasteringView view(agent, sections);
    view.setSize(800, 600);

    CHECK_FALSE(sections.isVisible()); // off until asked for
    const int agentAlone = agent.getHeight();
    view.sectionsToggle().setToggleState(true, juce::sendNotificationSync);
    CHECK(sections.isVisible());
    CHECK(sections.getHeight() > 0);
    CHECK(agent.getHeight() < agentAlone); // the agent makes room for it
}
