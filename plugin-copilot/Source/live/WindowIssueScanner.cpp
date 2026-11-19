#include "WindowIssueScanner.h"

#include <khensu/analysis/ArtifactDetector.h>
#include <khensu/analysis/ChannelLayout.h>
#include <khensu/analysis/DynamicsAnalyzer.h>
#include <khensu/analysis/LoudnessAnalyzer.h>
#include <khensu/analysis/SpectralAnalyzer.h>
#include <khensu/analysis/StereoAnalyzer.h>
#include <khensu/recommendations/RecommendationEngine.h>
#include <khensu/targets/CreativeTarget.h>
#include <khensu/targets/GenreEntry.h>

#include <algorithm>
#include <cmath>

namespace khensu::live {

WindowIssueScanner::WindowIssueScanner(timeline::IssueSource source, std::string fileId, double windowSec,
                                       double minWindowSec)
    : source_(source), fileId_(std::move(fileId)), windowSec_(std::max(0.5, windowSec)),
      minWindowSec_(std::clamp(minWindowSec, 0.1, std::max(0.5, windowSec)))
{
}

void WindowIssueScanner::reset()
{
    filled_ = 0;
    channels_ = 0;
    sampleRate_ = 0.0;
    expectedNextSec_ = -1.0;
}

AnalysisResult WindowIssueScanner::measure(const juce::AudioBuffer<float>& audio, double sampleRate,
                                           const std::string& fileId)
{
    AnalysisResult result;
    const int ch = audio.getNumChannels();
    const int n = audio.getNumSamples();
    if (ch <= 0 || n <= 0 || sampleRate <= 0.0) return result;
    const auto layoutResult = ChannelLayout::fromChannelCount(ch);
    if (layoutResult.is_err()) return result;
    const ChannelLayout layout = layoutResult.value();

    LoudnessAnalyzer loudness;
    SpectralAnalyzer spectral;
    DynamicsAnalyzer dynamics;
    StereoAnalyzer stereo;
    ArtifactDetector artifacts;
    loudness.reset(sampleRate, layout);
    spectral.reset(sampleRate, layout);
    dynamics.reset(sampleRate, layout);
    stereo.reset(layout);
    artifacts.reset(sampleRate);

    // Same block size as AudioAnalyzer so a window measures exactly as the
    // offline path would measure the same samples.
    constexpr int kBlock = 4096;
    std::vector<const float*> ptrs(static_cast<size_t>(ch));
    for (int start = 0; start < n; start += kBlock) {
        const int count = std::min(kBlock, n - start);
        for (int c = 0; c < ch; ++c) ptrs[static_cast<size_t>(c)] = audio.getReadPointer(c, start);
        loudness.process(ptrs.data(), ch, count);
        spectral.process(ptrs.data(), ch, count);
        dynamics.process(ptrs.data(), ch, count);
        stereo.process(ptrs.data(), ch, count);
        artifacts.process(ptrs.data(), ch, count);
    }

    result.file_id = fileId;
    result.is_partial = true; // a window, not a whole track
    result.sample_rate = sampleRate;
    result.channel_layout.layout_name = layout.name;
    result.channel_layout.channel_count = layout.numChannels;
    result.channel_layout.lfe_excluded = layout.hasLFE();
    result.loudness = loudness.getResult();
    result.spectral = spectral.getResult();
    result.dynamics = dynamics.getResult();
    result.stereo = stereo.getResult();
    result.phase = stereo.getPhaseResult();
    result.artifacts = artifacts.getResult();
    return result;
}

std::vector<timeline::TimedIssue> WindowIssueScanner::analyseWindow(const juce::AudioBuffer<float>& audio,
                                                                    int numSamples, double sampleRate,
                                                                    double startSec, timeline::IssueSource source,
                                                                    const std::string& fileId,
                                                                    const std::string& regionName)
{
    numSamples = std::min(numSamples, audio.getNumSamples());
    if (numSamples <= 0 || sampleRate <= 0.0) return {};
    // A non-owning view of the filled part of the window.
    juce::AudioBuffer<float> view(const_cast<float* const*>(audio.getArrayOfReadPointers()), audio.getNumChannels(),
                                  numSamples);
    const auto result = measure(view, sampleRate, fileId);
    if (result.channel_layout.channel_count <= 0) return {};

    RecommendationEngine engine;
    const GenreEntry genre;
    const CreativeTarget target;
    const auto recs = engine.generate(result, genre, target);
    const double endSec = startSec + static_cast<double>(numSamples) / sampleRate;
    return timeline::issuesFromRecommendations(recs, startSec, startSec, endSec, source, fileId, regionName);
}

void WindowIssueScanner::closeWindow(const WindowSink& sink, bool allowPartial)
{
    if (filled_ <= 0 || sampleRate_ <= 0.0) {
        filled_ = 0;
        return;
    }
    const double lengthSec = static_cast<double>(filled_) / sampleRate_;
    if (!allowPartial && lengthSec + 1e-9 < windowSec_) return;
    if (lengthSec + 1e-9 < minWindowSec_) {
        ++discarded_;
        filled_ = 0;
        return;
    }
    Window w;
    w.startSec = windowStartSec_;
    w.endSec = windowStartSec_ + lengthSec;
    w.issues = analyseWindow(window_, filled_, sampleRate_, windowStartSec_, source_, fileId_, regionName_);
    ++measured_;
    filled_ = 0;
    if (sink) sink(std::move(w));
}

void WindowIssueScanner::feed(const float* const* channels, int numChannels, int numSamples, double timeSec,
                              double sampleRate, const WindowSink& sink)
{
    if (channels == nullptr || numChannels <= 0 || numSamples <= 0 || sampleRate <= 0.0) return;

    // Format change: what is buffered cannot be measured with the new rate.
    if (numChannels != channels_ || std::abs(sampleRate - sampleRate_) > 0.5) {
        closeWindow(sink, true);
        channels_ = numChannels;
        sampleRate_ = sampleRate;
        window_.setSize(channels_, static_cast<int>(std::ceil(windowSec_ * sampleRate_)) + 1, false, true, false);
        filled_ = 0;
        expectedNextSec_ = -1.0;
    }

    // A jump on the host timeline (loop, seek, stop/start, dropped block).
    if (expectedNextSec_ >= 0.0 && std::abs(timeSec - expectedNextSec_) > kJumpToleranceSec) closeWindow(sink, true);

    const int capacity = static_cast<int>(std::ceil(windowSec_ * sampleRate_));
    int offset = 0;
    while (offset < numSamples) {
        if (filled_ == 0) windowStartSec_ = timeSec + static_cast<double>(offset) / sampleRate_;
        const int count = std::min(numSamples - offset, capacity - filled_);
        for (int c = 0; c < channels_; ++c) window_.copyFrom(c, filled_, channels[c] + offset, count);
        filled_ += count;
        offset += count;
        if (filled_ >= capacity) closeWindow(sink, false);
    }
    expectedNextSec_ = timeSec + static_cast<double>(numSamples) / sampleRate_;
}

void WindowIssueScanner::flush(const WindowSink& sink)
{
    closeWindow(sink, true);
    expectedNextSec_ = -1.0;
}

} // namespace khensu::live
