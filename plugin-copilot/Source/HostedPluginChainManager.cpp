#include "HostedPluginChainManager.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace khensu {

namespace {

/// Hosted instances are destroyed on the message thread. When we already are
/// on it (the normal case, and always during plugin unload) release right
/// here: a callAsync-deferred release could run after the host has unloaded
/// this module, or never run at all, leaking the instances (audit M6).
template <typename Resources>
void releaseOnMessageThread(Resources&& resources)
{
    if (juce::MessageManager::existsAndIsCurrentThread()) {
        auto dying = std::forward<Resources>(resources);
        juce::ignoreUnused(dying);
        return; // destroyed here, synchronously
    }
    juce::MessageManager::callAsync([held = std::forward<Resources>(resources)]() {
        juce::ignoreUnused(held); // released on the message thread
    });
}

int channelsNeeded(const juce::AudioPluginInstance& instance)
{
    return juce::jmax(instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels());
}

/// AudioBuffer keeps a non-owning channel table inline only for FEWER than
/// 32 channels; 32 or more heap-allocate (AUDIT-085).
constexpr int kMaxInlineChannels = 32;

/// A mono-only plugin on a multi-channel block runs on the mono sum.
bool runsOnMonoSum(int needed, int numChannels) noexcept
{
    return needed == 1 && numChannels > 1;
}

/// The single fit test shared by the audio path (runInstance) and the
/// latency/tail reports: an instance runs directly on a block of
/// `numChannels`, on the slot's preallocated mono scratch (a mono-only
/// plugin on a stereo block), or on its preallocated wide scratch.
bool canRunAt(const HostedPluginChainManager::Slot& slot, int numChannels, int numSamples)
{
    if (slot.instance == nullptr) return false;
    const int needed = channelsNeeded(*slot.instance);
    if (runsOnMonoSum(needed, numChannels))
        return slot.monoScratch.getNumChannels() >= 1 && slot.monoScratch.getNumSamples() >= numSamples;
    if (needed <= numChannels) return true;
    const auto& wide = slot.wideScratch;
    return needed < kMaxInlineChannels && wide.getNumChannels() >= needed && wide.getNumSamples() >= numSamples;
}

/// The latency the chain reports for a slot: the instance's own, up to the
/// capacity reserved for its dry line (FIX-14: the two used to differ above
/// one second, and a latency raised after insert stayed clamped until the
/// host re-prepared).
int compensatedLatency(const HostedPluginChainManager::Slot& slot) noexcept
{
    if (slot.instance == nullptr) return 0;
    return juce::jlimit(0, slot.dryCompDelay.reservedCapacity(), slot.instance->getLatencySamples());
}

/// Audio thread: the latency the slot's dry line can apply this block - the
/// reported one once the line reserveDryLines() grew has been adopted.
int audioCompensatedLatency(const HostedPluginChainManager::Slot& slot) noexcept
{
    if (slot.instance == nullptr) return 0;
    return juce::jlimit(0, slot.dryCompDelay.activeCapacity(), slot.instance->getLatencySamples());
}

/// A NaN/Inf wet amount (a corrupt project, a bad automation value) would
/// pass straight through jlimit; treat it as the fully-wet default.
float sanitizeWetMix(float wetMix) noexcept
{
    return std::isfinite(wetMix) ? juce::jlimit(0.0f, 1.0f, wetMix) : 1.0f;
}

/// Runs one hosted instance over `block`. When the instance needs more
/// channels than the block has, it runs on the slot's preallocated wide
/// scratch and only the block's channels are copied back; with no scratch big
/// enough it is skipped rather than handed a channel table it would read
/// past. Returns false when skipped.
bool runInstance(HostedPluginChainManager::Slot& slot, juce::AudioBuffer<float>& block, juce::MidiBuffer& midi)
{
    auto& instance = *slot.instance;
    const int numChannels = block.getNumChannels();
    const int numSamples = block.getNumSamples();
    if (!canRunAt(slot, numChannels, numSamples)) return false;
    const int needed = channelsNeeded(instance);
    if (runsOnMonoSum(needed, numChannels)) {
        // Sum to mono, process, and give every channel the result (FIX-14).
        juce::AudioBuffer<float> mono(slot.monoScratch.getArrayOfWritePointers(), 1, 0, numSamples);
        mono.copyFrom(0, 0, block, 0, 0, numSamples);
        for (int c = 1; c < numChannels; ++c) mono.addFrom(0, 0, block, c, 0, numSamples);
        mono.applyGain(1.0f / static_cast<float>(numChannels));
        instance.processBlock(mono, midi);
        for (int c = 0; c < numChannels; ++c) block.copyFrom(c, 0, mono, 0, 0, numSamples);
        return true;
    }
    if (needed <= numChannels) {
        instance.processBlock(block, midi);
        return true;
    }

    // Main-bus inputs the block lacks (a stereo-only plugin in a mono
    // session) get the session signal duplicated, so M/S and linked
    // processing see a centred source instead of a dead right channel
    // (AUDIT-028). Only auxiliary (sidechain) inputs are silent.
    const int mainInputs = instance.getMainBusNumInputChannels();
    juce::AudioBuffer<float> view(slot.wideScratch.getArrayOfWritePointers(), needed, 0, numSamples);
    for (int c = 0; c < needed; ++c) {
        if (c < numChannels)
            view.copyFrom(c, 0, block, c, 0, numSamples);
        else if (c < mainInputs && numChannels > 0)
            view.copyFrom(c, 0, block, c % numChannels, 0, numSamples);
        else
            view.clear(c, 0, numSamples);
    }
    instance.processBlock(view, midi);
    for (int c = 0; c < numChannels; ++c) block.copyFrom(c, 0, view, c, 0, numSamples);
    return true;
}

} // namespace

class HostedPluginChainManager::LatencyListener final : public juce::AudioProcessorListener {
  public:
    explicit LatencyListener(HostedPluginChainManager& owner) : owner_(owner) {}
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.latencyChanged) owner_.notifyLatencyChanged();
    }

  private:
    HostedPluginChainManager& owner_;
};

HostedPluginChainManager::Slot::~Slot()
{
    if (instance != nullptr && listener != nullptr) instance->removeListener(listener);
}

HostedPluginChainManager::HostedPluginChainManager() : latencyListener_(std::make_unique<LatencyListener>(*this)) {}

void HostedPluginChainManager::setLatencyChangedCallback(std::function<void()> callback)
{
    const juce::ScopedLock sl(mutex_);
    latencyChanged_ = std::move(callback);
}

void HostedPluginChainManager::notifyLatencyChanged()
{
    // May run on the audio thread (a hosted plugin re-reporting latency from
    // its process call): only invoke the pre-set, real-time-safe callback.
    if (latencyChanged_) latencyChanged_();
}

HostedPluginChainManager::~HostedPluginChainManager()
{
    std::shared_ptr<Chain> chainToClear;
    std::vector<RetiredChain> retiredToClear;
    {
        const juce::ScopedLock sl(mutex_);
        activeChain_.store(nullptr);
        chainToClear = std::move(activeChainOwner_);
        retiredToClear = std::move(retiredChains_);

        // Instances may outlive this manager (released on the message thread
        // later); they must not call back into it.
        const auto detach = [](const std::shared_ptr<Chain>& chain) {
            if (chain == nullptr) return;
            for (const auto& slot : *chain)
                if (slot != nullptr && slot->instance != nullptr && slot->listener != nullptr) {
                    slot->instance->removeListener(slot->listener);
                    slot->listener = nullptr;
                }
        };
        detach(chainToClear);
        for (const auto& retired : retiredToClear) detach(retired.chain);
    }

    if ((chainToClear != nullptr && !chainToClear->empty()) || !retiredToClear.empty()) {
        struct Held {
            std::shared_ptr<Chain> chain;
            std::vector<RetiredChain> retired;
        };
        releaseOnMessageThread(Held{std::move(chainToClear), std::move(retiredToClear)});
    }
}

void HostedPluginChainManager::prepareSlotLocked(Slot& slot)
{
    // Hosted instances must run at the host's real rate and block size (a
    // restore before the first prepare left them at 44.1k/512, and a larger
    // host block then overran what they were prepared for). The bus layout
    // is configured BEFORE prepareToPlay (audit M3) - a plugin sizes its
    // internal state for the channel count it is prepared with.
    if (slot.instance != nullptr) {
        auto& instance = *slot.instance;
        configureHostedLayout(instance, preparedChannels_);
        instance.setNonRealtime(nonRealtime_.load(std::memory_order_relaxed));
        instance.setRateAndBufferSizeDetails(sampleRate_, blockSize_);
        instance.prepareToPlay(sampleRate_, blockSize_);
    }
    // Room for the instance's latency, so a plugin reporting more than a
    // second is compensated (and reported) in full (FIX-14). Capped: a
    // plugin reporting an absurd latency gets kMaxCompensatedLatencySeconds.
    const int reported = slot.instance != nullptr ? juce::jmax(0, slot.instance->getLatencySamples()) : 0;
    const int cap = static_cast<int>(std::ceil(sampleRate_ * kMaxCompensatedLatencySeconds));
    slot.dryCompDelay.prepare(sampleRate_, blockSize_, preparedChannels_,
                              juce::jmin(cap, juce::jmax(static_cast<int>(std::ceil(sampleRate_)), reported)));
    slot.wetSmoothed.reset(sampleRate_, 0.02);
    slot.wetSmoothed.setCurrentAndTargetValue(
        slot.bypassed.load(std::memory_order_relaxed) ? 0.0f : slot.wetMix.load(std::memory_order_relaxed));

    const int needed = slot.instance != nullptr ? channelsNeeded(*slot.instance) : 0;
    if (needed > preparedChannels_)
        slot.wideScratch.setSize(needed, blockSize_);
    else
        slot.wideScratch.setSize(0, 0);
    if (runsOnMonoSum(needed, preparedChannels_))
        slot.monoScratch.setSize(1, blockSize_);
    else
        slot.monoScratch.setSize(0, 0);
}

void HostedPluginChainManager::prepareToPlay(double sampleRate, int samplesPerBlock, int preparedChannels,
                                             bool nonRealtime)
{
    // The control lock guards the chain walk against a concurrent add/remove
    // (audit L10); the audio thread never takes it.
    const juce::ScopedLock sl(mutex_);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    blockSize_ = juce::jmax(1, samplesPerBlock);
    preparedChannels_ = juce::jlimit(1, 2, preparedChannels);
    nonRealtime_.store(nonRealtime, std::memory_order_relaxed);
    partialWetDry_.setSize(preparedChannels_, blockSize_);
    midi_.ensureSize(kMidiReserveBytes);

    for (const auto& slot : *activeChainOwner_)
        if (slot != nullptr) prepareSlotLocked(*slot);
}

void HostedPluginChainManager::releaseResources()
{
    const juce::ScopedLock sl(mutex_);
    for (const auto& slot : *activeChainOwner_)
        if (slot != nullptr && slot->instance != nullptr) slot->instance->releaseResources();
}

void HostedPluginChainManager::setNonRealtime(bool nonRealtime) noexcept
{
    // Never lock here (AUDIT-004): the VST3 wrapper calls this on the audio
    // thread every block, while the control lock can be held for seconds
    // across a hosted plugin's prepare / state calls. processBlock() pushes
    // the flag to the instances it runs; prepareSlotLocked() to new ones.
    nonRealtime_.store(nonRealtime, std::memory_order_relaxed);
}

int HostedPluginChainManager::getPreparedBlockSize() const noexcept
{
    const juce::ScopedLock sl(mutex_);
    return blockSize_;
}

double HostedPluginChainManager::getPreparedSampleRate() const noexcept
{
    const juce::ScopedLock sl(mutex_);
    return sampleRate_;
}

bool configureHostedLayout(juce::AudioPluginInstance& instance, int channels)
{
    // Some VST3s come up with every bus disabled (0-in/0-out) and would be
    // silently skipped. Enable the main buses only (enabling sidechains
    // would demand more channels than the buffer has) and ask for the
    // session's channel count; keep the plugin's default if it refuses.
    const auto set = channels >= 2 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono();
    const auto current = instance.getBusesLayout();
    auto layout = current;
    if (layout.inputBuses.size() > 0) layout.inputBuses.getReference(0) = set;
    if (layout.outputBuses.size() > 0) layout.outputBuses.getReference(0) = set;
    auto withoutAux = layout;
    for (int i = 1; i < withoutAux.inputBuses.size(); ++i)
        withoutAux.inputBuses.getReference(i) = juce::AudioChannelSet::disabled();
    for (int i = 1; i < withoutAux.outputBuses.size(); ++i)
        withoutAux.outputBuses.getReference(i) = juce::AudioChannelSet::disabled();
    // Prefer the layout with sidechains off; a plugin that insists on them
    // keeps them and runs through the slot's wide scratch instead.
    for (const auto& candidate : {withoutAux, layout}) {
        if (candidate == current) return false;
        if (instance.checkBusesLayoutSupported(candidate) && instance.setBusesLayout(candidate)) return true;
    }
    return false;
}

int HostedPluginChainManager::processBlock(juce::AudioBuffer<float>& buffer, bool bypassed,
                                           juce::AudioPlayHead* playHead)
{
    int activeLatency = 0;
    // Increment before loading the raw snapshot. Along with the sequentially
    // consistent publication/reap operations, this prevents a reaper from
    // retiring a snapshot while a callback could still observe it.
    audioReaders_.fetch_add(1);
    const auto* chain = bypassed ? nullptr : activeChain_.load();
    const int totalSamples = buffer.getNumSamples();
    if (chain != nullptr && !chain->empty() && totalSamples > 0) {
        // Hosted plugins see the host's transport (audit L9) and offline flag
        // (AUDIT-004: applied here from the snapshot, never under the control
        // lock). Both are plain stores in JUCE's AudioProcessor; only write
        // them when they changed.
        const bool nonRealtime = nonRealtime_.load(std::memory_order_relaxed);
        for (const auto& slot : *chain) {
            if (slot == nullptr || slot->instance == nullptr) continue;
            if (slot->instance->getPlayHead() != playHead) slot->instance->setPlayHead(playHead);
            if (slot->instance->isNonRealtime() != nonRealtime) slot->instance->setNonRealtime(nonRealtime);
        }

        const int numChannels = buffer.getNumChannels();
        const int maxBlock = juce::jmax(1, blockSize_);
        const int mixChans = juce::jmin(numChannels, preparedChannels_, partialWetDry_.getNumChannels(), 2);

        // A host block larger than the prepared size is split into sub-blocks
        // of at most blockSize_ (audit M4): hosted instances must never see
        // more samples than they were prepared for. The non-owning view uses
        // AudioBuffer's inline channel table - no allocation for < 32 chans.
        for (int start = 0; start < totalSamples; start += maxBlock) {
            const int numSamples = juce::jmin(maxBlock, totalSamples - start);
            juce::AudioBuffer<float> block(buffer.getArrayOfWritePointers(), numChannels, start, numSamples);
            const bool firstBlock = start == 0;

            for (const auto& slot : *chain) {
                // A bypassed slot still runs (FIX-14): its wet ramps to 0, so
                // it crossfades to the dry signal, which stays delayed by the
                // slot's latency - the audio and the reported latency stay put.
                if (slot == nullptr || slot->instance == nullptr) continue;
                if (!canRunAt(*slot, numChannels, numSamples)) continue;
                if (firstBlock) slot->dryCompDelay.adoptPending(); // FIX-14: a grown line
                const bool slotBypassed = slot->bypassed.load(std::memory_order_relaxed);

                // Each slot starts from an empty MIDI buffer: one plugin's
                // MIDI output must not feed the next (audit L8).
                midi_.clear();

                // (A dry scratch smaller than the sub-block only happens
                // before the first prepare; run fully wet then, or not at
                // all when bypassed - there is no dry line to fade to.)
                if (numSamples > partialWetDry_.getNumSamples() || mixChans <= 0) {
                    if (!slotBypassed && runInstance(*slot, block, midi_) && firstBlock)
                        activeLatency += audioCompensatedLatency(*slot);
                    continue;
                }

                for (int c = 0; c < mixChans; ++c)
                    partialWetDry_.copyFrom(c, 0, block, c, 0, numSamples);

                if (!runInstance(*slot, block, midi_)) continue;

                // The dry side is ALWAYS fed and delayed by the slot latency
                // (AUDIT-061): skipping it near 0 % / 100 % wet replayed stale
                // history when the control moved back, and made the reported
                // latency jump mid-drag.
                const int slotLatency = audioCompensatedLatency(*slot);
                for (int c = 0; c < mixChans; ++c)
                    slot->dryCompDelay.process(c, partialWetDry_.getWritePointer(c), numSamples, slotLatency);

                // Per-sample smoothed wet: a moving control no longer steps
                // once per block (zipper), and bypass fades rather than
                // switching (FIX-14).
                auto& wet = slot->wetSmoothed;
                wet.setTargetValue(slotBypassed ? 0.0f : slot->wetMix.load(std::memory_order_relaxed));
                if (wet.isSmoothing() || wet.getCurrentValue() < 1.0f) {
                    float* out[2] = {block.getWritePointer(0), mixChans > 1 ? block.getWritePointer(1) : nullptr};
                    const float* dryIn[2] = {partialWetDry_.getReadPointer(0),
                                             mixChans > 1 ? partialWetDry_.getReadPointer(1) : nullptr};
                    for (int i = 0; i < numSamples; ++i) {
                        const float w = wet.getNextValue();
                        for (int c = 0; c < mixChans; ++c) out[c][i] = dryIn[c][i] + w * (out[c][i] - dryIn[c][i]);
                    }
                }
                if (firstBlock) activeLatency += slotLatency;
            }
        }
    }

    // Any snapshot retired before this increment can no longer be executing
    // on the audio thread.
    audioReaders_.fetch_sub(1);
    epoch_.fetch_add(1, std::memory_order_release);
    return activeLatency;
}

void HostedPluginChainManager::pruneCompletedRetirements(uint64_t completedEpoch)
{
    if (audioReaders_.load() != 0) return;

    retiredChains_.erase(
        std::remove_if(retiredChains_.begin(), retiredChains_.end(),
                       [completedEpoch](const RetiredChain& retired) {
                           return completedEpoch >= retired.releaseAfterEpoch;
                       }),
        retiredChains_.end());
}

void HostedPluginChainManager::publishReplacement(std::shared_ptr<Chain> replacement, uint64_t completedEpoch)
{
    retiredChains_.push_back({std::move(activeChainOwner_), completedEpoch + 1, reaperTick_});
    activeChainOwner_ = std::move(replacement);
    activeChain_.store(activeChainOwner_.get());
}

void HostedPluginChainManager::appendSlot(std::shared_ptr<Slot> slot)
{
    const juce::ScopedLock sl(mutex_);
    const auto completedEpoch = epoch_.load(std::memory_order_acquire);
    pruneCompletedRetirements(completedEpoch);
    auto replacement = std::make_shared<Chain>(*activeChainOwner_);
    replacement->push_back(std::move(slot));
    publishReplacement(std::move(replacement), completedEpoch);
}

void HostedPluginChainManager::addPluginInstance(std::unique_ptr<juce::AudioPluginInstance> instance,
                                                 const juce::String& name, const juce::String& filePath,
                                                 bool bypassed, float wetMix,
                                                 const juce::MemoryBlock* pluginState)
{
    if (instance == nullptr) return;

    auto slot = std::make_shared<Slot>();
    slot->name = name;
    slot->filePath = filePath;
    slot->instance = std::move(instance);
    slot->bypassed.store(bypassed, std::memory_order_relaxed);
    slot->wetMix.store(sanitizeWetMix(wetMix), std::memory_order_relaxed);

    // Held across prepare + publish so a concurrent prepareToPlay cannot
    // change the rate between preparing this instance and publishing it.
    const juce::ScopedLock sl(mutex_);
    slot->id = nextSlotId_++;

    // Always prepared here, at the CURRENT prepared rate/block size: an
    // async load was created with the values captured at request time, and
    // prepareToPlay() only re-prepares slots already in the chain (audit M7).
    prepareSlotLocked(*slot);
    slot->listener = latencyListener_.get();
    slot->instance->addListener(slot->listener);

    // A hosted plugin owns its opaque state contract. It must be restored only
    // after prepareToPlay(), and only from the message/control thread.
    if (pluginState != nullptr && pluginState->getSize() != 0)
        slot->instance->setStateInformation(pluginState->getData(), static_cast<int>(pluginState->getSize()));

    appendSlot(std::move(slot));
}

void HostedPluginChainManager::addPlaceholder(const PersistedSlot& saved)
{
    auto slot = std::make_shared<Slot>();
    slot->name = saved.name.isNotEmpty() ? saved.name : juce::File(saved.filePath).getFileNameWithoutExtension();
    slot->filePath = saved.filePath;
    slot->placeholderState = saved.pluginState;
    slot->bypassed.store(saved.bypassed, std::memory_order_relaxed);
    slot->wetMix.store(sanitizeWetMix(saved.wetMix), std::memory_order_relaxed);

    const juce::ScopedLock sl(mutex_);
    slot->id = nextSlotId_++;
    appendSlot(std::move(slot));
}

void HostedPluginChainManager::removePlugin(int slotIndex)
{
    std::shared_ptr<Slot> slotToDelete;
    {
        const juce::ScopedLock sl(mutex_);
        const auto completedEpoch = epoch_.load(std::memory_order_acquire);
        pruneCompletedRetirements(completedEpoch);
        if (slotIndex >= 0 && slotIndex < static_cast<int>(activeChainOwner_->size())) {
            auto replacement = std::make_shared<Chain>(*activeChainOwner_);
            slotToDelete = (*replacement)[static_cast<size_t>(slotIndex)];
            replacement->erase(replacement->begin() + slotIndex);
            publishReplacement(std::move(replacement), completedEpoch);
        }
    }

    if (slotToDelete != nullptr) releaseOnMessageThread(std::move(slotToDelete));
}

bool HostedPluginChainManager::moveSlot(int fromIndex, int toIndex)
{
    const juce::ScopedLock sl(mutex_);
    const int size = static_cast<int>(activeChainOwner_->size());
    if (fromIndex < 0 || fromIndex >= size || toIndex < 0 || toIndex >= size || fromIndex == toIndex) return false;
    const auto completedEpoch = epoch_.load(std::memory_order_acquire);
    pruneCompletedRetirements(completedEpoch);
    auto replacement = std::make_shared<Chain>(*activeChainOwner_);
    auto moved = (*replacement)[static_cast<size_t>(fromIndex)];
    replacement->erase(replacement->begin() + fromIndex);
    replacement->insert(replacement->begin() + toIndex, std::move(moved));
    publishReplacement(std::move(replacement), completedEpoch);
    return true;
}

std::shared_ptr<juce::AudioPluginInstance> HostedPluginChainManager::getInstanceForSlotId(uint64_t slotId) const
{
    const juce::ScopedLock sl(mutex_);
    for (const auto& slot : *activeChainOwner_)
        if (slot != nullptr && slot->id == slotId) return slot->instance;
    return nullptr;
}

void HostedPluginChainManager::clear()
{
    std::shared_ptr<Chain> chainToClear;
    {
        const juce::ScopedLock sl(mutex_);
        // Nothing to retire: every state restore calls clear(), and retiring
        // an empty snapshot each time piled past kMaxRetiredChains when a host
        // restored state repeatedly without processing audio (pluginval's
        // state-restoration test; caught by PluginvalStateRestorationTest in Debug).
        if (activeChainOwner_->empty()) return;
        const auto completedEpoch = epoch_.load(std::memory_order_acquire);
        chainToClear = std::move(activeChainOwner_);
        activeChainOwner_ = std::make_shared<Chain>();
        activeChain_.store(activeChainOwner_.get());
        retiredChains_.push_back({chainToClear, completedEpoch + 1, reaperTick_});
    }

    if (chainToClear != nullptr && !chainToClear->empty()) releaseOnMessageThread(std::move(chainToClear));
}

void HostedPluginChainManager::setBypass(int slotIndex, bool bypassed)
{
    const juce::ScopedLock sl(mutex_);
    if (slotIndex >= 0 && slotIndex < static_cast<int>(activeChainOwner_->size())
        && (*activeChainOwner_)[static_cast<size_t>(slotIndex)] != nullptr)
        (*activeChainOwner_)[static_cast<size_t>(slotIndex)]->bypassed.store(bypassed, std::memory_order_relaxed);
}

void HostedPluginChainManager::setWetMix(int slotIndex, float wetMix)
{
    if (!std::isfinite(wetMix)) return; // a NaN control value is not a request
    const juce::ScopedLock sl(mutex_);
    if (slotIndex >= 0 && slotIndex < static_cast<int>(activeChainOwner_->size())
        && (*activeChainOwner_)[static_cast<size_t>(slotIndex)] != nullptr)
        (*activeChainOwner_)[static_cast<size_t>(slotIndex)]->wetMix.store(sanitizeWetMix(wetMix),
                                                                           std::memory_order_relaxed);
}

int HostedPluginChainManager::getNumPlugins() const noexcept
{
    const juce::ScopedLock sl(mutex_);
    return static_cast<int>(activeChainOwner_->size());
}

void HostedPluginChainManager::reserveDryLines()
{
    const juce::ScopedLock sl(mutex_);
    if (activeChainOwner_ == nullptr) return;
    const int cap = static_cast<int>(std::ceil(sampleRate_ * kMaxCompensatedLatencySeconds));
    for (const auto& slot : *activeChainOwner_)
        if (slot != nullptr && slot->instance != nullptr)
            slot->dryCompDelay.reserve(juce::jmin(cap, juce::jmax(0, slot->instance->getLatencySamples())));
}

int HostedPluginChainManager::getActiveLatency() const noexcept
{
    const juce::ScopedLock sl(mutex_);
    int totalLatency = 0;
    for (const auto& slot : *activeChainOwner_) {
        if (slot != nullptr && canRunAt(*slot, preparedChannels_, blockSize_))
            totalLatency += compensatedLatency(*slot);
    }
    return totalLatency;
}

double HostedPluginChainManager::getTailLengthSeconds() const
{
    // VST3's kInfiniteTail arrives as UINT32_MAX samples (hours); anything
    // this long is treated as "infinite".
    constexpr double kInfiniteThresholdSeconds = 600.0;
    const juce::ScopedLock sl(mutex_);
    double tail = 0.0;
    for (const auto& slot : *activeChainOwner_) {
        if (slot == nullptr || slot->bypassed.load(std::memory_order_relaxed)
            || !canRunAt(*slot, preparedChannels_, blockSize_))
            continue;
        const double slotTail = slot->instance->getTailLengthSeconds();
        if (!std::isfinite(slotTail) || slotTail > kInfiniteThresholdSeconds) return kInfiniteTailFallbackSeconds;
        tail = juce::jmax(tail, slotTail);
    }
    return tail;
}

std::vector<HostedPluginChainManager::PersistedSlot> HostedPluginChainManager::captureState() const
{
    std::vector<PersistedSlot> result;
    const juce::ScopedLock sl(mutex_);
    result.reserve(activeChainOwner_->size());
    for (const auto& slot : *activeChainOwner_) {
        if (slot == nullptr) continue;

        PersistedSlot persisted;
        persisted.name = slot->name;
        persisted.filePath = slot->filePath;
        persisted.bypassed = slot->bypassed.load(std::memory_order_relaxed);
        persisted.wetMix = slot->wetMix.load(std::memory_order_relaxed);
        // A placeholder keeps the state it was saved with (audit M5): saving
        // on a machine that lacks the plugin must not drop it from the project.
        if (slot->instance != nullptr)
            slot->instance->getStateInformation(persisted.pluginState);
        else
            persisted.pluginState = slot->placeholderState;
        result.push_back(std::move(persisted));
    }
    return result;
}

std::vector<HostedPluginChainManager::SlotInfo> HostedPluginChainManager::getSlotInfo() const
{
    std::vector<SlotInfo> result;
    const juce::ScopedLock sl(mutex_);
    result.reserve(activeChainOwner_->size());
    // One entry per chain element (null slots included) so a row index always
    // matches the slotIndex removePlugin()/setBypass()/setWetMix() take.
    for (const auto& slot : *activeChainOwner_) {
        SlotInfo info;
        if (slot != nullptr) {
            info.id = slot->id;
            info.name = slot->name;
            info.filePath = slot->filePath;
            info.bypassed = slot->bypassed.load(std::memory_order_relaxed);
            info.wetMix = slot->wetMix.load(std::memory_order_relaxed);
            info.missing = slot->isPlaceholder();
            if (slot->instance != nullptr) info.latencySamples = slot->instance->getLatencySamples();
        }
        result.push_back(std::move(info));
    }
    return result;
}

size_t HostedPluginChainManager::getNumRetiredChains() const
{
    const juce::ScopedLock sl(mutex_);
    return retiredChains_.size();
}

void HostedPluginChainManager::reap()
{
    std::vector<RetiredChain> toFree;
    {
        const juce::ScopedLock sl(mutex_);
        // Dry lines the audio thread replaced with bigger ones (FIX-14).
        if (activeChainOwner_ != nullptr)
            for (const auto& slot : *activeChainOwner_)
                if (slot != nullptr) slot->dryCompDelay.reapRetired();
        const auto epochNow = epoch_.load(std::memory_order_acquire);
        const bool noAudioReader = audioReaders_.load() == 0;
        const bool quiescent = (epochNow == reaperLastSeenEpoch_);
        const uint32_t tickNow = reaperTick_;
        auto releasable = [epochNow, noAudioReader, quiescent, tickNow](const RetiredChain& retired) {
            return noAudioReader
                   && (epochNow >= retired.releaseAfterEpoch || (quiescent && retired.retiredAtTick < tickNow));
        };
        auto it = std::stable_partition(retiredChains_.begin(), retiredChains_.end(),
                                        [&releasable](const RetiredChain& retired) { return !releasable(retired); });
        toFree.assign(std::make_move_iterator(it), std::make_move_iterator(retiredChains_.end()));
        retiredChains_.erase(it, retiredChains_.end());
        reaperLastSeenEpoch_ = epochNow;
        ++reaperTick_;
        jassert(retiredChains_.size() <= kMaxRetiredChains);
    }
    toFree.clear();
}

} // namespace khensu
