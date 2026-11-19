#include "ModelDownloadDialog.h"
#include "../../IAppController.h"
#include "ServiceErrorHint.h"
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void ModelDownloadDialog::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    descLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    folderLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    changeFolderButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    actionButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTealDim);
    actionButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    cancelButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    cancelButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textPrimary);
}

ModelDownloadDialog::ModelDownloadDialog(IAppController& c) : controller_(c)
{
    applyThemeColours();
    titleLabel_.setText(TRANS("Demucs AI Separation Model Setup"), juce::dontSendNotification);
    titleLabel_.setFont(KhensuTheme::labelFont(16.0f).boldened());
    addAndMakeVisible(titleLabel_);

    descLabel_.setText(TRANS("High-fidelity vocal isolation requires downloading the Demucs AI model. "
                       "This is critical for analyzing the song as it separates vocals cleanly without "
                       "midrange instrument bleed from guitars, synths, and drums."),
                       juce::dontSendNotification);
    descLabel_.setFont(KhensuTheme::bodyFont(12.0f));
    descLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(descLabel_);

    folderLabel_.setFont(KhensuTheme::monoFont(10.0f));
    addAndMakeVisible(folderLabel_);

    changeFolderButton_.setButtonText(TRANS("Change Folder..."));
    changeFolderButton_.onClick = [this]() { selectCustomFolder(); };
    addAndMakeVisible(changeFolderButton_);

    actionButton_.onClick = [this]() {
        const bool downloading = (statusState_ == "downloading");
        runBridgeAction([downloading](ipc::HttpServiceBridge& bridge) {
            return downloading ? bridge.pauseModelDownload() : bridge.startModelDownload();
        });
    };
    addAndMakeVisible(actionButton_);

    cancelButton_.onClick = [this]() {
        if (statusState_ == "downloading" || statusState_ == "paused") {
            // Fire-and-forget the stop off-thread; the modal closes immediately.
            controller_.runInBackground([] {
                ipc::HttpServiceBridge bridge;
                (void)bridge.stopModelDownload(); // the dialog is closing; nobody to show an error to
            });
        }
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
            dw->exitModalState(0);
        }
    };
    addAndMakeVisible(cancelButton_);

    auto pref = controller_.sessions().sessionManager().getUserPreference("models_dir");
    if (pref.is_ok() && !pref.value().empty()) {
        const juce::String dir(pref.value());
        runBridgeAction([dir](ipc::HttpServiceBridge& bridge) { return bridge.setModelsDir(dir); });
    } else {
        refreshStatus();
    }
    startTimer(300);
}

ModelDownloadDialog::~ModelDownloadDialog() { stopTimer(); }

void ModelDownloadDialog::paint(juce::Graphics& g)
{
    g.fillAll(KhensuTheme::bgSurface);

    g.setColour(KhensuTheme::borderSubtle);
    g.drawRect(getLocalBounds().toFloat(), 1.0f);

    auto r = getLocalBounds().reduced(20);
    auto progressArea = r.removeFromBottom(60).removeFromTop(12);

    g.setColour(KhensuTheme::bgBase);
    g.fillRoundedRectangle(progressArea.toFloat(), 4.0f);

    if (progress_ > 0.0f) {
        auto fillWidth = progressArea.getWidth() * progress_;
        auto fillArea = progressArea.withWidth(static_cast<int>(fillWidth));
        g.setColour(KhensuTheme::accentTeal);
        g.fillRoundedRectangle(fillArea.toFloat(), 4.0f);
    }

    g.setFont(KhensuTheme::bodyFont(11.0f));
    g.setColour(KhensuTheme::textPrimary);
    g.drawFittedText(statusText_, r.removeFromBottom(40), juce::Justification::topLeft, 2);
}

void ModelDownloadDialog::resized()
{
    auto r = getLocalBounds().reduced(20);
    titleLabel_.setBounds(r.removeFromTop(24));
    r.removeFromTop(8);

    descLabel_.setBounds(r.removeFromTop(60));
    r.removeFromTop(12);

    auto folderRow = r.removeFromTop(26);
    changeFolderButton_.setBounds(folderRow.removeFromRight(110));
    folderRow.removeFromRight(8);
    folderLabel_.setBounds(folderRow);

    r.removeFromTop(16);

    auto bottomArea = r.removeFromBottom(40);
    cancelButton_.setBounds(bottomArea.removeFromRight(90));
    bottomArea.removeFromRight(12);
    actionButton_.setBounds(bottomArea.removeFromRight(90));
}

void ModelDownloadDialog::timerCallback() { refreshStatus(); }

void ModelDownloadDialog::runBridgeAction(BridgeAction action)
{
    statusRequestInFlight_ = true;
    // A new user action supersedes the previous one's failure message.
    if (action) actionError_.clear();
    juce::Component::SafePointer<ModelDownloadDialog> self(this);
    controller_.runInBackground([self, action = std::move(action)]() {
        ipc::HttpServiceBridge bridge;
        // Keep the mutation's error: a 403 means downloads are off in
        // Settings, a 422 a rejected folder - neither is "unreachable".
        juce::String actionError;
        if (action) {
            const auto r = action(bridge);
            if (r.is_err()) actionError = serviceErrorLine(r.error());
        }
        auto res = bridge.getModelStatus(); // always re-read after a mutation
        juce::MessageManager::callAsync([self, res, actionError]() {
            if (self == nullptr) return;
            self->statusRequestInFlight_ = false;
            if (actionError.isNotEmpty()) self->actionError_ = actionError;
            if (res.is_ok())
                self->applyStatus(res.value());
            else
                self->folderLabel_.setText(serviceErrorLine(res.error()), juce::dontSendNotification);
        });
    });
}

void ModelDownloadDialog::refreshStatus()
{
    if (statusRequestInFlight_) return; // a poll/mutation is already in flight
    runBridgeAction({});
}

void ModelDownloadDialog::applyStatus(const ipc::HttpServiceBridge::ModelStatus& ms)
{
    {
        statusState_ = ms.status;
        installed_ = ms.installed;
        progress_ = ms.progress;
        folderPath_ = ms.models_dir;

        folderLabel_.setText(TRANS("Folder: ") + folderPath_, juce::dontSendNotification);

        juce::String text;
        if (ms.installed && !ms.demucs_package_available) {
            text = "Model checkpoint downloaded, but the demucs/torch Python packages "
                   "are not installed, so separation will still silently fall back to "
                   "HPSS. Install the stems profile: "
                   "packaging/install-optional-stack.ps1 -Stack stems.";
            statusText_ = text;
            progress_ = 1.0f;
            actionButton_.setEnabled(false);
            actionButton_.setButtonText(TRANS("Completed"));
            cancelButton_.setButtonText(TRANS("Close"));
        } else if (ms.installed) {
            text = "Model fully installed.";
            statusText_ = text;
            progress_ = 1.0f;
            actionButton_.setEnabled(false);
            actionButton_.setButtonText(TRANS("Completed"));
            cancelButton_.setButtonText(TRANS("Close"));
        } else if (ms.status == "downloading") {
            auto mbDown = static_cast<double>(ms.downloaded_bytes) / (1024.0 * 1024.0);
            auto mbTotal = static_cast<double>(ms.total_bytes) / (1024.0 * 1024.0);
            text << "Downloading: " << juce::String(mbDown, 1) << " / " << juce::String(mbTotal, 1) << " MB ("
                 << juce::String(static_cast<int>(ms.progress * 100)) << "%)\n";
            text << "Speed: " << juce::String(ms.speed_kbps, 1)
                 << " KB/s | ETA: " << juce::String(static_cast<int>(ms.eta_seconds)) << "s";
            actionButton_.setButtonText(TRANS("Pause"));
            cancelButton_.setButtonText(TRANS("Abort"));
            actionButton_.setEnabled(true);
        } else if (ms.status == "paused") {
            text = "Download paused.";
            actionButton_.setButtonText(TRANS("Resume"));
            cancelButton_.setButtonText(TRANS("Abort"));
            actionButton_.setEnabled(true);
        } else if (ms.status == "stopped" || ms.status == "idle") {
            text = "Download stopped. Click Start to download model.";
            actionButton_.setButtonText(TRANS("Start"));
            cancelButton_.setButtonText(TRANS("Close"));
            actionButton_.setEnabled(true);
        } else if (ms.status == "failed") {
            text = "Error: " + ms.error_message;
            actionButton_.setButtonText(TRANS("Retry"));
            cancelButton_.setButtonText(TRANS("Close"));
            actionButton_.setEnabled(true);
        } else if (ms.status == "blocked") {
            // downloader.py refused the download (downloads off in Settings,
            // or no trusted checksum pin) - show why; Start stays available
            // for after the setting is changed.
            text = "Download unavailable: " + ms.error_message;
            actionButton_.setButtonText(TRANS("Start"));
            cancelButton_.setButtonText(TRANS("Close"));
            actionButton_.setEnabled(true);
        } else if (ms.status.isNotEmpty()) {
            text = "Status: " + ms.status + (ms.error_message.isNotEmpty() ? " - " + ms.error_message : juce::String());
            cancelButton_.setButtonText(TRANS("Close"));
        }

        // A failed Start/Pause/folder change stays visible over the status
        // text until the next action (the 300 ms poll would otherwise wipe it).
        statusText_ = actionError_.isNotEmpty() ? actionError_ : text;
        repaint();
    }
}

void ModelDownloadDialog::selectCustomFolder()
{
    auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    auto chooser = std::make_shared<juce::FileChooser>("Select models directory", juce::File(), "*");

    chooser->launchAsync(chooserFlags, [this, chooser](const juce::FileChooser& fc) {
        auto result = fc.getResult();
        if (result.isDirectory()) {
            auto path = result.getFullPathName();
            controller_.sessions().sessionManager().setUserPreference("models_dir", path.toStdString());
            runBridgeAction([path](ipc::HttpServiceBridge& bridge) { return bridge.setModelsDir(path); });
        }
    });
}

} // namespace khensu
