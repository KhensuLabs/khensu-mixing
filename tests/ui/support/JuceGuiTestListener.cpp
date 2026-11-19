// tests/ui/support/JuceGuiTestListener.cpp
// Brings the JUCE GUI subsystem up on the main thread for the whole KhensuTests
// run, the way the real app does before any component exists.
//
// Without it, the first MessageManager::callAsync() from a worker thread (any
// runInBackground job) lazily created the MessageManager on that worker, which
// then counted as "the message thread": every later component call from the
// test thread tripped JUCE_ASSERT_MESSAGE_MANAGER_IS_LOCKED, timers and
// AsyncUpdaters asserted that no message manager existed, and JUCE's
// singletons leaked at exit. Release builds compile those checks out, so the
// noise only showed in a Debug run (2026-10-16) - where it would also hide a
// real assertion. Tests that create their own ScopedJuceInitialiser_GUI still
// work: the initialiser is reference-counted.
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <JuceHeader.h>

#include <memory>

namespace {

class JuceGuiTestListener final : public Catch::EventListenerBase {
  public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        juce_ = std::make_unique<juce::ScopedJuceInitialiser_GUI>();
    }

    void testRunEnded(const Catch::TestRunStats&) override { juce_.reset(); }

  private:
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juce_;
};

} // namespace

CATCH_REGISTER_LISTENER(JuceGuiTestListener)
