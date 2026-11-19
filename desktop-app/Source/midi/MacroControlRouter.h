// desktop-app/Source/midi/MacroControlRouter.h
// Connects hardware controllers and OSC to the vocal macros. Listens to the
// app's Midi2DeviceManager (CC, note velocity, pitch bend) and, when enabled,
// an OSC UDP receiver; feeds MacroMappingEngine; and writes the resulting
// macro values through AppController::setVocalMacroValue (which updates the
// sliders via EventHub). Mappings and OSC settings persist as a user
// preference. OSC is OFF by default; when on it listens on loopback only
// unless the user explicitly allows other devices on the network.
// THREAD_CONTEXT(UIThread)
#pragma once
#include <JuceHeader.h>
#include <juce_osc/juce_osc.h>
#include "Midi2DeviceManager.h"
#include "MacroMappingEngine.h"

namespace khensu {

class IAppController;

class MacroControlRouter final : private Midi2DeviceManager::Listener,
                                 private juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback> {
  public:
    static constexpr const char* kPrefKey = "macro_mappings";
    static constexpr int kDefaultOscPort = 9031;

    MacroControlRouter(IAppController& controller, Midi2DeviceManager& midi);
    ~MacroControlRouter() override;

    MacroMappingEngine& engine() noexcept { return engine_; }

    /// Fires on the message thread after a learn binds, a mapped control
    /// moves, or settings load - so a settings panel can refresh.
    std::function<void()> onChange;

    /// Opens the OSC receiver. Bound to 127.0.0.1 unless `allowNetwork`
    /// (OSC is unauthenticated - LAN exposure is an explicit opt-in).
    bool enableOsc(int port, bool allowNetwork = false);
    void disableOsc();
    bool oscEnabled() const noexcept { return oscPort_ > 0; }
    int oscPort() const noexcept { return oscPort_; }
    bool oscAllowsNetwork() const noexcept { return oscNetwork_; }

    void save();
    void load();

    /// Entry point for decoded MIDI (also used by tests/simulation).
    void handleMidi(const midi::MidiUmpEvent& e);

  private:
    void midiEventReceived(const midi::MidiUmpEvent& e) override;
    void oscMessageReceived(const juce::OSCMessage& message) override;
    void apply(const std::vector<MacroMappingEngine::Output>& outputs);

    IAppController& controller_;
    Midi2DeviceManager& midi_;
    MacroMappingEngine engine_;
    juce::OSCReceiver osc_;
    std::unique_ptr<juce::DatagramSocket> oscSocket_; ///< outlives the receiver's use of it
    int oscPort_ = 0;
    bool oscNetwork_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MacroControlRouter)
};

} // namespace khensu
