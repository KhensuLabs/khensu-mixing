// desktop-app/Source/ui/shared/GuardrailText.h
#pragma once

// One-line descriptions of a render's safety guardrail (the service's
// RenderGuardrail payload: render_guardrails.post_render_guardrail, or a
// {"pre_render": ...} refusal). Shared by the mastering-agent and the
// section-intensity panels, which used to carry identical copies.

#include <JuceHeader.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu::guardrail_text {

/// True when the guardrail ran and passed. A missing guardrail never passes.
inline bool passed(const juce::var& guardrail)
{
    const auto* obj = guardrail.getDynamicObject();
    return obj != nullptr && static_cast<bool>(obj->getProperty(api::fields::RenderGuardrail::ok));
}

/// "Safety checks passed." / "Safety guardrail blocked promotion: <blocks; warnings>".
inline juce::String summary(const juce::var& guardrail)
{
    namespace rg = api::fields::RenderGuardrail;
    auto* obj = guardrail.getDynamicObject();
    if (obj == nullptr) return "Safety result was not returned; output was not promoted.";
    juce::StringArray details;
    const auto append = [&details](const juce::var& values) {
        if (auto* array = values.getArray())
            for (const auto& value : *array)
                details.add(value.toString());
    };
    append(obj->getProperty(rg::blocks));
    append(obj->getProperty(rg::warnings));
    if (const auto* pre = obj->getProperty(rg::pre_render).getDynamicObject())
        append(pre->getProperty(api::fields::PreRenderValidation::blocks));
    const auto artifactDelta = obj->getProperty(rg::artifact_score_delta);
    if (!artifactDelta.isVoid() && !artifactDelta.isUndefined())
        details.add("Artifact-score delta: " + artifactDelta.toString());
    return (static_cast<bool>(obj->getProperty(rg::ok)) ? "Safety checks passed" : "Safety guardrail blocked promotion") +
           (details.isEmpty() ? "." : ": " + details.joinIntoString("; "));
}

/// "Matched <gain> dB | TP a -> b dBTP | low mono ... | crest ... | clips ... | presence ...",
/// or empty when the before/after measurements are absent.
inline juce::String measurements(const juce::var& guardrail)
{
    namespace rg = api::fields::RenderGuardrail;
    namespace gm = api::fields::GuardrailMetrics;
    auto* obj = guardrail.getDynamicObject();
    if (obj == nullptr) return {};
    const auto before = obj->getProperty(rg::before).getDynamicObject();
    const auto after = obj->getProperty(rg::after).getDynamicObject();
    if (before == nullptr || after == nullptr) return {};
    const auto metric = [before, after](const char* key, int decimals) {
        return juce::String((double)before->getProperty(key), decimals) + " -> " +
               juce::String((double)after->getProperty(key), decimals);
    };
    return "Matched " + juce::String((double)obj->getProperty(rg::matched_gain_db), 2) + " dB | TP " +
           metric(gm::true_peak_dbtp, 2) + " dBTP | low mono " + metric(gm::mono_low_corr, 3) + " | crest " +
           metric(gm::crest_db, 2) + " dB | clips " + metric(gm::clip_samples, 0) + " | presence " +
           metric(gm::vocal_presence_proxy, 4);
}

} // namespace khensu::guardrail_text
