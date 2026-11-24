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

    suggestionLabel_.setFont(KhensuTheme::labelFont(9.5f));
    suggestionLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(suggestionLabel_);
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
            return juce::String((int)self.stems_.size()) + " stems mapped.";
        },
        std::move(options));
}

void DepthLadderPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult() || stems_.empty()) return;

    auto b = bodyArea();
    b.removeFromTop(28);
    b.removeFromBottom(46);
    auto ladder = b.toFloat().reduced(6.0f, 4.0f);
    if (ladder.getHeight() < 40.0f) return;

    // Rails + rungs: front (bottom) to back (top).
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.25f));
    for (int i = 0; i <= 4; ++i) {
        const float y = ladder.getY() + ladder.getHeight() * (float)i / 4.0f;
        g.drawHorizontalLine(juce::roundToInt(y), ladder.getX() + 44.0f, ladder.getRight());
    }
    g.setFont(KhensuTheme::labelFont(8.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("BACK", (int)ladder.getX(), (int)ladder.getY(), 40, 10, juce::Justification::left);
    g.drawText("FRONT", (int)ladder.getX(), (int)ladder.getBottom() - 10, 40, 10, juce::Justification::left);

    // Stems as pills placed by depth; x staggered so labels don't overlap.
    const float innerX = ladder.getX() + 50.0f;
    const float innerW = ladder.getRight() - innerX;
    int i = 0;
    for (const auto& s : stems_) {
        const float y = ladder.getY() + ladder.getHeight() * s.depth;
        const float x = innerX + innerW * ((float)(i % 4) / 4.0f);
        juce::Rectangle<float> pill(x, y - 9.0f, innerW / 4.5f, 18.0f);

        // Wetter = cooler color; front/dry = warm teal.
        auto col = KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentPurple, s.depth);
        g.setColour(col.withAlpha(0.25f));
        g.fillRoundedRectangle(pill, 9.0f);
        g.setColour(col);
        g.drawRoundedRectangle(pill, 9.0f, 1.2f);
        g.setFont(KhensuTheme::labelFont(9.0f).boldened());
        g.drawText(s.name.toUpperCase(), pill.toNearestInt(), juce::Justification::centred, false);
        ++i;
    }
}

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
}

} // namespace khensu
