// desktop-app/Source/ui/ExportDelivery/DeliveryQaPanel.h
// Delivery & QA: one panel surfacing the mastering/delivery service tools that
// operate on the current master — compliance sheet (+PDF), codec/transcode
// artifact audit, dither preview, headphone-vs-speaker crossfeed A/B, plus the
// reference-matched chain and A/B critique (which take a second file). All
// bridge traffic runs off the message thread.
// UXR-1: the tools are grouped by goal - Check, Preview, Fix (packaging is the
// Delivery Package card) - with one primary, "Run delivery check", and each
// result shows as rows (status, value, target) instead of a monospace log.
// THREAD_CONTEXT(UIThread)
#pragma once
#include "../shared/PanelCard.h"
#include "DeliveryResultView.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace khensu {

class DeliveryQaPanel final : public PanelCard {
  public:
    explicit DeliveryQaPanel(PanelCardController& c, const juce::String& baseUrl = {});
    ~DeliveryQaPanel() override;
    void resized() override;

  public: // Action seam for the fake-service regression tests.
    juce::String sourcePath() const; ///< the file Export writes (accepted render, else original), or ""
    /// Codec ids the audit runs (the user's pick from the platform list).
    const juce::StringArray& selectedCodecs() const noexcept { return selectedCodecs_; }
    const std::vector<std::pair<juce::String, juce::String>>& codecPlatforms() const noexcept { return codecPlatforms_; }
    /// Replaces `platforms` with the live list (id, name) and keeps only the
    /// selected ids it still contains - never leaving the selection empty.
    /// An empty live list changes nothing (the seed stays).
    static void applyLivePlatforms(std::vector<std::pair<juce::String, juce::String>>& platforms,
                                   juce::StringArray& selected,
                                   const std::vector<ipc::HttpServiceBridge::CodecPlatformInfo>& live);
    void setBusy(bool busy, const juce::String& what = {});
    void showResult(const juce::String& text);
    void showResult(DeliveryResult result);
    // Run a bridge call off-thread; `call` returns a formatted result string.
    void run(const juce::String& what, std::function<juce::String()> call);
    /// UXR-1: the same, for a call that returns rows.
    void runResult(const juce::String& what, std::function<DeliveryResult()> call);
    void chooseSecondFileThen(const juce::String& what, std::function<juce::String(const juce::String&)> call);
    void chooseSecondFileThenResult(const juce::String& what, std::function<DeliveryResult(const juce::String&)> call);
    /// UXR-1: compliance sheet / codec audit / stream preview replies as rows.
    static DeliveryResult complianceResult(const juce::var& v);
    static DeliveryResult codecAuditResult(const juce::var& v);
    static DeliveryResult streamPreviewResult(const juce::var& v);
    /// CE-DEL-3: runs /delivery/stream_preview with the chosen platform /
    /// codec / device and loads the loudness-matched codec preview into the
    /// A/B "B" slot.
    void runStreamPreview();
    /// Formats a StreamPreviewResponse for the results box.
    static juce::String formatStreamPreview(const juce::var& v);
    /// Formats an ImmersiveCheckResponse for the results box.
    static juce::String formatImmersiveCheck(const juce::var& v);
    /// Formats an ImmersiveTargetsResponse: each target's limits (REF-9).
    static juce::String formatImmersiveTargets(const juce::var& v);
    /// True when the service's stream-preview options list `id` under
    /// `listKey` ("platforms" / "codecs" / "devices"). An empty id (the
    /// platform default, no device) and options not fetched yet count as
    /// offered (REF-9).
    static bool streamOptionOffered(const juce::var& options, const char* listKey, const juce::String& id);
    juce::var streamServiceOptions_; ///< GET /delivery/stream_preview/options, once fetched
    bool streamOptionsRequested_ = false;
    juce::String streamPlatform_{"spotify"}; ///< a streaming delivery-target id
    juce::String streamCodec_;               ///< "" = the platform's default
    juce::String streamDevice_;              ///< "" = no device profile

#if KHENSU_TESTS
    const juce::String resultTextForTesting() const { return results_.result().toText(); }
#endif

  private:

    // UXR-1 section headings (Check / Preview / Fix) and the pointer to packaging.
    juce::Label checkHeading_, previewHeading_, fixHeading_, packageNote_;
    juce::TextButton complianceBtn_{TRANS("Run delivery check")};
    juce::TextButton codecBtn_{TRANS("Codec audit")};
    juce::TextButton ditherBtn_{TRANS("Dither preview")};
    juce::TextButton crossfeedBtn_{TRANS("Crossfeed A/B")};
    juce::TextButton referenceBtn_{TRANS("Reference chain...")};
    juce::TextButton critiqueBtn_{TRANS("A/B critique...")};
    juce::TextButton codecPickBtn_{TRANS("Codecs...")};
    // CE-AI-3: fit a readable EQ + compressor chain toward a reference
    // (POST /render/fit_chain), render it, and report predicted vs measured
    // error. The amount slider is FEAT-3's "how far toward the target".
    juce::TextButton fitChainBtn_{TRANS("Fit chain to reference...")};
    // FEAT-3: the same fit toward the session genre's targets, and a preview
    // (fit only, no render) that shows the predicted per-band result first.
    juce::TextButton fitGenreBtn_{TRANS("Fit to genre")};
    juce::ToggleButton fitPreviewToggle_{TRANS("Preview only")};
    // CE-DEL-3 / CE-DEL-1 (DeliveryQaPanel_CeDelivery.cpp): streaming playback
    // preview with its options, and the immersive / ADM check on a chosen file.
    juce::TextButton streamBtn_{TRANS("Stream preview")};
    juce::TextButton streamOptsBtn_{TRANS("Stream options...")};
    juce::TextButton immersiveBtn_{TRANS("Immersive check...")};
    void setupDeliveryPreviewControls();
    void showStreamOptions();
    juce::Slider fitAmountSlider_;
    juce::Label fitAmountLabel_;
    /// The platform codecs GET /analyze/codec/platforms lists, as (id, name).
    /// Seeded with the service's table (the offline fallback); replaced by
    /// the live list the first time the picker opens with the service up.
    std::vector<std::pair<juce::String, juce::String>> codecPlatforms_;
    juce::StringArray selectedCodecs_;
    bool codecPlatformsLive_ = false;
    bool codecFetchInFlight_ = false;
    void showCodecPicker();
    /// Fetches the live platform list once, then shows the picker (seed on
    /// failure).
    void openCodecPicker();
    /// The injected base URL (test seam, WIRE-13), else the default service.
    std::string bridgeUrl() const;
    DeliveryResultView results_;
    /// Rows a chooseSecondFileThenResult call is producing (shown in place of its text).
    std::shared_ptr<DeliveryResult> pendingRows_;
    juce::Label statusLabel_;
    std::unique_ptr<juce::FileChooser> chooser_;
    bool busy_ = false;
    juce::String baseUrl_;
    /// Re-applies every cached palette colour. Called at construction and on
    /// each theme change.
    void applyThemeColours();

    // Declared last - see ThemeListener's note on member ordering.
    ThemeListener themeListener_{[this] {
        applyThemeColours();
        repaint();
    }};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeliveryQaPanel)
};

} // namespace khensu
