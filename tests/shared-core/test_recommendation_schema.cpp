#include <catch2/catch_test_macros.hpp>
#include <khensu/recommendations/Recommendation.h>
#include <khensu/recommendations/RecommendationType.h>
#include <khensu/recommendations/ProcessingStage.h>
#include <khensu/common/TimeRange.h>
#include <khensu/common/FrequencyRange.h>

using namespace khensu;

TEST_CASE("Recommendation::is_complete returns false for empty recommendation", "[recommendation]")
{
    Recommendation r;
    CHECK_FALSE(r.is_complete());
}

TEST_CASE("Recommendation::is_complete returns true when all required fields are populated", "[recommendation]")
{
    Recommendation r;
    r.id = "some-uuid";
    r.what = "True peak exceeds -1 dBTP at 2:14.";
    r.why_helps = "Prevents clipping on streaming platforms that apply additional limiting.";
    r.suggested_action = "Reduce master bus output by 0.5 dB.";
    r.engineer_note = "This is right at the edge. A small gain reduction is all that's needed.";
    r.severity = 70.0f;
    r.confidence = 0.95f;
    CHECK(r.is_complete());
}

TEST_CASE("Recommendation optional time_range is null by default", "[recommendation]")
{
    Recommendation r;
    CHECK_FALSE(r.time_range.has_value());
}

TEST_CASE("Recommendation time_range can be set and queried", "[recommendation]")
{
    Recommendation r;
    r.time_range = TimeRange(32.0f, 64.0f);
    REQUIRE(r.time_range.has_value());
    CHECK(r.time_range->start_sec == 32.0f);
    CHECK(r.time_range->end_sec == 64.0f);
    CHECK(r.time_range->duration() == 32.0f);
    CHECK(r.time_range->contains(40.0f));
    CHECK_FALSE(r.time_range->contains(64.0f)); // half-open
}

TEST_CASE("Recommendation optional freq_range is null by default", "[recommendation]")
{
    Recommendation r;
    CHECK_FALSE(r.freq_range.has_value());
}

TEST_CASE("Recommendation default type is Technical", "[recommendation]")
{
    Recommendation r;
    CHECK(r.type == RecommendationType::Technical);
}

TEST_CASE("RecommendationType round-trips through string conversion", "[recommendation]")
{
    CHECK(to_string(RecommendationType::Technical) == "Technical");
    CHECK(to_string(RecommendationType::Perceptual) == "Perceptual");
    CHECK(to_string(RecommendationType::Musical) == "Musical");
    CHECK(to_string(RecommendationType::Structural) == "Structural");

    CHECK(recommendation_type_from_string("Technical") == RecommendationType::Technical);
    CHECK(recommendation_type_from_string("Perceptual") == RecommendationType::Perceptual);
    CHECK(recommendation_type_from_string("Musical") == RecommendationType::Musical);
    CHECK(recommendation_type_from_string("Structural") == RecommendationType::Structural);
    CHECK(recommendation_type_from_string("garbage") == RecommendationType::Technical);
}

TEST_CASE("Recommendation safe_to_auto_apply defaults to false", "[recommendation]")
{
    Recommendation r;
    CHECK_FALSE(r.safe_to_auto_apply);
}

TEST_CASE("Recommendation is_myth_adjacent defaults to false", "[recommendation]")
{
    Recommendation r;
    CHECK_FALSE(r.is_myth_adjacent);
}

TEST_CASE("Recommendation suggested_parameters can hold float key-value pairs", "[recommendation]")
{
    Recommendation r;
    r.suggested_parameters["eq_gain_db"] = -2.5f;
    r.suggested_parameters["eq_freq_hz"] = 3200.0f;
    CHECK(r.suggested_parameters.count("eq_gain_db") == 1);
    CHECK(r.suggested_parameters.at("eq_freq_hz") == 3200.0f);
}

TEST_CASE("Recommendation alternative_strategies can hold multiple entries", "[recommendation]")
{
    Recommendation r;
    r.alternative_strategies.push_back("Use a brickwall limiter at -0.3 dBTP instead.");
    r.alternative_strategies.push_back("Automate the fader down 1 dB on the loudest hit.");
    CHECK(r.alternative_strategies.size() == 2);
}

// --- TimeRange unit tests ---
TEST_CASE("TimeRange defaults to zero range", "[range]")
{
    TimeRange tr;
    CHECK(tr.start_sec == 0.0f);
    CHECK(tr.end_sec == 0.0f);
    CHECK(tr.duration() == 0.0f);
    CHECK(tr.is_valid());
}

TEST_CASE("TimeRange::to_label formats correctly", "[range]")
{
    TimeRange tr(32.0f, 94.0f);
    CHECK(tr.to_label() == "0:32-1:34");
}

TEST_CASE("TimeRange invalid when end < start", "[range]")
{
    TimeRange tr(10.0f, 5.0f);
    CHECK_FALSE(tr.is_valid());
}

// --- FrequencyRange unit tests ---
TEST_CASE("FrequencyRange named bands are valid and non-overlapping at key boundaries", "[range]")
{
    CHECK(FrequencyRange::sub_bass().is_valid());
    CHECK(FrequencyRange::bass().is_valid());
    CHECK(FrequencyRange::full_range().is_valid());
}

TEST_CASE("FrequencyRange::contains checks inclusive bounds", "[range]")
{
    FrequencyRange fr(1000.0f, 5000.0f);
    CHECK(fr.contains(1000.0f));
    CHECK(fr.contains(3000.0f));
    CHECK(fr.contains(5000.0f));
    CHECK_FALSE(fr.contains(999.9f));
    CHECK_FALSE(fr.contains(5001.0f));
}

TEST_CASE("FrequencyRange::centre returns midpoint", "[range]")
{
    FrequencyRange fr(1000.0f, 3000.0f);
    CHECK(fr.centre() == 2000.0f);
}

TEST_CASE("ProcessingStage to_string covers all values", "[schema][stage]")
{
    CHECK(std::string(to_string(ProcessingStage::Arrangement)) == "Arrangement");
    CHECK(std::string(to_string(ProcessingStage::SoundDesign)) == "SoundDesign");
    CHECK(std::string(to_string(ProcessingStage::Edit)) == "Edit");
    CHECK(std::string(to_string(ProcessingStage::MixChannel)) == "MixChannel");
    CHECK(std::string(to_string(ProcessingStage::MixBus)) == "MixBus");
    CHECK(std::string(to_string(ProcessingStage::Master)) == "Master");
    CHECK(std::string(to_string(ProcessingStage::Monitoring)) == "Monitoring");
    CHECK(std::string(to_string(ProcessingStage::Unknown)) == "Unknown");
}

TEST_CASE("Recommendation new fields have safe defaults", "[schema][fields]")
{
    Recommendation r;
    CHECK(r.stage == ProcessingStage::Unknown);
    CHECK(r.chain_position == 99);
    CHECK(r.is_taste_decision == false);
    CHECK(r.how_to_verify.empty());
    CHECK(r.conflicts_with.empty());
    CHECK(r.reference_delta == 0.0f);
}

TEST_CASE("Recommendation is_complete() still works with new fields unpopulated", "[schema][complete]")
{
    Recommendation r;
    r.id = "test_id";
    r.what = "Issue";
    r.why_helps = "Helps";
    r.suggested_action = "Do this";
    r.engineer_note = "Note";
    r.severity = 50.0f;
    r.confidence = 0.7f;
    CHECK(r.is_complete()); // new fields have defaults; completeness unaffected
}

TEST_CASE("issueCodeOf strips the file-id suffix the recommenders append", "[recommendation]")
{
    khensu::Recommendation r;
    r.id = "lufs_too_loud_file-42";
    CHECK(khensu::issueCodeOf(r, "file-42") == "lufs_too_loud");
    r.source_file_id = "stem-7";
    r.id = "stereo_too_wide_stem-7";
    CHECK(khensu::issueCodeOf(r, "file-42") == "stereo_too_wide");
    r.id = "custom";
    CHECK(khensu::issueCodeOf(r) == "custom");
}

#include <khensu/recommendations/ParameterLabels.h>

TEST_CASE("Parameter labels: known keys read as words with units (UX-6)", "[recommendation][ux-6]")
{
    namespace pl = khensu::parameter_labels;
    CHECK(pl::describe("comp_ratio", 2.5f) == "Compressor ratio: 2.50:1");
    CHECK(pl::describe("eq_freq_hz", 3000.0f) == "EQ frequency: 3000 Hz");
    CHECK(pl::describe("eq_q", 0.707f) == "EQ Q: 0.71");
    CHECK(pl::describe("true_peak_ceiling_dbtp", -1.0f) == "True-peak ceiling: -1 dBTP");
    // An unknown key still reads as words, its suffix as a unit.
    CHECK(pl::describe("tape_hiss_db", -3.25f) == "Tape hiss: -3.25 dB");
    CHECK(pl::describe("mystery", 1.0f) == "Mystery: 1");
}
