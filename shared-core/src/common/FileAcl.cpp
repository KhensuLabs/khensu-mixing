#include <khensu/common/FileAcl.h>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace khensu {

#if defined(_WIN32)

bool FileAcl::restrictToCurrentUser(const std::string& path)
{
    // Resolve the current process token's user SID.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) {
        CloseHandle(token);
        return false;
    }
    std::vector<BYTE> buffer(needed);
    const bool gotUser = GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed) != 0;
    CloseHandle(token);
    if (!gotUser) return false;

    auto* tokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
    PSID userSid = tokenUser->User.Sid;

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = SET_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = static_cast<LPWSTR>(userSid);

    PACL dacl = nullptr;
    if (SetEntriesInAclW(1, &ea, nullptr, &dacl) != ERROR_SUCCESS || dacl == nullptr) return false;

    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wpath(static_cast<size_t>(wlen));
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);

    // PROTECTED_DACL_SECURITY_INFORMATION strips inherited ACEs from the
    // parent directory, making this the *only* access granted - without it,
    // the file would still be readable via whatever broader ACEs it
    // inherited on creation.
    const DWORD result = SetNamedSecurityInfoW(
        wpath.data(), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);

    LocalFree(dacl);
    return result == ERROR_SUCCESS;
}

#else // !_WIN32 (macOS/Linux - not yet shipped, but the file-permission
      // restriction is portable and correct, so it's implemented rather than
      // no-op'd; the OS credential-store integration - Keychain/libsecret -
      // is the part that stays future work).

#include <sys/stat.h>

bool FileAcl::restrictToCurrentUser(const std::string& path)
{
    // chmod 0600: owner read/write only, no group/other access - the POSIX
    // equivalent of the Windows exclusive-DACL above (audit SEC-06). Same
    // best-effort contract: false on failure, never throws.
    return ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
}

#endif

} // namespace khensu
