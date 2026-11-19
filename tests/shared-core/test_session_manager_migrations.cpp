// Migration-directory / runtime consistency (2026-07-18 audit ARCH-07).
//
// The storage/schema/NNN_*.sql files are now the SINGLE SOURCE OF TRUTH:
// SessionManager::migrations() is generated from them at CMake configure time
// (cmake/GenerateMigrations.cmake), so drift between the two is impossible by
// construction. This test is now a codegen sanity check - it verifies the
// generation actually ran and produced one runtime migration per file, with
// matching versions and (whitespace/comment-normalized) SQL, and that the
// versions are contiguous from 1. A failure here means the codegen or the
// .sql files themselves are malformed, not that two hand-maintained copies
// drifted.
#include <catch2/catch_test_macros.hpp>
#include <khensu/session/SessionManager.h>
#include <juce_core/juce_core.h>

using namespace khensu;

namespace {

// Strips `-- ...` line comments and collapses all whitespace runs to a
// single space, so a checked-in file's human-readable formatting/comments
// can be compared against the runtime string's C++-source indentation.
juce::String normalizedCollapsed(const std::string& raw)
{
    juce::StringArray lines;
    lines.addLines(juce::String(raw));
    juce::String out;
    for (auto line : lines) {
        const int commentPos = line.indexOf("--");
        if (commentPos >= 0) line = line.substring(0, commentPos);
        out << line.replace("\t", " ") << " ";
    }
    while (out.contains("  ")) out = out.replace("  ", " ");
    return out.trim();
}

} // namespace

TEST_CASE("Every runtime migration has a matching checked-in storage/schema file", "[session][migrations]")
{
    const juce::File schemaDir(juce::String(KHENSU_REPO_ROOT_DIR) + "/storage/schema");
    REQUIRE(schemaDir.isDirectory());

    const auto& migrations = SessionManager::migrations();
    REQUIRE_FALSE(migrations.empty());

    for (const auto& m : migrations) {
        const auto matches = schemaDir.findChildFiles(
            juce::File::findFiles, false,
            juce::String::formatted("%03d_*.sql", m.version));
        INFO("No checked-in storage/schema/" << juce::String::formatted("%03d_*.sql", m.version)
             << " for runtime migration " << m.version);
        REQUIRE(matches.size() == 1);

        const auto checkedIn = matches[0].loadFileAsString().toStdString();
        CHECK(normalizedCollapsed(checkedIn) == normalizedCollapsed(m.sql));
    }
}

TEST_CASE("storage/schema has no orphaned migration file beyond the runtime list", "[session][migrations]")
{
    const juce::File schemaDir(juce::String(KHENSU_REPO_ROOT_DIR) + "/storage/schema");
    REQUIRE(schemaDir.isDirectory());

    const auto& migrations = SessionManager::migrations();
    int maxRuntimeVersion = 0;
    for (const auto& m : migrations) maxRuntimeVersion = std::max(maxRuntimeVersion, m.version);

    for (const auto& file : schemaDir.findChildFiles(juce::File::findFiles, false, "*.sql")) {
        const int version = file.getFileName().getIntValue();
        INFO("storage/schema/" << file.getFileName() << " has no matching runtime migration");
        CHECK(version >= 1);
        CHECK(version <= maxRuntimeVersion);
    }
}
