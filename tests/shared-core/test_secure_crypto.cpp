// tests/shared-core/test_secure_crypto.cpp
// AUDIT-020 / AUDIT-022: the service bearer token now comes from the OS CSPRNG
// (was juce::Uuid, a clock-seeded LCG) and the desktop verifies the listener
// on 127.0.0.1:47239 with an HMAC-SHA256(token, nonce) challenge before it
// trusts it. These pin the primitives to their published test vectors.
#include <catch2/catch_test_macros.hpp>
#include <khensu/common/HmacSha256.h>
#include <khensu/common/SecureRandom.h>

#include <algorithm>
#include <cctype>
#include <string>

using namespace khensu;

namespace {
std::string sha256Hex(const std::string& s)
{
    const auto d = HmacSha256::sha256(s.data(), s.size());
    return toLowerHex(d.data(), d.size());
}
} // namespace

TEST_CASE("SHA-256 matches the FIPS 180-4 example vectors", "[crypto]")
{
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // 56 bytes: the length no longer fits in the first padding block.
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("HMAC-SHA256 matches the RFC 4231 test vectors", "[crypto]")
{
    SECTION("test case 1: 20-byte key")
    {
        const std::string key(20, '\x0b');
        CHECK(HmacSha256::hmacHex(key, "Hi There") ==
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    }
    SECTION("test case 2: short text key")
    {
        CHECK(HmacSha256::hmacHex("Jefe", "what do ya want for nothing?") ==
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    }
    SECTION("test case 6: key longer than the block size is hashed first")
    {
        const std::string key(131, '\xaa');
        CHECK(HmacSha256::hmacHex(key, "Test Using Larger Than Block-Size Key - Hash Key First") ==
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
    }
}

TEST_CASE("constantTimeEquals compares length and content", "[crypto]")
{
    CHECK(HmacSha256::constantTimeEquals("abc", "abc"));
    CHECK_FALSE(HmacSha256::constantTimeEquals("abc", "abd"));
    CHECK_FALSE(HmacSha256::constantTimeEquals("abc", "abcd"));
    CHECK(HmacSha256::constantTimeEquals("", ""));
}

TEST_CASE("SecureRandom::hex(32) yields a 64-char lowercase-hex token that differs per call", "[crypto][token]")
{
    const std::string a = SecureRandom::hex(32);
    const std::string b = SecureRandom::hex(32);
    REQUIRE(a.size() == 64);
    REQUIRE(b.size() == 64);
    CHECK(std::all_of(a.begin(), a.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0 || (c >= 'a' && c <= 'f');
    }));
    CHECK(a != b);
}
