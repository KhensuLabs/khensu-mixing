// Deliberately free of JuceHeader.h: <windows.h> is confined to this TU so
// its macros can't collide with JUCE names elsewhere (as ServiceProcessJob.cpp).
#include "HeadlessCliProcess.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#endif

namespace khensu::headless {

#if defined(_WIN32)

namespace {

bool usableHandle(HANDLE h)
{
    return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
}

struct ConsoleHandles {
    HANDLE in = nullptr;
    HANDLE out = nullptr;
    HANDLE err = nullptr;
};

HANDLE openInheritable(const wchar_t* name, DWORD access)
{
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    return CreateFileW(name, access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
}

// A GUI-subsystem process gets real std handles only when its launcher
// redirected them (`> out.json`, a pipe, a CI runner). Otherwise attach to
// the parent's console and open it directly, so the CLI's output appears in
// the terminal that started the app.
const ConsoleHandles& consoleHandles()
{
    static const ConsoleHandles handles = [] {
        ConsoleHandles h;
        h.in = GetStdHandle(STD_INPUT_HANDLE);
        h.out = GetStdHandle(STD_OUTPUT_HANDLE);
        h.err = GetStdHandle(STD_ERROR_HANDLE);
        if (!usableHandle(h.out) || !usableHandle(h.err)) {
            const bool attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
            if (attached) {
                HANDLE con = openInheritable(L"CONOUT$", GENERIC_READ | GENERIC_WRITE);
                if (!usableHandle(h.out)) h.out = con;
                if (!usableHandle(h.err)) h.err = con;
                if (!usableHandle(h.in)) h.in = openInheritable(L"CONIN$", GENERIC_READ | GENERIC_WRITE);
            }
        }
        if (!usableHandle(h.in)) h.in = openInheritable(L"NUL", GENERIC_READ);
        for (HANDLE x : {h.in, h.out, h.err})
            if (usableHandle(x)) SetHandleInformation(x, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        return h;
    }();
    return handles;
}

void prependEnv(const wchar_t* name, const std::wstring& value)
{
    if (value.empty()) return;
    std::wstring combined = value;
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n > 1) {
        std::vector<wchar_t> old(n);
        GetEnvironmentVariableW(name, old.data(), n);
        combined += L";";
        combined += old.data();
    }
    SetEnvironmentVariableW(name, combined.c_str());
}

} // namespace

int runWithConsole(const std::wstring& commandLine, const std::wstring& pythonPath)
{
    const auto& h = consoleHandles();
    prependEnv(L"PYTHONPATH", pythonPath);
    // Non-ASCII file names must survive the console / redirect round trip.
    SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = h.in;
    si.hStdOutput = h.out;
    si.hStdError = h.err;

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = commandLine; // CreateProcessW may write into the buffer
    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

namespace {
void writeLine(HANDLE target, const std::string& utf8Line)
{
    if (!usableHandle(target)) return;
    const std::string line = utf8Line + "\r\n";
    DWORD written = 0;
    if (GetFileType(target) == FILE_TYPE_CHAR) {
        // A real console: write UTF-16 so non-ASCII paths display correctly.
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(), wlen);
        WriteConsoleW(target, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
    } else {
        WriteFile(target, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
}
} // namespace

void writeConsoleError(const std::string& utf8Line)
{
    writeLine(consoleHandles().err, utf8Line);
}

void writeConsoleOutput(const std::string& utf8Text)
{
    writeLine(consoleHandles().out, utf8Text);
}

#else

int runWithConsole(const std::wstring&, const std::wstring&) { return -1; }
void writeConsoleError(const std::string&) {}
void writeConsoleOutput(const std::string&) {}

#endif

} // namespace khensu::headless
