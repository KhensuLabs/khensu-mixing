#include <khensu/ipc/HttpConnectionLimit.h>
#include <juce_core/juce_core.h>

#include <mutex>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")
#endif

namespace khensu::ipc {

void ensureHttpConnectionLimit()
{
#if JUCE_WINDOWS
    static std::once_flag once;
    std::call_once(once, [] {
        // Process-wide (a null handle): applies to every connection opened after it.
        // KHENSU_HTTP_CONNECTIONS overrides it (1..64) - to measure, not to tune.
        DWORD perServer = kHttpConnectionsPerServer;
        const auto env = juce::SystemStats::getEnvironmentVariable("KHENSU_HTTP_CONNECTIONS", {});
        if (env.isNotEmpty()) perServer = static_cast<DWORD>(juce::jlimit(1, 64, env.getIntValue()));
        InternetSetOptionW(nullptr, INTERNET_OPTION_MAX_CONNS_PER_SERVER, &perServer, sizeof(perServer));
        DWORD perServer10 = perServer;
        InternetSetOptionW(nullptr, INTERNET_OPTION_MAX_CONNS_PER_1_0_SERVER, &perServer10, sizeof(perServer10));
    });
#endif
}

} // namespace khensu::ipc
