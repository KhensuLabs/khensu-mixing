// desktop-app/Source/ui/ExportDelivery/DeliveryPackagePanel.cpp
#include "DeliveryPackagePanel.h"
#include "../shared/PanelRunState.h"
#include <khensu/session/Session.h>
#include "../shared/JobPoller.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionAudioPaths.h"
#include "../shared/SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace af = khensu::api::fields;

namespace khensu {

namespace {
constexpr int kPollIntervalMs = 1000;

// Distributor profile ids (delivery_targets.DISTRIBUTOR_PROFILES) - service
// data, never translated; the names are display only.
struct Profile {
    const char* id;
    const char* name;
};
constexpr Profile kProfiles[] = {
    {"streaming_distributor", NEEDS_TRANS("Streaming distributor (16/24-bit, 44.1 kHz+)")},
    {"apple_digital_masters", NEEDS_TRANS("Apple Digital Masters (24-bit)")},
    {"cd_replication", NEEDS_TRANS("CD replication (16-bit / 44.1 kHz + DDP)")},
    {"bandcamp", NEEDS_TRANS("Bandcamp / direct download")},
};
constexpr Profile kAiUse[] = {
    {"none", NEEDS_TRANS("No AI-generated material")},
    {"ai_assisted", NEEDS_TRANS("Contains AI-generated parts")},
    {"ai_generated", NEEDS_TRANS("AI-generated audio")},
};

juce::String n2(const juce::var& v, int dp = 2)
{
    return (v.isDouble() || v.isInt() || v.isInt64()) ? juce::String((double)v, dp) : TRANS("n/a");
}
} // namespace

void DeliveryPackagePanel::applyThemeColours()
{
    buildButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal);
    buildButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::bgVoid);
    cancelButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentRed.withAlpha(0.35f));
    for (auto* b : {&cleanButton_, &admButton_, &signButton_, &readButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    resultsLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

DeliveryPackagePanel::DeliveryPackagePanel(PanelCardController& c, const juce::String& baseUrl)
    : PanelCard(c, "Delivery Package"), baseUrl_(baseUrl)
{
    applyThemeColours();
    setExplainer(TRANS("Delivery Package"),
                 TRANS("One click builds a folder a distributor or plant can take as-is: the master at the "
                       "profile's sample rate and bit depth (dithered), a DDP fileset for CD, the separated "
                       "stems and an instrumental, an optional clean version and ADM file, and a manifest with "
                       "a SHA-256 hash and spec checks for every file. With a signing certificate set in "
                       "Settings, the master carries C2PA content credentials; an unsigned provenance "
                       "sidecar is always written."));

    for (size_t i = 0; i < std::size(kProfiles); ++i)
        profileCombo_.addItem(juce::translate(kProfiles[i].name), (int)i + 1);
    profileCombo_.setSelectedId(1, juce::dontSendNotification);
    profileCombo_.setTitle(TRANS("Distributor profile"));
    profileCombo_.setTooltip(TRANS("The delivery route the files are converted for and checked against."));
    addAndMakeVisible(profileCombo_);

    for (size_t i = 0; i < std::size(kAiUse); ++i)
        aiUseCombo_.addItem(juce::translate(kAiUse[i].name), (int)i + 1);
    aiUseCombo_.setSelectedId(1, juce::dontSendNotification);
    aiUseCombo_.setTitle(TRANS("AI disclosure"));
    aiUseCombo_.setTooltip(TRANS("Recorded in the content credentials as the IPTC digital source type."));
    addAndMakeVisible(aiUseCombo_);

    buildButton_.onClick = [this] { build(); };
    addAndMakeVisible(buildButton_);
    cancelButton_.onClick = [this] { cancelBuild(); };
    cancelButton_.setVisible(false);
    addChildComponent(cancelButton_);
    cleanButton_.setTooltip(TRANS("Optional clean / radio version to include in the package."));
    cleanButton_.onClick = [this] { chooseOptionalFile(false); };
    addAndMakeVisible(cleanButton_);
    admButton_.setTooltip(TRANS("Optional ADM BWF (Dolby Atmos) master to pass through and check."));
    admButton_.onClick = [this] { chooseOptionalFile(true); };
    addAndMakeVisible(admButton_);
    signButton_.setTooltip(TRANS("Write C2PA content credentials for the current master (a signed copy when a "
                                 "certificate is set in Settings) plus the provenance sidecar."));
    signButton_.onClick = [this] { runCredentials(true); };
    addAndMakeVisible(signButton_);
    readButton_.setTooltip(TRANS("Show the content credentials embedded in, or written beside, the current master."));
    readButton_.onClick = [this] { runCredentials(false); };
    addAndMakeVisible(readButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    addAndMakeVisible(statusLabel_);
    resultsLabel_.setFont(KhensuTheme::labelFont(9.5f));
    resultsLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(resultsLabel_);
    refreshReadiness();
}

DeliveryPackagePanel::~DeliveryPackagePanel()
{
    if (pollStop_) pollStop_->store(true); // the poller cancels the job server-side
}

std::string DeliveryPackagePanel::bridgeUrl() const
{
    return baseUrl_.isNotEmpty() ? baseUrl_.toStdString() : std::string(ipc::HttpServiceBridge::kDefaultBaseUrl);
}

juce::String DeliveryPackagePanel::selectedProfileId() const
{
    const int idx = profileCombo_.getSelectedId() - 1;
    return idx >= 0 && idx < (int)std::size(kProfiles) ? juce::String(kProfiles[idx].id) : juce::String("streaming_distributor");
}

juce::String DeliveryPackagePanel::selectedAiUse() const
{
    const int idx = aiUseCombo_.getSelectedId() - 1;
    return idx >= 0 && idx < (int)std::size(kAiUse) ? juce::String(kAiUse[idx].id) : juce::String("none");
}

void DeliveryPackagePanel::visibilityChanged()
{
    if (isVisible() && currentJobId_.isEmpty() && !startInFlight_) refreshReadiness();
}

void DeliveryPackagePanel::resetForSession()
{
    cleanPath_ = {};
    admPath_ = {};
    cleanButton_.setButtonText(TRANS("Clean version..."));
    admButton_.setButtonText(TRANS("ADM file..."));
    resultsLabel_.setText({}, juce::dontSendNotification);
    refreshReadiness();
}

void DeliveryPackagePanel::refreshIfIdle()
{
    if (currentJobId_.isEmpty() && !startInFlight_) refreshReadiness();
}

void DeliveryPackagePanel::refreshReadiness()
{
    const bool ready = controller_.hasActiveSession() && !controller_.getActiveSession()->files.empty();
    const bool idle = currentJobId_.isEmpty() && !startInFlight_;
    buildButton_.setEnabled(ready && idle);
    signButton_.setEnabled(ready && !credentialsBusy_);
    readButton_.setEnabled(ready && !credentialsBusy_);
    if (idle)
        statusLabel_.setText(ready ? TRANS("Builds the package from the current master.") : TRANS("Load a session first."),
                             juce::dontSendNotification);
}

void DeliveryPackagePanel::chooseOptionalFile(bool adm)
{
    juce::String& slot = adm ? admPath_ : cleanPath_;
    if (slot.isNotEmpty()) {
        // A second click clears the choice.
        slot = {};
        (adm ? admButton_ : cleanButton_).setButtonText(adm ? TRANS("ADM file...") : TRANS("Clean version..."));
        return;
    }
    chooser_ = std::make_unique<juce::FileChooser>(adm ? TRANS("Select the ADM BWF master")
                                                       : TRANS("Select the clean version"),
                                                   juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                   adm ? "*.wav" : "*.wav;*.flac;*.aif;*.aiff");
    juce::Component::SafePointer<DeliveryPackagePanel> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [self, adm](const juce::FileChooser& fc) {
                              if (self == nullptr) return;
                              const auto file = fc.getResult();
                              if (!file.existsAsFile()) return;
                              (adm ? self->admPath_ : self->cleanPath_) = file.getFullPathName();
                              (adm ? self->admButton_ : self->cleanButton_)
                                  .setButtonText(file.getFileName() + "  (x)");
                          });
}

void DeliveryPackagePanel::build()
{
    if (!currentJobId_.isEmpty() || startInFlight_) return;
    if (!controller_.hasActiveSession() || controller_.getActiveSession()->files.empty()) {
        refreshReadiness();
        return;
    }
    ipc::HttpServiceBridge::DeliveryPackageOptions opts;
    opts.filePath = session_audio::exportSourcePath(controller_); // the accepted render, else the original
    opts.profileId = selectedProfileId();
    opts.aiUse = selectedAiUse();
    opts.cleanVersionPath = cleanPath_;
    opts.admPath = admPath_;
    const auto vocals = controller_.getStemFile("vocals");
    if (vocals.existsAsFile()) opts.stemsDir = vocals.getParentDirectory().getFullPathName();

    buildButton_.setEnabled(false);
    cancelButton_.setVisible(true);
    resized();
    statusLabel_.setText(TRANS("Starting..."), juce::dontSendNotification);
    resultsLabel_.setText({}, juce::dontSendNotification);
    startInFlight_ = true;
    cancelRequestedWhileStarting_ = false;

    juce::Component::SafePointer<DeliveryPackagePanel> self(this);
    controller_.runLongInBackground([self, opts, url = bridgeUrl()]() {
        auto result = ipc::HttpServiceBridge(url).startDeliveryPackageJob(opts);
        juce::MessageManager::callAsync([self, result = std::move(result)]() mutable {
            if (self == nullptr) return;
            self->startInFlight_ = false;
            // Not session-guarded: the job exists either way (see PlatformMastersPanel).
            if (self->cancelRequestedWhileStarting_) {
                self->cancelRequestedWhileStarting_ = false;
                if (result.is_ok()) {
                    const auto jobId = result.value();
                    self->controller_.runInBackground([jobId, url = self->bridgeUrl()]() {
                        ipc::HttpServiceBridge(url).cancelDeliveryPackageJob(jobId);
                    });
                }
                return;
            }
            if (result.is_err()) {
                self->cancelButton_.setVisible(false);
                self->resized();
                self->refreshReadiness();
                self->statusLabel_.setText(serviceErrorLine(result.error()), juce::dontSendNotification);
                return;
            }
            self->currentJobId_ = result.value();
            self->pollJob(self->currentJobId_);
        });
    });
}

void DeliveryPackagePanel::pollJob(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<DeliveryPackagePanel> self(this);
    controller_.runLongInBackground([self, jobId, stop, url = bridgeUrl()]() {
        ipc::HttpServiceBridge bridge(url);
        using Poller = JobPoller<ipc::HttpServiceBridge::DeliveryPackageJobStatus>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getDeliveryPackageJobStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelDeliveryPackageJob(juce::String(id)); };
        hooks.isTerminal = [](const auto& st) { return !khensu::api::isActiveStatus(st.status); };
        hooks.onProgress = [self, jobId](const auto& st) {
            juce::String t = TRANS("Building... step %d / %t").replace("%d", juce::String(st.done + 1))
                                 .replace("%t", juce::String(st.total));
            if (st.current.isNotEmpty()) t += "  (" + st.current + ")";
            juce::MessageManager::callAsync([self, jobId, t]() {
                if (self != nullptr && jobId == self->currentJobId_)
                    self->statusLabel_.setText(t, juce::dontSendNotification);
            });
        };
        hooks.shouldStop = [stop] { return stop->load(); };
        Poller::Options opts;
        opts.pollIntervalMs = kPollIntervalMs;
        opts.maxConsecutiveErrors = 6;
        opts.deadlineSeconds = 60 * 60;
        const auto out = Poller::run(hooks, opts, jobId.toStdString());
        juce::MessageManager::callAsync([self, jobId, out]() {
            if (self == nullptr || jobId != self->currentJobId_) return;
            self->currentJobId_ = {};
            self->cancelButton_.setVisible(false);
            self->resized();
            if (!out.finished() || !out.status.has_value()) {
                self->refreshReadiness();
                self->statusLabel_.setText(out.describe(), juce::dontSendNotification);
                return;
            }
            self->finishBuild(out.status->status, out.status->error, out.status->result);
        });
    });
}

void DeliveryPackagePanel::finishBuild(const juce::String& status, const juce::String& error, const juce::var& result)
{
    refreshReadiness();
    if (status == khensu::api::job_status::cancelled) {
        statusLabel_.setText(TRANS("Cancelled - the partial package was removed."), juce::dontSendNotification);
        return;
    }
    if (status != khensu::api::job_status::completed || !result.isObject()) {
        statusLabel_.setText(TRANS("Package failed:") + " " + error, juce::dontSendNotification);
        return;
    }
    const auto overall = result[af::DeliveryPackageResult::overall].toString();
    statusLabel_.setText(TRANS("Package built -> %d").replace("%d", result[af::DeliveryPackageResult::output_dir].toString()),
                         juce::dontSendNotification);
    resultsLabel_.setColour(juce::Label::textColourId, overall == "fail"   ? KhensuTheme::accentRed
                                                       : overall == "warn" ? KhensuTheme::accentAmber
                                                                           : KhensuTheme::accentTeal);
    resultsLabel_.setText(formatResult(result), juce::dontSendNotification);
}

juce::String DeliveryPackagePanel::formatResult(const juce::var& r)
{
    namespace R = af::DeliveryPackageResult;
    juce::String out;
    out << TRANS("Overall") << ": " << r[R::overall].toString().toUpperCase() << "   (" << r[R::sample_rate].toString()
        << " Hz / " << r[R::bit_depth].toString() << "-bit)\n";
    const auto c2pa = r[R::c2pa];
    if ((bool)c2pa[af::DeliveryPackageC2pa::signed_])
        out << TRANS("Content credentials: signed") << "\n";
    else
        out << TRANS("Content credentials: unsigned sidecar only") << " - "
            << c2pa[af::DeliveryPackageC2pa::reason].toString() << "\n";
    if (auto* files = r[R::files].getArray()) {
        int ddp = 0;
        for (const auto& f : *files) {
            const auto role = f[af::DeliveryPackageFile::role].toString();
            if (role == "ddp") {
                ++ddp;
                continue;
            }
            if (role == "provenance_sidecar") continue;
            out << "  " << f[af::DeliveryPackageFile::path].toString() << "  [" << role << "]";
            if (f[af::DeliveryPackageFile::overall].isString())
                out << "  " << f[af::DeliveryPackageFile::overall].toString().toUpperCase();
            if (f[af::DeliveryPackageFile::true_peak_dbtp].isDouble())
                out << "  " << n2(f[af::DeliveryPackageFile::true_peak_dbtp]) << " dBTP";
            out << "\n";
        }
        if (ddp > 0) out << "  " << TRANS("DDP fileset") << ": " << juce::String(ddp) << " " << TRANS("files") << "\n";
    }
    if (auto* w = r[R::warnings].getArray())
        for (const auto& line : *w)
            out << "(!) " << line.toString() << "\n";
    return out;
}

void DeliveryPackagePanel::runCredentials(bool sign)
{
    if (credentialsBusy_) return;
    if (!controller_.hasActiveSession() || controller_.getActiveSession()->files.empty()) return;
    const juce::String src = session_audio::exportSourcePath(controller_);
    const juce::String aiUse = selectedAiUse();
    credentialsBusy_ = true;
    refreshReadiness();
    statusLabel_.setText(sign ? TRANS("Writing content credentials...") : TRANS("Reading content credentials..."),
                         juce::dontSendNotification);
    juce::Component::SafePointer<DeliveryPackagePanel> self(this);
    controller_.runInBackground([self, &work = controller_, src, aiUse, sign, url = bridgeUrl()]() {
        ipc::HttpServiceBridge bridge(url);
        auto res = sign ? bridge.provenanceSign(src, "edited", aiUse) : bridge.provenanceRead(src);
        postIfCurrent(self, work, [res, sign](DeliveryPackagePanel& panel) {
            if (res.is_err()) {
                panel.statusLabel_.setText(serviceErrorLine(res.error()), juce::dontSendNotification);
                return;
            }
            panel.statusLabel_.setText(sign ? TRANS("Content credentials written.") : TRANS("Content credentials:"),
                                       juce::dontSendNotification);
            panel.resultsLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
            panel.resultsLabel_.setText(sign ? formatSign(res.value()) : formatRead(res.value()),
                                        juce::dontSendNotification);
        }, [](DeliveryPackagePanel& panel) {
            panel.credentialsBusy_ = false;
            panel.refreshReadiness();
        });
    });
}

juce::String DeliveryPackagePanel::formatSign(const juce::var& v)
{
    namespace S = af::ProvenanceSignResponse;
    juce::String out;
    if ((bool)v[S::signed_])
        out << TRANS("Signed copy") << " -> " << v[S::output_path].toString() << "  (" << v[S::alg].toString() << ", "
            << v[S::validation_state].toString() << ")\n";
    else
        out << TRANS("Not signed") << ": " << v[S::reason].toString() << "\n";
    out << TRANS("Sidecar") << " -> " << v[S::sidecar_path].toString() << "\n";
    out << "SHA-256 " << v[S::sha256].toString() << "\n";
    if (v[S::digital_source_type].isString())
        out << TRANS("AI disclosure") << ": " << v[S::digital_source_type].toString() << "\n";
    return out;
}

juce::String DeliveryPackagePanel::formatRead(const juce::var& v)
{
    namespace R = af::ProvenanceReadResponse;
    juce::String out;
    if ((bool)v[R::embedded]) {
        const auto m = v[R::manifest];
        out << TRANS("Embedded C2PA manifest") << ": " << m[af::ProvenanceManifest::claim_generator].toString() << ", "
            << m[af::ProvenanceManifest::validation_state].toString()
            << ((bool)m[af::ProvenanceManifest::trusted] ? juce::String() : " (" + TRANS("untrusted signer") + ")")
            << "\n";
        if (auto* acts = m[af::ProvenanceManifest::actions].getArray())
            for (const auto& a : *acts) {
                out << "  " << a[af::ProvenanceAction::action].toString();
                if (a[af::ProvenanceAction::digital_source_type].isString())
                    out << "  " << a[af::ProvenanceAction::digital_source_type].toString().fromLastOccurrenceOf("/", false, false);
                out << "\n";
            }
    } else if (!(bool)v[R::c2pa_available]) {
        out << TRANS("C2PA reading unavailable") << ": " << v[R::reason].toString() << "\n";
    } else {
        out << TRANS("No embedded C2PA manifest.") << "\n";
    }
    const auto side = v[R::sidecar];
    if ((bool)side[af::ProvenanceSidecar::present])
        out << TRANS("Sidecar") << ": " << ((bool)side[af::ProvenanceSidecar::sha256_matches] ? TRANS("matches the file")
                                                                                             : TRANS("does NOT match the file"))
            << "\n";
    else
        out << TRANS("No provenance sidecar.") << "\n";
    return out;
}

void DeliveryPackagePanel::cancelBuild()
{
    if (currentJobId_.isEmpty() && !startInFlight_) return;
    if (pollStop_) pollStop_->store(true); // the poller cancels server-side
    if (startInFlight_) {
        cancelRequestedWhileStarting_ = true;
    } else {
        const juce::String jobId = currentJobId_;
        controller_.runLongInBackground([jobId, url = bridgeUrl()]() {
            ipc::HttpServiceBridge(url).cancelDeliveryPackageJob(jobId);
        });
    }
    currentJobId_ = {};
    cancelButton_.setVisible(false);
    resized();
    refreshReadiness();
    statusLabel_.setText(TRANS("Cancelled."), juce::dontSendNotification);
}

void DeliveryPackagePanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto row1 = b.removeFromTop(24);
    profileCombo_.setBounds(row1.removeFromLeft(juce::jmax(0, row1.getWidth() / 2 - 3)));
    row1.removeFromLeft(6);
    aiUseCombo_.setBounds(row1);
    b.removeFromTop(6);
    auto row2 = b.removeFromTop(24);
    buildButton_.setBounds(row2.removeFromLeft(170));
    if (cancelButton_.isVisible()) {
        row2.removeFromLeft(6);
        cancelButton_.setBounds(row2.removeFromLeft(70));
    }
    row2.removeFromLeft(6);
    const int bw = juce::jmax(0, (row2.getWidth() - 3 * 6) / 4);
    cleanButton_.setBounds(row2.removeFromLeft(bw));
    row2.removeFromLeft(6);
    admButton_.setBounds(row2.removeFromLeft(bw));
    row2.removeFromLeft(6);
    signButton_.setBounds(row2.removeFromLeft(bw));
    row2.removeFromLeft(6);
    readButton_.setBounds(row2);
    b.removeFromTop(4);
    statusLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(2);
    resultsLabel_.setBounds(b);
}

} // namespace khensu
