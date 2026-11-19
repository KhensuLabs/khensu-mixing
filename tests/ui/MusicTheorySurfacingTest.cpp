// tests/ui/MusicTheorySurfacingTest.cpp
// AUDIT-016 / 052 / 084 (desktop side): the intonation calls now carry the
// mix's tuning reference, the pitch editor's Retune targets that grid, and
// the key/BPM/chord displays surface meter, Camelot, Roman numerals and
// confidence instead of dropping them.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "support/FakeApplyChainServer.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/DeepAnalysisResponse.h>
#include "../../desktop-app/Source/ui/VocalWorkspace/MixTuningReference.h"
#include "../../desktop-app/Source/ui/VocalWorkspace/PitchNoteGridModel.h"
#include "../../desktop-app/Source/ui/shared/MusicalContextText.h"
#include "../../desktop-app/Source/ui/Dashboard/ChordsPanel.h"

using khensu::ipc::HttpServiceBridge;
using Catch::Matchers::WithinAbs;

TEST_CASE("Intonation bridge calls carry reference_hz in the request body", "[tuning][ipc][integration]")
{
    khensu::test::FakeApplyChainServer server(19767);
    server.setResponse("POST /analyze/pitch_accuracy", {200, R"({"notes":[],"mean_abs_cents":0})"});
    server.setResponse("POST /analyze/pitch_curve", {200, R"({"notes":[],"f0_midi":[]})"});
    server.setResponse("POST /analyze/sound_recreation", {200, R"({"category":"lead"})"});
    server.setResponse("POST /analyze/tuning_reference", {200, R"({"reference_hz":432.0,"cents_from_440":-31.8})"});

    HttpServiceBridge bridge(server.baseUrl().toStdString(), 1000, 0);
    REQUIRE(bridge.analyzePitchAccuracy("C:/vox.wav", 432.0).is_ok());
    REQUIRE(bridge.analyzePitchCurve("C:/vox.wav", 432.0).is_ok());
    REQUIRE(bridge.analyzeSoundRecreation("C:/vox.wav", "auto", 432.0).is_ok());

    for (const char* route :
         {"POST /analyze/pitch_accuracy", "POST /analyze/pitch_curve", "POST /analyze/sound_recreation"}) {
        const auto body = juce::JSON::parse(server.lastBody(route));
        INFO(route);
        REQUIRE(body.hasProperty("reference_hz"));
        CHECK_THAT(static_cast<double>(body["reference_hz"]), WithinAbs(432.0, 1e-9));
    }

    // No reference, or one the service would 422 on: the field is omitted
    // and the service keeps its A=440 default.
    REQUIRE(bridge.analyzePitchAccuracy("C:/vox.wav").is_ok());
    CHECK_FALSE(juce::JSON::parse(server.lastBody("POST /analyze/pitch_accuracy")).hasProperty("reference_hz"));
    REQUIRE(bridge.analyzePitchCurve("C:/vox.wav", 512.0).is_ok());
    CHECK_FALSE(juce::JSON::parse(server.lastBody("POST /analyze/pitch_curve")).hasProperty("reference_hz"));

    // The cache resolves the mix reference through the bridge.
    khensu::MixTuningReference cache;
    int fetches = 0;
    auto fetch = [&](const juce::String& p) -> std::optional<juce::var> {
        ++fetches;
        auto r = bridge.analyzeTuningReference(p.toStdString());
        if (r.is_err()) return std::nullopt;
        return r.value();
    };
    const auto ref = cache.resolve("C:/mix.wav", uint64_t{7}, fetch);
    REQUIRE(ref.has_value());
    CHECK_THAT(*ref, WithinAbs(432.0, 1e-9));
    CHECK(server.requestCount("POST /analyze/tuning_reference") == 1);
}

TEST_CASE("MixTuningReference fetches once per session epoch", "[tuning]")
{
    khensu::MixTuningReference cache;
    int fetches = 0;
    double served = 432.0;
    auto fetch = [&](const juce::String&) -> std::optional<juce::var> {
        ++fetches;
        auto* o = new juce::DynamicObject();
        o->setProperty("reference_hz", served);
        return juce::var(o);
    };

    CHECK_THAT(*cache.resolve("C:/mix.wav", uint64_t{1}, fetch), WithinAbs(432.0, 1e-9));
    CHECK_THAT(*cache.resolve("C:/mix.wav", uint64_t{1}, fetch), WithinAbs(432.0, 1e-9));
    CHECK(fetches == 1);

    // A new session epoch re-measures.
    served = 441.0;
    CHECK_THAT(*cache.resolve("C:/mix.wav", uint64_t{2}, fetch), WithinAbs(441.0, 1e-9));
    CHECK(fetches == 2);

    // Out-of-window references are not sent (A=440 default), but are cached.
    served = 300.0;
    CHECK_FALSE(cache.resolve("C:/other.wav", uint64_t{2}, fetch).has_value());
    CHECK_FALSE(cache.resolve("C:/other.wav", uint64_t{2}, fetch).has_value());
    CHECK(fetches == 3);

    // A failed fetch is not cached - the next caller retries.
    int failing = 0;
    auto failingFetch = [&](const juce::String&) -> std::optional<juce::var> {
        ++failing;
        return std::nullopt;
    };
    CHECK_FALSE(cache.resolve("C:/x.wav", uint64_t{3}, failingFetch).has_value());
    CHECK_FALSE(cache.resolve("C:/x.wav", uint64_t{3}, failingFetch).has_value());
    CHECK(failing == 2);
}

TEST_CASE("Pitch editor retune targets the response's reference grid, not 440", "[tuning][vocal]")
{
    // A take in tune at A=432: on the 432 grid every note sits on its target,
    // so Retune-all at 100% must move nothing (on a 440 grid it read ~-32
    // cents and pulled the vocal sharp against the band).
    const auto v = juce::JSON::parse(R"({
        "duration_sec": 2.0, "hop_sec": 0.01, "f0_midi": [69.0, 69.0],
        "notes": [{"id": 0, "start_sec": 0.0, "end_sec": 1.0, "midi": 69.0, "target_midi": 69},
                  {"id": 1, "start_sec": 1.0, "end_sec": 2.0, "midi": 71.02, "target_midi": 71}],
        "reference_hz": 432.0, "measurement_reference_hz": 432.0,
        "detected_reference_hz": 432.1, "reference_offset_cents": -31.4,
        "reference_offset_warning": false})");
    khensu::PitchNoteGridModel model;
    REQUIRE(model.loadFromResponse(v));
    CHECK_THAT(model.tuning().gridHz, WithinAbs(432.0, 1e-9));
    CHECK_FALSE(model.tuning().offsetWarning);
    CHECK(model.tuning().warningText().isEmpty());
    CHECK(model.tuning().gridText().contains("432.0"));

    model.retuneAll(1.0);
    REQUIRE(model.notes().size() == 2);
    CHECK_THAT(model.notes()[0].shift, WithinAbs(0.0, 1e-9));
    CHECK_THAT(model.notes()[1].shift, WithinAbs(-0.02, 1e-9));
}

TEST_CASE("Tuning reading surfaces the service's off-grid warning", "[tuning]")
{
    const auto v = juce::JSON::parse(R"({"measurement_reference_hz": 440.0, "detected_reference_hz": 432.0,
                                         "reference_offset_cents": -31.8, "reference_offset_warning": true})");
    const auto t = khensu::TuningReading::fromResponse(v);
    CHECK(t.offsetWarning);
    CHECK_THAT(t.offsetFromGridCents(), WithinAbs(-31.8, 0.1));
    CHECK(t.warningText().contains("-32"));
    CHECK(t.warningText().contains("432.0"));
}

TEST_CASE("Deep analysis response keeps meter, felt tempo and Camelot", "[ipc][music_theory]")
{
    const auto json = juce::JSON::parse(R"({
        "mode": "advanced", "bpm": 192.0, "bpm_confidence": 0.87, "key": "A minor", "key_confidence": 0.78,
        "sections": [], "beat_times": [], "downbeat_times": [],
        "time_signature": "6/8", "meter_confidence": 0.6, "felt_bpm": 64.0,
        "camelot": "8A", "relative_key": "C major"})");
    const auto resp = khensu::ipc::DeepAnalysisResponse::from_json(json);
    REQUIRE(resp.has_value());
    khensu::AnalysisResult::PythonData pd;
    resp->apply_to(pd);
    CHECK(pd.time_signature == "6/8");
    CHECK_THAT(pd.felt_bpm, WithinAbs(64.0f, 1e-4f));
    CHECK(pd.camelot == "8A");
    CHECK(pd.relative_key == "C major");

    const auto tempo = khensu::musical_context::tempoDetail(pd);
    CHECK(tempo.contains("6/8"));
    CHECK(tempo.contains("64.0"));
    CHECK(tempo.contains("87%"));
    const auto key = khensu::musical_context::keyDetail(pd);
    CHECK(key.contains("8A"));
    CHECK(key.contains("78%"));

    // Quick mode: nulls stay empty, and the low-confidence 4/4 fallback is
    // not presented as a detected meter.
    khensu::AnalysisResult::PythonData quick;
    quick.time_signature = "4/4";
    quick.meter_confidence = 0.1f;
    CHECK_FALSE(khensu::musical_context::hasDetectedMeter(quick));
    CHECK(khensu::musical_context::tempoDetail(quick).isEmpty());
}

TEST_CASE("Chord chart shows N.C., Roman numerals, Camelot and key confidence", "[chords][music_theory]")
{
    khensu::AnalysisResult r;
    khensu::ChordAnalysis ca;
    ca.key = "A";
    ca.mode = "minor";
    ca.key_confidence = 0.82f;
    ca.camelot = "8A";
    ca.relative_key = "C major";
    khensu::ChordEvent nc;
    nc.time_sec = 0.0f;
    nc.chord = "N";
    khensu::ChordEvent am;
    am.time_sec = 2.0f;
    am.chord = "Am";
    am.roman = "i";
    am.diatonic = true;
    am.has_diatonic = true;
    khensu::ChordEvent bb;
    bb.time_sec = 4.0f;
    bb.chord = "Bb";
    bb.roman = "bII";
    bb.diatonic = false;
    bb.has_diatonic = true;
    ca.chords = {nc, am, bb};
    r.chord_analysis = ca;

    const auto text = khensu::ChordsPanel::buildText(r);
    CHECK(text.contains("N.C."));
    CHECK_FALSE(text.contains("  N\n"));
    CHECK(text.contains("8A"));
    CHECK(text.contains("82%"));
    CHECK(text.contains("bII"));
    // THY-05: the Neapolitan Bb in A minor is chromatic, not modal mixture.
    CHECK(text.contains("chromatic"));
    CHECK_FALSE(text.contains("borrowed"));
}

TEST_CASE("Chord fallback numbers bars from downbeats and spells for the key", "[chords][music_theory]")
{
    khensu::AnalysisResult r;
    r.python_data.pitch_key = "F major";
    // Bar 1 (0-2 s): Bb triad; bar 2 (2-4 s): silent; bar 3 (4-6 s): Am7.
    r.python_data.downbeat_times_json = "[0.0, 2.0, 4.0]";
    r.python_data.pitch_notes_json = R"([
        {"start_sec": 0.1, "end_sec": 1.9, "midi_pitch": 46, "velocity": 0.8},
        {"start_sec": 0.1, "end_sec": 1.9, "midi_pitch": 50, "velocity": 0.8},
        {"start_sec": 0.1, "end_sec": 1.9, "midi_pitch": 53, "velocity": 0.8},
        {"start_sec": 4.1, "end_sec": 5.9, "midi_pitch": 45, "velocity": 0.8},
        {"start_sec": 4.1, "end_sec": 5.9, "midi_pitch": 48, "velocity": 0.8},
        {"start_sec": 4.1, "end_sec": 5.9, "midi_pitch": 52, "velocity": 0.8},
        {"start_sec": 4.1, "end_sec": 5.9, "midi_pitch": 55, "velocity": 0.8}])";

    const auto text = khensu::ChordsPanel::buildText(r);
    INFO(text);
    CHECK(text.contains("M1 (0.0s): Bb"));
    CHECK(text.contains("M2 (2.0s): N.C."));
    CHECK(text.contains("M3 (4.0s): Am7")); // not "M2", not "C"
    CHECK_FALSE(text.contains("A#"));
}
