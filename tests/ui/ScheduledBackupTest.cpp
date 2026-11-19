// tests/ui/ScheduledBackupTest.cpp
// Pure policy for the automatic daily backup (audit DATA-02): due-check
// semantics and rotation. The copy itself is SessionManager::backupTo, which
// has its own WAL-aware backup/restore tests.
#include "ui/shared/LegacyStoreFiles.h"
#include <catch2/catch_test_macros.hpp>
#include "settings/ScheduledBackup.h"

using namespace khensu::scheduled_backup;

TEST_CASE("isBackupDue policy", "[backup][data02]")
{
    const auto now = juce::Time::fromISO8601("2026-08-07T12:00:00Z");

    SECTION("never backed up -> due") { REQUIRE(isBackupDue({}, now)); }
    SECTION("unparseable timestamp fails toward backing up")
    {
        REQUIRE(isBackupDue("not-a-timestamp", now));
    }
    SECTION("23h ago -> not due")
    {
        REQUIRE_FALSE(isBackupDue("2026-08-06T13:00:00Z", now));
    }
    SECTION("25h ago -> due") { REQUIRE(isBackupDue("2026-08-06T11:00:00Z", now)); }
    SECTION("a future timestamp (clock moved back) does not suppress backups")
    {
        REQUIRE(isBackupDue("2026-09-30T12:00:00Z", now));
    }
}

TEST_CASE("backupFileName is deterministic and chronologically sortable", "[backup][data02]")
{
    const auto t1 = juce::Time::fromISO8601("2026-08-07T09:05:00Z");
    const auto t2 = juce::Time::fromISO8601("2026-08-07T15:30:00Z");
    const auto n1 = backupFileName(t1), n2 = backupFileName(t2);
    REQUIRE(n1.startsWith(kFilePrefix));
    REQUIRE(n1.endsWith(".db"));
    REQUIRE(n1.compare(n2) < 0); // lexicographic == chronological
}

TEST_CASE("pruneOldBackups keeps the newest N and ignores foreign files", "[backup][data02]")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_backup_prune_test");
    dir.deleteRecursively();
    dir.createDirectory();

    for (int day = 1; day <= 10; ++day)
        dir.getChildFile(juce::String(kFilePrefix) + juce::String::formatted("202608%02d-120000.db", day))
            .replaceWithText("x");
    // A user's manual export must never be rotation-pruned.
    dir.getChildFile("khensu_manual_export.db").replaceWithText("x");

    const int deleted = pruneOldBackups(dir, 7);
    REQUIRE(deleted == 3);
    // The three OLDEST are the ones gone.
    REQUIRE_FALSE(dir.getChildFile(juce::String(kFilePrefix) + "20260801-120000.db").existsAsFile());
    REQUIRE_FALSE(dir.getChildFile(juce::String(kFilePrefix) + "20260803-120000.db").existsAsFile());
    REQUIRE(dir.getChildFile(juce::String(kFilePrefix) + "20260804-120000.db").existsAsFile());
    REQUIRE(dir.getChildFile(juce::String(kFilePrefix) + "20260810-120000.db").existsAsFile());
    REQUIRE(dir.getChildFile("khensu_manual_export.db").existsAsFile());

    // Idempotent at or under the cap.
    REQUIRE(pruneOldBackups(dir, 7) == 0);
    dir.deleteRecursively();
}

TEST_CASE("The DB upgrade's leftovers retire after a month, nothing else does (PKG-4)", "[backup][pkg4]")
{
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu_pkg4_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    REQUIRE(dir.createDirectory());
    const auto now = juce::Time::getCurrentTime();
    const auto old = now - juce::RelativeTime::days(45);
    auto make = [&dir](const char* name, juce::Time when) {
        auto f = dir.getChildFile(name);
        REQUIRE(f.replaceWithText("x"));
        REQUIRE(f.setLastModificationTime(when));
        return f;
    };
    make("feedback.db.migrated", old);
    make("feedback.db.migrated-wal", old);
    make("provenance_events.db.bak", old);
    make("service_feedback.db.migrated", now);       // too recent
    make("khensu.db", old);                          // the live store
    make("signing-key.pem.bak", old);                // not the upgrade's
    make("backup-2026.db.bak", old);                 // not a legacy store name

    juce::StringArray names;
    for (const auto& f : khensu::legacy_store::retirable(dir, now, juce::RelativeTime::days(30)))
        names.add(f.getFileName());
    names.sort(false);
    CHECK(names.joinIntoString(",") == "feedback.db.migrated,feedback.db.migrated-wal,provenance_events.db.bak");
    dir.deleteRecursively();
}
