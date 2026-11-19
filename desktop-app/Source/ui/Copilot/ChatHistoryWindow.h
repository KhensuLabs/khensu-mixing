// desktop-app/Source/ui/Copilot/ChatHistoryWindow.h
// Sliding window over the copilot chat history (AUDIT-043). The service's
// ChatRequest caps `history` at 200 messages and each message at 20 000
// characters; the panel used to send and persist the whole, ever-growing
// conversation, so from the 201st message on every /chat/stream returned 422
// forever (and the persisted history reloaded straight back into that state).
// Header-only so the trimming rule is unit-testable without the panel.
// THREAD_CONTEXT(AnyThread) - pure.
#pragma once
#include <JuceHeader.h>
#include <vector>

namespace khensu::chat_history {

/// Messages kept, sent and persisted - well under the service's 200 cap.
inline constexpr size_t kMaxMessages = 60;
/// Per-message ceiling mirroring the service's ChatMessage max_length.
inline constexpr int kMaxMessageChars = 20000;
/// Rough input budget for the whole window (~4 chars per token -> ~24k
/// tokens), so a few very long replies can't blow a provider's context.
inline constexpr int kMaxTotalChars = 96000;

/// Drops the OLDEST turns until `turns` fits `maxMessages` and `maxTotalChars`
/// (each message counted at most kMaxMessageChars, the length that is sent),
/// then drops a leading assistant turn so the window always opens on a user
/// message. `Turn` needs `role` and `content` juce::String members.
template <typename Turn>
void trimToWindow(std::vector<Turn>& turns, size_t maxMessages = kMaxMessages, int maxTotalChars = kMaxTotalChars)
{
    const auto sentLength = [](const Turn& t) { return juce::jmin(t.content.length(), kMaxMessageChars); };
    long total = 0;
    for (const auto& t : turns)
        total += sentLength(t);

    size_t drop = 0;
    while (drop < turns.size() && (turns.size() - drop > maxMessages || total > maxTotalChars)) {
        total -= sentLength(turns[drop]);
        ++drop;
    }
    while (drop < turns.size() && turns[drop].role != "user")
        ++drop;
    if (drop > 0) turns.erase(turns.begin(), turns.begin() + static_cast<std::ptrdiff_t>(drop));
}

/// The content as it may be sent (clipped to the service's per-message cap).
inline juce::String clipForSend(const juce::String& content)
{
    return content.length() <= kMaxMessageChars ? content : content.substring(0, kMaxMessageChars);
}

} // namespace khensu::chat_history
