// desktop-app/Source/ui/ExportDelivery/EvidenceReportBody.cpp
#include "EvidenceReportBody.h"
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/recommendations/Recommendation.h>
#include <cmath>

namespace khensu {

namespace apif = api::fields;

namespace {
juce::String issueCodeFor(const Recommendation& rec, const std::string& fileId)
{
    const juce::String code(issueCodeOf(rec, fileId));
    if (code != juce::String(rec.id)) return code;
    const int sep = code.lastIndexOfChar('_');
    return sep > 0 ? code.substring(0, sep) : code;
}
} // namespace

juce::var buildEvidenceReportBody(const AnalysisResult& result, const EvidenceReportMeta& meta)
{
    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty(apif::EvidenceReportRequest::session_name, meta.sessionName);
    body->setProperty(apif::EvidenceReportRequest::file_name, meta.fileName);
    body->setProperty(apif::EvidenceReportRequest::computed_at, juce::String(result.computed_at));
    body->setProperty(apif::EvidenceReportRequest::genre, meta.genre);
    body->setProperty(apif::EvidenceReportRequest::output_path, meta.outputPath);
    if (meta.jsonPath.isNotEmpty()) body->setProperty(apif::EvidenceReportRequest::json_path, meta.jsonPath);

    juce::Array<juce::var> recs;
    for (const auto& rec : result.recommendations) {
        juce::DynamicObject::Ptr r = new juce::DynamicObject();
        r->setProperty(apif::EvidenceRecommendationIn::id, juce::String(rec.id));
        r->setProperty(apif::EvidenceRecommendationIn::issue_code, issueCodeFor(rec, result.file_id));
        r->setProperty(apif::EvidenceRecommendationIn::what, juce::String(rec.what));
        r->setProperty(apif::EvidenceRecommendationIn::suggested_action, juce::String(rec.suggested_action));
        r->setProperty(apif::EvidenceRecommendationIn::severity, std::isfinite(rec.severity) ? rec.severity : 0.0f);
        r->setProperty(apif::EvidenceRecommendationIn::stem_or_bus, juce::String(rec.stem_or_bus));
        if (rec.freq_range.has_value() && rec.freq_range->high_hz > rec.freq_range->low_hz) {
            r->setProperty(apif::EvidenceRecommendationIn::freq_lo_hz, rec.freq_range->low_hz);
            r->setProperty(apif::EvidenceRecommendationIn::freq_hi_hz, rec.freq_range->high_hz);
        }
        juce::Array<juce::var> items;
        for (const auto& e : rec.evidence) {
            if (!std::isfinite(e.value)) continue;
            juce::DynamicObject::Ptr item = new juce::DynamicObject();
            item->setProperty(apif::EvidenceItemIn::measurement, juce::String(e.measurement));
            item->setProperty(apif::EvidenceItemIn::value, e.value);
            item->setProperty(apif::EvidenceItemIn::unit, juce::String(e.unit));
            if (e.threshold.has_value() && std::isfinite(*e.threshold))
                item->setProperty(apif::EvidenceItemIn::threshold, *e.threshold);
            if (e.threshold_low.has_value() && std::isfinite(*e.threshold_low))
                item->setProperty(apif::EvidenceItemIn::threshold_low, *e.threshold_low);
            item->setProperty(apif::EvidenceItemIn::comparison, juce::String(e.comparison));
            item->setProperty(apif::EvidenceItemIn::source, juce::String(e.source.empty() ? "absolute" : e.source));
            items.add(juce::var(item.get()));
        }
        r->setProperty(apif::EvidenceRecommendationIn::evidence, juce::var(items));
        recs.add(juce::var(r.get()));
    }
    body->setProperty(apif::EvidenceReportRequest::recommendations, juce::var(recs));
    return juce::var(body.get());
}

} // namespace khensu
