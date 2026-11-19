// desktop-app/Source/session/DesktopPluginChain.cpp
#include "DesktopPluginChain.h"
#include <khensu/session/AudioFileProber.h>

namespace khensu {

namespace {
const juce::Identifier kChainId{"HostedChain"};
const juce::Identifier kSlotId{"Slot"};
const juce::Identifier kPathProp{"path"};
const juce::Identifier kBypassProp{"bypassed"};
const juce::Identifier kStateProp{"state"};

/// Some VST3s (e.g. ValhallaFreqEcho) come up with every bus DISABLED - a
/// 0-in/0-out plugin that the chain would silently pass audio around.
/// Enable the buses and ask for stereo main in/out (mono/default kept when a
/// plugin refuses stereo).
void configureStereoLayout(juce::AudioPluginInstance& p)
{
    p.enableAllBuses();
    const auto stereo = juce::AudioChannelSet::stereo();
    auto layout = p.getBusesLayout();
    if (layout.inputBuses.size() > 0) layout.inputBuses.getReference(0) = stereo;
    if (layout.outputBuses.size() > 0) layout.outputBuses.getReference(0) = stereo;
    if (p.checkBusesLayoutSupported(layout)) p.setBusesLayout(layout);
}

int channelsNeeded(juce::AudioPluginInstance& p)
{
    return juce::jmax(2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
}
} // namespace

DesktopPluginChain::DesktopPluginChain() : formatManager_(std::make_unique<juce::AudioPluginFormatManager>())
{
    juce::addDefaultFormatsToManager(*formatManager_);
}

DesktopPluginChain::~DesktopPluginChain() { clear(); }

void DesktopPluginChain::loadPluginAsync(const juce::File& pluginFile, std::function<void(bool, juce::String)> onDone)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (size() >= kMaxSlots) {
        if (onDone) onDone(false, "The chain is full (" + juce::String(kMaxSlots) + " slots).");
        return;
    }
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formatManager_->getFormats())
        if (format->fileMightContainThisPluginType(pluginFile.getFullPathName()))
            format->findAllTypesForFile(types, pluginFile.getFullPathName());
    if (types.isEmpty()) {
        if (onDone) onDone(false, "No loadable plugin found in " + pluginFile.getFileName() + ".");
        return;
    }
    const auto desc = *types.getFirst();
    const auto path = pluginFile.getFullPathName();
    formatManager_->createPluginInstanceAsync(
        desc, kDefaultSampleRate, kRenderBlock,
        [this, alive = std::weak_ptr<int>(lifetime_), path, onDone](std::unique_ptr<juce::AudioPluginInstance> instance,
                                                                    const juce::String& error) {
            if (alive.expired()) return; // the chain was destroyed mid-load
            if (instance == nullptr) {
                if (onDone) onDone(false, error.isNotEmpty() ? error : juce::String("The plugin failed to load."));
                return;
            }
            const auto name = instance->getName();
            const bool added = addInstance(std::move(instance), path);
            if (onDone) onDone(added, added ? name : juce::String("The chain is full."));
        });
}

bool DesktopPluginChain::addInstance(std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& filePath)
{
    if (instance == nullptr) return false;
    configureStereoLayout(*instance);
    const juce::ScopedLock sl(lock_);
    if (static_cast<int>(slots_.size()) >= kMaxSlots) return false;
    slots_.push_back({std::move(instance), filePath, false});
    return true;
}

bool DesktopPluginChain::remove(int index)
{
    std::shared_ptr<juce::AudioPluginInstance> dying;
    {
        const juce::ScopedLock sl(lock_);
        if (index < 0 || index >= static_cast<int>(slots_.size())) return false;
        dying = std::move(slots_[static_cast<size_t>(index)].instance);
        slots_.erase(slots_.begin() + index);
    }
    return true; // `dying` is destroyed outside the lock
}

bool DesktopPluginChain::move(int from, int to)
{
    const juce::ScopedLock sl(lock_);
    const int n = static_cast<int>(slots_.size());
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    if (from == to) return true;
    auto slot = std::move(slots_[static_cast<size_t>(from)]);
    slots_.erase(slots_.begin() + from);
    slots_.insert(slots_.begin() + to, std::move(slot));
    return true;
}

bool DesktopPluginChain::setBypassed(int index, bool bypassed)
{
    const juce::ScopedLock sl(lock_);
    if (index < 0 || index >= static_cast<int>(slots_.size())) return false;
    slots_[static_cast<size_t>(index)].bypassed = bypassed;
    return true;
}

void DesktopPluginChain::clear()
{
    std::vector<Slot> dying;
    {
        const juce::ScopedLock sl(lock_);
        dying.swap(slots_);
    }
}

int DesktopPluginChain::size() const
{
    const juce::ScopedLock sl(lock_);
    return static_cast<int>(slots_.size());
}

std::vector<DesktopPluginChain::SlotInfo> DesktopPluginChain::slots() const
{
    const juce::ScopedLock sl(lock_);
    std::vector<SlotInfo> out;
    for (const auto& s : slots_) {
        juce::PluginDescription d;
        s.instance->fillInPluginDescription(d);
        out.push_back({s.instance->getName(), s.filePath, d.pluginFormatName, s.bypassed, d.isInstrument,
                       s.instance->getLatencySamples()});
    }
    return out;
}

int DesktopPluginChain::totalLatencySamples() const
{
    const juce::ScopedLock sl(lock_);
    int total = 0;
    for (const auto& s : slots_)
        if (!s.bypassed)
            total += juce::jlimit(0, static_cast<int>(kDefaultSampleRate * kMaxSlotLatencySec),
                                  s.instance->getLatencySamples());
    return total;
}

juce::AudioPluginInstance* DesktopPluginChain::instanceAt(int index) const
{
    const juce::ScopedLock sl(lock_);
    if (index < 0 || index >= static_cast<int>(slots_.size())) return nullptr;
    return slots_[static_cast<size_t>(index)].instance.get();
}

int DesktopPluginChain::renderStream(juce::int64 numSamples, double sampleRate, const BlockReader& read,
                                     const BlockWriter& write, std::function<bool()> shouldCancel,
                                     const juce::MidiMessageSequence* midi, bool& writeFailed)
{
    writeFailed = false;
    if (numSamples <= 0 || sampleRate <= 0.0) return 0;

    const juce::ScopedLock renderGuard(renderLock_);
    // Snapshot the active slots under the list lock, then render without it.
    // A slot removed mid-render stays alive through `owners` and is released
    // on the message thread afterwards (plugins expect to be destroyed there).
    std::vector<std::shared_ptr<juce::AudioPluginInstance>> owners;
    {
        const juce::ScopedLock sl(lock_);
        for (const auto& s : slots_)
            if (!s.bypassed && s.instance != nullptr) owners.push_back(s.instance);
    }

    std::vector<juce::AudioPluginInstance*> active;
    int scratchChannels = 2;
    for (const auto& owner : owners) {
        auto* p = owner.get();
        p->setNonRealtime(true);
        p->setPlayConfigDetails(p->getTotalNumInputChannels() > 0 ? 2 : 0, 2, sampleRate, kRenderBlock);
        p->prepareToPlay(sampleRate, kRenderBlock);
        scratchChannels = juce::jmax(scratchChannels, channelsNeeded(*p));
        active.push_back(p);
    }
    // Plugins self-report latency; clamp each to kMaxSlotLatencySec so a
    // buggy or hostile plugin can't force an unbounded tail.
    const int maxSlotLatency = static_cast<int>(sampleRate * kMaxSlotLatencySec);
    int latency = 0;
    for (auto* p : active)
        latency += juce::jlimit(0, maxSlotLatency, p->getLatencySamples());

    // Streamed block by block (AUDIT-065): the source is read, processed and
    // written one kRenderBlock at a time instead of decoding the whole file
    // and then copying it into a second full-length work buffer. `latency`
    // extra samples of silence flush the delayed tail out of every plugin;
    // the first `latency` output samples (pre-roll) are dropped, so the
    // result stays sample-aligned with the source for A/B.
    const juce::int64 total = numSamples + latency;
    juce::AudioBuffer<float> block(2, kRenderBlock);
    juce::AudioBuffer<float> scratch(scratchChannels, kRenderBlock);
    juce::MidiBuffer blockMidi;
    int nextEvent = 0;
    for (juce::int64 start = 0; start < total; start += kRenderBlock) {
        if (shouldCancel && shouldCancel()) break;
        const int len = static_cast<int>(juce::jmin(static_cast<juce::int64>(kRenderBlock), total - start));
        block.clear();
        const int inputLen = static_cast<int>(juce::jlimit<juce::int64>(0, len, numSamples - start));
        if (inputLen > 0) read(block, start, inputLen);

        blockMidi.clear();
        if (midi != nullptr) {
            while (nextEvent < midi->getNumEvents()) {
                const auto& m = midi->getEventPointer(nextEvent)->message;
                const auto pos = static_cast<juce::int64>(juce::roundToInt(m.getTimeStamp()));
                if (pos >= start + len) break;
                blockMidi.addEvent(m, static_cast<int>(juce::jmax<juce::int64>(0, pos - start)));
                ++nextEvent;
            }
        }
        for (auto* p : active) {
            scratch.setSize(scratchChannels, len, false, false, true);
            scratch.clear();
            for (int c = 0; c < 2; ++c)
                scratch.copyFrom(c, 0, block, c, 0, len);
            juce::MidiBuffer slotMidi(blockMidi);
            p->processBlock(scratch, slotMidi);
            for (int c = 0; c < 2; ++c)
                block.copyFrom(c, 0, scratch, c, 0, len);
        }

        // Global sample `start + i` lands at output index `start + i - latency`.
        const int skip = static_cast<int>(juce::jlimit<juce::int64>(0, len, latency - start));
        if (skip < len && !write(block, skip, len - skip)) {
            writeFailed = true;
            break;
        }
    }
    for (auto* p : active)
        p->releaseResources();
    if (!juce::MessageManager::existsAndIsCurrentThread() && juce::MessageManager::getInstanceWithoutCreating() != nullptr) {
        // Hand the references back so a slot removed during the render is
        // destroyed on the message thread rather than this worker.
        juce::MessageManager::callAsync([held = std::move(owners)]() mutable { held.clear(); });
    }
    return latency;
}

int DesktopPluginChain::renderBuffer(juce::AudioBuffer<float>& audio, double sampleRate,
                                     std::function<bool()> shouldCancel, const juce::MidiMessageSequence* midi)
{
    const int numSamples = audio.getNumSamples();
    if (numSamples <= 0 || sampleRate <= 0.0) return 0;

    // Rendered in place: a block's output lands at or before the samples it
    // read (output index = input index - latency), so it never overwrites
    // input that is still to be read. The chain is stereo; mono is widened.
    const int sourceChannels = audio.getNumChannels();
    audio.setSize(juce::jmax(2, sourceChannels), numSamples, true, true, true);
    if (sourceChannels < 2) audio.copyFrom(1, 0, audio, 0, 0, numSamples);

    int writePos = 0;
    bool writeFailed = false;
    const int latency = renderStream(
        numSamples, sampleRate,
        [&audio](juce::AudioBuffer<float>& dest, juce::int64 start, int n) {
            for (int c = 0; c < 2; ++c)
                dest.copyFrom(c, 0, audio, c, static_cast<int>(start), n);
        },
        [&audio, &writePos, numSamples](const juce::AudioBuffer<float>& src, int offset, int n) {
            const int count = juce::jmin(n, numSamples - writePos);
            for (int c = 0; c < 2 && count > 0; ++c)
                audio.copyFrom(c, writePos, src, c, offset, count);
            writePos += juce::jmax(0, count);
            return true;
        },
        std::move(shouldCancel), midi, writeFailed);
    audio.setSize(2, numSamples, true, false, true);
    return latency;
}

DesktopPluginChain::RenderResult DesktopPluginChain::renderFile(const juce::File& input, const juce::File& output,
                                                                std::function<bool()> shouldCancel,
                                                                const juce::File& midiFile)
{
    RenderResult r;
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader = AudioFileProber::openReader(fm, input);
    if (reader == nullptr) {
        r.error = "Could not read " + input.getFileName() + ".";
        return r;
    }
    if (reader->lengthInSamples > static_cast<juce::int64>(reader->sampleRate * 60.0 * 30.0)) {
        r.error = "Source is longer than 30 minutes.";
        return r;
    }
    const juce::int64 len = reader->lengthInSamples;
    const double sr = reader->sampleRate;

    juce::MidiMessageSequence seq;
    const juce::MidiMessageSequence* seqPtr = nullptr;
    if (midiFile.existsAsFile()) {
        juce::FileInputStream in(midiFile);
        juce::MidiFile mf;
        if (in.openedOk() && mf.readFrom(in)) {
            mf.convertTimestampTicksToSeconds();
            for (int t = 0; t < mf.getNumTracks(); ++t)
                seq.addSequence(*mf.getTrack(t), 0.0);
            seq.updateMatchedPairs();
            for (int i = 0; i < seq.getNumEvents(); ++i) {
                auto& m = seq.getEventPointer(i)->message;
                m.setTimeStamp(m.getTimeStamp() * sr);
            }
            seqPtr = &seq;
        }
    }

    output.getParentDirectory().createDirectory();
    // Written to a sibling temp file and swapped in only on success, so a
    // cancelled or failed render never leaves a truncated file at `output`.
    juce::TemporaryFile tmp(output, juce::TemporaryFile::useHiddenFile);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream(tmp.getFile().createOutputStream());
    if (stream == nullptr) {
        r.error = "Could not write " + output.getFullPathName() + ".";
        return r;
    }
    // 32-bit float (AUDIT-065): hosted plugins routinely produce overs above
    // 0 dBFS (saturators, makeup gain); 24-bit integer clipped them hard in
    // the file the A/B and the export then used.
    const auto options = juce::AudioFormatWriterOptions{}
                             .withSampleRate(sr)
                             .withNumChannels(2)
                             .withBitsPerSample(32)
                             .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = wav.createWriterFor(stream, options);
    if (writer == nullptr) {
        r.error = "Could not create a WAV writer.";
        return r;
    }

    bool writeFailed = false;
    auto* readerPtr = reader.get();
    auto* writerPtr = writer.get();
    r.compensatedLatencySamples = renderStream(
        len, sr,
        [readerPtr](juce::AudioBuffer<float>& dest, juce::int64 start, int n) {
            // A 2-channel destination: mono sources are duplicated, extra
            // channels beyond the stereo chain are not read.
            readerPtr->read(&dest, 0, n, start, true, true);
        },
        [writerPtr](const juce::AudioBuffer<float>& src, int offset, int n) {
            return writerPtr->writeFromAudioSampleBuffer(src, offset, n);
        },
        shouldCancel, seqPtr, writeFailed);
    writer.reset(); // flush + close before the swap
    if (shouldCancel && shouldCancel()) {
        r.error = "Cancelled.";
        return r;
    }
    if (writeFailed) {
        r.error = "Write error while rendering to " + output.getFullPathName() + ".";
        return r;
    }
    if (!tmp.overwriteTargetFileWithTemporary()) {
        r.error = "Could not replace " + output.getFullPathName() + ".";
        return r;
    }
    r.ok = true;
    r.output = output;
    return r;
}

juce::ValueTree DesktopPluginChain::toValueTree() const
{
    const juce::ScopedLock sl(lock_);
    juce::ValueTree tree(kChainId);
    for (const auto& s : slots_) {
        juce::ValueTree slot(kSlotId);
        slot.setProperty(kPathProp, s.filePath, nullptr);
        slot.setProperty(kBypassProp, s.bypassed, nullptr);
        juce::MemoryBlock state;
        s.instance->getStateInformation(state);
        slot.setProperty(kStateProp, state.toBase64Encoding(), nullptr);
        tree.appendChild(slot, nullptr);
    }
    return tree;
}

std::vector<std::pair<juce::String, bool>> DesktopPluginChain::savedSlotPaths(const juce::ValueTree& tree)
{
    std::vector<std::pair<juce::String, bool>> out;
    if (!tree.hasType(kChainId)) return out;
    for (const auto& slot : tree)
        if (slot.hasType(kSlotId) && slot[kPathProp].toString().isNotEmpty())
            out.emplace_back(slot[kPathProp].toString(), static_cast<bool>(slot[kBypassProp]));
    return out;
}

void DesktopPluginChain::restoreAsync(const juce::ValueTree& tree, std::function<void(int)> onDone)
{
    clear();
    auto pending = std::make_shared<std::vector<juce::ValueTree>>();
    if (tree.hasType(kChainId))
        for (const auto& slot : tree)
            if (slot.hasType(kSlotId) && slot[kPathProp].toString().isNotEmpty()) pending->push_back(slot);
    auto loaded = std::make_shared<int>(0);
    // Sequential so the restored order matches the saved order. The step
    // function holds only a weak reference to itself; each pending load
    // callback keeps it alive until the next step runs.
    auto step = std::make_shared<std::function<void(size_t)>>();
    std::weak_ptr<std::function<void(size_t)>> weakStep = step;
    *step = [this, pending, loaded, weakStep, onDone](size_t i) {
        if (i >= pending->size()) {
            if (onDone) onDone(*loaded);
            return;
        }
        const auto slot = (*pending)[i];
        auto keepAlive = weakStep.lock();
        loadPluginAsync(juce::File(slot[kPathProp].toString()),
                        [this, slot, loaded, keepAlive, i](bool ok, juce::String) {
                            if (ok) {
                                const int idx = size() - 1;
                                if (auto* inst = instanceAt(idx)) {
                                    juce::MemoryBlock state;
                                    if (state.fromBase64Encoding(slot[kStateProp].toString()) && state.getSize() > 0)
                                        inst->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
                                }
                                setBypassed(idx, static_cast<bool>(slot[kBypassProp]));
                                ++*loaded;
                            }
                            if (keepAlive) (*keepAlive)(i + 1);
                        });
    };
    (*step)(0);
}

} // namespace khensu
