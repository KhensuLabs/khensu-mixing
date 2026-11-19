#pragma once

#include <JuceHeader.h>
#include "../timeline/IssueTimeline.h"
#include <cstdint>
#include <vector>

namespace khensu::ara {

/// What the processor and editor need from the ARA document controller
/// (CE-DAW-1), behind an interface so the shared plugin sources compile
/// unchanged in the non-ARA KhensuCopilot target, where it is never
/// implemented and KhensuCopilotProcessor::getAraSession() returns nullptr.
class IAraSession {
  public:
    virtual ~IAraSession() = default;

    /// THREAD_CONTEXT(UIThread). Findings of every analysed audio source,
    /// mapped onto each of its playback regions (host timeline seconds).
    virtual std::vector<timeline::TimedIssue> regionIssues() = 0;
    /// THREAD_CONTEXT(UIThread). Asks the host to move its playhead via the
    /// ARA playback controller. False when the host provides none.
    virtual bool requestPlaybackPosition(double timeSec) = 0;
    /// THREAD_CONTEXT(UIThread). One line for the editor ("2 sources
    /// analysed, 1 pending", or why analysis cannot run).
    virtual juce::String statusText() = 0;
    /// Moves whenever regionIssues() would return something different.
    virtual uint64_t generation() const noexcept = 0;
};

} // namespace khensu::ara
