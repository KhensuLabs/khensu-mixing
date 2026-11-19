// desktop-app/Source/ui/Dashboard/ReferenceComparePanel.cpp
#include "ReferenceComparePanel.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h" // full definition needed for setReferenceTrack()
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <thread>
#include "../shared/KhensuTheme.h"
#include "../shared/RenderPaths.h"
#include "../shared/SessionEpoch.h"
#include "../../RenderJobManager.h"

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void ReferenceComparePanel::applyThemeColours()
{
    matchButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.22f));
    matchButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    toggleCurveButton_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    toggleCurveButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    blindAbButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    blindAbButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    matchLoudnessWidthToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textSecondary);
}

ReferenceComparePanel::ReferenceComparePanel(IAppController& c) : PanelCard(c, "Reference Compare")
{
    applyThemeColours();
    setExplainer(TRANS("Reference"), TRANS("Compare your mix against a reference track. Load a reference audio file to see "
                              "per-dimension deltas for loudness, dynamics, spectrum, and stereo width."));
    loadButton_.setEnabled(false);
    loadButton_.onClick = [this] {
        if (targetFilePath_.isEmpty()) return;
        fileChooser_ = std::make_unique<juce::FileChooser>(
            TRANS("Select a reference track"), juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav,*.aiff,*.flac,*.mp3,*.aac,*.ogg");
        const auto epoch = controller_.sessionEpoch();
        fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, epoch](const juce::FileChooser& fc) {
                                      auto f = fc.getResult();
                                      if (epoch != controller_.sessionEpoch()) return; // session changed meanwhile
                                      if (f.existsAsFile()) {
                                          // Register the reference path with AppController so AnalysisController
                                          // can auto-populate reference_deltas on the next analysis run.
                                          controller_.analysis().setReferenceTrack(f.getFullPathName());
                                          refPath_ = f.getFullPathName();
                                          addBlindReference(refPath_);
                                          runReferenceAnalysis(f.getFullPathName());
                                      }
                                  });
    };

    addAndMakeVisible(loadButton_);

    matchButton_.setEnabled(false);
    matchButton_.setTooltip(TRANS("Render a corrective EQ that matches your mix's tonal balance "
                            "to the reference, and load it into A/B for comparison."));
    matchButton_.onClick = [this] { applyMatchEq(); };
    addAndMakeVisible(matchButton_);

    toggleCurveButton_.setEnabled(false);
    toggleCurveButton_.onClick = [this] {
        showCurve_ = !showCurve_;
        toggleCurveButton_.setButtonText(showCurve_ ? TRANS("Hide Curve") : TRANS("Edit Curve"));
        matchEqCurve_.setVisible(showCurve_);
        resized();
        repaint();
    };
    addAndMakeVisible(toggleCurveButton_);

    blindAbButton_.setEnabled(false);
    blindAbButton_.setTooltip(TRANS("Instant loudness-matched switching between your mix (A) and the "
                              "reference (B), with an optional blind trial. Level-matched, because "
                              "louder always sounds 'better' - this removes that bias."));
    blindAbButton_.onClick = [this] {
        if (blindReferencePaths_.isEmpty()) return;
        controller_.shell().showAbxTrial(blindReferencePaths_);
    };
    addAndMakeVisible(blindAbButton_);

    addBlindReferenceButton_.setEnabled(false);
    addBlindReferenceButton_.setTooltip(TRANS("Add another reference to the blind-test set. Each trial hides which one is B."));
    addBlindReferenceButton_.onClick = [this] {
        fileChooser_ = std::make_unique<juce::FileChooser>(
            TRANS("Add a reference for blind testing"), juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav,*.aiff,*.flac,*.mp3,*.aac,*.ogg");
        const auto epoch = controller_.sessionEpoch();
        fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this, epoch](const juce::FileChooser& fc) {
                                      const auto file = fc.getResult();
                                      if (epoch != controller_.sessionEpoch()) return;
                                      if (file.existsAsFile()) addBlindReference(file.getFullPathName());
                                  });
    };
    addAndMakeVisible(addBlindReferenceButton_);

    blindSetLabel_.setFont(khensu::KhensuTheme::uiFont(9.5f, juce::Font::plain));
    blindSetLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    addAndMakeVisible(blindSetLabel_);
    updateBlindReferenceControls();

    matchLoudnessWidthToggle_.setToggleState(true, juce::dontSendNotification);
    matchLoudnessWidthToggle_.setTooltip(TRANS("Also trim gain toward the reference's LUFS and nudge "
                                         "stereo width toward its correlation when applying."));
    addAndMakeVisible(matchLoudnessWidthToggle_);

    matchEqCurve_.setVisible(false);
    matchEqCurve_.onBandChanged([this](int index, const EqBand& band) {
        // Frequency and Q drags were drawn but dropped (only gain was kept).
        if (index >= 0 && index < (int)matchEq_.size()) {
            auto& b = matchEq_[(size_t)index];
            b.gain_db = band.gain_db;
            b.center_hz = band.hz;
            b.q = band.q;
        }
    });
    addChildComponent(matchEqCurve_);

    errorState_.onRetry([this] {
        if (refPath_.isNotEmpty() && juce::File(refPath_).existsAsFile()) runReferenceAnalysis(refPath_);
    });
    addChildComponent(errorState_);
}

void ReferenceComparePanel::resetForSession()
{
    ++requestGeneration_; // an in-flight comparison of the old session is dropped
    fileChooser_.reset();
    targetFilePath_.clear();
    matchEqTargetPath_.clear();
    refPath_.clear();
    blindReferencePaths_.clear();
    deltas_.clear();
    matchEq_.clear();
    lufsDelta_ = 0.0f;
    stereoCorrelationDelta_ = 0.0f;
    loading_ = false;
    errorMsg_ = {};
    errorState_.setVisible(false);
    showCurve_ = false;
    matchEqCurve_.setBands({});
    matchEqCurve_.setVisible(false);
    toggleCurveButton_.setButtonText(TRANS("Edit Curve"));
    toggleCurveButton_.setEnabled(false);
    matchButton_.setEnabled(false);
    matchButton_.setButtonText(TRANS("Apply Match EQ"));
    loadButton_.setEnabled(false);
    addBlindReferenceButton_.setEnabled(false);
    updateBlindReferenceControls();
    resized();
    repaint();
}

void ReferenceComparePanel::applyMatchEq()
{
    if (matchEq_.empty() || targetFilePath_.isEmpty()) return;
    // WIRE-07: the curve must have been computed for THIS target.
    if (matchEqTargetPath_.isNotEmpty() && juce::File(matchEqTargetPath_) != juce::File(targetFilePath_)) {
        matchButton_.setButtonText(TRANS("Re-run comparison"));
        matchButton_.setEnabled(false);
        return;
    }

    // The match curve was measured on the session's file; the render
    // processes the current version (WF-1: fixes stack) unless the user
    // chose the original.
    const juce::File src(targetFilePath_);
    const juce::File out = uniqueRenderFile(src, "_matched");
    const juce::String renderInput = controller_.renders().renderInputPath();

    // Build a single-branch graph of peak filters (one per octave band with a
    // non-trivial gain). Uses matchEq_ as the live source-of-truth, which
    // reflects any user drag-edits made on matchEqCurve_ before applying.
    juce::Array<juce::var> effects;
    for (const auto& b : matchEq_) {
        if (std::abs(b.gain_db) < 0.3f) continue; // skip negligible bands
        juce::DynamicObject::Ptr params = new juce::DynamicObject();
        params->setProperty("cutoff_frequency_hz", b.center_hz);
        params->setProperty("gain_db", b.gain_db);
        params->setProperty("q", b.q);
        juce::DynamicObject::Ptr eff = new juce::DynamicObject();
        eff->setProperty("effect", "PeakFilter");
        eff->setProperty("parameters", juce::var(params.get()));
        effects.add(juce::var(eff.get()));
    }

    // Optional loudness + width matching, applied as post-chain stages after
    // the tonal-balance EQ so they act on the already-EQ'd signal.
    juce::Array<juce::var> post;
    if (matchLoudnessWidthToggle_.getToggleState()) {
        if (std::abs(lufsDelta_) > 0.3f) {
            // delta = target - reference; a positive delta means the target
            // is louder, so it needs to come DOWN by that amount to match.
            juce::DynamicObject::Ptr gainParams = new juce::DynamicObject();
            gainParams->setProperty("gain_db", -lufsDelta_);
            juce::DynamicObject::Ptr gainEff = new juce::DynamicObject();
            gainEff->setProperty("effect", "Gain");
            gainEff->setProperty("parameters", juce::var(gainParams.get()));
            post.add(juce::var(gainEff.get()));
        }
        if (std::abs(stereoCorrelationDelta_) > 0.03f) {
            // Higher correlation = narrower/more mono; a positive delta means
            // the target is narrower than the reference, so widen it (and
            // vice versa). Scale chosen so a full +/-1.0 correlation delta
            // (the theoretical max) maps to a hard but sane +/-1.0 width move.
            const float width = juce::jlimit(0.2f, 2.0f, 1.0f + stereoCorrelationDelta_ * 1.0f);
            juce::DynamicObject::Ptr widthParams = new juce::DynamicObject();
            widthParams->setProperty("width", width);
            juce::DynamicObject::Ptr widthEff = new juce::DynamicObject();
            widthEff->setProperty("effect", "StereoWidener");
            widthEff->setProperty("parameters", juce::var(widthParams.get()));
            post.add(juce::var(widthEff.get()));
        }
    }

    if (effects.isEmpty() && post.isEmpty()) {
        matchButton_.setButtonText(TRANS("Already matched"));
        return;
    }

    juce::DynamicObject::Ptr branch = new juce::DynamicObject();
    branch->setProperty("effects", effects);
    branch->setProperty("mix", 1.0);
    juce::Array<juce::var> branches;
    branches.add(juce::var(branch.get()));

    matchButton_.setEnabled(false);
    matchButton_.setButtonText(TRANS("Rendering..."));

    juce::String sessionId;
    if (const auto* s = controller_.getActiveSession()) sessionId = juce::String(s->id);

    juce::Component::SafePointer<ReferenceComparePanel> self(this);
    const juce::String inPath = renderInput.isNotEmpty() ? renderInput : src.getFullPathName();
    const juce::String outPath = out.getFullPathName();
    const juce::var branchesVar(branches);
    const juce::var postVar(post);
    controller_.renderJobs().startGraphRender(
        "Match EQ -> " + out.getFileName(), inPath, outPath, branchesVar, postVar, /*normalize*/ false, sessionId,
        "reference_match_eq",
        [self, outPath, inPath, sessionId](bool ok, const juce::String& pathOrError, const juce::var&) {
            if (self == nullptr) return;
            self->matchButton_.setEnabled(true);
            if (!ok) {
                if (pathOrError.isNotEmpty()) {
                    self->matchButton_.setButtonText(TRANS("Match failed"));
                    self->controller_.shell().showServiceError(TRANS("Match EQ render failed:") + " " + pathOrError);
                } else {
                    self->matchButton_.setButtonText(TRANS("Apply Match EQ"));
                }
                return;
            }
            self->matchButton_.setButtonText(TRANS("Apply Match EQ"));
            // Session-checked: the match render must not land in a session
            // opened while it ran (AUDIT-002).
            self->controller_.renders().registerRenderOf(outPath, inPath, sessionId);
        });
}

void ReferenceComparePanel::setTargetFilePath(const juce::String& path)
{
    if (targetFilePath_.isNotEmpty() && path != targetFilePath_) {
        // A different target: the old deltas and curve describe another file.
        ++requestGeneration_;
        deltas_.clear();
        matchEq_.clear();
        matchEqTargetPath_.clear();
        matchButton_.setEnabled(false);
        toggleCurveButton_.setEnabled(false);
    }
    targetFilePath_ = path;
    loadButton_.setEnabled(!path.isEmpty());
    addBlindReferenceButton_.setEnabled(!path.isEmpty());
}

void ReferenceComparePanel::addBlindReference(const juce::String& path)
{
    if (path.isEmpty() || blindReferencePaths_.contains(path)) return;
    blindReferencePaths_.add(path);
    // DSK-08: measure its loudness now, so the blind trial never has to
    // play a muted (still-measuring) B exactly when the answer is B.
    controller_.playback().primeAbLoudness(juce::StringArray(path), [](bool) {});
    updateBlindReferenceControls();
}

void ReferenceComparePanel::updateBlindReferenceControls()
{
    const int count = blindReferencePaths_.size();
    blindSetLabel_.setText(count == 0 ? TRANS("Blind set: add one or more references")
                                      : TRANS("Blind set:") + " " + juce::String(count) + " " +
                                            (count == 1 ? TRANS("reference") : TRANS("references")),
                           juce::dontSendNotification);
    blindAbButton_.setEnabled(count > 0);
    blindAbButton_.setButtonText(count > 1 ? TRANS("Blind Test") + " (" + juce::String(count) + ")"
                                           : TRANS("Blind A/B vs Ref"));
}

void ReferenceComparePanel::setData(const AnalysisResult& r)
{
    if (!r.reference_deltas.has_value()) return;
    deltas_.clear();
    // Map dimension name to display label + unit
    struct DimMeta {
        const char* key;
        const char* label;
        const char* unit;
    };
    static constexpr DimMeta meta[] = {
        {"lufs", "LUFS", " dB"},
        {"lra", "LRA", " LU"},
        {"crest_db", "CREST", " dB"},
        {"centroid_hz", "CENTROID", " Hz"},
        {"stereo_correlation", "CORRELATION", ""},
        {"balance_low", "LOW END", ""},
    };
    for (const auto& d : r.reference_deltas->deltas) {
        DeltaRow row;
        row.dim = juce::String(d.dimension).toUpperCase();
        // Find matching meta for unit
        juce::String unit;
        for (const auto& m : meta)
            if (d.dimension == m.key) {
                row.dim = m.label;
                unit = m.unit;
                break;
            }
        row.targetStr = juce::String(d.target, 1) + unit;
        row.refStr = juce::String(d.reference, 1) + unit;
        row.delta = d.delta;
        row.deltaStr = (d.delta >= 0 ? "+" : "") + juce::String(d.delta, 1) + unit;
        deltas_.push_back(row);
        if (d.dimension == "lufs")
            lufsDelta_ = d.delta;
        else if (d.dimension == "stereo_correlation")
            stereoCorrelationDelta_ = d.delta;
    }
    matchEq_ = r.reference_deltas->match_eq;
    matchEqTargetPath_ = targetFilePath_; // the analysis ran on the session's own file
    matchButton_.setEnabled(!matchEq_.empty() && !targetFilePath_.isEmpty());
    toggleCurveButton_.setEnabled(!matchEq_.empty());

    std::vector<EqBand> curveBands;
    curveBands.reserve(matchEq_.size());
    for (const auto& b : matchEq_)
        curveBands.push_back({b.center_hz, b.gain_db, b.q, /*is_suggested*/ true, /*enabled*/ true});
    matchEqCurve_.setBands(std::move(curveBands));

    loading_ = false;
    errorMsg_ = {};
    errorState_.setVisible(false);
    resized();
    repaint();
}

void ReferenceComparePanel::runReferenceAnalysis(const juce::String& refPath)
{
    loading_ = true;
    errorMsg_ = {};
    errorState_.setVisible(false);
    deltas_.clear();
    resized();
    repaint();

    juce::Component::SafePointer<ReferenceComparePanel> safeThis(this);
    auto target = targetFilePath_.toStdString();
    auto ref = refPath.toStdString();
    const int generation = ++requestGeneration_;

    controller_.runLongInBackground([safeThis, &work = controller_, target, ref, generation]() mutable {
        std::vector<DeltaRow> rows;
        std::vector<MatchEqBand> matchEq;
        float lufsDelta = 0.0f, corrDelta = 0.0f;
        juce::String error, errorRemedy;
        try {
            // Authenticated request via the bridge - a raw juce::URL here
            // previously got HTTP 401 and rendered a table of zeros.
            ipc::HttpServiceBridge bridge;
            auto result = bridge.analyzeReference(target, ref);
            if (result.is_err()) {
                const auto hint = classifyServiceError(result.error());
                error = hint.message;
                errorRemedy = hint.remedy;
            } else {
                matchEq = result.value().match_eq;
                struct DimMeta {
                    const char* key;
                    const char* label;
                    const char* unit;
                };
                static constexpr DimMeta meta[] = {
                    {"lufs", "LUFS", " dB"},
                    {"lra", "LRA", " LU"},
                    {"crest_db", "CREST", " dB"},
                    {"centroid_hz", "CENTROID", " Hz"},
                    {"stereo_correlation", "CORRELATION", ""},
                    {"balance_low", "LOW END", ""},
                };
                for (const auto& d : result.value().deltas) {
                    const char* label = d.dimension.c_str();
                    const char* unit = "";
                    for (const auto& m : meta) {
                        if (d.dimension == m.key) {
                            label = m.label;
                            unit = m.unit;
                            break;
                        }
                    }
                    DeltaRow row;
                    row.dim = label;
                    row.targetStr = juce::String(d.target, 1) + unit;
                    row.refStr = juce::String(d.reference, 1) + unit;
                    row.delta = d.delta;
                    row.deltaStr = (d.delta >= 0 ? "+" : "") + juce::String(d.delta, 1) + unit;
                    rows.push_back(row);
                    if (d.dimension == "lufs")
                        lufsDelta = d.delta;
                    else if (d.dimension == "stereo_correlation")
                        corrDelta = d.delta;
                }
            }
        } catch (...) {
            error = TRANS("The reference comparison failed unexpectedly.");
            errorRemedy = TRANS("Retry; if it persists, run \"Diagnose analysis service\".");
        }
        postIfCurrent(safeThis, work, [rows = std::move(rows), matchEq = std::move(matchEq), lufsDelta, corrDelta, error,
                                       errorRemedy, target, generation](ReferenceComparePanel& panel) mutable {
            // DSK-04 / WIRE-07: drop a result for a superseded session, a
            // newer comparison, or a target that is no longer current.
            if (generation != panel.requestGeneration_) return;
            if (juce::String(target) != panel.targetFilePath_) {
                panel.loading_ = false;
                panel.repaint();
                return;
            }
            panel.matchEqTargetPath_ = juce::String(target);
            panel.deltas_ = std::move(rows);
            panel.matchEq_ = std::move(matchEq);
            panel.lufsDelta_ = lufsDelta;
            panel.stereoCorrelationDelta_ = corrDelta;
            panel.matchButton_.setEnabled(!panel.matchEq_.empty() && !panel.targetFilePath_.isEmpty());
            panel.toggleCurveButton_.setEnabled(!panel.matchEq_.empty());

            std::vector<EqBand> curveBands;
            curveBands.reserve(panel.matchEq_.size());
            for (const auto& b : panel.matchEq_)
                curveBands.push_back({b.center_hz, b.gain_db, b.q, /*is_suggested*/ true, /*enabled*/ true});
            panel.matchEqCurve_.setBands(std::move(curveBands));

            panel.loading_ = false;
            panel.errorMsg_ = error;
            if (error.isNotEmpty()) panel.errorState_.setError(error, errorRemedy);
            panel.errorState_.setVisible(error.isNotEmpty());
            panel.resized();
            panel.repaint();
        });
    });
}

void ReferenceComparePanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto b = bodyArea().withTrimmedBottom(82); // leave room for blind-set, buttons, and options

    if (showCurve_) return;              // matchEqCurve_ draws itself over this area
    if (errorState_.isVisible()) return; // KhensuErrorState draws itself over this area

    if (loading_) {
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
        g.drawText(TRANS("Analyzing reference..."), b, juce::Justification::centred);
        return;
    }

    if (deltas_.empty()) {
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
        juce::String msg = targetFilePath_.isEmpty() ? TRANS("Load a session to enable reference comparison.")
                                                     : TRANS("Load a reference track to compare against your mix.");
        g.drawText(msg, b, juce::Justification::centred, true);
        return;
    }

    // Spectral-difference summary: the match-EQ curve IS the per-band gap
    // between this mix and the reference. Surface the single worst band in
    // plain language + a diverging strip across the frequency axis, so the
    // headline tonal problem reads instantly without opening the curve editor.
    if (!matchEq_.empty()) {
        auto bands = matchEq_;
        std::sort(bands.begin(), bands.end(),
                  [](const MatchEqBand& x, const MatchEqBand& y) { return x.center_hz < y.center_hz; });

        // Worst = largest corrective |gain|. gain_db > 0 boosts (mix is weak
        // there); < 0 cuts (mix is hot there).
        const MatchEqBand* worst = &bands.front();
        for (const auto& bd : bands)
            if (std::abs(bd.gain_db) > std::abs(worst->gain_db)) worst = &bd;

        auto fmtHz = [](float hz) {
            return hz >= 1000.0f ? juce::String(hz / 1000.0f, 1) + " kHz" : juce::String((int)std::lround(hz)) + " Hz";
        };

        auto callout = b.removeFromTop(15);
        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
        if (std::abs(worst->gain_db) < 0.5f) {
            g.setColour(KhensuTheme::accentTeal);
            g.drawText(TRANS("Tonal balance closely matches the reference."), callout,
                       juce::Justification::centredLeft, true);
        } else {
            g.setColour(worst->gain_db < 0.0f ? KhensuTheme::accentAmber : KhensuTheme::accentTeal);
            const juce::String verb = worst->gain_db < 0.0f ? "hot" : "shy";
            g.drawText("Biggest gap: mix is " + juce::String(std::abs(worst->gain_db), 1) + " dB " + verb + " at " +
                           fmtHz(worst->center_hz) + " vs reference.",
                       callout, juce::Justification::centredLeft, true);
        }

        // Diverging strip: low->high freq left->right; teal = mix hot (needs
        // cut), amber = mix shy (needs boost); alpha scales with magnitude.
        auto strip = b.removeFromTop(10);
        const float cellW = (float)strip.getWidth() / (float)bands.size();
        float x = (float)strip.getX();
        for (const auto& bd : bands) {
            const float mag = juce::jlimit(0.0f, 1.0f, std::abs(bd.gain_db) / 6.0f);
            const juce::Colour c = (bd.gain_db < 0.0f ? KhensuTheme::accentTeal : KhensuTheme::accentAmber);
            g.setColour(c.withAlpha(0.15f + 0.65f * mag));
            g.fillRect(x, (float)strip.getY(), cellW - 1.0f, (float)strip.getHeight());
            x += cellW;
        }
        b.removeFromTop(4);
    }

    // Delta table header
    auto headerRow = b.removeFromTop(16);
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
    g.drawText("DIM", headerRow.removeFromLeft(70), juce::Justification::centredLeft);
    g.drawText("YOUR", headerRow.removeFromLeft(54), juce::Justification::centred);
    g.drawText("REF", headerRow.removeFromLeft(54), juce::Justification::centred);
    g.drawText("DELTA", headerRow, juce::Justification::centredRight);
    b.removeFromTop(2);

    // Separator
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(b.getY(), (float)b.getX(), (float)b.getRight());
    b.removeFromTop(4);

    const int rowH = juce::jmax(16, b.getHeight() / (int)deltas_.size());
    for (const auto& row : deltas_) {
        auto r = b.removeFromTop(rowH);

        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(row.dim, r.removeFromLeft(70), juce::Justification::centredLeft);

        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::plain));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(row.targetStr, r.removeFromLeft(54), juce::Justification::centred);
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(row.refStr, r.removeFromLeft(54), juce::Justification::centred);

        const juce::Colour dc = std::abs(row.delta) < 0.5f   ? KhensuTheme::accentTeal
                                : std::abs(row.delta) < 2.0f ? KhensuTheme::accentAmber
                                                             : KhensuTheme::accentRed;

        // Mini delta bar behind the text - width proportional to magnitude (max at 5 units)
        {
            const float barNorm = juce::jlimit(0.0f, 1.0f, std::abs(row.delta) / 5.0f);
            const float barW2 = (float)r.getWidth() * barNorm;
            const float barX = row.delta >= 0.0f ? (float)r.getX() : (float)r.getRight() - barW2;
            g.setColour(dc.withAlpha(0.18f));
            g.fillRoundedRectangle(barX, (float)r.getY() + 1.0f, barW2, (float)r.getHeight() - 2.0f, 2.0f);
        }
        g.setColour(dc);
        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::bold));
        g.drawText(row.deltaStr, r, juce::Justification::centredRight);
    }
}

void ReferenceComparePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();

    auto optionsRow = b.removeFromBottom(20);
    toggleCurveButton_.setBounds(optionsRow.removeFromLeft(80));
    matchLoudnessWidthToggle_.setBounds(optionsRow.removeFromRight(140));

    auto buttonRow = b.removeFromBottom(28);
    const int thirdW = buttonRow.getWidth() / 3 - 4;
    matchButton_.setBounds(buttonRow.removeFromRight(thirdW));
    buttonRow.removeFromRight(6);
    blindAbButton_.setBounds(buttonRow.removeFromRight(thirdW));
    buttonRow.removeFromRight(6);
    loadButton_.setBounds(buttonRow);

    b.removeFromBottom(4);
    auto blindSetRow = b.removeFromBottom(22);
    addBlindReferenceButton_.setBounds(blindSetRow.removeFromRight(150));
    blindSetRow.removeFromRight(6);
    blindSetLabel_.setBounds(blindSetRow);

    matchEqCurve_.setBounds(b);
    errorState_.setBounds(b); // fills the table area, above the button rows
}

} // namespace khensu
