// tests/ui/EvidenceReportBodyTest.cpp - CE-TRU-3 evidence-report request body
// and the CE-TRU-1 calibration basis tag.
#include <catch2/catch_test_macros.hpp>
#include "../../desktop-app/Source/ui/ExportDelivery/EvidenceReportBody.h"
#include "../../desktop-app/Source/ui/shared/FindingCalibrationCache.h"
#include <khensu/ipc/generated/ApiSchema.h>

using namespace khensu;
namespace apif = khensu::api::fields;

TEST_CASE("Evidence report body carries recommendations and their evidence", "[evidence-report]")
{
    AnalysisResult r;
    r.file_id = "f-1";
    r.computed_at = "2026-11-07T10:00:00Z";
    Recommendation rec;
    rec.id = "true_peak_over_f-1";
    rec.source_file_id = "f-1";
    rec.what = "True peak is 0.4 dBTP";
    rec.suggested_action = "Limit to -1 dBTP";
    rec.severity = 90.0f;
    rec.freq_range = FrequencyRange{2500.0f, 5000.0f};
    EvidenceItem e;
    e.measurement = "True peak";
    e.value = 0.4f;
    e.unit = "dBTP";
    e.threshold = -1.0f;
    e.comparison = "above";
    e.source = "genre_norm";
    rec.evidence.push_back(e);
    r.recommendations.push_back(rec);

    Recommendation bare;
    bare.id = "hook_strength_low_0b9c-uuid";
    bare.what = "Hooks are weak";
    r.recommendations.push_back(bare);

    EvidenceReportMeta meta;
    meta.sessionName = "Song";
    meta.outputPath = "C:/tmp/e.html";
    meta.jsonPath = "C:/tmp/e.json";
    const auto body = buildEvidenceReportBody(r, meta);

    REQUIRE(body[apif::EvidenceReportRequest::session_name].toString() == "Song");
    REQUIRE(body[apif::EvidenceReportRequest::output_path].toString() == "C:/tmp/e.html");
    REQUIRE(body[apif::EvidenceReportRequest::json_path].toString() == "C:/tmp/e.json");
    REQUIRE(body[apif::EvidenceReportRequest::computed_at].toString() == "2026-11-07T10:00:00Z");
    const auto* recs = body[apif::EvidenceReportRequest::recommendations].getArray();
    REQUIRE(recs != nullptr);
    REQUIRE(recs->size() == 2);

    const auto first = (*recs)[0];
    REQUIRE(first[apif::EvidenceRecommendationIn::issue_code].toString() == "true_peak_over");
    REQUIRE(static_cast<double>(first[apif::EvidenceRecommendationIn::freq_lo_hz]) == 2500.0);
    const auto* items = first[apif::EvidenceRecommendationIn::evidence].getArray();
    REQUIRE(items != nullptr);
    REQUIRE(items->size() == 1);
    const auto item = (*items)[0];
    REQUIRE(item[apif::EvidenceItemIn::measurement].toString() == "True peak");
    REQUIRE(static_cast<double>(item[apif::EvidenceItemIn::threshold]) == -1.0);
    REQUIRE(item[apif::EvidenceItemIn::source].toString() == "genre_norm");
    REQUIRE(item[apif::EvidenceItemIn::comparison].toString() == "above");
    REQUIRE_FALSE(item.hasProperty(apif::EvidenceItemIn::threshold_low));

    const auto second = (*recs)[1];
    REQUIRE(second[apif::EvidenceRecommendationIn::issue_code].toString() == "hook_strength_low");
    REQUIRE(second[apif::EvidenceRecommendationIn::evidence].getArray()->isEmpty());
    REQUIRE_FALSE(second.hasProperty(apif::EvidenceRecommendationIn::freq_lo_hz));
}

TEST_CASE("Finding calibration cache reads heuristic until a table covers the code", "[evidence-report]")
{
    FindingCalibrationCache::resetForTests();
    REQUIRE(FindingCalibrationCache::basisTag("clipping") == "heuristic");

    auto* codes = new juce::DynamicObject();
    auto* row = new juce::DynamicObject();
    row->setProperty(apif::CalibrationCodeSummary::n, 42);
    row->setProperty(apif::CalibrationCodeSummary::precision, 0.8);
    row->setProperty(apif::CalibrationCodeSummary::recall, 0.7);
    codes->setProperty("clipping", juce::var(row));
    auto* resp = new juce::DynamicObject();
    resp->setProperty(apif::CalibrationTableResponse::available, true);
    resp->setProperty(apif::CalibrationTableResponse::codes, juce::var(codes));
    FindingCalibrationCache::setFromVar(juce::var(resp));

    REQUIRE(FindingCalibrationCache::basisTag("clipping") == "calibrated (n=42)");
    REQUIRE(FindingCalibrationCache::basisTag("mains_hum") == "heuristic");
    REQUIRE(FindingCalibrationCache::basisTooltip("clipping").contains("80%"));
    FindingCalibrationCache::resetForTests();
}
