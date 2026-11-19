// desktop-app/Source/ui/shared/CounterfactualView.cpp
#include "CounterfactualView.h"
#include "PanelRunState.h"
#include "../../IAppController.h"
#include "RenderPaths.h"
#include "ServiceErrorHint.h"
#include "SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <cmath>
#include <khensu/recommendations/SpectralRecommender.h>
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

void CounterfactualView::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    changeLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    detail_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgSurface);
    detail_.setColour(juce::TextEditor::textColourId, KhensuTheme::textSecondary);
    detail_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    for (auto* b : {&auditionButton_, &applyButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
}

CounterfactualView::Query CounterfactualView::queryFor(const Recommendation& rec)
{
    Query q;
    q.code = juce::String(issueCodeOf(rec));
    // Ids are "<code>_<file id>" and the file id is a UUID; drop that tail
    // when the card was built without a source_file_id.
    const int lastUnderscore = q.code.lastIndexOfChar('_');
    if (lastUnderscore > 0 && q.code.length() - lastUnderscore > 30) q.code = q.code.substring(0, lastUnderscore);
    const auto it = rec.suggested_parameters.find("eq_freq_hz");
    if (it != rec.suggested_parameters.end() && it->second > 0.0f)
        q.freqHz = (double)it->second;
    else if (rec.freq_range.has_value() && rec.freq_range->low_hz > 0.0f && rec.freq_range->high_hz > 0.0f)
        q.freqHz = std::sqrt((double)rec.freq_range->low_hz * (double)rec.freq_range->high_hz);
    return q;
}

CounterfactualView::CounterfactualView(IAppController& controller, const Recommendation& rec)
    : controller_(controller), query_(queryFor(rec))
{
    applyThemeColours();
    titleLabel_.setText(TRANS("What would fix this?"), juce::dontSendNotification);
    titleLabel_.setFont(KhensuTheme::labelFont(14.0f));
    addAndMakeVisible(titleLabel_);

    changeLabel_.setFont(KhensuTheme::metricFont(14.0f));
    changeLabel_.setText(TRANS("Searching for the smallest change..."), juce::dontSendNotification);
    addAndMakeVisible(changeLabel_);

    detail_.setMultiLine(true, true);
    detail_.setReadOnly(true);
    detail_.setCaretVisible(false);
    detail_.setFont(KhensuTheme::bodyFont(12.0f));
    detail_.setTitle(TRANS("Counterfactual fix details"));
    detail_.setText(TRANS("Each candidate is rendered through the processing chain and re-measured."));
    addAndMakeVisible(detail_);

    auditionButton_.setTooltip(TRANS("Play the verified preview at the mix's loudness"));
    applyButton_.setTooltip(TRANS("Render this change through the guarded render pipeline and make it the current "
                                  "render (undo returns to the original)"));
    auditionButton_.setEnabled(false);
    applyButton_.setEnabled(false);
    auditionButton_.onClick = [this] {
        if (previewPath_.isNotEmpty()) controller_.playback().auditionLevelMatchedPreview(juce::File(previewPath_));
    };
    applyButton_.onClick = [this] { apply(); };
    addAndMakeVisible(auditionButton_);
    addAndMakeVisible(applyButton_);

    setSize(420, 260);
    run();
}

void CounterfactualView::run()
{
    sourcePath_ = controller_.activeSourcePath();
    auto* session = controller_.getActiveSession();
    if (sourcePath_.isEmpty() || session == nullptr) {
        changeLabel_.setText(TRANS("No active session."), juce::dontSendNotification);
        return;
    }
    sessionId_ = juce::String(session->id);

    // The threshold the C++ recommender used, from the session genre entry,
    // so the search clears exactly the finding the card shows.
    std::optional<double> threshold;
    if (auto genre = controller_.genreDatabase().findById(session->target.primary_genre)) {
        const juce::String& c = query_.code;
        if (c == "lufs_too_loud") threshold = genre->lufs_range_max;
        else if (c == "lufs_too_quiet") threshold = genre->lufs_range_min;
        else if (c == "true_peak_over") threshold = genre->true_peak_max;
        else if (c == "tilt_too_bright")
            threshold = genre->spectral_tilt_target_db_per_octave + SpectralRecommender::kTiltToleranceDbPerOctave;
        else if (c == "tilt_too_dark")
            threshold = genre->spectral_tilt_target_db_per_octave - SpectralRecommender::kTiltToleranceDbPerOctave;
        else if (c == "centroid_too_bright") threshold = SpectralRecommender::centroidCeilingHz(*genre);
        else if (c == "centroid_too_dark") threshold = SpectralRecommender::centroidFloorHz(*genre);
        else if (c == "stereo_too_narrow") threshold = genre->stereo_width_min;
        else if (c == "stereo_too_wide") threshold = genre->stereo_width_max;
    }
    const juce::String output =
        uniqueRenderFile(juce::File(sourcePath_), "_cf_" + query_.code.substring(0, 24)).getFullPathName();

    const juce::String path = sourcePath_, code = query_.code;
    const auto freq = query_.freqHz;
    fetchQuietly(*this, controller_, [path, code, threshold, freq, output] {
        auto result = ipc::HttpServiceBridge{}.explainCounterfactual(path, code, threshold, freq, {}, output);
        return result;
    }, [](CounterfactualView& self, auto&& result) {
        if (result.is_err()) {
            const auto hint = classifyServiceError(result.error());
            self.changeLabel_.setText(TRANS("No counterfactual for this finding."), juce::dontSendNotification);
            self.detail_.setText(hint.message);
            return;
        }
        const juce::var v = result.value();
        const juce::String status = v[af::CounterfactualResponse::status].toString();
        const bool cleared = (bool)v[af::CounterfactualResponse::cleared];
        const bool verified = (bool)v[af::CounterfactualResponse::verified];
        self.changeLabel_.setText(status == "already_clear"  ? TRANS("Already within the threshold")
                                   : status == "unreachable" ? TRANS("No single-parameter fix")
                                                             : v[af::CounterfactualResponse::change_text].toString(),
                                   juce::dontSendNotification);
        juce::String text = v[af::CounterfactualResponse::explanation].toString();
        const juce::var side = v[af::CounterfactualResponse::side_effects];
        if (side.isObject() && !side[af::CounterfactualSideEffects::lufs_after].isVoid())
            text << "\n\n"
                 << TRANS("Side effects: loudness ")
                 << juce::String((double)side[af::CounterfactualSideEffects::lufs_before], 1) << " -> "
                 << juce::String((double)side[af::CounterfactualSideEffects::lufs_after], 1) << " LUFS, "
                 << TRANS("true peak ")
                 << juce::String((double)side[af::CounterfactualSideEffects::true_peak_before], 1) << " -> "
                 << juce::String((double)side[af::CounterfactualSideEffects::true_peak_after], 1) << " dBTP.";
        self.detail_.setText(text);
        const juce::String preview = v[af::CounterfactualResponse::preview_path].toString();
        self.chain_ = chainFrom(v[af::CounterfactualResponse::effects]);
        if (verified && cleared && preview.isNotEmpty() && juce::File(preview).existsAsFile()) {
            self.previewPath_ = preview;
            self.auditionButton_.setEnabled(true);
            self.applyButton_.setEnabled(!self.chain_.empty());
        }
    }, nullptr, true);
}

std::vector<ipc::HttpServiceBridge::EffectSpec> CounterfactualView::chainFrom(const juce::var& effects)
{
    std::vector<ipc::HttpServiceBridge::EffectSpec> chain;
    if (auto* arr = effects.getArray()) {
        for (const auto& e : *arr) {
            ipc::HttpServiceBridge::EffectSpec spec;
            spec.effect = e[af::CounterfactualEffect::effect].toString();
            if (spec.effect.isEmpty()) continue;
            if (auto* params = e[af::CounterfactualEffect::parameters].getDynamicObject())
                for (const auto& p : params->getProperties())
                    spec.parameters[p.name.toString()] = static_cast<float>(static_cast<double>(p.value));
            chain.push_back(std::move(spec));
        }
    }
    return chain;
}

void CounterfactualView::apply()
{
    // The preview proved the change clears the finding; the accepted render is
    // made fresh from the same chain through the normal apply path, so it
    // gets the guardrail, quarantine and provenance every render gets (the
    // service's preview WAV is never registered directly).
    if (chain_.empty()) return;
    auto* session = controller_.getActiveSession();
    if (session == nullptr || juce::String(session->id) != sessionId_) {
        changeLabel_.setText(TRANS("The session changed - ask again."), juce::dontSendNotification);
        applyButton_.setEnabled(false);
        return;
    }
    applyButton_.setEnabled(false);
    changeLabel_.setText(TRANS("Rendering through the render pipeline..."), juce::dontSendNotification);
    juce::Component::SafePointer<CounterfactualView> self(this);
    controller_.renders().applyEffectChain(chain_, "cf_" + query_.code, [self](bool ok, juce::String message) {
        if (self == nullptr) return;
        if (ok) {
            self->changeLabel_.setText(TRANS("Applied: ") + juce::File(message).getFileName(),
                                       juce::dontSendNotification);
            return;
        }
        self->changeLabel_.setText(TRANS("Not applied."), juce::dontSendNotification);
        self->detail_.setText(message);
        self->applyButton_.setEnabled(true);
    });
}

void CounterfactualView::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgRaised); }

void CounterfactualView::resized()
{
    auto b = getLocalBounds().reduced(10);
    titleLabel_.setBounds(b.removeFromTop(24));
    changeLabel_.setBounds(b.removeFromTop(26));
    b.removeFromTop(4);
    auto buttons = b.removeFromBottom(26);
    applyButton_.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(6);
    auditionButton_.setBounds(buttons.removeFromRight(90));
    b.removeFromBottom(6);
    detail_.setBounds(b);
}

} // namespace khensu
