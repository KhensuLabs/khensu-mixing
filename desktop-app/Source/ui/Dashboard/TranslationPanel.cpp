// desktop-app/Source/ui/Dashboard/TranslationPanel.cpp
#include "TranslationPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/RecommendationExplainer.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/ServiceErrorHint.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
const juce::String kTranslationExplainerBase =
    "Risk of poor translation to each playback environment (0 = safe, 1 = problematic). "
    "Green zone is < 0.3. Values are data-driven when extended analysis is complete, "
    "otherwise heuristic estimates marked (est).";
}

// Every cached palette colour on this component, in one place (UI-01).
void TranslationPanel::applyThemeColours()
{
    reportToggle_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgRaised);
    reportToggle_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    summaryHeaderLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    summaryBodyLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    hearOriginalButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgRaised);
    hearOriginalButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    deviceNoteLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

TranslationPanel::TranslationPanel(IAppController& c) : PanelCard(c, "Translation")
{
    applyThemeColours();
    setExplainer(TRANS("Translation"), kTranslationExplainerBase, "translation_risk");

    for (auto* m : {&studioMeter_, &earbudsMeter_, &phoneMeter_, &carMeter_, &clubMeter_}) {
        m->setRange(0.0f, 1.0f);
        m->setTargetZone(0.0f, 0.3f);
        addAndMakeVisible(*m);
    }

    reportToggle_.onClick = [this]() {
        clientMode_ = !clientMode_;
        reportToggle_.setButtonText(clientMode_ ? "Engineer Details" : "Client Summary");
        updateView();
    };
    addAndMakeVisible(reportToggle_);

    summaryHeaderLabel_.setText(TRANS("Mix Health Status"), juce::dontSendNotification);
    summaryHeaderLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::body, juce::Font::bold));
    addChildComponent(summaryHeaderLabel_);

    summaryBodyLabel_.setText(TRANS("Load audio to generate a client translation summary report."),
                              juce::dontSendNotification);
    summaryBodyLabel_.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, juce::Font::plain));
    summaryBodyLabel_.setJustificationType(juce::Justification::topLeft);
    addChildComponent(summaryBodyLabel_);

    // Audition-through-device buttons.
    struct Btn {
        juce::TextButton* b;
        const char* device;
    };
    for (const auto& [btn, device] : std::initializer_list<Btn>{{&hearPhoneButton_, "phone"},
                                                                {&hearEarbudsButton_, "earbuds"},
                                                                {&hearCarButton_, "car"},
                                                                {&hearClubButton_, "club"}}) {
        juce::String dev(device);
        btn->onClick = [this, dev] { auditionDevice(dev); };
        addAndMakeVisible(*btn);
    }
    hearOriginalButton_.onClick = [this] { restoreOriginal(); };
    addAndMakeVisible(hearOriginalButton_);

    deviceNoteLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(deviceNoteLabel_);
    // VIS-8: the risk matrix reads as one image with a text summary.
    matrixSummary_.setTitle(TRANS("Environmental risk matrix"));
    matrixSummary_.describe = [this] { return matrixSummary(); };
    addAndMakeVisible(matrixSummary_);

    updateView();
}

void TranslationPanel::auditionDevice(const juce::String& device)
{
    const juce::String source = controller_.activeSourcePath();
    if (source.isEmpty()) {
        deviceNoteLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    deviceNoteLabel_.setText(TRANS("Rendering") + " " + device + " " + TRANS("simulation..."),
                             juce::dontSendNotification);

    fetchQuietly(*this, controller_, [source, device]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.renderDevicePreview(source.toStdString(), device.toStdString());
        return result;
    }, [device](TranslationPanel& self, auto&& result) {
        if (result.is_err()) {
            self.deviceNoteLabel_.setText(serviceErrorLine(result.error()), juce::dontSendNotification);
            return;
        }
        const juce::var v = result.value();
        const juce::String rendered = v[af::DevicePreviewResponse::rendered_path].toString();
        if (!juce::File(rendered).existsAsFile()) {
            self.deviceNoteLabel_.setText(TRANS("Render produced no file."), juce::dontSendNotification);
            return;
        }
        // Level-matched to the mix (DSK-14): an unmatched device preview
        // biases the comparison toward whichever is louder.
        const bool wasPlaying = self.controller_.playback().isPlaying();
        self.controller_.playback().auditionLevelMatchedPreview(juce::File(rendered));
        if (!wasPlaying) self.controller_.playback().playAudio();
        self.deviceNoteLabel_.setText(TRANS("Hearing (level-matched):") + " " + device + " - " +
                                           v[af::DevicePreviewResponse::note].toString(),
                                       juce::dontSendNotification);
    }, [](TranslationPanel& self) {
        // Reset the busy text BEFORE the stale check (DSK-14): a stale
        // result used to leave "Rendering..." on screen for good.
        self.deviceNoteLabel_.setText({}, juce::dontSendNotification);
    });
}

void TranslationPanel::restoreOriginal()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) return;
    // Back to whichever A/B side the transport strip shows (DSK-14), with
    // position and play state kept.
    controller_.playback().setAbSide(controller_.playback().isPlayingProcessed());
    deviceNoteLabel_.setText(TRANS("Back to the real mix."), juce::dontSendNotification);
}

void TranslationPanel::setData(const AnalysisResult& r)
{
    lastResult_ = r;
    hasResult_ = true;

    // Fetch baseline environment risks
    float s = 0.1f, e = 0.2f, p = 0.2f, c = 0.2f, cl = 0.15f;
    if (r.translation_risk) {
        const auto& tr = *r.translation_risk;
        auto risk = [&](const char* key) -> float {
            auto it = tr.per_environment.find(key);
            return it != tr.per_environment.end() ? it->second : 0.0f;
        };
        s = risk("studio");
        e = risk("earbuds");
        p = risk("phone");
        c = risk("car");
        cl = risk("club");

        studioMeter_.setValue(s, "Studio: " + juce::String(s, 2));
        earbudsMeter_.setValue(e, "Earbuds: " + juce::String(e, 2));
        phoneMeter_.setValue(p, "Phone: " + juce::String(p, 2));
        carMeter_.setBounds(0, 0, 0, 0); // avoid unused compiler warnings by using meters
        carMeter_.setValue(c, "Car: " + juce::String(c, 2));
        clubMeter_.setValue(cl, "Club: " + juce::String(cl, 2));
    } else {
        e = r.spectral.balance_low > 0.6f ? 0.8f : 0.2f;
        p = r.spectral.balance_low > 0.6f ? 0.8f : 0.2f;
        c = r.spectral.balance_low > 0.55f ? 0.7f : 0.2f;
        cl = r.loudness.true_peak_dbtp > -1.0f ? 0.7f : 0.15f;

        studioMeter_.setValue(s, "Studio (est): " + juce::String(s, 2));
        earbudsMeter_.setValue(e, "Earbuds (est): " + juce::String(e, 2));
        phoneMeter_.setValue(p, "Phone (est): " + juce::String(p, 2));
        carMeter_.setValue(c, "Car (est): " + juce::String(c, 2));
        clubMeter_.setValue(cl, "Club (est): " + juce::String(cl, 2));
    }

    // Deeper companion metric (2026-07-09): analyze_translation()'s actual
    // per-device frequency-response + masking-collision simulation, appended
    // to each meter's label rather than replacing the existing risk score -
    // the two measure different things (translation_risk is a hand-tuned
    // spectral/dynamics heuristic; translation_prediction is a measured
    // masking-collision delta under a simulated device response).
    if (r.translation_prediction) {
        const auto& tp = r.translation_prediction->per_profile;
        auto appendDegradation = [&](MeterBarComponent& meter, const char* key, float riskValue,
                                     const juce::String& baseLabel) {
            auto it = tp.find(key);
            if (it == tp.end()) {
                meter.setValue(riskValue, baseLabel);
                return;
            }
            juce::String label =
                baseLabel +
                juce::String::formatted("  - masking %d%%", juce::roundToInt(it->second.degradation_score * 100.0f));
            meter.setValue(riskValue, label);
        };
        appendDegradation(earbudsMeter_, "earbuds", e, "Earbuds: " + juce::String(e, 2));
        appendDegradation(phoneMeter_, "phone", p, "Phone: " + juce::String(p, 2));
        appendDegradation(carMeter_, "car", c, "Car: " + juce::String(c, 2));
        appendDegradation(clubMeter_, "club", cl, "Club: " + juce::String(cl, 2));
    }

    // Calculate dynamic frequency risk dimensions
    float balanceLows = r.spectral.balance_low;
    float lowDev = std::abs(balanceLows - 0.35f);
    float lowEndStereoPct = r.mono_compat ? (r.mono_compat->low_end_stereo_pct / 100.0f) : 0.0f;
    float lowRisk = std::clamp(lowDev * 3.5f + lowEndStereoPct * 1.5f, 0.0f, 1.0f);

    float balanceMids = r.spectral.balance_mid;
    float midDev = std::abs(balanceMids - 0.40f);
    float midRisk = std::clamp(midDev * 3.5f, 0.0f, 1.0f);

    float balanceHighs = r.spectral.balance_high;
    float highDev = std::abs(balanceHighs - 0.25f);
    float harshScore = r.harshness ? r.harshness->score : 0.0f;
    float highRisk = std::clamp(highDev * 3.5f + harshScore * 0.7f, 0.0f, 1.0f);

    // Studio (Linear)
    riskMatrix_[0][0] = std::clamp(lowRisk * 0.2f, 0.0f, 1.0f);
    riskMatrix_[0][1] = std::clamp(midRisk * 0.2f, 0.0f, 1.0f);
    riskMatrix_[0][2] = std::clamp(highRisk * 0.2f, 0.0f, 1.0f);

    // Earbuds (Lack of low end, peaky treble)
    riskMatrix_[1][0] = std::clamp(lowRisk * 0.8f + 0.1f, 0.0f, 1.0f);
    riskMatrix_[1][1] = std::clamp(midRisk * 0.5f, 0.0f, 1.0f);
    riskMatrix_[1][2] = std::clamp(highRisk * 1.1f + 0.1f, 0.0f, 1.0f);

    // Phone (No bass, midrange focused, high mono phase cancellation risk)
    riskMatrix_[2][0] = std::clamp(lowRisk * 1.8f + 0.2f, 0.0f, 1.0f);
    riskMatrix_[2][1] = std::clamp(midRisk * 0.9f, 0.0f, 1.0f);
    riskMatrix_[2][2] = std::clamp(highRisk * 0.5f, 0.0f, 1.0f);

    // Car (Scooped mids, boomy reflections)
    riskMatrix_[3][0] = std::clamp(lowRisk * 1.3f + 0.1f, 0.0f, 1.0f);
    riskMatrix_[3][1] = std::clamp(midRisk * 0.9f + 0.1f, 0.0f, 1.0f);
    riskMatrix_[3][2] = std::clamp(highRisk * 0.7f, 0.0f, 1.0f);

    // Club (Massive mono sub, loud highs)
    riskMatrix_[4][0] = std::clamp(lowRisk * 1.5f + (lowEndStereoPct > 0.3f ? 0.4f : 0.0f), 0.0f, 1.0f);
    riskMatrix_[4][1] = std::clamp(midRisk * 0.5f, 0.0f, 1.0f);
    riskMatrix_[4][2] = std::clamp(highRisk * 1.0f, 0.0f, 1.0f);

    // Formulate Client Report Text
    float avgRisk = 0.0f;
    for (int i = 0; i < kNumEnvs; ++i) {
        for (int j = 0; j < kNumBands; ++j) {
            avgRisk += riskMatrix_[i][j];
        }
    }
    avgRisk /= (kNumEnvs * kNumBands);

    juce::String header;
    juce::String body;
    if (avgRisk < 0.25f) {
        header = "Mix Ready for Release (Excellent Translation)";
        body = "Your mix has outstanding tonal balance and phase alignment. It will translate beautifully across "
               "consumer earbuds, car stereos, and large club sound systems without any further adjustment.";
    } else if (avgRisk < 0.55f) {
        header = "Moderate Translation Risk Detected";
        body = "The mix is solid overall, but has minor translation risks. ";
        if (lowRisk > 0.4f) {
            body << "The low-end balance is slightly off; it may sound boomy in cars or lose punch on phone speakers. ";
        }
        if (highRisk > 0.4f) {
            body << "The high frequencies are slightly pronounced; check for harshness on consumer earbuds. ";
        }
        body << "Consider applying the recommended corrections in the copilot panel.";
    } else {
        header = "Critical Translation Risk - Adjustments Required";
        body = "Severe translation issues detected. ";
        if (lowRisk > 0.6f) {
            body << "Critical: Low-end frequency build-up or extreme stereo width below 100Hz will cause severe phase "
                    "cancellation in clubs or distortion on mobile screens. ";
        }
        if (highRisk > 0.6f) {
            body << "Critical: Harsh resonance peaks detected in the upper mid-range will cause listener fatigue on "
                    "consumer headphones. ";
        }
        body << "It is highly recommended to run the corrective DSP chains before master distribution.";
    }

    summaryHeaderLabel_.setText(header, juce::dontSendNotification);
    summaryHeaderLabel_.setColour(juce::Label::textColourId,
                                  avgRisk < 0.25f
                                      ? KhensuTheme::accentTeal
                                      : (avgRisk < 0.55f ? KhensuTheme::accentAmber : KhensuTheme::accentRed));
    summaryBodyLabel_.setText(body, juce::dontSendNotification);

    const auto* rec = explainer::findTopRecommendation(r.recommendations, {"translation_risk_high"});
    setExplainer(TRANS("Translation"), explainer::appendReasoningTrace(kTranslationExplainerBase, rec), "translation_risk");

    repaint();
}

void TranslationPanel::updateView()
{
    const bool showMeters = !clientMode_;
    for (auto* m : {&studioMeter_, &earbudsMeter_, &phoneMeter_, &carMeter_, &clubMeter_})
        m->setVisible(showMeters);

    summaryHeaderLabel_.setVisible(clientMode_);
    summaryBodyLabel_.setVisible(clientMode_);

    resized();
    repaint();
}

namespace {
const char* const kEnvNames[5] = {NEEDS_TRANS("Studio"), NEEDS_TRANS("Earbuds"), NEEDS_TRANS("Phone"), NEEDS_TRANS("Car"),
                                  NEEDS_TRANS("Club")};
const char* const kBandNames[3] = {NEEDS_TRANS("Lows"), NEEDS_TRANS("Mids"), NEEDS_TRANS("Highs")};

/// VIS-4: the risk in words beside the number.
juce::String riskWord(float risk)
{
    return risk < 0.3f ? TRANS("low") : risk < 0.65f ? TRANS("medium") : TRANS("high");
}
} // namespace

juce::Rectangle<int> TranslationPanel::matrixArea() const
{
    // Below what resized() lays out: the toggle row, the summary or the five
    // meters, the audition row and its note.
    auto b = bodyArea();
    b.removeFromTop(24 + 6);
    b.removeFromTop(clientMode_ ? 100 : 5 * 24);
    b.removeFromTop(4 + 22 + 2 + 14);
    b.removeFromTop(8);
    return b;
}

juce::Rectangle<int> TranslationPanel::cellRect(int env, int band) const
{
    auto b = matrixArea();
    b.removeFromTop(16 + 6 + 14 + 4); // title, gap, column headers, gap
    const int labelWidth = 70;
    const int colWidth = (b.getWidth() - labelWidth) / 3;
    const int rowHeight = juce::jmin(16, b.getHeight() / kNumEnvs);
    return {b.getX() + labelWidth + band * colWidth + 4, b.getY() + env * (rowHeight + 2) + 1, colWidth - 8, rowHeight - 2};
}

juce::String TranslationPanel::matrixSummary() const
{
    // VIS-8: the high-risk cells by name, then the rest by count.
    juce::StringArray high;
    for (int i = 0; i < kNumEnvs; ++i)
        for (int j = 0; j < kNumBands; ++j)
            if (riskMatrix_[i][j] >= 0.65f)
                high.add(juce::translate(kEnvNames[i]) + " " + juce::translate(kBandNames[j]).toLowerCase() + " " +
                         juce::String(riskMatrix_[i][j], 2));
    return TRANS("Translation risk per playback environment and band, 0 to 1") + ": " +
           (high.isEmpty() ? TRANS("no high-risk cells") : TRANS("high risk in") + " " + ui::chart::joinWords(high)) + ".";
}

void TranslationPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);

    auto b = matrixArea();
    const auto style = desktopChartStyle();

    // Matrix title
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel).boldened());
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(TRANS("Environmental risk matrix"), b.removeFromTop(16), juce::Justification::centredLeft, false);
    b.removeFromTop(6);

    const int labelWidth = 70;
    const int colWidth = (b.getWidth() - labelWidth) / 3;
    auto headerRow = b.removeFromTop(14);
    g.setFont(ui::Palette::uiFont(style.fontSize).boldened());
    g.setColour(KhensuTheme::textSecondary);
    for (int j = 0; j < kNumBands; ++j)
        g.drawText(juce::translate(kBandNames[j]), headerRow.getX() + labelWidth + j * colWidth + 4, headerRow.getY(), colWidth,
                   headerRow.getHeight(), juce::Justification::centred, false);
    b.removeFromTop(4);

    const int rowHeight = juce::jmin(16, b.getHeight() / kNumEnvs);
    for (int i = 0; i < kNumEnvs; ++i) {
        auto row = b.removeFromTop(rowHeight);
        g.setFont(ui::Palette::uiFont(style.fontSize));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(juce::translate(kEnvNames[i]), row.getX(), row.getY(), labelWidth, row.getHeight(),
                   juce::Justification::centredLeft, true);
        for (int j = 0; j < kNumBands; ++j) {
            const auto cell = cellRect(i, j);
            const float risk = riskMatrix_[i][j];
            // VIS-4: risk is a magnitude - the sequential palette, the number
            // and its word written in the cell.
            g.setColour(KhensuTheme::vizSequential(juce::jlimit(0.0f, 1.0f, risk)));
            g.fillRoundedRectangle(cell.toFloat(), 3.0f);
            const bool hovered = cell.contains(hoverPos_);
            g.setColour(hovered ? KhensuTheme::textPrimary : style.axis);
            g.drawRoundedRectangle(cell.toFloat(), 3.0f, 1.0f);
            g.setFont(KhensuTheme::monoFont(type::caption));
            if (KhensuTheme::vizSequential(juce::jlimit(0.0f, 1.0f, risk)).getPerceivedBrightness() < 0.5f)
                g.setColour(KhensuTheme::textPrimary);
            else
                g.setColour(KhensuTheme::bgVoid);
            g.drawText(juce::String(risk, 2) + (cell.getWidth() > 70 ? " " + riskWord(risk) : juce::String()), cell,
                       juce::Justification::centred, false);
        }
        b.removeFromTop(2); // row spacing
    }

    // VIS-1: the ChartKit readout for the cell under the pointer.
    for (int i = 0; i < kNumEnvs; ++i)
        for (int j = 0; j < kNumBands; ++j)
            if (cellRect(i, j).contains(hoverPos_)) {
                ui::chart::Readout r;
                r.title = juce::translate(kEnvNames[i]) + ", " + juce::translate(kBandNames[j]).toLowerCase();
                r.lines.push_back({TRANS("Risk") + " " + juce::String(riskMatrix_[i][j], 2) + " - " + riskWord(riskMatrix_[i][j]),
                                   KhensuTheme::vizSequential(juce::jlimit(0.0f, 1.0f, riskMatrix_[i][j]))});
                ui::chart::ChartView::paintReadout(g, r, hoverPos_.toFloat(), getLocalBounds().toFloat(), style);
            }
}

void TranslationPanel::mouseMove(const juce::MouseEvent& e)
{
    hoverPos_ = e.getPosition();
    repaint(matrixArea());
}

void TranslationPanel::mouseExit(const juce::MouseEvent&)
{
    hoverPos_ = {-1, -1};
    repaint(matrixArea());
}

void TranslationPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    // Position the toggle button at the top right
    auto topRow = b.removeFromTop(24);
    reportToggle_.setBounds(topRow.removeFromRight(120));
    b.removeFromTop(6);

    if (clientMode_) {
        auto summaryArea = b.removeFromTop(100);
        summaryHeaderLabel_.setBounds(summaryArea.removeFromTop(20));
        summaryArea.removeFromTop(4);
        summaryBodyLabel_.setBounds(summaryArea);
    } else {
        for (auto* m : {&studioMeter_, &earbudsMeter_, &phoneMeter_, &carMeter_, &clubMeter_}) {
            m->setBounds(b.removeFromTop(20));
            b.removeFromTop(4);
        }
    }

    // Audition row + note (both modes - hearing it beats reading about it).
    b.removeFromTop(4);
    auto hearRow = b.removeFromTop(22);
    const int bw = hearRow.getWidth() / 5 - 3;
    for (auto* btn :
         {&hearPhoneButton_, &hearEarbudsButton_, &hearCarButton_, &hearClubButton_, &hearOriginalButton_}) {
        btn->setBounds(hearRow.removeFromLeft(bw));
        hearRow.removeFromLeft(3);
    }
    b.removeFromTop(2);
    deviceNoteLabel_.setBounds(b.removeFromTop(14));
    matrixSummary_.setBounds(matrixArea());
}

} // namespace khensu
