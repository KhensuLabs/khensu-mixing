// desktop-app/Source/midi/MidiCiResponder.h
// STU-13: a minimal MIDI-CI responder for the control-surface mappings. With
// it on, a MIDI-CI initiator (a DAW, a controller editor, a MIDI 2.0 test
// tool) on the connected MIDI endpoint discovers the app and reads, through
// Property Exchange:
//   ResourceList          one entry per mappable control
//   X-Macro-<name>        read-only JSON: the control's current value, its
//                         range and the MIDI / OSC control mapped to it
// Nothing can be set and nothing is subscribed to (status 405 / refused): the
// mapping itself still happens in the MIDI / OSC macro mapping window. Off by
// default (Settings); the MIDI 1.0 / UMP control mapping is unaffected either
// way - this only adds SysEx handling beside it (Midi2DeviceManager).
// Built on JUCE 8's juce_midi_ci (juce::midi_ci::Device). The manufacturer
// id is 0x7D, the MIDI Association's id for non-commercial use.
// THREAD_CONTEXT(UIThread) - juce::midi_ci::Device is used on the message thread only.
#pragma once
#include <JuceHeader.h>
#include <juce_midi_ci/juce_midi_ci.h>
#include <functional>
#include <memory>
#include "Midi2DeviceManager.h"
#include "MacroMappingEngine.h"

namespace khensu {

class MidiCiResponder final : private Midi2DeviceManager::Listener,
                              private juce::midi_ci::PropertyDelegate,
                              private juce::midi_ci::DeviceMessageHandler {
  public:
    /// The current value of a control (a vocal macro), by name.
    using ValueOf = std::function<double(const juce::String& target)>;
    /// Where outgoing CI messages go; defaults to the MIDI endpoint.
    using Sender = std::function<void(juce::ump::BytesOnGroup)>;

    /// `midi` may be null (tests feed receive() and set a sender).
    MidiCiResponder(Midi2DeviceManager* midi, const MacroMappingEngine& engine, ValueOf valueOf);
    ~MidiCiResponder() override;

    void setEnabled(bool enabled);
    bool isEnabled() const noexcept { return device_ != nullptr; }

    /// Feeds one incoming MIDI-CI message (the SysEx body, no F0 / F7).
    void receive(juce::ump::BytesOnGroup message);
    void setSender(Sender sender) { sender_ = std::move(sender); }
    /// The device's MUID (0 when off).
    std::uint32_t muid() const noexcept;

    /// The Property Exchange resource for a control: "X-Macro-" + the name
    /// with anything but letters, digits and '-' dropped.
    static juce::String resourceNameFor(const juce::String& target);
    /// The ResourceList body.
    static juce::var resourceList(const juce::StringArray& targets);
    /// One control's property body.
    static juce::var propertyBody(const juce::String& target, double value, const MacroMappingEngine::Mapping* mapping);

  private:
    void midiSysExReceived(const midi::SysExMessage& message) override;
    void processMessage(juce::ump::BytesOnGroup message) override;

    juce::midi_ci::PropertyReplyData propertyGetDataRequested(juce::midi_ci::MUID,
                                                              const juce::midi_ci::PropertyRequestHeader&) override;
    juce::midi_ci::PropertyReplyHeader propertySetDataRequested(juce::midi_ci::MUID,
                                                                const juce::midi_ci::PropertyRequestData&) override;
    bool subscriptionStartRequested(juce::midi_ci::MUID, const juce::midi_ci::PropertySubscriptionHeader&) override
    {
        return false;
    }
    void subscriptionDidStart(juce::midi_ci::MUID, const juce::String&,
                              const juce::midi_ci::PropertySubscriptionHeader&) override
    {
    }
    void subscriptionWillEnd(juce::midi_ci::MUID, const juce::midi_ci::Subscription&) override {}

    Midi2DeviceManager* midi_;
    const MacroMappingEngine& engine_;
    ValueOf valueOf_;
    Sender sender_;
    std::unique_ptr<juce::midi_ci::Device> device_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MidiCiResponder)
};

} // namespace khensu
