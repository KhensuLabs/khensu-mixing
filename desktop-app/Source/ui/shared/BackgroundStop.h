// desktop-app/Source/ui/shared/BackgroundStop.h
#pragma once
#include <JuceHeader.h>

namespace khensu {

/// True when the current background job should stop: the app is shutting
/// down (the owning pool's removeAllJobs(true, ...) signals every running
/// job) or the thread itself was asked to exit.
///
/// Long polling loops MUST check this. juce::Thread::currentThreadShouldExit()
/// alone is not enough on a ThreadPool: removeAllJobs() sets the *job's* exit
/// flag, and the thread's own flag is only raised later, right before the
/// pool force-kills threads that didn't return.
inline bool backgroundShouldStop() noexcept
{
    if (juce::Thread::currentThreadShouldExit()) return true;
    if (auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob()) return job->shouldExit();
    return false;
}

/// Sleep in short slices, returning false early if the job should stop.
inline bool backgroundSleep(int milliseconds) noexcept
{
    for (int waited = 0; waited < milliseconds; waited += 50) {
        if (backgroundShouldStop()) return false;
        juce::Thread::sleep(juce::jmin(50, milliseconds - waited));
    }
    return !backgroundShouldStop();
}

} // namespace khensu
