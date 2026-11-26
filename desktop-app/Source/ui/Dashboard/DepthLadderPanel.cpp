// desktop-app/Source/ui/Dashboard/DepthLadderPanel.cpp
#include "DepthLadderPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void DepthLadderPanel::applyThemeColours()
{
}

DepthLadderPanel::DepthLadderPanel(IAppController& c) : PanelCard(c, "Depth Ladder")
{
    applyThemeColours();
    setExplainer(TRANS("Depth Ladder"), TRANS("Front-to-back staging, measured from the three cues that create depth "
                                 "perception: reverberant tail (wetness), high-frequency rolloff (darkness), "
                                 "and transient bluntness (softness). Front elements are dry, bright, and "
                                 "sharp; back elements are wet, dark, and smeared.\n\n"
                                 "A great mix uses the whole ladder - lead vocal near the front, "
                                 "pads and rooms behind. If every stem sits on the same rung, the mix reads "
                                 "as flat no matter how good the balance is. Requires separated stems."));

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Requires separated stems."));

    suggestionLabel_.setFont(KhensuTheme::labelFont(type::caption));
    suggestionLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(suggestionLabel_);
    chart_.owner = this;
    chart_.setTitle(TRANS("Depth ladder"));
    chart_.y = ui::chart::scales::percent();
    chart_.setMargins(52, 8, 10, 20);
    addChildComponent(chart_);
}

void DepthLadderPanel::runAnalysis()
{
    if (!controller_.stems().hasStemsAvailable()) {
        run_.setIdle(TRANS("Stems not available yet - run Isolate Stems first."));
        return;
    }
    std::map<std::string, std::string> stems;
    for (const char* name : {"vocals", "drums", "bass", "other"})
        stems[name] = controller_.getStemFile(name).getFullPathName().toStdString();

    FetchOptions options;
    options.failedText = TRANS("Scan failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Mapping depth..."),
        [stems] { return ipc::HttpServiceBridge{}.analyzeDepth(stems); },
        [](DepthLadderPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("depth_ladder", v); // WF-14
            self.stems_.clear();
            if (auto* arr = v[af::DepthResponse::stems].getArray()) {
                for (const auto& sv : *arr) {
                    StemDepth d;
                    d.name = sv[af::StemDepth::stem].toString();
                    d.depth = (float)(double)sv[af::StemDepth::depth];
                    d.wetness = (float)(double)sv[af::StemDepth::wetness];
                    d.darkness = (float)(double)sv[af::StemDepth::darkness];
                    d.softness = (float)(double)sv[af::StemDepth::softness];
                    self.stems_.push_back(d);
                }
            }
            juce::StringArray lines;
            if (auto* arr = v[af::DepthResponse::suggestions].getArray())
                for (const auto& s : *arr)
                    lines.add(s.toString());
            self.suggestionLabel_.setColour(juce::Label::textColourId,
                                            lines.isEmpty() ? KhensuTheme::textTertiary : KhensuTheme::accentAmber);
            self.suggestionLabel_.setText(lines.isEmpty() ? juce::String("Staging uses the depth field - no flags.")
                                                          : lines.joinIntoString("\n"),
                                          juce::dontSendNotification);
            self.rebuildChart();
            return juce::String((int)self.stems_.size()) + " stems mapped.";
        },
        std::move(options));
}

void DepthLadderPanel::rebuildChart()
{
    // VIS-1: a ChartKit dot chart - one column per stem (its categorical
    // colour), depth on the y axis from front (0 %) to back (100 %); the
    // readout adds wetness, darkness and softness.
    std::vector<juce::String> names;
    std::vector<ui::chart::Series> series;
    juce::StringArray parts;
    for (size_t i = 0; i < stems_.size(); ++i) {
        const auto& s = stems_[i];
        names.push_back(s.name);
        ui::chart::Series dot{s.name, KhensuTheme::vizCategory(static_cast<int>(i)), {static_cast<double>(i)},
                              {static_cast<double>(s.depth)}, ui::chart::Series::Kind::Dots, 4.0f};
        dot.inLegend = false;
        series.push_back(dot);
        parts.add(s.name + " " + juce::String(juce::roundToInt(s.depth * 100.0f)) + " %");
    }
    chart_.x = ui::chart::scales::categories(names);
    chart_.setXRange(chart_.x.lo, chart_.x.hi);
    chart_.setSeries(std::move(series));
    chart_.setSummary(TRANS("Depth of each stem, 0 % front to 100 % back") + ": " + parts.joinIntoString(", ") + ".");
}

void DepthLadderPanel::Chart::extendReadout(ui::chart::Readout& r, double x)
{
    if (owner == nullptr) return;
    const int i = juce::roundToInt(x);
    if (i < 0 || i >= static_cast<int>(owner->stems_.size())) return;
    const auto& s = owner->stems_[static_cast<size_t>(i)];
    r.lines.clear();
    r.title = s.name;
    const auto pct = [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; };
    r.lines.push_back({TRANS("Depth") + " " + pct(s.depth) + " (" + (s.depth < 0.33f ? TRANS("front") : s.depth < 0.66f ? TRANS("middle") : TRANS("back")) + ")",
                       KhensuTheme::vizCategory(i)});
    r.lines.push_back({TRANS("Wetness") + " " + pct(s.wetness), style().muted});
    r.lines.push_back({TRANS("Darkness") + " " + pct(s.darkness), style().muted});
    r.lines.push_back({TRANS("Softness") + " " + pct(s.softness), style().muted});
}

void DepthLadderPanel::paint(juce::Graphics& g) { PanelCard::paint(g); }

void DepthLadderPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(110));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    suggestionLabel_.setBounds(b.removeFromBottom(46));

    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
    chart_.setBounds(errArea.withTrimmedBottom(48));
    chart_.setVisible(run_.showsResult() && !stems_.empty());
}

} // namespace khensu
