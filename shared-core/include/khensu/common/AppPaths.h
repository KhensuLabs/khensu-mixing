#pragma once
#include <string>

namespace khensu {

/// Resolves platform-appropriate application data locations.
/// THREAD_CONTEXT(AnyThread) - pure path resolution + directory creation.
struct AppPaths {
    /// <userApplicationDataDirectory>/KhensuMixing, created if absent.
    static std::string dataDirectory();

    /// <dataDirectory>/khensu.db
    static std::string databaseFile();
};

} // namespace khensu
