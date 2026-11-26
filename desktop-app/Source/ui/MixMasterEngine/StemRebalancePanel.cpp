// desktop-app/Source/ui/MixMasterEngine/StemRebalancePanel.cpp
#include "StemRebalancePanel.h"
#include "../shared/ButtonRole.h"
#include "../shared/RenderPaths.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields;

StemRebalancePanel::StemRebalancePanel(IAppController& c) : PanelCard(c, TRANS("Stem Rebalance"))
{
    setExplainer(TRANS("Stem Rebalance"),
                 TRANS("Turn each stem up or down and render the re-summed mix as a new version. Suggest "
                       "moves each stem's share of the mix toward the genre's balance (at most 6 dB). With "
                       "every gain at 0 the render is the stems summed back: the residual says how closely "
                       "they still add up to the original."));
    for (size_t i = 0; i < kStems.size(); ++i) {
        names_[i].setText(juce::String(kStems[i]).substring(0, 1).toUpperCase() + juce::String(kStems[i]).substring(1),
                          juce::dontSendNotification);
        names_[i].setFont(KhensuTheme::labelFont(type::caption));
        addAndMakeVisible(names_[i]);
        auto& s = gains_[i];
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
        s.setRange(-12.0, 12.0, 0.5);
        s.setValue(0.0, juce::dontSendNotification);
        s.setDoubleClickReturnValue(true, 0.0);
        s.setTextValueSuffix(" dB");
        s.setTitle(TRANS("Gain for") + " " + juce::String(kStems[i]));
        addAndMakeVisible(s);
    }
    suggestButton_.setTooltip(TRANS("Gains that move each stem's share of the mix toward the genre's balance"));
    suggestButton_.onClick = [this] { suggest(); };
    addAndMakeVisible(suggestButton_);
    resetButton_.setTooltip(TRANS("All gains back to 0 dB"));
    resetButton_.onClick = [this] {
        for (auto& s : gains_)
            s.setValue(0.0, juce::dontSendNotification);
    };
    addAndMakeVisible(resetButton_);
    renderButton_.setTooltip(TRANS("Re-sum the stems at these gains into a new version of the mix"));
    renderButton_.onClick = [this] { render(); };
    button_role::markPrimary(renderButton_);
    addAndMakeVisible(renderButton_);
    run_.onRetry([this] { render(); });
    refreshAvailability();
    vocalListener_ = listen(controller_.stems(), this, &IStems::addCurrentVocalListener,
                            &IStems::removeCurrentVocalListener, [this] { refreshAvailability(); });
    separationListener_ = listen(controller_.stems(), this, &IStems::addVocalAnalysisListener,
                                 &IStems::removeVocalAnalysisListener, [this] { refreshAvailability(); });
}

void StemRebalancePanel::visibilityChanged()
{
    PanelCard::visibilityChanged();
    if (isShowing()) refreshAvailability();
}

std::map<std::string, std::string> StemRebalancePanel::stemPaths() const
{
    std::map<std::string, std::string> out;
    for (const char* name : kStems) {
        const auto f = controller_.getStemFile(name);
        if (f.existsAsFile()) out[name] = f.getFullPathName().toStdString();
    }
    // The vocal is the current one (WF-4): the Vocal Workspace's stacked
    // edits reach the mix through this re-sum - "commit to mix".
    const juce::String edited = controller_.currentVocalStem();
    if (edited.isNotEmpty() && out.count("vocals") != 0) out["vocals"] = edited.toStdString();
    return out;
}

void StemRebalancePanel::refreshAvailability()
{
    const bool has = stemPaths().size() >= 2;
    run_.setCanRun(has);
    suggestButton_.setEnabled(has);
    suggestButton_.setTooltip(has ? TRANS("Gains that move each stem's share of the mix toward the genre's balance")
                                  : TRANS("Needs separated stems - run Isolate Stems first."));
    if (!has && !run_.isBusy()) run_.setIdle(TRANS("Needs separated stems - run Isolate Stems first."));
    else if (!run_.isBusy() && !run_.showsResult() && !run_.hasError())
        run_.setIdle(controller_.currentVocalStem().isNotEmpty()
                         ? TRANS("Vocals: your edited vocal. Render to commit it to the mix.")
                         : TRANS("Set a gain per stem, or Suggest."));
}

void StemRebalancePanel::resetForSession()
{
    for (auto& s : gains_)
        s.setValue(0.0, juce::dontSendNotification);
    run_.reset();
    refreshAvailability();
}

void StemRebalancePanel::suggest()
{
    const auto stems = stemPaths();
    if (stems.size() < 2) return refreshAvailability();
    const auto* session = controller_.getActiveSession();
    const juce::String genre = session != nullptr ? juce::String(session->target.primary_genre) : juce::String();
    fetchThenPublish(
        *this, controller_, run_, TRANS("Measuring the stems..."),
        [stems, genre] { return ipc::HttpServiceBridge{}.stemRebalanceSuggest(stems, genre); },
        [](StemRebalancePanel& self, const juce::var& v) -> juce::String {
            if (!(bool)v[af::StemRebalanceSuggestResponse::available])
                return v[af::StemRebalanceSuggestResponse::reason].toString();
            const auto gains = v[af::StemRebalanceSuggestResponse::gains_db];
            for (size_t i = 0; i < kStems.size(); ++i)
                if (gains.hasProperty(kStems[i])) self.gains_[i].setValue((double)gains[kStems[i]]);
            return TRANS("Suggested toward the genre's stem balance - listen, then Render.");
        });
}

void StemRebalancePanel::render()
{
    const auto stems = stemPaths();
    if (stems.size() < 2) return refreshAvailability();
    std::map<std::string, double> gains;
    for (size_t i = 0; i < kStems.size(); ++i)
        if (stems.count(kStems[i])) gains[kStems[i]] = gains_[i].getValue();
    const juce::String original = controller_.activeSourcePath();
    const juce::String input = controller_.renders().renderInputPath();
    if (original.isEmpty()) return;
    const juce::String out = uniqueRenderFile(juce::File(original), "_rebalance").getFullPathName();
    const juce::String sessionId = controller_.getActiveSession() != nullptr
                                       ? juce::String(controller_.getActiveSession()->id)
                                       : juce::String();
    FetchOptions options;
    options.failedText = TRANS("Render failed.");
    fetchThenPublish(
        *this, controller_, run_, TRANS("Re-summing the stems..."),
        [stems, gains, out, original] { return ipc::HttpServiceBridge{}.stemRebalance(stems, gains, out, original); },
        [input, sessionId](StemRebalancePanel& self, const juce::var& v) -> juce::String {
            const auto path = v[af::StemRebalanceResponse::output_path].toString();
            // A new version of the mix, parented at the version it replaces (WF-1).
            self.controller_.renders().registerRenderOf(path, input, sessionId);
            juce::String text = TRANS("Rendered - now the current version (B).");
            const auto trim = (double)v[af::StemRebalanceResponse::trim_db];
            if (trim < -0.05) text << " " << TRANS("Trimmed") << " " << juce::String(trim, 1) << " dB "
                                   << TRANS("to keep -1 dBTP.");
            const auto residual = v[af::StemRebalanceResponse::residual_db];
            if (!residual.isVoid())
                text << " " << TRANS("Stems vs original:") << " " << juce::String((double)residual, 1) << " dB "
                     << TRANS("residual.");
            return text;
        },
        std::move(options));
}

void StemRebalancePanel::paint(juce::Graphics& g) { PanelCard::paint(g); }

void StemRebalancePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(KhensuTheme::buttonHSmall);
    renderButton_.setBounds(controls.removeFromRight(150));
    controls.removeFromRight(6);
    resetButton_.setBounds(controls.removeFromRight(70));
    controls.removeFromRight(6);
    suggestButton_.setBounds(controls.removeFromRight(80));
    b.removeFromTop(4);
    run_.label().setBounds(b.removeFromTop(18));
    b.removeFromTop(4);
    for (size_t i = 0; i < kStems.size(); ++i) {
        auto row = b.removeFromTop(24);
        names_[i].setBounds(row.removeFromLeft(70));
        gains_[i].setBounds(row);
        b.removeFromTop(2);
    }
    run_.errorState().setBounds(bodyArea().withTrimmedTop(KhensuTheme::buttonHSmall + 26));
}

} // namespace khensu
