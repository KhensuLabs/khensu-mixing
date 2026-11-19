// Deliberately free of JuceHeader.h: <windows.h> is confined to this TU so
// its macros can't collide with JUCE names elsewhere (as ServiceProcessJob.cpp).
#include "PortListeners.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace khensu {

std::vector<std::uint32_t> tcpListenerPids(int port)
{
    std::vector<std::uint32_t> pids;
#if defined(_WIN32)
    // The table can grow between the size query and the read: retry a few times.
    std::vector<unsigned char> buffer;
    DWORD size = 0;
    DWORD rc = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        buffer.resize(size);
        rc = GetExtendedTcpTable(buffer.empty() ? nullptr : buffer.data(), &size, FALSE, AF_INET,
                                 TCP_TABLE_OWNER_PID_LISTENER, 0);
    }
    if (rc != NO_ERROR) return pids;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
    const auto wanted = htons(static_cast<u_short>(port));
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (static_cast<u_short>(row.dwLocalPort) == wanted) pids.push_back(row.dwOwningPid);
    }
#else
    (void)port;
#endif
    return pids;
}

std::uint32_t currentProcessId() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return 0;
#endif
}

} // namespace khensu
