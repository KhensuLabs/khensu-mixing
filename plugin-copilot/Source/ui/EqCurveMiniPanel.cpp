#include "EqCurveMiniPanel.h"
#include "../PluginProcessor.h"
#include <khensu/mixerchain/RecommendationDspMapper.h>
#include <khensu/common/AppPaths.h>
#include <khensu/feedback/FeedbackManager.h>
#include <cmath>

namespace khensu {

namespace {
// STU-10: all six built-in bands.
constexpr int kBands = mixerchain::kNumEqBands;
const mixerchain::EqBandParamIds& bandIds(int index) { return mixerchain::kEqBandParamIds[index]; }

void forEachBandParam(juce::AudioProcessorValueTreeState& apvts, int index,
                      const std::function<void(juce::RangedAudioParameter&)>& fn)
{
    for (const char* id : {bandIds(index).freq, bandIds(index).gain, bandIds(index).q})
        if (auto* p = apvts.getParameter(id)) fn(*p);
}
} // namespace

EqCurveMiniPanel::EqCurveMiniPanel(KhensuCopilotProcessor& proc) : processor_(proc)
{
    // Drags are bracketed as host gestures so automation records one move,
    // and the override is persisted ONCE per drag with the real baseline
    // (previously a thread + DB write per mouse event, baselines 0 / 1000).
    eqCurve_.onBandDragStarted([this](int index, const EqBand& band) {
        if (index < 0 || index >= kBands) return;
        dragBaseline_ = band;
        forEachBandParam(processor_.getAPVTS(), index, [](auto& p) { p.beginChangeGesture(); });
    });
    eqCurve_.onBandChanged([this](int index, const EqBand& band) {
        if (index < 0 || index >= kBands) return;
        auto& apvts = processor_.getAPVTS();
        const auto set = [&](const char* id, float plain) {
            if (auto* p = apvts.getParameter(id)) p->setValueNotifyingHost(p->convertTo0to1(plain));
        };
        set(bandIds(index).freq, band.hz);
        set(bandIds(index).gain, band.gain_db);
        set(bandIds(index).q, band.q);
        if (index < (int)lastSyncedBands_.size()) lastSyncedBands_[(size_t)index] = band;
    });
    eqCurve_.onBandDragEnded([this](int index, const EqBand& band) {
        if (index < 0 || index >= kBands) return;
        forEachBandParam(processor_.getAPVTS(), index, [](auto& p) { p.endChangeGesture(); });
        persistOverride(index, dragBaseline_, band);
    });
    addAndMakeVisible(eqCurve_);

    syncBandsFromApvts();
    visibilityChanged();
}

void EqCurveMiniPanel::visibilityChanged()
{
    if (isVisible()) {
        syncBandsFromApvts();
        startTimerHz(8);
    } else {
        stopTimer();
    }
}

EqCurveMiniPanel::~EqCurveMiniPanel() { stopTimer(); }

void EqCurveMiniPanel::persistOverride(int index, const EqBand& before, const EqBand& after)
{
    // Same telemetry path as the desktop MixMasterScreen (shared khensu.db).
    // On the processor's short pool (PLG-15): the panel's own ThreadPool was
    // joined on the message thread when the editor closed, so one write
    // waiting on the database's 5 s busy_timeout stalled the DAW.
    processor_.runShortInBackground([index, before, after]() {
        try {
            FeedbackManager fbMgr(AppPaths::databaseFile(), FeedbackManager::OpenMode::NoProvision); // PLG-10: no DDL/import
            const std::string prefix = "eq_band_" + std::to_string(index);
            if (std::abs(after.gain_db - before.gain_db) > 0.01f)
                fbMgr.recordOverride("eq_adjustment", prefix + "_gain_db", before.gain_db, after.gain_db);
            if (std::abs(after.hz - before.hz) > 0.5f)
                fbMgr.recordOverride("eq_adjustment", prefix + "_hz", before.hz, after.hz);
            if (std::abs(after.q - before.q) > 0.001f)
                fbMgr.recordOverride("eq_adjustment", prefix + "_q", before.q, after.q);
        } catch (const std::exception& e) {
            juce::Logger::writeToLog(juce::String("Plugin EQ-override persist failed: ") + e.what());
        } catch (...) {
            juce::Logger::writeToLog("Plugin EQ-override persist failed: unknown error");
        }
    });
}

void EqCurveMiniPanel::resized() { eqCurve_.setBounds(getLocalBounds()); }

std::vector<EqBand> EqCurveMiniPanel::readBandsFromApvts() const
{
    auto& apvts = processor_.getAPVTS();
    auto raw = [&](const char* id) {
        auto* p = apvts.getRawParameterValue(id);
        return p != nullptr ? p->load() : 0.0f;
    };

    std::vector<EqBand> bands(static_cast<size_t>(kBands));
    for (int i = 0; i < kBands; ++i) {
        const auto& ids = bandIds(i);
        bands[static_cast<size_t>(i)] = {raw(ids.freq), raw(ids.gain), raw(ids.q), false, raw(ids.on) > 0.5f};
    }
    return bands;
}

void EqCurveMiniPanel::syncBandsFromApvts()
{
    auto bands = readBandsFromApvts();
    lastSyncedBands_ = bands;
    eqCurve_.setBands(std::move(bands));
}

void EqCurveMiniPanel::timerCallback()
{
    auto bands = readBandsFromApvts();
    bool changed = bands.size() != lastSyncedBands_.size();
    if (!changed) {
        for (size_t i = 0; i < bands.size(); ++i) {
            const auto& a = bands[i];
            const auto& b = lastSyncedBands_[i];
            if (std::abs(a.hz - b.hz) > 0.5f || std::abs(a.gain_db - b.gain_db) > 0.01f ||
                std::abs(a.q - b.q) > 0.001f || a.enabled != b.enabled) {
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        lastSyncedBands_ = bands;
        eqCurve_.setBands(std::move(bands));
    }
}

void EqCurveMiniPanel::pushAnalysis(const AnalysisResult& analysis)
{
    eqCurve_.setSpectrumSampleRate(analysis.sample_rate);
    eqCurve_.setSpectrumUnderlayer(analysis.spectral.fft_mid_magnitudes);
    eqCurve_.setGenreReferenceCurve(analysis.spectral.centroid_hz, analysis.spectral.balance_low);
    if (analysis.resonance) eqCurve_.setResonanceMarkers(analysis.resonance->hotspots);
}

} // namespace khensu
