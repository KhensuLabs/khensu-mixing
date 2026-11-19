// tests/ui/HeadlessCliTest.cpp
// `"Khensu Mixing.exe" --analyze ...` forwards to `python -m khensu_services.cli`:
// the mode detection, argument mapping and Windows command-line quoting must
// hand the CLI exactly the arguments the user typed.
#include <catch2/catch_test_macros.hpp>
#include "HeadlessCli.h"

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
