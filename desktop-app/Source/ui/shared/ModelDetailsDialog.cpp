// desktop-app/Source/ui/shared/ModelDetailsDialog.cpp
#include "ModelDetailsDialog.h"
#include "ServiceErrorHint.h"
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu {

namespace {
juce::String shortSha(const juce::String& sha) { return sha.isEmpty() ? juce::String("-") : sha.substring(0, 12); }
juce::String humanSize(juce::int64 bytes)
{
    const double mb = (double)bytes / (1024.0 * 1024.0);
    return juce::String(mb, mb < 10.0 ? 2 : 1) + " MB";
}
} // namespace

// Every cached palette colour on this component, in one place (UI-01).
void ModelDetailsDialog::applyThemeColours()
{
    headerLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    body_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgBase);
    body_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    body_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    checkButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
    checkButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    closeButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    closeButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::textPrimary);
}

ModelDetailsDialog::ModelDetailsDialog(BackgroundRunner runInBackground)
    : runInBackground_(std::move(runInBackground))
{
    jassert(runInBackground_ != nullptr);
    applyThemeColours();
    headerLabel_.setFont(KhensuTheme::labelFont(type::caption));
    headerLabel_.setText(TRANS("Bundled learned models"), juce::dontSendNotification);
    addAndMakeVisible(headerLabel_);

    body_.setMultiLine(true);
    body_.setReadOnly(true);
    body_.setCaretVisible(false);
    body_.setScrollbarsShown(true);
    body_.setFont(KhensuTheme::monoFont(type::caption));
    body_.setText(TRANS("Loading model manifest..."), juce::dontSendNotification);
    addAndMakeVisible(body_);

    checkButton_.onClick = [this] { checkForUpdates(); };
    addAndMakeVisible(checkButton_);

    closeButton_.onClick = [this] {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState(0);
    };
    addAndMakeVisible(closeButton_);

    refreshManifest();
}

void ModelDetailsDialog::refreshManifest()
{
    if (requestInFlight_) return;
    requestInFlight_ = true;
    juce::Component::SafePointer<ModelDetailsDialog> self(this);
    runInBackground_([self] {
        ipc::HttpServiceBridge bridge;
        auto res = bridge.getModelManifest();
        juce::MessageManager::callAsync([self, res] {
            if (self == nullptr) return;
            self->requestInFlight_ = false;
            if (res.is_err()) {
                self->body_.setText(TRANS("Could not reach the analysis service."), juce::dontSendNotification);
                return;
            }
            const auto& manifest = res.value();
            // The manifest also lists every expected-but-missing model
            // (installed == false, no size/sha/date) - name those separately.
            juce::StringArray lines, missing;
            for (const auto& m : manifest.models) {
                if (!m.installed) {
                    missing.add(m.name + (m.remediation_hint.isNotEmpty() ? "  - " + m.remediation_hint
                                                                          : juce::String()));
                    continue;
                }
                lines.add(m.name + (m.intervention_validated ? "  [validated]" : "  [opt-in]"));
                lines.add("    version:    " + (m.version.isEmpty() ? juce::String("(none)") : m.version));
                lines.add("    provenance: " + (m.provenance.isEmpty() ? juce::String("(none)") : m.provenance));
                lines.add("    size:       " + humanSize(m.size_bytes));
                lines.add("    sha256:     " + shortSha(m.sha256));
                lines.add("    downloaded: " + m.downloaded_at);
                lines.add("    source:     " + (m.source_url.isEmpty() ? juce::String("(none)") : m.source_url));
                lines.add({});
            }
            if (lines.isEmpty()) lines.add(TRANS("No models installed yet."));
            if (!missing.isEmpty()) {
                lines.add({});
                lines.add(TRANS("Not installed:"));
                for (const auto& m : missing)
                    lines.add("    " + m);
            }
            self->body_.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
        });
    });
}

void ModelDetailsDialog::checkForUpdates()
{
    if (requestInFlight_) return;
    requestInFlight_ = true;
    checkButton_.setEnabled(false);
    checkButton_.setButtonText(TRANS("Checking..."));
    juce::Component::SafePointer<ModelDetailsDialog> self(this);
    runInBackground_([self] {
        ipc::HttpServiceBridge bridge;
        auto res = bridge.checkModelUpdates();
        juce::MessageManager::callAsync([self, res] {
            if (self == nullptr) return;
            self->requestInFlight_ = false;
            self->checkButton_.setEnabled(true);
            self->checkButton_.setButtonText(TRANS("Check for updates"));
            if (res.is_err()) {
                self->body_.setText(TRANS("Update check failed: ") + serviceErrorLine(res.error()),
                                    juce::dontSendNotification);
                return;
            }
            const auto& report = res.value();
            juce::StringArray lines;
            lines.add(report.online ? "Update check (online):" : "Update check (offline - downloads disabled):");
            lines.add({});
            for (const auto& m : report.models) {
                juce::String line = "  " + m.name + ":  " + m.status;
                if (m.status == "update_available" && m.remote_version.isNotEmpty())
                    line += "  (" + (m.local_version.isEmpty() ? juce::String("?") : m.local_version) + " -> " +
                            m.remote_version + ")";
                lines.add(line);
            }
            if (!report.any_update_available) {
                lines.add({});
                lines.add("  All models up to date (or no update source configured).");
            }
            self->body_.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
        });
    });
}

void ModelDetailsDialog::resized()
{
    auto r = getLocalBounds().reduced(16);
    headerLabel_.setBounds(r.removeFromTop(20));
    r.removeFromTop(6);
    auto buttons = r.removeFromBottom(32);
    closeButton_.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(8);
    checkButton_.setBounds(buttons.removeFromRight(140));
    r.removeFromBottom(8);
    body_.setBounds(r);
}

} // namespace khensu
