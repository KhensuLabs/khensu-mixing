// desktop-app/Source/ui/Dashboard/SubBassPhasePanel.cpp
#include "SubBassPhasePanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <algorithm>
#include "../shared/PanelRunState.h"
#include <cmath>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void SubBassPhasePanel::applyThemeColours()
{
}

SubBassPhasePanel::SubBassPhasePanel(IAppController& c) : PanelCard(c, "Sub-Bass Phase")
{
    applyThemeColours();
    setExplainer(TRANS("Sub-Bass Phase"),
                 TRANS("The phase relationship between the kick and the bass fundamental in the "
                 "sub/low band (< 120 Hz). When they're in phase the low end sums and hits "
                 "hard; out of phase, it partially cancels on mono, club, and phone systems "
                 "- the classic \"the bass disappears on the big rig\" problem.\n\n"
                 "The goniometer plots kick (x) against bass (y): a cloud along the rising "
                 "diagonal is in phase, along the falling diagonal is out of phase, a round "
                 "blob is uncorrelated. The strip below tracks their correlation over time. "
                 "Kick is approximated by low-passing the drums stem; requires separated stems."),
                 "phase_correlation");

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Requires separated stems (auto-runs once they exist)."));
}

juce::Colour SubBassPhasePanel::phaseColour(float correlation)
{
    if (correlation >= 0.25f) return KhensuTheme::accentTeal;
    if (correlation <= -0.25f) return KhensuTheme::accentRed;
    return KhensuTheme::accentAmber;
}

void SubBassPhasePanel::runAnalysis()
{
    if (!controller_.stems().hasStemsAvailable()) {
        run_.setIdle(TRANS("Requires separated stems - run Isolate Stems first."));
        return;
    }
    const juce::String drums = controller_.getStemFile("drums").getFullPathName();
    const juce::String bass = controller_.getStemFile("bass").getFullPathName();
    if (drums.isEmpty() || bass.isEmpty()) {
        run_.setIdle(TRANS("Drums and bass stems are required."));
        return;
    }

    fetchThenPublish(
        *this, controller_, run_, TRANS("Analyzing kick vs bass phase..."),
        [drums, bass] { return ipc::HttpServiceBridge{}.analyzeSubBassPhase(drums.toStdString(), bass.toStdString()); },
        [](SubBassPhasePanel& self, const ipc::HttpServiceBridge::SubBassPhaseResult& v) -> juce::String {
            self.correlation_ = v.correlation;
            self.phaseDeg_ = v.phase_degrees;
            self.verdict_ = juce::String(v.verdict);
            self.gonio_.clear();
            self.gonio_.reserve(v.goniometer.size());
            for (const auto& pr : v.goniometer)
                self.gonio_.push_back({pr.first, pr.second});
            self.history_.clear();
            self.history_.reserve(v.correlation_over_time.size());
            for (const auto& p : v.correlation_over_time)
                self.history_.push_back({p.time_sec, p.correlation});
            return TRANS("corr ") + juce::String(self.correlation_, 2) + "   phase " +
                   juce::String((int)std::lround(self.phaseDeg_)) + juce::String(juce::CharPointer_UTF8("\xc2\xb0"));
        });
}

void SubBassPhasePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) return;

    auto b = bodyArea();
    b.removeFromTop(28); // controls row

    // Verdict line, coloured by alignment.
    auto verdictRow = b.removeFromTop(20);
    g.setColour(phaseColour(correlation_));
    g.setFont(KhensuTheme::labelFont(11.0f).boldened());
    g.drawText(verdict_, verdictRow, juce::Justification::centredLeft, false);
    b.removeFromTop(4);

    // Goniometer (square, left) + correlation history (right).
    const int gonioSize = juce::jmin(b.getHeight(), b.getWidth() / 2);
    auto gonioArea = b.removeFromLeft(gonioSize);
    b.removeFromLeft(8);
    drawGoniometer(g, gonioArea);
    drawHistory(g, b);
}

void SubBassPhasePanel::drawGoniometer(juce::Graphics& g, juce::Rectangle<int> area)
{
    auto sq = area.toFloat();
    const float cx = sq.getCentreX(), cy = sq.getCentreY();
    const float half = juce::jmin(sq.getWidth(), sq.getHeight()) * 0.5f - 2.0f;

    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRoundedRectangle(sq, 4.0f);

    // In-phase (rising) and out-of-phase (falling) guide diagonals.
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.25f));
    g.drawLine(cx - half, cy + half, cx + half, cy - half, 1.0f); // y = x
    g.setColour(KhensuTheme::accentRed.withAlpha(0.22f));
    g.drawLine(cx - half, cy - half, cx + half, cy + half, 1.0f); // y = -x
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.18f));
    g.drawLine(cx - half, cy, cx + half, cy, 1.0f);
    g.drawLine(cx, cy - half, cx, cy + half, 1.0f);

    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(7.5f));
    g.drawText("kick", (int)(cx + half - 24), (int)(cy + 2), 24, 9, juce::Justification::centredRight, false);
    g.drawText("bass", (int)(cx + 2), (int)(cy - half), 24, 9, juce::Justification::centredLeft, false);

    const juce::Colour pc = phaseColour(correlation_);
    for (const auto& p : gonio_) {
        const float px = cx + juce::jlimit(-1.0f, 1.0f, p.x) * half;
        const float py = cy - juce::jlimit(-1.0f, 1.0f, p.y) * half;
        g.setColour(pc.withAlpha(0.5f));
        g.fillEllipse(px - 1.0f, py - 1.0f, 2.0f, 2.0f);
    }
}

void SubBassPhasePanel::drawHistory(juce::Graphics& g, juce::Rectangle<int> area)
{
    auto r = area.toFloat();
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.4f));
    g.fillRoundedRectangle(r, 4.0f);

    g.setColour(KhensuTheme::textTertiary.withAlpha(0.5f));
    g.setFont(KhensuTheme::labelFont(7.5f));
    g.drawText("correlation over time", (int)r.getX() + 3, (int)r.getY() + 1, (int)r.getWidth() - 6, 9,
               juce::Justification::centredLeft, false);

    auto plot = r.reduced(3.0f).withTrimmedTop(10.0f);
    const float midY = plot.getCentreY();
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.25f));
    g.drawHorizontalLine(juce::roundToInt(midY), plot.getX(), plot.getRight()); // corr = 0

    if (history_.size() < 2) return;
    const float tMax = history_.back().x > 0.0f ? history_.back().x : 1.0f;
    auto xFor = [&](float t) { return plot.getX() + juce::jlimit(0.0f, 1.0f, t / tMax) * plot.getWidth(); };
    auto yFor = [&](float c) { return midY - juce::jlimit(-1.0f, 1.0f, c) * (plot.getHeight() * 0.5f); };

    juce::Path path;
    hover_.clear();
    for (size_t i = 0; i < history_.size(); ++i) {
        const float x = xFor(history_[i].x), y = yFor(history_[i].y);
        {
            const float w = plot.getWidth() / (float)std::max<size_t>(1, history_.size() - 1);
            hover_.addColumn(x - w * 0.5f, x + w * 0.5f, plot,
                             juce::String(history_[i].x, 1) + " s: " + TRANS("kick / bass correlation") + " "
                                 + juce::String(history_[i].y, 2));
        }
        if (i == 0)
            path.startNewSubPath(x, y);
        else
            path.lineTo(x, y);
    }
    g.setColour(phaseColour(correlation_));
    g.strokePath(path, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved));
}

void SubBassPhasePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);

    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
