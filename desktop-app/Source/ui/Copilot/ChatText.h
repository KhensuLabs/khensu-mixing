// desktop-app/Source/ui/Copilot/ChatText.h
#pragma once
#include <JuceHeader.h>
#include <cmath>
#include <vector>
#include <khensu/ipc/HttpServiceBridge.h>

/// Pure text helpers for ChatConsolePanel, unit-tested in
/// tests/ui/ChatTextTest.cpp.
namespace khensu::chat_text {

/// What to tell the user when a chat request fails, by error kind (UIX-19).
/// Every failure used to read "I couldn't reach the local service", even a
/// cloud provider's 401, a 422 for an over-long message or a timeout.
inline juce::String errorReply(const ipc::BridgeError& err)
{
    const juce::String detail = juce::String(err.message).trim();
    switch (err.kind) {
    case ipc::BridgeError::Kind::ServiceUnavailable:
        return TRANS("I couldn't reach the local analysis service. It may still be starting - try again in a moment.");
    case ipc::BridgeError::Kind::Timeout:
        return TRANS("The copilot took too long to answer, so the request was stopped. Try again, or ask a "
                     "shorter question.");
    case ipc::BridgeError::Kind::ParseError:
        return TRANS("The copilot sent a reply I couldn't read. Try again.");
    case ipc::BridgeError::Kind::HttpError:
        break;
    }
    const int status = err.http_status;
    juce::String text;
    // RUN-12: say which gate stopped it - the network switch or the key.
    if (status == 403 && (detail.containsIgnoreCase("online") || detail.containsIgnoreCase("disabled")))
        text = TRANS("Online features are off in Settings, so the cloud provider can't be reached (HTTP %s). "
                     "Turn them on, or choose the local model in Settings > AI providers to chat offline.");
    else if (status == 401 || status == 403)
        text = TRANS("The copilot request was refused (HTTP %s). Check the provider's API key and that "
                     "online features are enabled in Settings.");
    else if (status == 413 || status == 422)
        text = TRANS("The copilot rejected the request as invalid (HTTP %s) - the message or history may be "
                     "too long.");
    else if (status == 429)
        text = TRANS("The copilot is rate-limited right now (HTTP %s). Wait a moment and try again.");
    else if (status == 503)
        text = TRANS("The copilot isn't available (HTTP %s) - an optional component may be missing.");
    else if (status >= 500)
        text = TRANS("The copilot hit an error (HTTP %s). Details are in the service log.");
    else
        text = TRANS("The copilot request failed (HTTP %s).");
    text = text.replace("%s", juce::String(status));
    if (detail.isNotEmpty()) text << "\n" << detail;
    return text;
}

/// The reply for a stream that ended with an SSE error event but still sent
/// its final event (CopilotChatResponse::stream_error). The service sends only
/// a message, so the kind is inferred from it and worded like errorReply().
inline juce::String streamErrorReply(const juce::String& message)
{
    ipc::BridgeError e;
    e.message = message.toStdString();
    const juce::String lower = message.toLowerCase();
    if (lower.contains("timeout") || lower.contains("timed out")) {
        e.kind = ipc::BridgeError::Kind::Timeout;
    } else if (lower.contains("unreachable") || lower.contains("connection")) {
        e.kind = ipc::BridgeError::Kind::ServiceUnavailable;
    } else {
        e.kind = ipc::BridgeError::Kind::HttpError;
        // "HTTP 429", "status 401", "Error code: 503" ... -> the first 3-digit 4xx/5xx number.
        juce::String digits;
        for (int i = 0; i + 2 < lower.length(); ++i) {
            const auto t = lower.substring(i, i + 3);
            const bool boundaryBefore = i == 0 || !juce::CharacterFunctions::isDigit(lower[i - 1]);
            const bool boundaryAfter = i + 3 >= lower.length() || !juce::CharacterFunctions::isDigit(lower[i + 3]);
            if (t.containsOnly("0123456789") && (t[0] == '4' || t[0] == '5') && boundaryBefore && boundaryAfter) {
                digits = t;
                break;
            }
        }
        e.http_status = digits.getIntValue(); // 0 -> "The copilot request failed (HTTP 0)" is avoided below
        if (e.http_status == 0) return TRANS("The copilot's answer was cut off by an error:") + "\n" + message.trim();
    }
    return errorReply(e);
}

/// Parameter-level diff between the applied chain and a proposed one, keyed
/// by POSITION (UIX-19): keying by effect name merged two EQ stages into one
/// and diffed the wrong pair.
inline juce::String stageDiff(const std::vector<ipc::HttpServiceBridge::EffectSpec>& oldChain,
                              const std::vector<ipc::HttpServiceBridge::EffectSpec>& newChain)
{
    juce::String diff;
    const size_t n = std::max(oldChain.size(), newChain.size());
    for (size_t i = 0; i < n; ++i) {
        const juce::String pos = juce::String(static_cast<int>(i) + 1);
        if (i >= oldChain.size()) {
            diff << "  [+] " << TRANS("Stage") << " " << pos << ": " << newChain[i].effect << "\n";
            continue;
        }
        if (i >= newChain.size()) {
            diff << "  [-] " << TRANS("Stage") << " " << pos << ": " << oldChain[i].effect << "\n";
            continue;
        }
        const auto& o = oldChain[i];
        const auto& e = newChain[i];
        if (o.effect != e.effect) {
            diff << "  [~] " << TRANS("Stage") << " " << pos << ": " << o.effect << " -> " << e.effect << "\n";
            continue;
        }
        juce::StringArray paramDiffs;
        for (const auto& [name, val] : e.parameters) {
            const auto it = o.parameters.find(name);
            if (it == o.parameters.end())
                paramDiffs.add(name + " = " + juce::String(val, 2));
            else if (std::abs(it->second - val) > 0.001f)
                paramDiffs.add(name + " (" + juce::String(it->second, 2) + " -> " + juce::String(val, 2) + ")");
        }
        for (const auto& [name, val] : o.parameters)
            if (e.parameters.find(name) == e.parameters.end()) paramDiffs.add(name + " " + TRANS("removed"));
        if (!paramDiffs.isEmpty())
            diff << "  [*] " << TRANS("Stage") << " " << pos << " " << e.effect << ": "
                 << paramDiffs.joinIntoString(", ") << "\n";
    }
    return diff;
}

} // namespace khensu::chat_text
