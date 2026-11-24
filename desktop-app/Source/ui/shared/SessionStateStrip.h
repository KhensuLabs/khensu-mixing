// desktop-app/Source/ui/shared/SessionStateStrip.h
// VIZ-P1 / VIZ-U1: the top strip says what state the session is in - which
// version is current, what is still open, what the last fix check found, the
// delivery target and whether the analysis is current. It carries no
// measurements: loudness, crest and correlation live once each - analysed
// values in the Analysis tiles, live values in the transport's meters (the
// old strip repeated both).
// Clicking a figure goes where it is acted on (onFigureClicked).
// Drawn live in paint(), so a theme switch re-tints it.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <array>
#include <functional>
#include "KhensuTheme.h"

namespace khensu {

class SessionStateStrip final : public juce::Component {
  public:
    enum Index { kVersion = 0, kIssues, kFixCheck, kTarget, kAnalysis, kNumFigures };

    /// How a figure's dot is coloured: none, or a severity / a good outcome.
    enum class Tone { Neutral, Good, Minor, Moderate, Severe };

    struct Figure {
        juce::String label;
        juce::String value; ///< empty = no reading ("-")
        Tone tone = Tone::Neutral;
    };

    /// Everything the strip reads, from the controller (pure input).
    struct State {
        bool hasSession = false;
        int versionNode = -1; ///< the render tree's current node, -1 = the original
        int versionCount = 0; ///< renders in the tree
        int severe = 0, moderate = 0, minor = 0; ///< open issues by severity
        int resolved = 0, improved = 0, worse = 0; ///< the last fix check, per issue
        bool checked = false;  ///< a fix check has run on the current version
        juce::String target;   ///< "Streaming, -14 LUFS" / genre name
        enum class Analysis { None, Analysing, Ready, OutOfDate, Failed } analysis = Analysis::None;
    };

    SessionStateStrip()
    {
        setTitle(TRANS("Session state"));
        setState({});
    }

    std::function<void(int index)> onFigureClicked;

    void setState(const State& s)
    {
        figures_ = figuresFor(s);
        repaint();
    }

    const std::array<Figure, kNumFigures>& figures() const noexcept { return figures_; }

    /// The figures for a state (pure; tested).
    static std::array<Figure, kNumFigures> figuresFor(const State& s)
    {
        std::array<Figure, kNumFigures> f;
        f[kVersion].label = TRANS("Version");
        f[kIssues].label = TRANS("Open");
        f[kFixCheck].label = TRANS("Fix check");
        f[kTarget].label = TRANS("Target");
        f[kAnalysis].label = TRANS("Analysis");
        if (!s.hasSession) return f;

        f[kVersion].value = s.versionNode < 0 ? TRANS("Original")
                                              : TRANS("Version") + " " + juce::String(s.versionNode + 1)
                                                    + (s.versionCount > 1 ? " " + TRANS("of") + " " + juce::String(s.versionCount)
                                                                          : juce::String());

        juce::StringArray open;
        if (s.severe > 0) open.add(juce::String(s.severe) + " " + TRANS("severe"));
        if (s.moderate > 0) open.add(juce::String(s.moderate) + " " + TRANS("moderate"));
        if (s.minor > 0) open.add(juce::String(s.minor) + " " + TRANS("minor"));
        f[kIssues].value = open.isEmpty() ? (s.analysis == State::Analysis::Ready ? TRANS("Nothing open") : juce::String())
                                          : open.joinIntoString(", ");
        f[kIssues].tone = s.severe > 0 ? Tone::Severe : s.moderate > 0 ? Tone::Moderate : s.minor > 0 ? Tone::Minor
                                                                                                       : Tone::Neutral;

        if (s.checked) {
            juce::StringArray check;
            if (s.resolved > 0) check.add(juce::String(s.resolved) + " " + TRANS("resolved"));
            if (s.improved > 0) check.add(juce::String(s.improved) + " " + TRANS("better"));
            if (s.worse > 0) check.add(juce::String(s.worse) + " " + TRANS("worse"));
            f[kFixCheck].value = check.isEmpty() ? TRANS("No change") : check.joinIntoString(", ");
            f[kFixCheck].tone = s.worse > 0 ? Tone::Moderate : (s.resolved + s.improved) > 0 ? Tone::Good : Tone::Neutral;
        }

        f[kTarget].value = s.target;

        switch (s.analysis) {
        case State::Analysis::None: break;
        case State::Analysis::Analysing: f[kAnalysis].value = TRANS("Analysing"); break;
        case State::Analysis::Ready: f[kAnalysis].value = TRANS("Current"); f[kAnalysis].tone = Tone::Good; break;
        case State::Analysis::OutOfDate: f[kAnalysis].value = TRANS("Out of date"); f[kAnalysis].tone = Tone::Moderate; break;
        case State::Analysis::Failed: f[kAnalysis].value = TRANS("Failed"); f[kAnalysis].tone = Tone::Severe; break;
        }
        return f;
    }

    static juce::Colour toneColour(Tone t)
    {
        switch (t) {
        case Tone::Good: return KhensuTheme::accentGreen;
        case Tone::Minor: return KhensuTheme::accentBlue;
        case Tone::Moderate: return KhensuTheme::accentAmber;
        case Tone::Severe: return KhensuTheme::accentRed;
        case Tone::Neutral: break;
        }
        return juce::Colours::transparentBlack;
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto cells = layout();
        for (int i = 0; i < kNumFigures; ++i)
            if (cells[(size_t)i].contains(e.getPosition())) {
                if (onFigureClicked) onFigureClicked(i);
                return;
            }
    }
    juce::MouseCursor getMouseCursor() override { return juce::MouseCursor::PointingHandCursor; }

    void paint(juce::Graphics& g) override
    {
        g.setColour(KhensuTheme::bgBase);
        g.fillAll();
        g.setColour(KhensuTheme::borderSubtle);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());
        const auto cells = layout();
        bool first = true;
        for (int i = 0; i < kNumFigures; ++i) {
            const auto& f = figures_[(size_t)i];
            auto cell = cells[(size_t)i];
            if (cell.isEmpty()) continue;
            if (!first) {
                g.setColour(KhensuTheme::borderSubtle);
                g.drawVerticalLine(cell.getX(), (float)cell.getY() + 8.0f, (float)cell.getBottom() - 8.0f);
            }
            first = false;
            cell.removeFromLeft(kCellPad);
            if (f.tone != Tone::Neutral) {
                g.setColour(toneColour(f.tone));
                g.fillEllipse(juce::Rectangle<float>(6.0f, 6.0f).withCentre({(float)cell.getX() + 3.0f, (float)cell.getCentreY()}));
                cell.removeFromLeft(kDotW);
            }
            g.setFont(labelFont());
            g.setColour(KhensuTheme::textSecondary);
            g.drawText(f.label, cell.removeFromLeft(labelWidth(f)), juce::Justification::centredLeft, false);
            cell.removeFromLeft(kLabelGap);
            g.setFont(valueFont());
            g.setColour(f.value.isEmpty() ? KhensuTheme::textTertiary : KhensuTheme::textPrimary);
            g.drawText(shownValue(f), cell, juce::Justification::centredLeft, false);
        }
    }

    /// Where each figure is drawn; a figure that does not fit whole is dropped
    /// (least useful first) rather than truncated.
    std::array<juce::Rectangle<int>, kNumFigures> layout() const
    {
        std::array<juce::Rectangle<int>, kNumFigures> cells{};
        auto area = getLocalBounds().reduced(KhensuTheme::sp2, 0);
        std::array<bool, kNumFigures> shown{};
        shown.fill(true);
        auto total = [&] {
            int sum = 0;
            for (int i = 0; i < kNumFigures; ++i)
                if (shown[(size_t)i]) sum += naturalWidth(figures_[(size_t)i]);
            return sum;
        };
        for (const int drop : kDropOrder) {
            if (total() <= area.getWidth()) break;
            shown[(size_t)drop] = false;
        }
        if (total() > area.getWidth()) return cells;
        for (int i = 0; i < kNumFigures; ++i)
            if (shown[(size_t)i]) cells[(size_t)i] = area.removeFromLeft(naturalWidth(figures_[(size_t)i]));
        return cells;
    }

  private:
    static constexpr int kCellPad = KhensuTheme::sp4;
    static constexpr int kDotW = 12;
    static constexpr int kLabelGap = KhensuTheme::sp2;
    /// Dropped first to last when narrow: analysis, target, fix check, version.
    static constexpr int kDropOrder[] = {kAnalysis, kTarget, kFixCheck, kVersion};

    static juce::Font labelFont() { return KhensuTheme::labelFont(KhensuTheme::kFontLabel); }
    static juce::Font valueFont() { return KhensuTheme::numericFont(KhensuTheme::kFontBody); }
    static juce::String shownValue(const Figure& f) { return f.value.isEmpty() ? juce::String("-") : f.value; }
    static int labelWidth(const Figure& f) { return juce::GlyphArrangement::getStringWidthInt(labelFont(), f.label) + 2; }
    static int naturalWidth(const Figure& f)
    {
        const juce::String value = f.value.isEmpty() ? juce::String("Version 00") : f.value;
        return 2 * kCellPad + (f.tone != Tone::Neutral ? kDotW : 0) + labelWidth(f) + kLabelGap
               + juce::GlyphArrangement::getStringWidthInt(valueFont(), value) + 4;
    }

    std::array<Figure, kNumFigures> figures_;
};

} // namespace khensu
