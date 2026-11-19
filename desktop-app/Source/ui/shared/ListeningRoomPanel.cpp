// desktop-app/Source/ui/shared/ListeningRoomPanel.cpp
#include "ListeningRoomPanel.h"
#include "PanelRunState.h"
#include "../../IAppController.h"
#include "../../PythonServiceManager.h"
#include "ServiceErrorHint.h"
#include "SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace apif = api::fields;

namespace {
constexpr int kPollMs = 2000;
constexpr int kMaxExtraNodes = 4;

juce::String gainLine(const juce::var& versions)
{
    juce::String s;
    if (auto* arr = versions.getArray()) {
        for (const auto& v : *arr) {
            if (s.isNotEmpty()) s << "   ";
            s << v[apif::ListeningVersionOut::label].toString() << ": "
              << juce::String(static_cast<double>(v[apif::ListeningVersionOut::gain_db]), 1) << " dB";
        }
    }
    return s;
}
} // namespace

void ListeningRoomPanel::applyThemeColours()
{
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    votesLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    urlField_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgOverlay);
    urlField_.setColour(juce::TextEditor::textColourId, KhensuTheme::textAccent);
    urlField_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
}

ListeningRoomPanel::ListeningRoomPanel(IAppController& c) : PanelCard(c, "Listening Room")
{
    applyThemeColours();
    if (const auto* f = maturity::findExperimentalFeature("listening_room"))
        setExperimental(true, juce::translate(f->outstandingCheck));
    setExplainer(TRANS("Listening Room"),
                 TRANS("Share a blind A/B test with people in the same room or on the same network. "
                       "Khensu matches the versions' loudness, gives each listener their own random order, "
                       "and counts which version they prefer - nobody sees which is which.\n\n"
                       "Off by default: turn on the master online switch and 'LAN listening room' in "
                       "Settings. Only the loudness-matched copies are served, under a private link; "
                       "nothing else on this computer is reachable."));

    startButton_.setTooltip(TRANS("Share the original, the current render and recent renders as a blind A/B room"));
    stopButton_.setTooltip(TRANS("Close the room, delete its copies and keep the votes"));
    copyButton_.setTooltip(TRANS("Copy the room link to the clipboard"));
    urlField_.setTitle(TRANS("Listening room link"));
    urlField_.setTooltip(TRANS("Open this link on a phone or laptop on the same network"));
    urlField_.setReadOnly(true);
    urlField_.setFont(KhensuTheme::monoFont(11.0f));
    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    votesLabel_.setFont(KhensuTheme::monoFont(11.0f));
    votesLabel_.setJustificationType(juce::Justification::topLeft);
    statusLabel_.setText(TRANS("Start a room to let others vote blind between your versions."),
                         juce::dontSendNotification);

    startButton_.onClick = [this] { startRoom(); };
    stopButton_.onClick = [this] { stopRoom(); };
    copyButton_.onClick = [this] {
        if (urlField_.getText().isNotEmpty()) {
            juce::SystemClipboard::copyTextToClipboard(urlField_.getText());
            statusLabel_.setText(TRANS("Link copied."), juce::dontSendNotification);
        }
    };
    for (auto* comp : std::initializer_list<juce::Component*>{&startButton_, &stopButton_, &copyButton_, &urlField_,
                                                               &statusLabel_, &votesLabel_})
        addAndMakeVisible(comp);
    setRunning(false);
}

ListeningRoomPanel::~ListeningRoomPanel() { stopTimer(); }

void ListeningRoomPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea().reduced(4);
    auto row = b.removeFromTop(26);
    startButton_.setBounds(row.removeFromLeft(100).reduced(1));
    stopButton_.setBounds(row.removeFromLeft(90).reduced(1));
    copyButton_.setBounds(row.removeFromRight(90).reduced(1));
    urlField_.setBounds(row.reduced(2, 1));
    b.removeFromTop(4);
    statusLabel_.setBounds(b.removeFromTop(32));
    votesLabel_.setBounds(b);
}

void ListeningRoomPanel::resetForSession()
{
    // A room belongs to the session it was started for: close it.
    if (running_) stopRoom();
}

void ListeningRoomPanel::visibilityChanged()
{
    if (isShowing() && !running_) syncWithService();
}

void ListeningRoomPanel::syncWithService()
{
    const auto* session = controller_.getActiveSession();
    if (session == nullptr) return;
    const juce::String sessionId(session->id);
    juce::Component::SafePointer<ListeningRoomPanel> self(this);
    controller_.runInBackground([self, &work = controller_, sessionId] {
        const auto result = ipc::HttpServiceBridge(PythonServiceManager::kBaseUrl).getCollab("/listening/status");
        postIfCurrent(self, work, [result, sessionId](ListeningRoomPanel& panel) {
            if (panel.running_ || result.is_err()) return;
            const auto& v = result.value();
            if (!static_cast<bool>(v[apif::ListeningStatusResponse::running])) return;
            if (v[apif::ListeningStatusResponse::session_id].toString() != sessionId) {
                // Another session's room: offer to close it, never adopt it.
                panel.stopButton_.setEnabled(true);
                panel.statusLabel_.setText(TRANS("A room from another session is still open - Stop room closes it."),
                                           juce::dontSendNotification);
                return;
            }
            panel.urlField_.setText(v[apif::ListeningStatusResponse::url].toString(), juce::dontSendNotification);
            panel.gainsText_ = gainLine(v[apif::ListeningStatusResponse::versions]);
            panel.statusLabel_.setText(TRANS("Room open. Loudness match: ") + panel.gainsText_,
                                       juce::dontSendNotification);
            panel.setRunning(true);
            panel.pollResults();
        });
    });
}

void ListeningRoomPanel::setRunning(bool running)
{
    running_ = running;
    startButton_.setEnabled(!running);
    stopButton_.setEnabled(running);
    copyButton_.setEnabled(running);
    if (running)
        startTimer(kPollMs);
    else
        stopTimer();
}

void ListeningRoomPanel::startRoom()
{
    const auto* session = controller_.getActiveSession();
    const juce::String original = controller_.activeSourcePath();
    const juce::String current = controller_.currentRenderPath();
    if (session == nullptr || original.isEmpty()) {
        statusLabel_.setText(TRANS("Open a session first."), juce::dontSendNotification);
        return;
    }
    juce::Array<juce::var> versions;
    juce::StringArray used;
    auto add = [&](const juce::String& label, const juce::String& path) {
        if (path.isEmpty() || used.contains(path) || !juce::File(path).existsAsFile() || versions.size() >= 6) return;
        juce::DynamicObject::Ptr v = new juce::DynamicObject();
        v->setProperty(apif::ListeningVersionIn::label, label);
        v->setProperty(apif::ListeningVersionIn::path, path);
        versions.add(juce::var(v.get()));
        used.add(path);
    };
    add("Original", original);
    add("Current render", current);
    const auto& tree = controller_.renders().renderTree();
    int extra = 0;
    for (int i = (int)tree.size() - 1; i >= 0 && extra < kMaxExtraNodes; --i) {
        const auto before = versions.size();
        add("Render " + juce::String(i + 1), tree[(size_t)i].path);
        if (versions.size() > before) ++extra;
    }
    if (versions.size() < 2) {
        statusLabel_.setText(TRANS("Apply at least one render: a room compares two or more versions."),
                             juce::dontSendNotification);
        return;
    }

    juce::DynamicObject::Ptr body = new juce::DynamicObject();
    body->setProperty(apif::ListeningStartRequest::session_id, juce::String(session->id));
    body->setProperty(apif::ListeningStartRequest::versions, juce::var(versions));
    const juce::var request(body.get());

    startButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Matching loudness and opening the room..."), juce::dontSendNotification);
    juce::Component::SafePointer<ListeningRoomPanel> self(this);
    controller_.runLongInBackground([self, &work = controller_, request] {
        const auto result =
            ipc::HttpServiceBridge(PythonServiceManager::kBaseUrl).postCollab("/listening/start", request);
        postIfCurrent(self, work, [result](ListeningRoomPanel& panel) {
            if (result.is_err()) return;
            const auto& v = result.value();
            panel.urlField_.setText(v[apif::ListeningStatusResponse::url].toString(), juce::dontSendNotification);
            panel.gainsText_ = gainLine(v[apif::ListeningStatusResponse::versions]);
            panel.statusLabel_.setText(TRANS("Room open. Loudness match: ") + panel.gainsText_,
                                       juce::dontSendNotification);
            panel.votesLabel_.setText(TRANS("No votes yet."), juce::dontSendNotification);
            panel.setRunning(true);
        }, [result](ListeningRoomPanel& panel) {
            panel.startButton_.setEnabled(!panel.running_);
            if (result.is_err()) {
                const auto& e = result.error();
                panel.statusLabel_.setText(
                    e.http_status == 403
                        ? TRANS("Turn on 'LAN listening room' in Settings (it is off by default).")
                        : serviceErrorLine(e),
                    juce::dontSendNotification);
            }
        }, [ok = result.is_ok()](ListeningRoomPanel& panel) {
            if (!ok) return;
            // The session changed while the room opened: close it again.
            panel.running_ = true;
            panel.stopRoom();
        });
    });
}

void ListeningRoomPanel::stopRoom()
{
    setRunning(false);
    stopButton_.setEnabled(false);
    juce::Component::SafePointer<ListeningRoomPanel> self(this);
    controller_.runInBackground([self] {
        const auto result = ipc::HttpServiceBridge(PythonServiceManager::kBaseUrl)
                                .postCollab("/listening/stop", juce::var(new juce::DynamicObject()), 15000);
        juce::MessageManager::callAsync([self, result] {
            if (self == nullptr) return;
            self->urlField_.clear();
            if (result.is_err()) {
                self->statusLabel_.setText(serviceErrorLine(result.error()), juce::dontSendNotification);
                return;
            }
            const auto results = result.value()[apif::ListeningStopResponse::results];
            if (results.isObject()) self->showResults(results);
            self->statusLabel_.setText(TRANS("Room closed. The votes are saved in the session history."),
                                       juce::dontSendNotification);
        });
    });
}

void ListeningRoomPanel::timerCallback() { pollResults(); }

void ListeningRoomPanel::pollResults()
{
    if (pollInFlight_ || !running_) return;
    pollInFlight_ = true;
    juce::Component::SafePointer<ListeningRoomPanel> self(this);
    controller_.runInBackground([self, &work = controller_] {
        const auto result = ipc::HttpServiceBridge(PythonServiceManager::kBaseUrl).getCollab("/listening/results");
        postIfCurrent(self, work, [result](ListeningRoomPanel& panel) {
            if (!panel.running_ || result.is_err()) return;
            const auto& v = result.value();
            if (!static_cast<bool>(v[apif::ListeningResultsResponse::running])) {
                panel.setRunning(false);
                panel.statusLabel_.setText(TRANS("The room is no longer running."), juce::dontSendNotification);
            }
            panel.showResults(v);
        }, [](ListeningRoomPanel& panel) { panel.pollInFlight_ = false; }); // reset busy state first
    });
}

void ListeningRoomPanel::showResults(const juce::var& v)
{
    juce::String text;
    const int listeners = static_cast<int>(v[apif::ListeningResultsResponse::listeners]);
    const int total = static_cast<int>(v[apif::ListeningResultsResponse::total_votes]);
    text << TRANS("Listeners: ") << listeners << "   " << TRANS("Votes: ") << total << "\n";
    if (auto* tallies = v[apif::ListeningResultsResponse::tallies].getArray()) {
        for (const auto& t : *tallies) {
            const double share = static_cast<double>(t[apif::ListeningTally::share]);
            text << t[apif::ListeningTally::label].toString() << ": "
                 << static_cast<int>(t[apif::ListeningTally::votes]) << " (" << juce::roundToInt(share * 100.0)
                 << "%)\n";
        }
    }
    const auto leader = v[apif::ListeningResultsResponse::leader];
    if (total > 0)
        text << (leader.toString().isNotEmpty() ? TRANS("Preferred: ") + leader.toString()
                                                : TRANS("No clear preference yet"));
    votesLabel_.setText(text, juce::dontSendNotification);
}

} // namespace khensu
