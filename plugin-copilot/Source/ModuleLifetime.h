#pragma once

namespace khensu {

/// Pins the binary that contains the plugin code (the .vst3 module, or the
/// test executable) so the host can never unmap it for the rest of the
/// process. Last-resort safety valve used only when a cancelled background
/// worker overran its shutdown budget: leaking the loaded module is harmless,
/// executing unmapped code is a host crash. Idempotent; never throws.
void pinPluginModuleForProcessLifetime() noexcept;

} // namespace khensu
