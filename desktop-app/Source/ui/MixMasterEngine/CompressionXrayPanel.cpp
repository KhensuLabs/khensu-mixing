// desktop-app/Source/ui/MixMasterEngine/CompressionXrayPanel.cpp
#include "CompressionXrayPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void CompressionXrayPanel::applyThemeColours()
{
    xrayButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    xrayButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

CompressionXrayPanel::CompressionXrayPanel(IAppController& c) : PanelCard(c, "Compression X-Ray")
{
    applyThemeColours();
    setExplainer(TRANS("Compression X-Ray"), TRANS("Reconstructs what your dynamics processing actually did by comparing the "
                                      "current render against the original: the effective gain curve over time, "
                                      "with the two classic failure signatures flagged - PUMPING (gain "
                                      "breathing rhythmically with the beat: slow the release or ease the ratio) "
                                      "and OVER-COMPRESSION (sustained deep reduction that flattens the chorus "
                                      "lift your arrangement wrote).\n\n"
                                      "The 2-4 dB region is the musical sweet spot for most program material. "
                                      "Static level offsets (makeup gain) are separated out - the curve "
                                      "shows only dynamic movement. Requires an applied render (A/B loaded)."));

    xrayButton_.onClick = [this] { runXray(); };
    addAndMakeVisible(xrayButton_);
    run_.onRetry([this] { runXray(); });
    run_.setIdle(TRANS("Apply a render first (chat, macros, mastering agent)."));

    eventsLabel_.setFont(KhensuTheme::labelFont(type::caption));
    eventsLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(eventsLabel_);
}

void CompressionXrayPanel::runXray()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || s->files.empty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    if (!controller_.playback().hasAbComparison()) {
        run_.setIdle(TRANS("No processed render loaded - apply a chain first."));
        return;
    }
    const juce::String original(s->files[0].path);
    const juce::String processed = controller_.playback().abProcessedPath();

    FetchOptions options;
    options.failedText = TRANS("X-ray failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("X-raying..."),
        [original, processed] {
            return ipc::HttpServiceBridge{}.analyzeGainXray(original.toStdString(), processed.toStdString());
        },
        [](CompressionXrayPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("compression_xray", v); // WF-14
            self.curveDb_.clear();
            if (auto* arr = v[af::GainXrayResponse::gain_curve_db].getArray())
                for (const auto& e : *arr)
                    self.curveDb_.push_back((float)(double)e);
            self.brightnessDb_.clear();
            if (auto* arr = v[af::GainXrayResponse::brightness_curve_db].getArray())
                for (const auto& e : *arr)
                    self.brightnessDb_.push_back((float)(double)e);
            self.maxReductionDb_ = (float)(double)v[af::GainXrayResponse::max_reduction_db];
            self.staticOffsetDb_ = (float)(double)v[af::GainXrayResponse::static_offset_db];
            self.verdict_ = v[af::GainXrayResponse::verdict].toString();

            juce::StringArray lines;
            if (auto* arr = v[af::GainXrayResponse::events].getArray())
                for (size_t i = 0; i < arr->size() && i < 3; ++i)
                    lines.add(arr->getReference((int)i)[af::GainXrayEvent::message].toString());
            self.eventsLabel_.setColour(juce::Label::textColourId,
                                        lines.isEmpty() ? KhensuTheme::textTertiary : KhensuTheme::accentAmber);
            self.eventsLabel_.setText(lines.isEmpty() ? juce::String("No pumping or over-compression events.")
                                                      : lines.joinIntoString("\n"),
                                      juce::dontSendNotification);

            return TRANS("Verdict: ") + self.verdict_ + "  -  max reduction " + juce::String(self.maxReductionDb_, 1) +
                   " dB" + "  -  static offset " + juce::String(self.staticOffsetDb_, 1) + " dB";
        },
        std::move(options));
}

void CompressionXrayPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult() || curveDb_.empty()) return;

    auto b = bodyArea();
    b.removeFromTop(28);
    b.removeFromTop(16);
    b.removeFromBottom(44);
    auto plot = b.toFloat().reduced(2.0f, 3.0f);
    if (plot.getHeight() < 24.0f) return;

    const float maxDb = juce::jmax(6.0f, maxReductionDb_ + 2.0f);
    const int n = (int)curveDb_.size();

    // 0 dB baseline at the top; reduction pulls the curve down.
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.3f));
    g.drawHorizontalLine(juce::roundToInt(plot.getY()), plot.getX(), plot.getRight());
    // Sweet-spot band (2-4 dB reduction).
    const float y2 = plot.getY() + (2.0f / maxDb) * plot.getHeight();
    const float y4 = plot.getY() + (4.0f / maxDb) * plot.getHeight();
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.07f));
    g.fillRect(plot.getX(), y2, plot.getWidth(), y4 - y2);

    juce::Path curve;
    hover_.clear();
    for (int i = 0; i < n; ++i) {
        const float x = plot.getX() + plot.getWidth() * (float)i / (float)juce::jmax(1, n - 1);
        const float reduction = juce::jmax(0.0f, -curveDb_[(size_t)i]);
        {
            const float w = plot.getWidth() / (float)juce::jmax(1, n - 1);
            juce::String text = juce::String(juce::roundToInt(100.0f * (float)i / (float)juce::jmax(1, n - 1))) + " % "
                                + TRANS("through") + ": " + juce::String(reduction, 1) + " dB " + TRANS("of gain reduction");
            if (!brightnessDb_.empty()) {
                const auto bi = (size_t)juce::jlimit(0, (int)brightnessDb_.size() - 1,
                                                     juce::roundToInt((float)i / (float)juce::jmax(1, n - 1) * (float)(brightnessDb_.size() - 1)));
                text << ", " << (brightnessDb_[bi] >= 0.0f ? TRANS("brighter by") : TRANS("darker by")) << " "
                     << juce::String(std::abs(brightnessDb_[bi]), 1) << " dB";
            }
            hover_.addColumn(x - w * 0.5f, x + w * 0.5f, plot, text);
        }
        const float y = plot.getY() + juce::jmin(1.0f, reduction / maxDb) * plot.getHeight();
        if (i == 0)
            curve.startNewSubPath(x, y);
        else
            curve.lineTo(x, y);
    }
    g.setColour(KhensuTheme::accentTeal.withAlpha(0.9f));
    g.strokePath(curve, juce::PathStrokeType(1.4f));

    // Brightness (tonal) delta overlay: a second curve about the vertical
    // centre - above = the render got brighter here, below = darker. Amber to
    // read as distinct from the teal gain curve. Purely additive; the gain
    // curve stays the primary read.
    if (!brightnessDb_.empty()) {
        const int bn = (int)brightnessDb_.size();
        float maxBr = 1.0f;
        for (float v2 : brightnessDb_)
            maxBr = juce::jmax(maxBr, std::abs(v2));
        const float midY = plot.getCentreY();
        juce::Path bpath;
        for (int i = 0; i < bn; ++i) {
            const float x = plot.getX() + plot.getWidth() * (float)i / (float)juce::jmax(1, bn - 1);
            const float y = midY - (brightnessDb_[(size_t)i] / maxBr) * (plot.getHeight() * 0.42f);
            if (i == 0)
                bpath.startNewSubPath(x, y);
            else
                bpath.lineTo(x, y);
        }
        g.setColour(KhensuTheme::accentAmber.withAlpha(0.55f));
        g.strokePath(bpath, juce::PathStrokeType(1.0f));
    }

    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("0 dB", (int)plot.getX(), (int)plot.getY() - 2, 36, 10, juce::Justification::left);
    g.drawText("-" + juce::String(juce::roundToInt(maxDb)) + " dB", (int)plot.getX(), (int)plot.getBottom() - 10, 44, 10,
               juce::Justification::left);
    if (!brightnessDb_.empty()) {
        g.setColour(KhensuTheme::accentAmber.withAlpha(0.7f));
        g.drawText(juce::CharPointer_UTF8("tone \xe2\x86\x95"), (int)plot.getRight() - 44, (int)plot.getY() - 2, 44, 10,
                   juce::Justification::right);
    }
}

void CompressionXrayPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    xrayButton_.setBounds(controls.removeFromLeft(160));
    b.removeFromTop(2);
    run_.label().setBounds(b.removeFromTop(14));
    eventsLabel_.setBounds(b.removeFromBottom(44));
    run_.errorState().setBounds(b);
}

} // namespace khensu
