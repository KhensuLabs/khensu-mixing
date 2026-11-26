#include "ReferencePanel.h"
#include "ButtonRoles.h"
#include "PluginLookAndFeel.h"
#include "../PluginProcessor.h"
#include <khensu/ui/KhensuPalette.h>
#include <khensu/ui/StudioStyle.h>

#include <cmath>

namespace khensu {

namespace {
using MatchMode = copilot::reference::ReferencePlayer::MatchMode;

juce::String lufsText(float v) { return v > -90.0f ? juce::String(v, 1) : juce::String("-"); }

juce::String deltaText(float track, float ref, bool valid)
{
    if (!valid) return "-";
    const float d = track - ref;
    return (d >= 0.0f ? "+" : "") + juce::String(d, 1);
}
} // namespace

ReferencePanel::ReferencePanel(KhensuCopilotProcessor& processor) : processor_(processor)
{
    setTitle(TRANS("Reference"));
    setHelpText(TRANS("Load a reference track, hear it loudness matched against this track, and compare loudness, "
                      "dynamics and tonal balance."));

    loadButton_.setButtonText(TRANS("Load reference..."));
    loadButton_.setComponentID("loadReferenceButton");
    loadButton_.setTooltip(TRANS("Choose a reference track (WAV, AIFF, FLAC, MP3, OGG). You can also drop a file on "
                                 "this tab."));
    copilot::button_role::markPrimary(loadButton_);
    loadButton_.onClick = [this] { chooseFile(); };
    addAndMakeVisible(loadButton_);

    clearButton_.setButtonText(TRANS("Clear"));
    clearButton_.setTooltip(TRANS("Remove the reference"));
    clearButton_.onClick = [this] {
        processor_.clearReference();
        refresh();
    };
    addAndMakeVisible(clearButton_);

    listenButton_.setButtonText(TRANS("Hear reference"));
    listenButton_.setComponentID("hearReferenceButton");
    listenButton_.setClickingTogglesState(true);
    listenButton_.setTooltip(TRANS("Play the reference instead of this track, turned to the same loudness, so you "
                                   "compare sound and not level. Never in an offline render."));
    listenButton_.onClick = [this] { processor_.setReferenceListening(listenButton_.getToggleState()); };
    addAndMakeVisible(listenButton_);

    // Combo items are display text; the mode is stored as its index.
    matchBox_.addItem(TRANS("Match integrated"), 1);
    matchBox_.addItem(TRANS("Match short-term"), 2);
    matchBox_.setTitle(TRANS("Loudness match"));
    matchBox_.setTooltip(TRANS("Integrated: the reference plays at the track's integrated loudness (steady). "
                               "Short-term: it follows the last 3 seconds of both, like riding a fader."));
    matchBox_.onChange = [this] {
        processor_.setReferenceMatchMode(matchBox_.getSelectedId() == 2 ? MatchMode::ShortTerm : MatchMode::Integrated);
    };
    addAndMakeVisible(matchBox_);

    resetButton_.setButtonText(TRANS("Reset track"));
    resetButton_.setTooltip(TRANS("Start the track's loudness, range and spectrum measurement again"));
    resetButton_.onClick = [this] {
        processor_.resetTrackStats();
        trackPower_.fill(0.0);
        trackFrames_ = 0;
        refresh();
    };
    addAndMakeVisible(resetButton_);

    for (auto* label : {&nameLabel_, &offsetLabel_}) {
        label->setFont(ui::Palette::uiFont(ui::studio::kBodySize));
        label->setColour(juce::Label::textColourId, PluginLookAndFeel::textPrimary);
        label->setMinimumHorizontalScale(0.8f);
        addAndMakeVisible(*label);
    }
    nameLabel_.setTitle(TRANS("Reference file"));
    offsetLabel_.setTitle(TRANS("Level offset"));
    refresh();
}

ReferencePanel::~ReferencePanel() { stopTimer(); }

void ReferencePanel::visibilityChanged()
{
    if (isVisible()) {
        refresh();
        startTimerHz(4);
    } else {
        stopTimer();
    }
}

bool ReferencePanel::isInterestedInFileDrag(const juce::StringArray& files)
{
    return !files.isEmpty() && copilot::reference::isReferenceFile(juce::File(files[0]));
}

void ReferencePanel::fileDragEnter(const juce::StringArray&, int, int)
{
    dragOver_ = true;
    repaint();
}

void ReferencePanel::fileDragExit(const juce::StringArray&)
{
    dragOver_ = false;
    repaint();
}

void ReferencePanel::filesDropped(const juce::StringArray& files, int, int)
{
    dragOver_ = false;
    if (!files.isEmpty()) loadFile(juce::File(files[0]));
    repaint();
}

void ReferencePanel::chooseFile()
{
    const auto current = processor_.getReferencePath();
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Choose a reference track"),
                                                   current.isNotEmpty() ? juce::File(current).getParentDirectory()
                                                                        : juce::File(),
                                                   copilot::reference::kReferenceWildcard);
    juce::Component::SafePointer<ReferencePanel> safeThis(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safeThis](const juce::FileChooser& chooser) {
                              auto* self = safeThis.getComponent();
                              if (self == nullptr) return;
                              const auto file = chooser.getResult();
                              if (file.existsAsFile()) self->loadFile(file);
                          });
}

void ReferencePanel::loadFile(const juce::File& file)
{
    juce::Component::SafePointer<ReferencePanel> safeThis(this);
    processor_.loadReferenceAsync(file, [safeThis](bool, juce::String) {
        if (auto* self = safeThis.getComponent()) self->refresh();
    });
    refresh();
}

void ReferencePanel::refresh()
{
    const auto ref = processor_.getReference();
    const auto status = processor_.getReferenceStatus();
    juce::String name;
    if (processor_.isReferenceLoading())
        name = TRANS("Loading") + " " + juce::File(processor_.getReferencePath()).getFileName() + "...";
    else if (status.isNotEmpty())
        name = status;
    else if (ref != nullptr)
        name = ref->name() + "  (" + juce::String(ref->durationSec() / 60.0, 1) + " " + TRANS("min") + ")"
               + (ref->truncated ? " - " + TRANS("first 15 min") : juce::String());
    else
        name = TRANS("No reference - load one or drop a file here.");
    if (nameLabel_.getText() != name) nameLabel_.setText(name, juce::dontSendNotification);
    nameLabel_.setColour(juce::Label::textColourId,
                         status.isNotEmpty() && !processor_.isReferenceLoading() ? PluginLookAndFeel::statusWarn
                                                                                 : PluginLookAndFeel::textPrimary);

    const bool have = ref != nullptr;
    clearButton_.setEnabled(have || processor_.getReferencePath().isNotEmpty());
    listenButton_.setEnabled(have);
    listenButton_.setToggleState(processor_.isReferenceListening() && have, juce::dontSendNotification);
    matchBox_.setSelectedId(processor_.getReferenceMatchMode() == MatchMode::ShortTerm ? 2 : 1,
                            juce::dontSendNotification);

    const auto& player = processor_.referencePlayer();
    juce::String offset;
    if (!have)
        offset = {};
    else if (!player.isMatched())
        offset = TRANS("Level offset: waiting for track audio to match against");
    else
        offset = TRANS("Reference") + " " + (player.offsetDb() >= 0.0f ? "+" : "") + juce::String(player.offsetDb(), 1)
                 + " dB " + TRANS("to match the track")
                 + (processor_.getReferenceMatchMode() == MatchMode::ShortTerm ? " (" + TRANS("short-term") + ")"
                                                                               : " (" + TRANS("integrated") + ")");
    if (offsetLabel_.getText() != offset) offsetLabel_.setText(offset, juce::dontSendNotification);

    // The track's long-term spectrum from its own hub frames (new ones only).
    for (const auto& frame : processor_.getMixHub().snapshot(mixhub::MixHub::nowMs())) {
        if (frame.instanceId != processor_.getMixHubInstanceId() || !frame.hasSignal()) continue;
        if (frame.updatedMs <= lastFrameMs_) continue;
        lastFrameMs_ = frame.updatedMs;
        for (size_t b = 0; b < trackPower_.size(); ++b)
            trackPower_[b] += frame.bandDb[b] > mixhub::kSilentDb ? std::pow(10.0, frame.bandDb[b] / 10.0) : 0.0;
        ++trackFrames_;
    }
    shownReference_ = ref;
    setDescription(dynamicsLines().joinIntoString("; "));
    repaint();
}

juce::StringArray ReferencePanel::dynamicsLines() const
{
    juce::StringArray lines;
    const auto track = processor_.getTrackStats();
    const auto ref = shownReference_;
    const bool haveRef = ref != nullptr;
    const auto refStats = haveRef ? ref->stats : copilot::reference::LoudnessStats::Result{};
    const auto row = [&](const juce::String& name, float t, float r, bool tValid, bool rValid, const juce::String& unit) {
        lines.add(name + ": " + TRANS("track") + " " + (tValid ? juce::String(t, 1) : juce::String("-")) + ", "
                  + TRANS("reference") + " " + (rValid ? juce::String(r, 1) : juce::String("-")) + ", "
                  + TRANS("difference") + " " + deltaText(t, r, tValid && rValid) + " " + unit);
    };
    row(TRANS("Integrated"), track.integratedLufs, refStats.integratedLufs, track.integratedLufs > -90.0f,
        haveRef && refStats.integratedLufs > -90.0f, "LU");
    row(TRANS("Short-term"), track.shortTermLufs,
        haveRef ? ref->shortTermAt(static_cast<juce::int64>(processor_.referencePlayer().positionSec() * ref->sampleRate))
                : -96.0f,
        track.shortTermLufs > -90.0f, haveRef, "LU");
    row(TRANS("Loudness range"), track.loudnessRangeLu, refStats.loudnessRangeLu, track.valid, haveRef, "LU");
    row(TRANS("Crest factor"), track.crestDb, refStats.crestDb, track.valid, haveRef, "dB");
    row(TRANS("Peak"), track.peakDb, refStats.peakDb, track.peakDb > -90.0f, haveRef, "dB");
    return lines;
}

std::vector<float> ReferencePanel::tonalDelta() const
{
    if (shownReference_ == nullptr || trackFrames_ == 0) return {};
    mixhub::BandLevels track = mixhub::silentBands();
    for (size_t b = 0; b < track.size(); ++b) {
        const double p = trackPower_[b] / trackFrames_;
        track[b] = p > 1.0e-12 ? static_cast<float>(10.0 * std::log10(p)) : mixhub::kSilentDb;
    }
    const auto t = copilot::reference::normalisedShape(track);
    const auto r = copilot::reference::normalisedShape(shownReference_->bands);
    std::vector<float> delta(t.size(), 0.0f);
    for (size_t b = 0; b < t.size(); ++b)
        delta[b] = (t[b] > mixhub::kSilentDb && r[b] > mixhub::kSilentDb) ? t[b] - r[b] : 0.0f;
    return delta;
}

void ReferencePanel::paint(juce::Graphics& g)
{
    using P = PluginLookAndFeel;
    g.fillAll(P::bgDark);
    if (dragOver_) {
        g.setColour(P::accentTeal.withAlpha(0.6f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 6.0f, 2.0f);
    }
    paintDynamics(g, dynamicsArea_);
    paintTonal(g, tonalArea_);
}

void ReferencePanel::paintDynamics(juce::Graphics& g, juce::Rectangle<int> area) const
{
    using P = PluginLookAndFeel;
    if (area.isEmpty()) return;
    g.setColour(P::bgPanel);
    g.fillRoundedRectangle(area.toFloat(), ui::studio::kRadius);
    area = area.reduced(8, 6);
    g.setColour(P::textMuted);
    g.setFont(ui::Palette::uiFont(ui::studio::kLabelSize, juce::Font::bold));
    auto header = area.removeFromTop(16);
    const int nameW = area.getWidth() * 40 / 100, colW = (area.getWidth() - nameW) / 3;
    header.removeFromLeft(nameW);
    for (const auto* text : {NEEDS_TRANS("TRACK"), NEEDS_TRANS("REFERENCE"), NEEDS_TRANS("DIFF")})
        g.drawText(juce::translate(text), header.removeFromLeft(colW), juce::Justification::centredRight);

    const auto track = processor_.getTrackStats();
    const auto ref = shownReference_;
    const auto rs = ref != nullptr ? ref->stats : copilot::reference::LoudnessStats::Result{};
    const float refShort = ref != nullptr ? ref->shortTermAt(static_cast<juce::int64>(
                                                processor_.referencePlayer().positionSec() * ref->sampleRate))
                                          : -96.0f;
    struct Row {
        juce::String name;
        float t, r;
        bool tv, rv;
    };
    const Row rows[] = {
        {TRANS("Integrated LUFS"), track.integratedLufs, rs.integratedLufs, track.integratedLufs > -90.0f,
         ref != nullptr && rs.integratedLufs > -90.0f},
        {TRANS("Short-term LUFS"), track.shortTermLufs, refShort, track.shortTermLufs > -90.0f,
         ref != nullptr && refShort > -90.0f},
        {TRANS("Loudness range LU"), track.loudnessRangeLu, rs.loudnessRangeLu, track.valid, ref != nullptr},
        {TRANS("Crest factor dB"), track.crestDb, rs.crestDb, track.valid, ref != nullptr},
        {TRANS("Peak dBFS"), track.peakDb, rs.peakDb, track.peakDb > -90.0f, ref != nullptr},
    };
    const int rowH = juce::jlimit(16, 24, area.getHeight() / 6);
    for (const auto& row : rows) {
        auto r = area.removeFromTop(rowH);
        if (r.getHeight() < 12) break;
        g.setFont(ui::Palette::uiFont(ui::studio::kBodySize));
        g.setColour(P::textMuted);
        g.drawText(row.name, r.removeFromLeft(nameW), juce::Justification::centredLeft, true);
        g.setColour(P::textPrimary);
        g.setFont(ui::studio::numberFont(ui::studio::kBodySize));
        g.drawText(row.tv ? juce::String(row.t, 1) : juce::String("-"), r.removeFromLeft(colW),
                   juce::Justification::centredRight);
        g.drawText(row.rv ? juce::String(row.r, 1) : juce::String("-"), r.removeFromLeft(colW),
                   juce::Justification::centredRight);
        g.setColour(P::accentTeal);
        g.drawText(deltaText(row.t, row.r, row.tv && row.rv), r.removeFromLeft(colW), juce::Justification::centredRight);
    }
}

void ReferencePanel::paintTonal(juce::Graphics& g, juce::Rectangle<int> area) const
{
    using P = PluginLookAndFeel;
    if (area.isEmpty()) return;
    g.setColour(P::bgPanel);
    g.fillRoundedRectangle(area.toFloat(), ui::studio::kRadius);
    area = area.reduced(8, 6);
    g.setColour(P::textMuted);
    g.setFont(ui::Palette::uiFont(ui::studio::kLabelSize, juce::Font::bold));
    g.drawText(TRANS("TONAL BALANCE: TRACK MINUS REFERENCE (dB, +/-12)"), area.removeFromTop(16),
               juce::Justification::centredLeft, true);
    auto labels = area.removeFromBottom(14);
    const auto delta = tonalDelta();
    if (delta.empty()) {
        g.setFont(ui::Palette::uiFont(ui::studio::kBodySize));
        g.drawFittedText(shownReference_ == nullptr ? TRANS("Load a reference to compare the tonal balance.")
                                                    : TRANS("Play the track to measure its spectrum."),
                         area, juce::Justification::centred, 2);
        return;
    }
    const float midY = static_cast<float>(area.getCentreY());
    g.setColour(P::textPrimary.withAlpha(0.15f));
    g.drawHorizontalLine(juce::roundToInt(midY), static_cast<float>(area.getX()), static_cast<float>(area.getRight()));
    const float barW = static_cast<float>(area.getWidth()) / static_cast<float>(delta.size());
    const float half = static_cast<float>(area.getHeight()) * 0.5f;
    for (size_t b = 0; b < delta.size(); ++b) {
        const float d = juce::jlimit(-12.0f, 12.0f, delta[b]);
        const float h = std::abs(d) / 12.0f * half;
        const float x = static_cast<float>(area.getX()) + static_cast<float>(b) * barW + 1.0f;
        // Above the line: the track has more there than the reference.
        g.setColour((d >= 0.0f ? P::accentTeal : P::accentBlue).withAlpha(0.85f));
        g.fillRect(x, d >= 0.0f ? midY - h : midY, barW - 2.0f, h);
    }
    g.setColour(P::textMuted);
    g.setFont(ui::Palette::uiFont(ui::studio::kLabelSize));
    for (const float hz : {50.0f, 200.0f, 1000.0f, 5000.0f, 12000.0f}) {
        // The band whose centre is nearest.
        int best = 0;
        for (int b = 1; b < mixhub::kNumBands; ++b)
            if (std::abs(std::log(mixhub::bandCentreHz(b) / hz)) < std::abs(std::log(mixhub::bandCentreHz(best) / hz)))
                best = b;
        const int x = labels.getX() + juce::roundToInt((static_cast<float>(best) + 0.5f) * barW);
        g.drawText(mixhub::formatHz(hz), x - 30, labels.getY(), 60, labels.getHeight(), juce::Justification::centred);
    }
}

void ReferencePanel::resized()
{
    auto b = getLocalBounds().reduced(ui::studio::kGap, 6);
    auto row1 = b.removeFromTop(30);
    loadButton_.setBounds(row1.removeFromLeft(140).reduced(0, 1));
    row1.removeFromLeft(6);
    clearButton_.setBounds(row1.removeFromLeft(64).reduced(0, 1));
    row1.removeFromLeft(10);
    nameLabel_.setBounds(row1);
    b.removeFromTop(6);
    auto row2 = b.removeFromTop(30);
    listenButton_.setBounds(row2.removeFromLeft(140).reduced(0, 1));
    row2.removeFromLeft(6);
    matchBox_.setBounds(row2.removeFromLeft(160).reduced(0, 2));
    row2.removeFromLeft(10);
    resetButton_.setBounds(row2.removeFromRight(100).reduced(0, 1));
    row2.removeFromRight(6);
    offsetLabel_.setBounds(row2);
    b.removeFromTop(ui::studio::kGap);
    if (b.getWidth() >= 560) {
        dynamicsArea_ = b.removeFromLeft(b.getWidth() * 45 / 100);
        b.removeFromLeft(ui::studio::kGap);
        tonalArea_ = b;
    } else {
        dynamicsArea_ = b.removeFromTop(b.getHeight() / 2);
        b.removeFromTop(ui::studio::kGap);
        tonalArea_ = b;
    }
}

} // namespace khensu
