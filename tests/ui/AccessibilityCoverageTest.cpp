// tests/ui/AccessibilityCoverageTest.cpp
// Every interactive control must end up with a spoken name and a tooltip
// once AccessibilityCoverage::apply() has run over its screen - checked on
// synthetic trees and on real panels constructible in tests.
#include <catch2/catch_test_macros.hpp>
#include "support/FakeAppController.h"
#include "ui/shared/AccessibilityCoverage.h"
#include "ui/Copilot/MasteringAgentPanel.h"
#include "ui/Copilot/SectionIntensityPanel.h"
#include "../../desktop-app/Source/ui/ExportDelivery/BatchRenderPanel.h"
#include "../../desktop-app/Source/ui/ExportDelivery/PlatformMastersPanel.h"
#include "../../desktop-app/Source/ui/ExportDelivery/DeliveryQaPanel.h"
#include "ui/shared/SidebarComponent.h"

namespace AC = khensu::AccessibilityCoverage;

TEST_CASE("Coverage derives names from text, labels, symbols and tooltips", "[a11y]")
{
    juce::ScopedJuceInitialiser_GUI init;
    juce::Component root;
    juce::TextButton text{"Render"};
    juce::TextButton plus{"+"};
    juce::TextButton help{"?"};
    help.setTooltip("Explain the loudness panel. Opens a card.");
    juce::Slider slider;
    juce::Label sliderLabel{{}, "Threshold:"};
    sliderLabel.attachToComponent(&slider, true);
    juce::ComboBox combo;
    combo.setTextWhenNothingSelected("Choose a genre");
    juce::TextButton explicitTitle{"Go"};
    explicitTitle.setTitle("Start analysis");
    explicitTitle.setTooltip("Runs every analyzer.");
    juce::TextButton blank;

    for (juce::Component* c : std::initializer_list<juce::Component*>{&text, &plus, &help, &slider, &combo,
                                                                      &explicitTitle, &blank})
        root.addAndMakeVisible(c);

    CHECK(AC::isInteractive(text));
    CHECK_FALSE(AC::isInteractive(sliderLabel));
    CHECK(AC::audit(root).size() == 5); // help's name derives from its tooltip

    CHECK(AC::apply(root) > 0);
    CHECK(text.getTitle() == "Render");
    CHECK(text.getTooltip() == "Render");
    CHECK(plus.getTitle() == "Increase");
    CHECK(help.getTitle() == "Explain the loudness panel");
    CHECK(slider.getTitle() == "Threshold");
    CHECK(combo.getTitle() == "Choose a genre");
    CHECK(explicitTitle.getTitle() == "Start analysis"); // explicit metadata wins
    CHECK(explicitTitle.getTooltip() == "Runs every analyzer.");

    // Only the textless, untitled button remains, and the audit says why.
    const auto gaps = AC::audit(root);
    REQUIRE(gaps.size() == 1);
    CHECK(gaps[0].component == &blank);
    CHECK(gaps[0].missingName);
    CHECK(gaps[0].describe().contains("[no name]"));
    CHECK(AC::wordsForSymbol("...") == "More options");
}

TEST_CASE("Real panels have full name and tooltip coverage", "[a11y]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::test::FakeAppController fake;
    khensu::MasteringAgentPanel mastering(fake);
    khensu::SectionIntensityPanel sections(fake);
    khensu::BatchRenderPanel batch(fake);
    khensu::PlatformMastersPanel platforms(fake);
    khensu::DeliveryQaPanel qa(fake);
    khensu::SidebarComponent sidebar;

    for (juce::Component* panel : std::initializer_list<juce::Component*>{&mastering, &sections, &batch, &platforms,
                                                                          &qa, &sidebar}) {
        AC::apply(*panel);
        for (const auto& gap : AC::audit(*panel)) {
            INFO("Gap in " << typeid(*panel).name() << ": " << gap.describe());
            CHECK(false);
        }
    }
}
