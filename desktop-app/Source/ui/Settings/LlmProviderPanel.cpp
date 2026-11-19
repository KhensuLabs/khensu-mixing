// desktop-app/Source/ui/Settings/LlmProviderPanel.cpp
#include "LlmProviderPanel.h"
#include "../../settings/OnlineSettings.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/common/AppPaths.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace apif = api::fields;

juce::String LlmProviderPanel::formatReport(const juce::var& report)
{
    juce::String out;
    const auto chat = report[apif::ProvidersResponse::active]["chat"];
    const juce::String provider = chat[apif::ActiveProviderInfo::provider].toString();
    const juce::String choice = report[apif::ProvidersResponse::choice].toString();
    if (provider.isEmpty()) {
        out << TRANS("In use:") << " " << TRANS("none - the copilot answers from its built-in rules") << " ("
            << TRANS("choice") << ": " << choice << ")";
    } else {
        const bool local = static_cast<bool>(chat[apif::ActiveProviderInfo::local]);
        out << TRANS("In use:") << " " << provider;
        const juce::String model = chat[apif::ActiveProviderInfo::model].toString();
        if (model.isNotEmpty()) out << " (" << model << ")";
        out << " - " << (local ? TRANS("local, nothing leaves this machine") : TRANS("cloud")) << " ("
            << TRANS("choice") << ": " << choice << ")";
    }
    juce::StringArray found, missing;
    if (auto* servers = report[apif::ProvidersResponse::local_servers].getArray()) {
        for (const auto& s : *servers) {
            const juce::String label = s[apif::LocalServerInfo::label].toString();
            if (static_cast<bool>(s[apif::LocalServerInfo::available])) {
                const int n = s[apif::LocalServerInfo::models].isArray() ? s[apif::LocalServerInfo::models].size() : 0;
                found.add(label + " (" + juce::String(n) + " " + TRANS("models") + ")");
            } else {
                missing.add(label);
            }
        }
    }
    out << "\n" << TRANS("Found:") << " " << (found.isEmpty() ? TRANS("no local server") : found.joinIntoString(", "));
    if (!missing.isEmpty()) out << "   " << TRANS("Not running:") << " " << missing.joinIntoString(", ");
    const auto gguf = report[apif::ProvidersResponse::llamacpp];
    out << "\n" << TRANS("In-process GGUF:") << " "
        << (static_cast<bool>(gguf[apif::LlamaCppInfo::available]) ? TRANS("ready")
                                                                   : gguf[apif::LlamaCppInfo::reason].toString());
    if (!static_cast<bool>(report[apif::ProvidersResponse::online_llm_allowed]))
        out << "\n" << TRANS("Cloud LLMs are off in Settings; local models still work.");
    return out;
}

void LlmProviderPanel::applyThemeColours()
{
    headingLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    for (auto* l : {&reportLabel_, &ggufLabel_, &urlLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

LlmProviderPanel::LlmProviderPanel(IAppController& c, std::string baseUrl, juce::File configDir)
    : controller_(c), baseUrl_(std::move(baseUrl)),
      configDir_(configDir == juce::File() ? juce::File(juce::String(AppPaths::dataDirectory())) : std::move(configDir))
{
    applyThemeColours();
    headingLabel_.setFont(KhensuTheme::uiFont(12.0f, juce::Font::bold));
    reportLabel_.setFont(KhensuTheme::labelFont(10.5f));
    reportLabel_.setJustificationType(juce::Justification::topLeft);
    reportLabel_.setText(TRANS("Checking which LLM is available..."), juce::dontSendNotification);
    for (auto* l : {&ggufLabel_, &urlLabel_})
        l->setFont(KhensuTheme::labelFont(10.5f));

    detectButton_.setTooltip(TRANS("Look for llama.cpp server (:8080), Ollama (:11434) and LM Studio (:1234) on this "
                                   "computer"));
    detectButton_.onClick = [this] { refresh(true); };

    const OnlineSettings saved = OnlineSettings::load(configDir_);
    ggufField_.setText(saved.llmGgufPath, juce::dontSendNotification);
    ggufField_.setTextToShowWhenEmpty(TRANS("path to a .gguf model (needs the local_llm extra)"),
                                      KhensuTheme::textTertiary);
    ggufField_.setTitle(TRANS("GGUF model file"));
    ggufField_.onReturnKey = [this] { saveLocalSettings(); };
    ggufField_.onFocusLost = [this] { saveLocalSettings(); };
    browseButton_.setTooltip(TRANS("Choose a downloaded GGUF model file"));
    browseButton_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>(TRANS("Choose a GGUF model"), juce::File(), "*.gguf");
        juce::Component::SafePointer<LlmProviderPanel> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [self](const juce::FileChooser& fc) {
                                  if (self == nullptr || fc.getResult() == juce::File()) return;
                                  self->ggufField_.setText(fc.getResult().getFullPathName(),
                                                           juce::dontSendNotification);
                                  self->saveLocalSettings();
                              });
    };
    urlField_.setText(saved.llmLocalUrl, juce::dontSendNotification);
    urlField_.setTextToShowWhenEmpty("http://127.0.0.1:5000/v1", KhensuTheme::textTertiary); // i18n-exempt(example URL)
    urlField_.setTitle(TRANS("Other local server URL"));
    urlField_.setTooltip(TRANS("Any OpenAI-compatible server on this computer (localhost / 127.0.0.1 only)"));
    urlField_.onReturnKey = [this] { saveLocalSettings(); };
    urlField_.onFocusLost = [this] { saveLocalSettings(); };

    for (juce::Component* comp : std::initializer_list<juce::Component*>{
             &headingLabel_, &reportLabel_, &detectButton_, &ggufLabel_, &ggufField_, &browseButton_, &urlLabel_,
             &urlField_})
        addAndMakeVisible(comp);
    addChildComponent(experimentalBadge_);
    experimentalBadge_.forFeature("local_llm");
    refresh(false);
}

void LlmProviderPanel::resized()
{
    auto b = getLocalBounds();
    auto top = b.removeFromTop(20);
    detectButton_.setBounds(top.removeFromRight(150));
    headingLabel_.setBounds(experimentalBadge_.placeAtRight(top));
    reportLabel_.setBounds(b.removeFromTop(62));
    b.removeFromTop(4);
    auto row1 = b.removeFromTop(24);
    ggufLabel_.setBounds(row1.removeFromLeft(140));
    browseButton_.setBounds(row1.removeFromRight(80));
    row1.removeFromRight(4);
    ggufField_.setBounds(row1);
    b.removeFromTop(4);
    auto row2 = b.removeFromTop(24);
    urlLabel_.setBounds(row2.removeFromLeft(140));
    urlField_.setBounds(row2);
}

void LlmProviderPanel::saveLocalSettings()
{
    const juce::String gguf = ggufField_.getText().trim();
    const juce::String url = urlField_.getText().trim();
    const bool loopback = url.isEmpty() || url.startsWithIgnoreCase("http://127.0.0.1") ||
                          url.startsWithIgnoreCase("http://localhost") || url.startsWithIgnoreCase("http://[::1]");
    const juce::File configDir = configDir_;
    OnlineSettings::update(configDir, [gguf, url, loopback](OnlineSettings& s) {
        s.llmGgufPath = gguf;
        if (loopback) s.llmLocalUrl = url;
    });
    if (!loopback)
        reportLabel_.setText(TRANS("The server URL must be on this computer (localhost or 127.0.0.1) - not saved."),
                             juce::dontSendNotification);
    else
        refresh(false);
}

void LlmProviderPanel::refresh(bool reprobe)
{
    detectButton_.setEnabled(false);
    if (reprobe) reportLabel_.setText(TRANS("Looking for local models..."), juce::dontSendNotification);
    juce::Component::SafePointer<LlmProviderPanel> self(this);
    controller_.runInBackground([self, reprobe, baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto result = bridge.getLlmProviders(reprobe);
        juce::MessageManager::callAsync([self, result = std::move(result)]() {
            if (self == nullptr) return;
            self->detectButton_.setEnabled(true);
            // Machine-wide state, not session-scoped: no epoch check needed.
            if (result.is_err()) {
                self->reportLabel_.setText(TRANS("Could not ask the service: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const juce::String text = formatReport(result.value());
            self->reportLabel_.setText(text, juce::dontSendNotification);
            self->reportLabel_.setTooltip(text);
        });
    });
}

} // namespace khensu
