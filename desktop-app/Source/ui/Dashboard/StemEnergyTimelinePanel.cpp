// desktop-app/Source/ui/Dashboard/StemEnergyTimelinePanel.cpp
#include "StemEnergyTimelinePanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include "../shared/PanelRunState.h"
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

static juce::Colour stemColour(const juce::String& name)
{
    if (name == "vocals") return KhensuTheme::accentTeal;
    if (name == "drums") return KhensuTheme::accentAmber;
    if (name == "bass") return KhensuTheme::accentBlue;
    return KhensuTheme::textSecondary; // other
}

// Every cached palette colour on this component, in one place (UI-01).
void StemEnergyTimelinePanel::applyThemeColours()
{
    analyzeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    analyzeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
}

StemEnergyTimelinePanel::StemEnergyTimelinePanel(IAppController& c) : PanelCard(c, "Stem Energy Timeline")
{
    applyThemeColours();
    setExplainer(TRANS("Stem Energy Timeline"),
                 TRANS("Each separated stem's share of the mix's energy across the song, as a "
                 "stacked area - the arrangement building and dropping: where the "
                 "vocal enters, when the drums carry, the bass drop. Fractions sum to 100%%. "
                 "Requires separated stems."),
                 "stem_taxonomy");

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Requires separated stems (auto-runs once they exist)."));
}

void StemEnergyTimelinePanel::runAnalysis()
{
    if (!controller_.stems().hasStemsAvailable()) {
        run_.setIdle(TRANS("Requires separated stems - run Isolate Stems first."));
        return;
    }
    const juce::String vocals = controller_.getStemFile("vocals").getFullPathName();
    const juce::String drums = controller_.getStemFile("drums").getFullPathName();
    const juce::String bass = controller_.getStemFile("bass").getFullPathName();
    const juce::String other = controller_.getStemFile("other").getFullPathName();

    fetchThenPublish(
        *this, controller_, run_, TRANS("Measuring stem energy..."),
        [vocals, drums, bass, other] {
            juce::DynamicObject::Ptr body = new juce::DynamicObject();
            body->setProperty("vocals_path", vocals);
            body->setProperty("drums_path", drums);
            body->setProperty("bass_path", bass);
            body->setProperty("other_path", other);
            return ipc::HttpServiceBridge{}.postAnalyzeJson("/analyze/stem_energy_timeline", juce::var(body.get()));
        },
        [](StemEnergyTimelinePanel& self, const juce::var& v) -> juce::String {
            self.stems_.clear();
            if (auto* st = v[af::StemEnergyTimelineResponse::stems].getArray())
                for (const auto& s : *st)
                    self.stems_.push_back(s.toString());
            self.frames_.clear();
            if (auto* fr = v[af::StemEnergyTimelineResponse::frames].getArray())
                for (const auto& f : *fr) {
                    std::vector<float> row;
                    const juce::var fracs = f[af::StemEnergyFrame::fractions];
                    for (const auto& nm : self.stems_)
                        row.push_back((float)(double)fracs[juce::Identifier(nm)]);
                    self.frames_.push_back(std::move(row));
                }
            return TRANS("Stem energy across the song.");
        });
}

void StemEnergyTimelinePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult() || frames_.empty()) {
        if (!run_.hasError())
            drawEmptyState(g, TRANS("No stem timeline yet"), TRANS("Requires separated stems."), 28);
        return;
    }
    auto b = bodyArea();
    b.removeFromTop(28);
    auto legend = b.removeFromBottom(14);
    auto plot = b;

    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRect(plot);

    const int n = (int)frames_.size();
    const int k = (int)stems_.size();
    if (n < 2 || k == 0) return;
    const float cw = (float)plot.getWidth() / (float)(n - 1);

    // Stacked area: for each stem layer, build a band polygon across time.
    std::vector<float> baseline((size_t)n, 0.0f);
    for (int si = 0; si < k; ++si) {
        juce::Path area;
        // top edge left->right
        for (int fi = 0; fi < n; ++fi) {
            const float x = plot.getX() + cw * fi;
            const float top = baseline[(size_t)fi] + frames_[(size_t)fi][(size_t)si];
            const float y = plot.getBottom() - juce::jlimit(0.0f, 1.0f, top) * plot.getHeight();
            if (fi == 0)
                area.startNewSubPath(x, y);
            else
                area.lineTo(x, y);
        }
        // bottom edge right->left
        for (int fi = n - 1; fi >= 0; --fi) {
            const float x = plot.getX() + cw * fi;
            const float y = plot.getBottom() - juce::jlimit(0.0f, 1.0f, baseline[(size_t)fi]) * plot.getHeight();
            area.lineTo(x, y);
        }
        area.closeSubPath();
        g.setColour(stemColour(stems_[(size_t)si]).withAlpha(0.75f));
        g.fillPath(area);
        for (int fi = 0; fi < n; ++fi)
            baseline[(size_t)fi] += frames_[(size_t)fi][(size_t)si];
    }

    // Legend.
    int lx = legend.getX();
    g.setFont(KhensuTheme::labelFont(8.5f));
    for (const auto& nm : stems_) {
        g.setColour(stemColour(nm));
        g.fillRect(lx, legend.getY() + 3, 8, 8);
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(nm, lx + 11, legend.getY(), 54, 14, juce::Justification::centredLeft, false);
        lx += 11 + 54;
    }
}

void StemEnergyTimelinePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    analyzeButton_.setBounds(controls.removeFromLeft(140));
    controls.removeFromLeft(8);
    run_.label().setBounds(controls);
    auto errArea = bodyArea();
    errArea.removeFromTop(28);
    run_.errorState().setBounds(errArea);
}

} // namespace khensu
