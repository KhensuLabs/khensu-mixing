#pragma once
#include <khensu/targets/GenreEntry.h>
#include <khensu/common/Result.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace khensu {

/// Loads and queries the genre norms database from genre_targets.json.
/// Thread-safety: load once on a WorkerThread, then read from any thread.
/// THREAD_CONTEXT(WorkerThread) for load*; THREAD_CONTEXT(AnyThread) for queries after load.
class GenreDatabase {
  public:
    GenreDatabase() = default;

    /// Load from a JSON string. Used in tests and for embedded resources.
    Result<void> loadFromString(const std::string& jsonContent);

    /// Load from a file path on disk.
    Result<void> loadFromFile(const std::string& filePath);

    /// Returns the genre entry with the given id (e.g. "trap"), or nullopt.
    std::optional<GenreEntry> findById(const std::string& id) const;

    /// Case-insensitive name search (e.g. "Trap" or "trap").
    std::optional<GenreEntry> findByName(const std::string& name) const;

    /// All loaded genres in insertion order.
    const std::vector<GenreEntry>& listAll() const noexcept { return m_ordered; }

    size_t size() const noexcept { return m_byId.size(); }
    bool isLoaded() const noexcept { return m_loaded; }

  private:
    std::unordered_map<std::string, GenreEntry> m_byId;
    std::unordered_map<std::string, GenreEntry> m_byNameLower;
    std::vector<GenreEntry> m_ordered;
    bool m_loaded = false;

    Result<void> parseJson(const std::string& jsonContent);
};

} // namespace khensu
