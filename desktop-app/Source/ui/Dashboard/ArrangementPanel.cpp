// desktop-app/Source/ui/Dashboard/ArrangementPanel.cpp
#include "ArrangementPanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

static const char* kElementNames[5] = {"foundation", "rhythm", "pad", "lead", "fills"};
static const char* kElementLabels[5] = {"FOUNDATION", "RHYTHM", "PAD", "LEAD", "FILLS"};

// Every cached palette colour on this component, in one place (UI-01).
void ArrangementPanel::applyThemeColours()
{
}

ArrangementPanel::ArrangementPanel(IAppController& c) : PanelCard(c, "Arrangement Elements")
{
    applyThemeColours();
    setExplainer(TRANS("Arrangement Elements"),
                 TRANS("Every arrangement is built from five elements: the FOUNDATION (bass + kick), "
                 "the RHYTHM (drums/percussion), the PAD (sustained chords/beds), the LEAD "
                 "(the element carrying the ear - here, the vocal stem), and FILLS (transient "
                 "ear candy between lead phrases).\n\n"
                 "This grid shows each element's energy share across the song from the separated "
                 "stems. Most arrangement problems are element problems: four strong elements at "
                 "once reads as crowded; rhythm with no foundation has no anchor; long stretches "
                 "with no lead lose the listener. Flags below name the exact moments."));

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Requires separated stems (run Isolate Stems first)."));

    flagLabel_.setFont(KhensuTheme::labelFont(type::caption));
    flagLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(flagLabel_);
}

void ArrangementPanel::runAnalysis()
{
    if (!controller_.stems().hasStemsAvailable()) {
        run_.setIdle(TRANS("Stems not available yet - run Isolate Stems in the Vocal Workspace."));
        return;
    }
    const juce::String vocals = controller_.getStemFile("vocals").getFullPathName();
    const juce::String drums = controller_.getStemFile("drums").getFullPathName();
    const juce::String bass = controller_.getStemFile("bass").getFullPathName();
    const juce::String other = controller_.getStemFile("other").getFullPathName();

    FetchOptions options;
    options.failedText = TRANS("Scan failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Analyzing arrangement..."),
        [vocals, drums, bass, other] {
            return ipc::HttpServiceBridge{}.analyzeArrangement(vocals.toStdString(), drums.toStdString(),
                                                               bass.toStdString(), other.toStdString());
        },
        [](ArrangementPanel& self, const juce::var& v) -> juce::String {
            self.controller_.analysis().recordScanResult("arrangement", v); // WF-14
            self.windows_.clear();
            self.flags_.clear();

            if (auto* arr = v[af::ArrangementResponse::windows].getArray()) {
                for (const auto& wv : *arr) {
                    Window w;
                    w.start = (float)(double)wv[af::ArrangementWindow::start_sec];
                    w.end = (float)(double)wv[af::ArrangementWindow::end_sec];
                    w.silent = (bool)wv[af::ArrangementWindow::silent];
                    const juce::var shares = wv[af::ArrangementWindow::shares];
                    for (int e = 0; e < 5; ++e)
                        w.shares[e] = (float)(double)shares[kElementNames[e]];
                    self.windows_.push_back(w);
                }
            }
            if (auto* arr = v[af::ArrangementResponse::flags].getArray()) {
                for (const auto& fv : *arr) {
                    Flag f;
                    f.start = (float)(double)fv[af::ArrangementFlag::start_sec];
                    f.end = (float)(double)fv[af::ArrangementFlag::end_sec];
                    f.code = fv[af::ArrangementFlag::code].toString();
                    f.message = fv[af::ArrangementFlag::message].toString();
                    self.flags_.push_back(f);
                }
            }
            self.totalDuration_ = self.windows_.empty() ? 0.0f : self.windows_.back().end;

            juce::StringArray flagLines;
            for (size_t i = 0; i < self.flags_.size() && i < 3; ++i) {
                const auto& f = self.flags_[i];
                flagLines.add("[" + juce::String(juce::roundToInt(f.start)) + "s] " + f.message);
            }
            if (self.flags_.size() > 3) flagLines.add("+" + juce::String((int)self.flags_.size() - 3) + " more...");
            self.flagLabel_.setColour(juce::Label::textColourId,
                                      self.flags_.empty() ? KhensuTheme::textTertiary : KhensuTheme::accentAmber);
            self.flagLabel_.setText(self.flags_.empty()
                                        ? juce::String("No arrangement flags - element balance looks healthy.")
                                        : flagLines.joinIntoString("\n"),
                                    juce::dontSendNotification);

            return juce::String((int)self.windows_.size()) + " windows analyzed.";
        },
        std::move(options));
}

void ArrangementPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult() || windows_.empty()) return;

    auto b = bodyArea();
    b.removeFromTop(28);    // controls row
    b.removeFromBottom(50); // flags text
    b.removeFromBottom(4);

    const float labelW = 78.0f;
    auto grid = b.toFloat();
    const float rowH = grid.getHeight() / 5.0f;
    const int n = (int)windows_.size();
    const float cellW = (grid.getWidth() - labelW) / (float)n;

    for (int e = 0; e < 5; ++e) {
        const float y = grid.getY() + e * rowH;
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(kElementLabels[e], juce::Rectangle<float>(grid.getX(), y, labelW - 6.0f, rowH).toNearestInt(),
                   juce::Justification::centredLeft, false);

        for (int i = 0; i < n; ++i) {
            const auto& w = windows_[(size_t)i];
            const float x = grid.getX() + labelW + i * cellW;
            juce::Rectangle<float> cell(x, y + 1.0f, juce::jmax(1.0f, cellW - 1.0f), rowH - 2.0f);
            if (w.silent) {
                g.setColour(KhensuTheme::bgVoid);
                g.fillRect(cell);
                continue;
            }
            const float s = juce::jlimit(0.0f, 1.0f, w.shares[e] / 0.5f); // 50% share = full brightness
            g.setColour(KhensuTheme::bgSurface);
            g.fillRect(cell);
            if (s > 0.02f) {
                g.setColour(KhensuTheme::accentTeal.withAlpha(0.15f + 0.85f * s));
                g.fillRect(cell);
            }
        }
    }
}

void ArrangementPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(160));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    flagLabel_.setBounds(b.removeFromBottom(50));

    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
