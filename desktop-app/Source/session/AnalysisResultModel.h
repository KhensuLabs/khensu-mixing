// desktop-app/Source/session/AnalysisResultModel.h
#pragma once
#include <any>
#include "KeyedListeners.h"
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <khensu/analysis/AnalysisResult.h>

namespace khensu {

/// The observable registry for analysis results, extracted from AppController
/// (audit M4). Panels register a callback keyed by their `this` pointer and are
/// notified whenever a fresh AnalysisResult is broadcast (a new analysis lands,
/// or an on-demand masking result is merged in). Keeping the registry here — as
/// a small, testable unit — is the first step toward panels observing analysis
/// data through a dedicated model instead of reaching into AppController.
///
/// AnalysisController remains the owner of the actual result data; this model
/// owns only "who wants to be told when it changes" and the fan-out.
class AnalysisResultModel {
  public:
    /// Register/replace a listener. `key` is any stable identity (typically the
    /// subscriber's `this`); registering the same key again replaces its callback.
    void addListener(void* key, std::function<void(const AnalysisResult&)> cb) { listeners_[key] = std::move(cb); }
    void removeListener(void* key) { listeners_.erase(key); }

    /// Fan a result out to every registered listener (skips empty slots). A
    /// listener may (de)register listeners from its callback.
    void broadcast(const AnalysisResult& result) const { notifyKeyedListeners(listeners_, result); }

    /// Cache the latest result and broadcast it. Panels that subscribe after a
    /// result already landed can read it back via hasResult()/result() instead
    /// of reaching into AppController.
    void setResult(const AnalysisResult& result)
    {
        latest_ = result;
        hasResult_ = true;
        ++version_;
        derived_.reset();
        derivedKey_.clear();
        broadcast(latest_);
    }
    bool hasResult() const noexcept { return hasResult_; }
    /// PERF-5: changes whenever the published result does (set or cleared), so
    /// a screen can skip re-pushing an unchanged result to its panels.
    uint64_t version() const noexcept { return version_; }
    const AnalysisResult& result() const noexcept { return latest_; }

    /// Session change (DSK-02): forget the cached result without notifying
    /// listeners (the session-reset fan-out tells them separately).
    void clear()
    {
        latest_ = AnalysisResult{};
        hasResult_ = false;
        ++version_;
        derived_.reset();
        derivedKey_.clear();
    }

    // ---- Derived data, computed once per final result (§10.3) -------------
    // Recommendations and the MixReport used to be computed twice on the
    // message thread for every result (AppController for the persisted
    // readiness, DashboardScreen for its tiles). The first consumer computes
    // them through derived(); everyone after reuses the cached copy until the
    // next setResult()/clear(). `Derived` is opaque here so this header stays
    // free of the recommendation engine; callers own the type.
    /// `key` must capture every input besides the result (genre, target,
    /// weight profile); a different key recomputes. Message thread only.
    template <typename Derived, typename Compute>
    const Derived& derived(const std::string& key, Compute&& compute)
    {
        auto* cached = std::any_cast<Derived>(&derived_);
        if (cached == nullptr || derivedKey_ != key) {
            derived_ = Derived(compute(latest_));
            derivedKey_ = key;
            ++derivedComputeCount_;
            cached = std::any_cast<Derived>(&derived_);
        }
        return *cached;
    }
    /// For tests: how many times derived() actually computed.
    int derivedComputeCount() const noexcept { return derivedComputeCount_; }

    bool hasListeners() const noexcept { return !listeners_.empty(); }
    size_t listenerCount() const noexcept { return listeners_.size(); }

  private:
    std::map<void*, std::function<void(const AnalysisResult&)>> listeners_;
    AnalysisResult latest_{};
    bool hasResult_ = false;
    uint64_t version_ = 0;
    std::any derived_;
    std::string derivedKey_;
    int derivedComputeCount_ = 0;
};

} // namespace khensu
