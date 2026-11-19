#include "PluginProcessor.h"
#include "CopilotServiceClient.h"
// The short pool's shutdown wait is sized for one ServiceClient request.
static_assert(khensu::copilot::ServiceClient::kShortRequestMs == khensu::KhensuCopilotProcessor::kShortJobTimeoutMs);
#include "PluginEditor.h"
#include "PluginCrashHandler.h"
#include <khensu/common/Version.h>
#if JucePlugin_Enable_ARA
#include "ara/KhensuAraDocumentController.h"
#endif
#include <khensu/recommendations/Recommendation.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>

namespace khensu {

namespace {
// Live processor instances in this module; the last one to go shuts down the
// shared-core static workers while JUCE is still alive (audit L1).
std::atomic<int> liveProcessorInstances{0};

// Unload budget for the background pools (AUDIT-029). Long jobs poll
// shouldExit() between requests of at most 3 s (CopilotServiceJobs), short
// jobs bound each request by kShortJobTimeoutMs; both budgets cover that
// worst case so ~ThreadPool never reaches its force-kill path in the host.
constexpr int kLongJobUnloadBudgetMs = 8000;
constexpr int kShortJobUnloadBudgetMs = 4000;

// Instance ids live in this process (PLG-04). A restored id that another live
// instance already uses (the host duplicated a track, copying its state) must
// not be adopted, or both would share one transport slot again.
std::mutex& instanceIdRegistryMutex()
{
    static std::mutex m;
    return m;
}
std::set<juce::String>& instanceIdRegistry()
{
    static std::set<juce::String> ids;
    return ids;
}

const juce::Identifier kStateVersionId{"khensuStateVersion"};
const juce::Identifier kInstanceIdProp{"khensuInstanceId"};
const juce::Identifier kMixRoleProp{"khensuMixRole"};
const juce::Identifier kBoundSessionProp{"khensuBoundSession"}; // WF-8

bool isBlockFinite(const juce::AudioBuffer<float>& buffer) noexcept
{
    for (int c = 0; c < buffer.getNumChannels(); ++c) {
        const float* data = buffer.getReadPointer(c);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (!std::isfinite(data[i])) return false;
    }
    return true;
}
} // namespace

KhensuCopilotProcessor::KhensuCopilotProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts_(*this, nullptr, "KhensuCopilotState", createParameterLayout())
{
    liveProcessorInstances.fetch_add(1);
    // FIX-7: the plugin's bridges prove who answers on the service port
    // themselves; armed here too so a host-side probe made before any
    // ServiceClient::bridge() call is already gated.
    copilot::ServiceClient::armIdentityGate();
    // CE-ENG-4: local crash records for faults inside this module (chained
    // in front of the host's handler, never replacing it; nothing uploaded).
    plugincrash::acquire(KHENSU_VERSION_STRING);

    // Create the weak-reference master here, on the message thread (audit
    // L12): JUCE initialises it lazily and unsynchronised, so the first
    // WeakReference must not be made on the transport thread. Every async
    // path copies this one.
    weakThis_ = this;

    for (auto& flag : aiSetFlags_) flag.store(false, std::memory_order_relaxed);

    {
        const std::lock_guard<std::mutex> lock(instanceIdRegistryMutex());
        do {
            instanceId_ = juce::Uuid().toDashedString();
        } while (!instanceIdRegistry().insert(instanceId_).second);
    }
    transportController_.setInstanceId(instanceId_);

    // A hosted plugin re-reporting its latency (possibly from its own audio
    // callback) re-reports ours from the message thread (AUDIT-026).
    // triggerAsyncUpdate posts a preallocated message - no allocation here.
    hostedPlugins_.setLatencyChangedCallback([this] { triggerAsyncUpdate(); });

    pHpfOn_ = apvts_.getRawParameterValue(ParamIDs::hpfOn);
    pHpfFreq_ = apvts_.getRawParameterValue(ParamIDs::hpfFreq);
    pLpfOn_ = apvts_.getRawParameterValue(ParamIDs::lpfOn);
    pLpfFreq_ = apvts_.getRawParameterValue(ParamIDs::lpfFreq);
    pEqOn_ = apvts_.getRawParameterValue(ParamIDs::eqOn);
    pEqFreq_ = apvts_.getRawParameterValue(ParamIDs::eqFreq);
    pEqGainDb_ = apvts_.getRawParameterValue(ParamIDs::eqGainDb);
    pEqQ_ = apvts_.getRawParameterValue(ParamIDs::eqQ);
    pEq2On_ = apvts_.getRawParameterValue(ParamIDs::eq2On);
    pEq2Freq_ = apvts_.getRawParameterValue(ParamIDs::eq2Freq);
    pEq2GainDb_ = apvts_.getRawParameterValue(ParamIDs::eq2GainDb);
    pEq2Q_ = apvts_.getRawParameterValue(ParamIDs::eq2Q);
    pEq3On_ = apvts_.getRawParameterValue(ParamIDs::eq3On);
    pEq3Freq_ = apvts_.getRawParameterValue(ParamIDs::eq3Freq);
    pEq3GainDb_ = apvts_.getRawParameterValue(ParamIDs::eq3GainDb);
    pEq3Q_ = apvts_.getRawParameterValue(ParamIDs::eq3Q);
    pCompOn_ = apvts_.getRawParameterValue(ParamIDs::compOn);
    pCompThreshDb_ = apvts_.getRawParameterValue(ParamIDs::compThreshDb);
    pCompRatio_ = apvts_.getRawParameterValue(ParamIDs::compRatio);
    pCompAttackMs_ = apvts_.getRawParameterValue(ParamIDs::compAttackMs);
    pCompReleaseMs_ = apvts_.getRawParameterValue(ParamIDs::compReleaseMs);
    pCompMultiband_ = apvts_.getRawParameterValue(ParamIDs::compMultiband);
    pCompLowThreshDb_ = apvts_.getRawParameterValue(ParamIDs::compLowThreshDb);
    pCompMidThreshDb_ = apvts_.getRawParameterValue(ParamIDs::compMidThreshDb);
    pCompHighThreshDb_ = apvts_.getRawParameterValue(ParamIDs::compHighThreshDb);
    pCrossoverLowFreq_ = apvts_.getRawParameterValue(ParamIDs::crossoverLowFreq);
    pCrossoverHighFreq_ = apvts_.getRawParameterValue(ParamIDs::crossoverHighFreq);
    pOutGainDb_ = apvts_.getRawParameterValue(ParamIDs::outGainDb);
    pLimOn_ = apvts_.getRawParameterValue(ParamIDs::limOn);
    pLimThreshDb_ = apvts_.getRawParameterValue(ParamIDs::limThreshDb);
    pChainMix_ = apvts_.getRawParameterValue(ParamIDs::chainMix);
    pBypass_ = apvts_.getRawParameterValue(ParamIDs::bypass);

    // Any change to an AI-set parameter that did not come from setParamFromAI
    // clears its AI dot (PLG-20): typed values, arrow keys, host automation
    // and desktop pushes, not only a mouse drag on the knob.
    const auto& allParameters = getParameters();
    parameterToDspIndex_.assign(static_cast<size_t>(allParameters.size()), -1);
    for (int i = 0; i < kNumDspParams; ++i)
        if (auto* parameter = apvts_.getParameter(kDspParamIds[static_cast<size_t>(i)])) {
            const int index = parameter->getParameterIndex();
            if (index >= 0 && index < static_cast<int>(parameterToDspIndex_.size()))
                parameterToDspIndex_[static_cast<size_t>(index)] = i;
            parameter->addListener(this);
        }

    CopilotCorrectiveDsp::Parameters dspParams;
    dspParams.chainMix = pChainMix_;
    dspParams.hpfOn = pHpfOn_;
    dspParams.hpfFreq = pHpfFreq_;
    dspParams.lpfOn = pLpfOn_;
    dspParams.lpfFreq = pLpfFreq_;
    dspParams.eqOn = pEqOn_;
    dspParams.eqFreq = pEqFreq_;
    dspParams.eqGainDb = pEqGainDb_;
    dspParams.eqQ = pEqQ_;
    dspParams.eq2On = pEq2On_;
    dspParams.eq2Freq = pEq2Freq_;
    dspParams.eq2GainDb = pEq2GainDb_;
    dspParams.eq2Q = pEq2Q_;
    dspParams.eq3On = pEq3On_;
    dspParams.eq3Freq = pEq3Freq_;
    dspParams.eq3GainDb = pEq3GainDb_;
    dspParams.eq3Q = pEq3Q_;
    dspParams.compOn = pCompOn_;
    dspParams.compThreshDb = pCompThreshDb_;
    dspParams.compRatio = pCompRatio_;
    dspParams.compAttackMs = pCompAttackMs_;
    dspParams.compReleaseMs = pCompReleaseMs_;
    dspParams.compMultiband = pCompMultiband_;
    dspParams.compLowThreshDb = pCompLowThreshDb_;
    dspParams.compMidThreshDb = pCompMidThreshDb_;
    dspParams.compHighThreshDb = pCompHighThreshDb_;
    dspParams.crossoverLowFreq = pCrossoverLowFreq_;
    dspParams.crossoverHighFreq = pCrossoverHighFreq_;
    dspParams.outGainDb = pOutGainDb_;
    correctiveDsp_.bindParameters(dspParams);

    // Transport sync: owned worker POSTs playhead + meter state at ~5 Hz so the
    // desktop app can follow the DAW. Joined in the destructor - no detached
    // threads, no use-after-free window. It stays idle until prepareToPlay has
    // run once, so DAW plugin scans don't generate HTTP traffic.
    transportController_.startThread();

    // Retirement reaper (audit ARCH-04): frees retired chain snapshots from
    // the message thread instead of waiting for the next chain mutation.
    startTimer(kReaperPeriodMs);
}

KhensuCopilotProcessor::~KhensuCopilotProcessor()
{
    plugincrash::release();
    stopTimer();
    fixAutomationTimer_.stopTimer();
    fixWriter_.cancel(gestureSink_); // close any open host gesture
    liveAnalyzer_.stop();
    cancelPendingUpdate();
    closeAllHostedEditors();
    for (int i = 0; i < kNumDspParams; ++i)
        if (auto* parameter = apvts_.getParameter(kDspParamIds[static_cast<size_t>(i)])) parameter->removeListener(this);
    {
        const std::lock_guard<std::mutex> lock(instanceIdRegistryMutex());
        const juce::ScopedLock sl(instanceIdLock_);
        instanceIdRegistry().erase(instanceId_);
    }

    // Join editor background jobs before members go (audit H3, AUDIT-029).
    // Signal BOTH pools first so long and short jobs wind down in parallel,
    // then wait for each within its budget. Every long job polls
    // ThreadPoolJob::shouldExit() between <= 3 s requests and cancels its
    // service-side job; short jobs use <= 2.5 s requests - so the waits end
    // well inside the budget instead of ~ThreadPool killing a host thread.
    backgroundPool_.removeAllJobs(true, 0);
    shortJobPool_.removeAllJobs(true, 0);
    if (!backgroundPool_.removeAllJobs(true, kLongJobUnloadBudgetMs))
        juce::Logger::writeToLog("Khensu Copilot: a long background job did not stop within the unload budget");
    if (!shortJobPool_.removeAllJobs(true, kShortJobUnloadBudgetMs))
        juce::Logger::writeToLog("Khensu Copilot: a short background job did not stop within the unload budget");

    // Join the sync worker before members are destroyed. The worker's wait()
    // is interrupted by stopThread's notify; an in-flight HTTP call has a 1 s
    // timeout, so 4 s covers the worst case.
    transportController_.stopThread(4000);

    if (liveProcessorInstances.fetch_sub(1) == 1) ipc::HttpServiceBridge::shutdownBackgroundWorkers();
}

juce::AudioProcessorValueTreeState::ParameterLayout KhensuCopilotProcessor::createParameterLayout()
{
    using FloatParam = juce::AudioParameterFloat;
    using BoolParam = juce::AudioParameterBool;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    auto logRange = [](float lo, float hi) { return juce::NormalisableRange<float>(lo, hi, 0.0f, 0.35f); };
    // Continuous (log) ranges print 2 decimals so value -> text -> value ->
    // text round-trips exactly (clap-validator param-conversions, CE-DAW-2);
    // the default 7-decimal text drifted in the last digit.
    const auto logAttributes = juce::AudioParameterFloatAttributes()
                                   .withStringFromValueFunction([](float v, int) { return juce::String(v, 2); })
                                   .withValueFromStringFunction([](const juce::String& t) { return t.getFloatValue(); });

    layout.add(std::make_unique<BoolParam>(ParamIDs::hpfOn, "HPF On", false));
    layout.add(std::make_unique<FloatParam>(ParamIDs::hpfFreq, "HPF Freq", logRange(20.0f, 1000.0f), 80.0f, logAttributes));
    layout.add(std::make_unique<BoolParam>(ParamIDs::lpfOn, "LPF On", false));
    layout.add(std::make_unique<FloatParam>(ParamIDs::lpfFreq, "LPF Freq", logRange(1000.0f, 20000.0f), 18000.0f, logAttributes));
    layout.add(std::make_unique<BoolParam>(ParamIDs::eqOn, "EQ On", false));
    layout.add(std::make_unique<FloatParam>(ParamIDs::eqFreq, "EQ Freq", logRange(60.0f, 16000.0f), 1000.0f, logAttributes));
    layout.add(std::make_unique<FloatParam>(ParamIDs::eqGainDb, "EQ Gain",
                                            juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));
    layout.add(
        std::make_unique<FloatParam>(ParamIDs::eqQ, "EQ Q", juce::NormalisableRange<float>(0.3f, 8.0f, 0.01f), 1.0f));
    layout.add(std::make_unique<BoolParam>(ParamIDs::eq2On, "EQ2 On", false));
    layout.add(std::make_unique<FloatParam>(ParamIDs::eq2Freq, "EQ2 Freq", logRange(60.0f, 16000.0f), 1000.0f, logAttributes));
    layout.add(std::make_unique<FloatParam>(ParamIDs::eq2GainDb, "EQ2 Gain",
                                            juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));
    layout.add(
        std::make_unique<FloatParam>(ParamIDs::eq2Q, "EQ2 Q", juce::NormalisableRange<float>(0.3f, 8.0f, 0.01f), 1.0f));
    layout.add(std::make_unique<BoolParam>(ParamIDs::eq3On, "EQ3 On", false));
    layout.add(std::make_unique<FloatParam>(ParamIDs::eq3Freq, "EQ3 Freq", logRange(60.0f, 16000.0f), 1000.0f, logAttributes));
    layout.add(std::make_unique<FloatParam>(ParamIDs::eq3GainDb, "EQ3 Gain",
                                            juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));
    layout.add(
        std::make_unique<FloatParam>(ParamIDs::eq3Q, "EQ3 Q", juce::NormalisableRange<float>(0.3f, 8.0f, 0.01f), 1.0f));
    layout.add(std::make_unique<BoolParam>(ParamIDs::compOn, "Comp On", false));
    layout.add(std::make_unique<FloatParam>(ParamIDs::compThreshDb, "Comp Threshold",
                                            juce::NormalisableRange<float>(-48.0f, 0.0f, 0.1f), -18.0f));
    layout.add(std::make_unique<FloatParam>(ParamIDs::compRatio, "Comp Ratio",
                                            juce::NormalisableRange<float>(1.0f, 20.0f, 0.1f), 4.0f));
    layout.add(std::make_unique<FloatParam>(ParamIDs::compAttackMs, "Comp Attack", logRange(0.1f, 200.0f), 5.0f, logAttributes));
    layout.add(std::make_unique<FloatParam>(ParamIDs::compReleaseMs, "Comp Release", logRange(5.0f, 1000.0f), 100.0f, logAttributes));
    layout.add(std::make_unique<BoolParam>(ParamIDs::compMultiband, "Comp Multiband", false));
    layout.add(std::make_unique<FloatParam>(ParamIDs::compLowThreshDb, "Comp Low Threshold",
                                            juce::NormalisableRange<float>(-48.0f, 0.0f, 0.1f), -18.0f));
    layout.add(std::make_unique<FloatParam>(ParamIDs::compMidThreshDb, "Comp Mid Threshold",
                                            juce::NormalisableRange<float>(-48.0f, 0.0f, 0.1f), -18.0f));
    layout.add(std::make_unique<FloatParam>(ParamIDs::compHighThreshDb, "Comp High Threshold",
                                            juce::NormalisableRange<float>(-48.0f, 0.0f, 0.1f), -18.0f));
    layout.add(std::make_unique<FloatParam>(ParamIDs::crossoverLowFreq, "Crossover Low Freq", logRange(20.0f, 1000.0f),
                                            200.0f, logAttributes));
    layout.add(std::make_unique<FloatParam>(ParamIDs::crossoverHighFreq, "Crossover High Freq",
                                            logRange(1000.0f, 20000.0f), 2500.0f, logAttributes));
    layout.add(std::make_unique<FloatParam>(ParamIDs::outGainDb, "Output Gain",
                                            juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f), 0.0f));
    layout.add(std::make_unique<BoolParam>(ParamIDs::limOn, "Limiter On", false));
    layout.add(std::make_unique<FloatParam>(ParamIDs::limThreshDb, "Limiter Threshold",
                                            juce::NormalisableRange<float>(-12.0f, 0.0f, 0.1f), -1.0f));
    // Chain-level dry/wet - 1.0 = fully processed. Lower values blend the
    // untouched input back in (true parallel processing: the chain is
    // zero-latency, so dry and wet stay sample-aligned).
    layout.add(std::make_unique<FloatParam>(ParamIDs::chainMix, "Chain Mix",
                                            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));
    layout.add(std::make_unique<BoolParam>(ParamIDs::bypass, "Global Bypass", false));
    return layout;
}

bool KhensuCopilotProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // The DSP chain is prepared for 1 or 2 channels with matched in/out -
    // anything else (5.1, quad) would index past the prepared filter states.
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo();
}

void KhensuCopilotProcessor::setParamFromUi(const char* paramId, float plainValue)
{
    if (auto* p = apvts_.getParameter(paramId)) {
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(plainValue));
        p->endChangeGesture();
    }
}

int KhensuCopilotProcessor::dspParamIndex(const char* paramId) noexcept
{
    for (int i = 0; i < kNumDspParams; ++i)
        if (std::strcmp(kDspParamIds[static_cast<size_t>(i)], paramId) == 0) return i;
    return -1;
}

bool KhensuCopilotProcessor::setAiFlag(int index, bool value) noexcept
{
    if (index < 0 || index >= kNumDspParams) return false;
    return aiSetFlags_[static_cast<size_t>(index)].exchange(value, std::memory_order_relaxed) != value;
}

void KhensuCopilotProcessor::setParamFromAI(const char* paramId, float plainValue)
{
    // The value change clears the flag through parameterValueChanged; set it
    // again afterwards so it ends up AI-set.
    setParamFromUi(paramId, plainValue);
    const int idx = dspParamIndex(paramId);
    if (idx >= 0) {
        setAiFlag(idx, true);
        markNonParameterStateChanged();
    }
}

void KhensuCopilotProcessor::applyAiParameterSet(const std::map<std::string, float>& params)
{
    // Stage switches the new set does not mention are turned off, so an
    // earlier Apply's stages do not stay on underneath this one (PLG-19).
    static constexpr const char* kStageSwitches[] = {ParamIDs::hpfOn, ParamIDs::lpfOn,  ParamIDs::eqOn,
                                                     ParamIDs::eq2On, ParamIDs::eq3On,  ParamIDs::compOn,
                                                     ParamIDs::compMultiband, ParamIDs::limOn};
    for (const char* id : kStageSwitches)
        if (params.count(id) == 0) {
            setParamFromUi(id, 0.0f);
            clearAiSet(id);
        }
    if (params.count(ParamIDs::outGainDb) == 0) {
        setParamFromUi(ParamIDs::outGainDb, 0.0f);
        clearAiSet(ParamIDs::outGainDb);
    }
    for (const auto& [id, value] : params) setParamFromAI(id.c_str(), value);
}

void KhensuCopilotProcessor::adjustOutGainFromAi(float deltaDb)
{
    if (!std::isfinite(deltaDb) || pOutGainDb_ == nullptr) return;
    setParamFromAI(ParamIDs::outGainDb, pOutGainDb_->load(std::memory_order_relaxed) + deltaDb);
}

void KhensuCopilotProcessor::parameterValueChanged(int parameterIndex, float)
{
    // Any thread (host automation arrives on the audio thread): atomics and
    // a preallocated async message only.
    if (parameterIndex < 0 || parameterIndex >= static_cast<int>(parameterToDspIndex_.size())) return;
    const int idx = parameterToDspIndex_[static_cast<size_t>(parameterIndex)];
    if (idx >= 0 && setAiFlag(idx, false)) markNonParameterStateChanged();
}

void KhensuCopilotProcessor::markNonParameterStateChanged() noexcept
{
    // Loading a state is not an edit: the host must not see the project as
    // modified by the load itself.
    if (restoringState_.load(std::memory_order_acquire)) return;
    if (juce::MessageManager::existsAndIsCurrentThread()) {
        updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
        return;
    }
    pendingNonParameterChange_.store(true, std::memory_order_release);
    triggerAsyncUpdate();
}

void KhensuCopilotProcessor::setMixHubRoleOverride(const juce::String& role)
{
    if (mixHubTap_.getRoleOverride() == role) return;
    mixHubTap_.setRoleOverride(role);
    markNonParameterStateChanged();
}

juce::String KhensuCopilotProcessor::getInstanceId() const
{
    const juce::ScopedLock sl(instanceIdLock_);
    return instanceId_;
}

void KhensuCopilotProcessor::adoptInstanceId(const juce::String& restoredId)
{
    if (restoredId.trim().isEmpty()) return;
    {
        const std::lock_guard<std::mutex> lock(instanceIdRegistryMutex());
        const juce::ScopedLock sl(instanceIdLock_);
        // Saved verbatim even when not adopted, so a state round-trip is
        // byte-stable (pluginval / host duplicate checks); uniqueness is a
        // runtime property re-decided at every load.
        persistedInstanceId_ = restoredId;
        if (restoredId == instanceId_) return;
        if (!instanceIdRegistry().insert(restoredId).second) return; // a duplicated track: keep our own id
        instanceIdRegistry().erase(instanceId_);
        instanceId_ = restoredId;
    }
    transportController_.setInstanceId(restoredId);
}

void KhensuCopilotProcessor::migrateState(juce::ValueTree& state, int fromVersion)
{
    // v1 -> v2: the instance id and Mix role are new optional properties;
    // nothing existing changed shape. Future migrations go here, in order.
    if (fromVersion < 2) {
        // (no-op: v1 states simply lack the new properties)
    }
    state.setProperty(kStateVersionId, kStateVersion, nullptr);
}

bool KhensuCopilotProcessor::isAiSet(const char* paramId) const noexcept
{
    const int idx = dspParamIndex(paramId);
    return idx >= 0 && aiSetFlags_[static_cast<size_t>(idx)].load(std::memory_order_relaxed);
}

void KhensuCopilotProcessor::clearAiSet(const char* paramId) noexcept
{
    if (setAiFlag(dspParamIndex(paramId), false)) markNonParameterStateChanged();
}

void KhensuCopilotProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    currentSampleRate_ = sampleRate;
    preparedChannels_ = juce::jlimit(1, 2, getTotalNumOutputChannels());
    realtimeTelemetry_.prepareToPlay(sampleRate, preparedChannels_);
    correctiveDsp_.prepareToPlay(sampleRate, samplesPerBlock, preparedChannels_);

    outputStage_.prepareToPlay(sampleRate, samplesPerBlock, preparedChannels_);
    outputStage_.snapBypass(pBypass_->load(std::memory_order_relaxed) > 0.5f);
    preparedBlockSize_ = juce::jmax(1, samplesPerBlock);

    // Re-prepares every hosted instance at the host's real rate/block size,
    // then reports lookahead + hosted latency (setting only the lookahead
    // here dropped the hosted latency until the next chain edit).
    hostedPlugins_.prepareToPlay(sampleRate, samplesPerBlock, preparedChannels_, isNonRealtime());
    mixHubTap_.prepare(sampleRate);
    updateLatency();
#if JucePlugin_Enable_ARA
    prepareToPlayForARA(sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
#endif
    liveAnalyzer_.start(); // CE-DAW-3 worker; idle until the host plays

    preparedOnce_.store(true, std::memory_order_release);
}

void KhensuCopilotProcessor::releaseResources()
{
    hostedPlugins_.releaseResources();
#if JucePlugin_Enable_ARA
    releaseResourcesForARA();
#endif
}

void KhensuCopilotProcessor::setNonRealtime(bool isNonRealtime) noexcept
{
    // The JUCE VST3 wrapper calls this from the audio callback on EVERY
    // process call, so it must never lock (AUDIT-004). Early-out when the
    // flag is unchanged; the hosted chain only records it atomically and
    // applies it to its instances from processBlock (audit L9).
    if (nonRealtimeFlag_.exchange(isNonRealtime, std::memory_order_relaxed) == isNonRealtime
        && AudioProcessor::isNonRealtime() == isNonRealtime)
        return;
    AudioProcessor::setNonRealtime(isNonRealtime);
    hostedPlugins_.setNonRealtime(isNonRealtime);
}

void KhensuCopilotProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/)
{
    processBlockInternal(buffer, pBypass_->load(std::memory_order_relaxed) > 0.5f);
}

void KhensuCopilotProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midi*/)
{
    // Host bypass. JUCE's default passes audio through with ZERO delay while
    // the plugin reports lookahead + hosted latency, so bypassed audio came
    // out early. Run the latency-aligned bypass path instead.
    processBlockInternal(buffer, true);
}

juce::AudioProcessorParameter* KhensuCopilotProcessor::getBypassParameter() const
{
    // Hosts map their bypass button onto our own "bypass" parameter, instead
    // of the VST3 wrapper adding a second, unrelated bypass.
    return apvts_.getParameter(ParamIDs::bypass);
}

void KhensuCopilotProcessor::processBlockInternal(juce::AudioBuffer<float>& buffer, bool globalBypass)
{
    // THREAD_CONTEXT(AudioThread) - atomic writes; corrective DSP owns its
    // parameter smoothing and coefficient-update contract.
    juce::ScopedNoDenormals noDenormals;
    const int nSamples = buffer.getNumSamples();
    if (nSamples <= 0) return;

    const auto transport = realtimeTelemetry_.captureHostTransport(*this);
    // FEAT-7: a capture records what the host sends, before any processing.
    capture_.process(buffer, transport.playing);

#if JucePlugin_Enable_ARA
    // ARA (CE-DAW-1): the playback renderer passes the host's audio through
    // unchanged; the Copilot chain below then runs on it as usual.
    processBlockForARA(buffer, isRealtime(), getPlayHead());
#endif

    // A host block larger than prepareToPlay advertised is split ONCE here
    // into prepared-size chunks (AUDIT-027): every stage then runs on every
    // sample, instead of corrective DSP and the A/B / bypass alignment each
    // silently skipping the oversized block (a level jump and a misaligned
    // bypass). The non-owning views use AudioBuffer's inline channel table.
    const int maxBlock = preparedBlockSize_ > 0 ? preparedBlockSize_ : nSamples;
    if (nSamples <= maxBlock) {
        processChunk(buffer, globalBypass, transport);
        return;
    }
    for (int start = 0; start < nSamples; start += maxBlock) {
        juce::AudioBuffer<float> chunk(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), start,
                                       juce::jmin(maxBlock, nSamples - start));
        auto chunkTransport = transport;
        chunkTransport.timeSec += static_cast<double>(start) / juce::jmax(1.0, currentSampleRate_);
        processChunk(chunk, globalBypass, chunkTransport);
    }
}

void KhensuCopilotProcessor::processChunk(juce::AudioBuffer<float>& buffer, bool globalBypass,
                                          const CopilotRealtimeTelemetry::HostTransport& transport)
{
    const int nSamples = buffer.getNumSamples();
    // Offline renders always output the processed side (PLG-12).
    const int abMode = getEffectiveAbMode();
    outputStage_.captureDry(buffer, abMode);

    // Bypass crossfades (AUDIT-086): the chain keeps running until the fade
    // to the aligned dry signal has finished.
    const bool skipChain = !outputStage_.needsProcessedPath(globalBypass);

    correctiveDsp_.process(buffer, skipChain, nSamples);

    // ---- Hosted third-party plugin chain -------------------------------------
    // The manager owns snapshot publication, partial-wet compensation, and
    // retirement. Return the latency of the path that actually ran for A/B.
    const int activeHostedLatency = hostedPlugins_.processBlock(buffer, skipChain, getPlayHead());

    // NaN/Inf containment (AUDIT-025): a non-finite input or a misbehaving
    // hosted plugin would otherwise reach the host and leave the recursive
    // filter / limiter / level-match states poisoned until the next prepare.
    // Output silence for this block and restart every state we own.
    if (!isBlockFinite(buffer)) {
        buffer.clear();
        correctiveDsp_.reset();
        outputStage_.resetState();
    }

    outputStage_.process(buffer, globalBypass, pLimOn_->load(std::memory_order_relaxed) > 0.5f,
                         pLimThreshDb_->load(std::memory_order_relaxed), abMode, activeHostedLatency,
                         reportedHostedLatency_.load(std::memory_order_relaxed));

    realtimeTelemetry_.process(buffer);

    const auto meters = realtimeTelemetry_.snapshot(); // atomic loads
    mixHubTap_.pushBlock(buffer, meters.short_term_lufs, juce::jmax(meters.left_peak_db, meters.right_peak_db),
                         meters.width);

    // CE-DAW-3: hand what the host hears to the rolling analysis, stamped
    // with the host timeline position. Lock-free copy into a preallocated FIFO.
    liveAnalyzer_.pushBlock(buffer.getArrayOfReadPointers(), buffer.getNumChannels(), nSamples, transport.timeSec,
                            transport.timeValid, transport.playing, currentSampleRate_);
}

void KhensuCopilotProcessor::updateTrackProperties(const TrackProperties& properties)
{
    // Hosts call this on the message thread (JUCE VST3 wrapper: IInfoListener).
    mixHubTap_.setTrackName(properties.name.value_or(juce::String()));
}

void KhensuCopilotProcessor::setParamThreadSafe(const char* paramId, float plainValue)
{
    auto* parameter = apvts_.getParameter(paramId);
    auto* rawValue = apvts_.getRawParameterValue(paramId);
    if (parameter == nullptr || rawValue == nullptr) return;

    // The raw APVTS value is the audio-side handoff and is atomic. Do not call
    // setValueNotifyingHost from the transport worker: VST3 hosts may require
    // that notification on the plugin/message thread.
    const float normalized = parameter->convertTo0to1(plainValue);
    rawValue->store(parameter->convertFrom0to1(normalized), std::memory_order_relaxed);

    const juce::String id(paramId);
    const auto weakProcessor = weakThis_; // copy; never created off the message thread
    juce::MessageManager::callAsync([weakProcessor, id, normalized]() {
        auto* processor = weakProcessor.get();
        if (processor == nullptr) return;
        if (auto* messageThreadParameter = processor->apvts_.getParameter(id))
            messageThreadParameter->setValueNotifyingHost(normalized);
    });
}

juce::AudioProcessorEditor* KhensuCopilotProcessor::createEditor() { return new KhensuCopilotEditor(*this); }

void KhensuCopilotProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts_.copyState();
    state.setProperty(kStateVersionId, kStateVersion, nullptr);
    {
        const juce::ScopedLock sl(instanceIdLock_);
        state.setProperty(kInstanceIdProp, persistedInstanceId_.isNotEmpty() ? persistedInstanceId_ : instanceId_,
                          nullptr);
    }
    state.setProperty(kMixRoleProp, mixHubTap_.getRoleOverride(), nullptr);
    state.setProperty(kBoundSessionProp, juce::String(analysisController_.boundSessionId()), nullptr);

    // APVTS restores the full parent tree, so remove an older embedded
    // snapshot before writing the current hosted chain below.
    const juce::Identifier hostedChainType{"KhensuHostedChain"};
    const auto oldHostedChain = state.getChildWithName(hostedChainType);
    if (oldHostedChain.isValid()) state.removeChild(oldHostedChain, nullptr);

    // AI-set flags were previously in-memory only (lost every project
    // reload, even though the DSP values themselves persist via APVTS) -
    // stored here as a comma-separated list of param IDs currently AI-set.
    juce::StringArray aiSetIds;
    for (int i = 0; i < kNumDspParams; ++i)
        if (aiSetFlags_[(size_t)i].load(std::memory_order_relaxed)) aiSetIds.add(kDspParamIds[(size_t)i]);
    state.setProperty("khensuAiSetParams", aiSetIds.joinIntoString(","), nullptr);

    // APVTS stores a bool parameter as its snapped 0/1, but a host may write
    // any normalised value (0.55) and its own parameter cache keeps it - so a
    // reload left host and plugin disagreeing (pluginval strictness 10: "not
    // restored on setStateInformation"). Keep the exact raw values too.
    juce::StringArray boolRaw;
    for (auto* parameter : getParameters())
        if (auto* boolParam = dynamic_cast<juce::AudioParameterBool*>(parameter))
            boolRaw.add(boolParam->getParameterID() + "=" + juce::String((double)parameter->getValue(), 9));
    state.setProperty("khensuBoolRaw", boolRaw.joinIntoString(","), nullptr);

    // While an async restore is still in progress the live chain is
    // incomplete; save what was restored from instead (audit M5).
    std::vector<PersistedHostedPluginSlot> hostedSlots;
    bool restorePending = false;
    {
        const juce::ScopedLock sl(pendingHostedRestoreLock_);
        if (pendingHostedRestore_ != nullptr) {
            hostedSlots = *pendingHostedRestore_;
            restorePending = true;
        }
    }
    if (!restorePending) hostedSlots = hostedPlugins_.captureState();

    juce::ValueTree hostedChain(hostedChainType);
    hostedChain.setProperty("version", kHostedChainVersion, nullptr);
    for (const auto& slot : hostedSlots) {
        juce::ValueTree child("slot");
        child.setProperty("name", slot.name, nullptr);
        child.setProperty("filePath", slot.filePath, nullptr);
        child.setProperty("bypassed", slot.bypassed, nullptr);
        child.setProperty("wetMix", slot.wetMix, nullptr);
        child.setProperty("pluginState", juce::var(slot.pluginState), nullptr);
        hostedChain.addChild(child, -1, nullptr);
    }
    state.addChild(hostedChain, -1, nullptr);

    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

juce::ValueTree KhensuCopilotProcessor::validatedStateTree(const void* data, int sizeInBytes,
                                                           juce::String& reason) const
{
    // The header copyXmlToBinary() writes: a little-endian magic number, then
    // the UTF-8 string's length, then the string.
    constexpr juce::uint32 kXmlMagic = 0x21324356;
    if (data == nullptr || sizeInBytes < 9) {
        reason = "state is empty or shorter than its header";
        return {};
    }
    if (sizeInBytes > kMaxStateBytes) {
        reason = "state is larger than " + juce::String(kMaxStateBytes / (1024 * 1024)) + " MB";
        return {};
    }
    const auto* bytes = static_cast<const juce::uint8*>(data);
    if (juce::ByteOrder::littleEndianInt(bytes) != kXmlMagic) {
        reason = "not a Khensu Copilot state (bad header)";
        return {};
    }
    const auto declared = juce::ByteOrder::littleEndianInt(bytes + 4);
    if (declared == 0 || declared > static_cast<juce::uint32>(sizeInBytes - 8)) {
        reason = "state header declares " + juce::String((juce::int64)declared) + " bytes but holds "
                 + juce::String(sizeInBytes - 8);
        return {};
    }
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml == nullptr) {
        reason = "state XML does not parse";
        return {};
    }
    if (!xml->hasTagName(apvts_.state.getType())) {
        reason = "state root is <" + xml->getTagName() + ">, not <" + apvts_.state.getType().toString() + ">";
        return {};
    }
    auto tree = juce::ValueTree::fromXml(*xml);
    if (!tree.isValid()) {
        reason = "state XML is not a value tree";
        return {};
    }
    if (tree.hasProperty(kStateVersionId)) {
        const juce::String v = tree.getProperty(kStateVersionId).toString().trim();
        if (!v.containsOnly("0123456789") || v.isEmpty() || v.getIntValue() < 1) {
            reason = "state version '" + v + "' is not a positive integer";
            return {};
        }
    }
    int knownParams = 0;
    for (int i = 0; i < tree.getNumChildren(); ++i) {
        const auto child = tree.getChild(i);
        if (!child.hasType("PARAM")) continue;
        const juce::String id = child.getProperty("id").toString();
        const juce::String text = child.getProperty("value").toString().trim();
        const double v = text.getDoubleValue();
        if (id.isEmpty() || text.isEmpty() || !text.containsOnly("0123456789+-.eE") || !std::isfinite(v)) {
            reason = "state has a malformed PARAM node ('" + id + "')";
            return {};
        }
        if (apvts_.getParameter(id) != nullptr) ++knownParams;
    }
    if (knownParams == 0) {
        reason = "state holds none of this plugin's parameters";
        return {};
    }
    return tree;
}

void KhensuCopilotProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    // Validate everything first: an invalid blob (truncated, foreign, random
    // bytes) leaves the current state exactly as it was.
    juce::String rejection;
    auto tree = validatedStateTree(data, sizeInBytes, rejection);
    if (!tree.isValid()) {
        {
            const juce::ScopedLock lock(stateRejectionLock_);
            lastStateRejection_ = rejection;
        }
        juce::Logger::writeToLog("Khensu Copilot: ignored invalid state (" + rejection + ")");
        return;
    }
    {
        const juce::ScopedLock lock(stateRejectionLock_);
        lastStateRejection_.clear();
    }
    restoringState_.store(true, std::memory_order_release);
    const struct RestoreScope {
        std::atomic<bool>& flag;
        ~RestoreScope() { flag.store(false, std::memory_order_release); }
    } restoreScope{restoringState_};

    // State versioning (PLG-23): a missing version is the legacy v1 layout.
    const int savedVersion = static_cast<int>(tree.getProperty(kStateVersionId, 1));
    if (savedVersion > kStateVersion)
        juce::Logger::writeToLog("Khensu Copilot: state saved by a newer version (" + juce::String(savedVersion)
                                 + "); loading the parts this version understands");
    if (savedVersion < kStateVersion) migrateState(tree, savedVersion);
    adoptInstanceId(tree.getProperty(kInstanceIdProp).toString());
    mixHubTap_.setRoleOverride(tree.getProperty(kMixRoleProp).toString());
    analysisController_.setBoundSessionId(tree.getProperty(kBoundSessionProp).toString().toStdString());
    for (const auto& id : {kStateVersionId, kInstanceIdProp, kMixRoleProp, kBoundSessionProp})
        tree.removeProperty(id, nullptr);

    std::vector<PersistedHostedPluginSlot> hostedSlots;
    const juce::Identifier hostedChainType{"KhensuHostedChain"};
    const auto hostedChain = tree.getChildWithName(hostedChainType);
    if (hostedChain.isValid()) {
        const int chainVersion = static_cast<int>(hostedChain.getProperty("version", 1));
        if (chainVersion > kHostedChainVersion)
            juce::Logger::writeToLog("Khensu Copilot: hosted chain saved by a newer version ("
                                     + juce::String(chainVersion) + ")");
        for (int index = 0; index < hostedChain.getNumChildren(); ++index) {
            const auto child = hostedChain.getChild(index);
            if (!child.hasType("slot")) continue;
            PersistedHostedPluginSlot slot;
            slot.name = child.getProperty("name").toString();
            slot.filePath = child.getProperty("filePath").toString();
            slot.bypassed = static_cast<bool>(child.getProperty("bypassed"));
            // A missing property used to restore as 0 (silent slot) and a NaN
            // passed jlimit (AUDIT-085): default to fully wet instead.
            const auto wetVar = child.getProperty("wetMix", 1.0);
            const float wet = static_cast<float>(static_cast<double>(wetVar));
            slot.wetMix = std::isfinite(wet) ? juce::jlimit(0.0f, 1.0f, wet) : 1.0f;
            if (const auto* state = child.getProperty("pluginState").getBinaryData()) slot.pluginState = *state;
            if (slot.filePath.isNotEmpty()) hostedSlots.push_back(std::move(slot));
        }
        tree.removeChild(hostedChain, nullptr);
    }

    const auto aiSetIds = tree.getProperty("khensuAiSetParams").toString();
    // Kept out of the APVTS tree: getStateInformation writes every custom
    // property in one fixed order, so a round-trip is byte-stable.
    tree.removeProperty("khensuAiSetParams", nullptr);

    const auto boolRaw = tree.getProperty("khensuBoolRaw").toString();
    tree.removeProperty("khensuBoolRaw", nullptr);
    apvts_.replaceState(tree);

    // Re-apply the exact raw bool values (see getStateInformation). Only when
    // the raw value agrees with the restored on/off state, so a hand-edited
    // or mismatched property can never flip a switch.
    for (const auto& entry : juce::StringArray::fromTokens(boolRaw, ",", "")) {
        const auto id = entry.upToFirstOccurrenceOf("=", false, false);
        const float raw = (float)entry.fromFirstOccurrenceOf("=", false, false).getDoubleValue();
        auto* parameter = apvts_.getParameter(id);
        auto* boolParam = dynamic_cast<juce::AudioParameterBool*>(parameter);
        if (boolParam == nullptr || !std::isfinite(raw) || raw < 0.0f || raw > 1.0f) continue;
        if ((raw >= 0.5f) == boolParam->get() && raw != parameter->getValue()) parameter->setValueNotifyingHost(raw);
    }

    // AI-set flags last: restoring the values above fires the parameter
    // listener, which clears flags on any value change (PLG-20).
    for (auto& flag : aiSetFlags_) flag.store(false, std::memory_order_relaxed);
    for (const auto& id : juce::StringArray::fromTokens(aiSetIds, ",", "")) {
        const int idx = dspParamIndex(id.toRawUTF8());
        if (idx >= 0) aiSetFlags_[(size_t)idx].store(true, std::memory_order_relaxed);
    }

    restoreHostedChainAsync(std::move(hostedSlots));
}

bool KhensuCopilotProcessor::hasAnalysis() const noexcept
{
    return analysisController_.hasAnalysis();
}

void KhensuCopilotProcessor::getAnalysisSnapshot(AnalysisResult& outAnalysis,
                                                 std::vector<Recommendation>& outRecs) const
{
    analysisController_.getSnapshot(outAnalysis, outRecs);
}

void KhensuCopilotProcessor::loadLastSessionAsync(std::function<void()> onLoaded)
{
    analysisController_.loadLastSessionAsync(std::move(onLoaded));
}

void KhensuCopilotProcessor::setIssueState(const std::string& recId, issue_state::State state)
{
    analysisController_.setIssueState(recId, state);
}

issue_state::State KhensuCopilotProcessor::issueState(const std::string& recId) const
{
    return analysisController_.issueState(recId);
}

std::string KhensuCopilotProcessor::getSessionId() const
{
    return analysisController_.getSessionId();
}

bool KhensuCopilotProcessor::resolvePluginDescription(juce::AudioPluginFormatManager& formatManager,
                                                     const juce::String& filePath, juce::PluginDescription& out)
{
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : formatManager.getFormats())
        if (format->fileMightContainThisPluginType(filePath)) format->findAllTypesForFile(types, filePath);
    if (types.isEmpty()) return false;
    out = *types.getFirst();
    return true;
}

void KhensuCopilotProcessor::addPluginToChainAsync(const juce::String& filePath, std::function<void(bool)> onComplete)
{
    const auto weakProcessor = weakThis_;
    // Only the creation hint: the chain manager re-prepares the instance at
    // the rate/block size current when it ARRIVES (audit M7).
    const double sampleRate = currentSampleRate_;
    const int blockSize = getBlockSize() > 0 ? getBlockSize() : 512;
    juce::MessageManager::callAsync([weakProcessor, filePath, onComplete, sampleRate, blockSize]() {
        // The processor may have been unloaded while this task was queued.
        // In that case do not begin an asynchronous load whose completion
        // would otherwise have no valid owner.
        if (weakProcessor.get() == nullptr) {
            if (onComplete) onComplete(false);
            return;
        }
        auto formatManager = std::make_shared<juce::AudioPluginFormatManager>();
        juce::addDefaultFormatsToManager(*formatManager);

        // VST3 instantiation must match a class's name + uid inside the
        // module; a description with only fileOrIdentifier set never loads.
        juce::PluginDescription desc;
        if (!resolvePluginDescription(*formatManager, filePath, desc) || isCopilotItself(desc)) {
            // Hosting the Copilot inside itself is refused (PLG-24): it
            // would feed its own transport/Mix hub and can recurse.
            if (onComplete) onComplete(false);
            return;
        }

        formatManager->createPluginInstanceAsync(
            desc, sampleRate, blockSize,
            [weakProcessor, formatManager, filePath,
             onComplete](std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) {
                auto* processor = weakProcessor.get();
                if (processor == nullptr) {
                    // Let the callback-owned instance die on the message
                    // thread. The unloaded processor is never dereferenced.
                    if (onComplete) onComplete(false);
                    return;
                }
                if (instance != nullptr) {
                    // Not prepared here: addPluginInstance configures the bus
                    // layout first, then prepares (audits M3/M7).
                    const juce::String name = instance->getName();
                    const bool added = processor->addPluginInstanceToChain(std::move(instance), name, filePath);
                    if (onComplete) onComplete(added);
                } else {
                    juce::Logger::writeToLog("Failed to load plugin: " + error);
                    if (onComplete) onComplete(false);
                }
            });
    });
}

bool KhensuCopilotProcessor::isCopilotItself(const juce::PluginDescription& description)
{
    const juce::String selfName(JucePlugin_Name);
    const juce::String selfManufacturer(JucePlugin_Manufacturer);
    if (description.name == selfName
        && (description.manufacturerName.isEmpty() || description.manufacturerName == selfManufacturer))
        return true;
    const auto& id = description.fileOrIdentifier;
    return juce::File::isAbsolutePath(id) && juce::File(id).getFileNameWithoutExtension() == selfName;
}

bool KhensuCopilotProcessor::addPluginInstanceToChain(std::unique_ptr<juce::AudioPluginInstance> instance,
                                                      const juce::String& name, const juce::String& filePath)
{
    if (instance == nullptr) return false;
    auto description = instance->getPluginDescription();
    if (description.fileOrIdentifier.isEmpty()) description.fileOrIdentifier = filePath;
    if (isCopilotItself(description)) {
        juce::Logger::writeToLog("Khensu Copilot: refusing to host Khensu Copilot inside itself");
        return false; // the instance is released here, on the message thread
    }
    hostedPlugins_.addPluginInstance(std::move(instance), name, filePath);
    updateLatency();
    markNonParameterStateChanged();
    return true;
}

void KhensuCopilotProcessor::restoreHostedChainAsync(std::vector<PersistedHostedPluginSlot> slots)
{
    const auto generation = hostedRestoreGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto sharedSlots = std::make_shared<const std::vector<PersistedHostedPluginSlot>>(std::move(slots));
    {
        // Until the last slot lands, getStateInformation() saves this list.
        const juce::ScopedLock sl(pendingHostedRestoreLock_);
        pendingHostedRestore_ = sharedSlots;
    }
    const auto weakProcessor = weakThis_;
    juce::MessageManager::callAsync([weakProcessor, sharedSlots, generation]() {
        auto* processor = weakProcessor.get();
        if (processor == nullptr || processor->hostedRestoreGeneration_.load(std::memory_order_acquire) != generation)
            return;
        processor->closeAllHostedEditors(); // windows go before their slots retire
        processor->hostedPlugins_.clear();
        processor->updateLatency();
        processor->restoreNextHostedSlotAsync(sharedSlots, 0, generation);
    });
}

void KhensuCopilotProcessor::finishHostedRestore(uint64_t generation)
{
    if (hostedRestoreGeneration_.load(std::memory_order_acquire) != generation) return;
    const juce::ScopedLock sl(pendingHostedRestoreLock_);
    pendingHostedRestore_.reset();
}

bool KhensuCopilotProcessor::isHostedRestorePending() const
{
    const juce::ScopedLock sl(pendingHostedRestoreLock_);
    return pendingHostedRestore_ != nullptr;
}

void KhensuCopilotProcessor::restoreNextHostedSlotAsync(
    std::shared_ptr<const std::vector<PersistedHostedPluginSlot>> slots, size_t index, uint64_t generation)
{
    if (hostedRestoreGeneration_.load(std::memory_order_acquire) != generation) return;
    if (index >= slots->size()) {
        finishHostedRestore(generation);
        return;
    }
    const auto& saved = (*slots)[index];
    const auto weakProcessor = weakThis_;
    const double sampleRate = currentSampleRate_ > 0.0 ? currentSampleRate_ : 44100.0;
    const int blockSize = getBlockSize() > 0 ? getBlockSize() : 512;
    auto formatManager = std::make_shared<juce::AudioPluginFormatManager>();
    juce::addDefaultFormatsToManager(*formatManager);

    juce::PluginDescription description;
    if (!resolvePluginDescription(*formatManager, saved.filePath, description) || isCopilotItself(description)) {
        // Missing / unloadable plugin: keep a placeholder carrying its path,
        // saved state and controls, so a save on this machine round-trips it
        // instead of dropping it from the project (audit M5).
        juce::Logger::writeToLog("Hosted plugin not found, keeping placeholder: " + saved.filePath);
        hostedPlugins_.addPlaceholder(saved);
        restoreNextHostedSlotAsync(slots, index + 1, generation);
        return;
    }
    formatManager->createPluginInstanceAsync(
        description, sampleRate, blockSize,
        [weakProcessor, slots, index, generation,
         formatManager](std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) {
            auto* processor = weakProcessor.get();
            if (processor == nullptr || processor->hostedRestoreGeneration_.load(std::memory_order_acquire) != generation)
                return;
            const auto& saved = (*slots)[index];
            if (instance != nullptr) {
                // addPluginInstance configures buses, prepares at the current
                // rate, then restores the saved state (audits M3/M7).
                const auto instanceName = instance->getName();
                processor->hostedPlugins_.addPluginInstance(std::move(instance),
                                                            saved.name.isNotEmpty() ? saved.name : instanceName,
                                                            saved.filePath, saved.bypassed, saved.wetMix,
                                                            &saved.pluginState);
                processor->updateLatency();
            } else {
                juce::Logger::writeToLog("Failed to restore hosted plugin '" + saved.filePath + "': " + error);
                processor->hostedPlugins_.addPlaceholder(saved);
            }
            processor->restoreNextHostedSlotAsync(slots, index + 1, generation);
        });
}

void KhensuCopilotProcessor::removePluginFromChain(int slotIndex)
{
    // The slot's editor window is deleted before the slot is retired (PLG-06).
    const auto slots = hostedPlugins_.getSlotInfo();
    if (slotIndex >= 0 && slotIndex < static_cast<int>(slots.size()))
        closeHostedPluginEditor(slots[static_cast<size_t>(slotIndex)].id);
    hostedPlugins_.removePlugin(slotIndex);
    updateLatency();
    markNonParameterStateChanged();
}

void KhensuCopilotProcessor::clearHostedPluginChain()
{
    closeAllHostedEditors();
    hostedPlugins_.clear();
    updateLatency();
    markNonParameterStateChanged();
}

bool KhensuCopilotProcessor::moveHostedPlugin(int fromIndex, int toIndex)
{
    if (!hostedPlugins_.moveSlot(fromIndex, toIndex)) return false;
    updateLatency();
    markNonParameterStateChanged();
    return true;
}

bool KhensuCopilotProcessor::openHostedPluginEditor(int slotIndex)
{
    const auto slots = hostedPlugins_.getSlotInfo();
    if (slotIndex < 0 || slotIndex >= static_cast<int>(slots.size())) return false;
    const auto& info = slots[static_cast<size_t>(slotIndex)];
    if (auto it = hostedEditorWindows_.find(info.id); it != hostedEditorWindows_.end()) {
        it->second->toFront(true);
        return true;
    }
    auto instance = hostedPlugins_.getInstanceForSlotId(info.id);
    if (instance == nullptr) return false; // placeholder: nothing to edit
    const auto slotId = info.id;
    const auto weakProcessor = weakThis_;
    hostedEditorWindows_[slotId] = std::make_unique<HostedPluginEditorWindow>(
        std::move(instance), info.name + " - Khensu Copilot", [weakProcessor, slotId] {
            // Deferred: the window is still on the call stack of its own
            // close button.
            juce::MessageManager::callAsync([weakProcessor, slotId] {
                if (auto* processor = weakProcessor.get()) processor->closeHostedPluginEditor(slotId);
            });
        });
    return true;
}

void KhensuCopilotProcessor::closeHostedPluginEditor(uint64_t slotId)
{
    auto it = hostedEditorWindows_.find(slotId);
    if (it == hostedEditorWindows_.end()) return;
    auto window = std::move(it->second);
    hostedEditorWindows_.erase(it);
    window.reset(); // the editor is deleted here, before the instance can be
}

void KhensuCopilotProcessor::closeAllHostedEditors()
{
    auto windows = std::move(hostedEditorWindows_);
    hostedEditorWindows_.clear();
    windows.clear();
}

void KhensuCopilotProcessor::setHostedPluginBypass(int slotIndex, bool bypassed)
{
    hostedPlugins_.setBypass(slotIndex, bypassed);
    updateLatency();
    markNonParameterStateChanged();
}

void KhensuCopilotProcessor::setHostedPluginWetMix(int slotIndex, float wetMix)
{
    hostedPlugins_.setWetMix(slotIndex, wetMix);
    updateLatency();
    markNonParameterStateChanged();
}

int KhensuCopilotProcessor::getNumHostedPlugins() const noexcept
{
    return hostedPlugins_.getNumPlugins();
}

std::vector<KhensuCopilotProcessor::HostedPluginSlotInfo> KhensuCopilotProcessor::getHostedPluginSlots() const
{
    return hostedPlugins_.getSlotInfo();
}

void KhensuCopilotProcessor::updateLatency()
{
    // Mirrors processBlock's routing exactly (audit CQ-02): only slots the
    // audio path actually runs count (not a placeholder, fits the channel
    // table). Neither the wet amount nor a slot's bypass matters - the dry
    // side is always delayed to match (AUDIT-061, FIX-14) - so dragging wet
    // or bypassing a slot never moves it.
    // FIX-14: grow each slot's dry line to its plugin's current latency
    // first (a plugin can raise it after insert), then the A/B Original line
    // to the whole chain, so what is reported is what is compensated.
    hostedPlugins_.reserveDryLines();
    const int hosted = hostedPlugins_.getActiveLatency();
    outputStage_.reserveHostedLatency(hosted);
    // The audio thread needs this for latency-aligned bypass and must not
    // take the chain manager's lock, so publish it atomically.
    reportedHostedLatency_.store(hosted, std::memory_order_relaxed);
    hostedTailSeconds_.store(hostedPlugins_.getTailLengthSeconds(), std::memory_order_relaxed);
    setLatencySamples(outputStage_.latencySamples() + hosted);
}

void KhensuCopilotProcessor::handleAsyncUpdate()
{
    // A hosted instance changed its latency after it was inserted.
    updateLatency();
    // A non-parameter change made off the message thread (PLG-05).
    if (pendingNonParameterChange_.exchange(false, std::memory_order_acq_rel))
        updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));
}

void KhensuCopilotProcessor::timerCallback()
{
    hostedPlugins_.reap();
    outputStage_.reapRetired();
    // Hosted plugins can change their tail (e.g. a reverb's decay) without
    // any notification; keep the published value current.
    hostedTailSeconds_.store(hostedPlugins_.getTailLengthSeconds(), std::memory_order_relaxed);
}

// ---- CE-DAW-1/3/4 ---------------------------------------------------------------

ara::IAraSession* KhensuCopilotProcessor::getAraSession() noexcept
{
#if JucePlugin_Enable_ARA
    ARA::PlugIn::DocumentController* documentController = nullptr;
    if (auto* renderer = getPlaybackRenderer())
        documentController = renderer->getDocumentController();
    else if (auto* editorRenderer = getEditorRenderer())
        documentController = editorRenderer->getDocumentController();
    else if (auto* editorView = getEditorView())
        documentController = editorView->getDocumentController();
    if (documentController == nullptr) return nullptr;
    return juce::ARADocumentControllerSpecialisation::getSpecialisedDocumentController<
        ara::KhensuAraDocumentController>(documentController);
#else
    return nullptr;
#endif
}

KhensuCopilotProcessor::SeekPath KhensuCopilotProcessor::seekHostTo(double timeSec)
{
    timeSec = juce::jmax(0.0, timeSec);
    if (auto* session = getAraSession(); session != nullptr && session->requestPlaybackPosition(timeSec))
        return SeekPath::Ara;
    // Fallback: the service records the seek (every instance polls it) and
    // sends an MMC Locate to the Khensu MIDI port. Short pool, bounded.
    runShortInBackground([timeSec] {
        auto bridge = copilot::ServiceClient::bridge();
        bridge.seekTransport(static_cast<float>(timeSec));
    });
    return SeekPath::ServiceMmc;
}

float KhensuCopilotProcessor::getParamPlain(const std::string& paramId) const
{
    if (auto* raw = apvts_.getRawParameterValue(juce::String(paramId))) return raw->load(std::memory_order_relaxed);
    return 0.0f;
}

bool KhensuCopilotProcessor::armFixAutomation(const timeline::TimedIssue& issue, juce::String& message)
{
    auto curve = automation::curveFromFix(issue.suggestedParameters, issue.startSec, issue.endSec, 0.25,
                                          [this](const std::string& id) { return getParamPlain(id); });
    // Only lanes this plugin actually exposes.
    curve.lanes.erase(std::remove_if(curve.lanes.begin(), curve.lanes.end(),
                                     [this](const automation::Lane& lane) {
                                         return apvts_.getParameter(juce::String(lane.paramId)) == nullptr;
                                     }),
                      curve.lanes.end());
    if (curve.empty()) {
        message = TRANS("This finding has no parameter the Copilot can automate.");
        return false;
    }
    curve.label = issue.title;
    fixWriter_.arm(std::move(curve), gestureSink_);
    fixAutomationTimer_.startTimerHz(30);
    message = TRANS("Armed: put this track's automation in Write, Touch or Latch, then play through") + " "
              + juce::String(issue.startSec, 1) + "-" + juce::String(issue.endSec, 1) + " s.";
    return true;
}

void KhensuCopilotProcessor::cancelFixAutomation()
{
    fixWriter_.cancel(gestureSink_);
    fixAutomationTimer_.stopTimer();
}

void KhensuCopilotProcessor::tickFixAutomation()
{
    const auto telemetry = realtimeTelemetry_.snapshot();
    fixWriter_.tick(static_cast<double>(telemetry.position_sec), telemetry.is_playing, gestureSink_);
    if (fixWriter_.state() == automation::FixAutomationWriter::State::Finished
        || fixWriter_.state() == automation::FixAutomationWriter::State::Idle)
        fixAutomationTimer_.stopTimer();
}

bool KhensuCopilotProcessor::GestureSink::beginGesture(const std::string& paramId)
{
    if (auto* parameter = owner.apvts_.getParameter(juce::String(paramId))) {
        parameter->beginChangeGesture();
        return true;
    }
    return false;
}

void KhensuCopilotProcessor::GestureSink::setValue(const std::string& paramId, float plainValue)
{
    if (auto* parameter = owner.apvts_.getParameter(juce::String(paramId)))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
}

void KhensuCopilotProcessor::GestureSink::endGesture(const std::string& paramId)
{
    if (auto* parameter = owner.apvts_.getParameter(juce::String(paramId))) parameter->endChangeGesture();
}

} // namespace khensu

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new khensu::KhensuCopilotProcessor(); }
