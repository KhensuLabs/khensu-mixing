#include "PluginProcessor.h"
#include "CopilotServiceClient.h"
// The short pool's shutdown wait is sized for one ServiceClient request.
static_assert(khensu::copilot::ServiceClient::kShortRequestMs == khensu::KhensuCopilotProcessor::kShortJobTimeoutMs);
#include "PluginEditor.h"
#include "PluginCrashHandler.h"
#include <khensu/common/Version.h>
#include <khensu/common/AppPaths.h>
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
// State v3 (UI-8 / UI-9): editor view, program / preset, A/B snapshot.
const juce::Identifier kViewCompactProp{"khensuViewCompact"};
const juce::Identifier kViewSizeProp{"khensuViewSize"};
const juce::Identifier kViewTabProp{"khensuViewTab"};
const juce::Identifier kProgramProp{"khensuProgram"};
const juce::Identifier kPresetNameProp{"khensuPresetName"};
const juce::Identifier kSnapActiveProp{"khensuSnapActive"};
const juce::Identifier kSnapOtherProp{"khensuSnapOther"};

/// Same value from the host's point of view: a discrete (bool / choice)
/// parameter compares by the step its normalised value lands on.
bool sameDiscreteStep(juce::RangedAudioParameter& p, float a, float b)
{
    return p.convertFrom0to1(a) == p.convertFrom0to1(b);
}

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
    pEqType_ = apvts_.getRawParameterValue(ParamIDs::eqType);
    pEq2Type_ = apvts_.getRawParameterValue(ParamIDs::eq2Type);
    pEq3Type_ = apvts_.getRawParameterValue(ParamIDs::eq3Type);
    pDeessOn_ = apvts_.getRawParameterValue(ParamIDs::deessOn);
    pDeessFreq_ = apvts_.getRawParameterValue(ParamIDs::deessFreq);
    pDeessThreshDb_ = apvts_.getRawParameterValue(ParamIDs::deessThreshDb);
    pDeessRangeDb_ = apvts_.getRawParameterValue(ParamIDs::deessRangeDb);
    pFixAmount_ = apvts_.getRawParameterValue(ParamIDs::fixAmount);

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
    // OVH-8: moving the Amount re-applies the live fix (on the message thread).
    if (auto* amount = apvts_.getParameter(ParamIDs::fixAmount)) {
        fixAmountParamIndex_ = amount->getParameterIndex();
        amount->addListener(this);
    }

    // UI-9: what a preset / snapshot covers, and every factory preset as
    // normalised values resolved now - setCurrentProgram never allocates.
    for (const char* id : kDspParamIds)
        if (auto* parameter = apvts_.getParameter(id)) snapshotParams_.push_back(parameter);
    if (auto* mix = apvts_.getParameter(ParamIDs::chainMix)) snapshotParams_.push_back(mix);
    for (const auto& preset : copilot::presets::factoryPresets()) {
        std::vector<std::pair<juce::RangedAudioParameter*, float>> values;
        for (auto* parameter : snapshotParams_) {
            float normalised = parameter->getDefaultValue();
            for (const auto& [id, plain] : preset.values)
                if (parameter->getParameterID() == id) normalised = parameter->convertTo0to1(plain);
            values.emplace_back(parameter, normalised);
        }
        programValues_.push_back(std::move(values));
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
    dspParams.eqType = pEqType_;
    dspParams.eq2Type = pEq2Type_;
    dspParams.eq3Type = pEq3Type_;
    dspParams.deessOn = pDeessOn_;
    dspParams.deessFreq = pDeessFreq_;
    dspParams.deessThreshDb = pDeessThreshDb_;
    dspParams.deessRangeDb = pDeessRangeDb_;
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
    if (auto* amount = apvts_.getParameter(ParamIDs::fixAmount)) amount->removeListener(this);
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

    // ---- State v3 (OVH-8): appended, so a v2 project loads them at defaults.
    const juce::StringArray shapes{"Bell", "Low shelf", "High shelf"};
    layout.add(std::make_unique<juce::AudioParameterChoice>(ParamIDs::eqType, "EQ Shape", shapes, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>(ParamIDs::eq2Type, "EQ2 Shape", shapes, 0));
    layout.add(std::make_unique<juce::AudioParameterChoice>(ParamIDs::eq3Type, "EQ3 Shape", shapes, 0));
    layout.add(std::make_unique<BoolParam>(ParamIDs::deessOn, "De-esser On", false));
    layout.add(std::make_unique<FloatParam>(ParamIDs::deessFreq, "De-esser Freq", logRange(2000.0f, 12000.0f), 6500.0f,
                                            logAttributes));
    layout.add(std::make_unique<FloatParam>(ParamIDs::deessThreshDb, "De-esser Threshold",
                                            juce::NormalisableRange<float>(-60.0f, 0.0f, 0.1f), -30.0f));
    layout.add(std::make_unique<FloatParam>(ParamIDs::deessRangeDb, "De-esser Range",
                                            juce::NormalisableRange<float>(0.0f, 24.0f, 0.1f), 6.0f));
    // Percent text that round-trips exactly (clap-validator param-conversions).
    const auto percentAttributes =
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction([](float v, int) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; })
            .withValueFromStringFunction([](const juce::String& t) { return t.getFloatValue() / 100.0f; });
    layout.add(std::make_unique<FloatParam>(ParamIDs::fixAmount, "Fix Amount",
                                            juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f, percentAttributes));
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
    // UX2-6b: a programmatic write is never an undo step of its own; the
    // caller's transaction (or the user's gesture around it) records it.
    const copilot::ParamUndoHistory::ScopedSuppress quiet(undoHistory_);
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
    const copilot::ParamUndoHistory::ScopedTransaction step(undoHistory_, TRANS("Apply chain"));
    // Stage switches the new set does not mention are turned off, so an
    // earlier Apply's stages do not stay on underneath this one (PLG-19).
    static constexpr const char* kStageSwitches[] = {ParamIDs::hpfOn,         ParamIDs::lpfOn,  ParamIDs::eqOn,
                                                     ParamIDs::eq2On,         ParamIDs::eq3On,  ParamIDs::compOn,
                                                     ParamIDs::compMultiband, ParamIDs::limOn,  ParamIDs::deessOn,
                                                     ParamIDs::eqType,        ParamIDs::eq2Type, ParamIDs::eq3Type};
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
    const copilot::ParamUndoHistory::ScopedTransaction step(undoHistory_, TRANS("Suggestion: output gain"));
    setParamFromAI(ParamIDs::outGainDb, pOutGainDb_->load(std::memory_order_relaxed) + deltaDb);
}

void KhensuCopilotProcessor::parameterValueChanged(int parameterIndex, float)
{
    // Any thread (host automation arrives on the audio thread): atomics and
    // a preallocated async message only.
    if (parameterIndex == fixAmountParamIndex_ && fixAmountParamIndex_ >= 0) {
        if (liveFixActive_.load(std::memory_order_acquire) && !restoringState_.load(std::memory_order_acquire)) {
            pendingFixReapply_.store(true, std::memory_order_release);
            triggerAsyncUpdate();
        }
        return;
    }
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
    // v2 -> v3: new parameters (EQ shapes, de-esser, fix amount) were
    // APPENDED to the layout; a v2 state has no PARAM node for them, and
    // replaceState gives each its default - bell bands and the de-esser off,
    // which is exactly how v2 sounded. The editor view, program, preset name
    // and snapshot properties are optional and default when absent.
    if (fromVersion < 3) {
        // (no-op)
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
    state.setProperty(kViewCompactProp, viewCompact_.load(std::memory_order_relaxed) ? 1 : 0, nullptr);
    state.setProperty(kViewSizeProp, viewSize_.load(std::memory_order_relaxed), nullptr);
    state.setProperty(kViewTabProp, viewTab_.load(std::memory_order_relaxed), nullptr);
    state.setProperty(kProgramProp, currentProgram_.load(std::memory_order_relaxed), nullptr);
    {
        const juce::ScopedLock sl(presetLock_);
        state.setProperty(kPresetNameProp, presetName_, nullptr);
        state.setProperty(kSnapActiveProp, activeSnapshot_.load(std::memory_order_relaxed), nullptr);
        state.setProperty(kSnapOtherProp, snapshotToString(otherSnapshot_), nullptr);
    }

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
    // The same holds for choice parameters (EQ shapes, OVH-8).
    juce::StringArray boolRaw;
    for (auto* parameter : getParameters())
        if (dynamic_cast<juce::AudioParameterBool*>(parameter) != nullptr
            || dynamic_cast<juce::AudioParameterChoice*>(parameter) != nullptr)
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*>(parameter))
                boolRaw.add(ranged->getParameterID() + "=" + juce::String((double)parameter->getValue(), 9));
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
    undoHistory_.requestClear(); // a restored project starts a new history
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
    // v3 (UI-8 / UI-9): out-of-range values fall back to the defaults.
    viewCompact_.store(static_cast<int>(tree.getProperty(kViewCompactProp, 0)) != 0, std::memory_order_relaxed);
    viewSize_.store(juce::jlimit(0, 2, static_cast<int>(tree.getProperty(kViewSizeProp, 1))), std::memory_order_relaxed);
    viewTab_.store(juce::jmax(0, static_cast<int>(tree.getProperty(kViewTabProp, 0))), std::memory_order_relaxed);
    currentProgram_.store(juce::jlimit(0, getNumPrograms() - 1, static_cast<int>(tree.getProperty(kProgramProp, 0))),
                          std::memory_order_relaxed);
    {
        const juce::ScopedLock sl(presetLock_);
        presetName_ = tree.getProperty(kPresetNameProp).toString();
        activeSnapshot_.store(juce::jlimit(0, 1, static_cast<int>(tree.getProperty(kSnapActiveProp, 0))),
                              std::memory_order_relaxed);
        otherSnapshot_ = snapshotFromString(tree.getProperty(kSnapOtherProp).toString());
    }
    // A loaded state is not the live fix's base any more.
    liveFixActive_.store(false, std::memory_order_release);
    for (const auto& id : {kStateVersionId, kInstanceIdProp, kMixRoleProp, kBoundSessionProp, kViewCompactProp,
                           kViewSizeProp, kViewTabProp, kProgramProp, kPresetNameProp, kSnapActiveProp, kSnapOtherProp})
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
        const bool discrete = dynamic_cast<juce::AudioParameterBool*>(parameter) != nullptr
                              || dynamic_cast<juce::AudioParameterChoice*>(parameter) != nullptr;
        if (!discrete || !std::isfinite(raw) || raw < 0.0f || raw > 1.0f) continue;
        if (sameDiscreteStep(*parameter, raw, parameter->getValue()) && raw != parameter->getValue())
            parameter->setValueNotifyingHost(raw);
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
    // OVH-8: fix_amount moved (UI, host automation) - re-apply the live fix.
    if (pendingFixReapply_.exchange(false, std::memory_order_acq_rel)) reapplyLiveFix();
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
    const copilot::ParamUndoHistory::ScopedSuppress quiet(owner.undoHistory_);
    if (auto* parameter = owner.apvts_.getParameter(juce::String(paramId))) {
        parameter->beginChangeGesture();
        return true;
    }
    return false;
}

void KhensuCopilotProcessor::GestureSink::setValue(const std::string& paramId, float plainValue)
{
    const copilot::ParamUndoHistory::ScopedSuppress quiet(owner.undoHistory_);
    if (auto* parameter = owner.apvts_.getParameter(juce::String(paramId)))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(plainValue));
}

void KhensuCopilotProcessor::GestureSink::endGesture(const std::string& paramId)
{
    const copilot::ParamUndoHistory::ScopedSuppress quiet(owner.undoHistory_);
    if (auto* parameter = owner.apvts_.getParameter(juce::String(paramId))) parameter->endChangeGesture();
}

// ---- OVH-8: the live top fix -------------------------------------------------------

float KhensuCopilotProcessor::readPlain(const char* paramId) const noexcept
{
    if (auto* raw = apvts_.getRawParameterValue(paramId)) return raw->load(std::memory_order_relaxed);
    return 0.0f;
}

juce::String KhensuCopilotProcessor::autoApplyBlockReason(const Recommendation& rec)
{
    if (rec.safe_to_auto_apply) return {};
    juce::String why = TRANS("Not applied automatically: this finding needs your judgement");
    if (!rec.auto_apply_rationale.empty())
        why << " (" << juce::String(juce::CharPointer_UTF8(rec.auto_apply_rationale.c_str())) << ")";
    else if (rec.is_taste_decision)
        why << " (" << TRANS("a taste decision") << ")";
    return why + ".";
}

KhensuCopilotProcessor::LiveFixResult KhensuCopilotProcessor::applyLiveFix(const Recommendation& rec, bool requireSafe)
{
    LiveFixResult result;
    // Re-applying the same finding keeps its original base, so pressing
    // Apply twice does not stack the fix on itself.
    const bool sameFix = liveFixActive_.load(std::memory_order_acquire) && liveFix_.id == rec.id && !rec.id.empty();
    std::map<std::string, float> base = sameFix ? liveFixBase_ : std::map<std::string, float>{};
    if (!sameFix)
        for (const char* id : kDspParamIds) base[id] = readPlain(id);
    const auto baseOf = [&base](const std::string& id) {
        const auto it = base.find(id);
        return it != base.end() ? it->second : 0.0f;
    };
    result.mapped = mixerchain::mapRecommendationToDsp(rec, getFixAmount(), baseOf);
    if (requireSafe) {
        result.blockedReason = autoApplyBlockReason(rec);
        if (result.blockedReason.isNotEmpty()) return result;
    }
    if (result.mapped.empty()) {
        result.blockedReason = TRANS("Nothing in this fix maps to the built-in chain.");
        return result;
    }
    const auto fixId = rec.id;
    const copilot::ParamUndoHistory::ScopedTransaction step(
        undoHistory_, TRANS("Apply top fix") + " " + juce::String(juce::roundToInt(getFixAmount() * 100.0f)) + " %",
        [this, fixId] {
            if (getLiveFixId() == fixId) clearLiveFix();
        });
    liveFix_ = rec;
    liveFixBase_ = std::move(base);
    liveFixActive_.store(true, std::memory_order_release);
    reapplyLiveFix();
    result.applied = true;
    return result;
}

void KhensuCopilotProcessor::reapplyLiveFix()
{
    // Follows fix_amount (a gesture or host automation): not a step itself.
    const copilot::ParamUndoHistory::ScopedSuppress quiet(undoHistory_);
    if (!liveFixActive_.load(std::memory_order_acquire)) return;
    const auto baseOf = [this](const std::string& id) {
        const auto it = liveFixBase_.find(id);
        return it != liveFixBase_.end() ? it->second : 0.0f;
    };
    const float amount = getFixAmount();
    if (amount < 0.005f) {
        // Amount 0: the parameters the fix touches go back to before it.
        for (const auto& [id, value] : mixerchain::mapRecommendationToDsp(liveFix_, 1.0f, baseOf).values) {
            juce::ignoreUnused(value);
            setParamFromUi(id.c_str(), baseOf(id));
            clearAiSet(id.c_str());
        }
        return;
    }
    for (const auto& [id, value] : mixerchain::mapRecommendationToDsp(liveFix_, amount, baseOf).values)
        if (std::abs(readPlain(id.c_str()) - value) > 1.0e-5f || !isAiSet(id.c_str())) setParamFromAI(id.c_str(), value);
}

void KhensuCopilotProcessor::clearLiveFix()
{
    liveFixActive_.store(false, std::memory_order_release);
    pendingFixReapply_.store(false, std::memory_order_release);
}

std::string KhensuCopilotProcessor::getLiveFixId() const
{
    return liveFixActive_.load(std::memory_order_acquire) ? liveFix_.id : std::string();
}

// ---- UI-9: programs, user presets, A/B snapshots -----------------------------------

void KhensuCopilotProcessor::setCurrentProgram(int index)
{
    if (index < 0 || index >= static_cast<int>(programValues_.size())) return;
    currentProgram_.store(index, std::memory_order_relaxed);
    for (const auto& [parameter, normalised] : programValues_[static_cast<size_t>(index)])
        if (parameter->getValue() != normalised) parameter->setValueNotifyingHost(normalised);
    // The name is shown by the editor; a juce::String assignment may
    // allocate, so only the message thread writes it.
    if (juce::MessageManager::existsAndIsCurrentThread()) {
        const juce::ScopedLock sl(presetLock_);
        presetName_ = getProgramName(index);
    }
}

const juce::String KhensuCopilotProcessor::getProgramName(int index)
{
    const auto& presets = copilot::presets::factoryPresets();
    if (index < 0 || index >= static_cast<int>(presets.size())) return {};
    return presets[static_cast<size_t>(index)].name;
}

juce::String KhensuCopilotProcessor::getCurrentPresetName() const
{
    const juce::ScopedLock sl(presetLock_);
    return presetName_;
}

juce::File KhensuCopilotProcessor::getUserPresetDirectory() const
{
    if (userPresetDirOverride_ != juce::File()) return userPresetDirOverride_;
    return khensu::presets::presetsDirectory();
}

juce::File KhensuCopilotProcessor::getLegacyUserPresetDirectory() const
{
    if (legacyPresetDirOverride_ != juce::File()) return legacyPresetDirOverride_;
    return juce::File(juce::String(juce::CharPointer_UTF8(AppPaths::dataDirectory().c_str())))
        .getChildFile("Copilot Presets");
}

std::vector<khensu::presets::ListedPreset> KhensuCopilotProcessor::listUserPresets() const
{
    const auto dir = getUserPresetDirectory();
    copilot::presets::migrateLegacyUserPresets(getLegacyUserPresetDirectory(), dir);
    return khensu::presets::listPresets(dir);
}

void KhensuCopilotProcessor::loadProgramFromUi(int index)
{
    if (index < 0 || index >= getNumPrograms()) return;
    const copilot::ParamUndoHistory::ScopedTransaction step(undoHistory_,
                                                           TRANS("Preset:") + " " + getProgramName(index));
    setCurrentProgram(index);
}

bool KhensuCopilotProcessor::saveUserPreset(const juce::String& name, juce::String& error)
{
    if (name.trim().isEmpty()) {
        error = TRANS("A preset needs a name.");
        return false;
    }
    const auto dir = getUserPresetDirectory();
    khensu::presets::ChainPreset preset;
    // Saving over a preset of the same name keeps what this plugin does not
    // own: its author, when it was made and the desktop rack's stages.
    if (auto existing = khensu::presets::loadPreset(khensu::presets::fileFor(dir, name))) {
        preset.author = existing->author;
        preset.created = existing->created;
        preset.stages = existing->stages;
    }
    preset.name = name.trim();
    preset.source = "Khensu Copilot";
    for (auto* parameter : snapshotParams_)
        preset.params[parameter->getParameterID()] = parameter->convertFrom0to1(parameter->getValue());
    juce::File written;
    if (!khensu::presets::save(preset, dir, written, error)) return false;
    {
        const juce::ScopedLock sl(presetLock_);
        presetName_ = preset.name;
    }
    markNonParameterStateChanged();
    return true;
}

bool KhensuCopilotProcessor::loadUserPreset(const juce::File& file, juce::String& error)
{
    auto preset = khensu::presets::loadPreset(file, &error);
    if (!preset.has_value()) return false;
    bool namesAny = false;
    for (auto* parameter : snapshotParams_)
        if (preset->params.count(parameter->getParameterID()) != 0) namesAny = true;
    if (!namesAny) {
        // A desktop-only chain: nothing here for the built-in DSP.
        error = TRANS("The preset holds no parameters for the built-in chain.");
        return false;
    }
    const copilot::ParamUndoHistory::ScopedTransaction step(undoHistory_, TRANS("Preset:") + " " + preset->name);
    for (auto* parameter : snapshotParams_) {
        const auto it = preset->params.find(parameter->getParameterID());
        const float normalised =
            it != preset->params.end() ? parameter->convertTo0to1(it->second) : parameter->getDefaultValue();
        if (parameter->getValue() == normalised) continue;
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(normalised);
        parameter->endChangeGesture();
    }
    {
        const juce::ScopedLock sl(presetLock_);
        presetName_ = preset->name;
    }
    markNonParameterStateChanged();
    return true;
}

std::vector<float> KhensuCopilotProcessor::captureSnapshot() const
{
    std::vector<float> values;
    values.reserve(snapshotParams_.size());
    for (auto* parameter : snapshotParams_) values.push_back(parameter->convertFrom0to1(parameter->getValue()));
    return values;
}

void KhensuCopilotProcessor::loadSnapshot(const std::vector<float>& plainValues)
{
    if (plainValues.size() != snapshotParams_.size()) return;
    for (size_t i = 0; i < snapshotParams_.size(); ++i) {
        auto* parameter = snapshotParams_[i];
        const float normalised = parameter->convertTo0to1(plainValues[i]);
        if (parameter->getValue() == normalised) continue;
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(normalised);
        parameter->endChangeGesture();
    }
}

void KhensuCopilotProcessor::switchSnapshot(int slot)
{
    slot = juce::jlimit(0, 1, slot);
    const int from = activeSnapshot_.load(std::memory_order_relaxed);
    if (slot == from) return;
    // UX2-6b: one step; undo switches back, which restores both sides.
    undoHistory_.performCustom(TRANS("Snapshot") + " " + (slot == 0 ? "A" : "B"),
                               [this, slot] { switchSnapshotNow(slot); },
                               [this, from] { switchSnapshotNow(from); });
}

void KhensuCopilotProcessor::switchSnapshotNow(int slot)
{
    slot = juce::jlimit(0, 1, slot);
    if (slot == activeSnapshot_.load(std::memory_order_relaxed)) return;
    auto live = captureSnapshot();
    std::vector<float> other;
    {
        const juce::ScopedLock sl(presetLock_);
        // The other side starts as a copy of this one the first time.
        other = otherSnapshot_.size() == live.size() ? otherSnapshot_ : live;
        otherSnapshot_ = live;
    }
    activeSnapshot_.store(slot, std::memory_order_relaxed);
    clearLiveFix(); // the live fix's base belongs to the side it was applied on
    loadSnapshot(other);
    markNonParameterStateChanged();
}

void KhensuCopilotProcessor::copyActiveSnapshotToOther()
{
    auto live = captureSnapshot();
    {
        const juce::ScopedLock sl(presetLock_);
        otherSnapshot_ = std::move(live);
    }
    markNonParameterStateChanged();
}

juce::String KhensuCopilotProcessor::snapshotToString(const std::vector<float>& plainValues) const
{
    // id=value pairs in parameter order; 9 decimals round-trip a float
    // exactly, so a save -> load -> save is byte-stable.
    if (plainValues.size() != snapshotParams_.size()) return {};
    juce::StringArray parts;
    for (size_t i = 0; i < plainValues.size(); ++i)
        parts.add(snapshotParams_[i]->getParameterID() + "=" + juce::String(static_cast<double>(plainValues[i]), 9));
    return parts.joinIntoString(",");
}

std::vector<float> KhensuCopilotProcessor::snapshotFromString(const juce::String& text) const
{
    if (text.isEmpty()) return {};
    std::vector<float> values;
    values.reserve(snapshotParams_.size());
    for (auto* parameter : snapshotParams_) values.push_back(parameter->convertFrom0to1(parameter->getDefaultValue()));
    for (const auto& entry : juce::StringArray::fromTokens(text, ",", "")) {
        const auto id = entry.upToFirstOccurrenceOf("=", false, false);
        const auto valueText = entry.fromFirstOccurrenceOf("=", false, false);
        const float value = valueText.getFloatValue();
        if (!std::isfinite(value)) continue;
        for (size_t i = 0; i < snapshotParams_.size(); ++i)
            if (snapshotParams_[i]->getParameterID() == id) {
                const auto range = snapshotParams_[i]->getNormalisableRange();
                values[i] = juce::jlimit(range.start, range.end, value);
            }
    }
    return values;
}

// ---- UI-8: editor view --------------------------------------------------------------

KhensuCopilotProcessor::EditorViewState KhensuCopilotProcessor::getEditorViewState() const noexcept
{
    EditorViewState state;
    state.compact = viewCompact_.load(std::memory_order_relaxed);
    state.size = static_cast<ViewSize>(juce::jlimit(0, 2, viewSize_.load(std::memory_order_relaxed)));
    state.tab = viewTab_.load(std::memory_order_relaxed);
    return state;
}

void KhensuCopilotProcessor::setEditorViewState(const EditorViewState& state)
{
    if (state == getEditorViewState()) return;
    viewCompact_.store(state.compact, std::memory_order_relaxed);
    viewSize_.store(static_cast<int>(state.size), std::memory_order_relaxed);
    viewTab_.store(juce::jmax(0, state.tab), std::memory_order_relaxed);
    // Not marked as a project edit: switching a tab must not make the host
    // ask to save. The view is saved with the next save.
}

} // namespace khensu

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new khensu::KhensuCopilotProcessor(); }
