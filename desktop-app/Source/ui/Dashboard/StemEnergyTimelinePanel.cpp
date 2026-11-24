// desktop-app/Source/ui/Dashboard/StemEnergyTimelinePanel.cpp
#include "StemEnergyTimelinePanel.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include "../shared/PanelRunState.h"
#include "../shared/TimelineCache.h"
#include <khensu/common/Result.h>
#include <khensu/ipc/ILocalServiceBridge.h>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void StemEnergyTimelinePanel::applyThemeColours() {}

StemEnergyTimelinePanel::StemEnergyTimelinePanel(IAppController& c) : PanelCard(c, TRANS("Stem Energy Timeline"))
{
    applyThemeColours();
    setExplainer(TRANS("Stem Energy Timeline"),
                 TRANS("Each separated stem's energy across the song, stacked around a centre line - the "
                       "arrangement building and dropping: where the vocal enters, when the drums carry, the bass "
                       "drop. Below it, the masking map of a pair of stems: where both put comparable energy in the "
                       "same band at the same moment. Requires separated stems."),
                 "stem_taxonomy");

    analyzeButton_.onClick = [this] { runAnalysis(); };
    addAndMakeVisible(analyzeButton_);
    addChildComponent(stream_);
    pairBox_.setTitle(TRANS("Stem pair"));
    pairBox_.setTooltip(TRANS("Which two stems the masking map compares"));
    pairBox_.onChange = [this] { showPair(pairBox_.getSelectedItemIndex()); };
    addChildComponent(pairBox_);
    addChildComponent(masking_);
    run_.onRetry([this] { runAnalysis(); });
    run_.setIdle(TRANS("Requires separated stems (auto-runs once they exist)."));
}

void StemEnergyTimelinePanel::runAnalysis()
{
    if (!controller_.stems().hasStemsAvailable()) {
        run_.setIdle(TRANS("Requires separated stems - run Isolate Stems first."));
        return;
    }
    std::map<std::string, juce::String> paths;
    for (const auto& name : stem_viz::stemOrder())
        if (const auto f = controller_.getStemFile(name); f.existsAsFile()) paths[name] = f.getFullPathName();
    using Timelines = std::map<std::string, std::shared_ptr<const TrackTimeline>>;
    fetchThenPublish(
        *this, controller_, run_, TRANS("Measuring the stems..."),
        [paths]() -> Result<Timelines, ipc::BridgeError> {
            Timelines out;
            for (const auto& [name, path] : paths)
                if (auto tl = timeline_cache::measure(path)) out[name] = tl;
            if (out.size() < 2) {
                ipc::BridgeError e;
                e.kind = ipc::BridgeError::Kind::ParseError;
                e.message = "The stem files could not be measured.";
                return Result<Timelines, ipc::BridgeError>::err(e);
            }
            return Result<Timelines, ipc::BridgeError>::ok(std::move(out));
        },
        [](StemEnergyTimelinePanel& self, const Timelines& timelines) -> juce::String {
            self.timelines_ = timelines;
            self.stream_.setStream(stem_viz::stream(self.timelines_));
            // Every pair, the vocal ones first (the masking the ear notices).
            self.pairs_.clear();
            const auto& order = stem_viz::stemOrder();
            for (size_t i = 0; i < order.size(); ++i)
                for (size_t j = i + 1; j < order.size(); ++j)
                    if (self.timelines_.count(order[i]) != 0 && self.timelines_.count(order[j]) != 0)
                        self.pairs_.push_back({order[i], order[j]});
            self.pairBox_.clear(juce::dontSendNotification);
            for (size_t k = 0; k < self.pairs_.size(); ++k)
                self.pairBox_.addItem(juce::String(self.pairs_[k].first) + " / " + juce::String(self.pairs_[k].second),
                                      static_cast<int>(k) + 1);
            // Open on the worst-clashing pair.
            int worst = 0;
            float worstScore = -1.0f;
            for (size_t k = 0; k < self.pairs_.size(); ++k) {
                const auto m = stem_viz::masking(self.pairs_[k].first, *self.timelines_.at(self.pairs_[k].first),
                                                 self.pairs_[k].second, *self.timelines_.at(self.pairs_[k].second));
                if (m.worst > worstScore) {
                    worstScore = m.worst;
                    worst = static_cast<int>(k);
                }
            }
            self.pairBox_.setSelectedItemIndex(worst, juce::dontSendNotification);
            self.showPair(worst);
            self.stream_.setVisible(true);
            self.pairBox_.setVisible(!self.pairs_.empty());
            self.masking_.setVisible(!self.pairs_.empty());
            self.resized();
            return TRANS("Stem energy and masking across the song.");
        },
        FetchOptions{true});
}

void StemEnergyTimelinePanel::showPair(int index)
{
    if (index < 0 || index >= static_cast<int>(pairs_.size())) return;
    const auto& [a, b] = pairs_[static_cast<size_t>(index)];
    masking_.setMap(stem_viz::masking(a, *timelines_.at(a), b, *timelines_.at(b)));
}

void StemEnergyTimelinePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (!run_.showsResult() || timelines_.empty()) {
        if (!run_.hasError()) drawEmptyState(g, TRANS("No stem timeline yet"), TRANS("Requires separated stems."), 28);
        return;
    }
    if (masking_.isVisible() && masking_.map().worst > 0.0f) {
        auto caption = pairBox_.getBounds().withX(pairBox_.getRight() + KhensuTheme::sp3)
                           .withRight(getWidth() - KhensuTheme::sp3);
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
        ui::chart::Axis t, f;
        t.format = ui::chart::Format::Time;
        f.format = ui::chart::Format::Hz;
        g.drawText(TRANS("Worst clash at") + " " + t.label(masking_.map().worstSec, true) + ", "
                       + f.label(masking_.map().worstHz, true),
                   caption, juce::Justification::centredLeft, true);
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
    b.removeFromTop(KhensuTheme::sp1);
    auto errArea = b;
    run_.errorState().setBounds(errArea);
    if (stream_.isVisible()) {
        const int streamH = masking_.isVisible() ? b.getHeight() * 52 / 100 : b.getHeight();
        stream_.setBounds(b.removeFromTop(streamH));
        if (masking_.isVisible()) {
            b.removeFromTop(KhensuTheme::sp1);
            pairBox_.setBounds(b.removeFromTop(KhensuTheme::buttonHSmall).removeFromLeft(200));
            b.removeFromTop(KhensuTheme::sp1);
            masking_.setBounds(b);
        }
    }
}

} // namespace khensu
