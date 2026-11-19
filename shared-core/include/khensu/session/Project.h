#pragma once
#include <string>
#include <vector>

namespace khensu {

/// A project or album group (FEAT-6): an ordered set of sessions, one per
/// track (storage/schema/016_projects.sql). `session_ids` is the track order.
struct Project {
    std::string id;
    std::string name;
    std::string kind = "album"; ///< "album" | "ep" | "project"
    std::string created_at;     ///< ISO 8601 UTC
    std::string updated_at;
    std::vector<std::string> session_ids;

    bool is_valid() const noexcept { return !id.empty() && !name.empty(); }
};

} // namespace khensu
