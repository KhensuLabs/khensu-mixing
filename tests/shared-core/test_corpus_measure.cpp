// tests/shared-core/test_corpus_measure.cpp
// MEAS-1: measures the native metrics the genre targets are graded on, for
// scripts/measure_genre_norms.py. Hidden ([.measure]): it runs only when asked
//
//   KHENSU_MEASURE_LIST=<file of WAV paths, one per line>
//   KHENSU_MEASURE_OUT=<JSONL output>
//   KhensuCoreTests.exe "[.measure]"
//
// The numbers come from AudioAnalyzer itself - the same code the recommenders
// grade - so a target measured here and a mix graded in the app use one
// definition of centroid, low-end share, tilt, crest, width and loudness.
//
// A line may be "<path>\t<genre id>": the track is then also graded by the
// RecommendationEngine against that genre's targets and its finding codes are
// written as "findings" - the false-alarm side of the evidence each finding
// carries (NEW-2 / MEAS-4: how often it fires on released tracks of its genre,
// and on the same tracks with an injected defect).
#include <catch2/catch_test_macros.hpp>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/targets/GenreDatabase.h>
#include <juce_core/juce_core.h>

#include <fstream>
#include <string>

using namespace khensu;

TEST_CASE("Corpus measurement for genre norms", "[.measure]")
{
    const auto listPath = juce::SystemStats::getEnvironmentVariable("KHENSU_MEASURE_LIST", {});
    const auto outPath = juce::SystemStats::getEnvironmentVariable("KHENSU_MEASURE_OUT", {});
    if (listPath.isEmpty() || outPath.isEmpty()) SKIP("set KHENSU_MEASURE_LIST and KHENSU_MEASURE_OUT");

    std::ifstream list(listPath.toWideCharPointer());
    REQUIRE(list.good());
    std::ofstream out(outPath.toWideCharPointer(), std::ios::app);
    REQUIRE(out.good());

    GenreDatabase genres;
    const bool haveGenres =
        genres.loadFromFile(std::string(KHENSU_REPO_ROOT_DIR) + "/resources/genre_norms/genre_targets.json").is_ok();

    std::string line;
    int measured = 0;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::string genreId;
        if (const auto tab = line.find('\t'); tab != std::string::npos) {
            genreId = line.substr(tab + 1);
            line = line.substr(0, tab);
        }
        AudioFileRef ref;
        ref.id = "measure";
        ref.path = line;
        ref.format = "wav";
        AudioAnalyzer analyzer;
        auto r = analyzer.analyze(ref);
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty("path", juce::String(juce::CharPointer_UTF8(line.c_str())));
        if (r.is_err()) {
            o->setProperty("error", true);
        } else {
            const auto& a = r.value();
            o->setProperty("lufs", a.loudness.lufs_integrated);
            o->setProperty("lra", a.loudness.lra);
            o->setProperty("true_peak_dbtp", a.loudness.true_peak_dbtp);
            o->setProperty("centroid_hz", a.spectral.centroid_hz);
            o->setProperty("balance_low", a.spectral.balance_low);
            o->setProperty("tilt_db_per_octave", a.spectral.tilt_db_per_octave);
            o->setProperty("crest_factor_db", a.dynamics.crest_factor_db);
            o->setProperty("width", a.stereo.width);
            o->setProperty("noise_floor_db", a.artifacts.noise_floor_db);
            o->setProperty("noise_quiet_seconds", a.artifacts.noise_quiet_seconds);
            o->setProperty("noise_floor_spread_db", a.artifacts.noise_floor_spread_db);
            o->setProperty("clipping_frame_count", a.artifacts.clipping_frame_count);
            o->setProperty("dc_offset", a.artifacts.dc_offset);
            o->setProperty("dc_offset_steady", a.artifacts.dc_offset_steady);
            juce::Array<juce::var> octaves;
            for (float db : a.spectral.rms_per_octave_db)
                octaves.add(db);
            o->setProperty("octave_db", octaves);
            if (haveGenres && !genreId.empty()) {
                if (const auto genre = genres.findById(genreId)) {
                    juce::Array<juce::var> codes;
                    for (const auto& rec : RecommendationEngine{}.generate(a, *genre, CreativeTarget{}))
                        if (rec.type != RecommendationType::Strength)
                            codes.addIfNotAlreadyThere(juce::String(issueCodeOf(rec, ref.id)));
                    o->setProperty("findings", codes);
                }
            }
            ++measured;
        }
        out << juce::JSON::toString(juce::var(o.get()), true).toStdString() << "\n";
    }
    INFO("measured " << measured << " files");
    SUCCEED();
}
