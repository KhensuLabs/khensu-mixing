#pragma once

#include <JuceHeader.h>
#include <khensu/midi/MidiUmpEvent.h>
#include <array>
#include <atomic>
#include <optional>
#include <vector>

namespace khensu {

class Midi2DeviceManager final : private juce::ump::Consumer,
                                 private juce::ump::EndpointsListener,
                                 private juce::AsyncUpdater {
  public:
    struct EndpointDescriptor {
        juce::ump::EndpointId id;
        juce::String sourceIdentifier;
        juce::String name;
        midi::SourceProtocol sourceProtocol = midi::SourceProtocol::Unknown;
        bool supportsMidi1 = false;
        bool supportsMidi2 = false;
    };

    class Listener {
      public:
        virtual ~Listener() = default;
        virtual void midiEndpointsChanged() {}
        virtual void midiConnectionChanged() {}
        virtual void midiEventReceived(const midi::MidiUmpEvent&) {}
    };

    Midi2DeviceManager();
    ~Midi2DeviceManager() override;

    const std::vector<EndpointDescriptor>& endpoints() const noexcept { return endpointCache_; }
    juce::String backendName() const;
    bool nativeMidi2BackendAvailable() const noexcept;

    bool connectInput(const juce::String& sourceIdentifier);
    void disconnectInput();
    bool isInputConnected() const noexcept { return input_.isAlive(); }
    const juce::String& connectedSourceIdentifier() const noexcept { return connectedSourceIdentifier_; }
    int droppedEventCount() const noexcept { return droppedEvents_.load(std::memory_order_relaxed); }

    void addListener(Listener& listener) { listeners_.add(&listener); }
    void removeListener(Listener& listener) { listeners_.remove(&listener); }

  private:
    void consume(juce::ump::Iterator begin, juce::ump::Iterator end, double timestampSeconds) override;
    void endpointsChanged() override;
    void handleAsyncUpdate() override;
    void refreshEndpointCache();
    void drainEvents();

    static constexpr int kEventQueueCapacity = 256;

    juce::ump::Endpoints* endpoints_ = nullptr;
    std::optional<juce::ump::Session> session_;
    juce::ump::Input input_;
    std::vector<EndpointDescriptor> endpointCache_;
    juce::String connectedSourceIdentifier_;
    std::atomic<midi::SourceProtocol> connectedSourceProtocol_{midi::SourceProtocol::Unknown};

    juce::AbstractFifo eventFifo_{kEventQueueCapacity};
    std::array<midi::MidiUmpEvent, kEventQueueCapacity> eventQueue_{};
    std::atomic<int> droppedEvents_{0};
    std::atomic<bool> endpointRefreshRequested_{false};
    juce::ListenerList<Listener> listeners_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Midi2DeviceManager)
};

} // namespace khensu
