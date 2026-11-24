// desktop-app/Source/ui/VocalWorkspace/CompingPanel.cpp
#include "../shared/ServiceErrorHint.h"
#include "CompingPanel.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/RenderPaths.h"
#include "../shared/SessionAudioPaths.h"
#include "TakesBin.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every cached palette colour on this component, in one place (UI-01).
void CompingPanel::applyThemeColours()
{
    chooseButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    chooseButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    renderButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    renderButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

CompingPanel::CompingPanel(IAppController& c) : PanelCard(c, "Comping Assistant")
{
    applyThemeColours();
    setExplainer(TRANS("Comping Assistant"), TRANS("Modern vocal production comps the lead from many takes - often a "
                                      "different take per phrase. Load 2+ takes recorded against the same backing "
                                      "track and each phrase gets scored per take on pitch stability, level "
                                      "consistency, and timbre - the grid shows every score, the ring "
                                      "marks the proposed winner.\n\n"
                                      "Render Comp assembles the winners with equal-power crossfades and loads it "
                                      "into the transport for preview. The proposal is a starting point: emotion beats "
                                      "metrics, so audition before you commit."));

    chooseButton_.onClick = [this] { chooseTakes(); };
    addAndMakeVisible(chooseButton_);

    renderButton_.setEnabled(false);
    renderButton_.onClick = [this] { runAnalysis(true); };
    addAndMakeVisible(renderButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    addAndMakeVisible(statusLabel_);
    showBinStatus();
    takesListener_ = listen(controller_.vocalEdit(), this, &IVocalEdit::addTakesListener,
                            &IVocalEdit::removeTakesListener, [this] { showBinStatus(); });
}

void CompingPanel::chooseTakes()
{
    // From the session's Takes bin, or files from disk that then join it (WF-5).
    juce::Component::SafePointer<CompingPanel> self(this);
    takes_bin::pick(*this, chooseButton_, controller_.vocalEdit(), chooser_,
                    TRANS("Select 2+ vocal takes (same performance)"), takes_bin::Pick::Many,
                    [self](const juce::StringArray& chosen) {
                        if (self == nullptr) return;
                        if (chosen.size() < 2) {
                            self->statusLabel_.setText(TRANS("Need at least 2 takes."), juce::dontSendNotification);
                            return;
                        }
                        self->takePaths_ = chosen;
                        self->runAnalysis(false);
                    });
}

void CompingPanel::showBinStatus()
{
    // Every pick joins the bin, so an emptied bin (a new session) clears the
    // takes loaded here; otherwise a scored comp keeps its own status.
    if (controller_.vocalEdit().sessionTakes().isEmpty() && !takePaths_.isEmpty()) {
        takePaths_.clear();
        phrases_.clear();
        takeCount_ = 0;
        renderButton_.setEnabled(false);
        repaint();
    }
    if (!takePaths_.isEmpty()) return;
    const int n = controller_.vocalEdit().sessionTakes().size();
    statusLabel_.setText(n >= 2 ? TRANS("%n takes in the session's Takes bin - Load Takes... to score them.")
                                      .replace("%n", juce::String(n))
                                : TRANS("Load 2+ takes of the same performance."),
                         juce::dontSendNotification);
}

void CompingPanel::runAnalysis(bool render)
{
    if (takePaths_.size() < 2) return;
    statusLabel_.setText(render ? TRANS("Rendering comp...") : TRANS("Scoring takes..."), juce::dontSendNotification);
    chooseButton_.setEnabled(false);
    renderButton_.setEnabled(false);

    juce::Array<juce::var> paths;
    for (const auto& p : takePaths_)
        paths.add(p);
    // A fresh output name per render: the service's fixed "_comp.wav"
    // overwrote renders still being auditioned (UIX-11).
    const juce::String outPath =
        render ? uniqueRenderFile(juce::File(takePaths_[0]), "_comp").getFullPathName() : juce::String();

    juce::Component::SafePointer<CompingPanel> self(this);
    // Scoring decodes every take and a render crossfades them all: long pool (UIX-14).
    controller_.runLongInBackground([self, &work = controller_, paths, render, outPath]() {
        juce::DynamicObject::Ptr body = new juce::DynamicObject();
        body->setProperty(af::CompingRequest::take_paths, paths);
        body->setProperty(af::CompingRequest::render, render);
        if (outPath.isNotEmpty()) body->setProperty(af::CompingRequest::output_path, outPath); // fresh name per render (UIX-11)
        ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 300000);
        auto result = bridge.postAnalyzeJson("/analyze/comping", juce::var(body.get()));
        postIfCurrent(self, work, [result, render](CompingPanel& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            panel.phrases_.clear();
            panel.takeCount_ = (int)v[af::CompingResponse::take_count];
            if (auto* arr = v[af::CompingResponse::phrases].getArray()) {
                for (const auto& pv : *arr) {
                    Phrase p;
                    p.start = (float)(double)pv[af::CompingPhrase::start_sec];
                    p.end = (float)(double)pv[af::CompingPhrase::end_sec];
                    p.best = (int)pv[af::CompingPhrase::best_take];
                    if (auto* takes = pv[af::CompingPhrase::takes].getArray())
                        for (const auto& tv : *takes)
                            p.totals.push_back((float)(double)tv[af::CompingTakeScore::total]);
                    panel.phrases_.push_back(p);
                }
            }
            panel.renderButton_.setEnabled(!panel.phrases_.empty());

            if (render) {
                const juce::String rendered = v[af::CompingResponse::rendered_path].toString();
                if (rendered.isNotEmpty() && juce::File(rendered).existsAsFile()) {
                    // A comp of vocal takes is not a version of the MIX: it is
                    // auditioned directly and never becomes the master (UIX-01).
                    session_audio::applyRender(panel.controller_, rendered, {}, session_audio::RenderScope::Stem);
                    panel.statusLabel_.setText(TRANS("Comp rendered and loaded for preview - the mix is unchanged."),
                                               juce::dontSendNotification);
                } else {
                    panel.statusLabel_.setText(TRANS("Render produced no file."), juce::dontSendNotification);
                }
            } else {
                panel.statusLabel_.setText(TRANS("%p phrases scored across %t takes.")
                                               .replace("%p", juce::String((int)panel.phrases_.size()))
                                               .replace("%t", juce::String(panel.takeCount_)),
                                           juce::dontSendNotification);
            }
            panel.repaint();
        }, [](CompingPanel& panel) {
            // Busy state first - on error and on a stale result too (UIX-17).
            panel.chooseButton_.setEnabled(true);
            panel.renderButton_.setEnabled(!panel.phrases_.empty());
        });
    });
}

void CompingPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (phrases_.empty() || takeCount_ == 0) return;

    auto b = bodyArea();
    b.removeFromTop(28);
    b.removeFromTop(16);
    auto grid = b.toFloat().reduced(2.0f, 4.0f);

    const float labelW = 52.0f;
    const int nP = (int)phrases_.size();
    const float rowH = grid.getHeight() / (float)takeCount_;
    const float cellW = (grid.getWidth() - labelW) / (float)nP;

    for (int t = 0; t < takeCount_; ++t) {
        const float y = grid.getY() + t * rowH;
        g.setFont(KhensuTheme::labelFont(8.5f));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText("TAKE " + juce::String(t + 1),
                   juce::Rectangle<float>(grid.getX(), y, labelW - 4.0f, rowH).toNearestInt(),
                   juce::Justification::centredLeft, false);

        for (int p = 0; p < nP; ++p) {
            const auto& phrase = phrases_[(size_t)p];
            const float score = t < (int)phrase.totals.size() ? phrase.totals[(size_t)t] : 0.0f;
            juce::Rectangle<float> cell(grid.getX() + labelW + p * cellW, y + 1.5f, juce::jmax(2.0f, cellW - 2.0f),
                                        rowH - 3.0f);
            g.setColour(KhensuTheme::bgRaised);
            g.fillRoundedRectangle(cell, 3.0f);
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.15f + 0.75f * juce::jlimit(0.0f, 1.0f, score)));
            g.fillRoundedRectangle(cell, 3.0f);
            if (phrase.best == t) {
                g.setColour(KhensuTheme::accentTeal);
                g.drawRoundedRectangle(cell, 3.0f, 1.6f);
            }
        }
    }
}

void CompingPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    chooseButton_.setBounds(controls.removeFromLeft(110));
    controls.removeFromLeft(6);
    renderButton_.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(8);
    statusLabel_.setBounds(controls);
}

} // namespace khensu
