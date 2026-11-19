#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/AnalysisResult.h>

using namespace khensu;

TEST_CASE("AnalysisResult extended fields absent by default", "[analysis][extended]")
{
    AnalysisResult r;
    CHECK_FALSE(r.mono_compat.has_value());
    CHECK_FALSE(r.translation_risk.has_value());
    CHECK_FALSE(r.harshness.has_value());
    CHECK_FALSE(r.resonance.has_value());
}

TEST_CASE("MonoCompatData band names are correct constants", "[analysis][sp16c]")
{
    // Verify band name constants exist
    CHECK(std::string(AnalysisResult::MonoCompatData::kBandSub) == "sub");
    CHECK(std::string(AnalysisResult::MonoCompatData::kBandBass) == "bass");
    CHECK(std::string(AnalysisResult::MonoCompatData::kBandMidrange) == "midrange");
    CHECK(std::string(AnalysisResult::MonoCompatData::kBandUpperMid) == "upper_mid");
    CHECK(std::string(AnalysisResult::MonoCompatData::kBandAir) == "air");
}

TEST_CASE("TranslationRiskData environment names are correct constants", "[analysis][sp16c]")
{
    CHECK(std::string(AnalysisResult::TranslationRiskData::kEnvStudio) == "studio");
    CHECK(std::string(AnalysisResult::TranslationRiskData::kEnvEarbuds) == "earbuds");
    CHECK(std::string(AnalysisResult::TranslationRiskData::kEnvPhone) == "phone");
    CHECK(std::string(AnalysisResult::TranslationRiskData::kEnvCar) == "car");
    CHECK(std::string(AnalysisResult::TranslationRiskData::kEnvClub) == "club");
}

TEST_CASE("TranslationRiskData has score field defaulting to 0", "[analysis][sp16c]")
{
    AnalysisResult::TranslationRiskData d;
    CHECK(d.score == 0.0f);
    CHECK(d.per_environment.empty());
}

TEST_CASE("HarshnessData peak fields are optional and absent by default", "[analysis][sp16c]")
{
    AnalysisResult::HarshnessData h;
    CHECK(h.score == 0.0f);
    CHECK_FALSE(h.peak_hz.has_value());
    CHECK_FALSE(h.peak_db.has_value());
}

TEST_CASE("ResonancePeak has three separate fields", "[analysis][sp16c]")
{
    AnalysisResult::ResonancePeak p;
    CHECK(p.hz == 0.0f);
    CHECK(p.db == 0.0f);
    CHECK(p.prominence == 0.0f);
}

TEST_CASE("SectionData default construction is safe", "[analysis][sp16d]")
{
    SectionData s;
    CHECK(s.label.empty());
    CHECK(s.start_sec == 0.0f);
    CHECK(s.end_sec == 0.0f);
    CHECK(s.duration_sec == 0.0f);
    CHECK(s.lufs_integrated == 0.0f);
}

TEST_CASE("ReferenceDelta default construction is safe", "[analysis][sp16d]")
{
    ReferenceDelta d;
    CHECK(d.dimension.empty());
    CHECK(d.target == 0.0f);
    CHECK(d.reference == 0.0f);
    CHECK(d.delta == 0.0f);
}

TEST_CASE("SectionAnalysis with populated sections", "[analysis][sp16d]")
{
    SectionAnalysis sa;

    SectionData s1;
    s1.label = "section_1";
    s1.start_sec = 0.0f;
    s1.end_sec = 10.0f;
    s1.duration_sec = 10.0f;

    SectionData s2;
    s2.label = "section_2";
    s2.start_sec = 10.0f;
    s2.end_sec = 25.0f;
    s2.duration_sec = 15.0f;

    sa.sections.push_back(s1);
    sa.sections.push_back(s2);
    sa.section_count = 2;
    sa.total_duration_sec = 25.0f;

    CHECK(sa.sections.size() == 2);
    CHECK(sa.section_count == 2);
    CHECK(sa.total_duration_sec == 25.0f);
    CHECK(sa.sections[0].label == "section_1");
    CHECK(sa.sections[1].label == "section_2");
}

TEST_CASE("AnalysisResult section_analysis absent by default", "[analysis][sp16d]")
{
    AnalysisResult r;
    CHECK_FALSE(r.section_analysis.has_value());
}

TEST_CASE("AnalysisResult reference_deltas absent by default", "[analysis][sp16d]")
{
    AnalysisResult r;
    CHECK_FALSE(r.reference_deltas.has_value());
}

// --- AQ-01: the resolved channel layout travels on the result -------------

TEST_CASE("AnalysisResult: channel layout defaults are empty, not misleading", "[analysis_result]")
{
    khensu::AnalysisResult r;
    // A default-constructed result has not been measured, so it must not claim
    // a layout. An empty name is the honest "unknown" state.
    CHECK(r.channel_layout.layout_name.empty());
    CHECK(r.channel_layout.channel_count == 0);
    CHECK_FALSE(r.channel_layout.lfe_excluded);
}

TEST_CASE("AnalysisResult: channel layout is assignable", "[analysis_result]")
{
    khensu::AnalysisResult r;
    r.channel_layout.layout_name = "5.1";
    r.channel_layout.channel_count = 6;
    r.channel_layout.lfe_excluded = true;
    CHECK(r.channel_layout.layout_name == "5.1");
    CHECK(r.channel_layout.channel_count == 6);
    CHECK(r.channel_layout.lfe_excluded);
}
