// desktop-app/Source/ui/shared/NlCommandOverlay.cpp
#include "NlCommandOverlay.h"
#include "PanelRunState.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/session/Session.h>
#include "JobPoller.h"
#include "ServiceErrorHint.h"
#include "SessionAudioPaths.h"
#include "SessionEpoch.h"
#include <optional>

namespace khensu {

namespace apif = api::fields;

namespace {
juce::String effectLine(const juce::var& fx)
{
    juce::StringArray params;
    if (auto* obj = fx[apif::PlanEffect::parameters].getDynamicObject())
        for (const auto& p : obj->getProperties())
            params.add(p.name.toString() + " " + juce::String(static_cast<double>(p.value), 2));
    return fx[apif::PlanEffect::effect].toString() + " (" + params.joinIntoString(", ") + ")";
}

void appendList(juce::String& out, const juce::var& list, const juce::String& heading)
{
    auto* arr = list.getArray();
    if (arr == nullptr || arr->isEmpty()) return;
    out << "\n" << heading << "\n";
    for (const auto& item : *arr) out << "  - " << item.toString() << "\n";
}
} // namespace

juce::String NlCommandOverlay::formatPreview(const juce::var& parsed)
{
    juce::String out;
    if (auto* cmds = parsed[apif::CommandParseResponse::commands].getArray()) {
        for (const auto& c : *cmds) {
            const juce::String explanation = c[apif::ParsedCommand::explanation].toString();
            out << "- " << (explanation.isNotEmpty() ? explanation : c[apif::ParsedCommand::clause].toString())
                << "\n";
        }
    }
    const auto plan = parsed[apif::CommandParseResponse::plan];
    if (auto* fx = plan[apif::CommandPlan::mix_effects].getArray(); fx != nullptr && !fx->isEmpty()) {
        out << "\n" << TRANS("Whole mix:") << "\n";
        for (const auto& e : *fx) out << "  " << effectLine(e) << "\n";
    }
    if (auto* secs = plan[apif::CommandPlan::sections].getArray(); secs != nullptr && !secs->isEmpty()) {
        out << "\n" << TRANS("Sections:") << "\n";
        for (const auto& s : *secs) {
            out << "  " << s[apif::PlanSection::role].toString() << "  "
                << juce::String(static_cast<double>(s[apif::PlanSection::start_sec]), 1) << "-"
                << juce::String(static_cast<double>(s[apif::PlanSection::end_sec]), 1) << " s\n";
            if (auto* fx = s[apif::PlanSection::effects].getArray())
                for (const auto& e : *fx) out << "    " << effectLine(e) << "\n";
        }
    }
    if (auto* stems = plan[apif::CommandPlan::stems].getArray(); stems != nullptr && !stems->isEmpty()) {
        out << "\n" << TRANS("Stems (processed, then added back to the mix):") << "\n";
        for (const auto& st : *stems) {
            out << "  " << st[apif::PlanStem::stem].toString() << "\n";
            if (auto* fx = st[apif::PlanStem::effects].getArray())
                for (const auto& e : *fx) out << "    " << effectLine(e) << "\n";
        }
    }
    appendList(out, parsed[apif::CommandParseResponse::unresolved], TRANS("Not done:"));
    appendList(out, parsed[apif::CommandParseResponse::unparsed], TRANS("Not understood:"));
    appendList(out, parsed[apif::CommandParseResponse::warnings], TRANS("Notes:"));
    const juce::String parser = parsed[apif::CommandParseResponse::parser].toString();
    if (parser.startsWith("llm") || parser.contains("+llm"))
        out << "\n" << TRANS("Part of this was read by a language model (intent only; the numbers above are "
                             "Khensu's own).")
            << "\n";
    return out.trim();
}

void NlCommandOverlay::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    instructionLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentTeal);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    preview_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgRaised);
    preview_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    preview_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    confirmButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal);
    confirmButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::bgVoid);
    const bool live = ptt_.state() == push_to_talk::Machine::State::Recording;
    micButton_.setColour(juce::TextButton::buttonColourId,
                         live ? KhensuTheme::accentRed : KhensuTheme::accentTeal.withAlpha(0.2f));
    micButton_.setColour(juce::TextButton::textColourOffId, live ? KhensuTheme::bgVoid : KhensuTheme::accentTeal);
}

NlCommandOverlay::NlCommandOverlay(IAppController& controller, const juce::String& text, std::string baseUrl)
    : controller_(controller), baseUrl_(std::move(baseUrl)), text_(text.trim())
{
    applyThemeColours();
    addAndMakeVisible(card_);
    titleLabel_.setText(TRANS("Mix instruction"), juce::dontSendNotification);
    titleLabel_.setFont(KhensuTheme::uiFont(15.0f, juce::Font::bold));
    instructionLabel_.setText(text_.isNotEmpty() ? "\"" + text_ + "\""
                                                 : TRANS("Hold the mic button and say what to change."),
                              juce::dontSendNotification);
    instructionLabel_.setFont(KhensuTheme::bodyFont(14.0f));
    instructionLabel_.setMinimumHorizontalScale(0.6f);
    statusLabel_.setFont(KhensuTheme::labelFont(11.0f));
    preview_.setMultiLine(true);
    preview_.setReadOnly(true);
    preview_.setScrollbarsShown(true);
    preview_.setFont(KhensuTheme::monoFont(11.0f));
    preview_.setTitle(TRANS("What will be rendered"));
    confirmButton_.setEnabled(false);
    confirmButton_.setTooltip(TRANS("Render this plan through the guarded render pipeline"));
    confirmButton_.onClick = [this] { confirm(); };
    closeButton_.onClick = [this] { dismiss(); };
    micButton_.setTitle(TRANS("Push to talk"));
    micButton_.setTooltip(TRANS("Hold and speak a mix instruction, then release. Or tap once to start and again to "
                                "stop. Uses the default microphone; playback is not interrupted."));
    micButton_.onPress = [this] { handlePtt(ptt_.press(juce::Time::getMillisecondCounterHiRes())); };
    micButton_.onRelease = [this] { handlePtt(ptt_.release(juce::Time::getMillisecondCounterHiRes())); };
    micButton_.onActivate = [this] { handlePtt(ptt_.activate()); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&titleLabel_, &instructionLabel_, &statusLabel_,
                                                                      &preview_, &micButton_, &confirmButton_,
                                                                      &closeButton_})
        card_.addAndMakeVisible(c);
    card_.addChildComponent(experimentalBadge_);
    experimentalBadge_.forFeature("voice_commands");
    grabModalFocus();
    if (text_.isNotEmpty())
        startParse();
    else
        statusLabel_.setText(TRANS("Hold the mic button (or tap it to start and stop) and speak."),
                             juce::dontSendNotification);
}

NlCommandOverlay::~NlCommandOverlay()
{
    stopTimer();
    stop_->store(true);
    recorder_.stop();
}

void NlCommandOverlay::resized()
{
    auto bounds = getLocalBounds();
    const int cardW = juce::jmin(640, bounds.getWidth() - 40);
    const int cardH = juce::jmin(440, bounds.getHeight() - 40);
    card_.setBounds((bounds.getWidth() - cardW) / 2, (bounds.getHeight() - cardH) / 2, cardW, cardH);
    auto b = card_.getLocalBounds().reduced(16);
    titleLabel_.setBounds(experimentalBadge_.placeAtRight(b.removeFromTop(24)));
    instructionLabel_.setBounds(b.removeFromTop(24));
    b.removeFromTop(6);
    auto buttons = b.removeFromBottom(30);
    closeButton_.setBounds(buttons.removeFromRight(90));
    buttons.removeFromRight(8);
    confirmButton_.setBounds(buttons.removeFromRight(120));
    micButton_.setBounds(buttons.removeFromLeft(130));
    b.removeFromBottom(6);
    statusLabel_.setBounds(b.removeFromBottom(20));
    preview_.setBounds(b);
}

void NlCommandOverlay::sessionInputs(juce::String& filePath, std::map<juce::String, juce::String>& stems)
{
    const auto* session = controller_.getActiveSession();
    if (session != nullptr && !session->files.empty()) {
        sessionId_ = juce::String(session->id);
        filePath = controller_.renders().renderInputPath(); // WF-1: stack on the current version
        inputPath_ = filePath;
        stems = session_audio::activeStemPaths(controller_);
    }
}

void NlCommandOverlay::startParse()
{
    juce::String filePath;
    std::map<juce::String, juce::String> stems;
    sessionInputs(filePath, stems);
    statusLabel_.setText(filePath.isEmpty() ? TRANS("No session audio: showing what the instruction means only.")
                                            : TRANS("Reading the instruction and finding the sections..."),
                         juce::dontSendNotification);
    juce::Component::SafePointer<NlCommandOverlay> self(this);
    controller_.runInBackground([self, &work = controller_, text = text_, filePath, stems, baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto result = bridge.parseCommand(text, filePath, stems, true);
        postIfCurrent(self, work, [result = std::move(result)](NlCommandOverlay& panel) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Could not read the instruction: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            panel.onParsed(result.value());
        }, nullptr, [](NlCommandOverlay& panel) {
            panel.statusLabel_.setText(TRANS("The session changed - run the instruction again."),
                                       juce::dontSendNotification);
        });
    });
}

void NlCommandOverlay::handlePtt(push_to_talk::Machine::Action action)
{
    using Action = push_to_talk::Machine::Action;
    if (action == Action::StartRecording)
        startRecording();
    else if (action == Action::StopAndSend)
        stopAndSend();
    applyThemeColours();
    repaint();
}

void NlCommandOverlay::startRecording()
{
    const juce::String error = recorder_.start(push_to_talk::kMaxSeconds);
    if (error.isNotEmpty()) {
        ptt_.reset();
        statusLabel_.setText(error, juce::dontSendNotification);
        return;
    }
    planId_.clear();
    confirmButton_.setEnabled(false);
    micButton_.setButtonText(ptt_.latched() ? TRANS("Tap to stop") : TRANS("Listening..."));
    statusLabel_.setText(TRANS("Listening..."), juce::dontSendNotification);
    startTimerHz(10);
}

void NlCommandOverlay::timerCallback()
{
    if (ptt_.state() != push_to_talk::Machine::State::Recording) {
        stopTimer();
        return;
    }
    if (ptt_.latched()) micButton_.setButtonText(TRANS("Tap to stop"));
    const double secs = recorder_.recordedSeconds();
    const int bars = juce::jlimit(0, 10, juce::roundToInt(recorder_.inputLevel() * 20.0f));
    statusLabel_.setText(TRANS("Listening... ") + juce::String(secs, 1) + " s  " + juce::String::repeatedString("|", bars),
                         juce::dontSendNotification);
    if (secs >= push_to_talk::kMaxSeconds - 0.05) handlePtt(ptt_.activate()); // buffer full: stop and send
}

void NlCommandOverlay::stopAndSend()
{
    stopTimer();
    micButton_.setButtonText(TRANS("Hold to talk"));
    const double sampleRate = recorder_.sampleRate();
    auto samples = recorder_.stop();
    if (sampleRate <= 0.0 || (double)samples.size() < push_to_talk::kMinSeconds * sampleRate) {
        ptt_.reset();
        statusLabel_.setText(TRANS("Too short - hold the button while you speak."), juce::dontSendNotification);
        return;
    }
    juce::String filePath;
    std::map<juce::String, juce::String> stems;
    sessionInputs(filePath, stems);
    micButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Transcribing..."), juce::dontSendNotification);
    const juce::File wavFile =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("khensu_ptt", ".wav");
    juce::Component::SafePointer<NlCommandOverlay> self(this);
    controller_.runInBackground([self, &work = controller_, samples = std::move(samples), sampleRate, wavFile, filePath, stems,
                                 baseUrl = baseUrl_]() {
        const juce::String writeError = push_to_talk::writeWav(wavFile, samples, sampleRate);
        std::optional<khensu::Result<juce::var, ipc::BridgeError>> result;
        if (writeError.isEmpty()) {
            ipc::HttpServiceBridge bridge(baseUrl);
            result = bridge.voiceCommand(wavFile.getFullPathName(), filePath, stems, true);
        }
        wavFile.deleteFile();
        postIfCurrent(self, work, [result = std::move(result), writeError](NlCommandOverlay& panel) {
            if (writeError.isNotEmpty() || !result.has_value()) {
                panel.statusLabel_.setText(TRANS("Could not save the recording: ") + writeError,
                                           juce::dontSendNotification);
                return;
            }
            if (result->is_err()) {
                const auto& e = result->error();
                panel.statusLabel_.setText(e.kind == ipc::BridgeError::Kind::HttpError
                                               ? push_to_talk::voiceErrorMessage(e.http_status,
                                                                                 juce::String(e.message))
                                               : TRANS("Could not reach the voice service: ") + serviceErrorLine(e),
                                           juce::dontSendNotification);
                return;
            }
            panel.onVoiceResult(result->value());
        }, [](NlCommandOverlay& panel) {
            panel.ptt_.reset(); // reset busy state first
            panel.micButton_.setEnabled(true);
            panel.applyThemeColours();
        }, [](NlCommandOverlay& panel) {
            panel.statusLabel_.setText(TRANS("The session changed - say the instruction again."),
                                       juce::dontSendNotification);
        });
    });
}

void NlCommandOverlay::onVoiceResult(const juce::var& parsed)
{
    const juce::String transcript = parsed[apif::VoiceCommandResponse::transcript].toString().trim();
    text_ = transcript;
    instructionLabel_.setText(transcript.isNotEmpty() ? "\"" + transcript + "\"" : TRANS("(nothing recognised)"),
                              juce::dontSendNotification);
    onParsed(parsed);
}

void NlCommandOverlay::onParsed(const juce::var& parsed)
{
    preview_.setText(formatPreview(parsed), juce::dontSendNotification);
    planId_ = parsed[apif::CommandParseResponse::plan_id].toString();
    const bool executable = static_cast<bool>(parsed[apif::CommandParseResponse::executable]) && planId_.isNotEmpty();
    confirmButton_.setEnabled(executable);
    statusLabel_.setText(executable ? TRANS("Check the plan, then render it. Nothing has changed yet.")
                                    : TRANS("Nothing to render for this instruction."),
                         juce::dontSendNotification);
}

void NlCommandOverlay::confirm()
{
    if (planId_.isEmpty()) return;
    const auto* session = controller_.getActiveSession();
    if (session == nullptr || juce::String(session->id) != sessionId_) {
        statusLabel_.setText(TRANS("The session changed - run the instruction again."), juce::dontSendNotification);
        confirmButton_.setEnabled(false);
        return;
    }
    confirmButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Rendering..."), juce::dontSendNotification);
    juce::Component::SafePointer<NlCommandOverlay> self(this);
    auto* controller = &controller_;
    controller_.runLongInBackground([self, controller, planId = planId_, sessionId = sessionId_, stop = stop_,
                                     baseUrl = baseUrl_]() {
        ipc::HttpServiceBridge bridge(baseUrl);
        using Poller = JobPoller<juce::var>;
        Poller::Hooks hooks;
        hooks.start = [&bridge, planId, sessionId]() -> Poller::StartResult {
            auto r = bridge.startCommandExecute(planId, sessionId);
            if (r.is_err()) return Poller::StartResult::err(r.error());
            return Poller::StartResult::ok(r.value().toStdString());
        };
        hooks.poll = [&bridge](const std::string& id) { return bridge.getCommandExecuteStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelCommandExecute(juce::String(id)); };
        hooks.isTerminal = [](const juce::var& st) {
            return !api::isActiveStatus(st[apif::CommandJobStatusResponse::status].toString());
        };
        const uint64_t epoch = controller->sessionEpoch();
        hooks.shouldStop = [stop, controller, epoch] { return stop->load() || controller->sessionEpoch() != epoch; };
        Poller::Options opts;
        opts.deadlineSeconds = 20 * 60;
        const auto out = Poller::run(hooks, opts);
        bool ok = false;
        juce::String message, outputPath;
        if (!out.finished() || !out.status.has_value()) {
            message = out.describe();
        } else {
            const auto& st = *out.status;
            const auto result = st[apif::CommandJobStatusResponse::result];
            if (st[apif::CommandJobStatusResponse::status].toString() == api::job_status::completed) {
                ok = true;
                outputPath = result[apif::CommandExecuteResult::output_path].toString();
            } else {
                message = st[apif::CommandJobStatusResponse::error].toString();
                if (message.isEmpty()) message = TRANS("the render failed");
            }
        }
        juce::MessageManager::callAsync([self, ok, message, outputPath, epoch, controller]() {
            if (self == nullptr) return;
            if (controller->sessionEpoch() != epoch) return;
            self->onExecuted(ok, message, outputPath);
        });
    });
}

void NlCommandOverlay::onExecuted(bool ok, const juce::String& message, const juce::String& outputPath)
{
    if (!ok) {
        statusLabel_.setText(TRANS("Not applied: ") + message, juce::dontSendNotification);
        return;
    }
    controller_.renders().registerRenderOf(outputPath, inputPath_, sessionId_);
    statusLabel_.setText(TRANS("Applied: ") + juce::File(outputPath).getFileName() + " - " +
                             TRANS("it is now the current render (undo with Ctrl+Z)."),
                         juce::dontSendNotification);
    closeButton_.setButtonText(TRANS("Done"));
}

} // namespace khensu
