#include "IssueTimelinePanel.h"

#include "../PluginProcessor.h"
#include "../CopilotServiceClient.h"
#include "PluginLookAndFeel.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/ui/KhensuPalette.h>

#include <algorithm>
#include <cmath>

namespace khensu {

namespace af = khensu::api::fields;

namespace {

juce::String formatTime(double seconds)
{
    seconds = std::max(0.0, seconds);
    const int total = static_cast<int>(std::floor(seconds));
    const int tenths = static_cast<int>(std::floor((seconds - total) * 10.0));
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2) + "." + juce::String(tenths);
}

juce::String sourceTag(const timeline::TimedIssue& issue)
{
    if (issue.source == timeline::IssueSource::Ara)
        return issue.regionName.empty() ? juce::String("ARA") : juce::String(issue.regionName);
    return "Live";
}

} // namespace

juce::Colour IssueTimelinePanel::severityColour(float severity)
{
    if (severity >= 70.0f) return PluginLookAndFeel::statusBad;
    if (severity >= 40.0f) return PluginLookAndFeel::statusWarn;
    return PluginLookAndFeel::accentBlue;
}

IssueTimelinePanel::IssueTimelinePanel(KhensuCopilotProcessor& processor) : processor_(processor)
{
    titleLabel_.setText(TRANS("Issues on the timeline"), juce::dontSendNotification);
    titleLabel_.setFont(khensu::ui::Palette::uiFont(13.0f, juce::Font::bold));
    titleLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textPrimary);
    addAndMakeVisible(titleLabel_);

    liveToggle_.setToggleState(processor_.isLiveAnalysisEnabled(), juce::dontSendNotification);
    liveToggle_.setColour(juce::ToggleButton::textColourId, PluginLookAndFeel::textMuted);
    liveToggle_.setTooltip(TRANS("Measure what this track plays, in 4-second windows stamped with the host time."));
    liveToggle_.setTitle(TRANS("Live analysis"));
    liveToggle_.onClick = [this] { processor_.setLiveAnalysisEnabled(liveToggle_.getToggleState()); };
    addAndMakeVisible(liveToggle_);

    sourceLabel_.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    sourceLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    sourceLabel_.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(sourceLabel_);

    strip_.setTitle(TRANS("Findings timeline"));
    strip_.setTooltip(TRANS("Each bar is a finding at its time on the host timeline. Click one to select it."));
    addAndMakeVisible(strip_);

    list_.setRowHeight(22);
    list_.setColour(juce::ListBox::backgroundColourId, PluginLookAndFeel::bgPanel);
    list_.setColour(juce::ListBox::outlineColourId, PluginLookAndFeel::controlOutline);
    list_.setOutlineThickness(1);
    list_.setTitle(TRANS("Issues"));
    list_.setTooltip(TRANS("Findings in time order. Double-click to move the host playhead there."));
    addAndMakeVisible(list_);

    jumpButton_.setTooltip(TRANS("Move the host playhead to the selected issue (ARA, else MMC Locate)."));
    jumpButton_.onClick = [this] { jumpToSelected(); };
    writeButton_.setTooltip(TRANS("Play the selected fix on this plugin's parameters while the host plays through "
                                  "it, so a track in Write/Touch/Latch records it as automation."));
    writeButton_.onClick = [this] { writeSelectedAsAutomation(); };
    exportButton_.setTooltip(TRANS("Write the selected fix as MIDI CC lanes (.mid) and REAPER envelope chunks "
                                   "into Documents/Khensu Automation."));
    exportButton_.onClick = [this] { exportSelected(); };
    clearButton_.setTooltip(TRANS("Forget the live findings of this instance."));
    clearButton_.onClick = [this] {
        processor_.getIssueTimeline().clear(timeline::IssueSource::Live);
        refresh();
    };
    for (auto* b : {&jumpButton_, &writeButton_, &exportButton_, &clearButton_}) {
        b->setColour(juce::TextButton::buttonColourId, PluginLookAndFeel::bgPanel);
        b->setColour(juce::TextButton::textColourOffId, PluginLookAndFeel::textPrimary);
        addAndMakeVisible(*b);
    }

    statusLabel_.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    statusLabel_.setColour(juce::Label::textColourId, PluginLookAndFeel::textMuted);
    addAndMakeVisible(statusLabel_);

    refresh();
    startTimerHz(5);
}

IssueTimelinePanel::~IssueTimelinePanel() { stopTimer(); }

void IssueTimelinePanel::setStatus(const juce::String& text, juce::Colour colour)
{
    statusLabel_.setColour(juce::Label::textColourId, colour);
    statusLabel_.setText(text, juce::dontSendNotification);
}

void IssueTimelinePanel::refresh()
{
    std::vector<timeline::TimedIssue> all = processor_.getIssueTimeline().mergedSnapshot();
    juce::String source = TRANS("Live analysis of this track");
    if (auto* session = processor_.getAraSession()) {
        auto araIssues = timeline::IssueTimeline::merge(session->regionIssues());
        for (auto& i : araIssues) all.push_back(std::move(i));
        source = session->statusText();
        lastAraGeneration_ = session->generation();
    }
    std::stable_sort(all.begin(), all.end(),
                     [](const timeline::TimedIssue& a, const timeline::TimedIssue& b) { return a.startSec < b.startSec; });

    // Keep the selection on the same finding across rebuilds.
    const int previous = list_.getSelectedRow();
    std::string selectedKey;
    double selectedStart = -1.0;
    if (previous >= 0 && previous < static_cast<int>(issues_.size())) {
        selectedKey = issues_[static_cast<size_t>(previous)].key;
        selectedStart = issues_[static_cast<size_t>(previous)].startSec;
    }
    issues_ = std::move(all);
    lastLiveGeneration_ = processor_.getIssueTimeline().generation();

    double start = 0.0, end = 30.0;
    if (!issues_.empty()) {
        start = issues_.front().startSec;
        for (const auto& i : issues_) end = std::max(end, i.endSec);
        end = std::max(end, start + 5.0);
    }
    strip_.setRange(start, end);
    sourceLabel_.setText(source, juce::dontSendNotification);
    list_.updateContent();
    int reselect = -1;
    for (size_t i = 0; i < issues_.size() && !selectedKey.empty(); ++i)
        if (issues_[i].key == selectedKey && std::abs(issues_[i].startSec - selectedStart) < 5.0) {
            reselect = static_cast<int>(i);
            break;
        }
    if (reselect >= 0)
        list_.selectRow(reselect, true, true);
    else
        list_.deselectAllRows();
    updateButtons();
    repaint();
}

void IssueTimelinePanel::timerCallback()
{
    const bool liveMoved = processor_.getIssueTimeline().generation() != lastLiveGeneration_;
    auto* session = processor_.getAraSession();
    const bool araMoved = session != nullptr && session->generation() != lastAraGeneration_;
    if (liveMoved || araMoved) refresh();
    if (session != nullptr) sourceLabel_.setText(session->statusText(), juce::dontSendNotification);

    strip_.repaint(); // the playhead marker moves

    // Fix-automation progress.
    using State = automation::FixAutomationWriter::State;
    switch (processor_.getFixAutomationState()) {
    case State::Armed:
        writeButton_.setButtonText(TRANS("Cancel automation"));
        break;
    case State::Writing:
        writeButton_.setButtonText(TRANS("Cancel automation"));
        setStatus(TRANS("Writing automation") + " " + juce::String(juce::roundToInt(processor_.getFixAutomationProgress() * 100.0))
                      + "%",
                  PluginLookAndFeel::accentTeal);
        break;
    case State::Finished:
        writeButton_.setButtonText(TRANS("Write as automation"));
        if (!statusLabel_.getText().startsWith(TRANS("Automation written")))
            setStatus(TRANS("Automation written: check the track's automation lanes."), PluginLookAndFeel::statusGood);
        break;
    case State::Idle:
    default: writeButton_.setButtonText(TRANS("Write as automation")); break;
    }
}

void IssueTimelinePanel::updateButtons()
{
    const int row = list_.getSelectedRow();
    const bool has = row >= 0 && row < static_cast<int>(issues_.size());
    jumpButton_.setEnabled(has);
    const bool hasFix = has && !issues_[static_cast<size_t>(row)].suggestedParameters.empty();
    const bool armed = processor_.getFixAutomationState() == automation::FixAutomationWriter::State::Armed
                       || processor_.getFixAutomationState() == automation::FixAutomationWriter::State::Writing;
    writeButton_.setEnabled(hasFix || armed);
    exportButton_.setEnabled(hasFix);
}

void IssueTimelinePanel::selectIssue(int index)
{
    if (index >= 0 && index < static_cast<int>(issues_.size())) list_.selectRow(index);
}

void IssueTimelinePanel::jumpToSelected()
{
    const int row = list_.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(issues_.size())) return;
    const double target = std::max(0.0, issues_[static_cast<size_t>(row)].startSec - kPreRollSec);
    const auto path = processor_.seekHostTo(target);
    if (path == KhensuCopilotProcessor::SeekPath::Ara)
        setStatus(TRANS("Asked the host to move to") + " " + formatTime(target) + " " + TRANS("(ARA)"),
                  PluginLookAndFeel::textMuted);
    else
        setStatus(TRANS("Sent a seek to") + " " + formatTime(target) + " "
                      + TRANS("(MMC Locate - the host must follow MMC on the Khensu MIDI port)"),
                  PluginLookAndFeel::textMuted);
}

void IssueTimelinePanel::writeSelectedAsAutomation()
{
    using State = automation::FixAutomationWriter::State;
    const auto state = processor_.getFixAutomationState();
    if (state == State::Armed || state == State::Writing) {
        processor_.cancelFixAutomation();
        setStatus(TRANS("Automation cancelled."), PluginLookAndFeel::textMuted);
        updateButtons();
        return;
    }
    const int row = list_.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(issues_.size())) return;
    juce::String message;
    const bool ok = processor_.armFixAutomation(issues_[static_cast<size_t>(row)], message);
    setStatus(message, ok ? PluginLookAndFeel::accentTeal : PluginLookAndFeel::statusWarn);
    updateButtons();
}

void IssueTimelinePanel::exportSelected()
{
    const int row = list_.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(issues_.size())) return;
    const auto issue = issues_[static_cast<size_t>(row)];

    auto* params = new juce::DynamicObject();
    for (const auto& [key, value] : issue.suggestedParameters) params->setProperty(juce::Identifier(juce::String(key)), value);
    auto* baseline = new juce::DynamicObject();
    for (const char* id : KhensuCopilotProcessor::kDspParamIds)
        baseline->setProperty(id, processor_.getParamPlain(id));
    auto* fix = new juce::DynamicObject();
    fix->setProperty(af::AutomationFixModel::parameters, juce::var(params));
    fix->setProperty(af::AutomationFixModel::start_sec, issue.startSec);
    fix->setProperty(af::AutomationFixModel::end_sec, issue.endSec);
    fix->setProperty(af::AutomationFixModel::baseline, juce::var(baseline));
    auto* body = new juce::DynamicObject();
    const auto outDir = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                            .getChildFile("Khensu Automation");
    body->setProperty(af::AutomationExportRequest::output_dir, outDir.getFullPathName());
    body->setProperty(af::AutomationExportRequest::name,
                      "khensu_" + juce::String(issue.key) + "_" + juce::String(static_cast<int>(issue.startSec)) + "s");
    body->setProperty(af::AutomationExportRequest::fix, juce::var(fix));
    const double bpm = processor_.getBpm();
    if (bpm >= 20.0 && bpm <= 400.0) body->setProperty(af::AutomationExportRequest::bpm, bpm);
    const juce::var request(body);

    setStatus(TRANS("Exporting..."), PluginLookAndFeel::textMuted);
    juce::Component::SafePointer<IssueTimelinePanel> safe(this);
    processor_.runShortInBackground([safe, request] {
        auto bridge = copilot::ServiceClient::bridge();
        auto result = bridge.postAnalyzeJson("/automation/export", request);
        juce::String text;
        bool ok = false;
        if (result.is_ok()) {
            const auto& v = result.value();
            ok = true;
            text = TRANS("Exported") + " " + v[af::AutomationExportResponse::midi_path].toString();
            if (v[af::AutomationExportResponse::reaper_path].toString().isNotEmpty())
                text << " " << TRANS("and the REAPER envelopes");
        } else {
            text = TRANS("Export failed:") + " " + juce::String(result.error().message);
        }
        juce::MessageManager::callAsync([safe, text, ok] {
            if (safe == nullptr) return;
            safe->setStatus(text, ok ? PluginLookAndFeel::statusGood : PluginLookAndFeel::statusWarn);
        });
    });
}

void IssueTimelinePanel::paint(juce::Graphics& g) { g.fillAll(PluginLookAndFeel::bgDark); }

void IssueTimelinePanel::resized()
{
    auto b = getLocalBounds().reduced(4);
    auto header = b.removeFromTop(22);
    titleLabel_.setBounds(header.removeFromLeft(170));
    liveToggle_.setBounds(header.removeFromLeft(120));
    sourceLabel_.setBounds(header);
    b.removeFromTop(2);
    strip_.setBounds(b.removeFromTop(40));
    b.removeFromTop(4);
    statusLabel_.setBounds(b.removeFromBottom(18));
    auto buttons = b.removeFromBottom(24);
    b.removeFromBottom(4);
    jumpButton_.setBounds(buttons.removeFromLeft(70));
    buttons.removeFromLeft(4);
    writeButton_.setBounds(buttons.removeFromLeft(150));
    buttons.removeFromLeft(4);
    exportButton_.setBounds(buttons.removeFromLeft(150));
    buttons.removeFromLeft(4);
    clearButton_.setBounds(buttons.removeFromLeft(60));
    list_.setBounds(b);
}

// ---- ListBoxModel -------------------------------------------------------------

int IssueTimelinePanel::getNumRows() { return static_cast<int>(issues_.size()); }

void IssueTimelinePanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(issues_.size())) return;
    const auto& issue = issues_[static_cast<size_t>(row)];
    if (selected) g.fillAll(PluginLookAndFeel::accentTeal.withAlpha(0.18f));
    g.setColour(severityColour(issue.severity));
    g.fillRect(2, 4, 4, height - 8);
    g.setFont(khensu::ui::Palette::uiFont(11.0f, juce::Font::plain));
    g.setColour(PluginLookAndFeel::textMuted);
    const auto when = formatTime(issue.startSec) + "-" + formatTime(issue.endSec);
    g.drawText(when, 10, 0, 92, height, juce::Justification::centredLeft, true);
    g.drawText(sourceTag(issue), 104, 0, 70, height, juce::Justification::centredLeft, true);
    g.setColour(PluginLookAndFeel::textPrimary);
    g.drawText(juce::String(juce::CharPointer_UTF8(issue.title.c_str())), 178, 0, width - 182, height,
               juce::Justification::centredLeft, true);
}

void IssueTimelinePanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    list_.selectRow(row);
    jumpToSelected();
}

void IssueTimelinePanel::selectedRowsChanged(int)
{
    updateButtons();
    strip_.repaint();
}

juce::String IssueTimelinePanel::getTooltipForRow(int row)
{
    if (row < 0 || row >= static_cast<int>(issues_.size())) return {};
    const auto& issue = issues_[static_cast<size_t>(row)];
    return juce::String(juce::CharPointer_UTF8(issue.title.c_str())) + "\n"
           + juce::String(juce::CharPointer_UTF8(issue.action.c_str()));
}

// ---- Strip ----------------------------------------------------------------------

double IssueTimelinePanel::Strip::xToTime(float x) const noexcept
{
    const float w = std::max(1.0f, static_cast<float>(getWidth()));
    return start_ + (end_ - start_) * static_cast<double>(x / w);
}

float IssueTimelinePanel::Strip::timeToX(double t) const noexcept
{
    const double span = std::max(1e-6, end_ - start_);
    return static_cast<float>((t - start_) / span * getWidth());
}

void IssueTimelinePanel::Strip::paint(juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    g.setColour(PluginLookAndFeel::bgPanel);
    g.fillRoundedRectangle(bounds, 3.0f);
    g.setColour(PluginLookAndFeel::controlOutline);
    g.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);

    const auto& issues = owner_.issues_;
    const int selected = owner_.list_.getSelectedRow();
    // Lanes by severity so overlapping findings stay visible.
    const float laneH = (bounds.getHeight() - 14.0f) / 3.0f;
    for (size_t i = 0; i < issues.size(); ++i) {
        const auto& issue = issues[i];
        const int lane = issue.severity >= 70.0f ? 0 : (issue.severity >= 40.0f ? 1 : 2);
        const float x0 = timeToX(issue.startSec);
        const float x1 = std::max(x0 + 2.0f, timeToX(issue.endSec));
        const juce::Rectangle<float> r(x0, 2.0f + lane * laneH, x1 - x0, laneH - 1.0f);
        g.setColour(severityColour(issue.severity).withAlpha(static_cast<int>(i) == selected ? 1.0f : 0.65f));
        g.fillRect(r);
        if (static_cast<int>(i) == selected) {
            g.setColour(PluginLookAndFeel::textPrimary);
            g.drawRect(r, 1.0f);
        }
    }

    // Time labels and the host playhead.
    g.setFont(khensu::ui::Palette::uiFont(9.0f, juce::Font::plain));
    g.setColour(PluginLookAndFeel::textMuted);
    const int labelY = getHeight() - 12;
    g.drawText(formatTime(start_), 4, labelY, 60, 12, juce::Justification::centredLeft, false);
    g.drawText(formatTime(end_), getWidth() - 64, labelY, 60, 12, juce::Justification::centredRight, false);
    const double playhead = owner_.processor_.getLiveAnalyzer().lastQueuedTimeSec();
    if (playhead >= start_ && playhead <= end_) {
        const float x = timeToX(playhead);
        g.setColour(PluginLookAndFeel::accentTeal);
        g.drawLine(x, 0.0f, x, static_cast<float>(getHeight()), 1.5f);
    }
}

void IssueTimelinePanel::Strip::mouseUp(const juce::MouseEvent& e)
{
    const double t = xToTime(static_cast<float>(e.x));
    // The most severe finding under the click.
    int best = -1;
    for (size_t i = 0; i < owner_.issues_.size(); ++i) {
        const auto& issue = owner_.issues_[i];
        if (t >= issue.startSec - 0.25 && t <= issue.endSec + 0.25
            && (best < 0 || issue.severity > owner_.issues_[static_cast<size_t>(best)].severity))
            best = static_cast<int>(i);
    }
    if (best >= 0) {
        owner_.selectIssue(best);
        if (e.getNumberOfClicks() > 1) owner_.jumpToSelected();
    }
}

} // namespace khensu
