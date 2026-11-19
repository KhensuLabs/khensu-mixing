// tests/shared-core/test_file_acl.cpp
// Audit SEC-06 remainder: settings.json/token.txt previously inherited
// whatever ACLs their parent directory had, so another local account could
// read the DPAPI-encrypted blob (or the plaintext bearer token) even though
// it couldn't decrypt the former. FileAcl::restrictToCurrentUser locks the
// file down to an exclusive DACL for the current user.
#include <catch2/catch_test_macros.hpp>
#include <khensu/common/FileAcl.h>
#include <juce_core/juce_core.h>

#if defined(_WIN32)
#include <windows.h>
#include <aclapi.h>
#endif

using namespace khensu;

TEST_CASE("FileAcl::restrictToCurrentUser succeeds on an existing file", "[file-acl]")
{
    juce::TemporaryFile tf(".txt");
    tf.getFile().replaceWithText("some-content");

    CHECK(FileAcl::restrictToCurrentUser(tf.getFile().getFullPathName().toStdString()));

    // The file must still be fully usable by the process that just
    // restricted it - restricting to "current user" must not lock the
    // current user out.
    CHECK(tf.getFile().loadFileAsString() == "some-content");
    CHECK(tf.getFile().replaceWithText("more-content"));
}

TEST_CASE("FileAcl::restrictToCurrentUser fails closed on a nonexistent path", "[file-acl]")
{
    CHECK_FALSE(FileAcl::restrictToCurrentUser("Z:/definitely/not/a/real/path/file.txt"));
}

#if defined(_WIN32)
TEST_CASE("FileAcl::restrictToCurrentUser replaces inherited ACEs with a single exclusive one", "[file-acl]")
{
    juce::TemporaryFile tf(".txt");
    tf.getFile().replaceWithText("x");
    const auto path = tf.getFile().getFullPathName();

    REQUIRE(FileAcl::restrictToCurrentUser(path.toStdString()));

    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD rc = GetNamedSecurityInfoW(path.toWideCharPointer(), SE_FILE_OBJECT,
                                            DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd);
    REQUIRE(rc == ERROR_SUCCESS);
    REQUIRE(dacl != nullptr);
    CHECK(dacl->AceCount == 1);
    if (sd != nullptr) LocalFree(sd);
}
#endif
