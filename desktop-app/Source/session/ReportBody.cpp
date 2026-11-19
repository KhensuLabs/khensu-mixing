// desktop-app/Source/session/ReportBody.cpp
#include "ReportBody.h"
#include "../ui/shared/KhensuTheme.h" // the report's palette snapshot
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/session/Session.h>

namespace apif = khensu::api::fields;

namespace khensu::report {

juce::String buildRequestBody(const AnalysisResult& analysis, const Session* session, const juce::String& outputPath,
                              const juce::String& reportMode, const juce::var& followUpScans)
{
    if (session == nullptr) return juce::String{}; // guard: no active session
    const auto& r = analysis;
    const auto* s = session;

    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    const juce::String fileName = s->files.empty()
                                      ? juce::String{}
                                      : juce::String(s->files[0].title.empty() ? s->files[0].path : s->files[0].title);

    obj->setProperty(apif::ReportRequest::session_name, juce::var(juce::String(s->name)));
    obj->setProperty(apif::ReportRequest::file_name, juce::var(fileName));
    obj->setProperty(apif::ReportRequest::session_id, juce::var(juce::String(s->id)));
    obj->setProperty(apif::ReportRequest::report_mode, juce::var(reportMode));
    obj->setProperty(apif::ReportRequest::computed_at, juce::var(juce::String(r.computed_at)));
    obj->setProperty(apif::ReportRequest::lufs_integrated, juce::var(r.loudness.lufs_integrated));
    obj->setProperty(apif::ReportRequest::lra, juce::var(r.loudness.lra));
    obj->setProperty(apif::ReportRequest::true_peak_dbtp, juce::var(r.loudness.true_peak_dbtp));
    obj->setProperty(apif::ReportRequest::crest_factor_db, juce::var(r.dynamics.crest_factor_db));
    obj->setProperty(apif::ReportRequest::dynamic_range_db, juce::var(r.dynamics.dynamic_range_db));
    obj->setProperty(apif::ReportRequest::stereo_width, juce::var(r.stereo.width));
    obj->setProperty(apif::ReportRequest::stereo_correlation, juce::var(r.stereo.correlation));
    obj->setProperty(apif::ReportRequest::bpm, juce::var(r.python_data.bpm_detected));
    obj->setProperty(apif::ReportRequest::pitch_key, juce::var(juce::String(r.python_data.pitch_key)));
    // AUDIT-052: the reports print estimate confidences so a BPM/key guess
    // isn't read as a measurement. Only sent when the deep pass produced them
    // (absent = unknown). low_quality_extraction is not sent: the analysis
    // response carries no extraction-backend flag, so it is never known here.
    if (r.has_python_analysis()) {
        if (r.python_data.bpm_detected > 0.0f && r.python_data.bpm_confidence > 0.0f)
            obj->setProperty(apif::ReportRequest::bpm_confidence,
                             juce::var(juce::jlimit(0.0f, 1.0f, r.python_data.bpm_confidence)));
        if (!r.python_data.pitch_key.empty() && r.python_data.pitch_confidence > 0.0f)
            obj->setProperty(apif::ReportRequest::key_confidence,
                             juce::var(juce::jlimit(0.0f, 1.0f, r.python_data.pitch_confidence)));
    }
    obj->setProperty(apif::ReportRequest::genre_detected, juce::var(juce::String(r.python_data.genre_detected)));
    obj->setProperty(apif::ReportRequest::hook_strength, juce::var(r.python_data.hooks.hook_strength));
    obj->setProperty(apif::ReportRequest::hook_count, juce::var(r.python_data.hooks.hook_count));
    obj->setProperty(apif::ReportRequest::memorability_score, juce::var(r.python_data.memorability.score));
    obj->setProperty(apif::ReportRequest::heaviness_distortion, juce::var(r.python_data.heaviness.distortion));
    obj->setProperty(apif::ReportRequest::spectral_balance_low, juce::var(r.spectral.balance_low));
    obj->setProperty(apif::ReportRequest::spectral_balance_mid, juce::var(r.spectral.balance_mid));
    obj->setProperty(apif::ReportRequest::spectral_balance_high, juce::var(r.spectral.balance_high));
    obj->setProperty(apif::ReportRequest::spectral_centroid_hz, juce::var(r.spectral.centroid_hz));

    if (r.translation_risk) {
        auto getEnv = [&](const char* key) -> float {
            auto it = r.translation_risk->per_environment.find(key);
            return it != r.translation_risk->per_environment.end() ? it->second : 0.0f;
        };
        obj->setProperty(apif::ReportRequest::translation_risk_studio, juce::var(getEnv("studio")));
        obj->setProperty(apif::ReportRequest::translation_risk_earbuds, juce::var(getEnv("earbuds")));
        obj->setProperty(apif::ReportRequest::translation_risk_phone, juce::var(getEnv("phone")));
        obj->setProperty(apif::ReportRequest::translation_risk_car, juce::var(getEnv("car")));
        obj->setProperty(apif::ReportRequest::translation_risk_club, juce::var(getEnv("club")));
    }
    if (r.harshness) {
        obj->setProperty(apif::ReportRequest::harshness_score, juce::var(r.harshness->score));
        if (r.harshness->peak_hz.has_value())
            obj->setProperty(apif::ReportRequest::harshness_peak_hz, juce::var(*r.harshness->peak_hz));
    }
    if (r.resonance) {
        juce::Array<juce::var> hotspots;
        for (const auto& h : r.resonance->hotspots) {
            juce::DynamicObject::Ptr hObj = new juce::DynamicObject();
            hObj->setProperty(apif::ResonanceHotspotSummary::hz, juce::var(h.hz));
            hObj->setProperty(apif::ResonanceHotspotSummary::db, juce::var(h.db));
            hObj->setProperty(apif::ResonanceHotspotSummary::prominence_db, juce::var(h.prominence));
            hotspots.add(juce::var(hObj.get()));
        }
        obj->setProperty(apif::ReportRequest::resonance_hotspots, juce::var(hotspots));
    }
    obj->setProperty(apif::ReportRequest::output_path, juce::var(outputPath));

    juce::Array<juce::var> recs;
    for (const auto& rec : r.recommendations) {
        juce::DynamicObject::Ptr recObj = new juce::DynamicObject();
        recObj->setProperty(apif::RecommendationSummary::type, juce::var(juce::String(to_string(rec.type))));
        recObj->setProperty(apif::RecommendationSummary::what, juce::var(juce::String(rec.what)));
        recObj->setProperty(apif::RecommendationSummary::severity, juce::var(rec.severity));
        recObj->setProperty(apif::RecommendationSummary::suggested_action,
                            juce::var(juce::String(rec.suggested_action)));
        // Lets the report add myth context (myth_guardrail) and fill any
        // cause / trade-off / verification the engine left empty (text_factory).
        recObj->setProperty(apif::RecommendationSummary::issue_code,
                            juce::var(juce::String(issueCodeOf(rec, r.file_id))));
        recObj->setProperty(apif::RecommendationSummary::most_likely_cause,
                            juce::var(juce::String(rec.most_likely_cause)));
        recObj->setProperty(apif::RecommendationSummary::trade_off, juce::var(juce::String(rec.trade_off)));
        recObj->setProperty(apif::RecommendationSummary::how_to_verify, juce::var(juce::String(rec.how_to_verify)));
        recs.add(juce::var(recObj.get()));
    }
    obj->setProperty(apif::ReportRequest::recommendations, juce::var(recs));
    // WF-14: the session's follow-up scans (already compacted by ScanSummary).
    if (followUpScans.getDynamicObject() != nullptr)
        obj->setProperty(apif::ReportRequest::follow_up_scans, followUpScans);

    // Quick stats from the AnalysisResult (mirrors what /analyze/quick_stats returns)
    obj->setProperty(apif::ReportRequest::quick_stats_lufs_integrated, juce::var(r.loudness.lufs_integrated));
    obj->setProperty(apif::ReportRequest::quick_stats_true_peak, juce::var(r.loudness.true_peak_dbtp));
    obj->setProperty(apif::ReportRequest::quick_stats_lra, juce::var(r.loudness.lra));
    obj->setProperty(apif::ReportRequest::quick_stats_crest, juce::var(r.dynamics.crest_factor_db));
    obj->setProperty(apif::ReportRequest::quick_stats_clipping_frames, juce::var(r.artifacts.clipping_frame_count));
    // quick_stats_dc_offset is deliberately not sent: the C++ analysis has no
    // DC measurement, and a 0.0 would print as a measured "DC offset 0.0000"
    // (ReportRequest treats an absent value as unknown).

    // Reports render in the Python process, so pass a complete palette snapshot
    // rather than letting exported HTML/PDF drift from the selected app theme.
    const auto toCssColour = [](juce::Colour colour) {
        const auto channel = [](uint8_t value) { return juce::String::toHexString((int)value).paddedLeft('0', 2); };
        return "#" + channel(colour.getRed()) + channel(colour.getGreen()) + channel(colour.getBlue()) +
               channel(colour.getAlpha());
    };
    juce::DynamicObject::Ptr theme = new juce::DynamicObject();
    theme->setProperty(apif::ReportTheme::bg_void, toCssColour(KhensuTheme::bgVoid));
    theme->setProperty(apif::ReportTheme::bg_base, toCssColour(KhensuTheme::bgBase));
    theme->setProperty(apif::ReportTheme::bg_surface, toCssColour(KhensuTheme::bgSurface));
    theme->setProperty(apif::ReportTheme::bg_raised, toCssColour(KhensuTheme::bgRaised));
    theme->setProperty(apif::ReportTheme::bg_overlay, toCssColour(KhensuTheme::bgOverlay));
    theme->setProperty(apif::ReportTheme::border_subtle, toCssColour(KhensuTheme::borderSubtle));
    theme->setProperty(apif::ReportTheme::border_active, toCssColour(KhensuTheme::borderActive));
    theme->setProperty(apif::ReportTheme::accent_teal, toCssColour(KhensuTheme::accentTeal));
    theme->setProperty(apif::ReportTheme::accent_teal_dim, toCssColour(KhensuTheme::accentTealDim));
    theme->setProperty(apif::ReportTheme::accent_amber, toCssColour(KhensuTheme::accentAmber));
    theme->setProperty(apif::ReportTheme::accent_amber_dim, toCssColour(KhensuTheme::accentAmberDim));
    theme->setProperty(apif::ReportTheme::accent_red, toCssColour(KhensuTheme::accentRed));
    theme->setProperty(apif::ReportTheme::accent_red_dim, toCssColour(KhensuTheme::accentRedDim));
    theme->setProperty(apif::ReportTheme::accent_purple, toCssColour(KhensuTheme::accentPurple));
    theme->setProperty(apif::ReportTheme::accent_purple_dim, toCssColour(KhensuTheme::accentPurpleDim));
    theme->setProperty(apif::ReportTheme::accent_magenta, toCssColour(KhensuTheme::accentMagenta));
    theme->setProperty(apif::ReportTheme::accent_magenta_dim, toCssColour(KhensuTheme::accentMagentaDim));
    theme->setProperty(apif::ReportTheme::accent_blue, toCssColour(KhensuTheme::accentBlue));
    theme->setProperty(apif::ReportTheme::accent_blue_dim, toCssColour(KhensuTheme::accentBlueDim));
    theme->setProperty(apif::ReportTheme::accent_green, toCssColour(KhensuTheme::accentGreen));
    theme->setProperty(apif::ReportTheme::accent_green_dim, toCssColour(KhensuTheme::accentGreenDim));
    theme->setProperty(apif::ReportTheme::scrim, toCssColour(KhensuTheme::scrim));
    theme->setProperty(apif::ReportTheme::shadow_colour, toCssColour(KhensuTheme::shadowColour));
    theme->setProperty(apif::ReportTheme::highlight, toCssColour(KhensuTheme::highlight));
    theme->setProperty(apif::ReportTheme::focus_ring, toCssColour(KhensuTheme::focusRing));
    theme->setProperty(apif::ReportTheme::text_primary, toCssColour(KhensuTheme::textPrimary));
    theme->setProperty(apif::ReportTheme::text_secondary, toCssColour(KhensuTheme::textSecondary));
    theme->setProperty(apif::ReportTheme::text_tertiary, toCssColour(KhensuTheme::textTertiary));
    theme->setProperty(apif::ReportTheme::text_accent, toCssColour(KhensuTheme::textAccent));
    obj->setProperty(apif::ReportRequest::theme, juce::var(theme.get()));

    return juce::JSON::toString(juce::var(obj.get()), false);
}

} // namespace khensu::report
