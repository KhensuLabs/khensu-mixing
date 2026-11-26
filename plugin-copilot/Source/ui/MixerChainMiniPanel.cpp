#include "MixerChainMiniPanel.h"
#include <khensu/ui/KhensuPalette.h>
#include "PluginLookAndFeel.h"
#include "../PluginProcessor.h"
#include "../CopilotServiceJobs.h"
#include <khensu/mixerchain/DspChainMapper.h>

namespace khensu {

MixerChainMiniPanel::MixerChainMiniPanel(KhensuCopilotProcessor* proc) : processor_(proc)
{
    // Left Column Inputs
    targetLabel.setText("Target Channel:", juce::dontSendNotification);
    addAndMakeVisible(targetLabel);
    targetCombo.addItem("Vocals", 1);
    targetCombo.addItem("Instruments", 2);
    targetCombo.addItem("Mixer Bus", 3);
    targetCombo.addItem("Master", 4);
    targetCombo.setSelectedId(1);
    addAndMakeVisible(targetCombo);

    genreLabel.setText("Genre:", juce::dontSendNotification);
    addAndMakeVisible(genreLabel);
    genreCombo.addItem("Pop", 1);
    genreCombo.addItem("Rock", 2);
    genreCombo.addItem("Hip-Hop", 3);
    genreCombo.addItem("Electronic", 4);
    genreCombo.addItem("Acoustic", 5);
    genreCombo.addItem("Jazz", 6);
    genreCombo.setSelectedId(1);
    addAndMakeVisible(genreCombo);

    energyLabel.setText("Energy Level:", juce::dontSendNotification);
    addAndMakeVisible(energyLabel);
    energyCombo.addItem("Low", 1);
    energyCombo.addItem("Mid", 2);
    energyCombo.addItem("High", 3);
    energyCombo.setSelectedId(2);
    addAndMakeVisible(energyCombo);

    moodLabel.setText("Mood:", juce::dontSendNotification);
    addAndMakeVisible(moodLabel);
    moodCombo.addItem("Dreamy", 1);
    moodCombo.addItem("Aggressive", 2);
    moodCombo.addItem("Warm", 3);
    moodCombo.addItem("Bright", 4);
    moodCombo.addItem("Vintage", 5);
    moodCombo.addItem("Modern", 6);
    moodCombo.addItem("Clean", 7);
    moodCombo.setSelectedId(3);
    addAndMakeVisible(moodCombo);

    bpmLabel.setText("BPM:", juce::dontSendNotification);
    addAndMakeVisible(bpmLabel);
    bpmInput.setText("120", false);
    addAndMakeVisible(bpmInput);

    styleLabel.setText("Style:", juce::dontSendNotification);
    addAndMakeVisible(styleLabel);
    styleInput.setText("radio presence and warmth", false);
    addAndMakeVisible(styleInput);

    // Ecosystem Toggles
    ecoViewport.setScrollBarsShown(true, false);
    ecoViewport.setViewedComponent(&ecoContent, false);
    ecoViewport.setTitle("Plugin ecosystem");
    addAndMakeVisible(ecoViewport);

    const juce::StringArray ecosystems = {
        "Stock FL Studio", "Stock Ableton Live", "Tuned Plugins",   "Universal Audio (UAD)", "Valhalla",    "XLN Audio",
        "Neural DSP",      "oeksound",           "Plugin Alliance", "Make Believe Studios",  "Safari Audio"};

    int toggleY = 5;
    for (const auto& name : ecosystems) {
        auto* toggle = new juce::ToggleButton(name);
        if (name == "Universal Audio (UAD)" || name == "Valhalla" || name == "Stock Ableton Live")
            toggle->setToggleState(true, juce::dontSendNotification);

        toggle->setBounds(5, toggleY, 160, 22);
        ecoContent.addAndMakeVisible(toggle);
        ecoToggles.push_back(std::unique_ptr<juce::ToggleButton>(toggle));
        toggleY += 24;
    }
    ecoContent.setSize(160, toggleY + 5);

    generateButton.setButtonText("Generate Mixer Chain");
    generateButton.onClick = [this]() { generateChain(); };
    addAndMakeVisible(generateButton);

    applyButton.setButtonText("Load into DSP chain");
    applyButton.setEnabled(false); // enabled once a chain has been generated
    applyButton.onClick = [this]() { applyChainToProcessor(); };
    addAndMakeVisible(applyButton);

    // Right Column Results
    descriptionLabel.setText("Click Generate to create custom insert chain", juce::dontSendNotification);
    descriptionLabel.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    descriptionLabel.setFont(khensu::ui::Palette::uiFont(khensu::type::caption, juce::Font::italic));
    addAndMakeVisible(descriptionLabel);

    resultsViewport.setScrollBarsShown(true, false);
    resultsViewport.setViewedComponent(&resultsContent, false);
    resultsViewport.setTitle("Generated chain");
    addAndMakeVisible(resultsViewport);
}

void MixerChainMiniPanel::paint(juce::Graphics& g)
{
    g.fillAll(PluginLookAndFeel::bgDark);

    // Panel borders
    g.setColour(PluginLookAndFeel::controlOutline);
    g.drawRect(getLocalBounds(), 1);
}

void MixerChainMiniPanel::resized()
{
    auto b = getLocalBounds().reduced(8);
    const int midX = b.getWidth() * 45 / 100;

    auto leftCol = b.removeFromLeft(midX);
    b.removeFromLeft(8);
    auto rightCol = b;

    // Left Column Layout
    const int labelH = 15;
    const int inputH = 22;
    const int gap = 4;

    targetLabel.setBounds(leftCol.removeFromTop(labelH));
    targetCombo.setBounds(leftCol.removeFromTop(inputH));
    leftCol.removeFromTop(gap);

    auto row = leftCol.removeFromTop(labelH + inputH + gap);
    auto lLeft = row.removeFromLeft(row.getWidth() / 2 - 4);
    row.removeFromLeft(8);
    auto lRight = row;

    genreLabel.setBounds(lLeft.removeFromTop(labelH));
    genreCombo.setBounds(lLeft.removeFromTop(inputH));

    energyLabel.setBounds(lRight.removeFromTop(labelH));
    energyCombo.setBounds(lRight.removeFromTop(inputH));

    leftCol.removeFromTop(gap);

    auto row2 = leftCol.removeFromTop(labelH + inputH + gap);
    auto lLeft2 = row2.removeFromLeft(row2.getWidth() / 2 - 4);
    row2.removeFromLeft(8);
    auto lRight2 = row2;

    moodLabel.setBounds(lLeft2.removeFromTop(labelH));
    moodCombo.setBounds(lLeft2.removeFromTop(inputH));

    bpmLabel.setBounds(lRight2.removeFromTop(labelH));
    bpmInput.setBounds(lRight2.removeFromTop(inputH));

    leftCol.removeFromTop(gap);

    styleLabel.setBounds(leftCol.removeFromTop(labelH));
    styleInput.setBounds(leftCol.removeFromTop(36));
    leftCol.removeFromTop(6);

    ecoViewport.setBounds(leftCol.removeFromBottom(100));
    leftCol.removeFromBottom(4);
    ecoContent.setSize(ecoViewport.getWidth() - ecoViewport.getScrollBarThickness(), ecoContent.getHeight());

    generateButton.setBounds(leftCol);

    // Right Column Layout
    descriptionLabel.setBounds(rightCol.removeFromTop(30));
    rightCol.removeFromTop(4);
    applyButton.setBounds(rightCol.removeFromBottom(26));
    rightCol.removeFromBottom(4);
    resultsViewport.setBounds(rightCol);
    resultsContent.setSize(resultsViewport.getWidth() - resultsViewport.getScrollBarThickness(),
                           resultsContent.getHeight());
}

void MixerChainMiniPanel::generateChain()
{
    if (isGenerating_) return;
    if (processor_ == nullptr) {
        // No processor, no owned pool to run on (WIRE-15: the unmanaged
        // juce::Thread::launch fallback is gone).
        descriptionLabel.setText("Generation needs the plugin processor.", juce::dontSendNotification);
        return;
    }

    isGenerating_ = true;
    generateButton.setButtonText("Generating...");
    generateButton.setEnabled(false);

    // The service's chain templates are keyed "mixer_bus", "vocals", ...
    // (mixer_chain_generator.py); it only lowercases, so "Mixer Bus" missed.
    const juce::String target = targetCombo.getText().toLowerCase().replace(" ", "_");
    const juce::String genre = genreCombo.getText();
    const juce::String energy = energyCombo.getText();
    const juce::String mood = moodCombo.getText();
    const double bpm = bpmInput.getText().getDoubleValue();
    const juce::String style = styleInput.getText();

    juce::StringArray enabledEco;
    for (const auto& toggle : ecoToggles) {
        if (toggle->getToggleState()) enabledEco.add(toggle->getButtonText());
    }

    juce::Component::SafePointer<MixerChainMiniPanel> self(this);
    auto job = [self, target, genre, energy, mood, bpm, style, enabledEco]() {
        // Cancellable job instead of one blocking LLM round trip (audit H3):
        // the processor's pool asks running jobs to exit on unload.
        const copilot::ShouldCancel shouldCancel = [] { return copilot::currentBackgroundJobShouldExit(); };
        auto res = copilot::generateMixerChainCancellable(target, genre, energy, mood, bpm, style, enabledEco,
                                                          shouldCancel);
        if (shouldCancel()) return;

        juce::MessageManager::callAsync([self, res]() {
            if (self == nullptr) return;
            self->isGenerating_ = false;
            self->generateButton.setButtonText("Generate Mixer Chain");
            self->generateButton.setEnabled(true);

            if (res.is_ok()) {
                self->lastResponse_ = res.value();
                self->descriptionLabel.setText(self->lastResponse_.description, juce::dontSendNotification);
                self->descriptionLabel.setColour(juce::Label::textColourId, PluginLookAndFeel::textPrimary);
                self->applyButton.setEnabled(!self->lastResponse_.plugins.empty() && self->processor_ != nullptr);
                self->updateResultsView();
            } else {
                self->descriptionLabel.setText("Failed to communicate with service.", juce::dontSendNotification);
                self->descriptionLabel.setColour(juce::Label::textColourId, PluginLookAndFeel::statusBad);
            }
        });
    };
    processor_->runInBackground(std::move(job));
}

class MiniPluginCard : public juce::Component {
  public:
    MiniPluginCard(const juce::String& name, const juce::String& cat, const juce::String& eco,
                   const juce::String& settings)
        : name_(name), cat_(cat), eco_(eco), settings_(settings)
    {}

    void paint(juce::Graphics& g) override
    {
        g.fillAll(PluginLookAndFeel::bgPanel);
        g.setColour(PluginLookAndFeel::accentTeal);
        g.drawRect(getLocalBounds(), 1);

        g.setColour(PluginLookAndFeel::textPrimary);
        g.setFont(khensu::ui::Palette::uiFont(khensu::type::caption, juce::Font::bold));
        g.drawText(name_, 8, 4, getWidth() - 16, 14, juce::Justification::topLeft);

        g.setFont(khensu::ui::Palette::uiFont(khensu::type::caption, juce::Font::plain));
        g.setColour(PluginLookAndFeel::accentTeal);
        g.drawText(cat_ + "  (" + eco_ + ")", 8, 18, getWidth() - 16, 12, juce::Justification::topLeft);

        g.setColour(PluginLookAndFeel::textMuted);
        g.drawFittedText(settings_, juce::Rectangle<int>(8, 30, getWidth() - 16, 32), juce::Justification::topLeft, 2);
    }

  private:
    juce::String name_, cat_, eco_, settings_;
};

void MixerChainMiniPanel::updateResultsView()
{
    resultsContent.removeAllChildren();
    resultsComponents.clear();

    int y = 5;
    const int w = resultsViewport.getWidth() - resultsViewport.getScrollBarThickness();

    for (const auto& p : lastResponse_.plugins) {
        auto* card = new MiniPluginCard(p.name, p.category, p.ecosystem, p.settings_description);
        card->setBounds(5, y, w - 10, 68);
        resultsComponents.add(card);
        resultsContent.addAndMakeVisible(card);
        y += 74;
    }

    resultsContent.setSize(w, y + 10);
    resultsViewport.setViewedComponent(&resultsContent, false);
}

void MixerChainMiniPanel::applyChainToProcessor()
{
    // Maps the generated third-party plugin recipe onto the built-in corrective
    // DSP chain (HPF / LPF, six EQ bands, compressor, limiter, de-esser,
    // saturation, transient shaper, width, bass mono). This is an
    // approximation of the recipe's broad strokes - the exact plugins listed
    // still need to be loaded in the DAW for their specific character.
    // The parsing/mapping logic lives in shared-core's DspChainMapper so the
    // desktop app's MixerChainPanel can push the same approximation to the
    // plugin via HttpServiceBridge::postPluginParams (see MixerChainPanel::pushToPluginDsp).
    if (processor_ == nullptr || lastResponse_.plugins.empty()) {
        descriptionLabel.setText("Generate a chain first.", juce::dontSendNotification);
        return;
    }

    // STU-17: every EQ band and the v5 stages; what cannot map is listed.
    const auto mapping = mixerchain::mapRecipeToDsp(lastResponse_.plugins);
    bool loaded = false;
    if (onApplyRecipe) {
        loaded = onApplyRecipe(mapping);
    } else if (!mapping.values.empty()) {
        // One complete set: stages this chain does not map are switched off, so a
        // previous Apply's stages no longer stay on underneath it (PLG-19).
        processor_->applyAiParameterSet(mapping.values);
        loaded = true;
    }

    if (!loaded) {
        descriptionLabel.setText(TRANS("No stages could be mapped to the built-in DSP - this chain is a recipe: "
                                       "load the listed plugins on your DAW insert slots."),
                                 juce::dontSendNotification);
    } else {
        juce::StringArray stagesArr;
        for (const auto& s : mapping.mappedStages)
            stagesArr.add(juce::String(s));
        juce::String text = TRANS("Loaded an approximation into the DSP chain") + " (" + stagesArr.joinIntoString(", ")
                            + ").";
        if (!mapping.skipped.empty())
            text << " " << static_cast<int>(mapping.skipped.size()) << " " << TRANS("skipped - see the apply strip.");
        text << " " << TRANS("For the exact character, load the listed plugins in your DAW.");
        descriptionLabel.setText(text, juce::dontSendNotification);
        if (onLoadedIntoDsp) onLoadedIntoDsp();
    }
}

} // namespace khensu
