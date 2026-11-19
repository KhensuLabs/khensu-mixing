// desktop-app/Source/ui/shared/ExplainMomentView.cpp
#include "ExplainMomentView.h"
#include "PanelRunState.h"
#include "../../IAppController.h"
#include "ServiceErrorHint.h"
#include "SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
juce::String clock(double s)
{
    const int m = (int)(s / 60.0);
    return juce::String(m) + ":" + juce::String(s - m * 60.0, 1).paddedLeft('0', 4);
}

juce::String formatValue(double v, const juce::String& unit)
{
    const int decimals = (unit == "dB/oct" || unit.isEmpty()) ? 2 : 1;
    return juce::String(v, decimals) + (unit.isEmpty() ? juce::String() : " " + unit);
}
} // namespace

void ExplainMomentView::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    noteLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    text_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgSurface);
    text_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    text_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    for (auto* b : {&rewordButton_, &similarButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
}

ExplainMomentView::ExplainMomentView(IAppController& controller, juce::String filePath, double startS, double endS)
    : controller_(controller), filePath_(std::move(filePath)), startS_(startS), endS_(endS)
{
    applyThemeColours();
    titleLabel_.setText(TRANS("Explain ") + clock(startS_) + " - " + clock(endS_), juce::dontSendNotification);
    titleLabel_.setFont(KhensuTheme::labelFont(14.0f));
    addAndMakeVisible(titleLabel_);

    text_.setMultiLine(true, true);
    text_.setReadOnly(true);
    text_.setScrollbarsShown(true);
    text_.setCaretVisible(false);
    text_.setFont(KhensuTheme::bodyFont(12.5f));
    text_.setTitle(TRANS("Explanation of the selected passage"));
    text_.setText(TRANS("Measuring this passage against the whole track..."));
    addAndMakeVisible(text_);

    noteLabel_.setFont(KhensuTheme::bodyFont(10.5f));
    noteLabel_.setText(TRANS("Every sentence quotes a measurement; nothing here is guessed."),
                       juce::dontSendNotification);
    addAndMakeVisible(noteLabel_);

    rewordButton_.setTooltip(TRANS("Ask the configured language model to reword the measured facts "
                                   "(it may not add anything)"));
    rewordButton_.onClick = [this] { request(true); };
    similarButton_.setTooltip(TRANS("Find other passages of this track that sound like the selection"));
    similarButton_.onClick = [this] { findSimilarPassages(); };
    rewordButton_.setEnabled(false);
    addAndMakeVisible(rewordButton_);
    addAndMakeVisible(similarButton_);

    setSize(460, 420);
    request(false);
}

void ExplainMomentView::request(bool rephrase)
{
    rewordButton_.setEnabled(false);
    juce::String genre;
    if (auto* session = controller_.getActiveSession()) genre = juce::String(session->target.primary_genre);
    juce::StringPairArray stems;
    if (const auto& sep = controller_.stems().getLastSeparationResult(); sep.has_value()) {
        const std::pair<const char*, const std::string*> named[] = {
            {"vocals", &sep->vocals_path}, {"drums", &sep->drums_path},
            {"bass", &sep->bass_path},     {"other", &sep->other_path}};
        for (const auto& [name, path] : named)
            if (!path->empty() && juce::File(juce::String(*path)).existsAsFile())
                stems.set(name, juce::String(*path));
    }
    const juce::String path = filePath_;
    const double a = startS_, b = endS_;
    fetchQuietly(*this, controller_, [path, a, b, genre, stems, rephrase] {
        auto result = ipc::HttpServiceBridge{}.explainMoment(path, a, b, genre, stems, rephrase);
        return result;
    }, [rephrase](ExplainMomentView& self, auto&& result) {
        if (result.is_err()) {
            const auto hint = classifyServiceError(result.error());
            self.text_.setText(hint.message + "\n" + hint.remedy);
            return;
        }
        const juce::var v = result.value();
        self.text_.setText(v[af::ExplainMomentResponse::text].toString());
        self.deviations_.clear();
        if (auto* arr = v[af::ExplainMomentResponse::deviations].getArray())
            for (const auto& d : *arr) {
                if (self.deviations_.size() >= 6) break;
                Deviation dev;
                dev.label = d[af::ExplainDeviation::label].toString();
                const juce::String unit = d[af::ExplainDeviation::unit].toString();
                const double delta = (double)d[af::ExplainDeviation::delta];
                dev.value = (delta >= 0.0 ? "+" : "") +
                            (d[af::ExplainDeviation::metric].toString() == "transients"
                                 ? juce::String(juce::roundToInt(delta * 100.0)) + "%"
                                 : formatValue(delta, unit));
                dev.score = (double)d[af::ExplainDeviation::score];
                dev.notable = (bool)d[af::ExplainDeviation::notable];
                self.deviations_.push_back(dev);
            }
        if (rephrase) {
            const bool done = (bool)v[af::ExplainMomentResponse::rephrased];
            self.noteLabel_.setText(done ? TRANS("Reworded by ") +
                                                v[af::ExplainMomentResponse::rephrase_source].toString() +
                                                TRANS(" from the measured facts only.")
                                          : TRANS("Not reworded: ") +
                                                v[af::ExplainMomentResponse::rephrase_reason].toString(),
                                     juce::dontSendNotification);
        }
        self.repaint();
    }, [](ExplainMomentView& self) { self.rewordButton_.setEnabled(true); });
}

void ExplainMomentView::findSimilarPassages()
{
    similarButton_.setEnabled(false);
    const juce::String path = filePath_;
    const double a = startS_, b = endS_;
    fetchQuietly(*this, controller_, [path, a, b] {
        auto result = ipc::HttpServiceBridge{}.embeddingsSections(path, a, b, 4);
        return result;
    }, [](ExplainMomentView& self, auto&& result) {
        self.passageButtons_.clear();
        self.passages_.clear();
        if (result.is_err()) {
            self.noteLabel_.setText(classifyServiceError(result.error()).message, juce::dontSendNotification);
            return;
        }
        const juce::var v = result.value();
        if (auto* arr = v[af::EmbeddingSectionResponse::matches].getArray())
            for (const auto& m : *arr)
                self.passages_.push_back({(double)m[af::EmbeddingSectionMatch::start_s],
                                           (double)m[af::EmbeddingSectionMatch::end_s],
                                           (double)m[af::EmbeddingSectionMatch::similarity]});
        for (const auto& p : self.passages_) {
            auto* btn = self.passageButtons_.add(new juce::TextButton(
                clock(p.start) + " (" + juce::String(juce::roundToInt(p.similarity * 100.0)) + "%)"));
            btn->setTooltip(TRANS("Jump to this similar passage"));
            btn->setColour(juce::TextButton::buttonColourId, KhensuTheme::bgRaised);
            btn->setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
            const double s = p.start, e = p.end;
            btn->onClick = [safe = juce::Component::SafePointer<ExplainMomentView>(&self), s, e] {
                if (safe != nullptr && safe->onJumpTo) safe->onJumpTo(s, e);
            };
            self.addAndMakeVisible(btn);
        }
        if (self.passages_.empty())
            self.noteLabel_.setText(TRANS("No other passage of this track sounds like the selection."),
                                     juce::dontSendNotification);
        self.resized();
    }, [](ExplainMomentView& self) { self.similarButton_.setEnabled(true); });
}

void ExplainMomentView::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgRaised);
    // Ranked deviations, strongest first: bar length = score (1 = noticeable).
    auto area = getLocalBounds().reduced(10);
    area.removeFromTop(28 + 6 + 170 + 6);
    g.setFont(KhensuTheme::bodyFont(11.0f));
    for (const auto& d : deviations_) {
        if (area.getHeight() < 16) break;
        auto row = area.removeFromTop(16);
        const float frac = (float)juce::jlimit(0.0, 1.0, d.score / 3.0);
        auto bar = row.removeFromRight(90).reduced(0, 4).toFloat();
        g.setColour(KhensuTheme::bgSurface);
        g.fillRoundedRectangle(bar, 2.0f);
        g.setColour(d.notable ? KhensuTheme::accentAmber : KhensuTheme::textTertiary);
        g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * frac), 2.0f);
        g.setColour(d.notable ? KhensuTheme::textPrimary : KhensuTheme::textSecondary);
        g.drawText(d.label, row.removeFromLeft(row.getWidth() - 80), juce::Justification::centredLeft, true);
        g.drawText(d.value, row, juce::Justification::centredRight, true);
    }
}

void ExplainMomentView::resized()
{
    auto b = getLocalBounds().reduced(10);
    titleLabel_.setBounds(b.removeFromTop(28));
    b.removeFromTop(6);
    text_.setBounds(b.removeFromTop(170));
    b.removeFromTop(6);
    b.removeFromTop(6 * 16 + 4); // deviation rows (painted)
    auto buttons = b.removeFromBottom(26);
    rewordButton_.setBounds(buttons.removeFromLeft(90));
    buttons.removeFromLeft(6);
    similarButton_.setBounds(buttons.removeFromLeft(170));
    b.removeFromBottom(4);
    auto passages = b.removeFromBottom(24);
    const int w = passageButtons_.isEmpty() ? 0 : passages.getWidth() / passageButtons_.size();
    for (auto* btn : passageButtons_)
        btn->setBounds(passages.removeFromLeft(w).reduced(2, 0));
    noteLabel_.setBounds(b);
}

} // namespace khensu
