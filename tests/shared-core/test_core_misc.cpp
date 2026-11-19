// tests/shared-core/test_core_misc.cpp
// Regressions for shared-core logic defects from the 2026-10-10 audit:
// genre override precedence and malformed ranges, stem-energy blending,
// LIKE-prefix escaping, and concurrent SessionManager use.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/mixerchain/PluginScanner.h>
#include <khensu/session/SessionManager.h>
#include <khensu/targets/GenreDatabase.h>
#include <khensu/targets/GenreEntry.h>
#include <atomic>
#include <mutex>
#include <string>
#include <cmath>
#include <thread>
#include <vector>

using namespace khensu;

TEST_CASE("Explicit genre JSON wins over built-in tilt/crest overrides", "[genre][audit]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(R"({"genres": [
        {"id": "shoegaze", "name": "Shoegaze", "spectral_tilt_target_db_per_octave": -6.5, "crest_factor_target_db": 12.0},
        {"id": "hyperpop", "name": "Hyperpop"},
        {"id": "broken", "name": "Broken", "lufs_range": [-9.0]}]})")
                .is_ok());
    const auto shoegaze = db.findById("shoegaze");
    REQUIRE(shoegaze.has_value());
    CHECK(shoegaze->spectral_tilt_target_db_per_octave == -6.5f); // user value kept
    CHECK(shoegaze->crest_factor_target_db == 12.0f);
    const auto hyperpop = db.findById("hyperpop");
    REQUIRE(hyperpop.has_value());
    // override fills the gap (authored in the old density convention, +3.01
    // dB/oct into the octave-band-power convention - AUDIT-009)
    CHECK_THAT(hyperpop->spectral_tilt_target_db_per_octave, Catch::Matchers::WithinAbs(-1.0f + 3.0103f, 1e-4f));
    // A malformed lufs_range keeps sane defaults instead of 0/0 LUFS.
    const auto broken = db.findById("broken");
    REQUIRE(broken.has_value());
    CHECK(broken->lufs_range_min == -16.0f);
    CHECK(broken->lufs_range_max == -11.0f);
}

TEST_CASE("Blended stem-energy shares treat missing buckets as 0", "[genre][audit]")
{
    GenreEntry a, b;
    a.id = "a";
    b.id = "b";
    a.stem_energy_balance_pct = {{"vocals", 40.0f}, {"drums", 60.0f}};
    b.stem_energy_balance_pct = {{"vocals", 30.0f}, {"bass", 70.0f}};
    const auto blended = blendGenreEntries({{a, 0.5f}, {b, 0.5f}});
    float total = 0.0f;
    for (const auto& [bucket, pct] : blended.stem_energy_balance_pct) total += pct;
    CHECK(std::abs(total - 100.0f) < 0.01f); // was 135% when averaging only over definers
    CHECK(std::abs(blended.stem_energy_balance_pct.at("drums") - 30.0f) < 0.01f);
}

TEST_CASE("Preference-key prefix listing treats '_' literally", "[session][audit]")
{
    SessionManager sm(":memory:");
    REQUIRE(sm.setUserPreference("dashboard_layout_main", "1").is_ok());
    REQUIRE(sm.setUserPreference("dashboardXlayoutXother", "1").is_ok());
    const auto keys = sm.listUserPreferenceKeysWithPrefix("dashboard_layout_");
    REQUIRE(keys.size() == 1);
    CHECK(keys[0] == "dashboard_layout_main");
}

TEST_CASE("SessionManager tolerates concurrent writers on one connection", "[session][audit]")
{
    SessionManager sm(":memory:");
    std::atomic<int> failures{0};
    std::mutex errorMutex;
    std::string firstError;
    auto recordError = [&](const std::string& e) {
        const std::lock_guard<std::mutex> lk(errorMutex);
        if (firstError.empty()) firstError = e;
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&sm, &failures, &recordError, t] {
            for (int i = 0; i < 50; ++i) {
                if (auto r = sm.setUserPreference("k" + std::to_string(t) + "_" + std::to_string(i), "\"v\"");
                    !r.is_ok()) {
                    ++failures;
                    recordError("pref: " + r.error());
                }
                std::vector<mixerchain::ScannedPlugin> plugins(3);
                int n = 0;
                for (auto& p : plugins) {
                    p.id = "id" + std::to_string(t) + "_" + std::to_string(i) + "_" + std::to_string(n++);
                    p.name = "p" + std::to_string(i);
                    p.file_path = "C:/plug/" + p.id + ".vst3";
                }
                if (auto r = sm.saveScannedPlugins(plugins); !r.is_ok()) { // DELETE+INSERT transaction
                    ++failures;
                    recordError("scan: " + r.error());
                }
            }
        });
    for (auto& th : threads) th.join();
    INFO("first error: " << firstError);
    CHECK(failures.load() == 0);
    CHECK(sm.listUserPreferenceKeysWithPrefix("k").size() == 200);
}

#include <khensu/recommendations/RecommenderCommon.h>

TEST_CASE("Severity of an absolute-unit error does not depend on the threshold", "[recommend][audit]")
{
    using namespace khensu::recommendations::common;
    // 1 dB over a -11 LUFS ceiling is exactly as severe as 1 dB over -23.
    CHECK(severityFromError(-10.0f, -11.0f, severity_scale::kLufs) ==
          severityFromError(-22.0f, -23.0f, severity_scale::kLufs));
    // A tilt target near 0 dB/oct no longer saturates on a small miss.
    CHECK(severityFromError(-2.5f, -1.0f, severity_scale::kTilt) < 50.0f);
    CHECK(severityFromError(0.0f, 100.0f, 1.0f) == 100.0f); // clamped
}

TEST_CASE("Deleting a session removes its session-scoped preferences only", "[session][audit]")
{
    khensu::SessionManager sm(":memory:");
    auto created = sm.createSession("Doomed");
    REQUIRE(created.is_ok());
    const std::string id = created.value().id;
    REQUIRE(sm.setUserPreference("chat_history_" + id, "[]").is_ok());
    REQUIRE(sm.setUserPreference("vocal_macros_" + id, "{}").is_ok());
    REQUIRE(sm.setUserPreference("chat_history_other-session", "[]").is_ok());
    REQUIRE(sm.setUserPreference("theme", "\"dark\"").is_ok());

    REQUIRE(sm.deleteSession(id).is_ok());
    CHECK(sm.listUserPreferenceKeysWithPrefix("chat_history_" + id).empty());
    CHECK(sm.listUserPreferenceKeysWithPrefix("vocal_macros_" + id).empty());
    CHECK(sm.listUserPreferenceKeysWithPrefix("chat_history_other-session").size() == 1);
    CHECK(sm.listUserPreferenceKeysWithPrefix("theme").size() == 1);
}

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#include <khensu/ipc/HttpConnectionLimit.h>

TEST_CASE("WinINet allows more than 2 connections to the service (2026-11-18)", "[ipc][connections]")
{
    // WinINet's default is 2 per HTTP/1.1 server: the health probe queued
    // behind long scans and the identity gate closed on a healthy service.
    khensu::ipc::ensureHttpConnectionLimit();
    DWORD value = 0, size = sizeof(value);
    REQUIRE(InternetQueryOptionW(nullptr, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &value, &size));
    CHECK(value == khensu::ipc::kHttpConnectionsPerServer);
}
#endif
