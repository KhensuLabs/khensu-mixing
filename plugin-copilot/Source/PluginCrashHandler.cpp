// plugin-copilot/Source/PluginCrashHandler.cpp - see the header for the contract.
#include "PluginCrashHandler.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#if JUCE_WINDOWS
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <dbghelp.h>
#endif

namespace khensu::plugincrash {

namespace {

std::mutex g_mutex;
int g_refs = 0;

#if JUCE_WINDOWS

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// Prepared at acquire(); the crash path allocates nothing.
MiniDumpWriteDumpFn g_writeDump = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
wchar_t g_prefix[MAX_PATH * 2] = {}; // "<dir>\KhensuCopilot-<version>-"
char g_version[64] = "unknown";
std::atomic<bool> g_written{false};

HMODULE thisModule() noexcept
{
    HMODULE module = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&thisModule), &module);
    return module;
}

bool addressInThisModule(const void* address) noexcept
{
    if (address == nullptr) return false;
    HMODULE owner = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address), &owner))
        return false;
    return owner != nullptr && owner == thisModule();
}

bool writeRecord(const wchar_t* prefix, EXCEPTION_POINTERS* pointers, const char* reason) noexcept
{
    if (g_writeDump == nullptr || prefix[0] == 0) return false;
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    wchar_t dmpPath[MAX_PATH * 2], txtPath[MAX_PATH * 2];
    swprintf_s(dmpPath, L"%s%04u%02u%02u-%02u%02u%02u-%lu.dmp", prefix, t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, ::GetCurrentProcessId());
    swprintf_s(txtPath, L"%s%04u%02u%02u-%02u%02u%02u-%lu.txt", prefix, t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, ::GetCurrentProcessId());

    bool wrote = false;
    HANDLE dmp = ::CreateFileW(dmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dmp != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = ::GetCurrentThreadId();
        info.ExceptionPointers = pointers;
        info.ClientPointers = FALSE;
        // No heap memory: the host's data (and other plugins') stays private.
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo |
                                                     MiniDumpWithUnloadedModules);
        wrote = g_writeDump(::GetCurrentProcess(), ::GetCurrentProcessId(), dmp, type,
                            pointers != nullptr ? &info : nullptr, nullptr, nullptr) != FALSE;
        ::CloseHandle(dmp);
        if (!wrote) ::DeleteFileW(dmpPath);
    }

    const unsigned code = pointers != nullptr && pointers->ExceptionRecord != nullptr
                              ? static_cast<unsigned>(pointers->ExceptionRecord->ExceptionCode)
                              : 0u;
    const void* address = pointers != nullptr && pointers->ExceptionRecord != nullptr
                              ? pointers->ExceptionRecord->ExceptionAddress
                              : nullptr;
    const auto base = reinterpret_cast<const char*>(thisModule());
    const auto offset = address != nullptr && base != nullptr
                            ? static_cast<unsigned long long>(static_cast<const char*>(address) - base)
                            : 0ull;
    char host[MAX_PATH] = {};
    ::GetModuleFileNameA(nullptr, host, MAX_PATH);
    char body[1536];
    const int len = _snprintf_s(body, sizeof(body), _TRUNCATE,
                                "Khensu Copilot (VST3) %s\r\nHost: %s\r\nReason: %s\r\n"
                                "Exception code: 0x%08X at %p (plugin module +0x%llX)\r\n"
                                "Time: %04u-%02u-%02u %02u:%02u:%02u (local)\r\nDump written: %s\r\n"
                                "Nothing was sent anywhere.\r\n",
                                g_version, host, reason, code, address, offset, t.wYear, t.wMonth, t.wDay, t.wHour,
                                t.wMinute, t.wSecond, wrote ? "yes" : "no");
    HANDLE txt = ::CreateFileW(txtPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (txt != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (len > 0) ::WriteFile(txt, body, static_cast<DWORD>(len), &written, nullptr);
        ::CloseHandle(txt);
    }
    return wrote;
}

LONG WINAPI filter(EXCEPTION_POINTERS* pointers) noexcept
{
    const void* address = pointers != nullptr && pointers->ExceptionRecord != nullptr
                              ? pointers->ExceptionRecord->ExceptionAddress
                              : nullptr;
    if (addressInThisModule(address) && !g_written.exchange(true))
        writeRecord(g_prefix, pointers, "unhandled exception in the Khensu Copilot module");
    // Always hand on: the host's own crash handling runs exactly as before.
    if (g_previous != nullptr) return g_previous(pointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

void loadDbgHelp() noexcept
{
    if (g_writeDump == nullptr)
        if (HMODULE dbghelp = ::LoadLibraryW(L"dbghelp.dll"))
            g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
                reinterpret_cast<void*>(::GetProcAddress(dbghelp, "MiniDumpWriteDump")));
}

void prepare(const char* version) noexcept
{
    loadDbgHelp();
    strncpy_s(g_version, version != nullptr ? version : "unknown", _TRUNCATE);
    try {
        const auto dir = crashesDir();
        dir.createDirectory();
        const auto prefix = dir.getChildFile("KhensuCopilot-" + juce::String(g_version) + "-").getFullPathName();
        wcsncpy_s(g_prefix, prefix.toWideCharPointer(), _TRUNCATE);
    } catch (...) {
        g_prefix[0] = 0;
    }
}

#endif // JUCE_WINDOWS

} // namespace

juce::File crashesDir()
{
#if JUCE_WINDOWS
    return juce::File::getSpecialLocation(juce::File::windowsLocalAppData)
        .getChildFile("KhensuMixing")
        .getChildFile("crashes");
#else
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("KhensuMixing")
        .getChildFile("crashes");
#endif
}

void acquire(const char* pluginVersion) noexcept
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_refs++ > 0) return;
#if JUCE_WINDOWS
    prepare(pluginVersion);
    g_previous = ::SetUnhandledExceptionFilter(&filter);
    if (g_previous == &filter) g_previous = nullptr; // never chain to ourselves
#else
    juce::ignoreUnused(pluginVersion);
#endif
}

void release() noexcept
{
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (g_refs <= 0 || --g_refs > 0) return;
#if JUCE_WINDOWS
    // Restore the previous filter only if ours is still installed; if the
    // host replaced it since, put the host's back and leave it alone.
    auto current = ::SetUnhandledExceptionFilter(g_previous);
    if (current != &filter) ::SetUnhandledExceptionFilter(current);
    g_previous = nullptr;
#endif
}

juce::File writeTestRecord(const juce::File& dir)
{
#if JUCE_WINDOWS
    const std::lock_guard<std::mutex> lock(g_mutex);
    loadDbgHelp();
    dir.createDirectory();
    wchar_t prefix[MAX_PATH * 2];
    wcsncpy_s(prefix, dir.getChildFile("KhensuCopilot-test-").getFullPathName().toWideCharPointer(), _TRUNCATE);
    if (!writeRecord(prefix, nullptr, "test record")) return {};
    auto dumps = dir.findChildFiles(juce::File::findFiles, false, "KhensuCopilot-test-*.dmp");
    return dumps.isEmpty() ? juce::File() : dumps.getFirst();
#else
    juce::ignoreUnused(dir);
    return {};
#endif
}

} // namespace khensu::plugincrash
