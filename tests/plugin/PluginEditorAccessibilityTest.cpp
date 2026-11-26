// tests/plugin/PluginEditorAccessibilityTest.cpp
// Keyboard focus order, spoken names, keyboard shortcuts, contrast, and
// display-scale snapshot coverage for the VST3 copilot editor (audit
// AUD-UX-01). Hosts differ in how they hand focus to an embedded editor, but
// once it has focus the traversal below is what Tab/Shift+Tab walks.
//
// Snapshots for 100/125/150/200% scales are written to
// <temp>/khensu-plugin-snapshots/ so a reviewer can eyeball them; the test
// asserts their geometry and that each render actually drew content.
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/PluginEditor.h"
#include "../../plugin-copilot/Source/PluginProcessor.h"
#include "../../plugin-copilot/Source/ui/PluginLookAndFeel.h"

#include <cmath>

using khensu::KhensuCopilotEditor;
using khensu::KhensuCopilotProcessor;
using khensu::PluginLookAndFeel;

namespace {
double relativeLuminance(juce::Colour c)
{
    auto channel = [](float v) {
        return v <= 0.03928f ? v / 12.92 : std::pow((static_cast<double>(v) + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.getFloatRed()) + 0.7152 * channel(c.getFloatGreen()) +
           0.0722 * channel(c.getFloatBlue());
}

double contrastRatio(juce::Colour a, juce::Colour b)
{
    const double la = relativeLuminance(a), lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

int indexOf(const std::vector<juce::Component*>& order, const juce::Component* c)
{
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == c) return static_cast<int>(i);
    return -1;
}

struct EditorFixture {
    juce::ScopedJuceInitialiser_GUI juceInit;
    KhensuCopilotProcessor proc;
    std::unique_ptr<KhensuCopilotEditor> editor;

    EditorFixture()
    {
        proc.setPlayConfigDetails(2, 2, 48000.0, 512);
        editor = std::make_unique<KhensuCopilotEditor>(proc);
    }
};
} // namespace

TEST_CASE("Copilot editor exposes a deterministic header-to-footer focus order",
          "[plugin][accessibility][auduxo1]")
{
    EditorFixture f;
    auto& ed = *f.editor;
    auto* load = ed.findChildWithID("loadButton");
    auto* bypass = ed.findChildWithID("bypassButton");
    auto* tabs = ed.findChildWithID("tabs");
    auto* strip = ed.findChildWithID("comparisonStrip");
    REQUIRE(load != nullptr);
    REQUIRE(bypass != nullptr);
    REQUIRE(tabs != nullptr);
    REQUIRE(strip != nullptr);

    auto traverser = ed.createKeyboardFocusTraverser();
    REQUIRE(traverser != nullptr);
    const auto order = traverser->getAllComponents(&ed);
    REQUIRE_FALSE(order.empty());

    const int iLoad = indexOf(order, load), iBypass = indexOf(order, bypass);
    REQUIRE(iLoad >= 0);
    REQUIRE(iBypass >= 0);
    CHECK(iLoad < iBypass);

    // Section buttons (VIS-9: segments) live inside the tabs container and
    // follow the header.
    auto* sections = dynamic_cast<khensu::PluginSegmentedTabs*>(tabs);
    REQUIRE(sections != nullptr);
    REQUIRE(sections->getNumTabs() >= 5);
    const int iFirstTab = indexOf(order, sections->getSegmentButton(0));
    const int iLastTab = indexOf(order, sections->getSegmentButton(sections->getNumTabs() - 1));
    REQUIRE(iFirstTab >= 0);
    CHECK(iBypass < iFirstTab);
    CHECK(iFirstTab < iLastTab);

    // The A/B strip's buttons are last, after every tab.
    bool sawStripChild = false;
    for (size_t i = 0; i < order.size(); ++i) {
        if (strip->isParentOf(order[i])) {
            sawStripChild = true;
            CHECK(static_cast<int>(i) > iLastTab);
        }
    }
    CHECK(sawStripChild);
}

TEST_CASE("Every keyboard-focusable editor control has a spoken name", "[plugin][accessibility][auduxo1]")
{
    EditorFixture f;
    auto traverser = f.editor->createKeyboardFocusTraverser();
    for (auto* c : traverser->getAllComponents(f.editor.get())) {
        juce::String name = c->getTitle();
        if (name.isEmpty())
            if (auto* b = dynamic_cast<juce::Button*>(c)) name = b->getButtonText();
        INFO("Unnamed focusable component of type " << typeid(*c).name());
        CHECK(name.trim().isNotEmpty());
    }
    CHECK(f.editor->getTitle() == "Khensu Copilot");
}

TEST_CASE("Ctrl+number and Ctrl+Tab switch copilot tabs from the keyboard", "[plugin][accessibility][auduxo1]")
{
    EditorFixture f;
    auto& ed = *f.editor;
    const auto cmd = juce::ModifierKeys::commandModifier;

    CHECK(ed.keyPressed(juce::KeyPress('3', cmd, 0)));
    CHECK(ed.getCurrentTabIndex() == 2);
    CHECK(ed.keyPressed(juce::KeyPress(juce::KeyPress::tabKey, cmd, 0)));
    CHECK(ed.getCurrentTabIndex() == 3);
    CHECK(ed.keyPressed(juce::KeyPress(juce::KeyPress::tabKey, cmd | juce::ModifierKeys::shiftModifier, 0)));
    CHECK(ed.getCurrentTabIndex() == 2);
    CHECK_FALSE(ed.keyPressed(juce::KeyPress('9', cmd, 0))); // no 9th tab
    CHECK_FALSE(ed.keyPressed(juce::KeyPress('3')));          // plain keys go to the host

    auto* bypass = dynamic_cast<juce::Button*>(ed.findChildWithID("bypassButton"));
    REQUIRE(bypass != nullptr);
    const bool before = bypass->getToggleState();
    CHECK(ed.keyPressed(juce::KeyPress('B', cmd, 0)));
    CHECK(bypass->getToggleState() != before);
}

TEST_CASE("Copilot palette text meets WCAG contrast on its backgrounds", "[plugin][accessibility][auduxo1]")
{
    // The plugin has no runtime theme switching, so its single palette must
    // already satisfy high-contrast reading: AAA (7:1) for primary text and
    // AA (4.5:1) for muted/status text.
    CHECK(contrastRatio(PluginLookAndFeel::textPrimary, PluginLookAndFeel::bgDark) >= 7.0);
    CHECK(contrastRatio(PluginLookAndFeel::textPrimary, PluginLookAndFeel::bgPanel) >= 7.0);
    CHECK(contrastRatio(PluginLookAndFeel::textMuted, PluginLookAndFeel::bgDark) >= 4.5);
    CHECK(contrastRatio(PluginLookAndFeel::accentTeal, PluginLookAndFeel::bgDark) >= 4.5);
    CHECK(contrastRatio(PluginLookAndFeel::statusWarn, PluginLookAndFeel::bgDark) >= 4.5);
}

TEST_CASE("Copilot editor renders at 100/125/150/200% display scales", "[plugin][accessibility][auduxo1]")
{
    EditorFixture f;
    auto& ed = *f.editor;
    const auto outDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("khensu-plugin-snapshots");
    outDir.createDirectory();

    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        INFO("scale " << scale);
        const auto image = ed.createComponentSnapshot(ed.getLocalBounds(), true, scale);
        CHECK(image.getWidth() == juce::roundToInt(static_cast<float>(ed.getWidth()) * scale));
        CHECK(image.getHeight() == juce::roundToInt(static_cast<float>(ed.getHeight()) * scale));

        // A blank or single-colour render means layout collapsed at this scale.
        const auto corner = image.getPixelAt(0, 0);
        int differing = 0;
        for (int y = 0; y < image.getHeight(); y += 16)
            for (int x = 0; x < image.getWidth(); x += 16)
                if (image.getPixelAt(x, y) != corner) ++differing;
        CHECK(differing > 50);

        juce::PNGImageFormat png;
        const auto file = outDir.getChildFile("copilot-editor-" + juce::String(juce::roundToInt(scale * 100)) + ".png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        if (out.openedOk()) png.writeImageToStream(image, out);
    }
}

TEST_CASE("Apply reports inline with Undo instead of a dialog in the host (UX-10)", "[plugin][ux10]")
{
    EditorFixture f;
    auto& ed = *f.editor;
    auto& apvts = f.proc.getAPVTS();
    auto* gain = apvts.getParameter(khensu::KhensuCopilotProcessor::ParamIDs::eqGainDb);
    REQUIRE(gain != nullptr);
    const float before = gain->getValue();

    khensu::Recommendation rec;
    rec.id = "r1";
    rec.safe_to_auto_apply = true; // OVH-8: only safe fixes auto-apply
    rec.suggested_parameters["eq_freq_hz"] = 2500.0f;
    rec.suggested_parameters["eq_gain_db"] = -3.0f;
    rec.suggested_parameters["some_unmapped_knob"] = 1.0f;
    CHECK(ed.applySuggestion(rec));

    const auto text = ed.applyStripTextForTesting();
    CHECK(text.startsWith("Applied 2 parameters"));
    CHECK(text.contains("1 to dial in by hand"));
    CHECK(gain->getValue() != before);

    ed.undoLastApplyForTesting();
    CHECK(gain->getValue() == before); // Undo puts it back
    CHECK(ed.applyStripTextForTesting().startsWith("Undone"));
}

TEST_CASE("The copilot editor's sections are named like the desktop (UX-18, VIS-9)", "[plugin][ux18][vis9]")
{
    EditorFixture f;
    auto* tabs = dynamic_cast<khensu::PluginSegmentedTabs*>(f.editor->findChildWithID("tabs"));
    REQUIRE(tabs != nullptr);
    // Segmented sections, not a JUCE TabbedComponent; "Issues" as on the
    // desktop, and "Mix Hub" so it does not read as the desktop's Mix / Master.
    CHECK(tabs->getTabNames() == juce::StringArray{"Issues", "DSP", "Plugins", "Mix Hub", "Chat", "Reference"});
    CHECK(f.editor->getTabNames() == tabs->getTabNames());
    // A click on a segment selects its section.
    auto* hub = tabs->getSegmentButton(3);
    REQUIRE(hub != nullptr);
    hub->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    CHECK(f.editor->getCurrentTabIndex() == 3);
    // Every segment has a spoken name.
    for (int i = 0; i < tabs->getNumTabs(); ++i) CHECK(tabs->getSegmentButton(i)->getTitle().isNotEmpty());
}

TEST_CASE("High contrast switches the editor palette and is saved per instance (VIS-9)", "[plugin][vis9]")
{
    EditorFixture f;
    using P = khensu::PluginLookAndFeel;
    CHECK(P::currentPalette() == P::Palette::Dark);
    const auto darkText = P::textPrimary;
    f.editor->setHighContrast(true);
    CHECK(P::currentPalette() == P::Palette::HighContrast);
    CHECK(f.proc.getUiPalette() == 1);
    CHECK(P::textPrimary == P::tokensFor(P::Palette::HighContrast)[3]);
    CHECK(P::textPrimary != darkText);
    // High contrast is at least as readable as Dark.
    CHECK(contrastRatio(P::textPrimary, P::bgDark) >= 15.0);
    CHECK(contrastRatio(P::textMuted, P::bgDark) >= 7.0);
    CHECK(contrastRatio(P::accentTeal, P::bgDark) >= 7.0);
    // Cached colours on the editor moved over: no label still holds Dark's
    // primary text colour.
    std::function<int(juce::Component&)> countDark = [&](juce::Component& c) {
        int n = 0;
        if (auto* label = dynamic_cast<juce::Label*>(&c))
            if (label->isColourSpecified(juce::Label::textColourId)
                && (label->findColour(juce::Label::textColourId).getARGB() & 0xffffffu) == (darkText.getARGB() & 0xffffffu))
                ++n;
        for (auto* child : c.getChildren()) n += countDark(*child);
        return n;
    };
    CHECK(countDark(*f.editor) == 0);

    // Saved with the instance and restored.
    juce::MemoryBlock state;
    f.proc.getStateInformation(state);
    KhensuCopilotProcessor restored;
    restored.setStateInformation(state.getData(), (int)state.getSize());
    CHECK(restored.getUiPalette() == 1);

    f.editor->setHighContrast(false);
    CHECK(P::currentPalette() == P::Palette::Dark);
    CHECK(P::textPrimary == darkText);
}
