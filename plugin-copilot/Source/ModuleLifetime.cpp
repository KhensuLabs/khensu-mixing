// Kept in its own translation unit: it needs the raw platform headers, which
// must not leak their macros into JUCE-heavy sources.
#include "ModuleLifetime.h"

#include <atomic>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace khensu {

namespace {
// Any object with static storage in this module identifies the module.
const char kModuleAnchor = 0;
std::atomic<bool> pinned{false};
} // namespace

void pinPluginModuleForProcessLifetime() noexcept
{
    if (pinned.exchange(true)) return;
#if defined(_WIN32)
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&kModuleAnchor), &module);
#else
    Dl_info info{};
    if (dladdr(&kModuleAnchor, &info) != 0 && info.dli_fname != nullptr)
        (void) dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
#endif
}

} // namespace khensu
