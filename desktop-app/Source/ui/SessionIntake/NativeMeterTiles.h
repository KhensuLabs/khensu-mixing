// desktop-app/Source/ui/SessionIntake/NativeMeterTiles.h
#pragma once
#include <JuceHeader.h>
#include <khensu/analysis/NativeMeters.h>

#include <atomic>
#include <functional>
#include <memory>

namespace khensu {

/// CE-PLT-3: the four headline tiles (integrated LUFS, true peak, LRA,
/// stereo width) measured natively the moment a file is dropped on Intake -
/// before the analysis service has answered, and even while it is still
/// starting. The numbers come from khensu::native (shared-core), which is
/// pinned to the service's own meters by a parity test, so they never
/// disagree with what the full analysis later reports.
///
/// Colours are read live in paint() from KhensuTheme, so theme switches
/// need no listener (nothing is cached).
// THREAD_CONTEXT(khensu::ThreadContext::UIThread)
class NativeMeterTiles final : public juce::Component {
  public:
    using BackgroundRunner = std::function<void(std::function<void()>)>;
    explicit NativeMeterTiles(BackgroundRunner runInBackground);
    ~NativeMeterTiles() override;

    /// Decode `file` on a background worker and measure it. A newer call
    /// supersedes an older one (its result is dropped).
    void measure(const juce::File& file);
    void clear();

    /// Longest file measured natively (decoding is in memory). Longer files
    /// show a note and leave the numbers to the service.
    static constexpr double kMaxSeconds = 20.0 * 60.0;

    void paint(juce::Graphics& g) override;

    /// Formatting shared with tests: "-14.2", "--" for an undefined value.
    static juce::String formatValue(double v, int decimals);

  private:
    enum class State { Idle, Measuring, Ready, Failed };
    struct Shared {
        std::atomic<int> generation{0};
    };

    BackgroundRunner runInBackground_;
    std::shared_ptr<Shared> shared_ = std::make_shared<Shared>();
    State state_ = State::Idle;
    juce::String note_;
    native::NativeReport report_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NativeMeterTiles)
};

} // namespace khensu
