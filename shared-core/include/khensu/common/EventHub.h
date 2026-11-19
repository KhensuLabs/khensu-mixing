#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <juce_core/juce_core.h>

namespace khensu {

enum class EventType { VocalStrategyChanged, VocalMacroChanged, AnalysisCompleted, VocalAnalysisCompleted };

struct EventData {
    EventType type;
    juce::String stringVal;
    float floatVal = 0.0f;
    std::map<juce::String, float> mapVal;
};

/// Process-wide pub/sub hub.
///
/// Thread-safe: the listener table is guarded by a mutex, and publish()
/// iterates a SNAPSHOT taken under that lock, then invokes callbacks with the
/// lock released - so a callback may add or remove listeners (including
/// itself) without invalidating the iteration or deadlocking (AUDIT-088: the
/// table used to be iterated unguarded while add/remove mutated it).
/// A listener removed while a publish is in flight is not called after the
/// removal is observed: every entry carries an `active` flag that
/// removeListener() clears before erasing it.
class EventHub {
  public:
    using ListenerCallback = std::function<void(const EventData&)>;

    static EventHub& getInstance()
    {
        static EventHub instance;
        return instance;
    }

    void addListener(void* key, EventType type, ListenerCallback callback)
    {
        auto entry = std::make_shared<ListenerEntry>();
        entry->key = key;
        entry->callback = std::move(callback);
        const std::lock_guard<std::mutex> lock(mutex_);
        listeners_[type].push_back(std::move(entry));
    }

    void removeListener(void* key)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [type, list] : listeners_) {
            list.erase(std::remove_if(list.begin(), list.end(),
                                      [key](const std::shared_ptr<ListenerEntry>& entry) {
                                          if (entry->key != key) return false;
                                          entry->active.store(false, std::memory_order_release);
                                          return true;
                                      }),
                       list.end());
        }
    }

    void publish(const EventData& data)
    {
        std::vector<std::shared_ptr<ListenerEntry>> snapshot;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            auto it = listeners_.find(data.type);
            if (it == listeners_.end()) return;
            snapshot = it->second;
        }
        for (const auto& entry : snapshot) {
            if (!entry->active.load(std::memory_order_acquire)) continue; // removed mid-publish
            if (entry->callback) entry->callback(data);
        }
    }

  private:
    EventHub() = default;
    ~EventHub() = default;

    struct ListenerEntry {
        void* key = nullptr;
        ListenerCallback callback;
        std::atomic<bool> active{true};
    };

    std::mutex mutex_;
    std::map<EventType, std::vector<std::shared_ptr<ListenerEntry>>> listeners_;

    JUCE_DECLARE_NON_COPYABLE(EventHub)
};

} // namespace khensu
