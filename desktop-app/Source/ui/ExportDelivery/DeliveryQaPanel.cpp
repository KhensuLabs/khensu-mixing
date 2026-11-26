// desktop-app/Source/ui/ExportDelivery/DeliveryQaPanel.cpp
#include "DeliveryQaPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/SessionEpoch.h"
#include <khensu/session/Session.h>
#include "../shared/KhensuTheme.h"
#include "../shared/ButtonRole.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/RenderPaths.h"
#include "../shared/SessionAudioPaths.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace af = khensu::api::fields;

namespace khensu {

namespace {
juce::String num(const juce::var& v, int dp = 2)
{
    return v.isVoid() ? juce::String("-") : juce::String((double)v, dp);
}
} // namespace

// Every cached palette colour on this component, in one place (UI-01).
void DeliveryQaPanel::applyThemeColours()
{
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    fitAmountLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    fitAmountSlider_.setColour(juce::Slider::trackColourId, KhensuTheme::accentTeal);
    fitAmountSlider_.setColour(juce::Slider::textBoxTextColourId, KhensuTheme::textPrimary);
    for (auto* l : {&checkHeading_, &previewHeading_, &fixHeading_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    packageNote_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

namespace {
/// FEAT-3: one text for a fitted chain toward a reference or a genre:
/// the chain, the band error before / predicted / measured, the predicted
/// per-band result (always - it is what a preview is for) and the render.
juce::String formatFitChain(const juce::var& v, double amount, const juce::String& title)
{
    namespace FC = af::FitChainResponse;
    const auto chain = v[FC::chain];
    auto band = [](const juce::String& name, const juce::var& b) {
        return "  " + name + ": " + num(b[af::FitBand::frequency_hz], 0) + " Hz  " +
               num(b[af::FitBand::gain_db], 1) + " dB  Q " + num(b[af::FitBand::q], 2) + "\n";
    };
    juce::String text = title + " (" +
                        juce::String(juce::roundToInt(amount * 100.0)) + "%):\n";
    text << band(TRANS("Low shelf"), chain[af::FittedChain::low_shelf]);
    if (auto* peaks = chain[af::FittedChain::peaks].getArray())
        for (const auto& p : *peaks)
            text << band(TRANS("Peak"), p);
    text << band(TRANS("High shelf"), chain[af::FittedChain::high_shelf]);
    const auto comp = chain[af::FittedChain::compressor];
    if (comp.isObject())
        text << "  " << TRANS("Compressor") << ": " << num(comp[af::FitCompressor::threshold_db], 1)
             << " dB  " << num(comp[af::FitCompressor::ratio], 1) << ":1  "
             << num(comp[af::FitCompressor::attack_ms], 0) << " / "
             << num(comp[af::FitCompressor::release_ms], 0) << " ms\n";
    else
        text << "  " << TRANS("Compressor: none") << "\n";
    text << "  " << TRANS("Makeup gain") << ": " << num(chain[af::FittedChain::makeup_gain_db], 1) << " dB\n\n";
    auto errors = [](const juce::String& name, const juce::var& e) {
        return name + ": " + TRANS("band error") + " " + num(e[af::FitErrors::band_error_db], 2) + " dB,  " +
               TRANS("crest") + " " + num(e[af::FitErrors::crest_db], 1) + " dB,  LRA " +
               num(e[af::FitErrors::lra_lu], 1) + " LU\n";
    };
    text << errors(TRANS("Before"), v[FC::before]);
    text << errors(TRANS("Predicted"), v[FC::predicted]);
    if (v[FC::measured].isObject()) text << errors(TRANS("Measured"), v[FC::measured]);
    const auto* hz = v[FC::band_centers_hz].getArray();
    const auto* target = v[FC::target_delta_db].getArray();
    const auto* predicted = v[FC::predicted_delta_db].getArray();
    if (hz != nullptr && target != nullptr && predicted != nullptr && hz->size() == target->size() &&
        hz->size() == predicted->size()) {
        text << "\n" << TRANS("Per band, the move wanted -> the move predicted:") << "\n";
        for (int i = 0; i < hz->size(); ++i)
            text << "  " << num((*hz)[i], 0) << " Hz: " << num((*target)[i], 1) << " -> " << num((*predicted)[i], 1)
                 << " dB\n";
    }
    const auto state = v[FC::render_state].toString();
    if (state == "validated")
        text << "\n" << TRANS("Rendered") << " -> " << v[FC::output_path].toString() << "\n";
    else if (state.isNotEmpty())
        text << "\n" << TRANS("Render") << " " << state << ": " << v[FC::render_error].toString() << "\n";
    if (auto* notes = v[FC::notes].getArray())
        for (const auto& n : *notes)
            text << "\n- " << n.toString();
    return text;
}
} // namespace

DeliveryQaPanel::DeliveryQaPanel(PanelCardController& c, const juce::String& baseUrl)
    : PanelCard(c, "Delivery & QA"), baseUrl_(baseUrl)
{
    applyThemeColours();
    setExplainer(TRANS("Delivery & QA"), TRANS("Ship-readiness tools on the current master: a compliance sheet (true "
                                  "peak / mono / DC / clipping / metadata, with PDF), a codec/transcode "
                                  "artifact audit ('the Spotify encode'), a dither & noise-shaping "
                                  "preview, headphone-vs-speaker crossfeed A/B, a reference-matched "
                                  "corrective chain, and a measurement-grounded A/B critique of two takes."));

    // UXR-1: one primary (the delivery check); every other tool is a
    // secondary, grouped under its goal.
    for (auto* b : {&complianceBtn_, &codecBtn_, &ditherBtn_, &crossfeedBtn_, &referenceBtn_, &critiqueBtn_,
                    &codecPickBtn_, &fitChainBtn_, &streamBtn_, &streamOptsBtn_, &immersiveBtn_})
        addAndMakeVisible(b);
    button_role::markPrimary(complianceBtn_);
    complianceBtn_.setTooltip(TRANS("Check the current master for delivery: true peak, mono fold-down, DC, "
                                    "clipping and metadata, with a PDF compliance sheet."));
    codecBtn_.setTooltip(TRANS("Encode the master through the chosen platform codecs and measure what they "
                               "change (high-frequency loss, width, true peak)."));
    ditherBtn_.setTooltip(TRANS("Compare dither and noise shaping at 16 bits against plain truncation."));
    crossfeedBtn_.setTooltip(TRANS("Render a headphone (in-head) and a speaker (crossfeed) version for an A/B."));
    critiqueBtn_.setTooltip(TRANS("Compare this master with another take, measurement by measurement."));
    referenceBtn_.setTooltip(TRANS("A corrective chain that moves this master toward a reference track."));
    auto heading = [this](juce::Label& l, const juce::String& text) {
        l.setText(text, juce::dontSendNotification);
        l.setFont(KhensuTheme::sectionHeaderFont());
        addAndMakeVisible(l);
    };
    heading(checkHeading_, TRANS("Check"));
    heading(previewHeading_, TRANS("Preview"));
    heading(fixHeading_, TRANS("Fix"));
    packageNote_.setText(TRANS("Package: the Delivery Package card builds the release folder."),
                         juce::dontSendNotification);
    packageNote_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
    addAndMakeVisible(packageNote_);
    setupDeliveryPreviewControls(); // CE-DEL-3 / CE-DEL-1
    statusLabel_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
    addAndMakeVisible(statusLabel_);

    addAndMakeVisible(results_);

    // The service's platform codec table (codec_artifacts.PLATFORM_CODECS),
    // and its default selection.
    codecPlatforms_ = {{"spotify", "Spotify (AAC 256)"},
                       {"apple", "Apple (AAC 256)"},
                       {"youtube", "YouTube (Opus 128)"},
                       {"mp3_320", "MP3 320"},
                       {"mp3_128", "MP3 128"}};
    selectedCodecs_ = {"spotify", "youtube", "mp3_128"};
    codecPickBtn_.setTooltip(TRANS("Choose which platform codecs the codec audit encodes through."));
    codecPickBtn_.onClick = [this] { openCodecPicker(); };

    complianceBtn_.onClick = [this] {
        const juce::String src = sourcePath();
        // A fresh PDF per run instead of overwriting the last "_compliance.pdf"
        // (UIX-21).
        const juce::File pdfFile = juce::File(src).getSiblingFile(
            juce::File(src).getFileNameWithoutExtension() + "_compliance_"
            + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S") + ".pdf");
        const juce::String pdf = pdfFile.exists() ? pdfFile.getNonexistentSibling(false).getFullPathName()
                                                  : pdfFile.getFullPathName();
        runResult(TRANS("Delivery check"), [src, pdf, url = bridgeUrl()]() -> DeliveryResult {
            ipc::HttpServiceBridge bridge(url);
            auto res = bridge.complianceSheet(src, pdf);
            if (res.is_err()) return DeliveryResult::message(serviceErrorLine(res.error()));
            return complianceResult(res.value());
        });
    };
    codecBtn_.onClick = [this] {
        const juce::String src = sourcePath();
        // The user's codec pick (Codecs...), not always the service default.
        runResult(TRANS("Codec audit"), [src, codecs = selectedCodecs_, url = bridgeUrl()]() -> DeliveryResult {
            ipc::HttpServiceBridge bridge(url, 300000); // up to 5 ffmpeg round trips
            auto res = bridge.auditCodecs(src, codecs);
            if (res.is_err()) return DeliveryResult::message(serviceErrorLine(res.error()));
            return codecAuditResult(res.value());
        });
    };
    ditherBtn_.onClick = [this] {
        const juce::String src = sourcePath();
        runResult(TRANS("Dither preview (16-bit)"), [src, url = bridgeUrl()]() -> DeliveryResult {
            ipc::HttpServiceBridge bridge(url);
            auto res = bridge.ditherPreview(src, 16);
            if (res.is_err()) return DeliveryResult::message(serviceErrorLine(res.error()));
            auto v = res.value();
            DeliveryResult r;
            r.title = TRANS("Dither preview (16-bit)");
            r.add(DeliveryResult::Status::Info, TRANS("Noise floor, dither vs truncation"),
                  num(v[af::DitherPreviewResponse::noise_floor_delta_db], 2) + " dB");
            if (auto* arr = v[af::DitherPreviewResponse::modes].getArray())
                for (auto& m : *arr)
                    r.add(DeliveryResult::Status::Info, m[af::DitherMode::label].toString(),
                          num(m[af::DitherMode::noise_floor_dbfs], 1) + " dBFS",
                          TRANS("distortion corr") + " " + num(m[af::DitherMode::distortion_correlation], 3));
            r.notes.add(TRANS("Audition:") + " " + v[af::DitherPreviewResponse::audition_path].toString());
            return r;
        });
    };
    crossfeedBtn_.onClick = [this] {
        const juce::String src = sourcePath();
        runResult(TRANS("Crossfeed A/B"), [src, url = bridgeUrl()]() -> DeliveryResult {
            ipc::HttpServiceBridge bridge(url);
            auto res = bridge.crossfeedAb(src);
            if (res.is_err()) return DeliveryResult::message(serviceErrorLine(res.error()));
            auto v = res.value();
            DeliveryResult r;
            r.title = TRANS("Crossfeed A/B");
            r.add(DeliveryResult::Status::Info, TRANS("Speaker image width vs headphone"),
                  "-" + num(v[af::CrossfeedAbResponse::width_reduction_pct], 1) + "%");
            r.add(DeliveryResult::Status::Info, TRANS("Width: headphone / speaker"),
                  num(v[af::CrossfeedAbResponse::width_headphone], 2) + " / " +
                      num(v[af::CrossfeedAbResponse::width_speaker], 2));
            r.notes.add("A (" + TRANS("in-head") + "): " + v[af::CrossfeedAbResponse::headphone_path].toString());
            r.notes.add("B (" + TRANS("speaker") + "): " + v[af::CrossfeedAbResponse::speaker_path].toString());
            return r;
        });
    };
    referenceBtn_.onClick = [this] {
        // Read the source path here, on the message thread, and capture it
        // by value below - the formatter runs on a background worker
        // (DeliveryQaPanel::run -> controller_.runInBackground), and that
        // worker must never dereference `this` (the panel can be destroyed
        // while it's still in flight during shutdown; see
        // frontend-review-2026-07-17.md P2).
        const juce::String src = sourcePath();
        const double amount = fitAmountSlider_.getValue() / 100.0; // FEAT-3
        chooseSecondFileThen("Reference chain", [src, amount, url = bridgeUrl()](const juce::String& ref) -> juce::String {
            ipc::HttpServiceBridge bridge(url);
            auto res = bridge.referenceChain(src, ref, amount);
            if (res.is_err()) return serviceErrorLine(res.error());
            auto v = res.value();
            juce::String out = TRANS("Corrective chain toward reference") + " (" +
                               juce::String(juce::roundToInt(amount * 100.0)) + "%):\n";
            if (auto* arr = v[af::ReferenceChainResponse::rationale].getArray())
                for (auto& r : *arr)
                    out << "  - " << r.toString() << "\n";
            // FEAT-3: the tone difference per band, now and as predicted.
            const auto curve = v[af::ReferenceChainResponse::predicted_curve];
            const auto* hz = curve[af::ReferenceChainCurve::band_centers_hz].getArray();
            const auto* before = curve[af::ReferenceChainCurve::delta_before_db].getArray();
            const auto* after = curve[af::ReferenceChainCurve::delta_after_db].getArray();
            if (hz != nullptr && before != nullptr && after != nullptr && hz->size() == before->size() &&
                hz->size() == after->size()) {
                out << "\n" << TRANS("Difference to the reference per band (now -> predicted):") << "\n";
                for (int i = 0; i < hz->size(); ++i)
                    out << "  " << num((*hz)[i], 0) << " Hz: " << num((*before)[i], 1) << " -> "
                        << num((*after)[i], 1) << " dB\n";
            }
            out << "Apply via the copilot / Fix This path (runs the render safety validators).\n";
            return out;
        });
    };
    fitAmountLabel_.setText(TRANS("Amount"), juce::dontSendNotification);
    fitAmountLabel_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
    addAndMakeVisible(fitAmountLabel_);
    fitAmountSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    fitAmountSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 20);
    fitAmountSlider_.setRange(0.0, 100.0, 1.0);
    fitAmountSlider_.setValue(100.0, juce::dontSendNotification);
    fitAmountSlider_.setTextValueSuffix("%");
    fitAmountSlider_.setTitle(TRANS("Fit amount"));
    fitAmountSlider_.setTooltip(TRANS("How far the fitted chain moves the master toward the reference (0-100%)."));
    addAndMakeVisible(fitAmountSlider_);
    fitChainBtn_.setTooltip(TRANS("Fit low/high shelves, up to 4 EQ bands and a compressor that move this master "
                                  "toward a reference, render it and compare predicted vs measured error."));
    fitPreviewToggle_.setTooltip(TRANS("Fit only: show the predicted result per band without rendering"));
    addAndMakeVisible(fitPreviewToggle_);
    fitChainBtn_.onClick = [this] {
        const juce::String src = sourcePath();   // read on the message thread (see referenceBtn_)
        const double amount = fitAmountSlider_.getValue() / 100.0;
        const bool render = !fitPreviewToggle_.getToggleState(); // FEAT-3 preview
        const juce::String out = src.isEmpty() || !render
                                     ? juce::String()
                                     : uniqueRenderFile(juce::File(src), "_fitchain").getFullPathName();
        chooseSecondFileThen("Fit chain", [src, amount, render, out, url = bridgeUrl()](const juce::String& ref) -> juce::String {
            auto res = ipc::HttpServiceBridge(url).fitChain(src, ref, {}, amount, render, out);
            if (res.is_err()) return serviceErrorLine(res.error());
            return formatFitChain(res.value(), amount, TRANS("Fitted chain toward the reference"));
        });
    };
    // FEAT-3: the same fit toward the session genre's targets.
    fitGenreBtn_.setTooltip(TRANS("Fit the same chain toward the session genre's tonal and dynamic targets"));
    addAndMakeVisible(fitGenreBtn_);
    fitGenreBtn_.onClick = [this] {
        const juce::String src = sourcePath();
        const auto* session = controller_.getActiveSession();
        const juce::String genre = session != nullptr ? juce::String(session->target.primary_genre) : juce::String();
        if (genre.isEmpty()) {
            showResult(TRANS("Choose the session's genre on Intake first."));
            return;
        }
        const double amount = fitAmountSlider_.getValue() / 100.0;
        const bool render = !fitPreviewToggle_.getToggleState();
        const juce::String out = src.isEmpty() || !render
                                     ? juce::String()
                                     : uniqueRenderFile(juce::File(src), "_fitgenre").getFullPathName();
        run(TRANS("Fit to genre"), [src, genre, amount, render, out, url = bridgeUrl()]() -> juce::String {
            auto res = ipc::HttpServiceBridge(url).fitChain(src, {}, genre, amount, render, out);
            if (res.is_err()) return serviceErrorLine(res.error());
            return formatFitChain(res.value(), amount, TRANS("Fitted chain toward the genre") + " " + genre);
        });
    };

    critiqueBtn_.onClick = [this] {
        const juce::String src = sourcePath(); // see referenceBtn_ above
        chooseSecondFileThenResult("A/B critique", [src, url = bridgeUrl()](const juce::String& other) -> DeliveryResult {
            ipc::HttpServiceBridge bridge(url);
            auto res = bridge.abCritique(src, other);
            if (res.is_err()) return DeliveryResult::message(serviceErrorLine(res.error()));
            auto v = res.value();
            DeliveryResult r;
            r.title = TRANS("A/B critique") + ": " + v[af::AbCritiqueResponse::overall].toString();
            if (auto* arr = v[af::AbCritiqueResponse::comparisons].getArray())
                for (auto& cmp : *arr)
                    r.add(DeliveryResult::Status::Info, cmp[af::AbComparison::dimension].toString(),
                          num(cmp[af::AbComparison::a_value], 2) + " / " + num(cmp[af::AbComparison::b_value], 2),
                          TRANS("winner") + " " + cmp[af::AbComparison::winner].toString());
            r.notes.add(v[af::AbCritiqueResponse::verdict].toString());
            return r;
        });
    };

    showResult(TRANS("Load a session, then run a delivery check on the current master."));
}

DeliveryQaPanel::~DeliveryQaPanel() = default;

juce::String DeliveryQaPanel::sourcePath() const
{
    if (!controller_.hasActiveSession() || controller_.getActiveSession()->files.empty()) return {};
    // "The current master" = the file Export writes: the accepted render
    // when there is one (UIX-06), not always the original mix.
    return session_audio::exportSourcePath(controller_);
}

std::string DeliveryQaPanel::bridgeUrl() const
{
    // WIRE-13: every call used to default-construct the bridge, so the
    // injected base URL (the test seam) was dead.
    return baseUrl_.isNotEmpty() ? baseUrl_.toStdString() : std::string(ipc::HttpServiceBridge::kDefaultBaseUrl);
}

void DeliveryQaPanel::applyLivePlatforms(std::vector<std::pair<juce::String, juce::String>>& platforms,
                                         juce::StringArray& selected,
                                         const std::vector<ipc::HttpServiceBridge::CodecPlatformInfo>& live)
{
    if (live.empty()) return;
    platforms.clear();
    for (const auto& p : live)
        platforms.emplace_back(p.id, p.name);
    juce::StringArray kept;
    for (const auto& id : selected)
        for (const auto& p : platforms)
            if (p.first == id) {
                kept.addIfNotAlreadyThere(id);
                break;
            }
    if (kept.isEmpty()) kept.add(platforms.front().first);
    selected = kept;
}

void DeliveryQaPanel::openCodecPicker()
{
    if (codecPlatformsLive_) {
        showCodecPicker();
        return;
    }
    if (codecFetchInFlight_) return;
    codecFetchInFlight_ = true;
    codecPickBtn_.setEnabled(false);
    juce::Component::SafePointer<DeliveryQaPanel> self(this);
    controller_.runInBackground([self, url = bridgeUrl()]() {
        auto result = ipc::HttpServiceBridge(url).getCodecPlatforms();
        juce::MessageManager::callAsync([self, result]() {
            if (self == nullptr) return;
            self->codecFetchInFlight_ = false;
            self->codecPickBtn_.setEnabled(!self->busy_);
            // Not session-scoped data: the platform table is the service's.
            if (result.is_ok()) {
                applyLivePlatforms(self->codecPlatforms_, self->selectedCodecs_, result.value());
                self->codecPlatformsLive_ = true;
            }
            // Offline (or an older service): the seeded table still works, and
            // the next click tries the live list again.
            self->showCodecPicker();
        });
    });
}

void DeliveryQaPanel::showCodecPicker()
{
    juce::PopupMenu menu;
    menu.addSectionHeader(TRANS("Codec audit platforms"));
    int id = 1;
    for (const auto& [codecId, name] : codecPlatforms_)
        menu.addItem(id++, name, true, selectedCodecs_.contains(codecId));
    menu.addSeparator();
    menu.addItem(1000, TRANS("Done"));
    juce::Component::SafePointer<DeliveryQaPanel> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&codecPickBtn_), [self](int result) {
        if (self == nullptr || result <= 0 || result == 1000) return;
        const auto idx = static_cast<size_t>(result - 1);
        if (idx >= self->codecPlatforms_.size()) return;
        const juce::String codecId = self->codecPlatforms_[idx].first;
        if (self->selectedCodecs_.contains(codecId)) {
            if (self->selectedCodecs_.size() > 1) self->selectedCodecs_.removeString(codecId); // keep at least one
        } else {
            self->selectedCodecs_.add(codecId);
        }
        self->showCodecPicker(); // stay open for the next toggle
    });
}

void DeliveryQaPanel::setBusy(bool busy, const juce::String& what)
{
    busy_ = busy;
    for (auto* b : {&complianceBtn_, &codecBtn_, &ditherBtn_, &crossfeedBtn_, &referenceBtn_, &critiqueBtn_,
                    &codecPickBtn_, &fitChainBtn_, &streamBtn_, &streamOptsBtn_, &immersiveBtn_})
        b->setEnabled(!busy);
    statusLabel_.setText(busy ? (what + "...") : juce::String(), juce::dontSendNotification);
}

void DeliveryQaPanel::showResult(const juce::String& text) { results_.setResult(DeliveryResult::message(text)); }

void DeliveryQaPanel::showResult(DeliveryResult result) { results_.setResult(std::move(result)); }

DeliveryResult DeliveryQaPanel::complianceResult(const juce::var& v)
{
    DeliveryResult r;
    r.title = TRANS("Delivery check");
    if (auto* checks = v[af::ComplianceSheetResponse::checks].getDynamicObject())
        for (auto& p : checks->getProperties()) {
            const auto status = DeliveryResult::statusFrom(p.value[af::ComplianceCheck::status].toString());
            const juce::String name = p.name.toString().replace("_", " ");
            r.add(status, name.substring(0, 1).toUpperCase() + name.substring(1),
                  p.value[af::ComplianceCheck::value].toString());
            const auto note = p.value[af::ComplianceCheck::note].toString();
            if (status != DeliveryResult::Status::Pass && note.isNotEmpty())
                r.notes.add(name.substring(0, 1).toUpperCase() + name.substring(1) + ": " + note);
        }
    if (r.rows.empty())
        r.add(DeliveryResult::statusFrom(
                  v[af::ComplianceSheetResponse::summary][af::ComplianceSummary::overall].toString()),
              TRANS("Overall"));
    if (v[af::ComplianceSheetResponse::pdf_path].toString().isNotEmpty())
        r.notes.add(TRANS("PDF:") + " " + v[af::ComplianceSheetResponse::pdf_path].toString());
    return r;
}

DeliveryResult DeliveryQaPanel::codecAuditResult(const juce::var& v)
{
    if (!(bool)v[af::CodecAuditResponse::encoder_available])
        return DeliveryResult::message(TRANS("ffmpeg not found on PATH - install ffmpeg to run the codec audit."));
    DeliveryResult r;
    r.title = TRANS("Codec audit");
    if (auto* arr = v[af::CodecAuditResponse::results].getArray())
        for (auto& e : *arr) {
            if (e[af::CodecAuditResult::error].isString() && e[af::CodecAuditResult::error].toString().isNotEmpty()) {
                r.add(DeliveryResult::Status::Fail, e[af::CodecAuditResult::name].toString(),
                      e[af::CodecAuditResult::error].toString());
                continue;
            }
            r.add(DeliveryResult::statusFrom(e[af::CodecAuditResult::status].toString()),
                  e[af::CodecAuditResult::name].toString(),
                  TRANS("HF loss") + " " + num(e[af::CodecAuditResult::hf_loss_db], 1) + " dB",
                  TRANS("TP change") + " " + num(e[af::CodecAuditResult::true_peak_delta_dbtp], 2) + " dB");
        }
    return r;
}

void DeliveryQaPanel::runResult(const juce::String& what, std::function<DeliveryResult()> call)
{
    if (busy_) return;
    if (sourcePath().isEmpty()) {
        showResult(TRANS("No active session."));
        return;
    }
    setBusy(true, what);
    juce::Component::SafePointer<DeliveryQaPanel> self(this);
    controller_.runInBackground([self, &work = controller_, call = std::move(call)]() {
        DeliveryResult result = call ? call() : DeliveryResult{};
        postIfCurrent(self, work, [result](DeliveryQaPanel& panel) {
            panel.showResult(result);
        }, [](DeliveryQaPanel& panel) { panel.setBusy(false); });
    });
}

void DeliveryQaPanel::chooseSecondFileThenResult(const juce::String& what,
                                                 std::function<DeliveryResult(const juce::String&)> call)
{
    // The plain-text path chooses the file; the result is kept as rows.
    auto holder = std::make_shared<DeliveryResult>();
    pendingRows_ = holder;
    chooseSecondFileThen(what, [call, holder](const juce::String& second) -> juce::String {
        *holder = call(second);
        return holder->toText();
    });
}

void DeliveryQaPanel::run(const juce::String& what, std::function<juce::String()> call)
{
    if (busy_) return;
    if (sourcePath().isEmpty()) {
        showResult(TRANS("No active session."));
        return;
    }
    setBusy(true, what);
    juce::Component::SafePointer<DeliveryQaPanel> self(this);
    controller_.runInBackground([self, &work = controller_, call = std::move(call)]() {
        const juce::String text = call ? call() : juce::String();
        postIfCurrent(self, work, [text](DeliveryQaPanel& panel) {
            if (auto rows = std::exchange(panel.pendingRows_, nullptr); rows != nullptr && rows->toText() == text)
                panel.showResult(*rows); // UXR-1: a rows result routed through the file chooser
            else
                panel.showResult(text);
        }, [](DeliveryQaPanel& panel) { panel.setBusy(false); });
    });
}

void DeliveryQaPanel::chooseSecondFileThen(const juce::String& what,
                                           std::function<juce::String(const juce::String&)> call)
{
    if (busy_) return;
    if (sourcePath().isEmpty()) {
        showResult(TRANS("No active session."));
        return;
    }
    // WF-6: a reference-based check uses the session's reference track when
    // it has one (set on the References screen or Compare card) instead of
    // asking for a file again.
    if (what == "Reference chain" || what == "Fit chain") {
        const juce::String sessionRef = controller_.analysis().getReferenceTrack();
        if (sessionRef.isNotEmpty() && juce::File(sessionRef).existsAsFile()) {
            run(juce::translate(what), [call, sessionRef]() { return call(sessionRef); });
            return;
        }
    }
    chooser_ = std::make_unique<juce::FileChooser>(
        (what == "Reference chain" || what == "Fit chain") ? TRANS("Select the reference")
        : what == "Immersive check" ? TRANS("Select a multichannel / ADM BWF file")
                                    : TRANS("Select the other take"),
        juce::File::getSpecialLocation(juce::File::userMusicDirectory), "*.wav;*.flac;*.aif;*.aiff;*.mp3;*.ogg");
    juce::Component::SafePointer<DeliveryQaPanel> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [self, what, call = std::move(call)](const juce::FileChooser& fc) {
                              if (self == nullptr) return;
                              const auto file = fc.getResult();
                              if (!file.existsAsFile()) return;
                              const juce::String second = file.getFullPathName();
                              self->run(juce::translate(what), [call, second]() { return call(second); });
                          });
}

void DeliveryQaPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    const int gap = KhensuTheme::sp1 + 2;
    const int rowH = KhensuTheme::buttonHSmall;
    auto buttons = [gap](juce::Rectangle<int> row, std::initializer_list<juce::Component*> items) {
        const int n = static_cast<int>(items.size());
        const int w = (row.getWidth() - (n - 1) * gap) / juce::jmax(1, n);
        for (auto* item : items) {
            item->setBounds(row.removeFromLeft(w));
            row.removeFromLeft(gap);
        }
    };
    // Check: the primary first, then the other checks.
    checkHeading_.setBounds(b.removeFromTop(18));
    auto row = b.removeFromTop(KhensuTheme::buttonH);
    complianceBtn_.setBounds(row.removeFromLeft(juce::jmin(190, row.getWidth() / 3)));
    row.removeFromLeft(gap);
    buttons(row.withSizeKeepingCentre(row.getWidth(), rowH), {&codecBtn_, &immersiveBtn_, &critiqueBtn_});
    b.removeFromTop(KhensuTheme::sp2);
    // Preview.
    previewHeading_.setBounds(b.removeFromTop(18));
    buttons(b.removeFromTop(rowH), {&ditherBtn_, &crossfeedBtn_, &streamBtn_, &streamOptsBtn_, &codecPickBtn_});
    b.removeFromTop(KhensuTheme::sp2);
    // Fix.
    fixHeading_.setBounds(b.removeFromTop(18));
    buttons(b.removeFromTop(rowH), {&referenceBtn_, &fitChainBtn_, &fitGenreBtn_});
    b.removeFromTop(gap);
    auto amount = b.removeFromTop(rowH);
    fitPreviewToggle_.setBounds(amount.removeFromRight(juce::jmin(120, amount.getWidth() / 4)));
    fitAmountLabel_.setBounds(amount.removeFromLeft(56));
    fitAmountSlider_.setBounds(amount);
    b.removeFromTop(KhensuTheme::sp1);
    packageNote_.setBounds(b.removeFromTop(16));
    b.removeFromTop(KhensuTheme::sp1);
    statusLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(2);
    results_.setBounds(b);
}

} // namespace khensu
