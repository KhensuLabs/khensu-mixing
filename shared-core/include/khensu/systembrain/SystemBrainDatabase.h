#pragma once
#include <khensu/systembrain/FeatureEntry.h>
#include <khensu/common/Result.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace khensu {

/// Loads and queries the System Brain feature encyclopedia from
/// feature_encyclopedia.json.
/// THREAD_CONTEXT(WorkerThread) for load*; THREAD_CONTEXT(AnyThread) for queries after load.
class SystemBrainDatabase {
  public:
    SystemBrainDatabase() = default;

    Result<void> loadFromString(const std::string& jsonContent);
    Result<void> loadFromFile(const std::string& filePath);

    std::optional<FeatureEntry> findById(const std::string& id) const;
    const std::vector<FeatureEntry>& listAll() const noexcept { return m_ordered; }

    /// Case-insensitive substring match over name, short_description, category, and tags.
    /// An empty query returns all entries (in insertion order).
    std::vector<FeatureEntry> search(const std::string& query) const;

    /// Case-insensitive exact category match.
    std::vector<FeatureEntry> filterByCategory(const std::string& category) const;

    /// Unique category names, in first-seen order.
    std::vector<std::string> categories() const;

    size_t size() const noexcept { return m_byId.size(); }
    bool isLoaded() const noexcept { return m_loaded; }

  private:
    std::unordered_map<std::string, FeatureEntry> m_byId;
    std::vector<FeatureEntry> m_ordered;
    bool m_loaded = false;

    Result<void> parseJson(const std::string& jsonContent);
};

} // namespace khensu
