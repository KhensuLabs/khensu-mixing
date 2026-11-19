// tests/shared-core/test_credential_protector.cpp
// Audit SEC-06: cloud API keys were persisted in plaintext in settings.json.
// CredentialProtector now keeps settings.json on opaque references and stores
// the actual secret in native/file-backed local storage.
#include <catch2/catch_test_macros.hpp>
#include <khensu/common/CredentialProtector.h>
#include <juce_core/juce_core.h>

using namespace khensu;

TEST_CASE("CredentialProtector::protect then unprotect round-trips the original secret", "[credentials]")
{
    const std::string secret = "test-only-credential-not-a-real-key";
    const std::string stored = CredentialProtector::protect(secret);

#if JUCE_WINDOWS
    CHECK(stored != secret); // must not be stored as plaintext
    CHECK(CredentialProtector::isProtected(stored));
#endif

    CHECK(CredentialProtector::unprotect(stored) == secret);
}

TEST_CASE("CredentialProtector::protect leaves an empty secret empty", "[credentials]")
{
    CHECK(CredentialProtector::protect("").empty());
}

TEST_CASE("CredentialProtector::unprotect passes through legacy plaintext unchanged", "[credentials]")
{
    // A settings.json written before this feature existed has the raw key
    // with no marker prefix - this is the transparent migration path: load()
    // must still return the real key, not garbage or an empty string.
    const std::string legacyPlaintext = "sk-legacy-plaintext-key";
    CHECK(CredentialProtector::unprotect(legacyPlaintext) == legacyPlaintext);
    CHECK_FALSE(CredentialProtector::isProtected(legacyPlaintext));
}

TEST_CASE("CredentialProtector::isProtected distinguishes protected values from plaintext", "[credentials]")
{
    const std::string stored = CredentialProtector::protect("some-secret");
    CHECK(CredentialProtector::isProtected(stored) == (stored != "some-secret"));
    CHECK_FALSE(CredentialProtector::isProtected("plain-old-string"));
    CHECK_FALSE(CredentialProtector::isProtected(""));
}

TEST_CASE("CredentialProtector::protectForSettings returns an opaque reference and round-trips", "[credentials]")
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_credential_refs");
    dir.deleteRecursively();
    REQUIRE(dir.createDirectory());

    const std::string secret = "sk-anthropic-ref-secret";
    const std::string stored =
        CredentialProtector::protectForSettings("llm.anthropic_api_key", secret, dir.getFullPathName().toStdString());

    CHECK(CredentialProtector::isProtected(stored));
    CHECK(CredentialProtector::isReference(stored));
    CHECK(stored.rfind("credref1:", 0) == 0);
    CHECK(CredentialProtector::unprotectFromSettings("llm.anthropic_api_key", stored,
                                                     dir.getFullPathName().toStdString()) == secret);

    dir.deleteRecursively();
}
