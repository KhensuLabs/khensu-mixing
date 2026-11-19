// tests/ui/UpdateNoticeTest.cpp
// The release check's desktop half: the bridge parse of GET /updates/check,
// when the notice shows, and the notice bar's queue / action / dismiss flow.
#include <catch2/catch_test_macros.hpp>
#include "UpdateInstaller.h"
#include "UpdateNotice.h"
#include "ui/shared/AppNoticeBar.h"
#include "ui/shared/ServiceStatusText.h"
#include "support/FakeApplyChainServer.h"

using namespace khensu;
using khensu::test::FakeApplyChainServer;
using UpdateCheckResult = ipc::HttpServiceBridge::UpdateCheckResult;

namespace {
UpdateCheckResult available(const juce::String& latest, const juce::String& url = "https://github.com/x/y")
{
    UpdateCheckResult r;
    r.status = "update_available";
    r.currentVersion = "0.1.0";
    r.latestVersion = latest;
    r.url = url;
    return r;
}
} // namespace

TEST_CASE("Update notice: shows only for a newer release the user hasn't dismissed", "[update]")
{
    CHECK(update_notice::shouldShow(available("0.2.0"), {}));
    CHECK_FALSE(update_notice::shouldShow(available("0.2.0"), "0.2.0")); // dismissed this one
    CHECK(update_notice::shouldShow(available("0.3.0"), "0.2.0"));       // a later release shows again

    for (const char* status : {"up_to_date", "disabled", "unavailable"}) {
        auto r = available("0.2.0");
        r.status = status;
        CHECK_FALSE(update_notice::shouldShow(r, {}));
    }
    CHECK_FALSE(update_notice::shouldShow(available(""), {}));
}

TEST_CASE("Update notice: text names both versions; only https links are offered", "[update]")
{
    const auto r = available("0.2.0");
    CHECK(update_notice::text(r).contains("0.2.0"));
    CHECK(update_notice::text(r).contains("0.1.0"));
    CHECK(update_notice::hasSafeLink(r));
    CHECK_FALSE(update_notice::hasSafeLink(available("0.2.0", "http://github.com/x")));
    CHECK_FALSE(update_notice::hasSafeLink(available("0.2.0", "file:///C:/x.exe")));
    CHECK_FALSE(update_notice::hasSafeLink(available("0.2.0", "")));
}

TEST_CASE("Update notice: installer link is https only; a manual check words every outcome (SHIP-4)", "[update]")
{
    auto r = available("1.1.0");
    CHECK_FALSE(update_notice::hasSafeDownload(r)); // no installer asset
    r.downloadUrl = "http://github.com/x/khensu-setup-1.1.0.exe";
    CHECK_FALSE(update_notice::hasSafeDownload(r));
    r.downloadUrl = "https://github.com/x/khensu-setup-1.1.0.exe";
    CHECK(update_notice::hasSafeDownload(r));
    CHECK(update_notice::manualText(r).contains("1.1.0"));

    auto upToDate = available("0.1.0");
    upToDate.status = "up_to_date";
    CHECK(update_notice::manualText(upToDate).contains("up to date"));
    auto off = upToDate;
    off.status = "disabled";
    CHECK(update_notice::manualText(off).contains("Settings"));
    auto unknown = upToDate;
    unknown.status = "unavailable";
    CHECK(update_notice::manualText(unknown).contains("GitHub"));
}

TEST_CASE("Update check bridge: parses the typed /updates/check response", "[update]")
{
    FakeApplyChainServer server(19891);
    server.setResponse("GET /updates/check?current_version=0.1.0",
                       {200, R"({"status":"update_available","current_version":"0.1.0","latest_version":"0.2.0",)"
                             R"("url":"https://github.com/KhensuLabs/r/v0.2.0",)"
                             R"("download_url":"https://github.com/KhensuLabs/r/releases/download/v0.2.0/khensu-setup-0.2.0.exe",)"
                             R"("checked_at":12.5,"from_cache":true})"});
    server.setResponse("GET /updates/check?current_version=0.1.0&force=true",
                       {200, R"({"status":"disabled","current_version":"0.1.0","latest_version":null,"url":null,)"
                             R"("checked_at":null,"from_cache":false})"});

    ipc::HttpServiceBridge bridge(server.baseUrl().toStdString());
    auto r = bridge.checkForUpdates("0.1.0");
    REQUIRE(r.is_ok());
    CHECK(r.value().updateAvailable());
    CHECK(r.value().latestVersion == "0.2.0");
    CHECK(r.value().url == "https://github.com/KhensuLabs/r/v0.2.0");
    CHECK(r.value().fromCache);
    CHECK(r.value().downloadUrl.endsWith("khensu-setup-0.2.0.exe"));
    CHECK(update_notice::hasSafeDownload(r.value()));

    auto off = bridge.checkForUpdates("0.1.0", /*force=*/true);
    REQUIRE(off.is_ok());
    CHECK(off.value().status == "disabled");
    CHECK(off.value().latestVersion.isEmpty()); // JSON null -> empty, not "null"
    CHECK(off.value().url.isEmpty());
    CHECK(off.value().downloadUrl.isEmpty());
    CHECK_FALSE(update_notice::shouldShow(off.value(), {}));
}

TEST_CASE("AppNoticeBar: queues notices, runs the action or dismiss, then hides", "[update]")
{
    AppNoticeBar bar;
    int layouts = 0;
    bar.onVisibilityChanged = [&] { ++layouts; };
    CHECK_FALSE(bar.isVisible());

    int actions = 0, dismissals = 0;
    bar.post({"crash-report", "crashed", "Open folder", [&] { ++actions; }, [&] { ++dismissals; }});
    bar.post({"crash-report", "duplicate", {}, {}, {}}); // same id: ignored
    bar.post({"update-0.2.0", "0.2.0 is out", "View release", [&] { ++actions; }, [&] { ++dismissals; }});
    CHECK(bar.isVisible());
    CHECK(layouts == 1);
    REQUIRE(bar.current() != nullptr);
    CHECK(bar.current()->text == "crashed");

    bar.triggerAction(); // "Open folder": runs, closes, next notice shows
    CHECK(actions == 1);
    REQUIRE(bar.current() != nullptr);
    CHECK(bar.current()->id == "update-0.2.0");
    CHECK(bar.isVisible());

    bar.triggerDismiss();
    CHECK(dismissals == 1);
    CHECK_FALSE(bar.hasNotice());
    CHECK_FALSE(bar.isVisible());
    CHECK(layouts == 2); // shown once, hidden once
}

TEST_CASE("AppNoticeBar: withdraw removes a notice without running its callbacks (UX-7)", "[update]")
{
    AppNoticeBar bar;
    int callbacks = 0;
    bar.post({"engine", "starting", {}, [&] { ++callbacks; }, [&] { ++callbacks; }});
    bar.post({"update-0.2.0", "0.2.0 is out", {}, {}, {}});
    bar.withdraw("missing"); // not queued: no-op
    REQUIRE(bar.current() != nullptr);
    CHECK(bar.current()->id == "engine");

    bar.withdraw("engine"); // the front: the next one shows
    CHECK(callbacks == 0);
    REQUIRE(bar.current() != nullptr);
    CHECK(bar.current()->id == "update-0.2.0");

    bar.post({"engine", "stopped", {}, {}, {}}); // re-posting after a withdraw is allowed
    bar.withdraw("engine"); // not the front: the visible notice stays
    CHECK(bar.current()->id == "update-0.2.0");
    bar.withdraw("update-0.2.0");
    CHECK_FALSE(bar.isVisible());
}

TEST_CASE("Service status text: one wording for every state (UX-7)", "[update]")
{
    using service_status::bannerText;
    using service_status::stateLine;
    CHECK(bannerText(PythonStatus::Running).isEmpty()); // no banner while it runs
    for (auto s : {PythonStatus::Starting, PythonStatus::Stopped, PythonStatus::GaveUp}) {
        CHECK(bannerText(s).startsWith("The analysis engine"));
        CHECK(stateLine(s).startsWith("Analysis engine: "));
    }
    CHECK(stateLine(PythonStatus::Starting, 2).contains("attempt 2"));
    CHECK(service_status::bannerOffersRestart(PythonStatus::GaveUp));
    CHECK_FALSE(service_status::bannerOffersRestart(PythonStatus::Starting));
    CHECK(service_status::waitingPlaceholder().contains("analysis engine"));
}

// ---- v1.0.0: install a downloaded update on restart (UpdateInstaller) ----

TEST_CASE("Update installer: only a Khensu installer in an updates folder is accepted", "[update]")
{
    auto root = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getChildFile("khensu-update-installer-test-" + juce::String(juce::Random::getSystemRandom().nextInt()));
    auto updates = root.getChildFile("updates");
    REQUIRE(updates.createDirectory());
#if JUCE_MAC
    const juce::String good = "khensu-1.1.0.pkg";
#else
    const juce::String good = "khensu-setup-1.1.0.exe";
#endif
    auto ok = updates.getChildFile(good);
    REQUIRE(ok.replaceWithText("x"));
    auto wrongName = updates.getChildFile("other-setup.exe");
    REQUIRE(wrongName.replaceWithText("x"));
    auto wrongDir = root.getChildFile(good);
    REQUIRE(wrongDir.replaceWithText("x"));

    CHECK(update_installer::isAcceptableInstaller(ok));
    CHECK_FALSE(update_installer::isAcceptableInstaller(wrongName));
    CHECK_FALSE(update_installer::isAcceptableInstaller(wrongDir));
    CHECK_FALSE(update_installer::isAcceptableInstaller(updates.getChildFile("missing.exe")));

    CHECK_FALSE(update_installer::setPending(wrongDir, "1.1.0"));
    REQUIRE(update_installer::setPending(ok, "1.1.0"));
    CHECK(update_installer::pendingInstaller() == ok);
    CHECK(update_installer::pendingVersion() == "1.1.0");
    // The test binary is not an installed copy: nothing is ever launched from a dev tree.
    CHECK_FALSE(update_installer::isInstalledCopy(juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
    CHECK_FALSE(update_installer::launchPending(true));
    update_installer::clearPending();
    CHECK(update_installer::pendingInstaller() == juce::File());
    root.deleteRecursively();
}

TEST_CASE("Update installer: silent arguments keep the install mode and relaunch on request", "[update]")
{
    const auto log = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("install.log");
    const auto user = update_installer::windowsInstallerArgs(false, false, log);
    CHECK(user.contains("/VERYSILENT"));
    CHECK(user.contains("/SUPPRESSMSGBOXES"));
    CHECK(user.contains("/NORESTART"));
    CHECK(user.contains("/CURRENTUSER"));
    CHECK_FALSE(user.contains("/RELAUNCH=1"));
    const auto admin = update_installer::windowsInstallerArgs(true, true, log);
    CHECK(admin.contains("/ALLUSERS"));
    CHECK(admin.contains("/RELAUNCH=1"));

#if JUCE_WINDOWS
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("khensu-installed-copy-" + juce::String(juce::Random::getSystemRandom().nextInt()));
    REQUIRE(dir.createDirectory());
    const auto exe = dir.getChildFile("Khensu Mixing.exe");
    CHECK_FALSE(update_installer::isInstalledCopy(exe));
    REQUIRE(dir.getChildFile("unins000.exe").replaceWithText("x"));
    CHECK(update_installer::isInstalledCopy(exe));
    dir.deleteRecursively();
#endif
}
