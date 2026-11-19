#pragma once
// THREAD_CONTEXT(WorkerThread|UIThread) -- NOT AudioThread safe.
#include <khensu/common/Result.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace khensu {

/// POD returned by FeedbackManager::listOverrides().
struct ParameterOverride {
    std::string recommendation_id;
    std::string parameter_name;
    float recommended_value = 0.0f;
    float actual_value = 0.0f;
    std::string created_at;
};

/// One recommendation's stored feedback row (FeedbackManager::getFeedbackBatch).
struct FeedbackRow {
    bool acted_on = false;
    std::string note; ///< free text; the issue queue writes "dismissed[: reason]" (CON-1)
};

/// One accept/reject or A/B pick that moved a learned taste offset
/// (preference_learning_events, migration 012).
struct PreferenceLearningEvent {
    std::string pref_key;          // e.g. "pref_lufs_offset"
    std::string source;            // "recommendation" | "ab_pick"
    std::string recommendation_id; // empty for an A/B pick
    bool accepted = false;         // accepted / preferred the processed render
    float value_before = 0.0f;
    float value_after = 0.0f;
    std::string created_at;        // ISO-8601
};

/// Records and retrieves engineer feedback on recommendations.
/// Backed by a local SQLite database (WAL mode, NORMAL sync).
///
/// Tables:
///   recommendation_feedback  - acted_on / note per recommendation
///   user_preferences         - key-value float preferences
///   parameter_overrides      - when engineers deviate from a recommendation's
///                              suggested parameter value (#25 audit telemetry)
///   preference_learning_events - why each learned taste offset moved
class FeedbackManager {
  public:
    /// How the store is opened (CORE-16).
    enum class OpenMode {
        /// Desktop app (default): create the file if needed, switch to WAL,
        /// create the feedback tables and run the one-time legacy import.
        Provision,
        /// Guest process that also WRITES feedback (the VST3 plugin's acted-on
        /// and EQ-override rows): opens an EXISTING khensu.db read-write with no
        /// DDL, no journal-mode change and no legacy import. A missing database
        /// or missing tables never throw - every call returns an error instead.
        NoProvision,
        /// Same as NoProvision, but the connection is read-only (SQLite
        /// OPEN_READONLY): reads work, writes return an error.
        ReadOnlyNoProvision,
    };

    /// Provision mode may throw (as before) when the database cannot be
    /// created; the NoProvision modes never throw - check isAvailable().
    explicit FeedbackManager(const std::string& db_path, OpenMode mode = OpenMode::Provision);
    ~FeedbackManager();

    /// False when a NoProvision open failed (no database yet, locked, or
    /// migrated by a newer build - FIX-18): every call then fails soft with
    /// an error result.
    bool isAvailable() const noexcept;

    /// Record that the engineer acted (or chose not to act) on a recommendation.
    /// Upserts: a second call for the same recommendation_id updates the existing row.
    Result<void> recordActedOn(const std::string& recommendation_id, bool acted_on, const std::string& note = "");

    /// Query whether the engineer acted on a recommendation.
    /// Returns err if no record exists for the given recommendation_id.
    Result<bool> getActedOn(const std::string& recommendation_id);

    /// Batch equivalent of getActedOn() - one query instead of N, for UI
    /// lists that need acted-on state for every visible recommendation at
    /// once (e.g. SuggestionListPanel). Missing ids are simply absent from
    /// the returned map (caller treats "not found" as not-acted-on).
    std::map<std::string, bool> getActedOnBatch(const std::vector<std::string>& recommendation_ids);

    /// getActedOnBatch() with each row's note, so a caller can tell a
    /// dismissed finding (acted_on false + a "dismissed" note) from one never
    /// touched (no row) - the issue queue's states (CON-1).
    std::map<std::string, FeedbackRow> getFeedbackBatch(const std::vector<std::string>& recommendation_ids);

    /// Set a key-value user preference (e.g. "lufs_offset", 0.5f).
    Result<void> setPreference(const std::string& key, float val);

    /// Get a key-value user preference. Returns defaultVal if key is not found.
    Result<float> getPreference(const std::string& key, float defaultVal);

    // ---- #25 audit: parameter override telemetry ----

    /// Record that the engineer applied a parameter value different from the
    /// recommended value. Call this from the UI whenever an engineer drags a
    /// slider away from the suggested position after viewing a recommendation.
    Result<void> recordOverride(const std::string& recommendation_id, const std::string& parameter_name,
                                float recommended_value, float actual_value);

    /// Return all overrides recorded for a recommendation, newest first.
    Result<std::vector<ParameterOverride>> listOverrides(const std::string& recommendation_id);

    // ---- Learned taste evidence (What Khensu has learned) ----

    /// Log one action that moved a learned offset. Callers log only when the
    /// offset actually changed (see LearnedPreferences::learnFromRecommendation).
    Result<void> recordLearningEvent(const PreferenceLearningEvent& event);

    /// Every logged learning event, oldest first.
    Result<std::vector<PreferenceLearningEvent>> listLearningEvents();

    /// Forget one learned offset: deletes its recommendation_prefs row (so it
    /// reads back as the neutral default) and its evidence rows. Touches
    /// nothing else - UI state (sort order, stem positions), score weights and
    /// session-scoped keys are not learned and stay as they are.
    Result<void> resetLearnedPreference(const std::string& pref_key);

    /// resetLearnedPreference() for every key in `pref_keys`, in one transaction.
    Result<void> resetLearnedPreferences(const std::vector<std::string>& pref_keys);

    /// Perform a WAL checkpoint. If the WAL file size exceeds 10MB (or if force is true),
    /// commits WAL frames to the main database file and truncates the -wal file to zero
    /// (TRUNCATE mode: waits up to the busy timeout for readers, so call off the UI thread).
    Result<void> checkpoint(bool force = false);

    /// Erases every row in every table (recommendation_feedback,
    /// user_preferences, parameter_overrides) - the feedback.db half of
    /// "Delete all Khensu data" (audit DATA-02). Irreversible; the caller is
    /// responsible for confirming with the user first.
    Result<void> deleteAll();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace khensu
