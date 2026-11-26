// desktop-app/Source/ui/VocalWorkspace/VocalHarmonyPanel.cpp
#include "VocalHarmonyPanel.h"
#include "../shared/SessionAudioPaths.h"

namespace khensu {

namespace ft = feature_text;

VocalHarmonyPanel::VocalHarmonyPanel(IAppController& c)
    : OnDemandScanPanel(c, "Harmony MIDI", TRANS("Export Harmony MIDI..."),
                        TRANS("Write a diatonic harmony line for the lead vocal as a MIDI file.")),
      sessionRegistration_(c, *this)
{
    setExplainer(TRANS("Harmony MIDI"),
                 TRANS("Extracts the lead vocal's notes and writes one backing-harmony line - a diatonic 3rd or 5th "
                       "above or below each note, in the song's key - as a standard MIDI file for your DAW.\n\n"
                       "Diatonic means the harmony stays inside the scale, so the interval is a major or minor "
                       "3rd (or a perfect / diminished 5th) depending on the note - the way a backing singer would "
                       "sing it. The key and tempo are detected unless the service can't tell, in which case it "
                       "says so. Works best on the separated vocal stem."));

    intervalBox_.addItem(TRANS("3rd"), 1);
    intervalBox_.addItem(TRANS("5th"), 2);
    intervalBox_.setSelectedId(1, juce::dontSendNotification);
    intervalBox_.setTitle(TRANS("Harmony interval"));
    intervalBox_.setTooltip(TRANS("Harmony interval above or below the lead note (diatonic)."));
    addAndMakeVisible(intervalBox_);

    directionBox_.addItem(TRANS("above"), 1);
    directionBox_.addItem(TRANS("below"), 2);
    directionBox_.setSelectedId(1, juce::dontSendNotification);
    directionBox_.setTitle(TRANS("Harmony direction"));
    directionBox_.setTooltip(TRANS("Whether the harmony sits above or below the lead."));
    addAndMakeVisible(directionBox_);

    revealButton_.setTooltip(TRANS("Show the written MIDI file in the file browser."));
    revealButton_.setTitle(TRANS("Show MIDI file in folder"));
    revealButton_.onClick = [this] {
        if (result_.outputPath.isNotEmpty()) juce::File(result_.outputPath).revealToUser();
    };
    addChildComponent(revealButton_);

    statusLabel_.setText(idleText(), juce::dontSendNotification);
}

juce::String VocalHarmonyPanel::selectedInterval() const
{
    return intervalBox_.getSelectedId() == 2 ? "fifth" : "third";
}

juce::String VocalHarmonyPanel::selectedDirection() const
{
    return directionBox_.getSelectedId() == 2 ? "below" : "above";
}

juce::String VocalHarmonyPanel::idleText() const
{
    return app_.getStemFile("vocals").existsAsFile()
               ? TRANS("Uses the separated vocal stem.")
               : TRANS("No vocal stem - uses the mix (separate stems for a cleaner line).");
}

void VocalHarmonyPanel::clearResult()
{
    result_ = {};
    revealButton_.setVisible(false);
}

void VocalHarmonyPanel::layoutBody(juce::Rectangle<int> area)
{
    auto row = area.removeFromTop(24);
    intervalBox_.setBounds(row.removeFromLeft(80));
    row.removeFromLeft(6);
    directionBox_.setBounds(row.removeFromLeft(90));
    row.removeFromLeft(10);
    revealButton_.setBounds(row.removeFromLeft(juce::jmin(120, row.getWidth())));
    area.removeFromTop(6);
    textArea_ = area;
}

void VocalHarmonyPanel::run()
{
    const juce::String input = session_audio::vocalStemOrSourcePath(app_);
    if (input.isEmpty()) {
        statusLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    const juce::File source(app_.activeSourcePath().isNotEmpty() ? app_.activeSourcePath() : input);
    const auto suggested = ft::defaultHarmonyMidiFile(source, selectedInterval(), selectedDirection());
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Save harmony MIDI"), suggested, "*.mid;*.midi");
    juce::Component::SafePointer<VocalHarmonyPanel> self(this);
    const auto epoch = app_.sessionEpoch();
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [self, input, epoch](const juce::FileChooser& fc) {
                              if (self == nullptr) return;
                              if (self->app_.sessionEpoch() != epoch) return; // session changed meanwhile
                              auto file = fc.getResult();
                              if (file == juce::File()) return; // cancelled
                              if (!file.hasFileExtension(".mid;.midi")) file = file.withFileExtension(".mid");
                              self->exportTo(input, file);
                          });
}

void VocalHarmonyPanel::exportTo(const juce::String& input, const juce::File& midiFile)
{
    ipc::HttpServiceBridge::VocalHarmonyOptions options;
    options.interval = selectedInterval();
    options.direction = selectedDirection();
    options.outputPath = midiFile.getFullPathName();
    revealButton_.setVisible(false);
    startScan(
        TRANS("Extracting the vocal line and writing") + " " + midiFile.getFileName() + "...",
        [input, options] { return ipc::HttpServiceBridge{}.generateVocalHarmony(input, options); },
        [this](const ipc::HttpServiceBridge::VocalHarmonyResult& r) {
            result_ = r;
            const bool written = r.outputPath.isNotEmpty() && juce::File(r.outputPath).existsAsFile();
            revealButton_.setVisible(written);
            statusLabel_.setText(written ? TRANS("MIDI written - drag it into your DAW.")
                                         : TRANS("The service reported no MIDI file."),
                                 juce::dontSendNotification);
        },
        /*longPool=*/true); // pitch extraction can take minutes
}

void VocalHarmonyPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!showsResult()) return;
    g.setColour(result_.lowQualityExtraction ? KhensuTheme::accentAmber : KhensuTheme::textPrimary);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.drawFittedText(ft::vocalHarmonySummary(result_), textArea_, juce::Justification::topLeft,
                     juce::jmax(1, textArea_.getHeight() / 13), 1.0f);
}

} // namespace khensu
