// tests/shared-core/test_batch_result_aggregator.cpp
// Audit CQ-07: multi-file batch aggregation was inline in AnalysisController,
// untestable without running the whole async analysis pipeline, and merged
// recommendations lost which file they came from. These test the extracted
// pure aggregator directly, with deliberately divergent per-file fixtures.
#include <khensu/analysis/AnalysisInputs.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/analysis/BatchResultAggregator.h>

using namespace khensu;

namespace {

AnalysisResult makeResult(const std::string& fileId, float lufs, float truePeak, float crestDb,
                          float peakToRmsDb, float bpm)
{
    AnalysisResult r;
    r.file_id = fileId;
    r.sample_rate = 44100.0;
    r.loudness.lufs_integrated = lufs;
    r.loudness.true_peak_dbtp = truePeak;
    r.dynamics.crest_factor_db = crestDb;
    r.dynamics.peak_to_rms_db = peakToRmsDb;
    r.python_data.bpm_detected = bpm;
    return r;
}

} // namespace

TEST_CASE("aggregateBatchResults returns the single result unchanged for a one-file batch", "[batch_aggregator]")
{
    auto r = makeResult("f1", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);
    r.file_id = "f1";
    auto agg = aggregateBatchResults({r}, "session-1");
    CHECK(agg.file_id == "f1"); // untouched — not relabeled "batch"
    CHECK_THAT(agg.loudness.lufs_integrated, Catch::Matchers::WithinAbs(-14.0f, 0.001f));
}

TEST_CASE("aggregateBatchResults averages loudness across divergent files", "[batch_aggregator]")
{
    auto a = makeResult("f1", -10.0f, -1.0f, 8.0f, 10.0f, 100.0f);
    auto b = makeResult("f2", -20.0f, -3.0f, 12.0f, 14.0f, 140.0f);

    auto agg = aggregateBatchResults({a, b}, "session-1");

    CHECK(agg.file_id == "batch");
    CHECK(agg.session_id == "session-1");
    // Loudness combines in the power domain: 10*log10((10^-1 + 10^-2) / 2)
    // = -12.60 LUFS, not the arithmetic mean of dB values (-15).
    CHECK_THAT(agg.loudness.lufs_integrated, Catch::Matchers::WithinAbs(-12.596f, 0.002f));
    // PLR is consistent with the reported true peak and loudness.
    CHECK_THAT(agg.loudness.plr,
               Catch::Matchers::WithinAbs(agg.loudness.true_peak_dbtp - agg.loudness.lufs_integrated, 1e-4f));
    CHECK_THAT(agg.dynamics.crest_factor_db, Catch::Matchers::WithinAbs(10.0f, 0.001f));
    // Doesn't average meaningfully - representative (first file), same policy as section_analysis etc.
    CHECK(agg.sample_rate == 44100.0);
}

TEST_CASE("aggregateBatchResults carries representative per-band/per-bin spectral arrays into a batch result",
         "[batch_aggregator]")
{
    // Regression pin: these arrays were previously dropped entirely for any
    // multi-file (batch) session, leaving SpectralPanel/TonalBalancePanel
    // with empty data even though every per-file result had it.
    auto a = makeResult("f1", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);
    a.spectral.rms_per_octave_db = {-30.0f, -20.0f, -10.0f};
    a.spectral.rms_mid_per_octave_db = {-31.0f, -21.0f, -11.0f};
    a.spectral.rms_side_per_octave_db = {-32.0f, -22.0f, -12.0f};
    a.spectral.fft_mid_magnitudes = {-40.0f, -35.0f};
    a.spectral.fft_side_magnitudes = {-41.0f, -36.0f};
    a.spectral.tilt_db_per_octave = -3.5f;

    auto b = makeResult("f2", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);
    b.spectral.rms_per_octave_db = {-99.0f, -99.0f, -99.0f};
    b.spectral.fft_mid_magnitudes = {-99.0f, -99.0f};

    auto agg = aggregateBatchResults({a, b}, "session-1");

    CHECK(agg.spectral.rms_per_octave_db == a.spectral.rms_per_octave_db);
    CHECK(agg.spectral.rms_mid_per_octave_db == a.spectral.rms_mid_per_octave_db);
    CHECK(agg.spectral.rms_side_per_octave_db == a.spectral.rms_side_per_octave_db);
    CHECK(agg.spectral.fft_mid_magnitudes == a.spectral.fft_mid_magnitudes);
    CHECK(agg.spectral.fft_side_magnitudes == a.spectral.fft_side_magnitudes);
    CHECK_THAT(agg.spectral.tilt_db_per_octave, Catch::Matchers::WithinAbs(-3.5f, 0.001f));
}

TEST_CASE("aggregateBatchResults takes the max true peak, not the average", "[batch_aggregator]")
{
    // The loudest single peak across the batch is what matters for clipping
    // risk — averaging it would hide the actual worst-case peak.
    auto a = makeResult("f1", -14.0f, -6.0f, 10.0f, 12.0f, 120.0f);
    auto b = makeResult("f2", -14.0f, -0.5f, 10.0f, 12.0f, 120.0f);

    auto agg = aggregateBatchResults({a, b}, "session-1");
    CHECK_THAT(agg.loudness.true_peak_dbtp, Catch::Matchers::WithinAbs(-0.5f, 0.001f));
}

TEST_CASE("aggregateBatchResults keeps the largest DC offset, with its sign (MEAS-4)", "[batch_aggregator]")
{
    // Worst case, like clipping and the noise floor (CORE-01): one stem
    // centred off zero is a fault in the session, whatever the others read.
    auto a = makeResult("f1", -14.0f, -6.0f, 10.0f, 12.0f, 120.0f);
    auto b = makeResult("f2", -14.0f, -6.0f, 10.0f, 12.0f, 120.0f);
    auto c = makeResult("f3", -14.0f, -6.0f, 10.0f, 12.0f, 120.0f);
    a.artifacts.dc_offset = 0.0005f;
    b.artifacts.dc_offset = -0.02f;
    b.artifacts.dc_offset_steady = false;
    c.artifacts.dc_offset = 0.01f;

    auto agg = aggregateBatchResults({a, b, c}, "session-1");
    CHECK_THAT(agg.artifacts.dc_offset, Catch::Matchers::WithinAbs(-0.02f, 1e-6f));
    CHECK_FALSE(agg.artifacts.dc_offset_steady); // the worst stem's steadiness
}

TEST_CASE("aggregateBatchResults aggregates its own peak_to_rms_db field, not crest factor", "[batch_aggregator]")
{
    // Regression pin for the CQ-07 bug: peak_to_rms_db previously copied
    // crest factor's sum instead of its own field's values.
    auto a = makeResult("f1", -14.0f, -1.0f, 8.0f, 20.0f, 120.0f);
    auto b = makeResult("f2", -14.0f, -1.0f, 12.0f, 24.0f, 120.0f);

    auto agg = aggregateBatchResults({a, b}, "session-1");
    CHECK_THAT(agg.dynamics.crest_factor_db, Catch::Matchers::WithinAbs(10.0f, 0.001f));
    CHECK_THAT(agg.dynamics.peak_to_rms_db, Catch::Matchers::WithinAbs(22.0f, 0.001f));
}

TEST_CASE("aggregateBatchResults takes BPM only from files where it was actually detected", "[batch_aggregator]")
{
    auto a = makeResult("f1", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);
    auto b = makeResult("f2", -14.0f, -1.0f, 10.0f, 12.0f, 0.0f); // undetected
    auto c = makeResult("f3", -14.0f, -1.0f, 10.0f, 12.0f, 140.0f);

    auto agg = aggregateBatchResults({a, b, c}, "session-1");
    // Median of 120 and 140 (the undetected file must not drag it down).
    CHECK_THAT(agg.python_data.bpm_detected, Catch::Matchers::WithinAbs(130.0f, 0.001f));
}

TEST_CASE("aggregateBatchResults tags merged recommendations with their source file", "[batch_aggregator]")
{
    auto a = makeResult("vocals.wav", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);
    Recommendation recA;
    recA.what = "Vocal sibilance is harsh";
    a.recommendations.push_back(recA);

    auto b = makeResult("drums.wav", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);
    Recommendation recB;
    recB.what = "Kick lacks low-end weight";
    b.recommendations.push_back(recB);

    auto agg = aggregateBatchResults({a, b}, "session-1");

    REQUIRE(agg.recommendations.size() == 2);
    CHECK(agg.recommendations[0].source_file_id == "vocals.wav");
    CHECK(agg.recommendations[1].source_file_id == "drums.wav");
}

TEST_CASE("aggregateBatchResults retains full per-file results in batch_results", "[batch_aggregator]")
{
    auto a = makeResult("f1", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);
    auto b = makeResult("f2", -18.0f, -2.0f, 11.0f, 13.0f, 130.0f);

    auto agg = aggregateBatchResults({a, b}, "session-1");

    REQUIRE(agg.batch_results.size() == 2);
    CHECK(agg.batch_results[0].file_id == "f1");
    CHECK(agg.batch_results[1].file_id == "f2");
}

TEST_CASE("aggregateBatchResults on an empty batch returns a default result without crashing",
         "[batch_aggregator]")
{
    auto agg = aggregateBatchResults({}, "session-1");
    CHECK(agg.file_id.empty());
}

TEST_CASE("aggregateBatchResults folds octave-mismatched BPMs instead of averaging them", "[batch_aggregator]")
{
    // AUDIT-050: 70 + 140 averaged to 105 - a tempo neither file has.
    auto a = makeResult("f1", -14.0f, -1.0f, 10.0f, 12.0f, 70.0f);
    auto b = makeResult("f2", -14.0f, -1.0f, 10.0f, 12.0f, 140.0f);
    a.python_data.bpm_confidence = 0.9f;
    b.python_data.bpm_confidence = 0.7f;

    auto agg = aggregateBatchResults({a, b}, "session-1");
    CHECK_THAT(agg.python_data.bpm_detected, Catch::Matchers::WithinAbs(70.0f, 0.01f));
    // Real confidences, both files agree: their mean - not an invented 0.8.
    CHECK_THAT(agg.python_data.bpm_confidence, Catch::Matchers::WithinAbs(0.8f, 0.001f));
}

TEST_CASE("aggregateBatchResults confidence drops when files disagree on tempo", "[batch_aggregator]")
{
    auto a = makeResult("f1", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);
    auto b = makeResult("f2", -14.0f, -1.0f, 10.0f, 12.0f, 121.0f);
    auto c = makeResult("f3", -14.0f, -1.0f, 10.0f, 12.0f, 90.0f);
    for (auto* r : {&a, &b, &c})
        r->python_data.bpm_confidence = 0.9f;

    auto agg = aggregateBatchResults({a, b, c}, "session-1");
    CHECK_THAT(agg.python_data.bpm_detected, Catch::Matchers::WithinAbs(120.0f, 0.01f));
    CHECK_THAT(agg.python_data.bpm_confidence, Catch::Matchers::WithinAbs(0.6f, 0.001f)); // 0.9 * 2/3
}

TEST_CASE("aggregateBatchResults keeps key confidence, meter and beat grids from the representative file",
          "[batch_aggregator]")
{
    auto a = makeResult("f1", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);
    a.python_data.pitch_key = "A minor";
    a.python_data.pitch_confidence = 0.78f;
    a.python_data.camelot = "8A";
    a.python_data.time_signature = "3/4";
    a.python_data.meter_confidence = 0.6f;
    a.python_data.beat_times_json = "[0.5,1.0]";
    a.python_data.downbeat_times_json = "[0.5]";
    auto b = makeResult("f2", -14.0f, -1.0f, 10.0f, 12.0f, 120.0f);

    auto agg = aggregateBatchResults({a, b}, "session-1");
    CHECK(agg.python_data.pitch_key == "A minor");
    CHECK_THAT(agg.python_data.pitch_confidence, Catch::Matchers::WithinAbs(0.78f, 1e-6f));
    CHECK(agg.python_data.camelot == "8A");
    CHECK(agg.python_data.time_signature == "3/4");
    CHECK(agg.python_data.beat_times_json == "[0.5,1.0]");
    CHECK(agg.python_data.downbeat_times_json == "[0.5]");
}

TEST_CASE("aggregateBatchResults keeps every input a file asked for and did not get (FIX-2)",
          "[batch_aggregator][fix2]")
{
    namespace in = khensu::analysis_inputs;
    auto a = makeResult("f1", -10.0f, -1.0f, 8.0f, 10.0f, 100.0f);
    auto b = makeResult("f2", -20.0f, -3.0f, 12.0f, 14.0f, 140.0f);
    a.inputs_failed = in::kDrums;
    b.inputs_failed = in::kSections;
    CHECK(aggregateBatchResults({a, b}, "session-1").inputs_failed == (in::kDrums | in::kSections));
    b.inputs_failed = in::kNone;
    CHECK(aggregateBatchResults({a, b}, "session-1").inputs_failed == in::kDrums);

    // What one run asks the engine for: the deep response and the four
    // follow-up requests, plus the reference when there is one. Stem masking
    // is on demand, so its absence is never a failed input.
    CHECK(in::serviceInputs(false) == (in::kDeepResponse | in::kSections | in::kDrums | in::kChords | in::kTrust));
    CHECK(in::serviceInputs(true) == (in::serviceInputs(false) | in::kReference));
    CHECK((in::serviceInputs(true) & in::kMasking) == 0u);
}
