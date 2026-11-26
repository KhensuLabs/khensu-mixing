// desktop-app/Source/ui/VocalWorkspace/LyricIntelligibilityPanel.cpp
#include "LyricIntelligibilityPanel.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu {

void LyricIntelligibilityPanel::WordRow::paint(juce::Graphics& g)
{
    if (likelyMasked) {
        g.setColour(KhensuTheme::accentAmber.withAlpha(0.12f));
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 3.0f);
    }

    auto b = getLocalBounds().reduced(6, 2);
    const int min = (int)startSec / 60, sec = (int)startSec % 60;
    juce::String timeStr = juce::String(min) + ":" + juce::String(sec).paddedLeft('0', 2);

    g.setFont(KhensuTheme::monoFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(timeStr, b.removeFromLeft(46), juce::Justification::centredLeft, false);

    g.setFont(khensu::KhensuTheme::uiFont(khensu::type::caption, likelyMasked ? juce::Font::bold : juce::Font::plain));
    g.setColour(likelyMasked ? KhensuTheme::accentAmber : KhensuTheme::textSecondary);
    g.drawText(word, b.removeFromLeft(b.getWidth() - 60), juce::Justification::centredLeft, false);

    g.setFont(KhensuTheme::monoFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText(juce::String(maskingRatio, 2) + "x", b, juce::Justification::centredRight, false);
}

void LyricIntelligibilityPanel::HeatStrip::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setColour(KhensuTheme::bgSurface);
    g.fillRoundedRectangle(b, 3.0f);
    if (segs.empty() || duration <= 0.0f) {
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("INTELLIGIBILITY MAP", b.toNearestInt(), juce::Justification::centred, false);
        return;
    }
    for (const auto& s : segs) {
        const float x0 = b.getWidth() * (s.start / duration);
        const float x1 = b.getWidth() * (juce::jmax(s.end, s.start + 0.15f) / duration);
        // ratio < 1 = below the track's average presence (buried); scale so
        // 1.0 -> healthy teal, 0.5 -> amber, near 0 -> red.
        const float health = juce::jlimit(0.0f, 1.0f, s.ratio);
        auto col = s.masked ? KhensuTheme::accentAmber.interpolatedWith(KhensuTheme::accentRed, 1.0f - health)
                            : KhensuTheme::accentTeal;
        g.setColour(col.withAlpha(s.masked ? 0.95f : 0.55f));
        g.fillRect(b.getX() + x0, b.getY() + 1.0f, juce::jmax(1.5f, x1 - x0), b.getHeight() - 2.0f);
    }
}

// Every cached palette colour on this component, in one place (UI-01).
void LyricIntelligibilityPanel::applyThemeColours()
{
    analyzeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    analyzeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

LyricIntelligibilityPanel::LyricIntelligibilityPanel(IAppController& c) : PanelCard(c, "Lyric Intelligibility")
{
    applyThemeColours();
    setExplainer(TRANS("Lyric Intelligibility"),
                 TRANS("Transcribes the vocal (word-level timestamps) and checks the mix's vocal-presence-band "
                 "(1-5 kHz) energy at each word against the track's own average - flagging lyric moments "
                 "where something else in the arrangement is dominating that frequency range right when "
                 "the word lands, so it's likely to read as buried/unintelligible. Requires the optional "
                 "faster-whisper dependency server-side; click a flagged word to seek there."));

    run_.setCanRun(false);
    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);

    run_.label().setFont(KhensuTheme::bodyFont(type::caption));
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Load audio, then click Analyze Lyrics."));

    wordsViewport_.setViewedComponent(&wordsContent_, false);
    addAndMakeVisible(wordsViewport_);

    heatStrip_.onSeek = [this](float sec) { controller_.playback().setPlaybackPosition(sec); };
    addAndMakeVisible(heatStrip_);
    run_.errorState().toFront(false); // over the word list it replaces
}

void LyricIntelligibilityPanel::setFilePath(const juce::String& path)
{
    if (path == filePath_) return;
    filePath_ = path;
    run_.setCanRun(filePath_.isNotEmpty());
    rows_.clear();
    wordsContent_.removeAllChildren();
    resized();

    // Auto-runs now (previously required a manual click): /analyze/lyrics
    // already fails soft with a clear "Analysis failed" message when the
    // optional [asr] extra isn't installed (503) or transcription errors,
    // so there's no silent-surprise-cost concern left, same reasoning as
    // StemMaskingPanel's masking auto-run. The button stays for re-running
    // after a transcript comes back empty/wrong.
    if (filePath_.isNotEmpty()) {
        run_.setIdle(TRANS("Ready. Analyzing lyrics..."));
        runAnalysis();
    } else {
        run_.setIdle(TRANS("Load audio first."));
    }
}

void LyricIntelligibilityPanel::runAnalysis()
{
    if (filePath_.isEmpty()) return;
    const juce::String path = filePath_;
    FetchOptions options;
    options.longPool = true; // transcription (and a first-run model download) takes minutes
    fetchThenPublish(
        *this, controller_, run_, TRANS("Transcribing... (first run may download the model, can take a minute)"),
        [path] { return ipc::HttpServiceBridge{}.analyzeLyrics(path.toStdString()); },
        [](LyricIntelligibilityPanel& self, const AnalysisResult::LyricIntelligibilityData& data) -> juce::String {
            self.rows_.clear();
            self.wordsContent_.removeAllChildren();
            self.heatStrip_.segs.clear();
            float lastEnd = 0.0f;
            for (const auto& w : data.words) {
                HeatStrip::Seg seg;
                seg.start = w.start_sec;
                seg.end = w.end_sec;
                seg.ratio = w.masking_ratio;
                seg.masked = w.likely_masked;
                self.heatStrip_.segs.push_back(seg);
                lastEnd = juce::jmax(lastEnd, w.end_sec);
            }
            self.heatStrip_.duration = lastEnd + 1.0f;
            self.heatStrip_.repaint();
            for (const auto& w : data.words) {
                auto row = std::make_unique<WordRow>();
                row->word = juce::String(w.word);
                row->startSec = w.start_sec;
                row->maskingRatio = w.masking_ratio;
                row->likelyMasked = w.likely_masked;
                const float seekTo = w.start_sec;
                // The rows are owned by the panel, so they never outlive it.
                row->onClicked = [panel = &self, seekTo] { panel->controller_.playback().setPlaybackPosition(seekTo); };
                self.wordsContent_.addAndMakeVisible(*row);
                self.rows_.push_back(std::move(row));
            }
            if (data.words.empty()) return TRANS("No lyrics detected.");
            return TRANS("%f of %n words flagged as likely masked.")
                .replace("%f", juce::String(data.flagged_count))
                .replace("%n", juce::String((int)data.words.size()));
        },
        std::move(options));
}

void LyricIntelligibilityPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    auto topRow = b.removeFromTop(26);
    analyzeButton_.setBounds(topRow.removeFromLeft(130));
    topRow.removeFromLeft(8);
    run_.label().setBounds(topRow);
    b.removeFromTop(6);
    run_.errorState().setBounds(b);

    heatStrip_.setBounds(b.removeFromTop(18));
    b.removeFromTop(4);

    wordsViewport_.setBounds(b);
    constexpr int kRowH = 22;
    wordsContent_.setSize(b.getWidth() - 8, juce::jmax((int)rows_.size() * kRowH, b.getHeight()));
    int y = 0;
    for (auto& row : rows_) {
        row->setBounds(0, y, wordsContent_.getWidth(), kRowH);
        y += kRowH;
    }
}

} // namespace khensu
