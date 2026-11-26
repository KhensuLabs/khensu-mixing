// desktop-app/Source/ui/Dashboard/WaveformEditorPanel.cpp
#include "WaveformEditorPanel.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include "../shared/SpectrogramImage.h"
#include "../shared/ExplainMomentView.h"
#include <khensu/session/AudioFileProber.h>

namespace khensu {

namespace {
constexpr int kToolbarH = 26;
constexpr int kStatusH = 16;
constexpr float kOverviewH = 26.0f;
constexpr float kCommentsLaneH = 22.0f; // CE-COL-1
constexpr int kFftOrder = 11; // 2048
constexpr int kBands = 96;
constexpr int kMaxFrames = 1600;

juce::String formatTime(double s)
{
    const int m = static_cast<int>(s / 60.0);
    return juce::String(m) + ":" + juce::String(s - m * 60.0, 2).paddedLeft('0', 5);
}
} // namespace

void WaveformEditorPanel::applyThemeColours()
{
    for (auto* b : {&spectroButton_, &zoomInButton_, &zoomOutButton_, &fitButton_, &zoomSelButton_, &noteButton_,
                    &explainButton_}) {
        b->setColour(juce::TextButton::buttonOnColourId, KhensuTheme::bgOverlay); // UX2-3: a quiet "on"
        b->setColour(juce::TextButton::textColourOnId, KhensuTheme::textPrimary);
    }
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

WaveformEditorPanel::WaveformEditorPanel(IAppController& c)
    : PanelCard(c, "Waveform & Spectrogram Editor"), commentsLane_(c)
{
    applyThemeColours();
    setExplainer(
        TRANS("Waveform & Spectrogram Editor"),
        TRANS("Inspect the loaded audio at any zoom. Click to seek (drag the playhead to scrub), drag to select "
              "a range, then Zoom to Selection or Add Note to annotate it - notes are saved with this audio "
              "file. Ctrl+wheel zooms, the wheel scrolls, and the strip at the bottom shows where the view sits "
              "in the whole song. Spectrogram mode shows frequency content with a live frequency readout.\n\n"
              "Keyboard: Space play/pause, Left/Right move 1 s (Shift extends the selection), +/- zoom, 0 fit, "
              "Z zoom to selection, N add note, Delete remove the note at the playhead, [ / ] jump between "
              "notes, Esc clear selection, S toggle spectrogram."));
    setWantsKeyboardFocus(true);
    setTitle(TRANS("Waveform and spectrogram editor"));
    setDescription(TRANS("Zoomable audio view with seek, selection and annotations. Space plays, Left/Right move."));

    spectroButton_.setClickingTogglesState(true);
    spectroButton_.setTooltip(TRANS("Switch between waveform and spectrogram (S)."));
    spectroButton_.onClick = [this] {
        showSpectrogram_ = spectroButton_.getToggleState();
        if (showSpectrogram_ && spectroFrames_.empty() && sourcePath_.isNotEmpty()) computeSpectrogram(sourcePath_);
        repaint();
    };
    zoomInButton_.setTitle(TRANS("Zoom in"));
    zoomInButton_.setTooltip(TRANS("Zoom in around the playhead (+)."));
    zoomInButton_.onClick = [this] {
        state_.zoomAround(state_.playhead(), 2.0);
        repaint();
    };
    zoomOutButton_.setTitle(TRANS("Zoom out"));
    zoomOutButton_.setTooltip(TRANS("Zoom out (-)."));
    zoomOutButton_.onClick = [this] {
        state_.zoomAround(state_.playhead(), 0.5);
        repaint();
    };
    fitButton_.setTooltip(TRANS("Show the whole file (0)."));
    fitButton_.onClick = [this] {
        state_.zoomToFit();
        repaint();
    };
    zoomSelButton_.setTooltip(TRANS("Zoom so the selection fills the view (Z)."));
    zoomSelButton_.onClick = [this] {
        if (state_.hasSelection()) state_.zoomTo(state_.selectionStart(), state_.selectionEnd());
        repaint();
    };
    noteButton_.setTooltip(TRANS("Annotate the selection, or the playhead if nothing is selected (N)."));
    noteButton_.onClick = [this] { promptForAnnotation(-1); };
    explainButton_.setTooltip(TRANS("Explain what makes the selected passage different from the rest of the track (E)."));
    explainButton_.onClick = [this] { explainSelection(); };
    for (auto* b : {&spectroButton_, &zoomInButton_, &zoomOutButton_, &fitButton_, &zoomSelButton_, &noteButton_,
                    &explainButton_})
        addAndMakeVisible(*b);

    statusLabel_.setFont(KhensuTheme::monoFont(type::caption));
    addAndMakeVisible(statusLabel_);

    // CE-COL-1: the comments lane follows this view and seeks through it.
    commentsLane_.viewProvider = [this] {
        SessionCommentsLane::View v;
        v.viewStart = state_.viewStart();
        v.viewEnd = state_.viewEnd();
        v.duration = state_.duration();
        v.playhead = state_.playhead();
        if (state_.hasSelection()) {
            v.selStart = state_.selectionStart();
            v.selEnd = state_.selectionEnd();
        }
        return v;
    };
    commentsLane_.onSeek = [this](double t) { seek(t); };
    commentsLane_.onStatus = [this](const juce::String& s) { statusLabel_.setText(s, juce::dontSendNotification); };
    addAndMakeVisible(commentsLane_);
    updateStatus();
}

WaveformEditorPanel::~WaveformEditorPanel() = default;

juce::Rectangle<float> WaveformEditorPanel::mainArea() const
{
    auto b = bodyArea();
    b.removeFromTop(kToolbarH + 2 + kStatusH + 2);
    auto f = b.toFloat().reduced(2.0f);
    f.removeFromBottom(kOverviewH + 4.0f + kCommentsLaneH + 2.0f); // CE-COL-1 comments lane
    return f;
}

juce::Rectangle<float> WaveformEditorPanel::overviewArea() const
{
    auto b = bodyArea().toFloat().reduced(2.0f);
    return b.removeFromBottom(kOverviewH);
}

void WaveformEditorPanel::syncSource()
{
    const juce::String path = controller_.activeSourcePath();
    const double dur = controller_.playback().getPlaybackDuration();
    if (path != sourcePath_ || (dur > 0.0 && std::abs(dur - state_.duration()) > 0.01)) {
        if (path != sourcePath_) {
            sourcePath_ = path;
            spectroFrames_.clear();
            spectroImage_ = {};
            loadAnnotations();
            if (showSpectrogram_ && path.isNotEmpty()) computeSpectrogram(path);
        }
        const auto saved = state_.annotationsToVar();
        state_.setDuration(dur);
        state_.annotationsFromVar(saved);
        repaint();
    }
    auto peaks = controller_.playback().getWaveformPeaks();
    if (peaks.size() != peaks_.size()) {
        peaks_ = std::move(peaks);
        repaint();
    }
}

void WaveformEditorPanel::computeSpectrogram(const juce::String& path)
{
    if (spectroPending_) return;
    spectroPending_ = true;
    fetchQuietly(*this, controller_, [path] {
        std::vector<std::vector<float>> frames;
        double nyquist = 22050.0;
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader = AudioFileProber::openReader(fm, juce::File(path));
        if (reader != nullptr && reader->lengthInSamples > 0) {
            nyquist = reader->sampleRate * 0.5;
            const int fftSize = 1 << kFftOrder;
            const auto total = reader->lengthInSamples;
            const auto hop = juce::jmax<juce::int64>(fftSize / 4, total / kMaxFrames);
            juce::dsp::FFT fft(kFftOrder);
            juce::dsp::WindowingFunction<float> window(static_cast<size_t>(fftSize),
                                                       juce::dsp::WindowingFunction<float>::hann);
            juce::AudioBuffer<float> chunk(static_cast<int>(reader->numChannels), fftSize);
            std::vector<float> fftData(static_cast<size_t>(fftSize) * 2);
            // Log-spaced band edges from 30 Hz to Nyquist.
            std::vector<int> edges(kBands + 1);
            for (int b = 0; b <= kBands; ++b) {
                const double hz = 30.0 * std::pow(nyquist / 30.0, static_cast<double>(b) / kBands);
                edges[static_cast<size_t>(b)] =
                    juce::jlimit(1, fftSize / 2, static_cast<int>(hz / nyquist * (fftSize / 2)));
            }
            for (juce::int64 pos = 0; pos + fftSize <= total; pos += hop) {
                reader->read(&chunk, 0, fftSize, pos, true, true);
                std::fill(fftData.begin(), fftData.end(), 0.0f);
                for (int ch = 0; ch < chunk.getNumChannels(); ++ch)
                    juce::FloatVectorOperations::add(fftData.data(), chunk.getReadPointer(ch), fftSize);
                window.multiplyWithWindowingTable(fftData.data(), static_cast<size_t>(fftSize));
                fft.performFrequencyOnlyForwardTransform(fftData.data());
                std::vector<float> bands(kBands);
                for (int b = 0; b < kBands; ++b) {
                    float m = 0.0f;
                    for (int k = edges[static_cast<size_t>(b)]; k <= edges[static_cast<size_t>(b) + 1]; ++k)
                        m = juce::jmax(m, fftData[static_cast<size_t>(k)]);
                    const float db = juce::Decibels::gainToDecibels(m / static_cast<float>(fftSize), -100.0f);
                    bands[static_cast<size_t>(b)] = juce::jlimit(0.0f, 1.0f, (db + 100.0f) / 100.0f);
                }
                frames.push_back(std::move(bands));
            }
        }
        return std::make_pair(std::move(frames), nyquist);
    }, [path](WaveformEditorPanel& self, auto&& fetched) {
        auto& [frames, nyquist] = fetched;
        if (path != self.sourcePath_) return; // source changed meanwhile
        self.spectroFrames_ = std::move(frames);
        self.spectroMaxHz_ = nyquist;
        self.spectroImage_ = {};
        self.repaint();
    }, [](WaveformEditorPanel& self) { self.spectroPending_ = false; });
}

juce::String WaveformEditorPanel::annotationKey() const
{
    return "annotations:" + juce::String::toHexString(sourcePath_.hashCode64());
}

void WaveformEditorPanel::saveAnnotations()
{
    if (sourcePath_.isEmpty()) return;
    controller_.sessions().sessionManager().setUserPreference(annotationKey().toStdString(),
                                                   juce::JSON::toString(state_.annotationsToVar(), true).toStdString());
}

void WaveformEditorPanel::loadAnnotations()
{
    state_.annotationsFromVar({});
    if (sourcePath_.isEmpty()) return;
    auto pref = controller_.sessions().sessionManager().getUserPreference(annotationKey().toStdString());
    if (pref.is_ok() && !pref.value().empty()) state_.annotationsFromVar(juce::JSON::parse(juce::String(pref.value())));
}

void WaveformEditorPanel::promptForAnnotation(int editId)
{
    if (state_.duration() <= 0.0) return;
    juce::String existing;
    for (const auto& a : state_.annotations())
        if (a.id == editId) existing = a.text;
    noteDialog_ = std::make_unique<juce::AlertWindow>(editId < 0 ? "Add note" : "Edit note",
                                                      state_.hasSelection() || editId >= 0
                                                          ? "Note for this range:"
                                                          : "Note at " + formatTime(state_.playhead()) + ":",
                                                      juce::MessageBoxIconType::NoIcon, this);
    noteDialog_->addTextEditor("text", existing);
    noteDialog_->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    if (editId >= 0) noteDialog_->addButton("Delete", 2);
    noteDialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<WaveformEditorPanel> self(this);
    noteDialog_->enterModalState(true, juce::ModalCallbackFunction::create([self, editId](int result) {
                                     if (self == nullptr || self->noteDialog_ == nullptr) return;
                                     const auto text = self->noteDialog_->getTextEditorContents("text");
                                     if (result == 1 || result == 2) {
                                         if (editId >= 0) {
                                             // Edit = replace in place over the same range.
                                             for (const auto& a : self->state_.annotations())
                                                 if (a.id == editId) {
                                                     const auto keepSelStart = self->state_.selectionStart();
                                                     const auto keepSelEnd = self->state_.selectionEnd();
                                                     const auto keepPlayhead = self->state_.playhead();
                                                     const auto start = a.startSec, end = a.endSec;
                                                     self->state_.removeAnnotation(editId);
                                                     if (result == 1) {
                                                         self->state_.setSelection(start, end);
                                                         self->state_.setPlayhead(start);
                                                         self->state_.addAnnotation(text);
                                                     }
                                                     self->state_.setSelection(keepSelStart, keepSelEnd);
                                                     self->state_.setPlayhead(keepPlayhead);
                                                     break;
                                                 }
                                         } else {
                                             self->state_.addAnnotation(text);
                                         }
                                         self->saveAnnotations();
                                     }
                                     self->noteDialog_.reset();
                                     self->repaint();
                                 }));
}

void WaveformEditorPanel::seek(double t)
{
    state_.setPlayhead(t);
    controller_.playback().setPlaybackPosition(state_.playhead());
    updateStatus();
    repaint();
}

void WaveformEditorPanel::explainSelection()
{
    if (sourcePath_.isEmpty()) return;
    if (!state_.hasSelection() || state_.selectionEnd() - state_.selectionStart() < 1.0) {
        statusLabel_.setText(TRANS("Select at least one second of audio to explain."), juce::dontSendNotification);
        return;
    }
    auto view = std::make_unique<ExplainMomentView>(controller_, sourcePath_, state_.selectionStart(),
                                                    state_.selectionEnd());
    juce::Component::SafePointer<WaveformEditorPanel> self(this);
    view->onJumpTo = [self](double s, double e) {
        if (self == nullptr) return;
        self->state_.setSelection(s, e);
        self->seek(s);
    };
    juce::CallOutBox::launchAsynchronously(std::move(view), explainButton_.getScreenBounds(), nullptr);
}

void WaveformEditorPanel::updateStatus()
{
    juce::String s = formatTime(state_.playhead()) + " / " + formatTime(state_.duration());
    if (state_.hasSelection())
        s << "   sel " << formatTime(state_.selectionStart()) << "-" << formatTime(state_.selectionEnd()) << " ("
          << juce::String(state_.selectionEnd() - state_.selectionStart(), 2) << " s)";
    s << "   view x" << juce::String(state_.viewLength() > 0 ? state_.duration() / state_.viewLength() : 1.0, 1);
    const auto area = mainArea();
    if (showSpectrogram_ && hoverY_ >= area.getY() && hoverY_ <= area.getBottom() && area.getHeight() > 0) {
        const double frac = (area.getBottom() - hoverY_) / area.getHeight();
        const double hz = 30.0 * std::pow(spectroMaxHz_ / 30.0, frac);
        s << "   "
          << (hz >= 1000.0 ? juce::String(hz / 1000.0, 2) + " kHz" : juce::String(juce::roundToInt(hz)) + " Hz");
    }
    statusLabel_.setText(s, juce::dontSendNotification);
}

void WaveformEditorPanel::onVBlank(double timestampSec)
{
    if (!isShowing()) return;
    // Source / peak polling stays at ~30 Hz (it was the old timer's rate);
    // only the playhead runs at the display's refresh rate.
    if (lastSourceSyncSec_ < 0.0 || timestampSec - lastSourceSyncSec_ >= 1.0 / 30.0) {
        lastSourceSyncSec_ = timestampSec;
        syncSource();
    }
    if (!controller_.playback().isPlaying() || drag_ != Drag::None) return;

    const auto area = mainArea();
    const float w = area.getWidth();
    const float oldX = area.getX() + state_.xForTime(state_.playhead(), w);
    state_.setPlayhead(controller_.playback().getPlaybackPosition());
    const bool scrolled = state_.followPlayhead();
    const float newX = area.getX() + state_.xForTime(state_.playhead(), w);
    if (scrolled) {
        updateStatus();
        repaint(); // the whole view moved
        return;
    }
    if (juce::roundToInt(oldX) == juce::roundToInt(newX)) return; // same pixel column
    updateStatus();
    // Dirty rects: the 1.5 px line's old and new columns (padded for AA).
    const auto strip = [&area](float x) {
        return juce::Rectangle<float>(x - 3.0f, area.getY(), 6.0f, area.getHeight()).getSmallestIntegerContainer();
    };
    repaint(strip(oldX));
    repaint(strip(newX));
}

void WaveformEditorPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    const auto area = mainArea();
    if (area.getHeight() < 30.0f) return;
    if (state_.duration() <= 0.0) {
        drawEmptyState(g, "No audio loaded", "Load a session to inspect, select, and annotate its audio.",
                       kToolbarH + kStatusH + 4);
        return;
    }
    const float w = area.getWidth();
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRect(area);

    if (showSpectrogram_ && !spectroFrames_.empty()) {
        // Crop the whole-file frames to the visible window, then rasterize.
        const auto n = spectroFrames_.size();
        const auto f0 = static_cast<size_t>(
            juce::jlimit(0.0, static_cast<double>(n - 1), state_.viewStart() / state_.duration() * n));
        const auto f1 = static_cast<size_t>(juce::jlimit(static_cast<double>(f0 + 1), static_cast<double>(n),
                                                         state_.viewEnd() / state_.duration() * n));
        std::vector<std::vector<float>> visible(spectroFrames_.begin() + static_cast<std::ptrdiff_t>(f0),
                                                spectroFrames_.begin() + static_cast<std::ptrdiff_t>(f1));
        const auto img = buildSpectrogramImage(visible, juce::jmax(1, static_cast<int>(w)),
                                               juce::jmax(1, static_cast<int>(area.getHeight())));
        g.drawImage(img, area);
    } else if (showSpectrogram_) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(spectroPending_ ? "Computing spectrogram..." : "No spectrogram", area, juce::Justification::centred);
    } else if (!peaks_.empty()) {
        const float mid = area.getCentreY();
        const float half = area.getHeight() * 0.48f;
        g.setColour(KhensuTheme::accentBlue.withAlpha(0.8f));
        const auto n = peaks_.size();
        for (int x = 0; x < static_cast<int>(w); ++x) {
            const double t0 = state_.timeForX(static_cast<float>(x), w);
            const double t1 = state_.timeForX(static_cast<float>(x + 1), w);
            auto i0 = static_cast<size_t>(t0 / state_.duration() * n);
            auto i1 = juce::jmax(i0 + 1, static_cast<size_t>(t1 / state_.duration() * n));
            float p = 0.0f;
            for (auto i = i0; i < i1 && i < n; ++i)
                p = juce::jmax(p, peaks_[i]);
            const float h = juce::jmin(half, p * half);
            g.drawVerticalLine(juce::roundToInt(area.getX()) + x, mid - h, mid + h + 1.0f);
        }
    }

    // Selection.
    if (state_.hasSelection()) {
        const float x0 = area.getX() + state_.xForTime(state_.selectionStart(), w);
        const float x1 = area.getX() + state_.xForTime(state_.selectionEnd(), w);
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.18f));
        g.fillRect(juce::Rectangle<float>(x0, area.getY(), x1 - x0, area.getHeight()).getIntersection(area));
    }

    // Annotations: bands + tags along the top.
    g.setFont(KhensuTheme::labelFont(type::caption));
    for (const auto& a : state_.annotations()) {
        if (a.endSec < state_.viewStart() || a.startSec > state_.viewEnd()) continue;
        const float x0 = area.getX() + state_.xForTime(a.startSec, w);
        const float x1 = juce::jmax(x0 + 2.0f, area.getX() + state_.xForTime(a.endSec, w));
        g.setColour(KhensuTheme::accentAmber.withAlpha(0.12f));
        g.fillRect(juce::Rectangle<float>(x0, area.getY(), x1 - x0, area.getHeight()).getIntersection(area));
        g.setColour(KhensuTheme::accentAmber);
        g.drawVerticalLine(juce::roundToInt(x0), area.getY(), area.getBottom());
        const auto tag = juce::Rectangle<float>(x0 + 2.0f, area.getY() + 2.0f,
                                                juce::jmin(160.0f, area.getRight() - x0 - 4.0f), 13.0f);
        if (tag.getWidth() > 10.0f) {
            g.setColour(KhensuTheme::bgOverlay.withAlpha(0.85f));
            g.fillRoundedRectangle(tag, 3.0f);
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(a.text, tag.reduced(3.0f, 0.0f), juce::Justification::centredLeft, true);
        }
    }

    // Playhead.
    const float px = area.getX() + state_.xForTime(state_.playhead(), w);
    if (px >= area.getX() && px <= area.getRight()) {
        g.setColour(KhensuTheme::textPrimary);
        g.drawLine(px, area.getY(), px, area.getBottom(), 1.5f);
    }
    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing.withAlpha(0.7f));
        g.drawRect(area, 1.0f);
    }

    // Overview strip with the current view window.
    const auto ov = overviewArea();
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.6f));
    g.fillRect(ov);
    if (!peaks_.empty()) {
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.6f));
        const auto n = peaks_.size();
        for (int x = 0; x < static_cast<int>(ov.getWidth()); ++x) {
            const auto i = static_cast<size_t>(static_cast<double>(x) / ov.getWidth() * n);
            const float h = juce::jmin(1.0f, peaks_[juce::jmin(i, n - 1)]) * ov.getHeight() * 0.45f;
            g.drawVerticalLine(juce::roundToInt(ov.getX()) + x, ov.getCentreY() - h, ov.getCentreY() + h);
        }
    }
    const float vx0 = ov.getX() + static_cast<float>(state_.viewStart() / state_.duration()) * ov.getWidth();
    const float vx1 = ov.getX() + static_cast<float>(state_.viewEnd() / state_.duration()) * ov.getWidth();
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.25f));
    g.fillRect(vx0, ov.getY(), juce::jmax(2.0f, vx1 - vx0), ov.getHeight());
    g.setColour(KhensuTheme::accentTeal);
    g.drawRect(vx0, ov.getY(), juce::jmax(2.0f, vx1 - vx0), ov.getHeight(), 1.0f);
}

void WaveformEditorPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto bar = b.removeFromTop(kToolbarH);
    spectroButton_.setBounds(bar.removeFromLeft(100).reduced(1));
    bar.removeFromLeft(8);
    zoomInButton_.setBounds(bar.removeFromLeft(30).reduced(1));
    zoomOutButton_.setBounds(bar.removeFromLeft(30).reduced(1));
    fitButton_.setBounds(bar.removeFromLeft(44).reduced(1));
    zoomSelButton_.setBounds(bar.removeFromLeft(130).reduced(1));
    bar.removeFromLeft(8);
    noteButton_.setBounds(bar.removeFromLeft(90).reduced(1));
    explainButton_.setBounds(bar.removeFromLeft(70).reduced(1));
    b.removeFromTop(2);
    statusLabel_.setBounds(b.removeFromTop(kStatusH));
    // CE-COL-1: the comments lane sits between the main view and the overview.
    {
        auto lane = bodyArea().toFloat().reduced(2.0f);
        lane.removeFromBottom(kOverviewH + 2.0f);
        commentsLane_.setBounds(lane.removeFromBottom(kCommentsLaneH).toNearestInt());
    }
}

void WaveformEditorPanel::mouseDown(const juce::MouseEvent& e)
{
    const auto area = mainArea();
    if (state_.duration() <= 0.0) {
        PanelCard::mouseDown(e);
        return;
    }
    if (overviewArea().contains(e.position)) {
        grabKeyboardFocus();
        drag_ = Drag::Overview;
        const auto ov = overviewArea();
        state_.setViewStart((e.position.x - ov.getX()) / ov.getWidth() * state_.duration() - state_.viewLength() / 2);
        repaint();
        return;
    }
    if (!area.contains(e.position)) {
        PanelCard::mouseDown(e);
        return;
    }
    grabKeyboardFocus();
    const double t = state_.timeForX(e.position.x - area.getX(), area.getWidth());
    if (e.mods.isShiftDown()) {
        const double anchor = state_.hasSelection()
                                  ? (std::abs(t - state_.selectionStart()) < std::abs(t - state_.selectionEnd())
                                         ? state_.selectionEnd()
                                         : state_.selectionStart())
                                  : state_.playhead();
        state_.setSelection(anchor, t);
        dragAnchor_ = anchor;
    } else {
        state_.clearSelection();
        dragAnchor_ = t;
        seek(t);
    }
    drag_ = Drag::Select;
    updateStatus();
    repaint();
}

void WaveformEditorPanel::mouseDrag(const juce::MouseEvent& e)
{
    if (drag_ == Drag::Overview) {
        const auto ov = overviewArea();
        state_.setViewStart((e.position.x - ov.getX()) / ov.getWidth() * state_.duration() - state_.viewLength() / 2);
        repaint();
        return;
    }
    if (drag_ != Drag::Select) {
        PanelCard::mouseDrag(e);
        return;
    }
    const auto area = mainArea();
    const double t = state_.timeForX(juce::jlimit(0.0f, area.getWidth(), e.position.x - area.getX()), area.getWidth());
    state_.setSelection(dragAnchor_, t);
    updateStatus();
    repaint();
}

void WaveformEditorPanel::mouseUp(const juce::MouseEvent&)
{
    // A click without a real drag leaves a zero-length selection: clear it.
    if (drag_ == Drag::Select && state_.selectionEnd() - state_.selectionStart() < 0.01) state_.clearSelection();
    drag_ = Drag::None;
    updateStatus();
    repaint();
}

void WaveformEditorPanel::mouseMove(const juce::MouseEvent& e)
{
    hoverX_ = e.position.x;
    hoverY_ = e.position.y;
    if (showSpectrogram_) updateStatus();
}

void WaveformEditorPanel::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto area = mainArea();
    if (!area.contains(e.position) || state_.duration() <= 0.0) return;
    const double t = state_.timeForX(e.position.x - area.getX(), area.getWidth());
    const double tolerance = state_.viewLength() / juce::jmax(1.0f, area.getWidth()) * 4.0;
    const int id = state_.annotationAt(t, tolerance);
    if (id >= 0) promptForAnnotation(id);
}

void WaveformEditorPanel::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const auto area = mainArea();
    if (!area.contains(e.position) || state_.duration() <= 0.0) {
        PanelCard::mouseWheelMove(e, w);
        return;
    }
    if (e.mods.isCommandDown()) {
        const double anchor = state_.timeForX(e.position.x - area.getX(), area.getWidth());
        state_.zoomAround(anchor, w.deltaY > 0 ? 1.25 : 0.8);
    } else {
        const float delta = std::abs(w.deltaX) > std::abs(w.deltaY) ? w.deltaX : w.deltaY;
        state_.scrollBy(-static_cast<double>(delta) * state_.viewLength() * 0.5);
    }
    updateStatus();
    repaint();
}

bool WaveformEditorPanel::keyPressed(const juce::KeyPress& key)
{
    if (state_.duration() <= 0.0) return false;
    const auto ch = key.getTextCharacter();
    const bool shift = key.getModifiers().isShiftDown();
    if (key == juce::KeyPress::spaceKey) {
        if (controller_.playback().isPlaying())
            controller_.playback().pauseAudio();
        else
            controller_.playback().playAudio();
    } else if (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey) {
        const double step = key.getKeyCode() == juce::KeyPress::leftKey ? -1.0 : 1.0;
        const double from = state_.playhead();
        if (shift) {
            const double anchor = state_.hasSelection()
                                      ? (std::abs(from - state_.selectionStart()) < 1e-6 ? state_.selectionEnd()
                                                                                         : state_.selectionStart())
                                      : from;
            seek(from + step);
            state_.setSelection(anchor, state_.playhead());
        } else {
            seek(from + step);
        }
        state_.followPlayhead();
    } else if (ch == '+' || ch == '=') {
        state_.zoomAround(state_.playhead(), 2.0);
    } else if (ch == '-') {
        state_.zoomAround(state_.playhead(), 0.5);
    } else if (ch == '0') {
        state_.zoomToFit();
    } else if (ch == 'z' || ch == 'Z') {
        if (state_.hasSelection()) state_.zoomTo(state_.selectionStart(), state_.selectionEnd());
    } else if (ch == 'e' || ch == 'E') {
        explainSelection();
    } else if (ch == 'n' || ch == 'N') {
        promptForAnnotation(-1);
    } else if (ch == 's' || ch == 'S') {
        spectroButton_.setToggleState(!spectroButton_.getToggleState(), juce::sendNotificationSync);
    } else if (ch == '[') {
        seek(state_.previousAnnotationBefore(state_.playhead()));
        state_.followPlayhead();
    } else if (ch == ']') {
        seek(state_.nextAnnotationAfter(state_.playhead()));
        state_.followPlayhead();
    } else if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) {
        const int id = state_.annotationAt(state_.playhead(), 0.05);
        if (id < 0 || !state_.removeAnnotation(id)) return true;
        saveAnnotations();
    } else if (key == juce::KeyPress::escapeKey) {
        state_.clearSelection();
    } else {
        return false;
    }
    updateStatus();
    repaint();
    return true;
}

} // namespace khensu
