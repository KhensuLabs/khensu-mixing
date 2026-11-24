// diagnostics/AppDiagnostics.cpp (ENG-1 step 8; was AppController_UiBench.cpp)
// The whole-app diagnostics, run inside the real app against IDiagnosticsHost.
//
// `--ui-bench=<file>`: a message-thread benchmark, the performance sibling of
// `--a11y-audit`. Unit tests can't build the real screens (most need a live
// controller), so this runs inside the real app:
//   1. analyses a generated 30 s mix with the in-process fast analyzers and
//      publishes it, so every Dashboard panel has data to draw;
//   2. visits every screen: first-visit (lazy build) time, then per-frame
//      layout and paint cost (paint = the screen's paint code rendered into
//      a software image, so GPU readback isn't timed), worst frame per screen;
//   3. times the Dashboard update pass (one analysis broadcast);
//   4. switches sessions repeatedly (resetSessionScopedState + republish)
//      and reports working-set growth.
// Writes JSON to <file>; exit code 0 = within budget, 1 = over.
// Budgets and baseline: docs/archive/2026-10-17-performance-baseline.md.
//
// `--a11y-audit`, `--screenshot` and `--screenshot-audio`: see AppDiagnostics.h.
#include "AppDiagnostics.h"
#include "../ui/shared/TimelineCache.h"
#include "../ui/Dashboard/CardGroup.h"
#include "../ui/shared/AccessibilityCoverage.h"
#include "../ui/shared/KhensuLookAndFeel.h"
#include "../ui/shared/KhensuTheme.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/AudioAnalyzer.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <random>
#include <typeinfo>
#include <vector>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace khensu::diagnostics {
namespace {

// Generous against the 2026-10-17 baseline (i7-11700K, 1240x798 content area):
// worst frame 41 ms (Export), worst mean 24-31 ms (Export), first visit
// <= 1.5 ms (screens are built at startup), Dashboard broadcast ~0.6 ms,
// 0 MiB growth. Since the shared glass backdrop (same day) Export is ~6.5 ms
// mean / <= 10 ms worst; the slowest screens are Intake and System Brain
// (~9-10 ms mean, occasional ~19 ms worst).
constexpr double kWorstFrameBudgetMs = 150.0; // any one screen paint
constexpr double kMeanFrameBudgetMs = 100.0;  // per-screen mean paint
constexpr double kFirstVisitBudgetMs = 250.0; // showScreen() incl. lazy content
constexpr double kDashboardUpdateBudgetMs = 50.0;
constexpr double kSessionSwitchGrowthBudgetMiB = 64.0;
// The soak: private bytes may grow this much from cycle 2 to the last.
constexpr double kSoakGrowthBudgetMiB = 150.0;

double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

double workingSetMiB()
{
#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
#endif
    return 0.0;
}

/// 30 s stereo mix (chords, kick, noise, slow swell) - enough structure for
/// every fast analyzer to produce real numbers.
juce::File writeFixtureMix()
{
    auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_ui_bench_fixture.wav");
    file.deleteFile();
    constexpr int sr = 48000;
    constexpr int n = sr * 30;
    juce::AudioBuffer<float> buf(2, n);
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    const double roots[] = {110.0, 87.31, 130.81, 98.0};
    double phase = 0.0;
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / sr;
        phase += juce::MathConstants<double>::twoPi * roots[static_cast<int>(t / 2.0) % 4] / sr;
        const double tb = std::fmod(t, 0.5);
        const double kick =
            tb < 0.12 ? 0.6 * std::sin(juce::MathConstants<double>::twoPi * 60.0 * tb) * std::exp(-tb * 25) : 0.0;
        const double swell = 0.8 + 0.2 * std::sin(juce::MathConstants<double>::twoPi * t / 60.0);
        const double l = 0.15 * std::sin(phase) + 0.08 * std::sin(1.5 * phase) + kick + 0.01 * noise(rng);
        const double r = 0.15 * std::sin(phase + 0.3) + 0.08 * std::sin(1.5 * phase) + kick + 0.01 * noise(rng);
        buf.setSample(0, i, static_cast<float>(juce::jlimit(-0.99, 0.99, l * swell * 0.7)));
        buf.setSample(1, i, static_cast<float>(juce::jlimit(-0.99, 0.99, r * swell * 0.7)));
    }
    juce::WavAudioFormat fmt;
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr) return {};
    auto writer = fmt.createWriterFor(
        stream, juce::AudioFormatWriterOptions{}.withSampleRate(sr).withNumChannels(2).withBitsPerSample(24));
    if (writer == nullptr) return {};
    writer->writeFromAudioSampleBuffer(buf, 0, n);
    return file;
}

/// Paints `c` (and its children) into a software ARGB image: the paint code's
/// own CPU cost. createComponentSnapshot() would use the native image type
/// (Direct2D in JUCE 8), which adds a ~86 ms GPU readback per call even for
/// an empty component and drowns the signal.
void paintToSoftwareImage(juce::Component& c)
{
    juce::Image img(juce::Image::ARGB, juce::jmax(1, c.getWidth()), juce::jmax(1, c.getHeight()), true,
                    juce::SoftwareImageType());
    juce::Graphics g(img);
    c.paintEntireComponent(g, true);
}

struct FrameStats {
    double mean = 0, p95 = 0, worst = 0, worstLayout = 0;
};

FrameStats measureFrames(juce::Component& c, int frames)
{
    std::vector<double> paint, layout;
    const auto base = c.getBounds();
    paintToSoftwareImage(c); // warm glyph/path caches
    for (int i = 0; i < frames; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        c.setBounds(i % 2 == 0 ? base : base.withSizeKeepingCentre(base.getWidth() - 160, base.getHeight() - 100));
        layout.push_back(msSince(t0));
        t0 = std::chrono::steady_clock::now();
        paintToSoftwareImage(c);
        paint.push_back(msSince(t0));
    }
    c.setBounds(base);
    std::sort(paint.begin(), paint.end());
    FrameStats s;
    s.mean = std::accumulate(paint.begin(), paint.end(), 0.0) / static_cast<double>(paint.size());
    s.p95 = paint[std::min(paint.size() - 1, paint.size() * 95 / 100)];
    s.worst = paint.back();
    s.worstLayout = *std::max_element(layout.begin(), layout.end());
    return s;
}

juce::var round2(double v) { return std::round(v * 100.0) / 100.0; }

} // namespace

int writeAccessibilityAudit(IDiagnosticsHost& host, const juce::File& report)
{
    juce::StringArray lines;
    int gaps = 0;
    auto auditOne = [&](const juce::String& name, juce::Component& root) {
        AccessibilityCoverage::apply(root);
        const auto found = AccessibilityCoverage::audit(root);
        lines.add("[" + name + "] " + juce::String(static_cast<int>(found.size())) + " gap(s)");
        for (const auto& g : found)
            lines.add("    " + g.describe());
        gaps += static_cast<int>(found.size());
    };
    for (const auto& s : host.diagnosticScreens()) {
        host.showScreen(s.type); // builds lazily-populated content, as a user visit would
        if (s.component != nullptr) auditOne(s.name, *s.component);
    }
    auditOne("Sidebar", host.sidebarComponent());
    auditOne("Transport", host.transportComponent());
    lines.insert(0, "Khensu accessibility audit - " + juce::String(gaps) + " total gap(s)");
    report.getParentDirectory().createDirectory();
    report.replaceWithText(lines.joinIntoString("\n") + "\n");
    return gaps;
}

int writeUiBenchmark(IDiagnosticsHost& host, const juce::File& report)
{
    auto* root = new juce::DynamicObject();
    juce::Array<juce::var> overBudget;
    auto& window = host.window();
    auto& content = host.contentArea();

    // 1. Fixture analysis (fast C++ analyzers; deep/service-only fields stay
    //    empty, which the doc notes) published like a finished analysis.
    AnalysisResult fixture;
    const auto wav = writeFixtureMix();
    {
        AudioFileRef ref;
        ref.id = "ui-bench";
        ref.path = wav.getFullPathName().toStdString();
        const auto t0 = std::chrono::steady_clock::now();
        auto result = AudioAnalyzer{}.analyze(ref, "ui-bench");
        root->setProperty("fixture_fast_analysis_ms", round2(msSince(t0)));
        if (result.is_ok()) fixture = result.value();
    }
    wav.deleteFile();
    root->setProperty("window", juce::String(window.getWidth()) + "x" + juce::String(window.getHeight()));

    // 2. Every screen.
    juce::Array<juce::var> screenRows;
    double worstFrame = 0.0;
    // The screen as launched, before this bench switches anything: a launch
    // fade that targeted pre-layout bounds once left Intake at 0x0 - an empty
    // page with no buttons. That is a failure, not something to work around.
    for (auto* c : content.getChildren())
        if (c->isVisible() && c->getBounds() != content.getLocalBounds())
            overBudget.add("launch screen not laid out (" + c->getBounds().toString() + ")");
    for (const auto& e : host.diagnosticScreens()) {
        if (e.component == nullptr) continue;
        auto t0 = std::chrono::steady_clock::now();
        host.showScreen(e.type);
        const double firstVisit = msSince(t0);
        const auto area = content.getLocalBounds();
        const auto got = e.component->getBounds();
        const bool laidOut = e.drawer ? (got.getRight() == area.getRight() && got.getY() == area.getY() &&
                                         got.getHeight() == area.getHeight() &&
                                         got.getWidth() >= juce::jmin(420, area.getWidth()))
                                      : got == area;
        if (!laidOut) overBudget.add(juce::String(e.name) + " not laid out (" + got.toString() + ")");
        if (e.type == ScreenType::Dashboard)
            host.publishAnalysisForDiagnostics(fixture); // once it is visible, so its panels exist
        const auto f = measureFrames(*e.component, 20);
        worstFrame = std::max(worstFrame, f.worst);
        auto* row = new juce::DynamicObject();
        row->setProperty("screen", e.name);
        row->setProperty("first_visit_ms", round2(firstVisit));
        row->setProperty("size", juce::String(e.component->getWidth()) + "x" + juce::String(e.component->getHeight()));
        row->setProperty("paint_mean_ms", round2(f.mean));
        row->setProperty("paint_p95_ms", round2(f.p95));
        row->setProperty("paint_worst_ms", round2(f.worst));
        row->setProperty("layout_worst_ms", round2(f.worstLayout));
        if (f.worst > kWorstFrameBudgetMs || f.mean > kMeanFrameBudgetMs || firstVisit > kFirstVisitBudgetMs)
            overBudget.add(juce::String(e.name));
        screenRows.add(juce::var(row));
    }
    root->setProperty("screens", screenRows);
    root->setProperty("worst_frame_ms", round2(worstFrame));

    // 2b. Glass backdrop rebuild: what one window-size change costs (the
    //     app-wide blurred backdrop every PanelCard copies its region from is
    //     rebuilt once per size; a 1 px wider size defeats the cache).
    {
        auto rebuild = [](juce::Rectangle<int> app) {
            const auto t0 = std::chrono::steady_clock::now();
            const auto img =
                KhensuLookAndFeel::glassBackdrop(app.withWidth(app.getWidth() + 1), GlassPanelVariant::Default);
            juce::ignoreUnused(img);
            return msSince(t0);
        };
        root->setProperty("glass_backdrop_rebuild_ms", round2(rebuild(window.getLocalBounds())));
        root->setProperty("glass_backdrop_rebuild_4k_ms", round2(rebuild({0, 0, 3840, 2160})));
    }

    // 3. Dashboard update pass: one analysis broadcast reaching every panel.
    host.showScreen(ScreenType::Dashboard);
    {
        std::vector<double> passes;
        for (int i = 0; i < 10; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            host.publishAnalysisForDiagnostics(fixture);
            passes.push_back(msSince(t0));
        }
        const double worst = *std::max_element(passes.begin(), passes.end());
        root->setProperty("dashboard_update_mean_ms",
                          round2(std::accumulate(passes.begin(), passes.end(), 0.0) / passes.size()));
        root->setProperty("dashboard_update_worst_ms", round2(worst));
        if (worst > kDashboardUpdateBudgetMs) overBudget.add("dashboard_update");
    }

    // 4. Session switches: working-set growth after warm-up.
    {
        constexpr int warmup = 5, switches = 50;
        double afterWarmup = 0.0;
        for (int i = 0; i < warmup + switches; ++i) {
            host.resetSessionScopedState();
            host.publishAnalysisForDiagnostics(fixture);
            if (i == warmup - 1) afterWarmup = workingSetMiB();
        }
        const double end = workingSetMiB();
        root->setProperty("session_switches", switches);
        root->setProperty("working_set_after_warmup_mib", round2(afterWarmup));
        root->setProperty("working_set_end_mib", round2(end));
        root->setProperty("working_set_growth_mib", round2(end - afterWarmup));
        if (end - afterWarmup > kSessionSwitchGrowthBudgetMiB) overBudget.add("session_switch_memory");
    }

    root->setProperty("over_budget", overBudget);
    report.getParentDirectory().createDirectory();
    report.replaceWithText(juce::JSON::toString(juce::var(root)));
    return overBudget.size();
}

namespace {

/// --exercise-all: after the analysis, stems and fix check, press every
/// analysis button on every screen, card tab, Mix / Master view and Export
/// step, ask the Copilot a question and play the song, so the full pass shows
/// every feature's result rather than its "press to run" state.
bool gExerciseAll = false;

bool isExerciseButton(const juce::String& text)
{
    static const char* const kRun[] = {
        "Analyze Arrangement", "Analyze Collisions", "Analyze Groove", "Analyze M/S", "Analyze Pan Balance",
        "Analyze Phase", "Analyze Source", "Analyze Stem Energy", "Analyze Stems", "Check Mono", "Check Stem Phase",
        "Compute Gain Ride", "Detect Notes", "Detect Tuning", "Find Mono Culprits", "Find Words", "Find similar",
        "Map Depth", "Map Harmony", "Measure Tail", "Scan Signal Integrity", "Scan Source Quality",
        "Scan for AI / Artifacts", "Scan for AI Artifacts", "X-Ray Current Render", "Suggest", "Master it",
        "Run Mastering Agent", "Run verified auto-mix", "Generate Chain", "Mapping Report"};
    for (const auto* r : kRun)
        if (text == r) return true;
    return false;
}

void collectButtons(juce::Component& c, std::vector<juce::Button*>& out)
{
    for (auto* child : c.getChildren()) {
        if (auto* b = dynamic_cast<juce::TextButton*>(child)) out.push_back(b);
        collectButtons(*child, out);
    }
}

void collectEditors(juce::Component& c, std::vector<juce::TextEditor*>& out)
{
    for (auto* child : c.getChildren()) {
        if (auto* e = dynamic_cast<juce::TextEditor*>(child)) out.push_back(e);
        collectEditors(*child, out);
    }
}

using PressedList = std::vector<std::pair<juce::String, juce::Component::SafePointer<juce::Button>>>;

/// Collects the run buttons under `root` (not yet pressed) in tree order.
void pressIn(juce::Component& root, const juce::String& where, PressedList& found, juce::String& log)
{
    std::vector<juce::Button*> buttons;
    collectButtons(root, buttons);
    for (auto* b : buttons) {
        const auto text = b->getButtonText();
        if (!isExerciseButton(text)) continue;
        const bool seen = std::any_of(found.begin(), found.end(),
                                      [b](const auto& p) { return p.second.getComponent() == b; });
        if (seen) continue;
        log << "  exercise: found " << where << " / " << text << (b->isEnabled() ? "" : " (disabled now)") << "\n";
        found.push_back({where + " / " + text, juce::Component::SafePointer<juce::Button>(b)});
    }
}

PressedList exerciseEverything(IDiagnosticsHost& host, juce::String& log)
{
    PressedList pressed;
    for (const auto& sc : host.diagnosticScreens()) {
        host.showScreen(sc.type);
        host.finishScreenTransition();
        pressIn(host.contentArea(), sc.shotName, pressed, log);
        if (sc.type == ScreenType::Copilot) {
            // Ask the copilot something real about this mix.
            std::vector<juce::TextEditor*> editors;
            collectEditors(host.contentArea(), editors);
            for (auto* e : editors)
                if (e->isShowing() && !e->isReadOnly() && e->isEnabled()) {
                    e->setText("What is the biggest problem with this mix, and how do I fix it?", true); // i18n-exempt(diagnostic prompt)
                    std::vector<juce::Button*> buttons;
                    collectButtons(host.contentArea(), buttons);
                    for (auto* b : buttons)
                        if (b->getButtonText() == "Send" && b->isEnabled()) {
                            log << "  exercise: copilot / Send\n";
                            b->triggerClick();
                            break;
                        }
                    break;
                }
        }
    }
    host.showScreen(ScreenType::Dashboard);
    for (auto& [groupId, card] : host.dashboardCardsForDiagnostics()) {
        if (auto* group = dynamic_cast<CardGroup*>(card)) {
            for (const auto& m : group->members()) {
                group->selectMember(m.id);
                pressIn(*group, "card " + groupId + "/" + juce::String(m.id), pressed, log);
            }
        } else if (card != nullptr) {
            pressIn(*card, "card " + groupId, pressed, log);
        }
    }
    for (int v = 0, n = 1; v < n; ++v) {
        n = host.showMixMasterViewForDiagnostics(v);
        pressIn(host.contentArea(), "mix-master view " + juce::String(v), pressed, log);
    }
    host.showMixMasterViewForDiagnostics(0);
    for (int step = 0, n = 1; step < n; ++step) {
        n = host.showExportStepForDiagnostics(step);
        pressIn(host.contentArea(), "export step " + juce::String(step), pressed, log);
    }
    host.showExportStepForDiagnostics(0);
    host.showScreen(ScreenType::Dashboard);
    return pressed;
}

/// One line per component: depth, class, title/name, bounds, visibility.
void dumpTree(juce::Component& c, int depth, juce::String& out)
{
    const juce::String name = c.getTitle().isNotEmpty() ? c.getTitle() : c.getName();
    // Bounds in the parent, then "@" the bounds in the window (what a
    // screenshot's pixels are measured in).
    const auto* top = c.getTopLevelComponent();
    const auto abs = top != nullptr && top != &c ? top->getLocalArea(&c, c.getLocalBounds()) : c.getBounds();
    out << juce::String::repeatedString("  ", depth) << typeid(c).name() << " \"" << name << "\" "
        << c.getBounds().toString() << " @" << abs.toString() << (c.isVisible() ? "" : " [hidden]")
        << (c.isShowing() ? "" : " [not showing]") << "\n";
    if (depth < 9)
        for (auto* child : c.getChildren())
            dumpTree(*child, depth + 1, out);
}

/// UX2-7: --render-scale=1.5|2 paints every screenshot at that display
/// scale (the same logical layout, f times the pixels - what a 150 % / 200 %
/// Windows display does), so a stretched bitmap or a hairline that snaps
/// badly shows up without a high-DPI monitor.
float gRenderScale = 1.0f;

bool writePng(juce::Component& c, const juce::File& file)
{
    const float s = gRenderScale;
    juce::Image img(juce::Image::ARGB, juce::jmax(1, juce::roundToInt((float)c.getWidth() * s)),
                    juce::jmax(1, juce::roundToInt((float)c.getHeight() * s)), true, juce::SoftwareImageType());
    {
        juce::Graphics g(img);
        g.addTransform(juce::AffineTransform::scale(s));
        c.paintEntireComponent(g, true);
    }
    file.deleteFile();
    juce::FileOutputStream out(file);
    juce::PNGImageFormat png;
    return out.openedOk() && png.writeImageToStream(img, out);
}

/// The screenshot run's copy of the app log (juce::Logger): the service
/// manager and health monitor write there, so a run that loses the engine
/// says why (RUN-14).
std::unique_ptr<juce::FileLogger> gRunLog;

void quitWith(int code)
{
    if (gRunLog != nullptr) {
        juce::Logger::setCurrentLogger(nullptr);
        gRunLog.reset();
    }
    juce::JUCEApplicationBase::getInstance()->setApplicationReturnValue(code);
    juce::JUCEApplicationBase::quit();
}

} // namespace

int writeScreenshots(IDiagnosticsHost& host, const juce::File& dir)
{
    dir.createDirectory();
    auto& window = host.window();
    int written = 0;
    // As launched: no showScreen() call, so a launch-time layout bug shows up.
    if (writePng(window, dir.getChildFile("00-launch.png"))) ++written;
    juce::String tree;
    dumpTree(window, 0, tree);
    dir.getChildFile("00-launch-tree.txt").replaceWithText(tree);

    for (const auto& s : host.diagnosticScreens()) {
        host.showScreen(s.type);
        host.finishScreenTransition(); // end the fade-in (alpha starts at 0)
        if (writePng(window, dir.getChildFile(juce::String(s.shotName) + ".png"))) ++written;
    }
    host.showScreen(ScreenType::Intake);
    tree.clear();
    dumpTree(window, 0, tree);
    dir.getChildFile("01-intake-tree.txt").replaceWithText(tree);
    return written;
}

void runAnalysisScreenshots(IDiagnosticsHost& host, const juce::File& dir, const juce::File& audio)
{
    juce::Component::SafePointer<juce::Component> safe(&host.window());
    auto* hostPtr = &host;
    // Without the Python service only the fast C++ pass runs and the gate is
    // trivially short - wait (up to 3 min) for the first healthy check.
    if (!host.serviceSeenHealthy()) {
        static int waited = 0;
        if (waited++ < 360) {
            juce::Timer::callAfterDelay(500, [safe, hostPtr, dir, audio] {
                if (safe.getComponent() != nullptr) runAnalysisScreenshots(*hostPtr, dir, audio);
            });
            return;
        }
    }
    auto log = std::make_shared<juce::String>();
    if (!host.startDiagnosticSession(audio)) {
        dir.getChildFile("analysis-gate-log.txt").replaceWithText("could not start a session\n");
        quitWith(1);
        return;
    }
    host.showScreen(ScreenType::Dashboard);
    const auto started = juce::Time::getMillisecondCounter();

    auto capture = [hostPtr, dir](const juce::String& name) {
        hostPtr->finishScreenTransition();
        writePng(hostPtr->window(), dir.getChildFile(name + ".png"));
        // The component tree too, so a stray element on a results screen can
        // be found by its bounds (as 00-launch-tree.txt does for the launch).
        juce::String tree;
        dumpTree(hostPtr->window(), 0, tree);
        dir.getChildFile(name + "-tree.txt").replaceWithText(tree);
    };
    auto finish = [hostPtr, dir, log](int code) {
        dir.getChildFile("analysis-gate-log.txt").replaceWithText(*log);
        hostPtr->deleteDiagnosticSession([code] { quitWith(code); });
    };

    // Poll every 500 ms: log the gate, capture once while withheld and once
    // released; also prove the overlay covers the Dashboard whenever withheld.
    auto poll = std::make_shared<std::function<void()>>();
    auto shotPending = std::make_shared<bool>(false);
    auto shotChecked = std::make_shared<bool>(false);
    auto intakeStage = std::make_shared<int>(0); // 0 not started, 1 dropped
    auto intakeAt = std::make_shared<double>(0.0);
    auto exitCode = std::make_shared<int>(0);
    auto fixResult = std::make_shared<int>(0); // 0 pending, 1 rendered, 2 nothing to fix, -1 failed
    auto shotReleased = std::make_shared<bool>(false);
    auto releasedAt = std::make_shared<double>(0.0);
    auto openSeenAt = std::make_shared<double>(-1.0); // VIZ-F3: when the results first opened
    auto exStage = std::make_shared<int>(gExerciseAll ? 0 : 3); // 0 start, 1 running, 2 playing, 3 done
    auto exAt = std::make_shared<double>(0.0);
    auto exPressed = std::make_shared<PressedList>();
    auto exIndex = std::make_shared<int>(-1); // the feature running now
    auto exPressAt = std::make_shared<double>(0.0);
    auto exSkipped = std::make_shared<PressedList>(); // disabled on the first pass: tried once more at the end
    auto exRetried = std::make_shared<bool>(false);
    *poll = [hostPtr, safe, poll, log, capture, finish, dir, started, shotPending, shotReleased, releasedAt, shotChecked, fixResult, intakeStage, intakeAt, exitCode, audio, openSeenAt, exStage, exAt, exPressed, exIndex, exPressAt, exSkipped, exRetried] {
        if (safe.getComponent() == nullptr) return;
        const auto t = (juce::Time::getMillisecondCounter() - started) / 1000.0;
        const auto g = hostPtr->gateSnapshot();
        *log << juce::String(t, 1) << "s pending=" << (g.pending ? 1 : 0) << " overlay=" << (g.overlayCovers ? 1 : 0)
             << " gate=" << g.gate << " pendingScans=" << g.pendingScans << " inFlight=" << (g.inFlight ? 1 : 0)
             << " hasResult=" << (g.hasResult ? 1 : 0) << " deep=" << (g.deep ? 1 : 0)
             << " serviceUp=" << (hostPtr->serviceSeenHealthy() ? 1 : 0)
             << " engine=" << hostPtr->serviceStatusForDiagnostics() << "\n";
        if (g.pending && !g.overlayCovers) {
            *log << "FAIL: results withheld but the Dashboard is not covered\n";
            finish(1);
            return;
        }
        if (g.ended) {
            capture("21-dashboard-ended");
            *log << "FAIL: analysis ended without results (gate " << g.gate << ")\n";
            finish(1);
            return;
        }
        if (g.pending && !*shotPending && t > 1.5) {
            capture("20-dashboard-while-analyzing");
            *shotPending = true;
        }
        // VIZ-F3: the results screens are compared image by image, so they are
        // captured once the song timeline (measured in the background) is in
        // - never half-filled - or after 20 s at the most.
        if (!g.pending && g.open && *openSeenAt < 0.0) *openSeenAt = t;
        // In for two polls in a row: the card has had a turn to draw it.
        static int timelinePolls = 0;
        timelinePolls = timeline_cache::find(audio.getFullPathName()) != nullptr ? timelinePolls + 1 : 0;
        if (!g.pending && g.open && !*shotReleased && (timelinePolls >= 2 || t - *openSeenAt > 20.0)) {
            capture("21-dashboard-results");
            // CON-2..8: each group card, one image per tab.
            for (auto& [groupId, card] : hostPtr->dashboardCardsForDiagnostics())
                if (auto* group = dynamic_cast<CardGroup*>(card))
                    for (const auto& m : group->members()) {
                        group->selectMember(m.id);
                        writePng(*group, dir.getChildFile("21b-card-" + groupId + "-" + juce::String(m.id) + ".png"));
                    }
            // CON-1: the issue queue with the same results, then back.
            hostPtr->showScreen(ScreenType::Issues);
            capture("22-issues-results");
            hostPtr->showScreen(ScreenType::MixMaster); // CON-5's Versions card with a session
            capture("22b-mix-master-results");
            // CON-2 / FEAT-6 / FEAT-2: each Export step with a session.
            for (int step = 0, n = 1; step < n; ++step) {
                n = hostPtr->showExportStepForDiagnostics(step);
                if (n > 0) capture("22c-export-" + juce::String(step));
            }
            hostPtr->showExportStepForDiagnostics(0);
            hostPtr->showScreen(ScreenType::Dashboard);
            // WF-1 + FEAT-1 end to end: fix the top issue from the queue,
            // then wait for the check of the fixed version.
            hostPtr->fixTopIssueForDiagnostics([log, fixResult](bool ok, juce::String message) {
                // Nothing with a one-click fix (a clean track, or only fixes
                // that need listening, RUN-6) is a pass, as in the soak.
                const bool nothing = !ok && message.containsIgnoreCase("no open issue");
                *fixResult = ok ? 1 : (nothing ? 2 : -1);
                *log << "fix top issue: " << (ok ? "rendered " : nothing ? "nothing to fix - " : "FAILED ") << message
                     << "\n";
            });
            *log << "released after " << juce::String(t, 1) << "s\n";
            *shotReleased = true;
            *releasedAt = t;
        }
        // After release: stem separation starts; wait for it and log how it
        // ended (up to 10 more minutes).
        if (*shotReleased) {
            *log << "  stems running=" << (g.stemsRunning ? 1 : 0) << " available=" << (g.stemsAvailable ? 1 : 0)
                 << " msg=" << g.stemMessage << " render=" << juce::File(g.currentRender).getFileName()
                 << " verifying=" << (g.verifying ? 1 : 0) << " check=" << g.verification << "\n";
            // The fix check (FEAT-1) has to land before the run ends.
            const bool fixSettled =
                *fixResult < 0 || *fixResult == 2 || (*fixResult == 1 && g.verification.isNotEmpty());
            if (fixSettled && !*shotChecked) {
                *shotChecked = true;
                if (*fixResult == 1) {
                    *log << "fix check: " << g.verification << "\n";
                    hostPtr->showScreen(ScreenType::Issues);
                    capture("23-issues-after-fix");
                    hostPtr->showScreen(ScreenType::Dashboard);
                }
            }
            if (!fixSettled && t - *releasedAt > 600.0) {
                *log << "FAIL: the fix check did not finish within 10 minutes\n";
                finish(1);
                return;
            }
            if (fixSettled && !g.stemsRunning && (g.stemMessage.isNotEmpty() || t - *releasedAt > 5.0)) {
                if (*intakeStage == 0 && *exStage < 3) {
                    // --exercise-all: run every feature in turn, then play the song
                    // so the full pass captures the meters live.
                    if (*exStage == 0) {
                        *exPressed = exerciseEverything(*hostPtr, *log);
                        *log << "exercise: " << (int)exPressed->size() << " features to run, one at a time, from "
                             << juce::String(t, 1) << "s\n";
                        *exStage = 1;
                        *exAt = t;
                        *exIndex = -1;
                    } else if (*exStage == 1) {
                        // One at a time, as a person would: press the next once the
                        // current one has finished (its button enabled again, 3 s at
                        // the least) or after 6 minutes.
                        auto* cur = *exIndex >= 0 && *exIndex < (int)exPressed->size()
                                        ? (*exPressed)[(size_t)*exIndex].second.getComponent()
                                        : nullptr;
                        const double ran = t - *exPressAt;
                        const bool done = *exIndex < 0 || cur == nullptr || (cur->isEnabled() && ran > 3.0) || ran > 360.0;
                        if (done) {
                            if (*exIndex >= 0)
                                *log << "  exercise: " << (*exPressed)[(size_t)*exIndex].first << " finished in "
                                     << juce::String(ran, 1) << "s" << (ran > 360.0 ? " (TIMED OUT)" : "") << "\n";
                            // The next one that can run now.
                            for (++*exIndex; *exIndex < (int)exPressed->size(); ++*exIndex) {
                                auto* b = (*exPressed)[(size_t)*exIndex].second.getComponent();
                                if (b != nullptr && b->isEnabled()) {
                                    *log << "  exercise: pressing " << (*exPressed)[(size_t)*exIndex].first << "\n";
                                    b->triggerClick();
                                    *exPressAt = t;
                                    break;
                                }
                                *log << "  exercise: skipped " << (*exPressed)[(size_t)*exIndex].first
                                     << " (disabled)\n";
                                if (!*exRetried) exSkipped->push_back((*exPressed)[(size_t)*exIndex]);
                            }
                            if (*exIndex >= (int)exPressed->size() && !*exRetried && !exSkipped->empty()) {
                                *log << "exercise: retrying " << (int)exSkipped->size() << " that were disabled\n";
                                *exPressed = *exSkipped;
                                *exRetried = true;
                                *exIndex = -1;
                            } else if (*exIndex >= (int)exPressed->size()) {
                                *log << "exercise: all done after " << juce::String(t - *exAt, 1) << "s\n";
                                hostPtr->playForDiagnostics(60.0, true);
                                // The scopes draw on vblank: show Mix / Master
                                // while it plays, so the capture has a trace.
                                hostPtr->showScreen(ScreenType::MixMaster);
                                *exStage = 2;
                                *exAt = t;
                            }
                        }
                    } else if (t - *exAt > 6.0) {
                        *exStage = 3; // playing for 6 s: the full pass captures with the meters live
                    }
                } else if (*intakeStage == 0) {
                    *log << "stems: " << (g.stemsAvailable ? "available" : "NOT available") << " - "
                         << g.stemMessage << "\n";
                    // The full pass: every screen, every card tab, each Mix /
                    // Master view and Export step, after the analysis, the
                    // stems and the fix check have all finished (30-39).
                    for (const auto& sc : hostPtr->diagnosticScreens()) {
                        hostPtr->showScreen(sc.type);
                        hostPtr->finishScreenTransition();
                        capture("30-full-" + juce::String(sc.shotName));
                    }
                    hostPtr->showScreen(ScreenType::Dashboard);
                    for (auto& [groupId, card] : hostPtr->dashboardCardsForDiagnostics())
                        if (auto* group = dynamic_cast<CardGroup*>(card))
                            for (const auto& m : group->members()) {
                                group->selectMember(m.id);
                                writePng(*group, dir.getChildFile("31-card-" + groupId + "-" + juce::String(m.id) + ".png"));
                            }
                    for (int v = 0, n = 1; v < n; ++v) {
                        n = hostPtr->showMixMasterViewForDiagnostics(v);
                        hostPtr->finishScreenTransition();
                        if (n > 0) capture("32-mixmaster-" + juce::String(v));
                    }
                    hostPtr->showMixMasterViewForDiagnostics(0);
                    for (int step = 0, n = 1; step < n; ++step) {
                        n = hostPtr->showExportStepForDiagnostics(step);
                        if (n > 0) capture("33-export-" + juce::String(step));
                    }
                    hostPtr->showExportStepForDiagnostics(0);
                    // A fix that did not render fails the run too (VER-3): its log
                    // line used to be the only trace, with exit code 0.
                    *exitCode = (g.stemsAvailable && *fixResult > 0) ? 0 : 1;
                    if (gExerciseAll) hostPtr->playForDiagnostics(0.0, false);
                    // UX-12: Intake after a drop - detected context, Confirm, Create Session.
                    hostPtr->showScreen(ScreenType::Intake);
                    hostPtr->dropIntoIntakeForDiagnostics(audio);
                    *intakeStage = 1;
                    *intakeAt = t;
                } else if (hostPtr->intakeReadyForDiagnostics() || t - *intakeAt > 120.0) {
                    const bool ready = hostPtr->intakeReadyForDiagnostics();
                    capture("24-intake-after-drop");
                    *log << "intake after drop: " << (ready ? "Create Session ready" : "FAIL: not ready in 120 s")
                         << " after " << juce::String(t - *intakeAt, 1) << "s\n";
                    finish(ready ? *exitCode : 1);
                    return;
                }
            }
            if (t - *releasedAt > 600.0) {
                *log << "FAIL: stems still running 10 minutes after release\n";
                finish(1);
                return;
            }
        }
        if (!*shotReleased && t > 600.0) {
            *log << "FAIL: still withheld after 10 minutes\n";
            finish(1);
            return;
        }
        juce::Timer::callAfterDelay(500, *poll);
    };
    juce::Timer::callAfterDelay(500, *poll);
}

namespace {

struct ProcessCounters {
    double workingSetMiB = 0.0;
    double privateMiB = 0.0;
    unsigned long handles = 0;
    unsigned long gdiObjects = 0;
    unsigned long userObjects = 0;
};

ProcessCounters processCounters()
{
    ProcessCounters c;
#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        c.workingSetMiB = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        c.privateMiB = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) c.handles = handles;
    c.gdiObjects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    c.userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
#endif
    return c;
}

/// One soak run's state, owned by the poll closure.
struct SoakState {
    juce::File audio, log;
    double minutes = 30.0;
    juce::uint32 started = 0;
    juce::uint32 cycleStarted = 0;
    int cycle = 0;
    int failures = 0;
    enum class Step { Start, WaitResults, WaitFix, Deleting } step = Step::Start;
    int fixResult = 0; // 0 pending, 1 rendered, 2 nothing to fix, -1 failed
    juce::String fixMessage;
    std::vector<ProcessCounters> afterCycle;
    juce::String text;
    juce::String current; ///< the file of the running cycle
    juce::Array<juce::File> files; ///< a folder's audio, listed once

    void line(const juce::String& s)
    {
        text << s << "\n";
        log.replaceWithText(text);
    }
};

} // namespace

void runSoak(IDiagnosticsHost& host, const juce::File& audio, double minutes, const juce::File& log)
{
    juce::Component::SafePointer<juce::Component> safe(&host.window());
    auto* hostPtr = &host;
    if (!host.serviceSeenHealthy()) { // wait (up to 3 min) for the engine, as the screenshot run does
        static int waited = 0;
        if (waited++ < 360) {
            juce::Timer::callAfterDelay(500, [safe, hostPtr, audio, minutes, log] {
                if (safe.getComponent() != nullptr) runSoak(*hostPtr, audio, minutes, log);
            });
            return;
        }
    }
    auto st = std::make_shared<SoakState>();
    st->audio = audio;
    st->log = log;
    st->minutes = minutes;
    st->started = juce::Time::getMillisecondCounter();
    st->line("soak: " + audio.getFullPathName() + " for " + juce::String(juce::roundToInt(minutes)) + " min");
    st->line("cycle seconds working_set_mib private_mib handles gdi user result file");

    auto poll = std::make_shared<std::function<void()>>();
    *poll = [hostPtr, safe, poll, st] {
        if (safe.getComponent() == nullptr) return;
        const auto now = juce::Time::getMillisecondCounter();
        const double elapsedMin = (now - st->started) / 60000.0;
        const double cycleSec = (now - st->cycleStarted) / 1000.0;
        auto next = [poll] { juce::Timer::callAfterDelay(500, [poll] { (*poll)(); }); };

        switch (st->step) {
        case SoakState::Step::Start: {
            if (elapsedMin >= st->minutes) {
                // Growth: the last cycle against the second (the first warms caches).
                int code = st->failures > 0 ? 1 : 0;
                if (st->afterCycle.size() >= 3) {
                    const auto& a = st->afterCycle[1];
                    const auto& b = st->afterCycle.back();
                    const double grew = b.privateMiB - a.privateMiB;
                    const long handlesGrew = static_cast<long>(b.handles) - static_cast<long>(a.handles);
                    const long gdiGrew = static_cast<long>(b.gdiObjects) - static_cast<long>(a.gdiObjects);
                    const long userGrew = static_cast<long>(b.userObjects) - static_cast<long>(a.userObjects);
                    st->line("growth from cycle 2 to " + juce::String(static_cast<int>(st->afterCycle.size())) + ": private " +
                             juce::String(grew, 1) + " MiB, handles " + juce::String(handlesGrew) + ", gdi " +
                             juce::String(gdiGrew) + ", user " + juce::String(userGrew));
                    if (grew > kSoakGrowthBudgetMiB || handlesGrew > 500 || gdiGrew > 500 || userGrew > 500) {
                        st->line("FAIL: growth over budget (" + juce::String(juce::roundToInt(kSoakGrowthBudgetMiB)) +
                                 " MiB / 500 handles or objects)");
                        code = 1;
                    }
                }
                st->line(juce::String(st->cycle) + " cycles, " + juce::String(st->failures) + " failed");
                quitWith(code);
                return;
            }
            ++st->cycle;
            st->cycleStarted = now;
            st->fixResult = 0;
            // A folder: a different file each cycle (the same file again is
            // answered from the stored analysis and the service's caches).
            // The list is taken once: the fix renders land beside their source
            // and must not become inputs.
            juce::File audio = st->audio;
            if (st->audio.isDirectory()) {
                if (st->files.isEmpty()) {
                    for (const auto& f : st->audio.findChildFiles(juce::File::findFiles, false, "*.wav;*.flac"))
                        if (!f.getFileName().startsWith(".")) st->files.add(f); // not a .quarantine_ render
                    st->files.sort();
                }
                if (st->files.isEmpty()) {
                    st->line("FAIL: no audio files in " + st->audio.getFullPathName());
                    quitWith(1);
                    return;
                }
                audio = st->files[(st->cycle - 1) % st->files.size()];
            }
            st->current = audio.getFileName();
            if (!hostPtr->startDiagnosticSession(audio)) {
                st->line("FAIL: cycle " + juce::String(st->cycle) + " could not start a session");
                quitWith(1);
                return;
            }
            hostPtr->showScreen(ScreenType::Dashboard);
            st->step = SoakState::Step::WaitResults;
            break;
        }
        case SoakState::Step::WaitResults: {
            const auto g = hostPtr->gateSnapshot();
            if (g.ended) {
                ++st->failures;
                st->line("cycle " + juce::String(st->cycle) + ": analysis ended without results");
                st->step = SoakState::Step::Deleting;
                hostPtr->deleteDiagnosticSession([st] { st->step = SoakState::Step::Start; });
                break;
            }
            if (!g.pending && g.open) {
                // Every screen with results, as a user browsing them would.
                for (auto& screen : hostPtr->diagnosticScreens()) {
                    hostPtr->showScreen(screen.type);
                    hostPtr->finishScreenTransition();
                }
                hostPtr->showScreen(ScreenType::Dashboard);
                hostPtr->fixTopIssueForDiagnostics([st](bool ok, juce::String message) {
                    // A clean track has nothing with a one-click fix: a pass, not a failure.
                    const bool nothing = !ok && message.containsIgnoreCase("no open issue");
                    st->fixResult = ok ? 1 : (nothing ? 2 : -1);
                    st->fixMessage = message;
                });
                st->step = SoakState::Step::WaitFix;
            } else if (cycleSec > 600.0) {
                ++st->failures;
                st->line("cycle " + juce::String(st->cycle) + ": no results within 10 minutes");
                st->step = SoakState::Step::Deleting;
                hostPtr->deleteDiagnosticSession([st] { st->step = SoakState::Step::Start; });
            }
            break;
        }
        case SoakState::Step::WaitFix: {
            const auto g = hostPtr->gateSnapshot();
            const bool settled = st->fixResult < 0 || st->fixResult == 2 ||
                                 (st->fixResult == 1 && g.verification.isNotEmpty());
            if (settled || cycleSec > 900.0) {
                const bool ok = st->fixResult > 0 && settled;
                if (!ok) ++st->failures;
                st->step = SoakState::Step::Deleting;
                hostPtr->deleteDiagnosticSession([st, cycleSec, ok] {
                    const auto c = processCounters();
                    st->afterCycle.push_back(c);
                    st->line(juce::String(st->cycle) + " " + juce::String(cycleSec, 1) + " " +
                             juce::String(c.workingSetMiB, 1) + " " + juce::String(c.privateMiB, 1) + " " +
                             juce::String(static_cast<int>(c.handles)) + " " +
                             juce::String(static_cast<int>(c.gdiObjects)) + " " +
                             juce::String(static_cast<int>(c.userObjects)) + " " + (ok ? (st->fixResult == 2 ? "ok (nothing to fix)" : "ok") : "FAILED fix: " + st->fixMessage) +
                             " " + st->current);
                    st->step = SoakState::Step::Start;
                });
            }
            break;
        }
        case SoakState::Step::Deleting:
            break; // the delete callback moves on
        }
        next();
    };
    (*poll)();
}

bool scheduleFromCommandLine(IDiagnosticsHost& host)
{
    juce::Component::SafePointer<juce::Component> safe(&host.window());
    auto* hostPtr = &host;
    const auto args = juce::JUCEApplicationBase::getCommandLineParameterArray();
    auto valueOf = [](const juce::String& arg) {
        return juce::File(arg.fromFirstOccurrenceOf("=", false, false).unquoted());
    };
    // --theme=light|high-contrast|dark: switch the palette before any run, so
    // --screenshot can capture each theme (the theme is not persisted).
    for (const auto& arg : args) {
        if (!arg.startsWith("--theme=")) continue;
        const auto name = arg.fromFirstOccurrenceOf("=", false, false).trim().toLowerCase();
        const auto theme = name == "light"                                  ? KhensuTheme::Theme::Light
                           : (name == "high-contrast" || name == "highcontrast") ? KhensuTheme::Theme::HighContrast
                                                                            : KhensuTheme::Theme::Dark;
        KhensuTheme::setTheme(theme);
        host.window().repaint();
    }
    for (const auto& arg : args)
        if (arg.startsWith("--render-scale="))
            gRenderScale = juce::jlimit(0.5f, 4.0f, arg.fromFirstOccurrenceOf("=", false, false).getFloatValue());
    // Each run starts 3 s after launch, once the window is laid out.
    for (const auto& arg : args) {
        if (arg.startsWith("--soak-audio=")) {
            // Repeats open -> analyse -> every screen -> fix top issue -> fix
            // check -> delete for --soak-minutes (default 30), logging memory,
            // handles and GUI objects per cycle to --soak-log. Exit 1 on a
            // failed cycle or growth over budget.
            double minutes = 30.0;
            juce::File log = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu-soak.txt");
            for (const auto& other : args) {
                if (other.startsWith("--soak-minutes="))
                    minutes = other.fromFirstOccurrenceOf("=", false, false).getDoubleValue();
                if (other.startsWith("--soak-log=")) log = valueOf(other);
            }
            juce::Timer::callAfterDelay(3000, [safe, hostPtr, audio = valueOf(arg), minutes, log] {
                if (safe.getComponent() != nullptr) runSoak(*hostPtr, audio, minutes, log);
            });
            return true;
        }
        if (arg.startsWith("--a11y-audit=")) {
            // Every screen in the real app; exit 0 = clean, 1 = gaps. A
            // whole-app check the unit tests can't do.
            juce::Timer::callAfterDelay(3000, [safe, hostPtr, report = valueOf(arg)] {
                if (safe.getComponent() != nullptr) quitWith(writeAccessibilityAudit(*hostPtr, report) == 0 ? 0 : 1);
            });
            return true;
        }
        if (arg.startsWith("--ui-bench=")) {
            // JSON to <file>, exit 1 if any screen/frame/memory budget is exceeded.
            juce::Timer::callAfterDelay(3000, [safe, hostPtr, report = valueOf(arg)] {
                if (safe.getComponent() != nullptr) quitWith(writeUiBenchmark(*hostPtr, report) == 0 ? 0 : 1);
            });
            return true;
        }
        if (arg.startsWith("--screenshot=")) {
            // A PNG of the whole window as launched, then one per screen,
            // plus each screen's component tree (bounds / visibility). With
            // --screenshot-audio=<file>, afterwards analyse it in a throwaway
            // session and capture the Dashboard during and after the
            // analysis (the results gate), then delete that session.
            juce::File audio;
            juce::File(valueOf(arg)).createDirectory();
            gRunLog = std::make_unique<juce::FileLogger>(juce::File(valueOf(arg)).getChildFile("app-log.txt"),
                                                         "Khensu screenshot run", 0);
            juce::Logger::setCurrentLogger(gRunLog.get());
            for (const auto& other : args)
            {
                if (other.startsWith("--screenshot-audio=")) audio = valueOf(other);
                if (other == "--exercise-all") gExerciseAll = true;
            }
            juce::Timer::callAfterDelay(3000, [safe, hostPtr, dir = valueOf(arg), audio] {
                if (safe.getComponent() == nullptr) return;
                const int written = writeScreenshots(*hostPtr, dir);
                if (audio.existsAsFile()) {
                    runAnalysisScreenshots(*hostPtr, dir, audio); // quits when that finishes
                    return;
                }
                quitWith(written > 0 ? 0 : 1);
            });
            return true;
        }
    }
    return false;
}

} // namespace khensu::diagnostics
