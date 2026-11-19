#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/ipc/ILocalServiceBridge.h>
#include <khensu/ipc/DeepAnalysisRequest.h>
#include <khensu/ipc/DeepAnalysisResponse.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <juce_core/juce_core.h>

using namespace khensu;
using namespace khensu::ipc;
using Catch::Approx;

TEST_CASE("AnalysisResult::PythonData has beat/downbeat/pitch JSON string fields", "[ipc][compile]")
{
    AnalysisResult r;
    r.python_data.beat_times_json = "[0.5,1.0]";
    r.python_data.downbeat_times_json = "[0.5]";
    r.python_data.pitch_notes_json = "[]";

    CHECK(r.python_data.beat_times_json == "[0.5,1.0]");
    CHECK(r.python_data.downbeat_times_json == "[0.5]");
    CHECK(r.python_data.pitch_notes_json == "[]");
}

TEST_CASE("DeepAnalysisRequest::to_json serializes quick mode", "[ipc][request]")
{
    DeepAnalysisRequest req;
    req.file_path = "/audio/track.wav";
    req.mode = ILocalServiceBridge::AnalysisMode::Quick;

    auto json = req.to_json();
    auto parsed = juce::JSON::parse(json);

    CHECK(parsed["file_path"].toString() == "/audio/track.wav");
    CHECK(parsed["mode"].toString() == "quick");
}

TEST_CASE("DeepAnalysisRequest::to_json serializes advanced mode", "[ipc][request]")
{
    DeepAnalysisRequest req;
    req.file_path = "/audio/track.wav";
    req.mode = ILocalServiceBridge::AnalysisMode::Advanced;

    auto json = req.to_json();
    auto parsed = juce::JSON::parse(json);

    CHECK(parsed["mode"].toString() == "advanced");
}

TEST_CASE("DeepAnalysisResponse::from_json maps scalar fields", "[ipc][response]")
{
    juce::String jsonStr = R"({
        "mode": "quick",
        "bpm": 128.0,
        "bpm_confidence": 0.85,
        "beat_times": [0.5, 1.0, 1.5],
        "downbeat_times": null,
        "key": "D major",
        "key_confidence": 0.78,
        "sections": [{"start_sec": 0.0, "label": "A"}],
        "pitch_notes": null,
        "analysis_duration_ms": 300.0
    })";

    auto parsed = juce::JSON::parse(jsonStr);
    auto resp = DeepAnalysisResponse::from_json(parsed);

    REQUIRE(resp.has_value());
    CHECK(resp->bpm == Approx(128.0f));
    CHECK(resp->bpm_confidence == Approx(0.85f));
    CHECK(resp->key == "D major");
    CHECK(resp->key_confidence == Approx(0.78f));
    CHECK(resp->downbeat_times_json == "");
    CHECK(resp->pitch_notes_json == "");
}

TEST_CASE("DeepAnalysisResponse::from_json stores sections as JSON string", "[ipc][response]")
{
    juce::String jsonStr = R"({
        "mode": "quick",
        "bpm": 120.0,
        "bpm_confidence": 0.80,
        "beat_times": [0.5, 1.0],
        "downbeat_times": null,
        "key": "A minor",
        "key_confidence": 0.72,
        "sections": [{"start_sec": 0.0, "label": "A"}, {"start_sec": 16.0, "label": "B"}],
        "pitch_notes": null,
        "analysis_duration_ms": 450.0
    })";

    auto parsed = juce::JSON::parse(jsonStr);
    auto resp = DeepAnalysisResponse::from_json(parsed);

    REQUIRE(resp.has_value());
    CHECK(!resp->sections_json.empty());
    CHECK(resp->sections_json.find("start_sec") != std::string::npos);
    CHECK(resp->sections_json.find("label") != std::string::npos);

    // Verify apply_to maps into PythonData
    khensu::AnalysisResult::PythonData pd;
    resp->apply_to(pd);
    CHECK(pd.bpm_detected == Approx(120.0f));
    CHECK(pd.pitch_key == "A minor");
    CHECK(!pd.sections_json.empty());
}

TEST_CASE("HttpServiceBridge returns ServiceUnavailable when port is not listening", "[ipc][http]")
{
    // Port 1 is guaranteed to refuse connections on all platforms.
    HttpServiceBridge bridge("http://127.0.0.1:1", 500, 100);

    auto result = bridge.analyze("/any/path.wav", ILocalServiceBridge::AnalysisMode::Quick);

    REQUIRE(result.is_err());
    CHECK(result.error().kind == BridgeError::Kind::ServiceUnavailable);
}

TEST_CASE("AnalysisResult::PythonData has SP-06 typed fields", "[sp06][compile]")
{
    AnalysisResult r;
    r.python_data.hooks.hook_strength = 0.8f;
    r.python_data.hooks.hook_count = 2;
    r.python_data.hooks.instances.emplace_back();
    r.python_data.memorability.score = 0.7f;
    r.python_data.structural_flow.arc_quality = 0.6f;
    r.python_data.heaviness.distortion = 0.3f;

    REQUIRE(r.python_data.hooks.hook_strength == Approx(0.8f));
    REQUIRE(r.python_data.memorability.score == Approx(0.7f));
    REQUIRE(r.python_data.structural_flow.arc_quality == Approx(0.6f));
    REQUIRE(r.python_data.heaviness.distortion == Approx(0.3f));
    REQUIRE(r.python_data.hooks.instances.size() == 1);
}

TEST_CASE("from_json returns nullopt when required field bpm is absent", "[sp06][response]")
{
    juce::String jsonStr = R"({"key": "A minor", "key_confidence": 0.8, "sections": []})";
    auto parsed = juce::JSON::parse(jsonStr);
    auto result = DeepAnalysisResponse::from_json(parsed);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("from_json defaults SP-06 fields to zero when absent (quick mode)", "[sp06][response]")
{
    juce::String jsonStr = R"({
        "bpm": 120.0, "bpm_confidence": 0.80,
        "key": "A minor", "key_confidence": 0.72,
        "sections": [{"start_sec": 0.0, "label": "A"}],
        "beat_times": [0.5, 1.0],
        "analysis_duration_ms": 450.0
    })";
    auto parsed = juce::JSON::parse(jsonStr);
    auto result = DeepAnalysisResponse::from_json(parsed);
    REQUIRE(result.has_value());
    CHECK(result->hook_strength == Approx(0.0f));
    CHECK(result->hook_count == 0);
    CHECK(result->hook_instances.empty());
    CHECK(result->memorability_score == Approx(0.0f));
    CHECK(result->heaviness_distortion == Approx(0.0f));
}

TEST_CASE("from_json parses hook_instances array and maps hook_type", "[sp06][response]")
{
    juce::String jsonStr = R"({
        "bpm": 128.0, "bpm_confidence": 0.95,
        "key": "E minor", "key_confidence": 0.82,
        "sections": [{"start_sec": 0.0, "label": "A"}],
        "beat_times": [],
        "analysis_duration_ms": 10000.0,
        "hook_strength": 0.75,
        "hook_density": 1.5,
        "hook_count": 1,
        "hook_instances": [
            {"start_sec": 8.0, "end_sec": 24.0, "hook_type": "topline", "strength": 0.75}
        ]
    })";
    auto parsed = juce::JSON::parse(jsonStr);
    auto result = DeepAnalysisResponse::from_json(parsed);
    REQUIRE(result.has_value());
    REQUIRE(result->hook_instances.size() == 1);
    CHECK(result->hook_instances[0].type == khensu::HookType::Topline);
    CHECK(result->hook_instances[0].strength == Approx(0.75f));
    CHECK(result->hook_count == 1);
    CHECK(result->hook_strength == Approx(0.75f));
}

TEST_CASE("apply_to maps hook_instances and summary scalars into PythonData", "[sp06][apply_to]")
{
    DeepAnalysisResponse resp;
    resp.hook_strength = 0.8f;
    resp.hook_density = 2.0f;
    resp.hook_count = 1;
    khensu::HookInstance inst;
    inst.start_sec = 8.0f;
    inst.end_sec = 16.0f;
    inst.type = khensu::HookType::Compound;
    inst.strength = 0.9f;
    resp.hook_instances.push_back(inst);

    khensu::AnalysisResult::PythonData dst;
    resp.apply_to(dst);

    CHECK(dst.hooks.hook_strength == Approx(0.8f));
    CHECK(dst.hooks.hook_count == 1);
    REQUIRE(dst.hooks.instances.size() == 1);
    CHECK(dst.hooks.instances[0].type == khensu::HookType::Compound);
    CHECK(dst.hooks.instances[0].strength == Approx(0.9f));
}

TEST_CASE("apply_to maps memorability and heaviness into PythonData", "[sp06][apply_to]")
{
    DeepAnalysisResponse resp;
    resp.memorability_score = 0.65f;
    resp.earworm_likelihood = 0.40f;
    resp.melody_salience = 0.50f;
    resp.arc_quality = 0.72f;
    resp.heaviness_distortion = 0.30f;
    resp.heaviness_weight = 0.25f;
    resp.heaviness_clarity = 0.80f;
    resp.heaviness_extremity = 0.275f;

    khensu::AnalysisResult::PythonData dst;
    resp.apply_to(dst);

    CHECK(dst.memorability.score == Approx(0.65f));
    CHECK(dst.memorability.earworm_likelihood == Approx(0.40f));
    CHECK(dst.structural_flow.arc_quality == Approx(0.72f));
    CHECK(dst.heaviness.distortion == Approx(0.30f));
    CHECK(dst.heaviness.clarity == Approx(0.80f));
}

TEST_CASE("StemSeparationResult structure and startSeparationJob error path", "[ipc][separate]")
{
    StemSeparationResult res;
    res.vocals_path = "vocals.wav";
    res.drums_path = "drums.wav";
    res.bass_path = "bass.wav";
    res.other_path = "other.wav";

    CHECK(res.vocals_path == "vocals.wav");
    CHECK(res.drums_path == "drums.wav");
    CHECK(res.bass_path == "bass.wav");
    CHECK(res.other_path == "other.wav");

    HttpServiceBridge bridge("http://127.0.0.1:9999", 50, 10);
    auto outcome = bridge.startSeparationJob("input.wav", "out");
    REQUIRE(outcome.is_err());
    CHECK(outcome.error().kind == BridgeError::Kind::ServiceUnavailable);
}

TEST_CASE("HttpServiceBridge::generateMixerChain error path", "[ipc][mixer_chain]")
{
    HttpServiceBridge bridge("http://127.0.0.1:9999", 50, 10);
    juce::StringArray eco;
    eco.add("Valhalla");
    auto outcome = bridge.generateMixerChain("vocals", "pop", "high", "dreamy", 120.0, "style desc", eco);
    REQUIRE(outcome.is_err());
    CHECK(outcome.error().kind == BridgeError::Kind::ServiceUnavailable);
}

TEST_CASE("engineDidNotAnswer separates 'no answer' from 'an answer about the request' (FIX-2)", "[ipc][fix2]")
{
    using khensu::ipc::BridgeError;
    using khensu::ipc::engineDidNotAnswer;
    const auto err = [](BridgeError::Kind kind, int status) {
        BridgeError e;
        e.kind = kind;
        e.http_status = status;
        return e;
    };
    CHECK(engineDidNotAnswer(err(BridgeError::Kind::ServiceUnavailable, 0)));
    CHECK(engineDidNotAnswer(err(BridgeError::Kind::Timeout, 0)));
    CHECK(engineDidNotAnswer(err(BridgeError::Kind::HttpError, 429)));
    CHECK(engineDidNotAnswer(err(BridgeError::Kind::HttpError, 503)));
    CHECK(engineDidNotAnswer(err(BridgeError::Kind::HttpError, 504)));
    // The engine answered: the request itself is the problem.
    CHECK_FALSE(engineDidNotAnswer(err(BridgeError::Kind::HttpError, 404)));
    CHECK_FALSE(engineDidNotAnswer(err(BridgeError::Kind::HttpError, 413)));
    CHECK_FALSE(engineDidNotAnswer(err(BridgeError::Kind::HttpError, 422)));
    CHECK_FALSE(engineDidNotAnswer(err(BridgeError::Kind::HttpError, 500)));
    CHECK_FALSE(engineDidNotAnswer(err(BridgeError::Kind::ParseError, 200)));
}

// --- Music-theory identity (wiring H2) ---------------------------------------

TEST_CASE("DeepAnalysisResponse::from_json parses meter and key relationships", "[ipc][response][theory]")
{
    juce::String jsonStr = R"({
        "mode": "advanced", "bpm": 120.0, "bpm_confidence": 0.9,
        "key": "C major", "key_confidence": 0.8, "sections": [],
        "analysis_duration_ms": 10.0,
        "time_signature": "6/8", "meter_confidence": 0.64,
        "camelot": "8B", "circle_of_fifths_index": 0,
        "relative_key": "A minor", "parallel_key": "C minor"
    })";
    auto resp = DeepAnalysisResponse::from_json(juce::JSON::parse(jsonStr));
    REQUIRE(resp.has_value());
    CHECK(resp->time_signature == "6/8");
    CHECK(resp->meter_confidence == Approx(0.64f));
    CHECK(resp->camelot == "8B");
    CHECK(resp->circle_of_fifths_index == 0);
    CHECK(resp->relative_key == "A minor");
    CHECK(resp->parallel_key == "C minor");

    AnalysisResult::PythonData pd;
    resp->apply_to(pd);
    CHECK(pd.time_signature == "6/8");
    CHECK(pd.meter_confidence == Approx(0.64f));
    CHECK(pd.camelot == "8B");
    CHECK(pd.relative_key == "A minor");
    CHECK(pd.parallel_key == "C minor");
}

TEST_CASE("DeepAnalysisResponse::from_json leaves theory fields unknown on null", "[ipc][response][theory]")
{
    juce::String jsonStr = R"({
        "mode": "quick", "bpm": 0.0, "bpm_confidence": 0.0, "key": "", "key_confidence": 0.0,
        "sections": [], "analysis_duration_ms": 1.0,
        "time_signature": null, "meter_confidence": null, "camelot": null,
        "circle_of_fifths_index": null, "relative_key": null, "parallel_key": null,
        "harshness_score": 0.0
    })";
    auto resp = DeepAnalysisResponse::from_json(juce::JSON::parse(jsonStr));
    REQUIRE(resp.has_value());
    CHECK(resp->time_signature.empty());
    CHECK(resp->meter_confidence == 0.0f); // null reads as "not measured"
    CHECK(resp->camelot.empty());
    CHECK(resp->circle_of_fifths_index == -1);
    CHECK(resp->parallel_key.empty());
}

TEST_CASE("DeepAnalysisResponse::chords_from_json parses roman numerals and relations", "[ipc][chords][theory]")
{
    juce::String jsonStr = R"({
        "key": "C", "mode": "major", "key_confidence": 0.7,
        "camelot": "8B", "circle_of_fifths_index": 0,
        "relative_key": "A minor", "parallel_key": "C minor",
        "chords": [
            {"time_sec": 0.0, "chord": "C",  "confidence": 0.9, "roman": "I",    "diatonic": true},
            {"time_sec": 2.0, "chord": "Bb", "confidence": 0.8, "roman": "bVII", "diatonic": false},
            {"time_sec": 4.0, "chord": "N",  "confidence": 0.0, "roman": null,   "diatonic": null}
        ]
    })";
    auto ca = DeepAnalysisResponse::chords_from_json(juce::JSON::parse(jsonStr));
    REQUIRE(ca.has_value());
    CHECK(ca->camelot == "8B");
    CHECK(ca->circle_of_fifths_index == 0);
    CHECK(ca->relative_key == "A minor");
    CHECK(ca->parallel_key == "C minor");
    REQUIRE(ca->chords.size() == 3);
    CHECK(ca->chords[0].roman == "I");
    CHECK(ca->chords[0].diatonic);
    CHECK(ca->chords[0].has_diatonic);
    CHECK(ca->chords[1].roman == "bVII");
    CHECK_FALSE(ca->chords[1].diatonic);
    CHECK(ca->chords[2].chord == "N.C.");
    CHECK(ca->chords[2].roman.empty());
    CHECK_FALSE(ca->chords[2].has_diatonic); // unknown: neither diatonic nor borrowed

    CHECK_FALSE(DeepAnalysisResponse::chords_from_json(juce::var("nope")).has_value());
}
