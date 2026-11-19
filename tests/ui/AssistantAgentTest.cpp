// tests/ui/AssistantAgentTest.cpp
// CE-AG-1 / CE-AG-2 / CE-AG-4: the presentation of the auto-mix step log,
// the LLM provider detection report and the natural-language command
// preview, read from service JSON through the generated api::fields names.
#include <catch2/catch_test_macros.hpp>
#include <JuceHeader.h>
#include "../../desktop-app/Source/ui/Copilot/AutoMixPanel.h"
#include "../../desktop-app/Source/ui/Settings/LlmProviderPanel.h"
#include "../../desktop-app/Source/ui/shared/NlCommandOverlay.h"
#include "../../desktop-app/Source/settings/OnlineSettings.h"

using namespace khensu;

namespace {
juce::var json(const char* text) { return juce::JSON::parse(juce::String(text)); }
} // namespace

TEST_CASE("Auto-mix step log shows decisions, attempts and matched gain", "[ce-ag][automix]")
{
    const auto status = json(R"J({
        "status": "completed", "stage": "done",
        "result": {
            "source_path": "a.wav", "final_path": "a_automix/a_s1.wav", "changed": true,
            "kept_count": 1, "reverted_count": 1, "summary": "Kept 1 of 2 step(s).",
            "findings": [{"id": "agent:mud_rel_db"}, {"id": "agent:stereo_width"}],
            "before": {"lufs": -20.0, "true_peak_dbtp": -6.0},
            "after": {"lufs": -14.1, "true_peak_dbtp": -1.0},
            "remaining_out_of_range": ["stereo_width"],
            "steps": [
                {"index": 1, "finding_id": "agent:mud_rel_db", "title": "Low-mid build-up (mud)",
                 "reason": "Low mids are 13.0 dB.", "decision": "kept", "decision_reason": "ok",
                 "attempts": [
                    {"strength": 1.0, "effects": [{"effect": "PeakFilter", "parameters": {}}],
                     "decision": "reverted", "reason": "Guardrail blocked the render: x"},
                    {"strength": 0.5, "effects": [{"effect": "PeakFilter", "parameters": {}}],
                     "decision": "kept", "reason": "Low mids 13.0 -> 10.0", "matched_gain_db": 0.4}]},
                {"index": 2, "finding_id": "agent:stereo_width", "title": "Image too narrow",
                 "reason": "", "decision": "skipped", "decision_reason": "Already in range after the earlier steps.",
                 "attempts": []}]}})J");
    const auto log = AutoMixPanel::formatStepLog(status);
    CHECK(log.contains("Plan: 2 step(s)"));
    CHECK(log.contains("1. Low-mid build-up (mud)  [KEPT]"));
    CHECK(log.contains("strength 1.00 (PeakFilter): reverted - Guardrail blocked"));
    CHECK(log.contains("[matched by 0.4 dB]"));
    CHECK(log.contains("2. Image too narrow  [SKIPPED]"));
    CHECK(log.contains("Already in range"));
    CHECK(log.contains("-20.0 -> -14.1 LUFS"));
    CHECK(log.contains("Still out of range: stereo_width"));
}

TEST_CASE("Auto-mix step log reads the live progress of a running job", "[ce-ag][automix]")
{
    const auto status = json(R"J({"status": "running", "stage": "step 1/3: rendering",
        "progress": {"stage": "step 1/3: rendering", "findings": [{}, {}, {}],
                     "steps": [{"index": 1, "finding_id": "f", "title": "Harshness", "decision": "pending",
                                "attempts": [{"strength": 1.0, "effects": [], "decision": ""}]}]}})J");
    const auto log = AutoMixPanel::formatStepLog(status);
    CHECK(log.contains("Plan: 3 step(s)"));
    CHECK(log.contains("1. Harshness  [RUNNING]"));
}

TEST_CASE("Provider report names the provider in use and what was detected", "[ce-ag][llm]")
{
    const auto report = json(R"J({
        "choice": "auto", "choice_source": "default", "online_llm_allowed": false,
        "local_servers": [
            {"id": "ollama", "label": "Ollama", "base_url": "http://127.0.0.1:11434/v1", "available": true,
             "models": ["llama3.1:8b", "phi3"]},
            {"id": "lmstudio", "label": "LM Studio", "base_url": "http://127.0.0.1:1234/v1", "available": false,
             "reason": "not running"}],
        "llamacpp": {"available": false, "reason": "llama-cpp-python is not installed"},
        "cloud": [],
        "active": {"chat": {"choice": "auto", "source": "default", "provider": "ollama",
                            "model": "llama3.1:8b", "local": true}}})J");
    const auto text = LlmProviderPanel::formatReport(report);
    CHECK(text.contains("In use: ollama (llama3.1:8b) - local, nothing leaves this machine (choice: auto)"));
    CHECK(text.contains("Found: Ollama (2 models)"));
    CHECK(text.contains("Not running: LM Studio"));
    CHECK(text.contains("In-process GGUF: llama-cpp-python is not installed"));
    CHECK(text.contains("Cloud LLMs are off"));
}

TEST_CASE("Local LLM choices do not count as cloud for the chat disclosure", "[ce-ag][llm]")
{
    OnlineSettings s;
    for (const char* local : {"local", "ollama", "lmstudio", "llamacpp_server", "llamacpp"}) {
        s.llmProvider = local;
        CHECK_FALSE(isCloudProviderSelected(s));
    }
    s.llmProvider = "auto"; // may fall back to a cloud key: still disclosed
    CHECK(isCloudProviderSelected(s));
    s.llmProvider = "anthropic";
    CHECK(isCloudProviderSelected(s));
}

TEST_CASE("Local LLM settings round-trip through settings.json", "[ce-ag][llm]")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu_ce_ag2_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    REQUIRE(dir.createDirectory());
    OnlineSettings s;
    s.llmProvider = "llamacpp";
    s.llmGgufPath = "C:/models/tiny.gguf";
    s.llmLocalUrl = "http://127.0.0.1:5000/v1";
    REQUIRE(s.save(dir));
    const auto back = OnlineSettings::load(dir);
    CHECK(back.llmProvider == "llamacpp");
    CHECK(back.llmGgufPath == "C:/models/tiny.gguf");
    CHECK(back.llmLocalUrl == "http://127.0.0.1:5000/v1");
    dir.deleteRecursively();
}

TEST_CASE("Command preview lists effects by scope and what was not done", "[ce-ag][command]")
{
    const auto parsed = json(R"J({
        "text": "make the chorus louder and brighten the vocal", "parser": "grammar",
        "commands": [{"clause": "make the chorus louder", "intent": "level", "direction": 1,
                      "target_kind": "section", "explanation": "Level +1.5 dB on the chorus (30.0-50.0 s)"},
                     {"clause": "brighten the vocal", "intent": "brightness", "direction": 1,
                      "target_kind": "stem", "explanation": ""}],
        "plan": {"mix_effects": [{"effect": "PeakFilter", "parameters": {"gain_db": -2.0}}],
                 "sections": [{"label": "C", "role": "chorus", "start_sec": 30.0, "end_sec": 50.0,
                               "effects": [{"effect": "Gain", "parameters": {"gain_db": 1.5}}]}],
                 "stems": [], "resolved_sections": []},
        "unresolved": ["\"brighten the vocal\": targets the vocals stem, which has not been separated yet."],
        "warnings": [], "executable": true, "plan_id": "abc"})J");
    const auto text = NlCommandOverlay::formatPreview(parsed);
    CHECK(text.contains("- Level +1.5 dB on the chorus (30.0-50.0 s)"));
    CHECK(text.contains("- brighten the vocal"));
    CHECK(text.contains("Whole mix:"));
    CHECK(text.contains("PeakFilter (gain_db -2.00)"));
    CHECK(text.contains("chorus  30.0-50.0 s"));
    CHECK(text.contains("Gain (gain_db 1.50)"));
    CHECK(text.contains("Not done:"));
    CHECK_FALSE(text.contains("language model"));
}
