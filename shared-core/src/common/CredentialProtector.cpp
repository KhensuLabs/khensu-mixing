#include <khensu/common/CredentialProtector.h>
#include <khensu/common/FileAcl.h>
#include <juce_core/juce_core.h>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <tuple>

#if JUCE_WINDOWS
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#elif JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#elif JUCE_LINUX
#include <cstdio>
#include <sys/stat.h>
#endif

namespace khensu {

namespace {
// Distinguishes protect()'s output from legacy plaintext already sitting in
// an existing settings.json - anything without this prefix is treated as
// unprotected and passed through unprotect() unchanged (the migration path).
constexpr const char* kPrefix = "dpapi1:";
constexpr const char* kReferencePrefix = "credref1:";
constexpr const char* kCredentialDirName = "credentials";
constexpr const char* kCredentialFileSuffix = ".secret";
constexpr const char* kKeychainServiceName = "com.khensu.mixing.credentials";
constexpr const char* kLinuxSecretLabel = "Khensu Mixing";

std::string sanitizeSlotId(std::string_view slotId)
{
    std::string safe;
    safe.reserve(slotId.size());
    for (const unsigned char ch : slotId)
        safe.push_back(std::isalnum(ch) != 0 || ch == '.' || ch == '_' || ch == '-' ? static_cast<char>(ch) : '_');
    return safe;
}

std::string makeReference(const std::string& slotId)
{
    return std::string(kReferencePrefix) + sanitizeSlotId(slotId);
}

std::string slotIdFromReference(const std::string& stored, const std::string& fallbackSlotId)
{
    if (stored.rfind(kReferencePrefix, 0) == 0) return sanitizeSlotId(stored.substr(std::string(kReferencePrefix).size()));
    return sanitizeSlotId(fallbackSlotId);
}

juce::File credentialDir(const std::string& configDir)
{
    return juce::File(juce::String(configDir)).getChildFile(kCredentialDirName);
}

juce::File credentialFile(const std::string& configDir, const std::string& slotId)
{
    return credentialDir(configDir).getChildFile(juce::String(sanitizeSlotId(slotId) + kCredentialFileSuffix));
}

bool tightenCredentialPathPermissions(const juce::File& path)
{
#if JUCE_WINDOWS
    return FileAcl::restrictToCurrentUser(path.getFullPathName().toStdString());
#else
    const auto utf8 = path.getFullPathName().toRawUTF8();
    const mode_t mode = path.isDirectory() ? static_cast<mode_t>(0700) : static_cast<mode_t>(0600);
    return ::chmod(utf8, mode) == 0;
#endif
}

bool ensureCredentialDir(const std::string& configDir)
{
    auto dir = credentialDir(configDir);
    if (!dir.createDirectory() && !dir.isDirectory()) return false;
    if (!tightenCredentialPathPermissions(dir)) {
        // Best effort only: the write is still more private than leaving the
        // secret inline in settings.json.
    }
    return true;
}

// `requireOwnerOnly`: the payload is NOT encrypted (no OS sealing facility),
// so the file is only acceptable if it really is readable by the current user
// alone - otherwise it is removed again and the write reported as failed.
bool writeCredentialFile(const std::string& configDir, const std::string& slotId, const std::string& payload,
                         bool requireOwnerOnly)
{
    if (!ensureCredentialDir(configDir)) return false;
    const auto file = credentialFile(configDir, slotId);
    if (!file.replaceWithText(juce::String(payload), false, false, "\n")) return false;
    const bool tightened = tightenCredentialPathPermissions(file);
    if (requireOwnerOnly && !tightened) {
        file.deleteFile();
        return false;
    }
    return true;
}

std::string readCredentialFile(const std::string& configDir, const std::string& slotId)
{
    const auto file = credentialFile(configDir, slotId);
    if (!file.existsAsFile()) return {};
    return file.loadFileAsString().trimEnd().toStdString();
}

void deleteCredentialFile(const std::string& configDir, const std::string& slotId)
{
    const auto file = credentialFile(configDir, slotId);
    if (file.existsAsFile()) file.deleteFile();
}

#if JUCE_MAC
bool storeInNativeStore(const std::string& slotId, const std::string& plaintext)
{
    SecKeychainItemRef item = nullptr;
    UInt32 existingLength = 0;
    void* existingData = nullptr;
    const OSStatus findStatus = SecKeychainFindGenericPassword(
        nullptr, static_cast<UInt32>(std::strlen(kKeychainServiceName)), kKeychainServiceName,
        static_cast<UInt32>(slotId.size()), slotId.c_str(), &existingLength, &existingData, &item);
    if (existingData != nullptr) SecKeychainItemFreeContent(nullptr, existingData);

    if (findStatus == errSecSuccess && item != nullptr) {
        const OSStatus updateStatus = SecKeychainItemModifyAttributesAndData(item, nullptr,
                                                                             static_cast<UInt32>(plaintext.size()),
                                                                             plaintext.data());
        CFRelease(item);
        return updateStatus == errSecSuccess;
    }

    if (item != nullptr) CFRelease(item);
    const OSStatus addStatus = SecKeychainAddGenericPassword(
        nullptr, static_cast<UInt32>(std::strlen(kKeychainServiceName)), kKeychainServiceName,
        static_cast<UInt32>(slotId.size()), slotId.c_str(), static_cast<UInt32>(plaintext.size()),
        plaintext.data(), nullptr);
    return addStatus == errSecSuccess;
}

std::string loadFromNativeStore(const std::string& slotId)
{
    UInt32 length = 0;
    void* data = nullptr;
    SecKeychainItemRef item = nullptr;
    const OSStatus status = SecKeychainFindGenericPassword(
        nullptr, static_cast<UInt32>(std::strlen(kKeychainServiceName)), kKeychainServiceName,
        static_cast<UInt32>(slotId.size()), slotId.c_str(), &length, &data, &item);
    if (status != errSecSuccess || data == nullptr) {
        if (item != nullptr) CFRelease(item);
        return {};
    }
    std::string result(static_cast<const char*>(data), length);
    SecKeychainItemFreeContent(nullptr, data);
    if (item != nullptr) CFRelease(item);
    return result;
}

void deleteFromNativeStore(const std::string& slotId)
{
    SecKeychainItemRef item = nullptr;
    UInt32 length = 0;
    void* data = nullptr;
    const OSStatus status = SecKeychainFindGenericPassword(
        nullptr, static_cast<UInt32>(std::strlen(kKeychainServiceName)), kKeychainServiceName,
        static_cast<UInt32>(slotId.size()), slotId.c_str(), &length, &data, &item);
    if (data != nullptr) SecKeychainItemFreeContent(nullptr, data);
    if (status == errSecSuccess && item != nullptr) {
        SecKeychainItemDelete(item);
        CFRelease(item);
    }
}
#elif JUCE_LINUX
std::string shellQuote(const std::string& text)
{
    std::string quoted = "'";
    for (const char ch : text) {
        if (ch == '\'')
            quoted += "'\\''";
        else
            quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

bool storeInNativeStore(const std::string& slotId, const std::string& plaintext)
{
    const std::string command = "secret-tool store --label=" + shellQuote(kLinuxSecretLabel) +
                                " service " + shellQuote(kKeychainServiceName) + " account " +
                                shellQuote(slotId);
    FILE* pipe = popen(command.c_str(), "w");
    if (pipe == nullptr) return false;
    const size_t wrote = std::fwrite(plaintext.data(), 1, plaintext.size(), pipe);
    const int status = pclose(pipe);
    return wrote == plaintext.size() && status == 0;
}

std::string loadFromNativeStore(const std::string& slotId)
{
    const std::string command = "secret-tool lookup service " + shellQuote(kKeychainServiceName) +
                                " account " + shellQuote(slotId);
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return {};
    std::string output;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
        output += buffer;
    const int status = pclose(pipe);
    if (status != 0) return {};
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return output;
}

void deleteFromNativeStore(const std::string& slotId)
{
    const std::string command = "secret-tool clear service " + shellQuote(kKeychainServiceName) +
                                " account " + shellQuote(slotId);
    std::ignore = std::system(command.c_str());
}
#else
bool storeInNativeStore(const std::string&, const std::string&) { return false; }
std::string loadFromNativeStore(const std::string&) { return {}; }
void deleteFromNativeStore(const std::string&) {}
#endif
} // namespace

bool CredentialProtector::isProtected(const std::string& stored)
{
    return stored.rfind(kPrefix, 0) == 0 || stored.rfind(kReferencePrefix, 0) == 0;
}

bool CredentialProtector::isReference(const std::string& stored)
{
    return stored.rfind(kReferencePrefix, 0) == 0;
}

#if JUCE_WINDOWS

std::string CredentialProtector::protect(const std::string& plaintext)
{
    if (plaintext.empty()) return plaintext;

    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    in.cbData = static_cast<DWORD>(plaintext.size());

    DATA_BLOB out{};
    // No additional entropy and no description: the Python service must be
    // able to decrypt this with only the standard Win32 call, using nothing
    // beyond what CryptProtectData embeds in the blob itself.
    // CRYPTPROTECT_UI_FORBIDDEN (CORE-17): never pop a DPAPI prompt from a
    // background/service context - fail instead.
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        // Never drop the secret on encryption failure - falling back to
        // plaintext keeps the feature working (just unprotected) rather than
        // corrupting/losing the user's saved key.
        return plaintext;
    }

    const juce::String encoded = juce::Base64::toBase64(out.pbData, static_cast<size_t>(out.cbData));
    LocalFree(out.pbData);
    return std::string(kPrefix) + encoded.toStdString();
}

std::string CredentialProtector::unprotect(const std::string& stored)
{
    if (isReference(stored) || stored.rfind(kPrefix, 0) != 0) return stored; // legacy plaintext, ref, or empty

    const juce::String encoded(stored.substr(std::string(kPrefix).size()));
    juce::MemoryOutputStream decoded;
    // A corrupt Base64 body is undecryptable too (CORE-17): returning
    // `stored` handed the "dpapi1:..." blob back as if it were the secret,
    // which a caller could then send to a cloud API as an API key.
    if (!juce::Base64::convertFromBase64(decoded, encoded)) return {};

    DATA_BLOB in{};
    in.pbData = static_cast<BYTE*>(const_cast<void*>(decoded.getData()));
    in.cbData = static_cast<DWORD>(decoded.getDataSize());

    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        // Undecryptable (moved to another machine/user, or corrupted) - the
        // caller must not receive the still-encrypted blob as if it were the
        // real secret and, e.g., send it to a cloud API as an API key.
        return {};
    }

    std::string result(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

#else // !JUCE_WINDOWS - legacy plaintext passthrough for inline migration values

std::string CredentialProtector::protect(const std::string& plaintext) { return plaintext; }
std::string CredentialProtector::unprotect(const std::string& stored) { return stored; }

#endif

std::string CredentialProtector::protectForSettings(const std::string& slotId, const std::string& plaintext,
                                                    const std::string& configDir)
{
    const auto safeSlotId = sanitizeSlotId(slotId);
    if (plaintext.empty()) {
        deleteFromNativeStore(safeSlotId);
        deleteCredentialFile(configDir, safeSlotId);
        return {};
    }

    if (storeInNativeStore(safeSlotId, plaintext)) {
        deleteCredentialFile(configDir, safeSlotId);
        return makeReference(safeSlotId);
    }

    const auto sealed = protect(plaintext);
    const bool encrypted = isProtected(sealed);
    if (writeCredentialFile(configDir, safeSlotId, sealed, !encrypted)) return makeReference(safeSlotId);

    // Last resort for an ENCRYPTED blob: keep the credential functional inline
    // even if no sidecar storage could be created.
    if (encrypted) return sealed;

    // AUDIT-087: never write a plaintext secret into settings.json (off
    // Windows protect() is a passthrough, so this used to be the fallback).
    // The key stays usable for this run only.
    juce::Logger::writeToLog(juce::String("CredentialProtector: no secure storage for ") + juce::String(safeSlotId) +
                             " - the key was not persisted");
    return {};
}

bool CredentialProtector::needsReseal(const std::string& stored)
{
    return !stored.empty() && !isReference(stored);
}

std::string CredentialProtector::unprotectFromSettings(const std::string& slotId, const std::string& stored,
                                                       const std::string& configDir)
{
    if (stored.empty()) return {};
    if (!isReference(stored)) return unprotect(stored);

    const auto resolvedSlotId = slotIdFromReference(stored, slotId);
    if (const auto native = loadFromNativeStore(resolvedSlotId); !native.empty()) return native;
    return unprotect(readCredentialFile(configDir, resolvedSlotId));
}

} // namespace khensu
