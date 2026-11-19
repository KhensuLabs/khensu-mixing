// desktop-app/Source/CrashReporter.cpp
#include "CrashReporter.h"
#include <khensu/common/Version.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if JUCE_WINDOWS
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <dbghelp.h>
#endif

namespace khensu::crash {

namespace {

constexpr const char* kAckFileName = "acknowledged.txt";

#if JUCE_WINDOWS

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// Everything the crash path touches is prepared at install and lives in
// fixed storage: no heap, no juce::String once a crash is in progress.
MiniDumpWriteDumpFn g_writeDump = nullptr;
wchar_t g_prefix[MAX_PATH * 2] = {}; // "<dir>\Khensu-<version>-"
char g_header[1024] = {};            // version / service / OS lines
char g_screen[64] = "(none)";
std::atomic<bool> g_reported{false}; // one report per process (abort after terminate, ...)

// Writer thread: the crashing thread posts its context and waits.
HANDLE g_requestEvent = nullptr;
HANDLE g_doneEvent = nullptr;
// Kept open so the crash path can tell whether the writer is still alive:
// ExitProcess kills every other thread, so a crash during shutdown (static
// destructors, atexit) found a dead writer, waited 30 s and wrote nothing -
// the app-loop's one unexplained 0xE06D7363 exit left no report (2026-11-18).
HANDLE g_writerThread = nullptr;
EXCEPTION_POINTERS* g_requestPointers = nullptr;
DWORD g_requestThreadId = 0;
const char* g_requestReason = "";

MiniDumpWriteDumpFn loadDbgHelp() noexcept
{
    if (g_writeDump != nullptr) return g_writeDump;
    if (HMODULE dbghelp = ::LoadLibraryW(L"dbghelp.dll"))
        g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(
            reinterpret_cast<void*>(::GetProcAddress(dbghelp, "MiniDumpWriteDump")));
    return g_writeDump;
}

/// The one writer. `prefix` is "<dir>\Khensu-<version>-"; the timestamp,
/// pid and extension are appended here. Returns true when the .dmp was written.
bool writeFiles(const wchar_t* prefix, const char* header, EXCEPTION_POINTERS* pointers, DWORD threadId,
                const char* reason, wchar_t* dmpPathOut, size_t dmpPathCap) noexcept
{
    auto writeDump = loadDbgHelp();
    if (writeDump == nullptr || prefix[0] == 0) return false;

    static std::atomic<unsigned> counter{0};
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    wchar_t stem[MAX_PATH * 2];
    swprintf_s(stem, L"%s%04u%02u%02u-%02u%02u%02u-%lu-%u", prefix, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, ::GetCurrentProcessId(), counter.fetch_add(1));
    wchar_t dmpPath[MAX_PATH * 2];
    wchar_t txtPath[MAX_PATH * 2];
    swprintf_s(dmpPath, L"%s.dmp", stem);
    swprintf_s(txtPath, L"%s.txt", stem);

    bool wroteDump = false;
    HANDLE dmp = ::CreateFileW(dmpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dmp != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = threadId;
        info.ExceptionPointers = pointers;
        info.ClientPointers = FALSE;
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                                                     MiniDumpWithUnloadedModules);
        wroteDump = writeDump(::GetCurrentProcess(), ::GetCurrentProcessId(), dmp, type,
                              pointers != nullptr ? &info : nullptr, nullptr, nullptr) != FALSE;
        ::CloseHandle(dmp);
        if (!wroteDump) ::DeleteFileW(dmpPath);
    }

    char body[2048];
    const unsigned code = pointers != nullptr && pointers->ExceptionRecord != nullptr
                              ? static_cast<unsigned>(pointers->ExceptionRecord->ExceptionCode)
                              : 0u;
    const void* address = pointers != nullptr && pointers->ExceptionRecord != nullptr
                              ? pointers->ExceptionRecord->ExceptionAddress
                              : nullptr;
    const int len = _snprintf_s(body, sizeof(body), _TRUNCATE,
                                "%sReason: %s\r\nException code: 0x%08X at %p\r\nThread id: %lu\r\n"
                                "Last screen: %s\r\nTime: %04u-%02u-%02u %02u:%02u:%02u (local)\r\n"
                                "Dump written: %s\r\n",
                                header, reason, code, address, threadId, g_screen, t.wYear, t.wMonth, t.wDay,
                                t.wHour, t.wMinute, t.wSecond, wroteDump ? "yes" : "no");
    HANDLE txt = ::CreateFileW(txtPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (txt != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (len > 0) ::WriteFile(txt, body, static_cast<DWORD>(len), &written, nullptr);
        ::CloseHandle(txt);
    }
    if (wroteDump && dmpPathOut != nullptr) wcscpy_s(dmpPathOut, dmpPathCap, dmpPath);
    return wroteDump;
}

DWORD WINAPI writerThreadMain(LPVOID) noexcept
{
    for (;;) {
        if (::WaitForSingleObject(g_requestEvent, INFINITE) != WAIT_OBJECT_0) return 0;
        writeFiles(g_prefix, g_header, g_requestPointers, g_requestThreadId, g_requestReason, nullptr, 0);
        ::SetEvent(g_doneEvent);
    }
}

/// Crash path: hand off to the writer thread (or write inline if it never
/// started), at most once per process.
void reportCrash(EXCEPTION_POINTERS* pointers, const char* reason) noexcept
{
    if (g_reported.exchange(true)) return;
    const bool writerAlive = g_requestEvent != nullptr && g_doneEvent != nullptr && g_writerThread != nullptr &&
                             ::WaitForSingleObject(g_writerThread, 0) == WAIT_TIMEOUT;
    if (writerAlive) {
        g_requestPointers = pointers;
        g_requestThreadId = ::GetCurrentThreadId();
        g_requestReason = reason;
        ::SetEvent(g_requestEvent);
        if (::WaitForSingleObject(g_doneEvent, 30000) == WAIT_OBJECT_0) return;
        // The writer died or hung mid-way (process exit): write from here.
    }
    writeFiles(g_prefix, g_header, pointers, ::GetCurrentThreadId(), reason, nullptr, 0);
}

LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* pointers) noexcept
{
    reportCrash(pointers, "unhandled exception");
    return EXCEPTION_CONTINUE_SEARCH; // let Windows Error Reporting / the debugger carry on as before
}

void __cdecl pureCallHandler() noexcept
{
    reportCrash(nullptr, "pure virtual function call");
    std::abort();
}

void __cdecl invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int,
                                     uintptr_t) noexcept
{
    reportCrash(nullptr, "invalid CRT parameter");
    std::abort();
}

void __cdecl abortSignalHandler(int) noexcept
{
    reportCrash(nullptr, "abort()");
}

#endif // JUCE_WINDOWS

juce::Array<juce::File> reportsNewestFirst(const juce::File& dir)
{
    auto files = dir.findChildFiles(juce::File::findFiles, false, "*.dmp");
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
        const auto ta = a.getLastModificationTime(), tb = b.getLastModificationTime();
        return ta != tb ? ta > tb : a.getFileName() > b.getFileName();
    });
    return files;
}

} // namespace

juce::File defaultCrashesDir()
{
    // CE-ENG-4: %LOCALAPPDATA%\KhensuMixing\crashes on Windows - dumps are
    // large and machine-specific, so they must not roam with the profile.
    // The plugin and the Python service (crash_log.crashes_dir) use the same
    // folder.
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

void install(const juce::File& crashesDir, const juce::String& appVersion, const juce::String& serviceVersion)
{
    crashesDir.createDirectory();
    pruneReports(crashesDir, kMaxReports);
#if JUCE_WINDOWS
    loadDbgHelp();
    const auto prefix = crashesDir.getChildFile("Khensu-" + appVersion + "-").getFullPathName();
    wcsncpy_s(g_prefix, prefix.toWideCharPointer(), _TRUNCATE);
    const auto header = "Khensu Mixing " + appVersion + "\r\nService API: " + serviceVersion +
                        "\r\nOS: " + juce::SystemStats::getOperatingSystemName() +
                        (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : "") + "\r\n";
    strncpy_s(g_header, header.toRawUTF8(), _TRUNCATE);

    if (g_requestEvent == nullptr) {
        g_requestEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_doneEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_requestEvent != nullptr && g_doneEvent != nullptr) {
            // Runs for the life of the process; the handle stays open (see g_writerThread).
            g_writerThread = ::CreateThread(nullptr, 256 * 1024, writerThreadMain, nullptr, 0, nullptr);
        }
    }
    // Room for the filter itself if the main thread overflows its stack.
    ULONG guarantee = 64 * 1024;
    ::SetThreadStackGuarantee(&guarantee);
    ::SetUnhandledExceptionFilter(unhandledExceptionFilter);
    _set_purecall_handler(pureCallHandler);
    _set_invalid_parameter_handler(invalidParameterHandler);
    std::signal(SIGABRT, abortSignalHandler);
#else
    juce::ignoreUnused(appVersion, serviceVersion);
#endif
}

void setCurrentScreen(const char* name) noexcept
{
#if JUCE_WINDOWS
    strncpy_s(g_screen, name != nullptr ? name : "(none)", _TRUNCATE);
#else
    juce::ignoreUnused(name);
#endif
}

juce::File writeReportNow(const juce::File& crashesDir, void* exceptionPointers, const char* reason)
{
#if JUCE_WINDOWS
    crashesDir.createDirectory();
    // Same naming as the installed path, but for an arbitrary directory.
    const auto prefix = crashesDir.getChildFile(juce::String("Khensu-") + KHENSU_VERSION_STRING + "-")
                            .getFullPathName();
    const char* header = g_header[0] != 0 ? g_header : "Khensu Mixing (crash reporter not installed)\r\n";
    wchar_t dmpPath[MAX_PATH * 2] = {};
    if (!writeFiles(prefix.toWideCharPointer(), header, static_cast<EXCEPTION_POINTERS*>(exceptionPointers),
                    ::GetCurrentThreadId(), reason != nullptr ? reason : "manual", dmpPath, MAX_PATH * 2))
        return {};
    return juce::File(juce::String(dmpPath));
#else
    juce::ignoreUnused(crashesDir, exceptionPointers, reason);
    return {};
#endif
}

void writeTerminateReport() noexcept
{
#if JUCE_WINDOWS
    reportCrash(nullptr, "std::terminate (uncaught C++ exception)");
#endif
}

juce::Array<juce::File> listReports(const juce::File& dir)
{
    return reportsNewestFirst(dir);
}

void pruneReports(const juce::File& dir, int keep)
{
    const auto reports = reportsNewestFirst(dir);
    for (int i = std::max(keep, 0); i < reports.size(); ++i) {
        reports[i].deleteFile();
        reports[i].withFileExtension("txt").deleteFile();
    }
}

PendingNotice pendingNotice(const juce::File& dir)
{
    PendingNotice notice;
    const auto reports = reportsNewestFirst(dir);
    if (reports.isEmpty()) return notice;
    notice.newestReport = reports.getFirst();
    const auto ack = dir.getChildFile(kAckFileName);
    notice.show = !ack.existsAsFile() || ack.loadFileAsString().trim() != notice.newestReport.getFileName();
    return notice;
}

void acknowledge(const juce::File& dir)
{
    const auto reports = reportsNewestFirst(dir);
    if (reports.isEmpty()) return;
    dir.getChildFile(kAckFileName).replaceWithText(reports.getFirst().getFileName());
}

} // namespace khensu::crash
