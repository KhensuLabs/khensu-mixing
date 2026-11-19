// desktop-app/Source/session/KeyedListeners.h
#pragma once
#include <vector>

namespace khensu {

/// Calls every callback in a keyed listener map (`std::map<void*,
/// std::function<...>>`, the pattern every service and model uses) that is
/// still registered when its turn comes.
///
/// A listener may add or remove listeners - itself included - from inside its
/// callback (a panel that rebuilds, a component destroyed in response).
/// Iterating the live map then invalidates the iterator, and iterating a copy
/// calls a listener that an earlier callback already removed, whose owner may
/// be gone. So the keys are taken up front and each one is looked up again
/// before its call; the callback itself is copied, because erasing its entry
/// would otherwise destroy the std::function while it runs. Listeners added
/// during the pass are first called on the next one.
template <typename Map, typename... Args> void notifyKeyedListeners(const Map& listeners, const Args&... args)
{
    std::vector<typename Map::key_type> keys;
    keys.reserve(listeners.size());
    for (const auto& entry : listeners)
        keys.push_back(entry.first);
    for (const auto& key : keys) {
        const auto it = listeners.find(key);
        if (it == listeners.end() || !it->second) continue;
        const auto callback = it->second;
        callback(args...);
    }
}

} // namespace khensu
