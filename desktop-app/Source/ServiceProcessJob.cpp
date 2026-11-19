// Deliberately free of JuceHeader.h: <windows.h> is confined to this TU so
// its macros can't collide with JUCE names elsewhere.
#include "ServiceProcessJob.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <cwchar>
#include <cwctype>
#endif

namespace khensu {

#if defined(_WIN32)

ServiceProcessJob::ServiceProcessJob()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return;
    }
    handle_ = job;
}

ServiceProcessJob::~ServiceProcessJob()
{
    if (handle_ != nullptr) CloseHandle(static_cast<HANDLE>(handle_));
}

int ServiceProcessJob::adoptNewPythonChildren()
{
    if (handle_ == nullptr) return 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    const DWORD self = GetCurrentProcessId();
    int added = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (entry.th32ParentProcessID != self) continue;
        if (std::towlower(entry.szExeFile[0]) != L'p' || std::towlower(entry.szExeFile[1]) != L'y') continue;
        HANDLE proc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                  entry.th32ProcessID);
        if (proc == nullptr) continue;
        BOOL alreadyIn = FALSE;
        if (IsProcessInJob(proc, static_cast<HANDLE>(handle_), &alreadyIn) && !alreadyIn &&
            AssignProcessToJobObject(static_cast<HANDLE>(handle_), proc))
            ++added;
        CloseHandle(proc);
    }
    CloseHandle(snapshot);
    return added;
}

bool ServiceProcessJob::waitUntilEmpty(int timeoutMs)
{
    if (handle_ == nullptr) return true;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs > 0 ? timeoutMs : 0);
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(static_cast<HANDLE>(handle_), JobObjectBasicAccountingInformation, &accounting,
                                       sizeof(accounting), nullptr))
            return true;
        if (accounting.ActiveProcesses == 0) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(50);
    }
}

void ServiceProcessJob::terminateAll()
{
    if (handle_ != nullptr) TerminateJobObject(static_cast<HANDLE>(handle_), 1);
}

#else

ServiceProcessJob::ServiceProcessJob() = default;
ServiceProcessJob::~ServiceProcessJob() = default;
int ServiceProcessJob::adoptNewPythonChildren() { return 0; }
bool ServiceProcessJob::waitUntilEmpty(int) { return true; }
void ServiceProcessJob::terminateAll() {}

#endif

} // namespace khensu
