// desktop-app/Source/midi/MidiCiResponder.cpp
#include "MidiCiResponder.h"

#include <algorithm>
#include <string_view>

namespace khensu {

namespace ci = juce::midi_ci;

MidiCiResponder::MidiCiResponder(Midi2DeviceManager* midi, const MacroMappingEngine& engine, ValueOf valueOf)
    : midi_(midi), engine_(engine), valueOf_(std::move(valueOf))
{}

MidiCiResponder::~MidiCiResponder() { setEnabled(false); }

void MidiCiResponder::setEnabled(bool enabled)
{
    if (enabled == isEnabled()) return;
    if (!enabled) {
        if (midi_ != nullptr) {
            midi_->removeListener(*this);
            midi_->setSysExEnabled(false);
        }
        device_.reset();
        return;
    }
    juce::ump::DeviceInfo info{};
    info.manufacturer = {std::byte{0x7D}, std::byte{0x00}, std::byte{0x00}}; // non-commercial id
    info.family = {std::byte{0x00}, std::byte{0x00}};
    info.modelNumber = {std::byte{0x01}, std::byte{0x00}};
    info.revision = {std::byte{0x01}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}};
    std::array<char, 16> instance{};
    constexpr std::string_view id{"KhensuMixing"};
    std::copy_n(id.begin(), std::min(id.size(), instance.size()), instance.begin());
    const auto options = ci::DeviceOptions{}
                             .withOutputs({this})
                             .withDeviceInfo(info)
                             .withFeatures(ci::DeviceFeatures{}.withPropertyExchangeSupported(true))
                             .withMaxSysExSize(512)
                             .withProductInstanceId(instance)
                             .withPropertyDelegate(this);
    device_ = std::make_unique<ci::Device>(options);
    if (midi_ != nullptr) {
        midi_->addListener(*this);
        midi_->setSysExEnabled(true);
    }
}

std::uint32_t MidiCiResponder::muid() const noexcept { return device_ != nullptr ? device_->getMuid().get() : 0u; }

void MidiCiResponder::receive(juce::ump::BytesOnGroup message)
{
    if (device_ != nullptr) device_->processMessage(message);
}

void MidiCiResponder::midiSysExReceived(const midi::SysExMessage& message)
{
    // MIDI-CI messages are universal non-real-time SysEx (0x7E ... sub-id 0x0D).
    if (message.bytes.size() < 4 || message.bytes[0] != 0x7E || message.bytes[2] != 0x0D) return;
    const auto* data = reinterpret_cast<const std::byte*>(message.bytes.data());
    receive({message.group, juce::Span<const std::byte>(data, message.bytes.size())});
}

void MidiCiResponder::processMessage(juce::ump::BytesOnGroup message)
{
    if (sender_) {
        sender_(message);
        return;
    }
    if (midi_ != nullptr)
        midi_->sendSysEx(message.group, reinterpret_cast<const std::uint8_t*>(message.bytes.data()),
                         message.bytes.size());
}

juce::String MidiCiResponder::resourceNameFor(const juce::String& target)
{
    juce::String clean;
    for (const auto c : target)
        if (juce::CharacterFunctions::isLetterOrDigit(c) || c == '-') clean += juce::String::charToString(c);
    return "X-Macro-" + clean;
}

juce::var MidiCiResponder::resourceList(const juce::StringArray& targets)
{
    juce::Array<juce::var> list;
    for (const auto& t : targets) {
        auto* entry = new juce::DynamicObject();
        entry->setProperty("resource", resourceNameFor(t));
        entry->setProperty("canGet", true);
        entry->setProperty("canSet", "none");
        entry->setProperty("canSubscribe", false);
        entry->setProperty("requireResId", false);
        entry->setProperty("mediaTypes", juce::Array<juce::var>{"application/json"});
        entry->setProperty("encodings", juce::Array<juce::var>{"ASCII"});
        auto* schema = new juce::DynamicObject();
        schema->setProperty("type", "object");
        schema->setProperty("title", t);
        entry->setProperty("schema", juce::var(schema));
        list.add(juce::var(entry));
    }
    return list;
}

juce::var MidiCiResponder::propertyBody(const juce::String& target, double value,
                                        const MacroMappingEngine::Mapping* mapping)
{
    auto* body = new juce::DynamicObject();
    body->setProperty("name", target);
    body->setProperty("value", value);
    body->setProperty("min", 0.0);
    body->setProperty("max", 100.0);
    if (mapping != nullptr) {
        auto* m = new juce::DynamicObject();
        m->setProperty("source", mapping->source.describe());
        m->setProperty("valueAtMin", mapping->minValue);
        m->setProperty("valueAtMax", mapping->maxValue);
        m->setProperty("curve", MacroMappingEngine::curveName(mapping->curve));
        m->setProperty("invert", mapping->invert);
        body->setProperty("mapping", juce::var(m));
    } else {
        body->setProperty("mapping", juce::var());
    }
    return juce::var(body);
}

ci::PropertyReplyData MidiCiResponder::propertyGetDataRequested(ci::MUID, const ci::PropertyRequestHeader& header)
{
    if (header.resource == "ResourceList")
        return ci::PropertyReplyData{{}, ci::Encodings::jsonTo7BitText(resourceList(engine_.targets()))};
    for (const auto& t : engine_.targets()) {
        if (header.resource != resourceNameFor(t)) continue;
        const MacroMappingEngine::Mapping* mapping = nullptr;
        for (const auto& m : engine_.mappings())
            if (m.target == t) mapping = &m;
        const double value = valueOf_ ? valueOf_(t) : 0.0;
        return ci::PropertyReplyData{{}, ci::Encodings::jsonTo7BitText(propertyBody(t, value, mapping))};
    }
    ci::PropertyReplyData notFound;
    notFound.header.status = 404;
    notFound.header.message = "No resource " + header.resource;
    return notFound;
}

ci::PropertyReplyHeader MidiCiResponder::propertySetDataRequested(ci::MUID, const ci::PropertyRequestData&)
{
    ci::PropertyReplyHeader h;
    h.status = 405;
    h.message = "Read-only: map controls in Khensu Mixing";
    return h;
}

} // namespace khensu
