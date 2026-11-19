// desktop-app/Source/ui/VocalWorkspace/GainRiderPanel.cpp
#include "GainRiderPanel.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RenderPaths.h"
#include "../shared/SessionAudioPaths.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void GainRiderPanel::applyThemeColours()
{
    computeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    computeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    renderButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    renderButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

GainRiderPanel::GainRiderPanel(IAppController& c) : PanelCard(c, "Smart Gain Rider")
{
    applyThemeColours();
    setExplainer(TRANS("Smart Gain Rider"), TRANS("The secret sauce of pro vocal chains: level the performance with gain "
                                     "automation BEFORE the compressor, not with more compression. Loud phrases "
                                     "get pulled down and quiet phrases pushed up, each toward the -18 dBFS RMS "
                                     "sweet spot - so the compressor then only needs 2-4 dB of musical "
                                     "gain reduction instead of fighting a 20 dB performance range.\n\n"
                                     "Compute shows the per-phrase moves; Render applies them (with click-free "
                                     "ramps). Works on the isolated vocal stem when available - that render loads "
                                     "into the transport for preview and never replaces the mix."));

    computeButton_.onClick = [this] { computeRide(); };
    addAndMakeVisible(computeButton_);

    renderButton_.setEnabled(false);
    renderButton_.onClick = [this] { renderRide(); };
    addAndMakeVisible(renderButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    statusLabel_.setText(TRANS("Level vocal phrases before compression."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);
}

juce::String GainRiderPanel::targetFilePath() const
{
    // Prefer the isolated vocal stem; fall back to the session source.
    return session_audio::vocalStemOrSourcePath(controller_);
}

void GainRiderPanel::computeRide()
{
    const juce::String target = targetFilePath();
    if (target.isEmpty()) {
        statusLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    statusLabel_.setText(TRANS("Analyzing phrases..."), juce::dontSendNotification);
    computeButton_.setEnabled(false);

    juce::Component::SafePointer<GainRiderPanel> self(this);
    controller_.runInBackground([self, &work = controller_, target]() {
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeGainRider(target.toStdString(), -18.0f, false);
        postIfCurrent(self, work, [result](GainRiderPanel& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Analysis failed: ") + juce::String(result.error().message),
                                           juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            panel.phrases_.clear();
            if (auto* arr = v[af::GainRiderResponse::phrases].getArray()) {
                for (const auto& pv : *arr) {
                    Phrase p;
                    p.start = (float)(double)pv[af::GainRiderPhrase::start_sec];
                    p.end = (float)(double)pv[af::GainRiderPhrase::end_sec];
                    p.rmsDb = (float)(double)pv[af::GainRiderPhrase::rms_db];
                    p.gainDb = (float)(double)pv[af::GainRiderPhrase::gain_db];
                    panel.phrases_.push_back(p);
                }
            }
            panel.noiseFloorDb_ = (float)(double)v[af::GainRiderResponse::noise_floor_db];
            panel.totalDuration_ = panel.phrases_.empty() ? 0.0f : panel.phrases_.back().end;
            panel.hasRide_ = !panel.phrases_.empty();
            panel.renderButton_.setEnabled(panel.hasRide_);

            float maxMove = 0.0f;
            for (const auto& p : panel.phrases_)
                maxMove = juce::jmax(maxMove, std::abs(p.gainDb));
            panel.statusLabel_.setText(TRANS("%n phrases - largest move %d dB")
                                           .replace("%n", juce::String((int)panel.phrases_.size()))
                                           .replace("%d", juce::String(maxMove, 1)),
                                       juce::dontSendNotification);
            panel.repaint();
        }, [](GainRiderPanel& panel) { panel.computeButton_.setEnabled(true); });
    });
}

void GainRiderPanel::renderRide()
{
    const juce::String target = targetFilePath();
    if (target.isEmpty() || !hasRide_) return;
    statusLabel_.setText(TRANS("Rendering..."), juce::dontSendNotification);
    renderButton_.setEnabled(false);

    // Captured at start: the render must register into THIS session only (AUDIT-002).
    const juce::String sessionId = controller_.getActiveSession() != nullptr
                                       ? juce::String(controller_.getActiveSession()->id)
                                       : juce::String();
    // A fresh name per render: the server's fixed "<file>_gainride.wav"
    // overwrote renders the history / transport still referenced (UIX-11).
    const juce::String outPath = uniqueRenderFile(juce::File(target), "_gainride").getFullPathName();
    const auto scope = session_audio::scopeForInput(controller_, target);
    juce::Component::SafePointer<GainRiderPanel> self(this);
    // A render, not a quick fetch: the long pool (UIX-14).
    controller_.runLongInBackground([self, &work = controller_, target, outPath, scope, sessionId]() {
        ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 300000);
        juce::DynamicObject::Ptr body = new juce::DynamicObject();
        body->setProperty(af::GainRiderRequest::file_path, target);
        body->setProperty(af::GainRiderRequest::target_rms_db, -18.0f);
        body->setProperty(af::GainRiderRequest::render, true);
        body->setProperty(af::GainRiderRequest::output_path, outPath); // fresh name per render (UIX-11)
        auto result = bridge.postAnalyzeJson("/analyze/gain_rider", juce::var(body.get()));
        postIfCurrent(self, work, [result, scope, sessionId, target](GainRiderPanel& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Render failed: ") + juce::String(result.error().message),
                                           juce::dontSendNotification);
                return;
            }
            const juce::String rendered = result.value()[af::GainRiderResponse::rendered_path].toString();
            if (rendered.isEmpty() || !juce::File(rendered).existsAsFile()) {
                panel.statusLabel_.setText(TRANS("Render produced no file."), juce::dontSendNotification);
                return;
            }
            // A ride of the vocal STEM is not a version of the mix: audition
            // it directly instead of making it the master (UIX-01).
            const bool isMix = session_audio::applyRender(panel.controller_, rendered, sessionId, scope, target);
            panel.statusLabel_.setText(isMix ? TRANS("Rendered - use the transport A/B to compare.")
                                             : TRANS("Rendered vocal loaded for preview - the mix is unchanged."),
                                       juce::dontSendNotification);
        }, [](GainRiderPanel& panel) { panel.renderButton_.setEnabled(true); });
    });
}

void GainRiderPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!hasRide_ || totalDuration_ <= 0.0f) return;

    auto b = bodyArea();
    b.removeFromTop(28); // controls
    b.removeFromTop(16); // status
    auto strip = b.toFloat().reduced(2.0f, 4.0f);
    if (strip.getHeight() < 24.0f) return;

    // Zero line.
    const float midY = strip.getCentreY();
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.3f));
    g.drawHorizontalLine(juce::roundToInt(midY), strip.getX(), strip.getRight());

    // Per-phrase gain bars: up = boost, down = cut, colored by direction.
    const float maxDb = 12.0f;
    for (const auto& p : phrases_) {
        const float x0 = strip.getX() + strip.getWidth() * (p.start / totalDuration_);
        const float x1 = strip.getX() + strip.getWidth() * (p.end / totalDuration_);
        const float h = (std::abs(p.gainDb) / maxDb) * strip.getHeight() * 0.5f;
        const bool boost = p.gainDb > 0.0f;
        g.setColour((boost ? KhensuTheme::accentTeal : KhensuTheme::accentAmber).withAlpha(0.7f));
        if (boost)
            g.fillRect(x0, midY - h, juce::jmax(1.0f, x1 - x0 - 1.0f), h);
        else
            g.fillRect(x0, midY, juce::jmax(1.0f, x1 - x0 - 1.0f), h);
    }

    g.setFont(KhensuTheme::labelFont(8.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("+12 dB", (int)strip.getX(), (int)strip.getY(), 40, 10, juce::Justification::left);
    g.drawText("-12 dB", (int)strip.getX(), (int)strip.getBottom() - 10, 40, 10, juce::Justification::left);
}

void GainRiderPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    computeButton_.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(6);
    renderButton_.setBounds(controls.removeFromLeft(110));
    b.removeFromTop(2);
    statusLabel_.setBounds(b.removeFromTop(14));
}

} // namespace khensu
