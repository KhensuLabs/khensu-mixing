// desktop-app/Source/ui/VocalWorkspace/WordClipGainEditor.cpp
#include "../shared/ServiceErrorHint.h"
#include "WordClipGainEditor.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/session/AudioFileProber.h>
#include "../shared/RenderPaths.h"
#include "../shared/SessionAudioPaths.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
constexpr int kControlsH = 26;
constexpr int kStatusH = 16;
constexpr int kPeakBins = 1200;

/// Reads `path` and returns a kPeakBins-long |peak| envelope (0..1) plus the
/// duration. Runs on a background thread.
std::pair<std::vector<float>, double> readPeaks(const juce::String& path)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader = AudioFileProber::openReader(fm, juce::File(path));
    if (reader == nullptr || reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0) return {{}, 0.0};
    const auto total = reader->lengthInSamples;
    const auto perBin = juce::jmax<juce::int64>(1, total / kPeakBins);
    std::vector<float> peaks(static_cast<size_t>(kPeakBins), 0.0f);
    for (int b = 0; b < kPeakBins; ++b) {
        const auto start = static_cast<juce::int64>(b) * perBin;
        if (start >= total) break;
        const auto len = static_cast<int>(juce::jmin(perBin, total - start));
        juce::Range<float> range[2];
        reader->readMaxLevels(start, len, range, juce::jmin(2, static_cast<int>(reader->numChannels)));
        float p = 0.0f;
        for (const auto& r : range)
            p = juce::jmax(p, std::abs(r.getStart()), std::abs(r.getEnd()));
        peaks[static_cast<size_t>(b)] = juce::jmin(1.0f, p);
    }
    return {peaks, static_cast<double>(total) / reader->sampleRate};
}
} // namespace

void WordClipGainEditor::applyThemeColours()
{
    for (auto* b : {&transcribeButton_, &renderButton_, &resetButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

WordClipGainEditor::WordClipGainEditor(IAppController& c) : PanelCard(c, "Per-Word Clip Gain")
{
    applyThemeColours();
    setExplainer(TRANS("Per-Word Clip Gain"),
                 TRANS("Ride individual words before any compression. Find Words transcribes the vocal and boxes each "
                       "word over its waveform. Drag across the waveform to draw a gain line: every word the stroke "
                       "passes takes that level (+/-12 dB, 0.5 dB steps). Keyboard: Left/Right select a word, Up/Down "
                       "nudge it 0.5 dB, Delete resets it. Render applies click-free ramps; a render of the vocal stem "
                       "loads into the transport for preview (the mix is unchanged)."));
    setWantsKeyboardFocus(true);
    setTitle(TRANS("Per-word clip gain lane"));
    setDescription(TRANS("Waveform with per-word gain. Left/Right select a word, Up/Down adjust its gain."));

    transcribeButton_.setTooltip(TRANS("Transcribe the vocal into timed words (needs the optional ASR install)."));
    transcribeButton_.onClick = [this] { transcribe(); };
    addAndMakeVisible(transcribeButton_);

    resetButton_.setTooltip(TRANS("Set every word back to 0 dB."));
    resetButton_.onClick = [this] {
        model_.resetAll();
        updateStatus();
        repaint();
    };
    resetButton_.setEnabled(false);
    addAndMakeVisible(resetButton_);

    renderButton_.setTooltip(TRANS("Apply the per-word gains and load the result for preview."));
    renderButton_.onClick = [this] { render(); };
    renderButton_.setEnabled(false);
    addAndMakeVisible(renderButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    statusLabel_.setText(TRANS("Find words, then draw gain across the waveform."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);
}

juce::String WordClipGainEditor::targetFilePath() const
{
    return session_audio::vocalStemOrSourcePath(controller_);
}

void WordClipGainEditor::transcribe()
{
    const auto target = targetFilePath();
    if (target.isEmpty()) {
        statusLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    transcribeButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Transcribing words (first run downloads the ASR model)..."),
                         juce::dontSendNotification);
    juce::Component::SafePointer<WordClipGainEditor> self(this);
    // ASR (and a possible first-run model download): the long pool (UIX-14).
    controller_.runLongInBackground([self, &work = controller_, target] {
        auto [peaks, duration] = readPeaks(target);
        ipc::HttpServiceBridge bridge;
        auto result = bridge.analyzeLyrics(target.toStdString());
        postIfCurrent(self, work, [result, peaks = std::move(peaks), duration](WordClipGainEditor& panel) mutable {
            panel.peaks_ = std::move(peaks);
            panel.durationSec_ = duration;
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Transcription failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                panel.repaint();
                return;
            }
            std::vector<WordClipGainModel::Word> words;
            for (const auto& w : result.value().words)
                words.push_back({juce::String(w.word).trim(), w.start_sec, w.end_sec, 0.0});
            panel.model_.setWords(std::move(words));
            if (panel.durationSec_ <= 0.0 && !panel.model_.empty())
                panel.durationSec_ = panel.model_.words().back().endSec;
            const bool ok = !panel.model_.empty();
            panel.selected_ = ok ? 0 : -1;
            panel.resetButton_.setEnabled(ok);
            panel.renderButton_.setEnabled(ok);
            panel.updateStatus();
            panel.repaint();
        }, [](WordClipGainEditor& panel) { panel.transcribeButton_.setEnabled(true); });
    });
}

void WordClipGainEditor::render()
{
    const auto target = targetFilePath();
    const auto regions = model_.regionsAsVar();
    if (target.isEmpty()) return;
    if (regions.size() == 0) {
        statusLabel_.setText(TRANS("Nothing to render - draw some gain first."), juce::dontSendNotification);
        return;
    }
    renderButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Rendering..."), juce::dontSendNotification);
    // Captured at start: the render must register into THIS session only (AUDIT-002).
    const juce::String sessionId = controller_.getActiveSession() != nullptr
                                       ? juce::String(controller_.getActiveSession()->id)
                                       : juce::String();
    juce::Component::SafePointer<WordClipGainEditor> self(this);
    // A fresh name per render: the service's fixed <source>_wordgain.wav default
    // overwrote the file earlier render-history nodes point at.
    const juce::String outPath = uniqueRenderFile(juce::File(target), "_wordgain").getFullPathName();
    const auto scope = session_audio::scopeForInput(controller_, target);
    controller_.runLongInBackground([self, &work = controller_, target, regions, outPath, scope, sessionId] {
        juce::DynamicObject::Ptr body = new juce::DynamicObject();
        body->setProperty("file_path", target);
        body->setProperty("regions", regions);
        body->setProperty("output_path", outPath);
        ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 300000);
        auto result = bridge.postAnalyzeJson("/render/word_clip_gain", juce::var(body.get()));
        postIfCurrent(self, work, [result, scope, sessionId, target](WordClipGainEditor& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Render failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const auto rendered = result.value()[af::WordClipGainResponse::rendered_path].toString();
            if (rendered.isEmpty() || !juce::File(rendered).existsAsFile()) {
                panel.statusLabel_.setText(TRANS("Render produced no file."), juce::dontSendNotification);
                return;
            }
            // A vocal-stem render is auditioned, never made the mix (UIX-01).
            const bool isMix = session_audio::applyRender(panel.controller_, rendered, sessionId, scope, target);
            // The render is 32-bit float and never normalized, so boosts can
            // push words past full scale: it won't clip in the file, but it
            // will on any fixed-point bounce or DAC. Say so instead of
            // quietly reporting a positive peak. Whole sentences with a
            // placeholder, never fragments (UIX-23).
            const double peak = static_cast<double>(result.value()[af::WordClipGainResponse::peak_dbfs]);
            const auto peakText = juce::String(peak, 1);
            juce::String msg;
            if (peak > -0.1)
                msg = TRANS("Rendered, but peaks reach %p dBFS - lower the loudest boosts to avoid clipping.");
            else if (isMix)
                msg = TRANS("Rendered (peak %p dBFS) - use the transport A/B to compare.");
            else
                msg = TRANS("Rendered vocal (peak %p dBFS) loaded for preview - the mix is unchanged.");
            panel.statusLabel_.setText(msg.replace("%p", peakText), juce::dontSendNotification);
        }, [](WordClipGainEditor& panel) { panel.renderButton_.setEnabled(true); });
    });
}

void WordClipGainEditor::updateStatus()
{
    juce::String s = TRANS("%w words, %r gain regions")
                         .replace("%w", juce::String(static_cast<int>(model_.words().size())))
                         .replace("%r", juce::String(static_cast<int>(model_.regions().size())));
    if (selected_ >= 0 && static_cast<size_t>(selected_) < model_.words().size()) {
        const auto& w = model_.words()[static_cast<size_t>(selected_)];
        s << " - \"" << w.text << "\" " << (w.gainDb > 0 ? "+" : "") << juce::String(w.gainDb, 1) << " dB";
    }
    statusLabel_.setText(s, juce::dontSendNotification);
}

juce::Rectangle<float> WordClipGainEditor::laneArea() const
{
    auto b = bodyArea();
    b.removeFromTop(kControlsH + 2 + kStatusH + 2);
    return b.toFloat().reduced(2.0f);
}

float WordClipGainEditor::xForTime(double t) const
{
    const auto a = laneArea();
    return a.getX() + static_cast<float>(t / juce::jmax(0.001, durationSec_)) * a.getWidth();
}

double WordClipGainEditor::timeForX(float x) const
{
    const auto a = laneArea();
    return static_cast<double>((x - a.getX()) / juce::jmax(1.0f, a.getWidth())) * juce::jmax(0.001, durationSec_);
}

float WordClipGainEditor::yForDb(double db) const
{
    const auto a = laneArea();
    return a.getCentreY() - static_cast<float>(db / WordClipGainModel::kMaxGainDb) * a.getHeight() * 0.5f;
}

double WordClipGainEditor::dbForY(float y) const
{
    const auto a = laneArea();
    return static_cast<double>((a.getCentreY() - y) / juce::jmax(1.0f, a.getHeight() * 0.5f)) *
           WordClipGainModel::kMaxGainDb;
}

void WordClipGainEditor::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    const auto lane = laneArea();
    if (lane.getHeight() < 30.0f) return;
    if (model_.empty() && peaks_.empty()) {
        drawEmptyState(g, "No words yet", "Find Words boxes each sung word so you can ride its level.",
                       kControlsH + kStatusH + 4);
        return;
    }

    g.setColour(KhensuTheme::bgVoid.withAlpha(0.4f));
    g.fillRoundedRectangle(lane, 4.0f);

    // Waveform, drawn after per-word gain so the user sees the result shape.
    if (!peaks_.empty()) {
        const float mid = lane.getCentreY();
        const float half = lane.getHeight() * 0.45f;
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.55f));
        const auto n = peaks_.size();
        for (size_t i = 0; i < n; ++i) {
            const double t = durationSec_ * static_cast<double>(i) / static_cast<double>(n);
            const int w = model_.wordAt(t);
            const double gain =
                w >= 0 ? juce::Decibels::decibelsToGain(model_.words()[static_cast<size_t>(w)].gainDb) : 1.0;
            const float h = juce::jmin(half, peaks_[i] * static_cast<float>(gain) * half);
            const float x = lane.getX() + lane.getWidth() * static_cast<float>(i) / static_cast<float>(n);
            g.drawVerticalLine(juce::roundToInt(x), mid - h, mid + h);
        }
    }

    // 0 dB line.
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(juce::roundToInt(lane.getCentreY()), lane.getX(), lane.getRight());

    g.setFont(KhensuTheme::labelFont(9.0f));
    for (size_t i = 0; i < model_.words().size(); ++i) {
        const auto& w = model_.words()[i];
        const float x0 = xForTime(w.startSec), x1 = xForTime(w.endSec);
        const juce::Rectangle<float> box(x0, lane.getY(), juce::jmax(2.0f, x1 - x0), lane.getHeight());
        const bool sel = static_cast<int>(i) == selected_;
        g.setColour((sel ? KhensuTheme::accentTeal : KhensuTheme::accentBlue).withAlpha(sel ? 0.18f : 0.07f));
        g.fillRect(box);
        // Gain handle: boost teal, cut amber.
        const float gy = yForDb(w.gainDb);
        g.setColour(w.gainDb >= 0.0 ? KhensuTheme::accentTeal : KhensuTheme::accentAmber);
        g.drawLine(x0 + 1.0f, gy, x1 - 1.0f, gy, sel ? 2.5f : 1.5f);
        if (box.getWidth() > 18.0f) {
            g.setColour(KhensuTheme::textSecondary);
            g.drawText(w.text, box.withHeight(12.0f).reduced(1.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        if (sel) {
            g.setColour(KhensuTheme::focusRing);
            g.drawRect(box, 1.0f);
        }
    }

    g.setColour(KhensuTheme::textTertiary);
    g.drawText("+12 dB", lane.withHeight(10.0f).withTrimmedLeft(lane.getWidth() - 40.0f), juce::Justification::right);
    g.drawText("-12 dB", lane.withTrimmedTop(lane.getHeight() - 10.0f).withTrimmedLeft(lane.getWidth() - 40.0f),
               juce::Justification::right);
}

void WordClipGainEditor::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(kControlsH);
    transcribeButton_.setBounds(controls.removeFromLeft(100).reduced(0, 1));
    controls.removeFromLeft(6);
    resetButton_.setBounds(controls.removeFromLeft(60).reduced(0, 1));
    renderButton_.setBounds(controls.removeFromRight(110).reduced(0, 1));
    b.removeFromTop(2);
    statusLabel_.setBounds(b.removeFromTop(kStatusH));
}

void WordClipGainEditor::mouseDown(const juce::MouseEvent& e)
{
    if (model_.empty() || !laneArea().contains(e.position)) {
        PanelCard::mouseDown(e);
        return;
    }
    grabKeyboardFocus();
    drawing_ = true;
    lastDrawPos_ = e.position;
    const double t = timeForX(e.position.x);
    model_.drawStroke(t, dbForY(e.position.y), t, dbForY(e.position.y));
    const int hit = model_.wordAt(t);
    if (hit >= 0) selected_ = hit;
    updateStatus();
    repaint();
}

void WordClipGainEditor::mouseDrag(const juce::MouseEvent& e)
{
    if (!drawing_) {
        PanelCard::mouseDrag(e);
        return;
    }
    model_.drawStroke(timeForX(lastDrawPos_.x), dbForY(lastDrawPos_.y), timeForX(e.position.x), dbForY(e.position.y));
    lastDrawPos_ = e.position;
    updateStatus();
    repaint();
}

void WordClipGainEditor::mouseUp(const juce::MouseEvent&) { drawing_ = false; }

bool WordClipGainEditor::keyPressed(const juce::KeyPress& key)
{
    if (model_.empty()) return false;
    const int n = static_cast<int>(model_.words().size());
    bool handled = true;
    if (key == juce::KeyPress::leftKey)
        selected_ = juce::jmax(0, selected_ - 1);
    else if (key == juce::KeyPress::rightKey)
        selected_ = juce::jmin(n - 1, selected_ + 1);
    else if (key == juce::KeyPress::upKey && selected_ >= 0)
        model_.nudge(static_cast<size_t>(selected_), 0.5);
    else if (key == juce::KeyPress::downKey && selected_ >= 0)
        model_.nudge(static_cast<size_t>(selected_), -0.5);
    else if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && selected_ >= 0)
        model_.setGain(static_cast<size_t>(selected_), 0.0);
    else
        handled = false;
    if (handled) {
        updateStatus();
        repaint();
    }
    return handled;
}

} // namespace khensu
