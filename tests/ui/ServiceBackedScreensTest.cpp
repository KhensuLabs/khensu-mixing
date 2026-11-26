// tests/ui/ServiceBackedScreensTest.cpp
// VER-6 (rest): the AI Detection, Reference Library and Settings screens
// against a fake service and a throwaway settings folder - what reaches the
// screen from a scan result, the reference cloud, a similar-references
// search, the Settings toggles that write settings.json, and the cache and
// compute-device calls. ScreensCoverageTest only builds and lays them out.
#include <catch2/catch_test_macros.hpp>
#include "support/FakeApplyChainServer.h"
#include "support/FakeAppController.h"
#include "settings/OnlineSettings.h"
#include "ui/ProvenanceSource/AiDetectionScreen.h"
#include "ui/ReferenceLibrary/ReferenceLibraryScreen.h"
#include "ui/ReferenceLibrary/SimilarReferencesPanel.h"
#include "ui/Settings/SettingsScreen.h"
#include <khensu/session/Session.h>

using khensu::test::FakeAppController;
using khensu::test::FakeApplyChainServer;

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

void pumpFor(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

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

bool anyLabelContains(juce::Component& root, const juce::String& text)
{
    for (auto* l : findAll<juce::Label>(root))
        if (l->getText().contains(text)) return true;
    return false;
}

juce::String labelsText(juce::Component& root)
{
    juce::StringArray all;
    for (auto* l : findAll<juce::Label>(root))
        if (l->getText().isNotEmpty()) all.add(l->getText());
    return all.joinIntoString(" | ");
}

khensu::Session sessionWithFile(const juce::String& path)
{
    khensu::Session s;
    s.id = "sess-ver6";
    s.name = "VER-6";
    khensu::AudioFileRef f;
    f.path = path.toStdString();
    s.files.push_back(f);
    return s;
}

/// A fresh settings folder under the system temp directory.
juce::File freshConfigDir(const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

const char* kScanResponse = R"({
  "duration_sec": 30.0,
  "findings": [
    {"kind": "spectral_ceiling", "summary": "Hard cutoff at 16 kHz", "evidence": "x", "evidence_class": "heuristic",
     "weight": 0.4, "effective_weight": 0.4, "time_range_sec": [2.0, 9.5]},
    {"kind": "clipping", "summary": "Clipped peaks", "evidence": "y", "evidence_class": "observation",
     "weight": 0.2, "effective_weight": 0.0}
  ],
  "timeline": [{"start_sec": 0.0, "end_sec": 5.0, "confidence": 0.3}],
  "service_checklist": [],
  "clipping": {"event_positions_sec": [1.5, 7.25]},
  "detectors_run": ["spectral", "clipping", "codec"]
})";

} // namespace

// ---- AI Detection ------------------------------------------------------------

TEST_CASE("AI Detection: a scan result reaches the screen and asks for the deep pass", "[ver6][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19981);
    server.setResponse("POST /analyze/artifacts_scan", {200, kScanResponse});
    server.setResponse("POST /analyze/mix_quality", {200, R"({"available": false, "reason": "no model trained"})"});
    FakeAppController controller(server.baseUrl());
    controller.session = sessionWithFile("C:/ver6/song.wav");
    {
        khensu::AiDetectionScreen screen(controller, server.baseUrl().toStdString());
        screen.setSize(1200, 800);
        screen.runScan();
        REQUIRE(pumpUntil([&] { return screen.statusTextForTesting().startsWith("Scan complete"); }));
        CHECK(screen.statusTextForTesting().endsWith("2 finding(s) from 3 detector(s)."));

        const auto sent = juce::JSON::parse(server.lastBody("POST /analyze/artifacts_scan"));
        CHECK(sent["file_path"].toString() == "C:/ver6/song.wav");
        CHECK(static_cast<bool>(sent["include_learned"])); // AID-08: the explicit scan is the deep one
        CHECK(server.requestCount("POST /analyze/mix_quality") == 1);
        // AUTO-1: with a result on screen the button runs the scan again.
        REQUIRE(findButton(screen, "Re-scan") != nullptr);
        CHECK(findButton(screen, "Re-scan")->isEnabled());
    }
}

TEST_CASE("AI Detection: a failed scan says so and re-enables the button", "[ver6][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19982);
    server.setResponse("POST /analyze/artifacts_scan", {500, R"({"detail": "Internal error"})"});
    FakeAppController controller(server.baseUrl());
    controller.session = sessionWithFile("C:/ver6/song.wav");
    {
        khensu::AiDetectionScreen screen(controller, server.baseUrl().toStdString());
        screen.setSize(1200, 800);
        screen.runScan();
        REQUIRE(pumpUntil([&] { return screen.statusTextForTesting() == "Scan failed."; }));
        CHECK(findButton(screen, "Scan for AI / Artifacts")->isEnabled());
        CHECK(server.requestCount("POST /analyze/artifacts_scan") == 1);
    }
}

// ---- Reference Library -------------------------------------------------------

TEST_CASE("Reference Library: the cloud shows the selected genre's points and drops a late reply",
          "[ver6][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19983); // the profiles bar asks it for the user's own profiles
    FakeAppController controller(server.baseUrl());
    REQUIRE(controller.genreDb
                .loadFromString(R"({"genres": [
        {"id": "pop", "name": "Pop", "lufs_target": -9.0},
        {"id": "rock", "name": "Rock", "lufs_target": -10.0}]})")
                .is_ok());
    {
        khensu::ReferenceLibraryScreen screen(controller, server.baseUrl().toStdString());
        screen.setSize(1400, 900);
        screen.setVisible(true); // no session: the picker starts on "All genres", nothing to fetch
        CHECK(controller.pendingReferencePoints.empty());
        CHECK(screen.selectedGenreId().isEmpty());
        // UXR-4: All, Unsorted, then per family "All <family>" and its genres
        // (here each genre is its own family, so "All Pop" / "All Rock").
        REQUIRE(screen.pickerChoices().size() == 4);
        screen.selectGenre("pop");
        REQUIRE(controller.pendingReferencePoints.size() == 1);
        const juce::String first = controller.pendingReferencePoints[0].genreId;
        CHECK(first == "pop");

        // Switch genre before the first reply lands (UIX-20).
        screen.selectGenre("rock");
        REQUIRE(controller.pendingReferencePoints.size() == 2);
        const juce::String second = controller.pendingReferencePoints[1].genreId;
        REQUIRE(second != first);

        khensu::ipc::GenreReferencePoint p;
        p.lufs_integrated = -9.0;
        p.lra = 6.0;
        p.spectral_tilt_db_per_octave = -4.5;
        p.crest_factor_db = 9.0;
        REQUIRE(controller.answerReferencePoints(first, {p, p, p}));
        INFO(labelsText(screen).toStdString());
        CHECK_FALSE(anyLabelContains(screen, "3 reference sample points")); // the old genre's reply
        CHECK(anyLabelContains(screen, "Loading reference data for"));

        auto q = p;
        q.lufs_integrated = -11.0;
        q.crest_factor_db = 12.0;
        REQUIRE(controller.answerReferencePoints(second, {p, q}));
        INFO(labelsText(screen).toStdString());
        CHECK(anyLabelContains(screen, "2 reference sample points"));
        CHECK(anyLabelContains(screen, "Load a track to see where it sits in this genre."));
        pumpFor(50); // let the profiles bar's reply land before teardown
    }
}

TEST_CASE("Reference Library: a genre with no reference data says so", "[ver6][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19984);
    FakeAppController controller(server.baseUrl());
    REQUIRE(controller.genreDb.loadFromString(R"({"genres": [{"id": "pop", "name": "Pop", "lufs_target": -9.0}]})")
                .is_ok());
    {
        khensu::ReferenceLibraryScreen screen(controller, server.baseUrl().toStdString());
        screen.setSize(1400, 900);
        screen.setVisible(true);
        screen.selectGenre("pop");
        REQUIRE(controller.answerReferencePoints("pop", {}));
        CHECK(anyLabelContains(screen, "No reference data available for this genre yet."));
        pumpFor(50);
    }
}

TEST_CASE("Similar references: results fill the list and the backend is named", "[ver6][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19985);
    server.setResponse("POST /embeddings/similar", {200, R"({
        "results": [
          {"id": "r1", "path": "C:/refs/one.wav", "label": "One", "similarity": 0.91, "exists": true},
          {"id": "r2", "path": "C:/refs/two.wav", "label": "Two", "similarity": 0.62, "exists": false}],
        "index_size": 2,
        "backend": {"backend": "handcrafted", "reason": "CLAP weights are not cached"}})"});
    FakeAppController controller(server.baseUrl());
    controller.session = sessionWithFile("C:/ver6/song.wav");
    {
        khensu::SimilarReferencesPanel panel(controller, server.baseUrl().toStdString());
        panel.setSize(800, 300);
        auto* find = findButton(panel, "Find similar");
        REQUIRE(find != nullptr);
        find->triggerClick();
        REQUIRE(pumpUntil([&] { return anyLabelContains(panel, "hand-built embedding"); }));
        CHECK(anyLabelContains(panel, "hand-built embedding - CLAP weights are not cached"));

        auto lists = findAll<juce::ListBox>(panel);
        REQUIRE(lists.size() == 1);
        CHECK(lists[0]->getListBoxModel()->getNumRows() == 2);
        const auto sent = juce::JSON::parse(server.lastBody("POST /embeddings/similar"));
        CHECK(sent["file_path"].toString() == "C:/ver6/song.wav");
        CHECK(static_cast<int>(sent["k"]) == 10);
    }
}

TEST_CASE("Similar references: an empty index asks for references first", "[ver6][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19986);
    server.setResponse("POST /embeddings/similar",
                       {200, R"({"results": [], "index_size": 0, "backend": {"backend": "handcrafted"}})"});
    FakeAppController controller(server.baseUrl());
    controller.session = sessionWithFile("C:/ver6/song.wav");
    {
        khensu::SimilarReferencesPanel panel(controller, server.baseUrl().toStdString());
        panel.setSize(800, 300);
        findButton(panel, "Find similar")->triggerClick();
        REQUIRE(pumpUntil([&] { return anyLabelContains(panel, "No references indexed yet - add some first."); }));
        CHECK(findAll<juce::ListBox>(panel)[0]->getListBoxModel()->getNumRows() == 0);
    }
}

// ---- Settings ------------------------------------------------------------------

TEST_CASE("Settings: the master switch writes settings.json and keeps fields it does not own",
          "[ver6][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19987);
    const auto dir = freshConfigDir("khensu_ver6_settings");
    {
        khensu::OnlineSettings seed;
        seed.enabled = true;
        seed.acoustidLookup = true;
        seed.dismissedUpdateVersion = "0.2.0";
        REQUIRE(seed.save(dir));
    }
    FakeAppController controller(server.baseUrl());
    {
        khensu::SettingsScreen screen(controller, server.baseUrl().toStdString(), dir);
        screen.setSize(1200, 2400);
        auto* master = findButton(screen, "Allow online features");
        auto* acoustid = findButton(screen, "AcoustID database lookup");
        REQUIRE(master != nullptr);
        REQUIRE(acoustid != nullptr);
        CHECK(master->getToggleState());
        CHECK(acoustid->isEnabled());

        // Another writer (the scheduled backup) changes a field this screen
        // does not own after the screen loaded (DSK-11).
        khensu::OnlineSettings::update(
            dir, [](khensu::OnlineSettings& s) { s.lastScheduledBackupIso = "2026-11-14T09:00:00Z"; });

        master->triggerClick();
        REQUIRE(pumpUntil([&] { return !khensu::OnlineSettings::load(dir).enabled; }));
        const auto saved = khensu::OnlineSettings::load(dir);
        CHECK(saved.acoustidLookup);                                   // untouched toggle kept
        CHECK(saved.dismissedUpdateVersion == "0.2.0");                // not this screen's
        CHECK(saved.lastScheduledBackupIso == "2026-11-14T09:00:00Z"); // written after load
        CHECK_FALSE(acoustid->isEnabled());                            // the dependent switches follow the master
        pumpFor(50);
    }
    dir.deleteRecursively();
}

TEST_CASE("Settings: every row is reachable in a short window (UI-SET-1)", "[screens][settings]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19990);
    const auto dir = freshConfigDir("khensu_ui_set1_settings");
    FakeAppController controller(server.baseUrl());
    {
        khensu::SettingsScreen screen(controller, server.baseUrl().toStdString(), dir);
        screen.setSize(1440, 900);
        auto* forget = findButton(screen, "Forget my genre examples");
        auto* clear = findButton(screen, "Clear analysis cache");
        REQUIRE(forget != nullptr);
        REQUIRE(clear != nullptr);
        // Laid out at full size, not squeezed to nothing at the bottom...
        CHECK(forget->getHeight() >= 20);
        CHECK(clear->getHeight() >= 20);
        // ...in a viewport that scrolls to them.
        juce::Viewport* vp = nullptr;
        for (auto* child : screen.getChildren())
            if (auto* v = dynamic_cast<juce::Viewport*>(child)) vp = v;
        REQUIRE(vp != nullptr);
        REQUIRE(vp->getViewedComponent() != nullptr);
        CHECK(vp->getViewedComponent()->getHeight() > vp->getHeight());
        CHECK(forget->getBottom() <= vp->getViewedComponent()->getHeight());
        vp->setViewPosition(0, vp->getViewedComponent()->getHeight());
        CHECK(vp->getViewArea().contains(forget->getBounds()));
        pumpFor(50);
    }
    dir.deleteRecursively();
}

TEST_CASE("Settings: compute devices and cache clearing show the service's answer", "[ver6][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19988);
    server.setResponse("GET /diagnostics/accel", {200, R"({
        "onnxruntime": {"installed": true, "chosen_providers": ["CPUExecutionProvider"]},
        "torch": {"installed": true, "chosen_device": "cpu", "device_name": ""},
        "notes": []})"});
    server.setResponse("POST /analysis/cache/clear", {200, R"({"removed": 7})"});
    const auto dir = freshConfigDir("khensu_ver6_settings_cache");
    FakeAppController controller(server.baseUrl());
    {
        khensu::SettingsScreen screen(controller, server.baseUrl().toStdString(), dir);
        screen.setSize(1200, 2400);
        REQUIRE(pumpUntil([&] { return anyLabelContains(screen, "ONNX: CPU"); }));
        INFO(labelsText(screen).toStdString());
        CHECK(anyLabelContains(screen, "Torch: cpu"));

        auto* clear = findButton(screen, "Clear analysis cache");
        REQUIRE(clear != nullptr);
        clear->triggerClick();
        REQUIRE(pumpUntil([&] { return clear->getButtonText() == "Cache cleared (7)"; }));
        CHECK(clear->isEnabled());
        CHECK(server.requestCount("POST /analysis/cache/clear") == 1);
        CHECK_FALSE(dir.getChildFile("settings.json").existsAsFile()); // reading never writes
    }
    dir.deleteRecursively();
}

TEST_CASE("Settings: a failed cache clear offers a retry", "[ver6][screens][service]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19989);
    server.setResponse("POST /analysis/cache/clear", {500, R"({"detail": "Internal error"})"});
    const auto dir = freshConfigDir("khensu_ver6_settings_retry");
    FakeAppController controller(server.baseUrl());
    {
        khensu::SettingsScreen screen(controller, server.baseUrl().toStdString(), dir);
        screen.setSize(1200, 2400);
        auto* clear = findButton(screen, "Clear analysis cache");
        REQUIRE(clear != nullptr);
        clear->triggerClick();
        REQUIRE(pumpUntil([&] { return clear->getButtonText() == "Clear failed - retry"; }));
        CHECK(clear->isEnabled());
    }
    dir.deleteRecursively();
}

// ---- Reference folder scan (REFS-1, UXR-4) -------------------------------------

TEST_CASE("Reference Library: the folder scan fills the counts, the picker and the list", "[screens][service][refs1]")
{
    juce::ScopedJuceInitialiser_GUI init;
    FakeApplyChainServer server(19971);
    server.setResponse("GET /references/scan/roots", {200, R"({"roots": [{"path": "C:/Music", "added_at": "x",
        "last_scan_at": "y", "rescan_on_launch": true, "exists": true, "items": 3}]})"});
    server.setResponse("GET /references/scan/summary", {200, R"({"songs": 2, "samples": 1, "packs": 1,
        "unsorted": 0, "genres": {"pop": 2, "rock": 1}, "families": {"pop": 2, "rock": 1}})"});
    server.setResponse("GET /references/scan/current", {200, "{}"});
    server.setResponse("GET /references/scan/items?limit=500", {200, R"({"total": 0, "items": []})"});
    server.setResponse("GET /references/scan/items?kind=song&limit=500", {200, R"({"total": 2, "items": [
        {"id": "a", "root": "C:/Music", "path": "C:/Music/A/one.wav", "kind": "song", "sample_type": "",
         "title": "One", "author": "Artist A", "album": "LP", "pack": "", "pack_path": "", "artwork_path": "",
         "has_artwork": false, "genre": "pop", "family": "pop", "genre_source": "tag", "genre_confidence": 0.95,
         "tag_genre": "Pop", "bpm": 0, "key": "", "duration_s": 200, "indexed": true, "exists": true, "error": ""},
        {"id": "b", "root": "C:/Music", "path": "C:/Music/B/two.wav", "kind": "song", "sample_type": "",
         "title": "Two", "author": "", "album": "", "pack": "", "pack_path": "", "artwork_path": "",
         "has_artwork": false, "genre": "pop", "family": "pop", "genre_source": "ranker", "genre_confidence": 0.3,
         "tag_genre": "", "bpm": 0, "key": "", "duration_s": 180, "indexed": true, "exists": true, "error": ""}]})"});
    FakeAppController controller(server.baseUrl());
    REQUIRE(controller.genreDb
                .loadFromString(R"({"genres": [
        {"id": "pop", "name": "Pop", "lufs_target": -9.0},
        {"id": "rock", "name": "Rock", "lufs_target": -10.0}]})")
                .is_ok());
    {
        khensu::ReferenceLibraryScreen screen(controller, server.baseUrl().toStdString());
        screen.setSize(1400, 900);
        screen.setVisible(true);
        REQUIRE(pumpUntil([&] { return anyLabelContains(screen, "2 songs, 1 samples, 1 packs found"); }));
        CHECK(anyLabelContains(screen, "C:/Music"));
        // The picker shows each genre's count once the summary is in.
        auto combos = findAll<juce::ComboBox>(screen);
        juce::ComboBox* picker = nullptr;
        for (auto* c : combos)
            if (c->getTitle() == "Genre") picker = c;
        REQUIRE(picker != nullptr);
        bool sawCount = false;
        for (int i = 0; i < picker->getNumItems(); ++i)
            sawCount = sawCount || picker->getItemText(i) == "All Pop (2)";
        CHECK(sawCount);
        // The Songs list asks for songs in every genre.
        REQUIRE(pumpUntil([&] { return server.requestCount("GET /references/scan/items?kind=song&limit=500") > 0; }));
        pumpFor(100);
        // The rescan-on-launch setting came back from the folder.
        bool rescanOn = false;
        for (auto* t : findAll<juce::ToggleButton>(screen))
            if (t->getButtonText() == "Rescan on launch") rescanOn = t->getToggleState();
        CHECK(rescanOn);
        // No analysis: the fit chart explains itself instead of a ranking.
        CHECK(khensu::GenreFitChart::matchOf(0.0f) == 1.0f);
        CHECK(khensu::GenreFitChart::matchOf(2.0f) < 0.4f);
        pumpFor(50);
    }
}
