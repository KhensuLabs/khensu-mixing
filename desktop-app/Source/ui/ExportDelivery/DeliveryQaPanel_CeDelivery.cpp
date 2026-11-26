// desktop-app/Source/ui/ExportDelivery/DeliveryQaPanel_CeDelivery.cpp
// CE-DEL-3 streaming playback preview (real codec round trip or labelled
// simulation -> platform normalisation -> device, loudness-matched A/B) and
// the CE-DEL-1 immersive / ADM check, on the Delivery & QA panel.
#include "DeliveryQaPanel.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include "../shared/KhensuTheme.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <iterator>

namespace af = khensu::api::fields;

namespace khensu {

namespace {
juce::String n1(const juce::var& v, int dp = 1)
{
    return (v.isDouble() || v.isInt() || v.isInt64()) ? juce::String((double)v, dp) : TRANS("n/a");
}

// (id, display name). Ids are service data - never translated.
struct Choice {
    const char* id;
    const char* name;
};
constexpr Choice kPlatforms[] = {{"spotify", NEEDS_TRANS("Spotify")},
                                 {"spotify_loud", NEEDS_TRANS("Spotify (Loud)")},
                                 {"apple_music", NEEDS_TRANS("Apple Music")},
                                 {"youtube", NEEDS_TRANS("YouTube")},
                                 {"tidal", NEEDS_TRANS("Tidal")},
                                 {"amazon_music", NEEDS_TRANS("Amazon Music")},
                                 {"deezer", NEEDS_TRANS("Deezer")}};
constexpr Choice kCodecs[] = {{"", NEEDS_TRANS("Platform default")},
                              {"aac", NEEDS_TRANS("AAC")},
                              {"opus", NEEDS_TRANS("Opus")},
                              {"mp3", NEEDS_TRANS("MP3")}};
constexpr Choice kDevices[] = {{"", NEEDS_TRANS("No device")},
                               {"phone", NEEDS_TRANS("Phone speaker")},
                               {"earbuds", NEEDS_TRANS("Earbuds (binaural)")},
                               {"car", NEEDS_TRANS("Car")},
                               {"club", NEEDS_TRANS("Club PA")}};
constexpr int kPlatformBase = 100, kCodecBase = 200, kDeviceBase = 300;

juce::String nameFor(const Choice* table, size_t n, const juce::String& id)
{
    for (size_t i = 0; i < n; ++i)
        if (id == table[i].id) return juce::translate(table[i].name);
    return id;
}

juce::String levels(const juce::var& stage)
{
    return n1(stage[af::StreamStageLevels::integrated_lufs]) + " LUFS / " +
           n1(stage[af::StreamStageLevels::true_peak_dbtp], 2) + " dBTP";
}
} // namespace

void DeliveryQaPanel::setupDeliveryPreviewControls()
{
    streamBtn_.setTooltip(TRANS("Hear the master as a streaming listener does: a real codec round trip through "
                                "ffmpeg (or a labelled simulation without it), the platform's loudness "
                                "normalisation and limiter, and an optional playback device. Loads the "
                                "loudness-matched preview into the A/B B slot."));
    streamBtn_.onClick = [this] { runStreamPreview(); };
    streamOptsBtn_.setTooltip(TRANS("Choose the platform, codec and playback device for the stream preview."));
    streamOptsBtn_.onClick = [this] { showStreamOptions(); };
    immersiveBtn_.setTooltip(TRANS("Check a multichannel or ADM BWF master (5.1, 7.1, 7.1.4): BS.1770 "
                                   "multichannel loudness, per-channel true peak, the stereo fold-down and a "
                                   "binaural render, graded against Dolby Atmos music delivery."));
    immersiveBtn_.onClick = [this] {
        chooseSecondFileThen("Immersive check", [url = bridgeUrl()](const juce::String& file) -> juce::String {
            ipc::HttpServiceBridge bridge(url);
            auto res = bridge.immersiveCheck(file);
            if (res.is_err()) return serviceErrorLine(res.error());
            juce::String text = formatImmersiveCheck(res.value());
            // The other immersive delivery targets, so the grade can be read
            // against them (best effort: the check stands without them).
            if (auto targets = bridge.immersiveTargets(); targets.is_ok())
                text << "\n" << formatImmersiveTargets(targets.value());
            return text;
        });
    };
}

juce::String DeliveryQaPanel::formatImmersiveTargets(const juce::var& v)
{
    namespace T = af::ImmersiveTarget;
    juce::String out;
    const auto* targets = v[af::ImmersiveTargetsResponse::targets].getArray();
    if (targets == nullptr || targets->isEmpty()) return out;
    out << TRANS("Immersive delivery targets") << ":\n";
    for (const auto& t : *targets)
        out << "  " << t[T::name].toString() << ": <= " << n1(t[T::lufs_integrated_max]) << " LUFS, "
            << n1(t[T::true_peak_dbtp]) << " dBTP, " << t[T::sample_rate].toString() << " Hz / "
            << t[T::bit_depth].toString() << "-bit  (" << t[T::standard].toString() << ")\n";
    return out;
}

bool DeliveryQaPanel::streamOptionOffered(const juce::var& options, const char* listKey, const juce::String& id)
{
    if (id.isEmpty() || !options.isObject()) return true;
    const auto* list = options[listKey].getArray();
    if (list == nullptr) return true;
    for (const auto& item : *list)
        if (item.toString() == id) return true;
    return false;
}

void DeliveryQaPanel::showStreamOptions()
{
    // REF-9: what the service supports on this machine, fetched once. The
    // menu opens straight away and reopens when the answer arrives.
    if (!streamOptionsRequested_) {
        streamOptionsRequested_ = true;
        juce::Component::SafePointer<DeliveryQaPanel> self(this);
        controller_.runInBackground([self, url = bridgeUrl()] {
            auto res = ipc::HttpServiceBridge(url).streamPreviewOptions();
            juce::MessageManager::callAsync([self, res] {
                if (self == nullptr) return;
                if (res.is_err()) {
                    self->streamOptionsRequested_ = false; // ask again next time
                    return;
                }
                self->streamServiceOptions_ = res.value();
            });
        });
    }
    const auto& opts = streamServiceOptions_;
    namespace O = af::StreamPreviewOptionsResponse;
    juce::PopupMenu platforms, codecs, devices;
    for (size_t i = 0; i < std::size(kPlatforms); ++i)
        platforms.addItem(kPlatformBase + (int)i, juce::translate(kPlatforms[i].name),
                          streamOptionOffered(opts, O::platforms, kPlatforms[i].id),
                          streamPlatform_ == kPlatforms[i].id);
    for (size_t i = 0; i < std::size(kCodecs); ++i)
        codecs.addItem(kCodecBase + (int)i, juce::translate(kCodecs[i].name),
                       streamOptionOffered(opts, O::codecs, kCodecs[i].id), streamCodec_ == kCodecs[i].id);
    for (size_t i = 0; i < std::size(kDevices); ++i)
        devices.addItem(kDeviceBase + (int)i, juce::translate(kDevices[i].name),
                        streamOptionOffered(opts, O::devices, kDevices[i].id), streamDevice_ == kDevices[i].id);
    juce::PopupMenu menu;
    menu.addSectionHeader(TRANS("Stream preview"));
    if (opts.isObject())
        menu.addSectionHeader(static_cast<bool>(opts[O::ffmpeg_available])
                                  ? TRANS("Real codec round trip (ffmpeg found)")
                                  : TRANS("Codecs simulated - install ffmpeg for a real round trip"));
    menu.addSubMenu(TRANS("Platform") + ": " + nameFor(kPlatforms, std::size(kPlatforms), streamPlatform_), platforms);
    menu.addSubMenu(TRANS("Codec") + ": " + nameFor(kCodecs, std::size(kCodecs), streamCodec_), codecs);
    menu.addSubMenu(TRANS("Device") + ": " + nameFor(kDevices, std::size(kDevices), streamDevice_), devices);
    juce::Component::SafePointer<DeliveryQaPanel> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&streamOptsBtn_), [self](int result) {
        if (self == nullptr || result <= 0) return;
        if (result >= kDeviceBase && result < kDeviceBase + (int)std::size(kDevices))
            self->streamDevice_ = kDevices[result - kDeviceBase].id;
        else if (result >= kCodecBase && result < kCodecBase + (int)std::size(kCodecs))
            self->streamCodec_ = kCodecs[result - kCodecBase].id;
        else if (result >= kPlatformBase && result < kPlatformBase + (int)std::size(kPlatforms))
            self->streamPlatform_ = kPlatforms[result - kPlatformBase].id;
        self->showStreamOptions(); // stay open for the next choice
    });
}

void DeliveryQaPanel::runStreamPreview()
{
    if (busy_) return;
    const juce::String src = sourcePath();
    if (src.isEmpty()) {
        showResult(TRANS("No active session."));
        return;
    }
    setBusy(true, TRANS("Stream preview"));
    juce::Component::SafePointer<DeliveryQaPanel> self(this);
    controller_.runInBackground([self, &work = controller_, src, platform = streamPlatform_, codec = streamCodec_,
                                 device = streamDevice_, url = bridgeUrl()]() {
        ipc::HttpServiceBridge bridge(url);
        auto res = bridge.streamPreview(src, platform, codec, 0, device);
        postIfCurrent(self, work, [res](DeliveryQaPanel& panel) {
            if (res.is_err()) {
                panel.showResult(serviceErrorLine(res.error()));
                return;
            }
            const auto v = res.value();
            const juce::String matched = v[af::StreamPreviewResponse::matched_preview_path].toString();
            auto result = streamPreviewResult(v);
            if (matched.isNotEmpty() && juce::File(matched).existsAsFile()) {
                // B = the codec preview at the reference's loudness; the app's
                // own level-matched A/B then compares it with the master.
                panel.controller_.setAbProcessedPath(matched);
                result.notes.add(TRANS("Loaded into the A/B B slot - toggle A/B to compare with your master."));
            }
            panel.showResult(std::move(result));
        }, [](DeliveryQaPanel& panel) { panel.setBusy(false); });
    });
}

juce::String DeliveryQaPanel::formatStreamPreview(const juce::var& v)
{
    namespace S = af::StreamPreviewResponse;
    const bool real = v[S::codec_mode].toString() == "ffmpeg";
    juce::String out;
    out << v[S::platform_name].toString() << "  -  " << v[S::codec].toString().toUpperCase() << " "
        << v[S::bitrate_kbps].toString() << " kbps";
    if (v[S::device].isString()) out << "  -  " << v[S::device].toString();
    out << "\n" << (real ? TRANS("Real encode (ffmpeg).") : TRANS("SIMULATED codec:") + " " + v[S::simulated_reason].toString())
        << "\n\n";
    out << TRANS("Master") << ":          " << levels(v[S::master]) << "\n";
    out << TRANS("After decode") << ":    " << levels(v[S::decoded]) << "   (" << TRANS("true peak change") << " "
        << n1(v[S::true_peak_delta_db], 2) << " dB)\n";
    const auto art = v[S::artifacts];
    out << TRANS("Codec distortion") << ": " << n1(v[S::codec_distortion_db]) << " dB,  " << TRANS("HF loss") << " "
        << n1(art[af::StreamCodecArtifacts::hf_loss_db]) << " dB,  " << TRANS("width change") << " "
        << n1(art[af::StreamCodecArtifacts::stereo_width_delta], 3) << "\n";
    out << TRANS("Platform gain") << ":   " << n1(v[S::platform_gain_db]) << " dB -> " << levels(v[S::after_platform]);
    if ((bool)v[S::limiter_engaged]) out << "  (" << TRANS("limiter") << ")";
    out << "\n" << TRANS("As heard") << ":        " << levels(v[S::final]) << "\n\n";
    out << "A (" << TRANS("no codec") << ") -> " << v[S::reference_path].toString() << "\n";
    out << "B (" << TRANS("codec, matched") << ") -> " << v[S::matched_preview_path].toString() << "\n";
    if (auto* notes = v[S::notes].getArray())
        for (const auto& n : *notes)
            out << "- " << n.toString() << "\n";
    return out;
}

DeliveryResult DeliveryQaPanel::streamPreviewResult(const juce::var& v)
{
    namespace S = af::StreamPreviewResponse;
    const bool real = v[S::codec_mode].toString() == "ffmpeg";
    DeliveryResult r;
    r.title = v[S::platform_name].toString() + "  -  " + v[S::codec].toString().toUpperCase() + " " +
              v[S::bitrate_kbps].toString() + " kbps";
    r.add(DeliveryResult::Status::Info, TRANS("Master"), levels(v[S::master]));
    const auto tpDelta = v[S::true_peak_delta_db];
    const bool tpRose = (tpDelta.isDouble() || tpDelta.isInt()) && static_cast<double>(tpDelta) > 0.5;
    r.add(tpRose ? DeliveryResult::Status::Warn : DeliveryResult::Status::Info, TRANS("After decode"),
          levels(v[S::decoded]), TRANS("true peak change") + " " + n1(tpDelta, 2) + " dB");
    const auto art = v[S::artifacts];
    r.add(DeliveryResult::Status::Info, TRANS("Codec distortion"), n1(v[S::codec_distortion_db]) + " dB",
          TRANS("HF loss") + " " + n1(art[af::StreamCodecArtifacts::hf_loss_db]) + " dB");
    r.add((bool)v[S::limiter_engaged] ? DeliveryResult::Status::Warn : DeliveryResult::Status::Info,
          TRANS("Platform gain"), n1(v[S::platform_gain_db]) + " dB",
          (bool)v[S::limiter_engaged] ? TRANS("limiter engaged") : juce::String());
    r.add(DeliveryResult::Status::Info, TRANS("As heard"), levels(v[S::final]));
    r.notes.add(real ? TRANS("Real encode (ffmpeg).") : TRANS("SIMULATED codec:") + " " + v[S::simulated_reason].toString());
    if (auto* notes = v[S::notes].getArray())
        for (const auto& n : *notes)
            r.notes.add(n.toString());
    return r;
}

juce::String DeliveryQaPanel::formatImmersiveCheck(const juce::var& v)
{
    namespace I = af::ImmersiveCheckResponse;
    juce::String out;
    out << TRANS("Layout") << " " << v[I::layout].toString() << "  (" << TRANS("from") << " "
        << v[I::layout_source].toString() << "),  " << v[I::channels].toString() << " ch,  "
        << v[I::sample_rate].toString() << " Hz,  " << v[I::container].toString() << "\n";
    if ((bool)v[I::adm][af::ImmersiveAdm::present])
        out << "ADM: " << v[I::adm][af::ImmersiveAdm::chna_tracks].toString() << " " << TRANS("tracks") << ", "
            << v[I::adm][af::ImmersiveAdm::object_count].toString() << " " << TRANS("objects") << "\n";
    out << TRANS("Integrated (BS.1770, LFE excluded)") << ": " << n1(v[I::integrated_lufs]) << " LUFS,  "
        << TRANS("max true peak") << " " << n1(v[I::max_true_peak_dbtp], 2) << " dBTP\n\n";
    if (auto* chans = v[I::channel_list].getArray())
        for (const auto& c : *chans)
            out << "  " << c[af::ImmersiveChannel::label].toString().paddedRight(' ', 6) << " "
                << n1(c[af::ImmersiveChannel::true_peak_dbtp], 2) << " dBTP   w " << n1(c[af::ImmersiveChannel::weight], 2)
                << "\n";
    const auto fd = v[I::fold_down];
    out << "\n" << TRANS("Stereo fold-down") << ": " << n1(fd[af::ImmersiveFoldDown::integrated_lufs]) << " LUFS ("
        << n1(fd[af::ImmersiveFoldDown::loudness_delta_lu]) << " LU),  "
        << n1(fd[af::ImmersiveFoldDown::true_peak_dbtp], 2) << " dBTP,  L/R corr "
        << n1(fd[af::ImmersiveFoldDown::lr_correlation], 2) << "\n";
    if (auto* issues = fd[af::ImmersiveFoldDown::issues].getArray())
        for (const auto& i : *issues)
            out << "  (!) " << i.toString() << "\n";
    const auto bi = v[I::binaural];
    out << TRANS("Binaural") << ": " << n1(bi[af::ImmersiveBinaural::integrated_lufs]) << " LUFS ("
        << n1(bi[af::ImmersiveBinaural::loudness_delta_lu]) << " LU)\n";
    const auto comp = v[I::compliance];
    out << "\n" << comp[af::ImmersiveCompliance::name].toString() << ": "
        << comp[af::ImmersiveCompliance::overall].toString().toUpperCase() << "\n";
    if (auto* checks = comp[af::ImmersiveCompliance::checks].getArray())
        for (const auto& c : *checks) {
            out << "  " << c[af::DeliveryCheck::id].toString() << ": " << c[af::DeliveryCheck::status].toString()
                << " - " << c[af::DeliveryCheck::note].toString();
            if (c[af::DeliveryCheck::fix].toString().isNotEmpty()) out << " " << c[af::DeliveryCheck::fix].toString();
            out << "\n";
        }
    if (fd[af::ImmersiveFoldDown::output_path].isString())
        out << "\n" << TRANS("Fold-down") << " -> " << fd[af::ImmersiveFoldDown::output_path].toString() << "\n";
    if (bi[af::ImmersiveBinaural::output_path].isString())
        out << TRANS("Binaural") << " -> " << bi[af::ImmersiveBinaural::output_path].toString() << "\n";
    if (auto* notes = v[I::notes].getArray())
        for (const auto& n : *notes)
            out << "- " << n.toString() << "\n";
    return out;
}

} // namespace khensu
