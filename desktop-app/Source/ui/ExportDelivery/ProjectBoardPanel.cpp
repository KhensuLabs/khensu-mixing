// desktop-app/Source/ui/ExportDelivery/ProjectBoardPanel.cpp
#include "ProjectBoardPanel.h"
#include "../../IAppController.h"
#include "../shared/ButtonRole.h"

namespace khensu {

void ProjectBoardPanel::applyThemeColours()
{
    projectLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

ProjectBoardPanel::ProjectBoardPanel(IAppController& c) : PanelCard(c, TRANS("Project"))
{
    applyThemeColours();
    setExplainer(TRANS("Project"),
                 TRANS("An album, EP or batch is one project: a session per track, in album order. The board "
                       "shows every track's loudness, true peak and readiness, and which master the album "
                       "uses - the version you accepted, else the mix. Click a track to open it. Album "
                       "Assembly and the album master use these masters in this order."));

    projectLabel_.setText(TRANS("This track's project:"), juce::dontSendNotification);
    projectLabel_.setFont(KhensuTheme::labelFont(11.0f));
    addAndMakeVisible(projectLabel_);
    projectBox_.setTitle(TRANS("Project"));
    projectBox_.setTooltip(TRANS("The project this track belongs to. Choosing one moves the track into it."));
    projectBox_.onChange = [this] { onProjectChosen(); };
    addAndMakeVisible(projectBox_);

    newButton_.setTooltip(TRANS("Make a new project with this track as its first track."));
    newButton_.onClick = [this] { promptNewProject(); };
    addAndMakeVisible(newButton_);
    renameButton_.setTooltip(TRANS("Rename this track's project."));
    renameButton_.onClick = [this] { promptRename(); };
    addAndMakeVisible(renameButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    addAndMakeVisible(statusLabel_);

    projectsListener_ = listen(controller_.sessions(), this, &ISessions::addProjectsListener,
                               &ISessions::removeProjectsListener, [this] { refresh(); });
    // A finished analysis changes this track's row - and is the first sign of
    // a newly opened session (resetForSession runs before it is adopted).
    analysisListener_ = listen(controller_.analysis(), this, &IAnalysisHub::addAnalysisListener,
                               &IAnalysisHub::removeAnalysisListener, [this](const AnalysisResult&) { refresh(); });
    refresh();
}

std::string ProjectBoardPanel::activeSessionId() const
{
    const auto* s = controller_.getActiveSession();
    return s != nullptr ? s->id : std::string();
}

void ProjectBoardPanel::visibilityChanged()
{
    if (isShowing()) refresh();
}

void ProjectBoardPanel::refresh()
{
    auto db = controller_.sessions().sessionManagerPtr();
    if (db == nullptr) return;
    const auto sessionId = activeSessionId();
    const int request = ++request_;
    if (snapshot_.tracks.empty())
        statusLabel_.setText(sessionId.empty() ? TRANS("Open a track to group it into a project.")
                                               : TRANS("Reading this track's project..."),
                             juce::dontSendNotification);
    juce::Component::SafePointer<ProjectBoardPanel> self(this);
    controller_.runInBackground([self, db, sessionId, request] {
        auto snap = project_tracks::load(*db, sessionId);
        juce::MessageManager::callAsync([self, snap = std::move(snap), sessionId, request]() mutable {
            if (self == nullptr || request != self->request_) return; // a newer read is on its way
            self->publish(std::move(snap), sessionId);
        });
    });
}

void ProjectBoardPanel::publish(project_tracks::Snapshot snap, const std::string& forSession)
{
    // The session changed while this was read: read again for the new one.
    if (forSession != activeSessionId()) return refresh();
    snapshot_ = std::move(snap);
    rebuildProjectBox();
    const bool hasSession = !forSession.empty();
    projectBox_.setEnabled(hasSession);
    newButton_.setEnabled(hasSession);
    renameButton_.setEnabled(snapshot_.current.has_value());
    if (!hasSession)
        statusLabel_.setText(TRANS("Open a track to group it into a project."), juce::dontSendNotification);
    else if (!snapshot_.current)
        statusLabel_.setText(TRANS("Not in a project. Pick one, or make one with New project..."),
                             juce::dontSendNotification);
    else {
        int ready = 0;
        for (const auto& t : snapshot_.tracks)
            if (t.summary && t.summary->readiness_score >= 0.0f && !t.masterMissing) ++ready;
        statusLabel_.setText(TRANS("%n tracks, %r analysed. Click a track to open it.")
                                 .replace("%n", juce::String((int)snapshot_.tracks.size()))
                                 .replace("%r", juce::String(ready)),
                             juce::dontSendNotification);
    }
    repaint();
}

void ProjectBoardPanel::rebuildProjectBox()
{
    projectBox_.clear(juce::dontSendNotification);
    projectBox_.addItem(TRANS("No project"), kNoProjectId);
    for (size_t i = 0; i < snapshot_.projects.size(); ++i) {
        const auto& p = snapshot_.projects[i];
        projectBox_.addItem(juce::String(p.name) + " (" + juce::String((int)p.session_ids.size()) + ")",
                            kFirstProjectId + (int)i);
        if (snapshot_.current && snapshot_.current->id == p.id)
            projectBox_.setSelectedId(kFirstProjectId + (int)i, juce::dontSendNotification);
    }
    if (!snapshot_.current) projectBox_.setSelectedId(kNoProjectId, juce::dontSendNotification);
}

void ProjectBoardPanel::change(std::function<void(SessionManager&)> write)
{
    auto db = controller_.sessions().sessionManagerPtr();
    if (db == nullptr) return;
    juce::Component::SafePointer<ProjectBoardPanel> self(this);
    controller_.runInBackground([self, db, write = std::move(write)] {
        write(*db);
        juce::MessageManager::callAsync([self] {
            if (self != nullptr) self->controller_.sessions().notifyProjectsChanged();
        });
    });
}

void ProjectBoardPanel::onProjectChosen()
{
    const auto sessionId = activeSessionId();
    if (sessionId.empty()) return;
    const int id = projectBox_.getSelectedId();
    if (id == kNoProjectId) {
        change([sessionId](SessionManager& db) { db.removeSessionFromProject(sessionId); });
        return;
    }
    const int index = id - kFirstProjectId;
    if (index < 0 || index >= (int)snapshot_.projects.size()) return;
    const auto projectId = snapshot_.projects[(size_t)index].id;
    change([projectId, sessionId](SessionManager& db) { db.addSessionToProject(projectId, sessionId); });
}

void ProjectBoardPanel::promptNewProject()
{
    const auto sessionId = activeSessionId();
    if (sessionId.empty()) return;
    dialog_ = std::make_unique<juce::AlertWindow>(TRANS("New project"),
                                                  TRANS("Name the album, EP or project. This track becomes its "
                                                        "first track; add the others from their sessions."),
                                                  juce::MessageBoxIconType::NoIcon, this);
    dialog_->addTextEditor("name", {}, TRANS("Name"));
    dialog_->addButton(TRANS("Create"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ProjectBoardPanel> self(this);
    dialog_->enterModalState(true, juce::ModalCallbackFunction::create([self, sessionId](int r) {
                                 if (self == nullptr || self->dialog_ == nullptr) return;
                                 const auto name = self->dialog_->getTextEditorContents("name").trim().toStdString();
                                 self->dialog_.reset();
                                 if (r != 1 || name.empty()) return;
                                 self->change([name, sessionId](SessionManager& db) {
                                     if (auto p = db.createProject(name); p.is_ok())
                                         db.addSessionToProject(p.value().id, sessionId);
                                 });
                             }));
}

void ProjectBoardPanel::promptRename()
{
    if (!snapshot_.current) return;
    const auto projectId = snapshot_.current->id;
    dialog_ = std::make_unique<juce::AlertWindow>(TRANS("Rename project"), juce::String(),
                                                  juce::MessageBoxIconType::NoIcon, this);
    dialog_->addTextEditor("name", juce::String(snapshot_.current->name), TRANS("Name"));
    dialog_->addButton(TRANS("Rename"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ProjectBoardPanel> self(this);
    dialog_->enterModalState(true, juce::ModalCallbackFunction::create([self, projectId](int r) {
                                 if (self == nullptr || self->dialog_ == nullptr) return;
                                 const auto name = self->dialog_->getTextEditorContents("name").trim().toStdString();
                                 self->dialog_.reset();
                                 if (r != 1 || name.empty()) return;
                                 self->change(
                                     [projectId, name](SessionManager& db) { db.renameProject(projectId, name); });
                             }));
}

juce::Rectangle<int> ProjectBoardPanel::listArea() const
{
    auto b = bodyArea();
    b.removeFromTop(KhensuTheme::buttonHSmall + KhensuTheme::sp2); // project row
    b.removeFromTop(16 + KhensuTheme::sp1);                        // status line
    return b;
}

int ProjectBoardPanel::rowAt(juce::Point<int> p) const
{
    const auto area = listArea().withTrimmedTop(kRowH); // under the column header
    if (!area.contains(p)) return -1;
    const int row = (p.y - area.getY()) / kRowH;
    return row < (int)snapshot_.tracks.size() ? row : -1;
}

void ProjectBoardPanel::mouseMove(const juce::MouseEvent& e)
{
    const int row = rowAt(e.getPosition());
    if (row != hoverRow_) {
        hoverRow_ = row;
        repaint();
    }
}

void ProjectBoardPanel::mouseDown(const juce::MouseEvent& e)
{
    const int row = rowAt(e.getPosition());
    if (row < 0) return PanelCard::mouseDown(e);
    const auto& t = snapshot_.tracks[(size_t)row];
    if (t.sessionId != activeSessionId()) controller_.sessions().loadSessionById(t.sessionId);
}

void ProjectBoardPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto area = listArea();
    if (!snapshot_.current || area.getHeight() < kRowH) return;

    const auto font = KhensuTheme::labelFont(10.5f);
    g.setFont(font);
    const int w = area.getWidth();
    // # | track | LUFS | TP | readiness | master
    const int cols[] = {24, juce::jmax(80, w - 24 - 3 * 58 - 90), 58, 58, 58, 90};
    auto cells = [&](juce::Rectangle<int> row, std::initializer_list<juce::String> texts, juce::Colour colour) {
        g.setColour(colour);
        int i = 0;
        for (const auto& t : texts) {
            auto cell = row.removeFromLeft(cols[i++]);
            g.drawFittedText(t, cell.reduced(3, 0), juce::Justification::centredLeft, 1);
        }
    };
    auto header = area.removeFromTop(kRowH);
    cells(header, {"#", TRANS("Track"), "LUFS", "TP", TRANS("Ready"), TRANS("Master")}, KhensuTheme::textTertiary);

    const auto active = activeSessionId();
    for (size_t i = 0; i < snapshot_.tracks.size() && area.getHeight() >= kRowH; ++i) {
        const auto& t = snapshot_.tracks[i];
        auto row = area.removeFromTop(kRowH);
        if (t.sessionId == active) {
            g.setColour(KhensuTheme::accentTealDim);
            g.fillRect(row);
        } else if ((int)i == hoverRow_) {
            g.setColour(KhensuTheme::bgOverlay);
            g.fillRect(row);
        }
        const auto& s = t.summary;
        const juce::String lufs = s ? juce::String(s->lufs_integrated, 1) : juce::String("-");
        const juce::String tp = s ? juce::String(s->true_peak_dbtp, 1) : juce::String("-");
        const juce::String ready =
            s && s->readiness_score >= 0.0f ? juce::String(juce::roundToInt(s->readiness_score)) : juce::String("-");
        const juce::String master = t.masterMissing    ? TRANS("missing")
                                    : t.masterIsRender ? TRANS("accepted")
                                                       : TRANS("mix");
        cells(row, {juce::String((int)i + 1), t.name, lufs, tp, ready, master},
              t.masterMissing ? KhensuTheme::accentAmber : KhensuTheme::textPrimary);
    }
}

void ProjectBoardPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto row = b.removeFromTop(KhensuTheme::buttonHSmall);
    projectLabel_.setBounds(row.removeFromLeft(130));
    renameButton_.setBounds(row.removeFromRight(84));
    row.removeFromRight(KhensuTheme::sp1);
    newButton_.setBounds(row.removeFromRight(110));
    row.removeFromRight(KhensuTheme::sp1);
    projectBox_.setBounds(row);
    b.removeFromTop(KhensuTheme::sp2);
    statusLabel_.setBounds(b.removeFromTop(16));
}

} // namespace khensu
