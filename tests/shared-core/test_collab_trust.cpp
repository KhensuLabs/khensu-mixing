// CE-TRU-3 evidence trail + CE-COL-1/2 session cleanup of the 013/014 tables.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/recommendations/RecommendationEvidence.h>
#include <khensu/session/SessionManager.h>
#include <SQLiteCpp/SQLiteCpp.h>
#include <juce_core/juce_core.h>
#include <algorithm>

using namespace khensu;

namespace {

GenreEntry testGenre()
{
    GenreEntry g;
    g.id = "pop";
    g.name = "Pop";
    g.lufs_range_min = -12.0f;
    g.lufs_range_max = -8.0f;
    g.lra_range_min = 4.0f;
    g.lra_range_max = 9.0f;
    g.true_peak_max = -1.0f;
    g.stereo_width_min = 0.3f;
    g.stereo_width_max = 0.8f;
    return g;
}

const Recommendation* find(const std::vector<Recommendation>& recs, const std::string& code, const std::string& fileId)
{
    for (const auto& r : recs)
        if (issueCodeOf(r, fileId) == code) return &r;
    return nullptr;
}

} // namespace

TEST_CASE("Evidence: loudness and true-peak cards carry measurement, threshold and source", "[evidence]")
{
    AnalysisResult r;
    r.file_id = "f1";
    r.loudness.lufs_integrated = -5.5f;
    r.loudness.lra = 6.0f;
    r.loudness.true_peak_dbtp = 0.3f;
    r.stereo.width = 0.5f;
    r.stereo.correlation = 0.6f;
    r.dynamics.crest_factor_db = 10.0f;

    const auto recs = RecommendationEngine{}.generate(r, testGenre(), CreativeTarget{});
    const auto* loud = find(recs, "lufs_too_loud", "f1");
    REQUIRE(loud != nullptr);
    REQUIRE(loud->evidence.size() == 1);
    const auto& e = loud->evidence.front();
    CHECK(e.measurement == "Integrated loudness");
    CHECK(e.value == Catch::Approx(-5.5f));
    CHECK(e.unit == "LUFS");
    REQUIRE(e.threshold.has_value());
    CHECK(*e.threshold == Catch::Approx(-8.0f));
    CHECK(e.comparison == "above");
    CHECK(e.source == "genre_norm");

    const auto* tp = find(recs, "true_peak_over", "f1");
    REQUIRE(tp != nullptr);
    REQUIRE_FALSE(tp->evidence.empty());
    CHECK(tp->evidence.front().unit == "dBTP");
    CHECK(*tp->evidence.front().threshold == Catch::Approx(-1.0f));
}

TEST_CASE("Evidence: absolute-standard and window comparisons", "[evidence]")
{
    AnalysisResult r;
    r.file_id = "f2";
    r.stereo.correlation = -0.4f;
    r.artifacts.noise_floor_db = -40.0f;
    r.loudness.lufs_integrated = -10.0f;
    const auto genre = testGenre();

    const auto phase = evidenceForCode("phase_issue", r, genre);
    REQUIRE(phase.size() == 1);
    CHECK(phase[0].source == "absolute");
    CHECK(phase[0].comparison == "below");

    const auto noise = evidenceForCode("noise_floor_high", r, genre);
    REQUIRE(noise.size() == 1);
    CHECK(*noise[0].threshold == Catch::Approx(-50.0f));

    const auto onTarget = evidenceForCode("strength_lufs_on_target", r, genre);
    REQUIRE(onTarget.size() == 1);
    CHECK(onTarget[0].comparison == "within");
    CHECK(*onTarget[0].threshold_low == Catch::Approx(-12.0f));
    CHECK(*onTarget[0].threshold == Catch::Approx(-8.0f));

    CHECK(evidenceForCode("hook_strength_low", r, genre).empty()); // judgement card: no single measurement
}

TEST_CASE("Evidence: a reference delta adds a reference item", "[evidence]")
{
    AnalysisResult r;
    r.file_id = "f3";
    r.loudness.lufs_integrated = -5.0f;
    Recommendation rec;
    rec.id = "lufs_too_loud_f3";
    rec.reference_delta = 2.5f;
    std::vector<Recommendation> recs{rec};
    attachEvidence(recs, r, testGenre());
    REQUIRE(recs[0].evidence.size() == 2);
    CHECK(recs[0].evidence.back().source == "reference");
    CHECK(recs[0].evidence.back().value == Catch::Approx(2.5f));
}

TEST_CASE("deleteSession removes the session's comments and render versions", "[session][collab]")
{
    const auto file = juce::File::createTempFile(".db");
    {
        SessionManager mgr(file.getFullPathName().toStdString());
        REQUIRE(mgr.isOpen());
        auto created = mgr.createSession("Collab");
        REQUIRE(created.is_ok());
        const auto id = created.value().id;
        {
            SQLite::Database db(file.getFullPathName().toStdString(), SQLite::OPEN_READWRITE);
            db.exec("INSERT INTO session_comments (id, session_id, start_s, text, created_at, updated_at) "
                    "VALUES ('c1', '" + id + "', 1.0, 'hi', 'x', 'x'), ('c2', 'other', 1.0, 'keep', 'x', 'x')");
            db.exec("INSERT INTO render_versions (id, session_id, path, created_at, updated_at) "
                    "VALUES ('v1', '" + id + "', 'a.wav', 'x', 'x')");
            db.exec("INSERT INTO render_branches (session_id, name, created_at) VALUES ('" + id + "', 'alt', 'x')");
        }
        REQUIRE(mgr.deleteSession(id).is_ok());
        SQLite::Database check(file.getFullPathName().toStdString(), SQLite::OPEN_READONLY);
        CHECK(check.execAndGet("SELECT COUNT(*) FROM session_comments").getInt() == 1);
        CHECK(check.execAndGet("SELECT COUNT(*) FROM render_versions").getInt() == 0);
        CHECK(check.execAndGet("SELECT COUNT(*) FROM render_branches").getInt() == 0);
    }
    file.deleteFile();
    juce::File(file.getFullPathName() + "-wal").deleteFile();
    juce::File(file.getFullPathName() + "-shm").deleteFile();
}
