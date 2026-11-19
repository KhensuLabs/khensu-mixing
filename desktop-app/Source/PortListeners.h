#pragma once
#include <cstdint>
#include <vector>

namespace khensu {

/// PIDs of every process listening on TCP `port` (IPv4, any local address).
/// Empty when nothing listens or the table can't be read; always empty on
/// non-Windows builds.
std::vector<std::uint32_t> tcpListenerPids(int port);

/// This process's PID (0 on non-Windows builds).
std::uint32_t currentProcessId() noexcept;

} // namespace khensu
