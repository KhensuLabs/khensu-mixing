// tests/ui/MultitrackModelTest.cpp
// Multitrack (stem-folder) sessions: strict parsing of the service payloads,
// the session file list built from them, the masking matrix hit-testing, and
// the track name/role round trip through SessionManager.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "ui/SessionIntake/MaskingMatrixComponent.h"
#include "ui/SessionIntake/MultitrackModel.h"
#include <khensu/session/SessionManager.h>

using namespace khensu;
using Catch::Matchers::ContainsSubstring;

namespace {

const char* kResult = R"({
  "track_count": 2, "session_sample_rate": 44100, "consistent": false,
  "mismatches": [{"track": "Bass", "path": "C:/s/bass.wav", "kind": "length", "detail": "1.20 s shorter"}],
  "tracks": [
    {"name": "Kick", "path": "C:/s/kick.wav", "file_name": "kick.wav", "role": "kick", "role_label": "Kick",
     "role_source": "name", "role_confidence": 0.85, "sample_rate": 44100, "channels": 1, "duration_sec": 4.0,
     "integrated_lufs": -18.4, "true_peak_dbtp": -2.1, "sample_peak_dbfs": -2.2, "crest_db": 14.0,
     "stereo_width": 0.0, "correlation": null, "low_end_width": 0.0, "spectral_tilt_db_per_oct": -6.0,
     "band_profile": [0.9, 0.1, 0, 0, 0, 0, 0, 0, 0, 0], "silent": false,
     "advice": [{"issue_code": "kick_bass_collision", "severity": "severe", "what": "Kick and Bass compete",
                 "suggested_action": "Carve space", "why_helps": "Clarity", "related_track": "Bass"}]},
    {"name": "Bass", "path": "C:/s/bass.wav", "file_name": "bass.wav", "role": "bass", "role_label": "Bass",
     "role_source": "name", "role_confidence": 0.85, "sample_rate": 44100, "channels": 2, "duration_sec": 2.8,
     "integrated_lufs": -9.2, "true_peak_dbtp": 0.4, "sample_peak_dbfs": 0.0, "crest_db": 4.0,
     "stereo_width": 0.1, "correlation": 0.98, "low_end_width": 0.02, "spectral_tilt_db_per_oct": -8.0,
     "band_profile": [0.5, 0.5, 0, 0, 0, 0, 0, 0, 0, 0], "silent": false,
     "advice": [{"issue_code": "clipping_detected", "severity": "severe", "what": "Clipped",
                 "suggested_action": "Lower it", "why_helps": ""},
                {"issue_code": "kick_bass_collision", "severity": "moderate", "what": "x",
                 "suggested_action": "y", "why_helps": "z", "related_track": "Kick"}]}
  ],
  "pairs": [{"track_a": "Kick", "track_b": "Bass", "collision_score": 0.37, "band_index": 0,
             "band_overlap": 0.35, "level_diff_db": -4.6, "risk": 0.4, "band_lo_hz": 20, "band_hi_hz": 60,
             "band_label": "20-60 Hz", "risk_level": "high", "summary": "Kick vs Bass at 20-60 Hz"}],
  "matrix_tracks": ["Kick", "Bass"],
  "matrix": [[0, 0.4], [0.4, 0]],
  "bands_hz": [20, 60, 120, 250, 500, 1000, 2000, 4000, 8000, 12000],
  "mix": {"sample_rate": 44100, "integrated_lufs": -8.9, "true_peak_dbtp": 0.6, "headroom_ok": false,
          "stereo_width": 0.05, "correlation": 0.99, "duration_sec": 4.0,
          "mixdown_path": "C:/cfg/multitrack/abc/khensu_mixdown.wav"}
})";

juce::var parse(const char* json) { return juce::JSON::parse(juce::String(juce::CharPointer_UTF8(json))); }

} // namespace

TEST_CASE("Multitrack: a full result parses into typed tracks, pairs and matrix", "[multitrack]")
{
    const auto v = parse(kResult);
    auto parsed = multitrack::parseAnalysis(v);
    REQUIRE(parsed.is_ok());
    const auto& a = parsed.value();
    REQUIRE(a.tracks.size() == 2);
    CHECK(a.tracks[0].name == "Kick");
    CHECK(a.tracks[0].channels == 1);
    CHECK_FALSE(a.tracks[0].correlation.has_value()); // null = not measurable, not 0
    REQUIRE(a.tracks[1].integratedLufs.has_value());
    CHECK(*a.tracks[1].integratedLufs == -9.2);
    CHECK(a.tracks[1].adviceCount("severe") == 1);
    CHECK(a.tracks[1].adviceCount("moderate") == 1);
    CHECK(a.tracks[0].advice[0].relatedTrack == "Bass");
    REQUIRE(a.pairs.size() == 1);
    CHECK(a.pairs[0].bandLabel == "20-60 Hz");
    CHECK(a.riskBetween("Kick", "Bass") == 0.4);
    CHECK(a.riskBetween("Bass", "Kick") == 0.4);
    CHECK(a.riskBetween("Kick", "Nope") == 0.0);
    CHECK_FALSE(a.consistent);
    CHECK(a.mismatches.at(0).kind == "length");
    CHECK_FALSE(a.mix.headroomOk);
    CHECK(multitrack::severityForRiskLevel(a.pairs[0].riskLevel) == "severe");
}

TEST_CASE("Multitrack: a renamed required field is a parse error naming the key", "[multitrack]")
{
    auto text = juce::String(juce::CharPointer_UTF8(kResult)).replace("\"risk_level\"", "\"riskLevel\"");
    auto parsed = multitrack::parseAnalysis(juce::JSON::parse(text));
    REQUIRE(parsed.is_err());
    CHECK_THAT(parsed.error(), ContainsSubstring("result.pairs[0].risk_level"));

    auto noMix = juce::String(juce::CharPointer_UTF8(kResult)).replace("\"mix\":", "\"mixx\":");
    auto parsed2 = multitrack::parseAnalysis(juce::JSON::parse(noMix));
    REQUIRE(parsed2.is_err());
    CHECK_THAT(parsed2.error(), ContainsSubstring("result.mix"));
}

TEST_CASE("Multitrack: job status uses the shared job states", "[multitrack]")
{
    auto running = multitrack::parseJobStatus(
        parse(R"({"status": "running", "progress": {"total": 4, "done": 1, "current": "Kick"}, "result": null})"));
    REQUIRE(running.is_ok());
    CHECK_FALSE(running.value().isTerminal());
    CHECK(running.value().current == "Kick");
    CHECK_FALSE(running.value().result.has_value());

    auto done = multitrack::parseJobStatus(parse(
        (juce::String(R"({"status": "completed", "progress": {"total": 2, "done": 2}, "result": )") +
         juce::String(juce::CharPointer_UTF8(kResult)) + "}")
            .toRawUTF8()));
    REQUIRE(done.is_ok());
    CHECK(done.value().isCompleted());
    CHECK(done.value().result.has_value());

    // A legacy/unknown state is not terminal - the poller keeps going to its deadline.
    auto legacy = multitrack::parseJobStatus(parse(R"({"status": "done", "progress": {"total": 1, "done": 1}})"));
    REQUIRE(legacy.is_ok());
    CHECK_FALSE(legacy.value().isTerminal());
    CHECK(multitrack::parseJobStatus(parse(R"({"state": "running"})")).is_err());
}

TEST_CASE("Multitrack: scan response parses names, roles and mismatches", "[multitrack]")
{
    auto scan = multitrack::parseScan(parse(R"({"folder": "C:/s", "track_count": 1, "session_sample_rate": 48000,
        "consistent": true, "max_tracks": 64, "mismatches": [],
        "files": [{"path": "C:/s/01_Kick.wav", "file_name": "01_Kick.wav", "track_name": "Kick", "role": "kick",
                   "role_label": "Kick", "role_source": "name", "sample_rate": 48000, "channels": 2,
                   "duration_sec": 10.5}]})"));
    REQUIRE(scan.is_ok());
    CHECK(scan.value().files.at(0).trackName == "Kick");
    CHECK(scan.value().sessionSampleRate == 48000);
    CHECK(multitrack::parseScan(parse(R"({"folder": "C:/s"})")).is_err());
}

TEST_CASE("Multitrack: session files lead with the mixdown and carry track names and roles", "[multitrack]")
{
    auto a = multitrack::parseAnalysis(parse(kResult)).value();
    auto files = multitrack::buildSessionFiles(a);
    REQUIRE(files.size() == 3);
    CHECK(files[0].track_role == "mix");
    CHECK(files[0].path == "C:/cfg/multitrack/abc/khensu_mixdown.wav");
    CHECK(files[0].format == "wav");
    CHECK(files[1].track_name == "Kick");
    CHECK(files[1].track_role == "kick");
    CHECK(files[2].sample_rate == 44100);
    CHECK(files[1].id != files[2].id);

    a.mix.mixdownPath = {};
    auto noMix = multitrack::buildSessionFiles(a);
    REQUIRE(noMix.size() == 2);
    CHECK(noMix[0].track_name == "Kick");
}

TEST_CASE("Multitrack: track names and roles survive a session save/load", "[multitrack]")
{
    const auto dbFile = juce::File::createTempFile(".db");
    {
        SessionManager mgr(dbFile.getFullPathName().toStdString());
        auto created = mgr.createSession("Stems", {});
        REQUIRE(created.is_ok());
        auto s = created.value();
        s.files = multitrack::buildSessionFiles(multitrack::parseAnalysis(parse(kResult)).value());
        CHECK(s.is_multitrack());
        REQUIRE(mgr.saveSession(s).is_ok());
        auto loaded = mgr.loadSession(s.id);
        REQUIRE(loaded.is_ok());
        REQUIRE(loaded.value().has_value());
        const auto& back = *loaded.value();
        REQUIRE(back.files.size() == 3);
        CHECK(back.files[0].track_role == "mix");
        CHECK(back.files[2].track_name == "Bass");
        CHECK(back.is_multitrack());

        Session plain;
        plain.files.resize(1);
        CHECK_FALSE(plain.is_multitrack());
    }
    dbFile.deleteFile();
    juce::File(dbFile.getFullPathName() + "-wal").deleteFile();
    juce::File(dbFile.getFullPathName() + "-shm").deleteFile();
}

TEST_CASE("Multitrack: masking matrix maps points to cells and reports clicks", "[multitrack]")
{
    auto a = multitrack::parseAnalysis(parse(kResult)).value();
    MaskingMatrixComponent m;
    m.setBounds(0, 0, 220, 120);
    CHECK(m.cellAt({5, 5}).first == -1); // the name column is not a cell
    m.setAnalysis(&a);
    // Grid starts after the 120 px name column and the 18 px header.
    const auto cell = m.cellAt({125, 25});
    CHECK(cell.first == 0);
    CHECK(cell.second == 0);
    const auto other = m.cellAt({125 + 45, 25 + 45});
    CHECK(other.first == 1);
    CHECK(other.second == 1);
    int clicked = -1;
    m.onTrackClicked = [&](int row) { clicked = row; };
    const auto pos = juce::Point<float>(125.0f + 45.0f, 25.0f);
    juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(), 0.0f, 0.0f,
                       0.0f, 0.0f, 0.0f, &m, &m, juce::Time::getCurrentTime(), pos, juce::Time::getCurrentTime(), 1,
                       false);
    m.mouseMove(e);
    CHECK_THAT(m.getTooltip().toStdString(), ContainsSubstring("Kick vs Bass at 20-60 Hz"));
    m.mouseUp(e);
    CHECK(clicked == 0);
    m.setAnalysis(nullptr);
    CHECK(m.cellAt({125, 25}).first == -1);
}
