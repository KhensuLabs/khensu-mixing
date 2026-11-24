// desktop-app/Source/ui/Dashboard/SourceForensicsPanel.cpp
#include "SourceForensicsPanel.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void SourceForensicsPanel::applyThemeColours()
{
}

SourceForensicsPanel::SourceForensicsPanel(IAppController& c) : PanelCard(c, "Source Quality Forensics")
{
    applyThemeColours();
    setExplainer(TRANS("Source Quality Forensics"),
                 TRANS("Is this file what its container claims? Three checks catch a masquerading "
                 "source: a brickwall low-pass (a WAV decoded from an MP3/AAC), a fake hi-res "
                 "file (an 88.2/96 kHz container upsampled from 44.1/48 with a hard cliff at "
                 "the old Nyquist), and a padded bit depth (a 24-bit file whose noise floor "
                 "sits at 16-bit levels). Verdicts are heuristic and abstain when uncertain."));

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Runs at intake; re-run any time."));
}

void SourceForensicsPanel::runAnalysis()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) {
        run_.setIdle(TRANS("No active session."));
        return;
    }
    FetchOptions options;
    options.onSettled = [this] { autoScan_.finish(); }; // auto-run done (results gate)
    fetchThenPublish(
        *this, controller_, run_, TRANS("Inspecting source..."),
        [path] {
            juce::DynamicObject::Ptr body = new juce::DynamicObject();
            body->setProperty(af::SourceForensicsRequest::file_path, path);
            return ipc::HttpServiceBridge{}.postAnalyzeJson("/analyze/source_forensics", juce::var(body.get()));
        },
        [](SourceForensicsPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("source_forensics", v); // WF-14
            const int sr = (int)v[af::SourceForensicsResponse::sample_rate];
            const int bits = (int)v[af::SourceForensicsResponse::declared_bits];
            self.header_ = juce::String(sr / 1000.0, 1) + " kHz / " + juce::String(bits) + "-bit (" +
                           v[af::SourceForensicsResponse::subtype].toString() + ")";
            self.findings_.clear();
            const juce::var bw = v[af::SourceForensicsResponse::brickwall];
            self.findings_.push_back({"Bandwidth", bw["likely_source"].toString(), (bool)bw["brickwall_detected"]});
            const juce::var hr = v[af::SourceForensicsResponse::hi_res];
            if ((bool)hr["applicable"])
                self.findings_.push_back({"Hi-res",
                                          (bool)hr["upsampled_suspected"]
                                              ? juce::String("Upsampled - hard cliff at the source Nyquist")
                                              : juce::String("Genuine ultrasonic content present"),
                                          (bool)hr["upsampled_suspected"]});
            const juce::var bd = v[af::SourceForensicsResponse::bit_depth];
            self.findings_.push_back(
                {"Bit depth", bd["evidence"].toString(), bd["verdict"].toString() == "possibly_16bit_source"});
            return self.header_;
        },
        std::move(options));
}

void SourceForensicsPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No forensics yet"), TRANS("Runs at intake; re-run any time."), 28);
        return;
    }
    auto b = bodyArea();
    b.removeFromTop(28);
    g.setFont(KhensuTheme::monoFont(10.0f));
    g.setColour(KhensuTheme::textSecondary);
    g.drawText(header_, b.removeFromTop(16), juce::Justification::centredLeft, false);
    b.removeFromTop(4);

    const int rowH = juce::jmax(28, b.getHeight() / (int)juce::jmax((size_t)1, findings_.size()));
    for (const auto& f : findings_) {
        auto row = b.removeFromTop(rowH);
        const juce::Colour c = f.flag ? KhensuTheme::accentAmber : KhensuTheme::accentGreen;
        g.setColour(c);
        g.setFont(KhensuTheme::labelFont(10.0f).boldened());
        g.drawText((f.flag ? juce::String(juce::CharPointer_UTF8("\xe2\x9a\xa0 "))
                           : juce::String(juce::CharPointer_UTF8("+ "))) +
                       f.label,
                   row.removeFromTop(14), juce::Justification::centredLeft, false);
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(9.5f));
        g.drawFittedText(f.detail, row, juce::Justification::topLeft, 2);
    }
}

void SourceForensicsPanel::resized()
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
