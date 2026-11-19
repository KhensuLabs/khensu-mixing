// desktop-app/Source/ui/VocalWorkspace/PreflightPanel.cpp
#include "PreflightPanel.h"
#include "TakesBin.h"
#include "../../IAppController.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

void PreflightPanel::CheckRow::paint(juce::Graphics& g)
{
    auto b = getLocalBounds().reduced(2, 1);
    const juce::Colour col = state == 1   ? KhensuTheme::accentTeal
                             : state == 2 ? KhensuTheme::accentAmber
                             : state == 3 ? KhensuTheme::accentRed
                                          : KhensuTheme::textTertiary;
    g.setColour(col);
    const char* mark = state == 1 ? "+" : state == 0 ? "-" : "!";
    g.setFont(KhensuTheme::labelFont(11.0f).boldened());
    g.drawText(mark, b.removeFromLeft(16), juce::Justification::centred, false);

    g.setFont(KhensuTheme::bodyFont(10.5f).boldened());
    g.setColour(KhensuTheme::textPrimary);
    g.drawText(title, b.removeFromLeft(juce::jmin(150, b.getWidth() / 3)), juce::Justification::centredLeft, true);

    g.setFont(KhensuTheme::labelFont(9.5f));
    g.setColour(state >= 2 ? col : KhensuTheme::textSecondary);
    g.drawFittedText(detail, b, juce::Justification::centredLeft, 2);
}

// Every cached palette colour on this component, in one place (UI-01).
void PreflightPanel::applyThemeColours()
{
    chooseButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    chooseButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

PreflightPanel::PreflightPanel(IAppController& c) : PanelCard(c, "Recording Preflight")
{
    applyThemeColours();
    setExplainer(TRANS("Recording Preflight"), "A good vocal is ~80% capture, ~20% mixing - so check the capture "
                                        "BEFORE the real take. Record 20-30 seconds of test vocal in position "
                                        "(include some loud phrases and a few seconds of room silence), load it here, "
                                        "and the scan verifies: recording level and headroom, noise floor, room "
                                        "reflections/comb filtering, mains hum, and proximity boom.\n\n"
                                        "Placement fundamentals while you're at it: 10-20 cm off the mic with a pop "
                                        "filter; off-axis 10-15 degrees to tame plosives; never dead-center of the "
                                        "room; a duvet behind the singer beats an untreated wall.");

    chooseButton_.onClick = [this] { chooseTestRecording(); };
    addAndMakeVisible(chooseButton_);
    run_.onRetry([this] {
        if (lastPath_.isNotEmpty()) runPreflight(lastPath_);
    });
    run_.setIdle(TRANS("Record a short test take, then load it here."));

    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    run_.errorState().toFront(false); // over the checklist it replaces

    buildDefaultChecklist();
}

void PreflightPanel::buildDefaultChecklist()
{
    rows_.clear();
    content_.removeAllChildren();
    struct Item {
        const char* title;
        const char* detail;
    };
    const Item items[] = {
        {"Level & headroom", "Aim for peaks around -10 dBFS, average around -18 dBFS."},
        {"Noise floor", "Silence between phrases should sit below -60 dBFS."},
        {"Room reflections", "No comb filtering from nearby desk/window/wall."},
        {"Mains hum", "No 50/60 Hz ground-loop hum."},
        {"Proximity boom", "Low-end tilt consistent with intended mic distance."},
        {"File integrity", "True bit depth / sample rate match the container."},
    };
    for (const auto& item : items) {
        auto row = std::make_unique<CheckRow>();
        row->title = item.title;
        row->detail = item.detail;
        content_.addAndMakeVisible(*row);
        rows_.push_back(std::move(row));
    }
    resized();
}

void PreflightPanel::chooseTestRecording()
{
    // A take from the session's Takes bin, or a file from disk that then
    // joins it (WF-5).
    juce::Component::SafePointer<PreflightPanel> self(this);
    takes_bin::pick(*this, chooseButton_, controller_.vocalEdit(), chooser_, TRANS("Select the test recording"),
                    takes_bin::Pick::One, [self](const juce::StringArray& chosen) {
                        if (self != nullptr && !chosen.isEmpty()) self->runPreflight(chosen[0]);
                    });
}

void PreflightPanel::runPreflight(const juce::String& path)
{
    lastPath_ = path;
    FetchOptions options;
    options.failedText = TRANS("Scan failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Scanning test recording..."),
        [path] { return ipc::HttpServiceBridge{}.analyzeSourceTriage(path.toStdString()); },
        [](PreflightPanel& self, const juce::var& v) -> juce::String {
            // Start every check green, then downgrade from findings.
            for (auto& row : self.rows_) {
                row->state = 1;
            }
            auto setState = [&](int rowIdx, int state, const juce::String& detail) {
                if (rowIdx < (int)self.rows_.size()) {
                    auto& r = *self.rows_[(size_t)rowIdx];
                    if (state > r.state) {
                        r.state = state;
                        r.detail = detail;
                    }
                    r.repaint();
                }
            };

            int failCount = 0;
            if (auto* arr = v[af::SourceTriageResponse::findings].getArray()) {
                for (const auto& f : *arr) {
                    const juce::String code = f[af::SourceTriageFinding::code].toString();
                    const juce::String detail = f[af::SourceTriageFinding::detail].toString();
                    const juce::String cat = f[af::SourceTriageFinding::category].toString();
                    const int st = cat == "rerecord" ? 3 : cat == "info" ? 2 : 2;
                    if (st == 3) ++failCount;
                    if (code == "clipping" || code == "dropouts")
                        setState(0, st, detail);
                    else if (code == "noise_floor")
                        setState(1, st, detail);
                    else if (code == "comb_filtering")
                        setState(2, st, detail);
                    else if (code == "mains_hum")
                        setState(3, st, detail);
                    else if (code == "proximity_boom")
                        setState(4, st, detail);
                    else if (code == "integrity" || code == "dc_offset")
                        setState(5, st, detail);
                }
            }
            const juce::String verdict = v[af::SourceTriageResponse::verdict].toString();
            // A transient verdict tint (not restored on a theme change).
            self.run_.label().setColour(juce::Label::textColourId, verdict == "clean" ? KhensuTheme::accentTeal
                                                                   : failCount > 0    ? KhensuTheme::accentRed
                                                                                      : KhensuTheme::accentAmber);
            return verdict == "clean" ? juce::String("Preflight clean - roll the real take.")
                                      : "Verdict: " + verdict + " - fix the flagged items before tracking.";
        },
        std::move(options));
}

void PreflightPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    chooseButton_.setBounds(controls.removeFromLeft(180));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    b.removeFromTop(4);
    viewport_.setBounds(b);
    run_.errorState().setBounds(b);

    const int rowH = 34;
    content_.setSize(juce::jmax(50, viewport_.getWidth() - 10), juce::jmax(1, (int)rows_.size() * (rowH + 2)));
    int y = 0;
    for (auto& row : rows_) {
        row->setBounds(0, y, content_.getWidth(), rowH);
        y += rowH + 2;
    }
}

} // namespace khensu
