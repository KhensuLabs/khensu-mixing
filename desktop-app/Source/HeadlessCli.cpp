#include "HeadlessCli.h"
#include "BundledResources.h"
#include "HeadlessCliProcess.h"
#include "PythonServiceManager.h"
#include "analysis/FixVerification.h"
#include "session/FixAllSafe.h"
#include "session/RecommendationChain.h"
#include "session/RenderService.h"
#include "ui/shared/FindingEvidence.h"
#include <khensu/analysis/AnalysisInputs.h>
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/dsp/OfflineChainRenderer.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/targets/GenreDatabase.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace khensu::headless {

namespace {

constexpr const char* kAnalyzeFlag = "--analyze";
constexpr const char* kTargetsFlag = "--targets";
constexpr const char* kFixFlag = "--fix";
constexpr const char* kMasterFlag = "--master";
constexpr int kExitOk = 0;
constexpr int kExitThreshold = 1;
constexpr int kExitUsage = 2;
constexpr int kExitFailed = 3;
constexpr int kNoInterpreter = 127;
/// The Mix Notes / MixScorecard buckets on the 0..100 severity scale.
constexpr float kModerateSeverity = 33.0f;
constexpr float kSevereSeverity = 66.0f;
constexpr double kMasterCeilingDbtp = -1.0;
constexpr double kMaxMasterGainDb = 18.0;

void writeErr(const juce::String& message)
{
#if JUCE_WINDOWS
    writeConsoleError(message.toStdString());
#else
    std::fprintf(stderr, "%s\n", message.toRawUTF8());
    std::fflush(stderr);
#endif
}

void writeOut(const juce::String& text)
{
#if JUCE_WINDOWS
    writeConsoleOutput(text.toStdString());
#else
    std::fprintf(stdout, "%s\n", text.toRawUTF8());
    std::fflush(stdout);
#endif
}

bool isFlag(const juce::String& arg, const char* flag)
{
    return arg == flag || arg.startsWith(juce::String(flag) + "=");
}

bool hasFlag(const juce::StringArray& args, const char* flag)
{
    for (const auto& a : args)
        if (isFlag(a, flag)) return true;
    return false;
}

/// `--name=value` or `--name value`; nullopt when absent, "" when given without a value.
std::optional<juce::String> optionValue(const juce::StringArray& args, const juce::String& name)
{
    for (int i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a.startsWith(name + "=")) return a.fromFirstOccurrenceOf("=", false, false).unquoted();
        if (a == name) {
            if (i + 1 < args.size() && !args[i + 1].startsWith("--")) return args[i + 1].unquoted();
            return juce::String();
        }
    }
    return std::nullopt;
}

juce::File resolvePath(const juce::String& path)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile(path);
}

juce::var numberOrNull(double v)
{
    return std::isfinite(v) ? juce::var(std::round(v * 100.0) / 100.0) : juce::var();
}

} // namespace

Mode modeOf(const juce::StringArray& appArgs)
{
    if (appArgs.contains(kAnalyzeFlag)) return Mode::Analyze;
    if (appArgs.contains(kTargetsFlag)) return Mode::Targets;
    if (hasFlag(appArgs, kFixFlag)) return Mode::Fix;
    if (hasFlag(appArgs, kMasterFlag)) return Mode::Master;
    if (appArgs.contains("--help") || appArgs.contains("-h") || appArgs.contains("/?")) return Mode::Help;
    return Mode::None;
}

bool isHeadlessRequest(const juce::StringArray& appArgs)
{
    return modeOf(appArgs) != Mode::None;
}

juce::StringArray toCliArguments(const juce::StringArray& appArgs)
{
    const bool analyze = appArgs.contains(kAnalyzeFlag);
    juce::StringArray out;
    out.add(analyze ? "analyze" : "targets");
    bool modeFlagSkipped = false;
    for (const auto& arg : appArgs) {
        // Drop the first mode flag only; a later one is forwarded so the CLI
        // reports it rather than this layer silently swallowing it.
        if (!modeFlagSkipped && arg == (analyze ? kAnalyzeFlag : kTargetsFlag)) {
            modeFlagSkipped = true;
            continue;
        }
        out.add(arg);
    }
    return out;
}

juce::String quoteWindowsArgument(const juce::String& arg)
{
    if (arg.isNotEmpty() && !arg.containsAnyOf(" \t\n\v\""))
        return arg;

    juce::String quoted = "\"";
    int backslashes = 0;
    for (auto p = arg.getCharPointer(); !p.isEmpty(); ++p) {
        const juce::juce_wchar c = *p;
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // 2n+1 backslashes, then the literal quote.
            quoted += juce::String::repeatedString("\\", backslashes * 2 + 1) + "\"";
        } else {
            quoted += juce::String::repeatedString("\\", backslashes);
            quoted += juce::String::charToString(c);
        }
        backslashes = 0;
    }
    // Backslashes before the closing quote are doubled so they stay literal.
    quoted += juce::String::repeatedString("\\", backslashes * 2) + "\"";
    return quoted;
}

juce::String buildCommandLine(const juce::String& pythonExe, const juce::StringArray& cliArgs)
{
    juce::StringArray parts{quoteWindowsArgument(pythonExe), "-m", "khensu_services.cli"};
    for (const auto& a : cliArgs) parts.add(quoteWindowsArgument(a));
    return parts.joinIntoString(" ");
}

// --------------------------------------------------------------------------- parsing

NativeRequest parseNativeRequest(const juce::StringArray& appArgs)
{
    NativeRequest req;
    const Mode mode = modeOf(appArgs);
    if (mode != Mode::Fix && mode != Mode::Master) return req;
    req.mode = mode;
    const juce::String modeFlag = mode == Mode::Fix ? kFixFlag : kMasterFlag;
    const auto fail = [&req](const juce::String& why) {
        if (req.error.isEmpty()) req.error = why;
    };

    // Every argument must be one of ours (a typo is an error, not ignored).
    static const juce::StringArray valued{"--fix", "--master", "--out", "--genre", "--style", "--strength",
                                          "--target-lufs", "--max-iterations", "--fail-on"};
    static const juce::StringArray switches{"--top", "--include-denoise", "--native"};
    for (int i = 0; i < appArgs.size(); ++i) {
        const auto& a = appArgs[i];
        const auto name = a.upToFirstOccurrenceOf("=", false, false);
        if (valued.contains(name)) {
            if (!a.containsChar('=') && i + 1 < appArgs.size() && !appArgs[i + 1].startsWith("--")) ++i;
            continue;
        }
        if (switches.contains(a)) continue;
        fail("unknown argument '" + a + "' (see --help)");
    }
    if (mode == Mode::Fix && hasFlag(appArgs, kMasterFlag)) fail("give either --fix or --master, not both");

    req.input = optionValue(appArgs, modeFlag).value_or(juce::String());
    if (req.input.isEmpty()) fail(modeFlag + " needs an audio file (" + modeFlag + "=<file>)");
    req.output = optionValue(appArgs, "--out").value_or(juce::String());
    if (req.output.isEmpty() && req.input.isNotEmpty()) {
        const auto in = resolvePath(req.input);
        req.output = in.getSiblingFile(in.getFileNameWithoutExtension() + (mode == Mode::Fix ? "_fix" : "_master") +
                                       ".wav")
                         .getFullPathName();
    }
    req.genre = optionValue(appArgs, "--genre").value_or(juce::String());
    req.top = appArgs.contains("--top");
    req.includeDenoise = appArgs.contains("--include-denoise");
    req.forceNative = appArgs.contains("--native");

    if (const auto style = optionValue(appArgs, "--style")) {
        req.style = style->toLowerCase();
        if (!juce::StringArray{"warm", "balanced", "open", "loud"}.contains(req.style))
            fail("--style must be warm, balanced, open or loud");
    }
    if (const auto strength = optionValue(appArgs, "--strength")) {
        req.strength = strength->getDoubleValue();
        if (strength->isEmpty() || !strength->containsOnly("0123456789.") || req.strength < 0.0 || req.strength > 1.0)
            fail("--strength must be a number from 0 to 1");
    }
    if (const auto lufs = optionValue(appArgs, "--target-lufs")) {
        req.hasTargetLufs = true;
        req.targetLufs = lufs->getDoubleValue();
        if (lufs->isEmpty() || !lufs->containsOnly("-0123456789.") || req.targetLufs < -40.0 || req.targetLufs > -3.0)
            fail("--target-lufs must be from -40 to -3");
    }
    if (const auto iterations = optionValue(appArgs, "--max-iterations")) {
        req.maxIterations = iterations->getIntValue();
        if (!iterations->containsOnly("0123456789") || req.maxIterations < 1 || req.maxIterations > 20)
            fail("--max-iterations must be from 1 to 20");
    }
    if (const auto failOn = optionValue(appArgs, "--fail-on")) {
        req.failOn = *failOn;
        if (req.failOn != "moderate" && req.failOn != "severe") fail("--fail-on must be moderate or severe");
    }
    if (mode == Mode::Fix && (req.style.isNotEmpty() || req.strength >= 0.0 || req.maxIterations > 0))
        fail("--style / --strength / --max-iterations are --master options");
    if (mode == Mode::Master && (req.top || req.includeDenoise)) fail("--top / --include-denoise are --fix options");
    return req;
}

juce::StringArray toMasterCliArguments(const NativeRequest& req)
{
    juce::StringArray out{"master", req.input, "--out", req.output};
    if (req.genre.isNotEmpty()) out.addArray(juce::StringArray{"--genre", req.genre});
    if (req.style.isNotEmpty()) out.addArray(juce::StringArray{"--style", req.style});
    if (req.strength >= 0.0) out.addArray(juce::StringArray{"--strength", juce::String(req.strength, 3)});
    if (req.maxIterations > 0) out.addArray(juce::StringArray{"--max-iterations", juce::String(req.maxIterations)});
    if (req.failOn.isNotEmpty()) out.addArray(juce::StringArray{"--fail-on", req.failOn});
    out.addArray(juce::StringArray{"--json", "-"});
    return out;
}

juce::String usageText()
{
    return juce::StringArray{
        "Khensu Mixing - headless modes (no window opens)",
        "",
        "  \"Khensu Mixing.exe\" --analyze <files or folders> [--report out.html] [--target ID] [--json -]",
        "        [--fail-on moderate|severe] [--recursive]   (runs khensu_services.cli analyze; --analyze --help lists all)",
        "  \"Khensu Mixing.exe\" --targets [--json]          delivery targets",
        "",
        "  \"Khensu Mixing.exe\" --fix=<file> [--out=<file.wav>] [--top] [--include-denoise] [--genre=<id>]",
        "        [--fail-on=moderate|severe]",
        "      Native analysis, then one chain from every open finding whose fix is validated on the measured",
        "      corpus (advice-only findings and denoise are left out; --include-denoise adds the denoise,",
        "      --top keeps only the most severe fix), rendered by the native renderer under the guardrail,",
        "      then analysed again (resolved / improved / unchanged / worse / new). Needs no service.",
        "      Default output: <name>_fix.wav beside the input.",
        "",
        "  \"Khensu Mixing.exe\" --master=<file> [--out=<file.wav>] [--genre=<id>] [--style=warm|balanced|open|loud]",
        "        [--strength=0..1] [--max-iterations=N] [--target-lufs=-14] [--native] [--fail-on=...]",
        "      The mastering agent (render, measure, critique; styles and strength) through the analysis",
        "      runtime (`python -m khensu_services.cli master`) - it needs the Python runtime the app installs,",
        "      not a running service. Without that runtime, or with --native, a native loudness master: gain",
        "      to --target-lufs (default the genre's target, else -14 LUFS) into a -1 dBTP true-peak limiter.",
        "      Styles need the runtime. Default output: <name>_master.wav beside the input.",
        "",
        "  --fix and --master print a JSON report on stdout and progress on stderr.",
        "  Exit codes: 0 ok, 1 --fail-on reached (findings remain), 2 usage error, 3 analysis or render failed,",
        "  127 the analysis runtime was needed and not found.",
        "  More headless commands (deliver, album, batch): python -m khensu_services.cli --help",
    }
        .joinIntoString("\n");
}

// --------------------------------------------------------------------------- native fix

namespace {

struct Loaded {
    GenreEntry genre;
    bool genreGiven = false;
};

bool loadGenre(const juce::String& id, const NativeData& data, Loaded& out, juce::String& why)
{
    if (id.isEmpty()) return true;
    const auto file = data.genreTargets != juce::File()
                          ? data.genreTargets
                          : bundled_resources::find("resources/genre_norms/genre_targets.json");
    GenreDatabase db;
    if (!file.existsAsFile() || !db.loadFromFile(file.getFullPathName().toStdString()).is_ok()) {
        why = "the genre targets could not be read (" + file.getFullPathName() + ")";
        return false;
    }
    const auto genre = db.findById(id.toStdString());
    if (!genre.has_value()) {
        why = "unknown genre '" + id + "' (an id from genre_targets.json)";
        return false;
    }
    out.genre = *genre;
    out.genreGiven = true;
    return true;
}

fix_all_safe::Evidence loadEvidence(const NativeData& data)
{
    if (data.findingEvidence != juce::File()) {
        if (!data.findingEvidence.existsAsFile()) return {};
        return finding_evidence::parse(juce::JSON::parse(data.findingEvidence));
    }
    return finding_evidence::table();
}

/// The app's native pass with the engine down (OVH-9): the deep inputs marked
/// failed, so the fix check never claims what only the engine can measure.
std::optional<AnalysisResult> nativePass(const juce::File& file, const Loaded& genre, juce::String& error)
{
    AudioFileRef ref;
    ref.id = "headless"; // the same id on both sides: the same issue codes
    ref.path = file.getFullPathName().toStdString();
    ref.format = file.getFileExtension().trimCharactersAtStart(".").toLowerCase().toStdString();
    auto analysed = AudioAnalyzer{}.analyze(ref, "headless");
    if (!analysed.is_ok()) {
        error = "could not analyse " + file.getFullPathName() + ": " + juce::String(analysed.error());
        return std::nullopt;
    }
    auto r = analysed.value();
    r.file_id = ref.id;
    r.inputs_failed |= analysis_inputs::serviceInputs(false);
    CreativeTarget target;
    if (genre.genreGiven) target.primary_genre = genre.genre.id;
    r.recommendations = RecommendationEngine{}.generate(r, genre.genre, target);
    return r;
}

/// Same bar as fix_all_safe::whyNotSafe's evidence checks, for the denoise
/// path (which whyNotSafe always refuses: it needs listening).
juce::String evidenceWhyNot(const std::string& code, const fix_all_safe::Evidence& evidence)
{
    const auto it = evidence.find(juce::String(code));
    if (it == evidence.end()) return "its fix was never measured on real music";
    const auto& e = it->second;
    if (e.adviceOnly) return "advice only - the fix is made at the source";
    if (e.fixTried < fix_all_safe::kMinTried || e.fixClearsRate < 0.0) return "its fix was measured on too few tracks";
    if (e.fixClearsRate < fix_all_safe::kMinClearsRate)
        return "its fix cleared it on only " + juce::String(juce::roundToInt(e.fixClearsRate * 100.0)) + "% of tracks";
    return {};
}

bool hasDenoise(const fix_all_safe::Chain& chain)
{
    return std::any_of(chain.begin(), chain.end(),
                       [](const ipc::HttpServiceBridge::EffectSpec& e) { return e.effect == "SpectralDenoise"; });
}

struct Selection {
    std::vector<const Recommendation*> fixed;
    std::vector<std::pair<const Recommendation*, juce::String>> skipped;
    fix_all_safe::Chain chain;
};

/// The open findings, one per issue code (the most severe), most severe first.
std::vector<const Recommendation*> openFindings(const AnalysisResult& r)
{
    std::vector<const Recommendation*> out;
    for (const auto& [code, rec] : fix_verification::findingsByCode(r)) out.push_back(rec);
    std::stable_sort(out.begin(), out.end(),
                     [](const Recommendation* a, const Recommendation* b) { return a->severity > b->severity; });
    return out;
}

Selection selectFixes(const AnalysisResult& r, const fix_all_safe::Evidence& evidence, bool top, bool includeDenoise)
{
    Selection sel;
    std::vector<fix_all_safe::Chain> chains;
    for (const auto* rec : openFindings(r)) {
        const std::string code = issueCodeOf(*rec, r.file_id);
        auto chain = buildRecommendationChain(*rec);
        if (chain.empty()) {
            sel.skipped.emplace_back(rec, "advice only - no one-click fix");
            continue;
        }
        juce::String why;
        if (code == "noise_floor_high" || hasDenoise(chain)) {
            why = includeDenoise ? evidenceWhyNot(code, evidence)
                                 : juce::String("a denoise needs listening - pass --include-denoise");
        } else {
            IIssues::Issue issue;
            issue.recId = rec->id.empty() ? code : rec->id;
            issue.code = code;
            issue.rec = *rec;
            issue.renderable = true;
            why = fix_all_safe::whyNotSafe(issue, evidence);
        }
        if (why.isEmpty() && top && !sel.fixed.empty()) why = "--top: only the most severe fix";
        if (why.isNotEmpty()) {
            sel.skipped.emplace_back(rec, why);
            continue;
        }
        sel.fixed.push_back(rec);
        chains.push_back(std::move(chain));
    }
    sel.chain = fix_all_safe::mergeChains(chains);
    return sel;
}

juce::var findingVar(const Recommendation& rec, const std::string& fileId)
{
    auto* o = new juce::DynamicObject();
    o->setProperty("code", juce::String(issueCodeOf(rec, fileId)));
    o->setProperty("title", juce::String(rec.what));
    o->setProperty("severity", juce::roundToInt(rec.severity));
    return juce::var(o);
}

juce::var chainVar(const fix_all_safe::Chain& chain)
{
    juce::Array<juce::var> out;
    for (const auto& e : chain) {
        auto* stage = new juce::DynamicObject();
        auto* params = new juce::DynamicObject();
        for (const auto& [name, value] : e.parameters)
            params->setProperty(name, std::round(static_cast<double>(value) * 1000.0) / 1000.0);
        stage->setProperty("effect", e.effect);
        stage->setProperty("parameters", juce::var(params));
        out.add(juce::var(stage));
    }
    return out;
}

juce::var renderVar(const dsp::OfflineRenderResult& res)
{
    auto* o = new juce::DynamicObject();
    juce::StringArray applied, blocks, warnings;
    for (const auto& a : res.applied) applied.add(juce::String(a));
    for (const auto& b : res.blocks) blocks.add(juce::String(b));
    for (const auto& w : res.warnings) warnings.add(juce::String(w));
    o->setProperty("applied_effects", applied);
    o->setProperty("guardrail_ok", res.guardrailOk);
    o->setProperty("blocks", blocks);
    o->setProperty("warnings", warnings);
    o->setProperty("lufs_before", numberOrNull(res.before.integratedLufs));
    o->setProperty("lufs_after", numberOrNull(res.after.integratedLufs));
    o->setProperty("true_peak_before_dbtp", numberOrNull(res.before.truePeakDbtp));
    o->setProperty("true_peak_after_dbtp", numberOrNull(res.after.truePeakDbtp));
    return juce::var(o);
}

bool reaches(const std::vector<const Recommendation*>& open, const juce::String& failOn)
{
    if (failOn.isEmpty()) return false;
    const float floor = failOn == "severe" ? kSevereSeverity : kModerateSeverity;
    return std::any_of(open.begin(), open.end(), [floor](const Recommendation* r) { return r->severity >= floor; });
}

/// The JSON report and its exit code, filled as the run goes.
struct Report {
    juce::DynamicObject::Ptr root{new juce::DynamicObject()};
    juce::String& json;
    explicit Report(juce::String& out, const juce::String& tool) : json(out)
    {
        root->setProperty("tool", tool);
        root->setProperty("status", "ok");
    }
    void set(const juce::Identifier& key, const juce::var& value) { root->setProperty(key, value); }
    int finish(int code)
    {
        root->setProperty("exit_code", code);
        json = juce::JSON::toString(juce::var(root.get()));
        return code;
    }
    int fail(int code, const juce::String& why)
    {
        root->setProperty("status", "error");
        root->setProperty("error", why);
        writeErr("Khensu Mixing: " + why);
        return finish(code);
    }
};

/// Input / output checks shared by --fix and --master.
bool checkPaths(const NativeRequest& req, Report& report, juce::File& in, juce::File& out, int& code)
{
    if (req.error.isNotEmpty()) {
        code = report.fail(kExitUsage, req.error);
        return false;
    }
    in = resolvePath(req.input);
    out = resolvePath(req.output);
    report.set("input", in.getFullPathName());
    report.set("output", out.getFullPathName());
    if (!in.existsAsFile()) {
        code = report.fail(kExitUsage, "no such file: " + in.getFullPathName());
        return false;
    }
    if (out == in || out.getFullPathName().equalsIgnoreCase(in.getFullPathName())) {
        code = report.fail(kExitUsage, "--out must not be the input file (the original is never overwritten)");
        return false;
    }
    if (!out.hasFileExtension("wav")) {
        code = report.fail(kExitUsage, "--out must be a .wav file");
        return false;
    }
    if (!out.getParentDirectory().createDirectory()) {
        code = report.fail(kExitFailed, "could not create " + out.getParentDirectory().getFullPathName());
        return false;
    }
    return true;
}

} // namespace

int runNativeFix(const NativeRequest& req, juce::String& json, const NativeData& data)
{
    Report report(json, "Khensu Mixing --fix");
    report.set("engine", "native");
    juce::File in, out;
    int code = kExitOk;
    if (!checkPaths(req, report, in, out, code)) return code;

    Loaded genre;
    juce::String why;
    if (!loadGenre(req.genre, data, genre, why)) return report.fail(kExitUsage, why);
    report.set("genre", genre.genreGiven ? juce::var(juce::String(genre.genre.id)) : juce::var());
    const auto evidence = loadEvidence(data);

    writeErr("analysing " + in.getFullPathName());
    const auto before = nativePass(in, genre, why);
    if (!before) return report.fail(kExitFailed, why);

    const auto sel = selectFixes(*before, evidence, req.top, req.includeDenoise);
    juce::Array<juce::var> fixed, skipped, beforeFindings;
    for (const auto* rec : sel.fixed) fixed.add(findingVar(*rec, before->file_id));
    for (const auto& [rec, reason] : sel.skipped) {
        auto v = findingVar(*rec, before->file_id);
        v.getDynamicObject()->setProperty("reason", reason);
        skipped.add(v);
    }
    const auto openBefore = openFindings(*before);
    for (const auto* rec : openBefore) beforeFindings.add(findingVar(*rec, before->file_id));
    report.set("fixed", fixed);
    report.set("skipped", skipped);
    report.set("chain", chainVar(sel.chain));
    report.set("findings_before", beforeFindings);

    if (sel.chain.empty()) {
        report.set("output", juce::var());
        report.set("note", "nothing to fix: no open finding has a validated one-click fix");
        report.set("findings_after", beforeFindings);
        return report.finish(reaches(openBefore, req.failOn) ? kExitThreshold : kExitOk);
    }

    writeErr("rendering " + juce::String(static_cast<int>(sel.chain.size())) + " stage(s) for " +
             juce::String(static_cast<int>(sel.fixed.size())) + " finding(s)");
    const auto rendered = dsp::OfflineChainRenderer::renderFile(in, out, RenderService::nativeChain(sel.chain));
    report.set("render", renderVar(rendered));
    if (rendered.status != dsp::OfflineRenderResult::Status::Rendered) {
        report.set("output", juce::var());
        juce::StringArray engineOnly;
        for (const auto& e : rendered.needsEngine) engineOnly.add(juce::String(e));
        const juce::String reason = rendered.status == dsp::OfflineRenderResult::Status::NeedsEngine
                                        ? "the chain needs the analysis engine: " + engineOnly.joinIntoString(", ")
                                        : juce::String(rendered.error);
        return report.fail(kExitFailed, "render failed: " + reason);
    }

    writeErr("checking the fix");
    const auto after = nativePass(out, genre, why);
    if (!after) return report.fail(kExitFailed, why);
    const auto check = fix_verification::compare(*before, *after, out.getFullPathName().toStdString());
    juce::Array<juce::var> checks, afterFindings;
    for (const auto& f : check.findings) {
        auto* o = new juce::DynamicObject();
        o->setProperty("code", juce::String(f.code));
        o->setProperty("title", juce::String(f.title));
        o->setProperty("outcome", juce::String(fix_verification::outcomeLabel(f.outcome)));
        o->setProperty("severity_before", juce::roundToInt(f.severityBefore));
        o->setProperty("severity_after", juce::roundToInt(f.severityAfter));
        checks.add(juce::var(o));
    }
    const auto openAfter = openFindings(*after);
    for (const auto* rec : openAfter) afterFindings.add(findingVar(*rec, after->file_id));
    report.set("check", checks);
    report.set("check_summary", juce::String(fix_verification::summary(check)));
    report.set("findings_after", afterFindings);
    writeErr(juce::String(fix_verification::summary(check)));
    return report.finish(reaches(openAfter, req.failOn) ? kExitThreshold : kExitOk);
}

int runNativeMaster(const NativeRequest& req, juce::String& json, const NativeData& data)
{
    Report report(json, "Khensu Mixing --master");
    report.set("engine", "native");
    juce::File in, out;
    int code = kExitOk;
    if (!checkPaths(req, report, in, out, code)) return code;
    if (req.style.isNotEmpty() && req.style != "balanced")
        return report.fail(kExitUsage, "the mastering styles need the analysis runtime (drop --native / --style)");

    Loaded genre;
    juce::String why;
    if (!loadGenre(req.genre, data, genre, why)) return report.fail(kExitUsage, why);
    const double target = req.hasTargetLufs  ? req.targetLufs
                          : genre.genreGiven ? static_cast<double>(genre.genre.lufs_target)
                                             : -14.0;
    report.set("target_lufs", target);
    report.set("ceiling_dbtp", kMasterCeilingDbtp);

    writeErr("analysing " + in.getFullPathName());
    const auto before = nativePass(in, genre, why);
    if (!before) return report.fail(kExitFailed, why);
    const double lufsBefore = before->loudness.lufs_integrated;
    if (!std::isfinite(lufsBefore) || lufsBefore <= -70.0)
        return report.fail(kExitFailed, "the input is silent or too quiet to measure");

    const auto chainFor = [](double gainDb) {
        std::vector<dsp::ChainStageSpec> chain(2);
        chain[0].effect = "Gain";
        chain[0].parameters["gain_db"] = static_cast<float>(gainDb);
        chain[1].effect = "Limiter";
        chain[1].parameters["threshold_db"] = static_cast<float>(kMasterCeilingDbtp);
        return chain;
    };
    double gain = std::clamp(target - lufsBefore, -24.0, kMaxMasterGainDb);
    writeErr("rendering the master");
    auto rendered = dsp::OfflineChainRenderer::renderFile(in, out, chainFor(gain));
    int passes = 1;
    // The limiter costs loudness on a dense mix: one more pass with the shortfall added.
    if (rendered.status == dsp::OfflineRenderResult::Status::Rendered && std::isfinite(rendered.after.integratedLufs) &&
        rendered.after.integratedLufs < target - 0.5 && gain < kMaxMasterGainDb) {
        gain = std::min(kMaxMasterGainDb, gain + (target - rendered.after.integratedLufs));
        rendered = dsp::OfflineChainRenderer::renderFile(in, out, chainFor(gain));
        passes = 2;
    }
    report.set("render", renderVar(rendered));
    report.set("gain_db", numberOrNull(gain));
    report.set("passes", passes);
    if (rendered.status != dsp::OfflineRenderResult::Status::Rendered) {
        report.set("output", juce::var());
        return report.fail(kExitFailed, "render failed: " + juce::String(rendered.error));
    }
    const double miss = std::abs(rendered.after.integratedLufs - target);
    report.set("loudness_miss_lu", numberOrNull(miss));
    writeErr("master: " + juce::String(rendered.after.integratedLufs, 1) + " LUFS, " +
             juce::String(rendered.after.truePeakDbtp, 1) + " dBTP");
    const bool reached = req.failOn == "severe" ? miss > 3.0 : req.failOn == "moderate" ? miss > 1.0 : false;
    return report.finish(reached ? kExitThreshold : kExitOk);
}

// --------------------------------------------------------------------------- run

namespace {

/// Runs `python -m khensu_services.cli <cliArgs>`; kNoInterpreter (after
/// saying why, when `reportMissing`) without an interpreter / service dir.
int runPythonCli(const juce::StringArray& cliArgs, bool reportMissing)
{
    juce::String pythonExe;
    juce::File servicesDir;
    {
        // Only used for its interpreter / launcher lookup - never launched,
        // so its destructor sends nothing to a running service.
        PythonServiceManager locator;
        pythonExe = locator.resolvedPythonExe();
        const juce::File launcher = locator.resolvedLauncherScript();
        if (launcher.existsAsFile()) servicesDir = launcher.getParentDirectory();
    }
    if (pythonExe.isEmpty() || servicesDir == juce::File()) {
        if (reportMissing)
            writeErr("Khensu Mixing: headless mode needs the analysis service: " +
                     juce::String(pythonExe.isEmpty() ? "no Python interpreter found"
                                                      : "local-services/launch_service.py not found") +
                     ". Run the app once normally to set it up.");
        return kNoInterpreter;
    }

#if JUCE_WINDOWS
    const juce::String cmd = buildCommandLine(pythonExe, cliArgs);
    const int code = runWithConsole(cmd.toWideCharPointer(), servicesDir.getFullPathName().toWideCharPointer());
    if (code < 0) {
        writeErr("Khensu Mixing: could not start " + pythonExe);
        return kNoInterpreter;
    }
    return code;
#else
    // Unix-like: no console attach problem - stream the child's output through.
    const juce::String existing = juce::SystemStats::getEnvironmentVariable("PYTHONPATH", {});
    setenv("PYTHONPATH",
           (servicesDir.getFullPathName() + (existing.isNotEmpty() ? ":" + existing : juce::String())).toRawUTF8(),
           1);
    juce::StringArray argv{pythonExe, "-m", "khensu_services.cli"};
    argv.addArray(cliArgs);
    juce::ChildProcess child;
    if (!child.start(argv, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
        writeErr("Khensu Mixing: could not start " + pythonExe);
        return kNoInterpreter;
    }
    char buffer[4096];
    for (;;) {
        const int n = child.readProcessOutput(buffer, sizeof(buffer));
        if (n <= 0) break;
        std::fwrite(buffer, 1, static_cast<size_t>(n), stdout);
    }
    std::fflush(stdout);
    child.waitForProcessToFinish(-1);
    return static_cast<int>(child.getExitCode());
#endif
}

} // namespace

int run(const juce::StringArray& appArgs)
{
    switch (modeOf(appArgs)) {
    case Mode::Help:
        writeOut(usageText());
        return kExitOk;
    case Mode::Fix: {
        juce::String json;
        const int code = runNativeFix(parseNativeRequest(appArgs), json);
        writeOut(json);
        return code;
    }
    case Mode::Master: {
        const auto req = parseNativeRequest(appArgs);
        if (req.error.isEmpty() && !req.forceNative) {
            const int code = runPythonCli(toMasterCliArguments(req), false);
            if (code != kNoInterpreter) return code;
            writeErr("Khensu Mixing: the analysis runtime was not found - making a native loudness master instead.");
        }
        juce::String json;
        const int code = runNativeMaster(req, json);
        writeOut(json);
        return code;
    }
    case Mode::Analyze:
    case Mode::Targets:
        return runPythonCli(toCliArguments(appArgs), true);
    case Mode::None:
        break;
    }
    return kExitUsage;
}

} // namespace khensu::headless
