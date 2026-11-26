#include "Midi2DeviceManager.h"
#include <algorithm>

namespace khensu {
namespace {

midi::SourceProtocol sourceProtocolFor(const juce::ump::Endpoint& endpoint) noexcept
{
    const auto protocol = endpoint.getProtocol();
    if (!protocol.has_value()) return midi::SourceProtocol::Unknown;

    return *protocol == juce::ump::PacketProtocol::MIDI_2_0 ? midi::SourceProtocol::Midi2 : midi::SourceProtocol::Midi1;
}

} // namespace

Midi2DeviceManager::Midi2DeviceManager()
{
    // A native MIDI 2.0 backend (Windows MIDI Services' WinRT/COM activation,
    // in particular) failing to initialize on a given machine - missing or
    // mis-registered runtime component, driver quirk, etc. - must degrade to
    // "no MIDI 2.0 available", not crash the whole app before any window has
    // even opened. This is not hypothetical: it reproduced on first launch
    // after enabling KHENSU_ENABLE_WINDOWS_MIDI_SERVICES (ucrtbase.dll
    // fast-fail, 0xC0000409, from an uncaught exception escaping this
    // constructor). Mirrors the existing endpoints_ == nullptr fallback path.
    try {
        endpoints_ = juce::ump::Endpoints::getInstance();
        if (endpoints_ == nullptr) return;

        session_.emplace(endpoints_->makeSession("Khensu Mixing MIDI"));
        endpoints_->addListener(*this);
        refreshEndpointCache();
    } catch (...) {
        endpoints_ = nullptr;
        session_.reset();
    }
}

Midi2DeviceManager::~Midi2DeviceManager()
{
    cancelPendingUpdate();
    disconnectInput();
    if (endpoints_ != nullptr) endpoints_->removeListener(*this);
}

juce::String Midi2DeviceManager::backendName() const
{
    if (endpoints_ == nullptr) return "Unavailable";

    const auto backend = endpoints_->getBackend();
    if (!backend.has_value()) return "Unavailable";

    switch (*backend) {
    case juce::ump::Backend::alsa:
        return "ALSA";
    case juce::ump::Backend::android:
        return "Android MIDI";
    case juce::ump::Backend::coremidi:
        return "CoreMIDI";
    case juce::ump::Backend::winmm:
        return "Windows Multimedia MIDI 1.0";
    case juce::ump::Backend::winrt:
        return "Windows Runtime MIDI 1.0";
    case juce::ump::Backend::wms:
        return "Windows MIDI Services (MIDI 2.0)";
    }

    return "Unavailable";
}

bool Midi2DeviceManager::nativeMidi2BackendAvailable() const noexcept
{
    if (endpoints_ == nullptr) return false;

    const auto backend = endpoints_->getBackend();
#if JUCE_WINDOWS
    return backend.has_value() && *backend == juce::ump::Backend::wms;
#else
    return backend.has_value();
#endif
}

bool Midi2DeviceManager::connectInput(const juce::String& sourceIdentifier)
{
    disconnectInput();

    if (!session_.has_value() || !session_->isAlive()) return false;

    const auto found = std::find_if(endpointCache_.begin(), endpointCache_.end(),
                                    [&sourceIdentifier](const EndpointDescriptor& endpoint) {
                                        return endpoint.sourceIdentifier == sourceIdentifier;
                                    });
    if (found == endpointCache_.end()) return false;

    input_ = session_->connectInput(found->id, juce::ump::PacketProtocol::MIDI_2_0);
    if (!input_.isAlive()) return false;
    connectedId_ = found->id;
    connectOutputIfWanted();

    connectedSourceIdentifier_ = found->sourceIdentifier;
    connectedSourceProtocol_.store(found->sourceProtocol, std::memory_order_release);
    input_.addConsumer(*this);
    listeners_.call([](Listener& listener) { listener.midiConnectionChanged(); });
    return true;
}

void Midi2DeviceManager::disconnectInput()
{
    const bool wasConnected = input_.isAlive() || connectedSourceIdentifier_.isNotEmpty();
    if (input_.isAlive()) input_.removeConsumer(*this);
    input_ = {};
    output_ = {};
    connectedSourceIdentifier_.clear();
    connectedSourceProtocol_.store(midi::SourceProtocol::Unknown, std::memory_order_release);

    if (wasConnected) listeners_.call([](Listener& listener) { listener.midiConnectionChanged(); });
}

void Midi2DeviceManager::consume(juce::ump::Iterator begin, juce::ump::Iterator end, double timestampSeconds)
{
    const auto sourceProtocol = connectedSourceProtocol_.load(std::memory_order_acquire);

    const bool sysEx = sysExEnabled_.load(std::memory_order_acquire);
    for (auto iterator = begin; iterator != end; ++iterator) {
        const auto& packet = *iterator;
        if (sysEx && packet.size() == 2 && midi::SysEx7Assembler::isSysEx7(packet.data()[0])) {
            if (auto message = sysExAssembler_.feed(packet.data()[0], packet.data()[1])) {
                const std::lock_guard<std::mutex> lock(sysExMutex_);
                if (sysExQueue_.size() < 64) sysExQueue_.push_back(std::move(*message));
            }
        }
        auto decoded = midi::decodeUmpEvent(packet.data(), packet.size(), timestampSeconds);
        if (!decoded.has_value()) continue;

        decoded->sourceProtocol = sourceProtocol;

        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        eventFifo_.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 + size2 == 0) {
            droppedEvents_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const int index = size1 != 0 ? start1 : start2;
        eventQueue_[static_cast<std::size_t>(index)] = *decoded;
        eventFifo_.finishedWrite(1);
    }

    triggerAsyncUpdate();
}

void Midi2DeviceManager::setSysExEnabled(bool enabled)
{
    sysExEnabled_.store(enabled, std::memory_order_release);
    if (enabled) {
        connectOutputIfWanted();
    } else {
        output_ = {};
        const std::lock_guard<std::mutex> lock(sysExMutex_);
        sysExQueue_.clear();
    }
}

void Midi2DeviceManager::connectOutputIfWanted()
{
    if (!sysExEnabled() || output_.isAlive() || !input_.isAlive() || !session_.has_value() || !session_->isAlive()) return;
    try {
        output_ = session_->connectOutput(connectedId_);
    } catch (...) {
        output_ = {}; // an input-only device: MIDI-CI just gets no replies
    }
}

bool Midi2DeviceManager::sendSysEx(std::uint8_t group, const std::uint8_t* bytes, std::size_t size)
{
    if (!output_.isAlive()) return false;
    const auto words = midi::toSysEx7Packets(group, bytes, size);
    juce::ump::Packets packets;
    for (std::size_t i = 0; i + 1 < words.size(); i += 2)
        packets.add(juce::ump::PacketX2{words[i], words[i + 1]});
    return output_.send(packets.begin(), packets.end());
}

void Midi2DeviceManager::drainSysEx()
{
    std::vector<midi::SysExMessage> ready;
    {
        const std::lock_guard<std::mutex> lock(sysExMutex_);
        ready.swap(sysExQueue_);
    }
    if (!sysExEnabled()) return;
    for (const auto& m : ready)
        listeners_.call([&m](Listener& listener) { listener.midiSysExReceived(m); });
}

void Midi2DeviceManager::endpointsChanged()
{
    endpointRefreshRequested_.store(true, std::memory_order_release);
    triggerAsyncUpdate();
}

void Midi2DeviceManager::handleAsyncUpdate()
{
    if (endpointRefreshRequested_.exchange(false, std::memory_order_acq_rel)) refreshEndpointCache();
    drainEvents();
    drainSysEx();
}

void Midi2DeviceManager::refreshEndpointCache()
{
    endpointCache_.clear();
    if (endpoints_ == nullptr) return;

    for (const auto& id : endpoints_->getEndpoints()) {
        const auto sourceIdentifier = id.get(juce::ump::IOKind::src);
        if (sourceIdentifier.isEmpty()) continue;

        EndpointDescriptor descriptor;
        descriptor.id = id;
        descriptor.sourceIdentifier = sourceIdentifier;
        descriptor.name = sourceIdentifier;

        if (const auto endpoint = endpoints_->getEndpoint(id)) {
            if (endpoint->getName().isNotEmpty()) descriptor.name = endpoint->getName();
            descriptor.supportsMidi1 = endpoint->hasMidi1Support();
            descriptor.supportsMidi2 = endpoint->hasMidi2Support();
            descriptor.sourceProtocol = sourceProtocolFor(*endpoint);
        } else if (const auto info = endpoints_->getStaticDeviceInfo(id)) {
            if (info->getName().isNotEmpty()) descriptor.name = info->getName();
        }

        endpointCache_.push_back(std::move(descriptor));
    }

    const bool connectedStillExists =
        connectedSourceIdentifier_.isEmpty() ||
        std::any_of(endpointCache_.begin(), endpointCache_.end(),
                    [this](const auto& endpoint) { return endpoint.sourceIdentifier == connectedSourceIdentifier_; });
    if (!connectedStillExists) disconnectInput();

    listeners_.call([](Listener& listener) { listener.midiEndpointsChanged(); });
}

void Midi2DeviceManager::drainEvents()
{
    while (eventFifo_.getNumReady() > 0) {
        int start1 = 0;
        int size1 = 0;
        int start2 = 0;
        int size2 = 0;
        eventFifo_.prepareToRead(eventFifo_.getNumReady(), start1, size1, start2, size2);

        for (int i = 0; i < size1; ++i)
            listeners_.call([&event = eventQueue_[static_cast<std::size_t>(start1 + i)]](Listener& listener) {
                listener.midiEventReceived(event);
            });
        for (int i = 0; i < size2; ++i)
            listeners_.call([&event = eventQueue_[static_cast<std::size_t>(start2 + i)]](Listener& listener) {
                listener.midiEventReceived(event);
            });

        eventFifo_.finishedRead(size1 + size2);
    }
}

} // namespace khensu
