// desktop-app/Source/ui/Dashboard/RecreateSoundPanel.cpp
#include "../shared/ServiceErrorHint.h"
#include "RecreateSoundPanel.h"
#include <khensu/session/Session.h>
#include "../VocalWorkspace/MixTuningReference.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this panel, in one place (UI-01).
void RecreateSoundPanel::applyThemeColours()
{
    for (auto* s : {&timingVarSlider_, &velocityVarSlider_, &pitchVarSlider_})
        s->setColour(juce::Slider::trackColourId, KhensuTheme::accentTeal.withAlpha(0.4f));
    for (auto* l : {&timingVarLabel_, &velocityVarLabel_, &pitchVarLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

RecreateSoundPanel::RecreateSoundPanel(IAppController& c) : PanelCard(c, "Recreate Sound")
{
    applyThemeColours();
    setExplainer(TRANS("Recreate Sound"), TRANS("Reverse-engineers a stem into (1) the notes it plays and (2) the kind of "
                                   "sound it is - sub, 808, bass, reese, lead, pad, fx, guitar, kick, "
                                   "snare, hat or perc - then hands you concrete recipes for rebuilding "
                                   "it in your own instruments: which synth/sampler and patch to start from and "
                                   "an FX chain (FabFilter, SSL, Arturia, NI, IK) that gets you in the ballpark. "
                                   "It classifies from timbre and the extracted MIDI; treat the recipes as a "
                                   "starting point, not a clone."));

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);

    midiVarButton_.onClick = [this] { runMidiVariation(); };
    midiVarButton_.setEnabled(false);
    addAndMakeVisible(midiVarButton_);

    timingVarSlider_.setRange(0.0, 100.0, 1.0);
    timingVarSlider_.setValue(15.0);
    timingVarSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 16);
    addAndMakeVisible(timingVarSlider_);

    timingVarLabel_.setText(TRANS("Timing (ms):"), juce::dontSendNotification);
    timingVarLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(timingVarLabel_);

    velocityVarSlider_.setRange(0.0, 40.0, 1.0);
    velocityVarSlider_.setValue(12.0);
    velocityVarSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 16);
    addAndMakeVisible(velocityVarSlider_);

    velocityVarLabel_.setText(TRANS("Velocity:"), juce::dontSendNotification);
    velocityVarLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(velocityVarLabel_);

    pitchVarSlider_.setRange(0.0, 100.0, 1.0);
    pitchVarSlider_.setValue(5.0);
    pitchVarSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 16);
    addAndMakeVisible(pitchVarSlider_);

    pitchVarLabel_.setText(TRANS("Pitch Var:"), juce::dontSendNotification);
    pitchVarLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(pitchVarLabel_);

    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Runs on the loaded stem; re-run any time."));
}

void RecreateSoundPanel::runAnalysis()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    const juce::String path(s->files[0].path);
    const juce::String mixPath = controller_.activeSourcePath();
    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    options.failedText = TRANS("Recreation failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Classifying sound & extracting notes..."),
        [path, mixPath] {
            ipc::HttpServiceBridge bridge;
            // AUDIT-016: extract pitched notes on the mix's tuning grid, so a
            // 432 Hz track's vibrato doesn't split at 440-grid note boundaries.
            const auto referenceHz = MixTuningReference::forJob(bridge, mixPath);
            return bridge.analyzeSoundRecreation(path.toStdString(), "auto", referenceHz);
        },
        [](RecreateSoundPanel& self, const juce::var& v) -> juce::String {
            self.category_ = v[af::SoundRecreationResponse::category].toString();

            // MIDI summary line (pitched line vs drum hits).
            const juce::var ms = v[af::SoundRecreationResponse::midi_summary];
            juce::String midi;
            const juce::String kind = ms[af::SoundRecreationMidiSummary::kind].toString();
            if (kind == "pitched") {
                const int nn = (int)ms[af::SoundRecreationMidiSummary::n_notes];
                const int lo = (int)ms[af::SoundRecreationMidiSummary::pitch_min_midi];
                const int hi = (int)ms[af::SoundRecreationMidiSummary::pitch_max_midi];
                midi = juce::String(nn) + " notes, range MIDI " + juce::String(lo) + "-" + juce::String(hi);
                if ((bool)ms[af::SoundRecreationMidiSummary::has_glide]) midi += ", glide";
                // A monophonic pyin fallback can't represent chords (AUDIT-052).
                if ((bool)ms[af::SoundRecreationMidiSummary::low_quality_extraction] ||
                    ms[af::SoundRecreationMidiSummary::extraction_backend].toString() == "librosa_pyin_monophonic")
                    midi += TRANS(" (monophonic estimate - chords lost)");
            } else if (kind == "drums") {
                midi = juce::String((int)ms[af::SoundRecreationMidiSummary::n_hits]) + " drum hits";
            } else {
                midi = "no clear notes";
            }
            self.midiSummary_ = midi;

            // A couple of headline timbre features.
            const juce::var f = v[af::SoundRecreationResponse::features];
            if (f.isObject()) {
                const int cen = (int)(double)f[af::SoundRecreationFeatures::centroid_hz];
                const double flat = (double)f[af::SoundRecreationFeatures::flatness];
                self.timbreSummary_ = "centroid " + juce::String(cen) + " Hz, flatness " + juce::String(flat, 2);
            }

            self.recipes_.clear();
            if (auto* arr = v[af::SoundRecreationResponse::recommendations].getArray())
                for (const auto& r : *arr) {
                    Recipe rec;
                    rec.plugin = r[af::SoundRecreationRecommendation::plugin].toString();
                    rec.patch = r[af::SoundRecreationRecommendation::patch].toString();
                    rec.chain = r[af::SoundRecreationRecommendation::chain].toString();
                    rec.why = r[af::SoundRecreationRecommendation::why].toString();
                    self.recipes_.push_back(std::move(rec));
                }
            self.midiVarButton_.setEnabled(self.midiSummary_ != "no clear notes");
            return TRANS("Classified as \"") + self.category_ + "\" - " + juce::String((int)self.recipes_.size()) +
                   " recipe(s).";
        },
        std::move(options));
}

void RecreateSoundPanel::runMidiVariation()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) {
        run_.label().setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    run_.label().setText(TRANS("Applying variation to MIDI..."), juce::dontSendNotification);
    midiVarButton_.setEnabled(false);

    float timingVarSec = (float)timingVarSlider_.getValue() / 1000.0f; // ms to seconds
    float velocityVar = (float)velocityVarSlider_.getValue();
    float pitchVarProb = (float)pitchVarSlider_.getValue() / 100.0f; // percent to ratio

    juce::Component::SafePointer<RecreateSoundPanel> self(this);
    controller_.runInBackground([self, &work = controller_, path, timingVarSec, velocityVar, pitchVarProb]() {
        ipc::HttpServiceBridge bridge;
        const auto referenceHz = MixTuningReference::forJob(bridge, path);
        juce::DynamicObject::Ptr body = new juce::DynamicObject();
        if (referenceHz.has_value()) body->setProperty("reference_hz", *referenceHz);
        body->setProperty("file_path", path);
        body->setProperty("timing_variance", timingVarSec);
        body->setProperty("velocity_variance", velocityVar);
        body->setProperty("pitch_variation_prob", pitchVarProb);
        body->setProperty("stem_type", "auto");

        auto result = bridge.postAnalyzeJson("/analyze/midi_variation", juce::var(body.get()));
        postIfCurrent(self, work, [result](RecreateSoundPanel& panel) {
            if (result.is_err()) {
                panel.run_.label().setText(TRANS("MIDI variation failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            juce::File midFile(v[af::MidiVariationResponse::output_path].toString());
            juce::String status = TRANS("Saved varied MIDI -> ") + midFile.getFileName();
            juce::StringArray context; // tempo + key the MIDI was written at
            if ((double)v[af::MidiVariationResponse::bpm] > 0.0) context.add(juce::String((double)v[af::MidiVariationResponse::bpm], 1) + " BPM");
            if (v[af::MidiVariationResponse::key].toString().isNotEmpty()) context.add(v[af::MidiVariationResponse::key].toString());
            if (!context.isEmpty()) status << "  (" << context.joinIntoString(", ") << ")";
            // AUDIT-052: the pyin fallback is a monophonic reduction - say so
            // instead of presenting it as a faithful transcription.
            const bool lowQuality = (bool)v[af::MidiVariationResponse::low_quality_extraction];
            if (lowQuality) status << "  -  " << TRANS("monophonic estimate: chords and overlapping notes were lost");
            panel.run_.label().setText(status, juce::dontSendNotification);
            panel.run_.label().setColour(juce::Label::textColourId,
                                         lowQuality ? KhensuTheme::accentAmber : KhensuTheme::textTertiary);
        }, [](RecreateSoundPanel& panel) {
            panel.midiVarButton_.setEnabled(panel.midiSummary_ != "no clear notes" && panel.run_.showsResult());
        });
    });
}

void RecreateSoundPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No recreation yet"),
                           TRANS("Load a stem and press Recreate Sound to get plugin recipes."), 52);
        return;
    }
    auto b = bodyArea();
    b.removeFromTop(52); // controls rows

    // Category badge + timbre/MIDI summary.
    auto head = b.removeFromTop(20);
    const juce::String cat = category_.toUpperCase();
    g.setFont(KhensuTheme::labelFont(type::caption));
    const int badgeW = juce::jmax(56, juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), cat) + 16);
    auto badge = head.removeFromLeft(badgeW).reduced(0, 1);
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.18f));
    g.fillRoundedRectangle(badge.toFloat(), 4.0f);
    g.setColour(KhensuTheme::accentTeal);
    g.drawText(cat, badge, juce::Justification::centred, false);
    head.removeFromLeft(8);
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::monoFont(type::caption));
    g.drawText(midiSummary_ + (timbreSummary_.isNotEmpty() ? "   -   " + timbreSummary_ : juce::String()), head,
               juce::Justification::centredLeft, false);
    b.removeFromTop(6);

    // Recipe cards.
    for (size_t i = 0; i < recipes_.size() && b.getHeight() > 40; ++i) {
        const auto& r = recipes_[i];
        // Estimate card height from wrapped chain text.
        g.setFont(KhensuTheme::bodyFont(type::caption));
        const int chainLines = juce::jmax(1, (juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), r.chain) /
                                              juce::jmax(1, b.getWidth() - 12)) +
                                                 1);
        const int cardH = juce::jmin(b.getHeight(), 34 + chainLines * 13);
        auto card = b.removeFromTop(cardH);
        g.setColour(KhensuTheme::bgVoid.withAlpha(0.35f));
        g.fillRoundedRectangle(card.toFloat(), 4.0f);
        auto inner = card.reduced(6, 4);

        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(r.plugin, inner.removeFromTop(14), juce::Justification::centredLeft, true);

        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::monoFont(type::caption));
        g.drawText(r.why, inner.removeFromTop(12), juce::Justification::centredLeft, true);

        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont(type::caption));
        g.drawFittedText("Chain: " + r.chain, inner, juce::Justification::topLeft, chainLines, 1.0f);

        b.removeFromTop(6);
    }
}

void RecreateSoundPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(120));
    controls.removeFromLeft(6);
    midiVarButton_.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);

    auto slidersRow = b.removeFromTop(24);
    int colW = slidersRow.getWidth() / 3;

    auto c1 = slidersRow.removeFromLeft(colW);
    timingVarLabel_.setBounds(c1.removeFromLeft(70));
    timingVarSlider_.setBounds(c1.reduced(0, 2));

    auto c2 = slidersRow.removeFromLeft(colW);
    velocityVarLabel_.setBounds(c2.removeFromLeft(55));
    velocityVarSlider_.setBounds(c2.reduced(0, 2));

    auto c3 = slidersRow;
    pitchVarLabel_.setBounds(c3.removeFromLeft(60));
    pitchVarSlider_.setBounds(c3.reduced(0, 2));

    auto errArea = bodyArea();
    errArea.removeFromTop(52); // account for the two control rows
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
