#pragma once

namespace khensu::ipc {

/// Raises WinINet's per-server connection limit for this process, once
/// (no-op off Windows). juce::URL uses WinINet on Windows, whose default is 2
/// simultaneous HTTP/1.1 connections per server: with the app's background
/// pools running several long service requests at once, every other request
/// - the 2.5 s health probe included - queued behind them, the probe missed
/// three times and the identity gate closed on a healthy service ("render
/// status unknown", 2026-11-18 soak test). Call before opening a connection;
/// every bridge request and health probe does.
void ensureHttpConnectionLimit();

/// The limit set (per HTTP/1.1 and HTTP/1.0 server).
constexpr unsigned long kHttpConnectionsPerServer = 32;

} // namespace khensu::ipc
