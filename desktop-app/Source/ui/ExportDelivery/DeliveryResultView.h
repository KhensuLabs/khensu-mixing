// desktop-app/Source/ui/ExportDelivery/DeliveryResultView.h
// UXR-1: a Delivery & QA result as rows instead of a monospace log - each
// check with its status (pass / warn / fail with an icon and the word, so
// colour is never the only cue), its value and its target, then any notes.
// DeliveryResult is plain data (tests build and read it); the view paints it
// in a scrolling list with theme tokens only.
// THREAD_CONTEXT(UIThread) for the view; DeliveryResult is AnyThread.
#pragma once
#include <JuceHeader.h>
#include "../shared/KhensuTheme.h"
#include <vector>

namespace khensu {

struct DeliveryResult {
    enum class Status { Pass, Warn, Fail, Info };
    struct Row {
        Status status = Status::Info;
        juce::String label, value, target;
    };
    juce::String title;
    std::vector<Row> rows;
    juce::StringArray notes;

    void add(Status s, const juce::String& label, const juce::String& value = {}, const juce::String& target = {})
    {
        rows.push_back({s, label, value, target});
    }
    /// One plain-text message (an error, a hint, a long formatted report).
    static DeliveryResult message(const juce::String& text)
    {
        DeliveryResult r;
        r.notes.addLines(text);
        while (!r.notes.isEmpty() && r.notes[r.notes.size() - 1].trim().isEmpty())
            r.notes.remove(r.notes.size() - 1);
        return r;
    }
    /// A service status word ("pass", "warn", "fail", ...) as a Status.
    static Status statusFrom(const juce::String& word)
    {
        const auto w = word.trim().toLowerCase();
        if (w == "pass" || w == "ok" || w == "passed") return Status::Pass;
        if (w == "warn" || w == "warning") return Status::Warn;
        if (w == "fail" || w == "failed" || w == "error") return Status::Fail;
        return Status::Info;
    }
    static juce::String statusWord(Status s)
    {
        switch (s) {
        case Status::Pass: return TRANS("Pass");
        case Status::Warn: return TRANS("Warn");
        case Status::Fail: return TRANS("Fail");
        case Status::Info: break;
        }
        return {};
    }
    /// The worst status among the rows (Info when none graded).
    Status overall() const
    {
        Status worst = Status::Info;
        for (const auto& r : rows) {
            if (r.status == Status::Fail) return Status::Fail;
            if (r.status == Status::Warn) worst = Status::Warn;
            else if (r.status == Status::Pass && worst == Status::Info) worst = Status::Pass;
        }
        return worst;
    }
    /// Plain text, for tests and the clipboard: the title, one line per row,
    /// then the notes. A message alone is exactly its text.
    juce::String toText() const
    {
        juce::StringArray lines;
        if (title.isNotEmpty()) lines.add(title);
        for (const auto& r : rows) {
            juce::String line = r.status == Status::Info ? juce::String() : statusWord(r.status).toUpperCase() + "  ";
            line << r.label;
            if (r.value.isNotEmpty()) line << ": " << r.value;
            if (r.target.isNotEmpty()) line << "  (" << TRANS("target") << " " << r.target << ")";
            lines.add(line);
        }
        lines.addArray(notes);
        return lines.joinIntoString("\n");
    }
};

class DeliveryResultView final : public juce::Component {
  public:
    DeliveryResultView()
    {
        viewport_.setViewedComponent(&content_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        setTitle(TRANS("Delivery QA results"));
        setDescription(TRANS("Each check of the last delivery tool you ran: status, value and target."));
    }

    void setResult(DeliveryResult r)
    {
        result_ = std::move(r);
        layoutContent();
        repaint();
    }
    const DeliveryResult& result() const noexcept { return result_; }

    void resized() override
    {
        viewport_.setBounds(getLocalBounds());
        layoutContent();
    }

    void paint(juce::Graphics& g) override
    {
        g.setColour(KhensuTheme::bgBase);
        g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
    }

    static juce::Colour colourFor(DeliveryResult::Status s)
    {
        switch (s) {
        case DeliveryResult::Status::Pass: return KhensuTheme::accentGreen;
        case DeliveryResult::Status::Warn: return KhensuTheme::accentAmber;
        case DeliveryResult::Status::Fail: return KhensuTheme::accentRed;
        case DeliveryResult::Status::Info: break;
        }
        return KhensuTheme::textTertiary;
    }

  private:
    static constexpr int kPad = 10;
    static constexpr int kTitleH = 22;
    static constexpr int kRowH = 24;
    static constexpr int kNoteLineH = 16;

    struct Content final : public juce::Component {
        DeliveryResultView& owner;
        explicit Content(DeliveryResultView& o) : owner(o) {}
        void paint(juce::Graphics& g) override { owner.paintContent(g, getWidth()); }
    };

    int noteLines(int width) const
    {
        const auto font = KhensuTheme::bodyFont(KhensuTheme::kFontLabel + 1.0f);
        int lines = 0;
        for (const auto& n : result_.notes) {
            const float w = juce::GlyphArrangement::getStringWidth(font, n);
            lines += juce::jmax(1, static_cast<int>(std::ceil(w / static_cast<float>(juce::jmax(40, width - 2 * kPad)))));
        }
        return lines;
    }

    void layoutContent()
    {
        const int w = juce::jmax(0, viewport_.getMaximumVisibleWidth());
        const int h = kPad + (result_.title.isNotEmpty() ? kTitleH : 0) + static_cast<int>(result_.rows.size()) * kRowH +
                      (result_.notes.isEmpty() ? 0 : 6 + noteLines(w) * kNoteLineH) + kPad;
        content_.setSize(w, h);
        content_.repaint();
    }

    void paintContent(juce::Graphics& g, int width)
    {
        int y = kPad;
        if (result_.title.isNotEmpty()) {
            const auto overall = result_.overall();
            g.setFont(KhensuTheme::uiFont(KhensuTheme::kFontH2, juce::Font::bold));
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(result_.title, kPad, y, width - 2 * kPad - 90, kTitleH, juce::Justification::centredLeft, true);
            if (overall != DeliveryResult::Status::Info) {
                g.setColour(colourFor(overall));
                g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
                g.drawText(DeliveryResult::statusWord(overall).toUpperCase(), width - kPad - 90, y, 90, kTitleH,
                           juce::Justification::centredRight, false);
            }
            y += kTitleH;
        }
        // status | label | value | target
        const int statusW = 64;
        const int targetW = juce::jlimit(0, 160, width / 5);
        const int valueW = juce::jlimit(0, 240, width / 3);
        for (size_t i = 0; i < result_.rows.size(); ++i) {
            const auto& r = result_.rows[i];
            juce::Rectangle<int> row(kPad, y, width - 2 * kPad, kRowH);
            if ((i % 2) == 1) {
                g.setColour(KhensuTheme::bgSurface);
                g.fillRect(row);
            }
            auto cell = row.removeFromLeft(statusW);
            if (r.status != DeliveryResult::Status::Info) {
                const auto c = colourFor(r.status);
                auto dot = cell.removeFromLeft(14).withSizeKeepingCentre(10, 10).toFloat();
                g.setColour(c);
                if (r.status == DeliveryResult::Status::Pass) {
                    g.fillEllipse(dot);
                } else if (r.status == DeliveryResult::Status::Warn) {
                    juce::Path tri;
                    tri.addTriangle(dot.getCentreX(), dot.getY(), dot.getRight(), dot.getBottom(), dot.getX(),
                                    dot.getBottom());
                    g.fillPath(tri);
                } else {
                    g.fillRect(dot);
                }
                g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
                g.drawText(DeliveryResult::statusWord(r.status), cell.withTrimmedLeft(4), juce::Justification::centredLeft,
                           false);
            }
            auto target = row.removeFromRight(targetW);
            auto value = row.removeFromRight(valueW);
            g.setFont(KhensuTheme::bodyFont(KhensuTheme::kFontLabel + 1.0f));
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(r.label, row.reduced(4, 0), juce::Justification::centredLeft, true);
            g.setFont(KhensuTheme::numericFont(KhensuTheme::kFontLabel + 1.0f));
            g.drawText(r.value, value.reduced(4, 0), juce::Justification::centredLeft, true);
            g.setColour(KhensuTheme::textSecondary);
            if (r.target.isNotEmpty())
                g.drawText(TRANS("target") + " " + r.target, target.reduced(4, 0), juce::Justification::centredLeft, true);
            y += kRowH;
        }
        if (!result_.notes.isEmpty()) {
            y += 6;
            g.setFont(KhensuTheme::bodyFont(KhensuTheme::kFontLabel + 1.0f));
            g.setColour(KhensuTheme::textSecondary);
            const int textW = width - 2 * kPad;
            for (const auto& n : result_.notes) {
                const float w = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), n);
                const int lines = juce::jmax(1, static_cast<int>(std::ceil(w / static_cast<float>(juce::jmax(40, textW)))));
                g.drawFittedText(n, kPad, y, textW, lines * kNoteLineH, juce::Justification::topLeft, lines, 1.0f);
                y += lines * kNoteLineH;
            }
        }
    }

    DeliveryResult result_;
    Content content_{*this};
    juce::Viewport viewport_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeliveryResultView)
};

} // namespace khensu
