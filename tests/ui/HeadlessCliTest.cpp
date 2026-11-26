// tests/ui/HeadlessCliTest.cpp
// `"Khensu Mixing.exe" --analyze ...` forwards to `python -m khensu_services.cli`:
// the mode detection, argument mapping and Windows command-line quoting must
// hand the CLI exactly the arguments the user typed. ENH-3: --fix / --master
// run natively (analysis, validated fixes, native renderer, fix check).
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "HeadlessCli.h"
#include "support/CleanTestMix.h"

using namespace khensu;

TEST_CASE("Headless CLI: only --analyze / --targets select headless mode", "[headless]")
{
    CHECK(headless::isHeadlessRequest(juce::StringArray{"--analyze", "a.wav"}));
    CHECK(headless::isHeadlessRequest(juce::StringArray{"--targets"}));
    CHECK_FALSE(headless::isHeadlessRequest(juce::StringArray{}));
    CHECK_FALSE(headless::isHeadlessRequest(juce::StringArray{"--a11y-audit=out.txt"}));
    CHECK_FALSE(headless::isHeadlessRequest(juce::StringArray{"song.wav"}));
}

TEST_CASE("Headless CLI: app arguments map to CLI arguments in order", "[headless]")
{
    const juce::StringArray app{"--analyze", "C:/mixes/a b.wav", "--report", "out.html", "--fail-on", "severe"};
    const auto cli = headless::toCliArguments(app);
    CHECK(cli.joinIntoString("|") == "analyze|C:/mixes/a b.wav|--report|out.html|--fail-on|severe");

    // Options before the mode flag are kept too.
    CHECK(headless::toCliArguments(juce::StringArray{"--json", "-", "--analyze", "x.wav"}).joinIntoString("|") ==
          "analyze|--json|-|x.wav");
    CHECK(headless::toCliArguments(juce::StringArray{"--targets", "--json"}).joinIntoString("|") ==
          "targets|--json");
}

TEST_CASE("Headless CLI: Windows quoting survives CommandLineToArgvW", "[headless]")
{
    CHECK(headless::quoteWindowsArgument("plain.wav") == "plain.wav");
    CHECK(headless::quoteWindowsArgument("") == "\"\"");
    CHECK(headless::quoteWindowsArgument("a b.wav") == "\"a b.wav\"");
    // Backslashes are literal unless they precede a quote.
    CHECK(headless::quoteWindowsArgument("C:\\My Mixes\\") == "\"C:\\My Mixes\\\\\"");
    CHECK(headless::quoteWindowsArgument("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(headless::quoteWindowsArgument("x\\\"y") == "\"x\\\\\\\"y\"");
    CHECK(headless::quoteWindowsArgument("C:\\dir\\file.wav") == "C:\\dir\\file.wav");
}

TEST_CASE("Headless CLI: the interpreter runs the khensu_services.cli module", "[headless]")
{
    const auto cmd = headless::buildCommandLine("C:\\Program Files\\Khensu\\runtime\\python.exe",
                                                juce::StringArray{"analyze", "my mix.wav"});
    CHECK(cmd == "\"C:\\Program Files\\Khensu\\runtime\\python.exe\" -m khensu_services.cli analyze \"my mix.wav\"");
}

// ---------------------------------------------------------------------------
// ENH-3: --fix / --master / --help
// ---------------------------------------------------------------------------
namespace {

namespace mix = khensu::test::clean_mix;

juce::File repoFile(const char* relative)
{
    return juce::File(juce::String(__FILE__))
        .getParentDirectory()
        .getParentDirectory()
        .getParentDirectory()
        .getChildFile(relative);
}

headless::NativeData sourceTreeData()
{
    return {repoFile("resources/genre_norms/genre_targets.json"),
            repoFile("resources/calibration/finding_evidence.json")};
}

juce::File writeStereo(const mix::Stereo& y, const juce::String& name)
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    file.deleteFile();
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    REQUIRE(stream != nullptr);
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(static_cast<double>(mix::kRate))
                             .withNumChannels(2)
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = fmt.createWriterFor(stream, options);
    REQUIRE(writer != nullptr);
    const float* chans[2] = {y[0].data(), y[1].data()};
    REQUIRE(writer->writeFromFloatArrays(chans, 2, static_cast<int>(y[0].size())));
    return file;
}

} // namespace

TEST_CASE("Headless CLI: --fix / --master / --help select headless mode", "[headless][enh3]")
{
    CHECK(headless::modeOf(juce::StringArray{"--fix=a.wav"}) == headless::Mode::Fix);
    CHECK(headless::modeOf(juce::StringArray{"--fix", "a.wav", "--out", "b.wav"}) == headless::Mode::Fix);
    CHECK(headless::modeOf(juce::StringArray{"--master=a.wav", "--style=warm"}) == headless::Mode::Master);
    CHECK(headless::modeOf(juce::StringArray{"--help"}) == headless::Mode::Help);
    CHECK(headless::modeOf(juce::StringArray{"--analyze", "a.wav", "--help"}) == headless::Mode::Analyze);
    CHECK(headless::isHeadlessRequest(juce::StringArray{"--master", "a.wav"}));
    CHECK_FALSE(headless::isHeadlessRequest(juce::StringArray{"--fixture"}));
    CHECK(headless::usageText().contains("--fix=<file>"));
    CHECK(headless::usageText().contains("--master=<file>"));
}

TEST_CASE("Headless CLI: --fix / --master options parse in both spellings", "[headless][enh3]")
{
    const auto fix = headless::parseNativeRequest(juce::StringArray{
        "--fix=\"C:/mixes/a b.wav\"", "--out", "C:/out/fixed.wav", "--top", "--include-denoise", "--genre=garage",
        "--fail-on=severe"});
    CHECK(fix.error.isEmpty());
    CHECK(fix.mode == headless::Mode::Fix);
    CHECK(fix.input == "C:/mixes/a b.wav");
    CHECK(fix.output == "C:/out/fixed.wav");
    CHECK(fix.top);
    CHECK(fix.includeDenoise);
    CHECK(fix.genre == "garage");
    CHECK(fix.failOn == "severe");

    // The default output sits beside the input.
    const auto def = headless::parseNativeRequest(juce::StringArray{"--fix", "C:/mixes/song.flac"});
    CHECK(def.error.isEmpty());
    CHECK(juce::File(def.output).getFileName() == "song_fix.wav");

    const auto master = headless::parseNativeRequest(
        juce::StringArray{"--master=C:/m/a.wav", "--out=C:/m/a_m.wav", "--style", "Warm", "--strength=0.6",
                          "--max-iterations", "3", "--target-lufs", "-11"});
    CHECK(master.error.isEmpty());
    CHECK(master.style == "warm");
    CHECK(master.strength == Catch::Approx(0.6));
    CHECK(master.maxIterations == 3);
    CHECK(master.hasTargetLufs);
    CHECK(master.targetLufs == Catch::Approx(-11.0));
    CHECK(headless::toMasterCliArguments(master).joinIntoString("|") ==
          "master|C:/m/a.wav|--out|C:/m/a_m.wav|--style|warm|--strength|0.600|--max-iterations|3|--json|-");

    // Usage problems are reported, never ignored.
    CHECK(headless::parseNativeRequest(juce::StringArray{"--fix"}).error.isNotEmpty());
    CHECK(headless::parseNativeRequest(juce::StringArray{"--fix=a.wav", "--bogus"}).error.contains("--bogus"));
    CHECK(headless::parseNativeRequest(juce::StringArray{"--master=a.wav", "--style=loudest"}).error.isNotEmpty());
    CHECK(headless::parseNativeRequest(juce::StringArray{"--master=a.wav", "--strength=2"}).error.isNotEmpty());
    CHECK(headless::parseNativeRequest(juce::StringArray{"--fix=a.wav", "--style=warm"}).error.isNotEmpty());
    CHECK(headless::parseNativeRequest(juce::StringArray{"--master=a.wav", "--top"}).error.isNotEmpty());
    CHECK(headless::parseNativeRequest(juce::StringArray{"--fix=a.wav", "--fail-on=always"}).error.isNotEmpty());
}

TEST_CASE("Headless CLI: --fix renders the validated fixes natively and checks them", "[headless][enh3][render][e2e]")
{
    auto y = mix::cleanMix();
    mix::shelf(y, false, 120.0, 6.0); // +6 dB low shelf: low_end_heavy against garage (OVH-9)
    const auto defect = writeStereo(y, "khensu_enh3_defect.wav");
    const auto fixed = defect.getSiblingFile("khensu_enh3_defect_fixed.wav");
    fixed.deleteFile();

    auto req = headless::parseNativeRequest(juce::StringArray{"--fix=" + defect.getFullPathName(),
                                                              "--out=" + fixed.getFullPathName(), "--genre=garage"});
    REQUIRE(req.error.isEmpty());
    juce::String json;
    const int code = headless::runNativeFix(req, json, sourceTreeData());
    INFO(json);
    REQUIRE(code == 0);
    const auto report = juce::JSON::parse(json);
    CHECK(report["status"].toString() == "ok");
    CHECK(report["engine"].toString() == "native");
    CHECK(fixed.existsAsFile());
    CHECK(static_cast<bool>(report["render"]["guardrail_ok"]));

    bool lowEndFixed = false;
    for (const auto& f : *report["fixed"].getArray()) lowEndFixed |= f["code"].toString() == "low_end_heavy";
    CHECK(lowEndFixed);
    // Every finding left out says why (advice only, not validated, denoise, ...).
    for (const auto& s : *report["skipped"].getArray()) CHECK(s["reason"].toString().isNotEmpty());
    bool resolved = false;
    for (const auto& c : *report["check"].getArray())
        resolved |= c["code"].toString() == "low_end_heavy" && c["outcome"].toString() == "Resolved";
    CHECK(resolved);

    // --top keeps exactly one finding.
    req.top = true;
    REQUIRE(headless::runNativeFix(req, json, sourceTreeData()) == 0);
    CHECK(juce::JSON::parse(json)["fixed"].getArray()->size() == 1);

    // The original is never the output; a missing input or genre is a usage error.
    auto onto = req;
    onto.output = defect.getFullPathName();
    CHECK(headless::runNativeFix(onto, json, sourceTreeData()) == 2);
    auto missing = req;
    missing.input = defect.getSiblingFile("khensu_enh3_missing.wav").getFullPathName();
    CHECK(headless::runNativeFix(missing, json, sourceTreeData()) == 2);
    auto badGenre = req;
    badGenre.genre = "no-such-genre";
    CHECK(headless::runNativeFix(badGenre, json, sourceTreeData()) == 2);

    defect.deleteFile();
    fixed.deleteFile();
}

TEST_CASE("Headless CLI: the native master lands on the loudness target under the ceiling",
          "[headless][enh3][render][e2e]")
{
    const auto src = writeStereo(mix::cleanMix(), "khensu_enh3_master_src.wav");
    const auto out = src.getSiblingFile("khensu_enh3_master_out.wav");
    out.deleteFile();
    auto req = headless::parseNativeRequest(juce::StringArray{"--master=" + src.getFullPathName(),
                                                              "--out=" + out.getFullPathName(), "--native",
                                                              "--target-lufs=-12", "--fail-on=moderate"});
    REQUIRE(req.error.isEmpty());
    REQUIRE(req.forceNative);
    juce::String json;
    const int code = headless::runNativeMaster(req, json, sourceTreeData());
    INFO(json);
    CHECK(code == 0);
    const auto report = juce::JSON::parse(json);
    REQUIRE(out.existsAsFile());
    CHECK(static_cast<double>(report["render"]["lufs_after"]) == Catch::Approx(-12.0).margin(1.0));
    CHECK(static_cast<double>(report["render"]["true_peak_after_dbtp"]) <= -0.9);

    // Styles need the mastering agent.
    req.style = "warm";
    CHECK(headless::runNativeMaster(req, json, sourceTreeData()) == 2);
    src.deleteFile();
    out.deleteFile();
}
