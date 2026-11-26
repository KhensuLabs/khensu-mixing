// tests/ui/SettingsIntakeAutoTest.cpp
// AUTO-1 (AI Detection's quick scan), AUTO-4 (Settings' service diagnostics),
// AUTO-5 (recurring issues, recent sessions, trend rows) and BUG-6 (the theme
// picker).
#include <catch2/catch_test_macros.hpp>
#include <JuceHeader.h>
#include "support/FakeApplyChainServer.h"
#include "support/FakeAppController.h"
#include "ui/ProvenanceSource/AiDetectionScreen.h"
#include "ui/Settings/DisplayPrefs.h"
#include "ui/Settings/ServiceDiagnosticsText.h"
#include "ui/Settings/SettingsScreen.h"
#include "ui/SessionIntake/RecentSessionsPanel.h"
#include "ui/SessionIntake/RecurringIssues.h"
#include "ui/SessionIntake/RecurringIssuesPanel.h"
#include "ui/SessionIntake/TrendRows.h"
#include "ui/shared/KhensuTheme.h"
#include <khensu/session/Session.h>
#include <thread>

using khensu::test::FakeAppController;
using khensu::test::FakeApplyChainServer;
namespace ri = khensu::recurring_issues;
namespace sd = khensu::service_diagnostics;

namespace {

template <typename Pred> bool pumpUntil(Pred pred, int timeoutMs = 10000)
{
    auto* mm = juce::MessageManager::getInstance();
    const auto deadline = juce::Time::currentTimeMillis() + timeoutMs;
    while (!pred()) {
        if (juce::Time::currentTimeMillis() > deadline) return false;
        mm->runDispatchLoopUntil(20);
    }
    return true;
}

template <typename T> void collect(juce::Component& root, std::vector<T*>& out)
{
    for (auto* child : root.getChildren()) {
        if (auto* t = dynamic_cast<T*>(child)) out.push_back(t);
        collect(*child, out);
    }
}

template <typename T> std::vector<T*> findAll(juce::Component& root)
{
    std::vector<T*> out;
    collect(root, out);
    return out;
}

juce::Button* findButton(juce::Component& root, const juce::String& text)
{
    for (auto* b : findAll<juce::Button>(root))
        if (b->getButtonText() == text) return b;
    return nullptr;
}

juce::String allLabelText(juce::Component& root)
{
    juce::StringArray all;
    for (auto* l : findAll<juce::Label>(root)) all.add(l->getText());
    for (auto* e : findAll<juce::TextEditor>(root)) all.add(e->getText());
    return all.joinIntoString("\n");
}

juce::File freshDir(const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

ri::SessionFindings session(const char* id, const char* updated, std::initializer_list<const char*> codes)
{
    ri::SessionFindings s;
    s.sessionId = id;
    s.sessionName = juce::String("Track ") + id;
    s.updatedAt = updated;
    for (const char* c : codes) s.findings.push_back({c, juce::String("Title of ") + c, 0.4});
    return s;
}

} // namespace

// ---- AUTO-5: the aggregation ---------------------------------------------------

TEST_CASE("Recurring issues: counts each track once, keeps the newest, sorts by count", "[auto5][intake]")
{
    // Newest first.
    const std::vector<ri::SessionFindings> sessions = {
        session("s4", "2026-11-20T10:00:00", {"agent:mud", "agent:harsh", "agent:mud"}),
        session("s3", "2026-11-18T10:00:00", {"agent:mud"}),
        session("s2", "2026-11-10T10:00:00", {"agent:harsh", "agent:wide", "agent:wide"}),
        session("s1", "2026-11-01T10:00:00", {"agent:mud"}),
    };
    const auto rows = ri::aggregate(sessions);
    REQUIRE(rows.size() == 2); // "wide" is in one track only
    CHECK(rows[0].code == "agent:mud");
    CHECK(rows[0].count == 3);
    CHECK(rows[0].scanned == 4);
    CHECK(rows[0].latestSessionId == "s4");
    CHECK(rows[0].lastSeen == "2026-11-20T10:00:00");
    CHECK(rows[1].code == "agent:harsh");
    CHECK(rows[1].count == 2);
}

TEST_CASE("Recurring issues: the trend compares the newer half of the tracks with the older", "[auto5][intake]")
{
    CHECK(ri::trendOf({true, true, false, false}) == ri::Trend::MoreOften);
    CHECK(ri::trendOf({false, false, true, true}) == ri::Trend::LessOften);
    CHECK(ri::trendOf({true, false, true, false}) == ri::Trend::Steady);
    CHECK(ri::trendOf({true, false}) == ri::Trend::Steady); // too few tracks to say
}

TEST_CASE("Recurring issues panel: one row per result, clicking opens that session", "[auto5][intake]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::RecurringIssuesPanel panel([](std::function<void()>) {});
    panel.setSize(500, 220);
    std::string opened;
    panel.onOpenSession = [&](const std::string& id) { opened = id; };
    panel.setResultsForTesting(ri::aggregate({session("a", "2026-11-02", {"agent:mud"}),
                                              session("b", "2026-11-01", {"agent:mud"})}),
                               2);
    REQUIRE(panel.rowCountForTesting() == 1);
    CHECK(panel.statusForTesting().contains("2"));
    auto buttons = findAll<juce::Button>(panel);
    juce::Button* row = nullptr;
    for (auto* b : buttons)
        if (b->getButtonText() == "Title of agent:mud") row = b;
    REQUIRE(row != nullptr);
    CHECK(row->getHeight() > 20);
    row->triggerClick();
    REQUIRE(pumpUntil([&] { return !opened.empty(); }, 2000));
    CHECK(opened == "a");
}

TEST_CASE("Recurring issues panel: measures each listed file once and reads its findings", "[auto5][intake][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19461);
    const auto dir = freshDir("khensu_auto5_recurring");
    const auto a = dir.getChildFile("a.wav");
    const auto b = dir.getChildFile("b.wav");
    a.replaceWithText("x");
    b.replaceWithText("y");
    auto esc = [](const juce::File& f) { return f.getFullPathName().replace("\\", "\\\\"); };
    server.setResponse("GET /agent/sessions?limit=60",
                       {200, R"({"sessions": [
                            {"id": "s2", "name": "B", "updated_at": "2026-11-20", "file_paths": [")" + esc(b) + R"("]},
                            {"id": "s1", "name": "A", "updated_at": "2026-11-10", "file_paths": [")" + esc(a) + R"("]},
                            {"id": "s0", "name": "A again", "updated_at": "2026-11-01", "file_paths": [")" + esc(a) + R"("]},
                            {"id": "gone", "name": "Missing", "updated_at": "2026-10-01", "file_paths": ["C:/nope/missing.wav"]}
                        ], "db_path": "x"})"});
    server.setResponse("POST /agent/findings",
                       {200, R"J({"file_path": "x", "findings": [
                            {"id": "agent:mud_rel_db", "title": "Low-mid build-up (mud)", "severity": 0.6}]})J"});
    {
        khensu::RecurringIssuesPanel panel([](std::function<void()> job) { std::thread(std::move(job)).detach(); },
                                           server.baseUrl().toStdString());
        panel.setSize(500, 220);
        panel.refresh(true);
        REQUIRE(pumpUntil([&] { return panel.rowCountForTesting() == 1; }));
        // a.wav is listed twice (two sessions) but is one mix; the missing file is skipped.
        CHECK(server.requestCount("POST /agent/findings") == 2);
        CHECK(panel.statusForTesting().startsWith("Across your last 2 tracks"));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    }
    dir.deleteRecursively();
}

// ---- AUTO-5: Intake's recent sessions and trend rows ---------------------------

TEST_CASE("Recent sessions: cards that arrive after the layout are full height", "[auto5][intake]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::RecentSessionsPanel panel;
    panel.setSize(900, 160); // laid out first, as Intake does
    std::vector<khensu::Session> sessions(3);
    for (int i = 0; i < 3; ++i) {
        sessions[(size_t)i].id = "id" + std::to_string(i);
        sessions[(size_t)i].name = "Song " + std::to_string(i);
        sessions[(size_t)i].created_at = "2026-11-2" + std::to_string(i);
    }
    panel.setSessions(sessions);
    REQUIRE(panel.cardCountForTesting() == 3);
    CHECK(panel.cardBoundsForTesting(0).getHeight() > 80);
    CHECK(panel.cardBoundsForTesting(2).getX() > panel.cardBoundsForTesting(1).getX());
}

TEST_CASE("Intake trend rows: unmeasured sessions dropped, 0-1 shares put on the 0-100 scale", "[auto5][intake]")
{
    khensu::SessionManager::AnalysisSummary measured;
    measured.lufs_integrated = -14.0f;
    measured.hook_strength = 0.9f;
    measured.memorability_score = 0.5f;
    measured.energy_detected = 55.0f;
    khensu::SessionManager::AnalysisSummary unmeasured; // lufs 0: saved without a measurement
    const auto rows = khensu::trend_rows::forChart({measured, unmeasured});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].hook_strength == 90.0f);
    CHECK(rows[0].memorability_score == 50.0f);
    CHECK(rows[0].energy_detected == 55.0f); // already on its scale
}

// ---- AUTO-4: the wording ---------------------------------------------------------

TEST_CASE("Service diagnostics text: memory, queues, security, cache, registry, genres", "[auto4][settings]")
{
    const auto memory = juce::JSON::parse(R"({"rss_bytes": 524288000, "result_cache_bytes": 1048576,
        "result_cache_budget_bytes": 201326592, "result_caches": [{"name": "a", "entries": 1, "bytes": 1}],
        "decoded_audio_bytes": 0, "decoded_audio_budget_bytes": 268435456, "decoded_audio_entries": 0,
        "pools": [{"name": "dsp", "workers": 4}], "cpu_count": 8,
        "memory_gate": {"enabled": true, "budget_bytes": 1073741824, "in_use_bytes": 0, "waits": 2, "overcommits": 0}})");
    const auto mem = sd::memoryLines(memory);
    REQUIRE(mem.size() == 2);
    CHECK(mem[0].contains("500.0 MB"));
    CHECK(mem[1].contains("dsp 4"));
    CHECK(mem[1].contains("2 waits"));

    const auto queues = sd::queueLines(juce::JSON::parse(R"({"queues": [
        {"name": "render", "active": 1, "oldest_active_age_sec": 12.0, "finished_retained": 3, "max_active": 2},
        {"name": "intake", "active": 0, "oldest_active_age_sec": 0, "finished_retained": 0}],
        "llm_budget": {"anthropic": {"usd": 0.25, "tokens": 1200}}})"));
    CHECK(queues[0].contains("render 1/2"));
    CHECK(queues[1].contains("anthropic"));

    const auto security = sd::securityLines(juce::JSON::parse(R"({"count": 2, "anomalies": {"auth_failure": 6},
        "events": [{"ts": 1764000000.0, "kind": "auth_failure", "detail": "bad token"}]})"));
    CHECK(security[0].contains("auth_failure x6"));
    REQUIRE(security.size() == 2);
    CHECK(security[1].contains("bad token"));

    CHECK(sd::cacheLine(juce::JSON::parse(R"({"enabled": true, "directory": "d", "entries": 12, "bytes": 2097152,
        "budget_bytes": 536870912, "hits": 5, "misses": 1, "puts": 3, "evictions": 0, "analyzers": []})"))
              .contains("12 results, 2.0 MB"));
    CHECK(sd::registryLine(juce::JSON::parse(R"({"schema": 1, "models": {
        "a": {"license": "MIT"}, "b": {"license": "MIT"}, "c": {}}})")) == "Model registry: 3 models (MIT 2, licence not stated 1)");
    CHECK(sd::genreExamplesLine(juce::JSON::parse(R"({"examples": 5, "genres": {"rock": 2, "metal": 3}})")) ==
          "Your genre examples: 5 (metal 3, rock 2)");
    CHECK(sd::genreExamplesLine(juce::JSON::parse(R"({"examples": 0, "genres": {}})")).startsWith("No genre"));
}

TEST_CASE("Settings: the service diagnostics reach the Data section", "[auto4][settings][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19462);
    server.setResponse("GET /diagnostics/memory",
                       {200, R"({"rss_bytes": 1048576, "result_cache_bytes": 0, "result_cache_budget_bytes": 1,
                                 "decoded_audio_bytes": 0, "decoded_audio_budget_bytes": 1, "decoded_audio_entries": 0,
                                 "cpu_count": 4})"});
    server.setResponse("GET /diagnostics/queues", {200, R"({"queues": [], "llm_budget": {}})"});
    server.setResponse("GET /diagnostics/security_events", {200, R"({"count": 0, "events": [], "anomalies": {}})"});
    server.setResponse("GET /analysis/cache",
                       {200, R"({"enabled": true, "directory": "d", "entries": 7, "bytes": 1024, "budget_bytes": 2048,
                                 "hits": 1, "misses": 2, "puts": 3, "evictions": 0, "analyzers": []})"});
    server.setResponse("GET /analyze/models/registry", {200, R"({"models": {"m": {"license": "MIT"}}})"});
    server.setResponse("GET /genre/examples", {200, R"({"examples": 4, "genres": {"pop": 4}})"});
    const auto dir = freshDir("khensu_auto4_settings");
    FakeAppController controller(server.baseUrl());
    {
        khensu::SettingsScreen screen(controller, server.baseUrl().toStdString(), dir);
        screen.setSize(1440, 900);
        REQUIRE(pumpUntil([&] { return allLabelText(screen).contains("Analysis cache: 7 results"); }));
        const auto text = allLabelText(screen);
        CHECK(text.contains("Model registry: 1 model (MIT 1)"));
        CHECK(text.contains("Your genre examples: 4 (pop 4)"));
        CHECK(text.contains("Memory: 1.0 MB resident"));
        CHECK(text.contains("Job queues: all idle"));
        CHECK(text.contains("Security events: 0"));
        REQUIRE(findButton(screen, "Refresh") != nullptr);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    }
    dir.deleteRecursively();
}

// ---- BUG-6: the theme picker -------------------------------------------------------

TEST_CASE("Settings: the theme picker applies the theme live and keeps it", "[bug6][settings]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19463);
    const auto dir = freshDir("khensu_bug6_theme");
    FakeAppController controller(server.baseUrl());
    const auto before = khensu::KhensuTheme::currentTheme();
    {
        khensu::SettingsScreen screen(controller, server.baseUrl().toStdString(), dir);
        screen.setSize(1440, 900);
        juce::ComboBox* theme = nullptr;
        for (auto* c : findAll<juce::ComboBox>(screen))
            if (c->getTitle() == "Theme") theme = c;
        REQUIRE(theme != nullptr);
        theme->setSelectedId(3, juce::sendNotificationSync); // Light
        CHECK(khensu::KhensuTheme::currentTheme() == khensu::KhensuTheme::Theme::Light);
        auto saved = khensu::display_prefs::load(dir);
        REQUIRE(saved.theme.has_value());
        CHECK(*saved.theme == khensu::KhensuTheme::Theme::Light);

        // Ctrl+K's cycle changes it elsewhere: the picker follows and keeps it.
        khensu::KhensuTheme::cycleTheme(); // Light -> Dark
        CHECK(theme->getSelectedId() == 1);
        CHECK(*khensu::display_prefs::load(dir).theme == khensu::KhensuTheme::Theme::Dark);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    }
    // The next launch applies the kept theme.
    khensu::display_prefs::save(dir, {khensu::KhensuTheme::Theme::HighContrast});
    {
        khensu::SettingsScreen screen(controller, server.baseUrl().toStdString(), dir);
        CHECK(khensu::KhensuTheme::currentTheme() == khensu::KhensuTheme::Theme::HighContrast);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    }
    khensu::KhensuTheme::setTheme(before);
    dir.deleteRecursively();
}

// ---- AUTO-1: the quick scan ----------------------------------------------------------

TEST_CASE("AI Detection: the quick scan reuses the analysis' fast pass and offers Re-scan", "[auto1][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19464);
    server.setResponse("POST /analyze/artifacts_scan",
                       {200, R"({"duration_sec": 30.0, "findings": [
                            {"kind": "clipping", "summary": "Clipped peaks", "evidence": "y",
                             "evidence_class": "observation", "weight": 0.2, "effective_weight": 0.0}],
                            "timeline": [], "service_checklist": [], "clipping": {"event_positions_sec": []},
                            "detectors_run": ["spectral", "clipping"]})"});
    FakeAppController controller(server.baseUrl());
    khensu::Session s;
    s.id = "sess-auto1";
    khensu::AudioFileRef f;
    f.path = "C:/auto1/song.wav";
    s.files.push_back(f);
    controller.session = s;
    {
        khensu::AiDetectionScreen screen(controller, server.baseUrl().toStdString());
        screen.setSize(1200, 800);
        REQUIRE(findButton(screen, "Scan for AI / Artifacts") != nullptr); // nothing shown yet
        screen.runFastScan();
        REQUIRE(pumpUntil([&] { return screen.statusTextForTesting().startsWith("Quick scan"); }));
        const auto sent = juce::JSON::parse(server.lastBody("POST /analyze/artifacts_scan"));
        CHECK_FALSE(static_cast<bool>(sent["include_learned"])); // the Dashboard's fast pass, from its cache
        CHECK(server.requestCount("POST /analyze/mix_quality") == 0);
        CHECK(findButton(screen, "Re-scan") != nullptr);
        CHECK(screen.statusTextForTesting().contains("1 finding(s) from 2 detector(s)"));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    }
}
