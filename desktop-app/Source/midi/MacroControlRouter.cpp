// desktop-app/Source/midi/MacroControlRouter.cpp
#include "MacroControlRouter.h"
#include "../IAppController.h"

namespace khensu {

namespace {
juce::StringArray vocalMacroTargets()
{
    // Must match VocalMacroPanel::kMacroNames.
    return {"Compression", "Presence", "Width", "De-essing", "Saturation", "Reverb", "Delay"};
}
} // namespace

MacroControlRouter::MacroControlRouter(IAppController& controller, Midi2DeviceManager& midi)
    : controller_(controller), midi_(midi), engine_(vocalMacroTargets()),
      ci_(&midi, engine_, [this](const juce::String& target) {
          return static_cast<double>(controller_.vocalEdit().getVocalMacroValue(target));
      })
{
    midi_.addListener(*this);
    osc_.addListener(this);
    load();
}

MacroControlRouter::~MacroControlRouter()
{
    osc_.removeListener(this);
    disableOsc();
    midi_.removeListener(*this);
}

bool MacroControlRouter::enableOsc(int port, bool allowNetwork)
{
    disableOsc();
    if (port < 1024 || port > 65535) return false;
    // Loopback-only unless the user explicitly allows other devices: OSC has
    // no authentication, so a LAN-reachable port lets anyone on the network
    // move the macros.
    auto socket = std::make_unique<juce::DatagramSocket>(false);
    if (!socket->bindToPort(port, allowNetwork ? juce::String() : juce::String("127.0.0.1"))) return false;
    if (!osc_.connectToSocket(*socket)) return false;
    oscSocket_ = std::move(socket);
    oscPort_ = port;
    oscNetwork_ = allowNetwork;
    return true;
}

void MacroControlRouter::disableOsc()
{
    osc_.disconnect();
    oscSocket_.reset();
    oscPort_ = 0;
    oscNetwork_ = false;
}

void MacroControlRouter::midiEventReceived(const midi::MidiUmpEvent& e) { handleMidi(e); }

void MacroControlRouter::handleMidi(const midi::MidiUmpEvent& e)
{
    MacroMappingEngine::Source s;
    s.channel = e.channel;
    double norm = 0.0;
    switch (e.kind) {
    case midi::EventKind::ControlChange:
        s.type = MacroMappingEngine::SourceType::MidiCC;
        s.number = e.index;
        norm = MacroMappingEngine::normaliseMidi(e.value, e.valueBits);
        break;
    case midi::EventKind::NoteOn:
        s.type = MacroMappingEngine::SourceType::MidiNote;
        s.number = e.index;
        norm = MacroMappingEngine::normaliseMidi(e.value, e.valueBits);
        if (norm <= 0.0) return; // MIDI 1.0 note-on velocity 0 is a note-off
        break;
    case midi::EventKind::PitchBend:
        s.type = MacroMappingEngine::SourceType::MidiPitchBend;
        norm = MacroMappingEngine::normaliseMidi(e.value, e.valueBits == 0 ? 14 : e.valueBits);
        break;
    default:
        return;
    }
    apply(engine_.handle(s, norm));
}

void MacroControlRouter::oscMessageReceived(const juce::OSCMessage& message)
{
    if (message.isEmpty()) return;
    const auto& arg = message[0];
    juce::var v;
    if (arg.isFloat32())
        v = static_cast<double>(arg.getFloat32());
    else if (arg.isInt32())
        v = arg.getInt32();
    else
        return;
    MacroMappingEngine::Source s;
    s.type = MacroMappingEngine::SourceType::Osc;
    s.oscAddress = message.getAddressPattern().toString();
    apply(engine_.handle(s, MacroMappingEngine::normaliseOsc(v)));
}

void MacroControlRouter::apply(const std::vector<MacroMappingEngine::Output>& outputs)
{
    if (outputs.empty()) return;
    for (const auto& o : outputs)
        controller_.vocalEdit().setVocalMacroValue(o.target, static_cast<float>(o.value));
    if (onChange) onChange();
}

void MacroControlRouter::save()
{
    auto* root = new juce::DynamicObject();
    root->setProperty("mappings", engine_.toVar());
    root->setProperty("osc_port", oscPort_);
    root->setProperty("osc_allow_network", oscNetwork_);
    root->setProperty("midi_ci", ci_.isEnabled());
    controller_.sessions().sessionManager().setUserPreference(kPrefKey, juce::JSON::toString(juce::var(root), true).toStdString());
}

void MacroControlRouter::load()
{
    auto pref = controller_.sessions().sessionManager().getUserPreference(kPrefKey);
    if (!pref.is_ok() || pref.value().empty()) return;
    const auto v = juce::JSON::parse(juce::String(pref.value()));
    engine_.fromVar(v["mappings"]);
    const int port = static_cast<int>(v["osc_port"]);
    if (port > 0) enableOsc(port, static_cast<bool>(v["osc_allow_network"]));
    ci_.setEnabled(static_cast<bool>(v["midi_ci"]));
    if (onChange) onChange();
}

} // namespace khensu
