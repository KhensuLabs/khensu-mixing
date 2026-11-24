// tests/shared-core/test_chain_preset.cpp
// FN2-5: the shared chain preset format (plugin + desktop rack).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <khensu/presets/ChainPreset.h>

using namespace khensu::presets;

namespace {
struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getNonexistentChildFile("khensu-chain-presets", "");
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

ChainPreset sample()
{
    ChainPreset p;
    p.name = "Club master / v2";
    p.author = "Someone";
    p.source = "Khensu Copilot";
    p.params = {{"comp_on", 1.0f}, {"comp_thresh_db", -18.5f}, {"eq2_type", 2.0f}, {"hpf_freq", 31.25f}};
    p.stages = {{"reverb", {{"mix", 0.2f}, {"decay_s", 1.8f}}}, {"saturator", {}}};
    return p;
}
} // namespace

TEST_CASE("A chain preset round-trips through its file (FN2-5)", "[presets][fn2-5]")
{
    TempDir tmp;
    juce::File written;
    juce::String error;
    REQUIRE(save(sample(), tmp.dir, written, error));
    CHECK(written.getFileName().endsWith(kChainPresetExtension));
    CHECK(written.getParentDirectory() == tmp.dir);

    const auto back = loadPreset(written, &error);
    REQUIRE(back.has_value());
    CHECK(back->name == "Club master / v2");
    CHECK(back->author == "Someone");
    CHECK(back->source == "Khensu Copilot");
    CHECK(back->created.isNotEmpty()); // filled in on save
    CHECK(back->version == kChainPresetVersion);
    CHECK(back->params == sample().params);
    CHECK(back->stages == sample().stages);

    // The text names its format and version.
    const auto json = juce::JSON::parse(written.loadFileAsString());
    CHECK(json["format"].toString() == kChainPresetFormat);
    CHECK(static_cast<int>(json["version"]) == kChainPresetVersion);

    // Saving the same name replaces the file, never adds a second one.
    auto again = sample();
    again.params["comp_on"] = 0.0f;
    REQUIRE(save(again, tmp.dir, written, error));
    const auto listed = listPresets(tmp.dir);
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].preset.params.at("comp_on") == 0.0f);

    // A blank name is refused.
    ChainPreset blank = sample();
    blank.name = "  ";
    CHECK_FALSE(save(blank, tmp.dir, written, error));
    CHECK(error.isNotEmpty());
}

TEST_CASE("Unknown keys are ignored and an older version loads (FN2-5)", "[presets][fn2-5]")
{
    // A newer writer's extra keys - top level, in params' neighbours and in a stage.
    const auto newer = fromJson(R"({"format":"khensu.chain_preset","version":7,"name":"Future",
        "colour":"teal","tags":["a","b"],"params":{"eq_on":1,"eq_gain_db":-2.5,"label":"text is skipped"},
        "stages":[{"effect":"reverb","parameters":{"mix":0.3},"bypassed":true},{"no_effect":1},"junk"],
        "automation":{"x":1}})");
    REQUIRE(newer.has_value());
    CHECK(newer->name == "Future");
    CHECK(newer->version == 7);
    CHECK(newer->params.size() == 2);
    CHECK(newer->params.at("eq_gain_db") == Catch::Approx(-2.5f));
    REQUIRE(newer->stages.size() == 1);
    CHECK(newer->stages[0].effect == "reverb");

    // An early file: no format, no version, no stages, no author/created.
    const auto older = fromJson(R"({"name":"Old","params":{"hpf_on":true,"hpf_freq":80}})");
    REQUIRE(older.has_value());
    CHECK(older->version == 0);
    CHECK(older->name == "Old");
    CHECK(older->params.at("hpf_on") == 1.0f);
    CHECK(older->params.at("hpf_freq") == 80.0f);
    CHECK(older->stages.empty());
    CHECK(older->author.isEmpty());

    // A stage-only (desktop) chain is a preset too.
    CHECK(fromJson(R"({"name":"Rack","stages":[{"effect":"eq","parameters":{"gain":1}}]})").has_value());
}

TEST_CASE("Bad files are refused and skipped when listing (FN2-5)", "[presets][fn2-5]")
{
    TempDir tmp;
    juce::File written;
    juce::String error;
    REQUIRE(save(sample(), tmp.dir, written, error));
    auto b = sample();
    b.name = "alpha";
    REQUIRE(save(b, tmp.dir, written, error));

    const auto put = [&](const char* name, const juce::String& text) {
        tmp.dir.getChildFile(juce::String(name) + kChainPresetExtension).replaceWithText(text);
    };
    put("garbage", "this is not json {");
    put("xml", "<KhensuCopilotPreset/>");
    put("array", "[1, 2, 3]");
    put("foreign", R"({"format":"someone.else","name":"x","params":{"a":1}})");
    put("empty", R"({"name":"Nothing","params":{}})");
    put("strings", R"({"name":"Strings","params":{"a":"1"}})");
    tmp.dir.getChildFile("notes.txt").replaceWithText("ignored");

    for (const char* bad : {"garbage", "xml", "array", "foreign", "empty", "strings"}) {
        INFO(bad);
        error.clear();
        CHECK_FALSE(loadPreset(tmp.dir.getChildFile(juce::String(bad) + kChainPresetExtension), &error).has_value());
        CHECK(error.isNotEmpty());
    }
    CHECK_FALSE(loadPreset(tmp.dir.getChildFile("missing.chain.json")).has_value());

    const auto listed = listPresets(tmp.dir);
    REQUIRE(listed.size() == 2); // sorted by name, case-insensitively
    CHECK(listed[0].preset.name == "alpha");
    CHECK(listed[1].preset.name == "Club master / v2");
    CHECK(listPresets(tmp.dir.getChildFile("does-not-exist")).empty());
}

TEST_CASE("A preset with no name takes its file's; file names are made legal (FN2-5)", "[presets][fn2-5]")
{
    TempDir tmp;
    const auto file = tmp.dir.getChildFile(juce::String("Unnamed bus") + kChainPresetExtension);
    file.replaceWithText(R"({"params":{"out_gain_db":1.5}})");
    const auto p = loadPreset(file);
    REQUIRE(p.has_value());
    CHECK(p->name == "Unnamed bus");
    CHECK(fileFor(tmp.dir, "a/b:c*d").getFileName().endsWith(kChainPresetExtension));
    CHECK_FALSE(fileFor(tmp.dir, "a/b:c*d").getFileName().containsAnyOf("/:*"));
    CHECK(fileFor(tmp.dir, "a/b:c*d").getParentDirectory() == tmp.dir);
}
