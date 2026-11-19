// tests/ui/CeDeliveryPanelsTest.cpp
// CE-DEL-1..4: the delivery package panel's guards and the result formatters
// for the stream preview, immersive check, package and content credentials.
#include <catch2/catch_test_macros.hpp>
#include "support/FakeAppController.h"
#include "../../desktop-app/Source/ui/ExportDelivery/DeliveryPackagePanel.h"
#include "../../desktop-app/Source/ui/ExportDelivery/DeliveryQaPanel.h"

namespace {
juce::var parse(const char* json) { return juce::JSON::parse(juce::String(json)); }
} // namespace

TEST_CASE("Delivery package panel guards and defaults", "[exportdelivery][cedel]")
{
    khensu::test::FakeAppController controller;
    khensu::DeliveryPackagePanel panel(controller);
    CHECK(panel.selectedProfileId() == "streaming_distributor");
    CHECK(panel.selectedAiUse() == "none");
    panel.build(); // no session: no job, no background work
    CHECK(controller.runInBackgroundCallCount == 0);
    CHECK(panel.statusTextForTesting().containsIgnoreCase("session"));
}

TEST_CASE("Delivery package result formatting", "[exportdelivery][cedel]")
{
    const auto r = parse(R"({"output_dir":"C:/x/Song_delivery","manifest_path":"C:/x/Song_delivery/manifest.json",
        "profile_id":"cd_replication","sample_rate":44100,"bit_depth":16,"overall":"warn",
        "c2pa":{"signed":false,"reason":"not set"},"warnings":["The DDP fileset has not been checked"],
        "files":[{"path":"Song.wav","role":"master","sha256":"ab","bytes":10,"overall":"pass","true_peak_dbtp":-1.2},
                 {"path":"Song_DDP/DDPID","role":"ddp","sha256":"cd","bytes":128},
                 {"path":"Song.wav.provenance.json","role":"provenance_sidecar","sha256":"ef","bytes":5}]})");
    const auto text = khensu::DeliveryPackagePanel::formatResult(r);
    CHECK(text.contains("WARN"));
    CHECK(text.contains("44100 Hz / 16-bit"));
    CHECK(text.contains("unsigned sidecar only - not set"));
    CHECK(text.contains("Song.wav  [master]  PASS  -1.20 dBTP"));
    CHECK(text.contains("DDP fileset: 1 files"));
    CHECK_FALSE(text.contains("provenance.json"));
    CHECK(text.contains("(!) The DDP fileset"));
}

TEST_CASE("Content credential formatting", "[exportdelivery][cedel]")
{
    const auto signedResp = parse(R"({"file_path":"a.wav","signed":true,"c2pa_available":true,
        "credentials_configured":true,"sidecar_path":"a_c2pa.wav.provenance.json","sha256":"00ff","action":"edited",
        "ai_use":"ai_assisted","software_agent":"Khensu Mixing 0.1.0","output_path":"a_c2pa.wav","alg":"es256",
        "validation_state":"Valid",
        "digital_source_type":"http://cv.iptc.org/newscodes/digitalsourcetype/compositeWithTrainedAlgorithmicMedia"})");
    const auto s = khensu::DeliveryPackagePanel::formatSign(signedResp);
    CHECK(s.contains("Signed copy -> a_c2pa.wav  (es256, Valid)"));
    CHECK(s.contains("compositeWithTrainedAlgorithmicMedia"));
    const auto unsignedResp = parse(R"({"signed":false,"reason":"No signing certificate","sidecar_path":"b.json","sha256":"1"})");
    CHECK(khensu::DeliveryPackagePanel::formatSign(unsignedResp).contains("Not signed: No signing certificate"));

    const auto read = parse(R"({"file_path":"a.wav","sha256":"1","c2pa_available":true,"embedded":true,
        "manifest":{"claim_generator":"Khensu Mixing 0.1.0","validation_state":"Valid","trusted":false,
                    "actions":[{"action":"c2pa.edited","software_agent":"Khensu Mixing 0.1.0",
                    "digital_source_type":"http://cv.iptc.org/newscodes/digitalsourcetype/trainedAlgorithmicMedia"}],
                    "ingredient_count":1,"manifest_count":2,"validation_codes":[]},
        "sidecar":{"path":"a.wav.provenance.json","present":true,"sha256_matches":false}})");
    const auto t = khensu::DeliveryPackagePanel::formatRead(read);
    CHECK(t.contains("untrusted signer"));
    CHECK(t.contains("c2pa.edited  trainedAlgorithmicMedia"));
    CHECK(t.contains("does NOT match"));
}

TEST_CASE("Stream preview and immersive formatting", "[exportdelivery][cedel]")
{
    const auto sp = parse(R"({"platform":"spotify","platform_name":"Spotify","codec":"aac","bitrate_kbps":256,
        "codec_mode":"simulated","simulated_reason":"ffmpeg is not on PATH","sample_rate":44100,
        "alignment_lag_samples":0,"master":{"integrated_lufs":-9.0,"true_peak_dbtp":-0.3},
        "decoded":{"integrated_lufs":-9.1,"true_peak_dbtp":0.2},"after_platform":{"integrated_lufs":-14.0,"true_peak_dbtp":-4.8},
        "final":{"integrated_lufs":-14.0,"true_peak_dbtp":-4.8},"reference":{"integrated_lufs":-14.0,"true_peak_dbtp":-5.3},
        "codec_distortion_db":-31.5,"true_peak_delta_db":0.5,
        "artifacts":{"hf_loss_db":1.2,"air_loss_db":0.3,"stereo_width_delta":-0.01,"true_peak_delta_dbtp":0.5,"pre_echo_score":0.0},
        "platform_gain_db":-5.0,"limiter_engaged":false,"match_gain_db":0.0,"preview_path":"p.wav",
        "matched_preview_path":"m.wav","reference_path":"r.wav","notes":["Codec SIMULATED"]})");
    const auto s = khensu::DeliveryQaPanel::formatStreamPreview(sp);
    CHECK(s.contains("SIMULATED codec: ffmpeg is not on PATH"));
    CHECK(s.contains("B (codec, matched) -> m.wav"));
    CHECK(s.contains("-5.0 dB -> -14.0 LUFS"));

    const auto im = parse(R"({"file_path":"x.wav","container":"BW64","sample_rate":48000,"channels":12,"frames":1,
        "duration_sec":1.0,"layout":"7.1.4","layout_source":"adm",
        "adm":{"present":true,"chna_tracks":12,"programmes":[],"packs":[],"has_objects":false,"object_count":0},
        "channel_list":[{"index":0,"label":"L","azimuth":30.0,"elevation":0.0,"is_lfe":false,"kind":"speaker",
                         "weight":1.0,"true_peak_dbtp":-3.0,"silent":false}],
        "integrated_lufs":-18.4,"max_true_peak_dbtp":-3.0,
        "fold_down":{"integrated_lufs":-16.0,"loudness_delta_lu":2.4,"true_peak_dbtp":-1.0,"lr_correlation":0.4,
                     "issues":["The LFE channel is dropped"]},
        "binaural":{"integrated_lufs":-17.0,"loudness_delta_lu":1.4,"model":"synthetic"},
        "compliance":{"target_id":"atmos_music","name":"Dolby Atmos music","category":"immersive",
                      "measurement":"immersive","overall":"pass",
                      "checks":[{"id":"integrated","status":"pass","measured":-18.4,"target":-18.0,"delta":-0.4,
                                 "note":"ok","fix":""}]},
        "notes":[]})");
    const auto t = khensu::DeliveryQaPanel::formatImmersiveCheck(im);
    CHECK(t.contains("Layout 7.1.4  (from adm)"));
    CHECK(t.contains("-18.4 LUFS"));
    CHECK(t.contains("(!) The LFE channel is dropped"));
    CHECK(t.contains("Dolby Atmos music: PASS"));
}
