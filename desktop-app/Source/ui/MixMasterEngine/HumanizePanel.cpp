// desktop-app/Source/ui/MixMasterEngine/HumanizePanel.cpp
#include "HumanizePanel.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include "../../RenderJobManager.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RenderPaths.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include "../../PythonServiceManager.h"

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this panel, in one place (UI-01). Note this
// also walks stemLanes_: those controls are created on demand per stem, so a
// theme switch after the lanes exist has to reach them too - re-applying only
// the fixed widgets would leave every per-stem slider on the old palette.
void HumanizePanel::applyThemeColours()
{
    intensity_.setColour(juce::Slider::trackColourId, KhensuTheme::accentTeal.withAlpha(0.5f));
    for (auto* s : {&wowRateSlider_, &depthSlider_, &levelVarSlider_, &noiseFloorSlider_})
        s->setColour(juce::Slider::trackColourId, KhensuTheme::accentTeal.withAlpha(0.4f));
    for (auto* l : {&intensityCaption_, &wowRateLabel_, &depthLabel_, &levelVarLabel_, &noiseFloorLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);

    humanizeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    humanizeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    analogFeelButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.3f));
    analogFeelButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    for (auto* t : {&tapeDriftToggle_, &faderRideToggle_, &roomToneToggle_}) {
        t->setColour(juce::ToggleButton::textColourId, KhensuTheme::textSecondary);
        t->setColour(juce::ToggleButton::tickColourId, KhensuTheme::accentTeal);
    }
    deepButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentBlue.withAlpha(0.2f));
    deepButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentBlue);
    cleanMetaButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgRaised);
    cleanMetaButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    reportLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);

    for (auto& lane : stemLanes_) {
        if (lane.intensity)
            lane.intensity->setColour(juce::Slider::trackColourId, KhensuTheme::accentBlue.withAlpha(0.5f));
        if (lane.customToggle) {
            lane.customToggle->setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
            lane.customToggle->setColour(juce::TextButton::textColourOffId, KhensuTheme::textTertiary);
            lane.customToggle->setColour(juce::TextButton::textColourOnId, KhensuTheme::accentBlue);
        }
        for (auto* s : {&lane.wowRate, &lane.depth, &lane.levelVar, &lane.noiseFloor})
            if (*s) (*s)->setColour(juce::Slider::trackColourId, KhensuTheme::accentBlue.withAlpha(0.5f));
    }
}

HumanizePanel::HumanizePanel(IAppController& c) : PanelCard(c, "Analog Feel")
{
    applyThemeColours();
    setExplainer(TRANS("Analog Feel"), TRANS("A creative effect: adds the character of a played, taped and "
                             "hand-ridden performance to rigid programmed or in-the-box material.\n\n"
                             "- Tape drift: tape-style wow/flutter (micro-timing AND micro-pitch "
                             "drift from one mechanism, like a real tape path)\n"
                             "- Fader ride: a slow random level ride, as if a hand were on the fader\n"
                             "- Room tone: a breathing low-level floor instead of digital-black silence\n\n"
                             "Amount: ~0.25 = Slight, ~0.5 = Moderate, ~0.85 = Aggressive. Analog Feel "
                             "renders the characters you tick in one pass (tape drift per stem when "
                             "stems exist). Custom Render uses the shaping sliders below; Per-Stem "
                             "Render gives each stem its own amount, and a stem's \"Custom\" toggle "
                             "its own groove. Every render is a new file loaded as the loudness-matched "
                             "A/B \"B\" side - judge it by ear.\n\n"
                             "This is a sound-design tool. It does not change where the audio came "
                             "from: if AI tools were part of the work, disclose it. Clean Metadata "
                             "writes a _clean copy without workflow / personal tags (comments, "
                             "encoder and software tags, private app data) and always keeps the "
                             "title, artist, ISRC, copyright, artwork and every AI-provenance or "
                             "disclosure tag."));

    intensity_.setRange(0.0, 1.0, 0.01);
    intensity_.setValue(0.5);
    intensity_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
    intensity_.onValueChange = [this] {
        const double v = intensity_.getValue();
        intensityCaption_.setText(v < 0.375   ? "Slight"
                                  : v < 0.675 ? "Moderate"
                                              : "Aggressive",
                                  juce::dontSendNotification);
    };
    addAndMakeVisible(intensity_);

    intensityCaption_.setFont(KhensuTheme::labelFont(type::caption));
    intensityCaption_.setText(TRANS("Moderate"), juce::dontSendNotification);
    addAndMakeVisible(intensityCaption_);

    // Wow Rate Setup
    wowRateSlider_.setRange(0.05, 2.0, 0.01);
    wowRateSlider_.setValue(0.3);
    wowRateSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 16);
    addAndMakeVisible(wowRateSlider_);
    wowRateLabel_.setText(TRANS("Wow Rate:"), juce::dontSendNotification);
    wowRateLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(wowRateLabel_);

    // Depth Setup
    depthSlider_.setRange(0.0, 12.0, 0.1);
    depthSlider_.setValue(4.0);
    depthSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 16);
    addAndMakeVisible(depthSlider_);
    depthLabel_.setText(TRANS("Wow Depth:"), juce::dontSendNotification);
    depthLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(depthLabel_);

    // Level Var Setup
    levelVarSlider_.setRange(0.0, 3.0, 0.1);
    levelVarSlider_.setValue(0.8);
    levelVarSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 16);
    addAndMakeVisible(levelVarSlider_);
    levelVarLabel_.setText(TRANS("Level Var:"), juce::dontSendNotification);
    levelVarLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(levelVarLabel_);

    // Noise Floor Setup
    noiseFloorSlider_.setRange(-90.0, -50.0, 1.0);
    noiseFloorSlider_.setValue(-72.0);
    noiseFloorSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 16);
    addAndMakeVisible(noiseFloorSlider_);
    noiseFloorLabel_.setText(TRANS("Noise Floor:"), juce::dontSendNotification);
    noiseFloorLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(noiseFloorLabel_);

    humanizeButton_.onClick = [this] { runHumanize(); };
    addAndMakeVisible(humanizeButton_);

    humanizeButton_.setTooltip(TRANS("Render the mix with the shaping sliders below (wow rate, depth, "
                                     "level variation, noise floor)."));

    // Analog Feel (2026-11-07 reframe of the closed-loop "targeted humanize"):
    // one pass, the characters the user ticks, at the chosen amount.
    analogFeelButton_.setTooltip(TRANS("Render the ticked characters at the chosen amount in one pass - "
                                       "tape drift runs per stem when stems exist. Loaded as the "
                                       "loudness-matched A/B \"B\" side."));
    analogFeelButton_.onClick = [this] { runAnalogFeel(); };
    addAndMakeVisible(analogFeelButton_);
    tapeDriftToggle_.setTooltip(TRANS("Tape-style wow/flutter: micro-timing and micro-pitch drift."));
    faderRideToggle_.setTooltip(TRANS("A slow random level ride, like a hand on the fader."));
    roomToneToggle_.setTooltip(TRANS("A breathing low-level room tone instead of digital-black silence."));
    for (auto* t : {&tapeDriftToggle_, &faderRideToggle_, &roomToneToggle_}) {
        t->setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(*t);
    }

    deepButton_.setTooltip(TRANS("Render each separated stem at its own amount, then re-sum "
                           "to a new mix. Requires stems (auto-separated at intake) or drag-dropped external stems."));
    deepButton_.onClick = [this] { runDeepHumanize(); };
    addAndMakeVisible(deepButton_);

    cleanMetaButton_.setTooltip(TRANS("Write a _clean copy without workflow / personal tags. Title, artist, "
                                      "ISRC, copyright, artwork and every AI-provenance or disclosure tag are kept."));
    cleanMetaButton_.onClick = [this] { runCleanMetadata(); };
    addAndMakeVisible(cleanMetaButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    statusLabel_.setText(
        TRANS("Renders a copy with analog character and A/Bs it against the original. Drag & drop external stems to sum them."),
        juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    reportLabel_.setFont(KhensuTheme::labelFont(type::caption));
    reportLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(reportLabel_);
}

void HumanizePanel::rebuildStemLanes()
{
    // One intensity slider per available Demucs stem. Built lazily once stems
    // exist so Deep Humanize can drive each stem independently.
    if (stemLanesBuilt_ || !controller_.stems().hasStemsAvailable()) return;
    stemLanes_.clear();
    struct Def {
        const char* id;
        const char* label;
    };
    for (const auto& d : {Def{"vocals", "Vocals"}, Def{"drums", "Drums"}, Def{"bass", "Bass"}, Def{"other", "Other"}}) {
        if (!controller_.getStemFile(d.id).existsAsFile()) continue;
        StemLane lane;
        lane.id = d.id;
        lane.label = d.label;
        lane.intensity = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        lane.intensity->setRange(0.0, 1.0, 0.01);
        lane.intensity->setValue(0.5);
        lane.intensity->setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 16);
        lane.intensity->setColour(juce::Slider::trackColourId, KhensuTheme::accentBlue.withAlpha(0.5f));
        addAndMakeVisible(*lane.intensity);
        stemLanes_.push_back(std::move(lane));
        buildCustomGrooveControls(stemLanes_.back());
    }
    stemLanesBuilt_ = true;
    deepButton_.setEnabled(!stemLanes_.empty());
    resized();
}

void HumanizePanel::buildCustomGrooveControls(StemLane& lane)
{
    // Index is stable once assigned: rebuildStemLanes() only ever runs once
    // (guarded by stemLanesBuilt_) and filesDropped() only appends, so no
    // earlier lane's index ever changes after this closure captures it.
    const int index = (int)(&lane - stemLanes_.data());

    lane.customToggle = std::make_unique<juce::TextButton>("Custom");
    lane.customToggle->setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    lane.customToggle->setColour(juce::TextButton::textColourOffId, KhensuTheme::textTertiary);
    lane.customToggle->setColour(juce::TextButton::textColourOnId, KhensuTheme::accentBlue);
    lane.customToggle->setClickingTogglesState(true);
    lane.customToggle->setTooltip(TRANS("Give this stem its own wow/depth/level-variation/noise-floor "
                                  "groove instead of sharing the shaping sliders below."));
    lane.customToggle->onClick = [this, index] { setExpandedLane(index); };
    addAndMakeVisible(*lane.customToggle);

    auto makeSlider = [this](double lo, double hi, double interval, double def, const juce::String& tip) {
        auto s = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
        s->setRange(lo, hi, interval);
        s->setValue(def);
        s->setTextBoxStyle(juce::Slider::TextBoxRight, false, 38, 14);
        s->setColour(juce::Slider::trackColourId, KhensuTheme::accentBlue.withAlpha(0.5f));
        s->setTooltip(tip);
        addChildComponent(*s); // hidden until this lane is expanded
        return s;
    };
    // Seed each per-stem groove control from the current global shaping
    // sliders, so switching a stem to "Custom" starts from what it already
    // sounded like rather than jumping to an unrelated default.
    lane.wowRate = makeSlider(0.05, 2.0, 0.01, wowRateSlider_.getValue(), "Wow rate (Hz)");
    lane.depth = makeSlider(0.0, 12.0, 0.1, depthSlider_.getValue(), "Wow depth (ms)");
    lane.levelVar = makeSlider(0.0, 3.0, 0.1, levelVarSlider_.getValue(), "Level variation (dB)");
    lane.noiseFloor = makeSlider(-90.0, -50.0, 1.0, noiseFloorSlider_.getValue(), "Noise floor (dB)");
}

void HumanizePanel::setExpandedLane(int laneIndex)
{
    if (laneIndex < 0 || laneIndex >= (int)stemLanes_.size()) return;

    // Accordion: expanding one lane's custom-groove row collapses every
    // other lane's, bounding how much extra height the fixed-size card needs
    // regardless of how many stems are present.
    const bool wasExpanded = stemLanes_[(size_t)laneIndex].customGroove;
    for (auto& lane : stemLanes_)
        lane.customGroove = false;
    stemLanes_[(size_t)laneIndex].customGroove = !wasExpanded;

    for (auto& lane : stemLanes_) {
        if (lane.customToggle) lane.customToggle->setToggleState(lane.customGroove, juce::dontSendNotification);
        const bool show = lane.customGroove;
        if (lane.wowRate) lane.wowRate->setVisible(show);
        if (lane.depth) lane.depth->setVisible(show);
        if (lane.levelVar) lane.levelVar->setVisible(show);
        if (lane.noiseFloor) lane.noiseFloor->setVisible(show);
    }
    resized();
    repaint();
}

void HumanizePanel::refreshForSession()
{
    // Stems may have arrived since construction - build the per-stem lanes.
    rebuildStemLanes();
    if (controller_.activeSourcePath().isEmpty()) return;
    peaks_ = controller_.playback().getWaveformPeaks();
    repaint();
}

void HumanizePanel::runHumanize()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty() || renderInFlight_) {
        if (s == nullptr || s->files.empty()) statusLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    const juce::String inPath(s->files[0].path);
    const juce::File inFile(inPath);
    // Unique per run: a fixed "_humanized.wav" was overwritten while the
    // transport could still have it open as B (AUDIT-018).
    const juce::String outPath = uniqueRenderFile(inFile, "_humanized").getFullPathName();
    const float intensity = (float)intensity_.getValue();

    renderInFlight_ = true;
    humanizeButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Rendering humanized copy..."), juce::dontSendNotification);

    IRenders::ChainRender render;
    render.description = "Humanize (" + intensityCaption_.getText().toLowerCase() + ")";
    render.sourceRecommendationId = "humanize";
    render.chain.push_back({"Humanize",
                     {{"intensity", intensity},
                      {"wow_rate_hz", (float)wowRateSlider_.getValue()},
                      {"depth_ms", (float)depthSlider_.getValue()},
                      {"level_var_db", (float)levelVarSlider_.getValue()},
                      {"noise_floor_db", (float)noiseFloorSlider_.getValue()}}});

    juce::Component::SafePointer<HumanizePanel> self(this);
    controller_.renders().renderPreview(
        render, inPath, outPath,
        [self, outPath](IRenders::RenderOutcome outcome, const juce::String& pathOrError, const juce::var&) {
            if (self == nullptr) return;
            self->renderInFlight_ = false;
            self->humanizeButton_.setEnabled(true);
            switch (outcome) {
            case IRenders::RenderOutcome::Rendered: break;
            case IRenders::RenderOutcome::Failed:
                self->statusLabel_.setText(TRANS("Humanize failed: ") + pathOrError, juce::dontSendNotification);
                return;
            case IRenders::RenderOutcome::Cancelled:
                self->statusLabel_.setText(TRANS("Humanize cancelled."), juce::dontSendNotification);
                return;
            case IRenders::RenderOutcome::SessionChanged: return; // never song A's copy in song B's A/B (AUDIT-002)
            }
            // Non-destructive: load as the loudness-matched A/B "B" side
            // (also fires the auto X-Ray). Judged by ear - no detector rescan.
            self->controller_.setAbProcessedPath(outPath);
            self->statusLabel_.setText(TRANS("Rendered - A/B loaded (loudness-matched). Judge it by ear."),
                                       juce::dontSendNotification);
        });
}

void HumanizePanel::runAnalogFeel()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty() || renderInFlight_) {
        if (s == nullptr || s->files.empty()) statusLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    ipc::HttpServiceBridge::AnalogFeelCharacter character;
    character.tapeDrift = tapeDriftToggle_.getToggleState();
    character.faderRide = faderRideToggle_.getToggleState();
    character.roomTone = roomToneToggle_.getToggleState();
    const float amount = (float)intensity_.getValue();
    if (amount <= 0.0f || !(character.tapeDrift || character.faderRide || character.roomTone)) {
        statusLabel_.setText(TRANS("Tick at least one character and set an amount above 0."),
                             juce::dontSendNotification);
        return;
    }
    const juce::String inPath(s->files[0].path);
    // Must sit beside the source (the endpoint refuses an output elsewhere);
    // unique per run so an open B file is never overwritten (AUDIT-018).
    const juce::String outPath = uniqueRenderFile(juce::File(inPath), "_analog").getFullPathName();

    // Tape drift runs per stem when stems exist (no cross-stem smearing).
    std::map<std::string, std::string> stems;
    if (controller_.stems().hasStemsAvailable()) {
        for (const char* name : {"drums", "vocals", "bass", "other"}) {
            const auto f = controller_.getStemFile(name);
            if (f.existsAsFile()) stems[name] = f.getFullPathName().toStdString();
        }
    }

    renderInFlight_ = true;
    analogFeelButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Rendering analog feel..."), juce::dontSendNotification);

    juce::Component::SafePointer<HumanizePanel> self(this);
    controller_.runLongInBackground([self, &work = controller_, inPath, outPath, stems, amount, character]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analogFeel(inPath.toStdString(), outPath.toStdString(), amount, character, stems);
        postIfCurrent(self, work, [result](HumanizePanel& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Analog feel failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            juce::StringArray applied;
            if (auto* arr = v[af::AnalogFeelResponse::applied].getArray())
                for (const auto& m : *arr) applied.add(m.toString().replace("_", " "));
            const bool perStem = (bool)v[af::AnalogFeelResponse::stem_aware];
            // Non-destructive: load the render as the loudness-matched A/B "B" side.
            panel.controller_.setAbProcessedPath(v[af::AnalogFeelResponse::output_path].toString());
            panel.statusLabel_.setText(TRANS("Analog feel") + " (" + applied.joinIntoString(", ")
                                           + (perStem ? juce::String(", per stem") : juce::String()) + ") - "
                                           + TRANS("A/B loaded. Judge it by ear."),
                                       juce::dontSendNotification);
        }, [](HumanizePanel& panel) {
            panel.renderInFlight_ = false;
            panel.analogFeelButton_.setEnabled(true);
        });
    });
}

void HumanizePanel::runDeepHumanize()
{
    if (stemLanes_.empty()) {
        statusLabel_.setText(TRANS("Per-stem render needs stems. Use auto-separated stems or drag & drop external files."),
                             juce::dontSendNotification);
        return;
    }
    if (renderInFlight_) return;

    std::map<std::string, std::string> stems;
    std::map<std::string, float> intensities;
    std::map<std::string, float> wowRateOverrides, depthOverrides, levelVarOverrides, noiseFloorOverrides;
    for (const auto& lane : stemLanes_) {
        const auto f = lane.externalFile.existsAsFile() ? lane.externalFile : controller_.getStemFile(lane.id);
        if (!f.existsAsFile()) continue;
        const std::string id = lane.id.toStdString();
        stems[id] = f.getFullPathName().toStdString();
        intensities[id] = (float)lane.intensity->getValue();
        if (lane.customGroove && lane.wowRate) {
            wowRateOverrides[id] = (float)lane.wowRate->getValue();
            depthOverrides[id] = (float)lane.depth->getValue();
            levelVarOverrides[id] = (float)lane.levelVar->getValue();
            noiseFloorOverrides[id] = (float)lane.noiseFloor->getValue();
        }
    }
    if (stems.empty()) {
        statusLabel_.setText(TRANS("No valid stems found to render."), juce::dontSendNotification);
        return;
    }

    renderInFlight_ = true;
    deepButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Rendering stems: ") + juce::String((int)stems.size()) + "...",
                         juce::dontSendNotification);

    float wow_rate = (float)wowRateSlider_.getValue();
    float depth = (float)depthSlider_.getValue();
    float level_var = (float)levelVarSlider_.getValue();
    float noise_floor = (float)noiseFloorSlider_.getValue();
    // A fresh file beside the first stem (the service requires that folder)
    // - its own default name would overwrite the previous run's A/B "B".
    const std::string outPath =
        uniqueRenderFile(juce::File(stems.begin()->second), "_deep_humanized").getFullPathName().toStdString();

    juce::Component::SafePointer<HumanizePanel> self(this);
    controller_.runLongInBackground([self, &work = controller_, stems, intensities, wow_rate, depth, level_var, noise_floor, wowRateOverrides,
                                 depthOverrides, levelVarOverrides, noiseFloorOverrides, outPath]() {
        // Longer timeout - humanizing several stems + summing takes a while.
        ipc::HttpServiceBridge bridge(PythonServiceManager::kBaseUrl, 180000, 2000);
        auto result = bridge.deepHumanize(stems, intensities, 0.5f, wow_rate, depth, level_var, noise_floor,
                                          wowRateOverrides, depthOverrides, levelVarOverrides, noiseFloorOverrides,
                                          outPath);
        postIfCurrent(self, work, [result](HumanizePanel& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Per-stem render failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            const juce::String outPath = v[af::DeepHumanizeResponse::output_path].toString();
            // Non-destructive: load the re-summed mix as the A/B "B" side
            // (fires auto X-Ray + What-Changed + verification rescan).
            panel.controller_.setAbProcessedPath(outPath);

            // Applied-changes report (spec §5.2).
            juce::StringArray lines;
            if (auto* arr = v[af::DeepHumanizeResponse::report].getArray()) {
                for (const auto& r : *arr)
                    lines.add(r[af::DeepHumanizeStemReport::stem].toString() + " @ " +
                              juce::String((int)std::round((double)r[af::DeepHumanizeStemReport::intensity] * 100.0)) + "%");
            }
            panel.reportLabel_.setText(TRANS("Per-stem render applied - ") + lines.joinIntoString(",  "),
                                       juce::dontSendNotification);
            panel.statusLabel_.setText(TRANS("Per-stem render - re-summed mix loaded as A/B \"B\" (loudness-matched)."),
                                       juce::dontSendNotification);
        }, [](HumanizePanel& panel) {
            panel.renderInFlight_ = false;
            panel.deepButton_.setEnabled(true);
        });
    });
}

void HumanizePanel::runCleanMetadata()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) {
        statusLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    const juce::String target(s->files[0].path);
    cleanMetaButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Cleaning delivery metadata..."), juce::dontSendNotification);

    juce::Component::SafePointer<HumanizePanel> self(this);
    controller_.runInBackground([self, &work = controller_, target]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.cleanDeliveryMetadata(target.toStdString());
        postIfCurrent(self, work, [result](HumanizePanel& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Clean metadata failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            const int removed = (int)v[af::CleanMetadataResponse::removed_tag_count];
            const auto* prov = v[af::CleanMetadataResponse::kept_provenance_tags].getArray();
            juce::String text = TRANS("Removed") + " " + juce::String(removed) + " " + TRANS("workflow tag(s)");
            if (prov != nullptr && !prov->isEmpty())
                text << "; " << TRANS("kept") << " " << prov->size() << " " << TRANS("provenance / disclosure tag(s)");
            text << " -> " << juce::File(v[af::CleanMetadataResponse::output_path].toString()).getFileName();
            panel.statusLabel_.setText(text, juce::dontSendNotification);
        }, [](HumanizePanel& panel) { panel.cleanMetaButton_.setEnabled(true); });
    });
}

bool HumanizePanel::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files) {
        auto ext = juce::File(f).getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".mp3" || ext == ".m4a" ||
            ext == ".ogg")
            return true;
    }
    return false;
}

void HumanizePanel::fileDragEnter(const juce::StringArray&, int, int)
{
    dragOver_ = true;
    repaint();
}

void HumanizePanel::fileDragExit(const juce::StringArray&)
{
    dragOver_ = false;
    repaint();
}

void HumanizePanel::filesDropped(const juce::StringArray& files, int, int)
{
    dragOver_ = false;
    for (const auto& file : files) {
        juce::File f(file);
        auto ext = f.getFileExtension().toLowerCase();
        if (ext == ".wav" || ext == ".aiff" || ext == ".aif" || ext == ".flac" || ext == ".mp3" || ext == ".m4a" ||
            ext == ".ogg") {
            StemLane lane;
            lane.id = "external_" + juce::String(f.getFileNameWithoutExtension().hashCode());
            lane.label = f.getFileNameWithoutExtension();
            lane.externalFile = f;
            lane.intensity = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            lane.intensity->setRange(0.0, 1.0, 0.01);
            lane.intensity->setValue(0.5);
            lane.intensity->setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 16);
            lane.intensity->setColour(juce::Slider::trackColourId, KhensuTheme::accentBlue.withAlpha(0.5f));
            addAndMakeVisible(*lane.intensity);
            stemLanes_.push_back(std::move(lane));
        }
    }
    deepButton_.setEnabled(!stemLanes_.empty());
    resized();
    repaint();
}

void HumanizePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);

    // Per-stem lane labels (sliders are child components, positioned in
    // resized()); drawn to the left of each slider.
    g.setFont(KhensuTheme::labelFont(type::caption));
    for (const auto& lane : stemLanes_) {
        if (lane.intensity == nullptr) continue;
        const auto sb = lane.intensity->getBounds();
        g.setColour(KhensuTheme::accentBlue.withAlpha(0.9f));
        g.drawText(lane.label, sb.getX() - 58, sb.getY(), 54, sb.getHeight(), juce::Justification::centredLeft, false);
    }

    if (laneArea_.isEmpty()) return;

    // ── Waveform lane ────────────────────────────────────────────────────
    g.setColour(KhensuTheme::bgSurface.brighter(0.04f));
    g.fillRoundedRectangle(laneArea_.toFloat(), 4.0f);

    if (!peaks_.empty()) {
        const float midY = (float)laneArea_.getCentreY();
        const float halfH = laneArea_.getHeight() * 0.46f;
        const float dx = (float)laneArea_.getWidth() / (float)peaks_.size();
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.45f));
        for (size_t i = 0; i < peaks_.size(); ++i) {
            const float px = laneArea_.getX() + dx * (float)i;
            const float ph = juce::jlimit(0.02f, 1.0f, peaks_[i]) * halfH;
            g.drawVerticalLine(juce::roundToInt(px), midY - ph, midY + ph);
        }
    } else {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText("Load a session to see the waveform lane.", laneArea_, juce::Justification::centred, false);
    }

    if (dragOver_) {
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.4f));
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(2.0f), 5.0f, 2.0f);
    }
}

void HumanizePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    auto controls = b.removeFromTop(26);
    analogFeelButton_.setBounds(controls.removeFromLeft(110));
    controls.removeFromLeft(6);
    humanizeButton_.setBounds(controls.removeFromLeft(110));
    controls.removeFromLeft(6);
    deepButton_.setBounds(controls.removeFromLeft(120));
    controls.removeFromLeft(6);
    cleanMetaButton_.setBounds(controls.removeFromLeft(120));
    controls.removeFromLeft(10);
    intensityCaption_.setBounds(controls.removeFromRight(64));
    intensity_.setBounds(controls);
    b.removeFromTop(2);

    auto toggles = b.removeFromTop(20);
    tapeDriftToggle_.setBounds(toggles.removeFromLeft(100));
    faderRideToggle_.setBounds(toggles.removeFromLeft(100));
    roomToneToggle_.setBounds(toggles.removeFromLeft(100));
    b.removeFromTop(2);

    statusLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(2);

    // Lay out shaping sliders at the bottom
    auto shapeRow = b.removeFromBottom(24);
    int colW = shapeRow.getWidth() / 4;

    auto c1 = shapeRow.removeFromLeft(colW);
    wowRateLabel_.setBounds(c1.removeFromLeft(60));
    wowRateSlider_.setBounds(c1.reduced(0, 2));

    auto c2 = shapeRow.removeFromLeft(colW);
    depthLabel_.setBounds(c2.removeFromLeft(65));
    depthSlider_.setBounds(c2.reduced(0, 2));

    auto c3 = shapeRow.removeFromLeft(colW);
    levelVarLabel_.setBounds(c3.removeFromLeft(60));
    levelVarSlider_.setBounds(c3.reduced(0, 2));

    auto c4 = shapeRow;
    noiseFloorLabel_.setBounds(c4.removeFromLeft(75));
    noiseFloorSlider_.setBounds(c4.reduced(0, 2));

    b.removeFromBottom(4);
    reportLabel_.setBounds(b.removeFromBottom(14));

    // Per-stem lanes (Deep Humanize): a label + intensity slider + Custom
    // toggle each. An expanded lane (accordion - only one at a time) gets an
    // extra sub-row for its own wow/depth/level-var/noise-floor sliders.
    for (auto& lane : stemLanes_) {
        auto row = b.removeFromTop(lane.customGroove ? 18 + 20 : 18);
        auto topRow = row.removeFromTop(18);
        topRow.removeFromLeft(4);
        // Label drawn in paint(); reserve the left gutter here.
        topRow.removeFromLeft(58);
        if (lane.customToggle) {
            lane.customToggle->setBounds(topRow.removeFromRight(50).reduced(1, 1));
            topRow.removeFromRight(4);
        }
        lane.intensity->setBounds(topRow.reduced(2, 1));

        if (lane.customGroove && lane.wowRate) {
            auto sub = row.reduced(2, 1);
            const int quarterW = sub.getWidth() / 4;
            lane.wowRate->setBounds(sub.removeFromLeft(quarterW).reduced(2, 0));
            lane.depth->setBounds(sub.removeFromLeft(quarterW).reduced(2, 0));
            lane.levelVar->setBounds(sub.removeFromLeft(quarterW).reduced(2, 0));
            lane.noiseFloor->setBounds(sub.reduced(2, 0));
        }
    }
    if (!stemLanes_.empty()) b.removeFromTop(2);

    laneArea_ = b;
}

} // namespace khensu
