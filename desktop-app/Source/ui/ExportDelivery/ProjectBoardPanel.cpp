// desktop-app/Source/ui/ExportDelivery/ProjectBoardPanel.cpp
#include "ProjectBoardPanel.h"
#include "../shared/FixDifferenceView.h"
#include "../shared/PanelRunState.h"
#include "../shared/TimelineCache.h"
#include "../../IAppController.h"
#include "../shared/ButtonRole.h"
#include "../shared/ServiceErrorHint.h"
#include "ExportPlan.h"
#include <khensu/ipc/generated/ApiJobStatus.h>

namespace khensu {

void ProjectBoardPanel::applyThemeColours()
{
    projectLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    bulkLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
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
    projectLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(projectLabel_);
    projectBox_.setTitle(TRANS("Project"));
    projectBox_.setTooltip(TRANS("The project this track belongs to. Choosing one moves the track into it."));
    projectBox_.onChange = [this] { onProjectChosen(); };
    addAndMakeVisible(projectBox_);
    addChildComponent(album_);

    newButton_.setTooltip(TRANS("Make a new project with this track as its first track."));
    newButton_.onClick = [this] { promptNewProject(); };
    addAndMakeVisible(newButton_);
    renameButton_.setTooltip(TRANS("Rename this track's project."));
    renameButton_.onClick = [this] { promptRename(); };
    addAndMakeVisible(renameButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    addAndMakeVisible(statusLabel_);

    // ENH-4: bulk actions, one track at a time.
    analyseAllButton_.setTooltip(TRANS("Open and analyse every track of the project in turn, so every row has "
                                       "its numbers. You come back to this track afterwards."));
    analyseAllButton_.onClick = [this] { analyseAll(); };
    addAndMakeVisible(analyseAllButton_);
    fixMasterAllButton_.setTooltip(TRANS("For every track: fix its own findings that have a verified fix (each "
                                         "move kept only when it helped), then master it toward the genre. "
                                         "Each result becomes that track's new version."));
    fixMasterAllButton_.onClick = [this] { fixAndMasterAll(); };
    button_role::markPrimary(fixMasterAllButton_);
    addAndMakeVisible(fixMasterAllButton_);
    exportAllButton_.setTooltip(TRANS("Write every track's current master to one folder, in the Export step's "
                                      "format, as mastered."));
    exportAllButton_.onClick = [this] { exportAll(); };
    addAndMakeVisible(exportAllButton_);
    cancelBulkButton_.setTooltip(TRANS("Stop after the current track."));
    cancelBulkButton_.onClick = [this] { cancelBulk(); };
    addChildComponent(cancelBulkButton_);
    bulkLabel_.setFont(KhensuTheme::labelFont(KhensuTheme::kFontCaption));
    bulkLabel_.setTitle(TRANS("Project bulk action status"));
    addAndMakeVisible(bulkLabel_);

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
    updateBulkButtons();
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
    refreshAlbum(); // VIZ-N2
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
    b.removeFromTop(KhensuTheme::buttonHSmall + KhensuTheme::sp1); // bulk actions (ENH-4)
    b.removeFromTop(16 + KhensuTheme::sp1);                        // bulk status
    if (album_.isVisible()) b.removeFromBottom(juce::jlimit(180, 280, b.getHeight() / 2) + KhensuTheme::sp2);
    return b;
}

void ProjectBoardPanel::refreshAlbum()
{
    album_.setVisible(snapshot_.current.has_value() && snapshot_.tracks.size() >= 2);
    resized();
    if (!album_.isVisible()) return;
    struct Item {
        juce::String name, path;
    };
    std::vector<Item> items;
    for (const auto& t : snapshot_.tracks)
        items.push_back({t.name, t.masterMissing ? juce::String() : t.masterPath});
    album_.setStatus(TRANS("Measuring the album..."));
    const int request = ++albumRequest_;
    fetchQuietly(
        *this, controller_,
        [items] {
            std::vector<album_consistency::TrackData> tracks;
            std::vector<float> bandHz;
            for (const auto& it : items) {
                album_consistency::TrackData d;
                d.name = it.name;
                if (const auto tl = timeline_cache::measure(it.path)) {
                    d.lufs = album_consistency::integratedLufs(*tl);
                    d.truePeak = tl->maxTruePeakDbtp;
                    d.bandDb = fix_difference::longTermBandDb(*tl);
                    if (bandHz.empty()) bandHz = tl->bandHz;
                    if (d.bandDb.size() != bandHz.size()) d.bandDb.clear();
                }
                tracks.push_back(std::move(d));
            }
            return album_consistency::build(std::move(tracks), std::move(bandHz));
        },
        [request](ProjectBoardPanel& self, album_consistency::Model m) {
            if (request != self.albumRequest_) return; // a newer project / track list since
            self.album_.setModel(std::move(m));
        },
        nullptr, true);
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

    const auto font = KhensuTheme::labelFont(type::caption);
    g.setFont(font);
    const int w = area.getWidth();
    // # | track | LUFS | TP | readiness | master | last bulk run (ENH-4)
    const int resultW = juce::jlimit(0, 260, w / 3);
    const int cols[] = {24, juce::jmax(80, w - 24 - 3 * 58 - 80 - resultW), 58, 58, 58, 80, resultW};
    auto cells = [&](juce::Rectangle<int> row, std::initializer_list<juce::String> texts, juce::Colour colour) {
        g.setColour(colour);
        int i = 0;
        for (const auto& t : texts) {
            auto cell = row.removeFromLeft(cols[i++]);
            g.drawFittedText(t, cell.reduced(3, 0), juce::Justification::centredLeft, 1);
        }
    };
    auto header = area.removeFromTop(kRowH);
    cells(header, {"#", TRANS("Track"), "LUFS", "TP", TRANS("Ready"), TRANS("Master"), TRANS("Last run")},
          KhensuTheme::textTertiary);

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
        const auto last = lastResult_.find(t.sessionId);
        const juce::String lastRun = last != lastResult_.end() ? last->second : juce::String();
        cells(row, {juce::String((int)i + 1), t.name, lufs, tp, ready, master, lastRun},
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
    b.removeFromTop(KhensuTheme::sp1);
    row = b.removeFromTop(KhensuTheme::buttonHSmall);
    analyseAllButton_.setBounds(row.removeFromLeft(100));
    row.removeFromLeft(KhensuTheme::sp1);
    fixMasterAllButton_.setBounds(row.removeFromLeft(180));
    row.removeFromLeft(KhensuTheme::sp1);
    exportAllButton_.setBounds(row.removeFromLeft(100));
    row.removeFromLeft(KhensuTheme::sp1);
    if (cancelBulkButton_.isVisible()) cancelBulkButton_.setBounds(row.removeFromLeft(80));
    b.removeFromTop(KhensuTheme::sp1);
    bulkLabel_.setBounds(b.removeFromTop(16));
    if (album_.isVisible()) album_.setBounds(b.removeFromBottom(juce::jlimit(180, 280, b.getHeight() / 2)));
}

// ---- ENH-4: bulk actions ----------------------------------------------------

namespace {
constexpr double kAnalyseTimeoutMs = 15.0 * 60.0 * 1000.0; // a long deep pass, then move on
}

void ProjectBoardPanel::updateBulkButtons()
{
    const bool running = bulk_ != Bulk::None;
    const bool hasTracks = snapshot_.current.has_value() && !snapshot_.tracks.empty();
    for (auto* b : {&analyseAllButton_, &fixMasterAllButton_, &exportAllButton_})
        b->setEnabled(!running && hasTracks);
    if (cancelBulkButton_.isVisible() != running) {
        cancelBulkButton_.setVisible(running);
        resized();
    }
    if (!running && !hasTracks)
        bulkLabel_.setText(TRANS("Put tracks in a project to analyse, fix, master or export them together."),
                           juce::dontSendNotification);
}

bool ProjectBoardPanel::startBulk(Bulk kind)
{
    if (bulk_ != Bulk::None || !snapshot_.current || snapshot_.tracks.empty()) return false;
    bulk_ = kind;
    bulkTracks_ = snapshot_.tracks;
    bulkIndex_ = 0;
    bulkLoaded_ = false;
    bulkReturnSession_ = activeSessionId();
    registerQueue_.clear();
    updateBulkButtons();
    return true;
}

void ProjectBoardPanel::finishBulk(const juce::String& text)
{
    stopTimer();
    const bool switched = bulk_ == Bulk::Analyse || bulk_ == Bulk::Register;
    bulk_ = Bulk::None;
    smartJobId_.clear();
    if (pollStop_) pollStop_->store(true);
    bulkLabel_.setText(text, juce::dontSendNotification);
    // Back to the track the user started from.
    if (switched && !bulkReturnSession_.empty() && activeSessionId() != bulkReturnSession_) {
        controller_.sessions().loadSessionById(bulkReturnSession_);
        controller_.shell().showScreen(ScreenType::Export);
    }
    updateBulkButtons();
    bulkLabel_.setText(text, juce::dontSendNotification);
    refresh();
}

void ProjectBoardPanel::cancelBulk()
{
    if (bulk_ == Bulk::None) return;
    if (smartJobId_.isNotEmpty()) {
        const auto id = smartJobId_;
        controller_.runInBackground([id] { ipc::HttpServiceBridge{}.cancelSmartBatch(id); });
    }
    const size_t total = bulk_ == Bulk::Register ? registerQueue_.size() : bulkTracks_.size();
    finishBulk(TRANS("Stopped after %d of %n tracks.")
                   .replace("%d", juce::String((int)bulkIndex_))
                   .replace("%n", juce::String((int)total)));
}

void ProjectBoardPanel::analyseAll()
{
    if (!startBulk(Bulk::Analyse)) return;
    bulkLabel_.setText(TRANS("Analysing the project's tracks one at a time..."), juce::dontSendNotification);
    startTimer(500);
}

void ProjectBoardPanel::timerCallback()
{
    if (bulk_ == Bulk::Analyse)
        tickAnalyse();
    else if (bulk_ == Bulk::Register)
        tickRegister();
    else
        stopTimer();
}

void ProjectBoardPanel::tickAnalyse()
{
    if (bulkIndex_ >= bulkTracks_.size()) {
        finishBulk(TRANS("Analysed %n tracks.").replace("%n", juce::String((int)bulkTracks_.size())));
        return;
    }
    const auto t = bulkTracks_[bulkIndex_];
    const auto progress = [this, &t](const juce::String& what) {
        bulkLabel_.setText(TRANS("Track %i of %n (%t): ")
                                   .replace("%i", juce::String((int)bulkIndex_ + 1))
                                   .replace("%n", juce::String((int)bulkTracks_.size()))
                                   .replace("%t", t.name) +
                               what,
                           juce::dontSendNotification);
    };
    auto next = [this, &t](const juce::String& outcome) {
        lastResult_[t.sessionId] = outcome;
        ++bulkIndex_;
        bulkLoaded_ = false;
        repaint();
    };
    if (t.masterMissing) return next(TRANS("file missing"));
    if (!bulkLoaded_) {
        bulkLoaded_ = true;
        bulkStepStartMs_ = juce::Time::getMillisecondCounterHiRes();
        progress(TRANS("opening..."));
        if (activeSessionId() != t.sessionId) {
            if (!controller_.sessions().loadSessionById(t.sessionId)) return next(TRANS("could not open"));
            controller_.shell().showScreen(ScreenType::Export); // stay on the board
        } else if (!controller_.analysis().hasAnalysis()) {
            controller_.analysis().reanalyzeActiveSession();
        }
        return;
    }
    if (activeSessionId() != t.sessionId) return next(TRANS("superseded"));
    if (controller_.analysis().hasAnalysis() && !controller_.analysis().resultsPending()) {
        const auto& r = controller_.analysis().currentAnalysis();
        return next(TRANS("analysed: %l LUFS").replace("%l", juce::String(r.loudness.lufs_integrated, 1)));
    }
    if (juce::Time::getMillisecondCounterHiRes() - bulkStepStartMs_ > kAnalyseTimeoutMs)
        return next(TRANS("timed out"));
    progress(TRANS("analysing..."));
}

void ProjectBoardPanel::fixAndMasterAll() { fixAndMasterAll(MasterOptions{}); }

void ProjectBoardPanel::fixAndMasterAll(const MasterOptions& options)
{
    if (!startBulk(Bulk::FixMaster)) return;
    ipc::HttpServiceBridge::SmartBatchRequest req;
    for (const auto& t : bulkTracks_)
        if (!t.masterMissing && t.masterPath.isNotEmpty()) req.filePaths.push_back(t.masterPath);
    if (req.filePaths.empty()) return finishBulk(TRANS("No track has a master on disk."));
    req.fix = options.fix;
    req.master = true;
    req.style = options.style;
    req.strength = options.strength;
    req.loudnessOffsetDb = options.loudnessOffsetDb;
    req.ceilingDbtp = options.ceilingDbtp;
    // One genre for the album: this track's, when its targets are confirmed;
    // otherwise neutral targets (never a genre the user has not confirmed).
    if (const auto* s = controller_.getActiveSession();
        s != nullptr && controller_.analysis().genreRelativeActionAllowed()) {
        req.genreId = juce::String(s->target.primary_genre);
        req.genreActionAllowed = req.genreId.isNotEmpty();
    }
    bulkLabel_.setText((options.label.isNotEmpty() ? options.label + ": " : juce::String()) +
                           TRANS("starting the per-track fix and master..."),
                       juce::dontSendNotification);
    juce::Component::SafePointer<ProjectBoardPanel> self(this);
    controller_.runLongInBackground([self, req] {
        auto started = ipc::HttpServiceBridge{}.startSmartBatch(req);
        juce::MessageManager::callAsync([self, started = std::move(started)] {
            if (self == nullptr || self->bulk_ != Bulk::FixMaster) return;
            if (started.is_err())
                return self->finishBulk(TRANS("Could not start: ") + serviceErrorLine(started.error()));
            self->smartJobId_ = started.value().jobId;
            self->pollSmartBatch(self->smartJobId_);
        });
    });
}

void ProjectBoardPanel::pollSmartBatch(const juce::String& jobId)
{
    if (pollStop_) pollStop_->store(true);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    pollStop_ = stop;
    juce::Component::SafePointer<ProjectBoardPanel> self(this);
    controller_.runLongInBackground([self, jobId, stop] {
        ipc::HttpServiceBridge bridge;
        int misses = 0;
        while (!stop->load()) {
            auto st = bridge.getSmartBatchStatus(jobId);
            if (st.is_err()) {
                if (++misses < 10) {
                    juce::Thread::sleep(2000);
                    continue;
                }
                juce::MessageManager::callAsync([self, jobId, err = st.error()] {
                    if (self != nullptr && self->smartJobId_ == jobId)
                        self->finishBulk(TRANS("Lost the batch: ") + serviceErrorLine(err));
                });
                return;
            }
            misses = 0;
            const auto status = st.value();
            const bool active = khensu::api::isActiveStatus(status.status);
            juce::MessageManager::callAsync([self, jobId, status, active] {
                if (self == nullptr || self->smartJobId_ != jobId || self->bulk_ != Bulk::FixMaster) return;
                if (active) {
                    juce::String text = TRANS("Fix and master: %d of %n done")
                                            .replace("%d", juce::String(status.done))
                                            .replace("%n", juce::String(status.total));
                    if (status.current.isNotEmpty())
                        text << " - " << status.current
                             << (status.stage.isNotEmpty() ? ": " + status.stage : juce::String());
                    self->bulkLabel_.setText(text, juce::dontSendNotification);
                    return;
                }
                self->onSmartBatchFinished(status);
            });
            if (!active) return;
            juce::Thread::sleep(1000);
        }
    });
}

juce::String ProjectBoardPanel::describeResult(const ipc::HttpServiceBridge::SmartBatchFileResult& r)
{
    auto lufs = [](const std::optional<double>& v) { return v ? juce::String(*v, 1) : juce::String("-"); };
    if (r.status == "failed") return TRANS("failed: ") + r.error;
    if (r.status == "unchanged") return TRANS("unchanged (nothing helped safely)");
    juce::String s = lufs(r.beforeLufs) + " -> " + lufs(r.afterLufs) + " LUFS, ";
    s << "TP " << lufs(r.beforeTruePeak) << " -> " << lufs(r.afterTruePeak) << ", ";
    s << TRANS("cleared %c of %b")
             .replace("%c", juce::String(r.findingsCleared))
             .replace("%b", juce::String(r.findingsBefore));
    if (r.mastered) s << ", " << TRANS("mastered");
    return s;
}

void ProjectBoardPanel::onSmartBatchFinished(const ipc::HttpServiceBridge::SmartBatchStatus& st)
{
    smartJobId_.clear();
    if (st.status != khensu::api::job_status::completed)
        return finishBulk(TRANS("Fix and master stopped: ") + (st.error.isNotEmpty() ? st.error : st.status));
    registerQueue_.clear();
    for (const auto& r : st.results)
        for (const auto& t : bulkTracks_)
            if (t.masterPath == r.filePath) {
                lastResult_[t.sessionId] = describeResult(r);
                if (r.status == "completed" && r.outputPath.isNotEmpty())
                    registerQueue_.push_back({t.sessionId, t.masterPath, r.outputPath, describeResult(r)});
                break;
            }
    repaint();
    if (registerQueue_.empty()) return finishBulk(TRANS("Done: no track changed (see Last run)."));
    // Each result becomes its track's new version: that needs its session open.
    bulk_ = Bulk::Register;
    bulkIndex_ = 0;
    bulkLoaded_ = false;
    bulkLabel_.setText(TRANS("Saving each result as its track's new version..."), juce::dontSendNotification);
    startTimer(400);
}

void ProjectBoardPanel::tickRegister()
{
    if (bulkIndex_ >= registerQueue_.size()) {
        finishBulk(TRANS("Fixed and mastered %n tracks; each is its track's current version now.")
                       .replace("%n", juce::String((int)registerQueue_.size())));
        return;
    }
    const auto item = registerQueue_[bulkIndex_];
    if (activeSessionId() != item.sessionId) {
        if (bulkLoaded_ || !controller_.sessions().loadSessionById(item.sessionId)) {
            lastResult_[item.sessionId] = item.summary + " (" + TRANS("not saved: could not open") + ")";
            ++bulkIndex_;
            bulkLoaded_ = false;
            return;
        }
        bulkLoaded_ = true;
        controller_.shell().showScreen(ScreenType::Export);
        return; // register on the next tick, once the session is adopted
    }
    controller_.renders().registerRenderOf(item.outputPath, item.inputPath, juce::String(item.sessionId));
    bulkLabel_.setText(TRANS("Saved %i of %n.")
                           .replace("%i", juce::String((int)bulkIndex_ + 1))
                           .replace("%n", juce::String((int)registerQueue_.size())),
                       juce::dontSendNotification);
    ++bulkIndex_;
    bulkLoaded_ = false;
}

void ProjectBoardPanel::exportAll()
{
    if (bulk_ != Bulk::None || !snapshot_.current || snapshot_.tracks.empty()) return;
    exportChooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Export every track to..."), juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    juce::Component::SafePointer<ProjectBoardPanel> self(this);
    exportChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                [self](const juce::FileChooser& fc) {
                                    if (self == nullptr) return;
                                    const auto dir = fc.getResult();
                                    if (!dir.isDirectory() || !self->startBulk(Bulk::Export)) return;
                                    FormatPanelState fmt;
                                    if (self->exportFormat) fmt = self->exportFormat();
                                    self->exportNext(dir, fmt);
                                });
}

void ProjectBoardPanel::exportNext(juce::File dir, FormatPanelState format)
{
    if (bulk_ != Bulk::Export) return;
    while (bulkIndex_ < bulkTracks_.size() && bulkTracks_[bulkIndex_].masterMissing) {
        lastResult_[bulkTracks_[bulkIndex_].sessionId] = TRANS("not exported: file missing");
        ++bulkIndex_;
    }
    if (bulkIndex_ >= bulkTracks_.size()) {
        finishBulk(TRANS("Exported the project to %d.").replace("%d", dir.getFullPathName()));
        return;
    }
    const auto& t = bulkTracks_[bulkIndex_];
    const juce::String ext = export_plan::extensionFor(format.format);
    const auto out = dir.getNonexistentChildFile(
        juce::File::createLegalFileName(t.name.isNotEmpty() ? t.name : juce::String("track")), ext, false);
    AudioExportOptions opts;
    opts.sourcePath = t.masterPath;
    opts.outputPath = out.getFullPathName();
    opts.format = format.format;
    opts.bitDepth = format.bitDepth;
    opts.sampleRate = format.sampleRate;
    opts.mp3Bitrate = format.mp3Bitrate;
    opts.gainLinear = 1.0f; // as mastered
    bulkLabel_.setText(TRANS("Exporting %i of %n: %t")
                           .replace("%i", juce::String((int)bulkIndex_ + 1))
                           .replace("%n", juce::String((int)bulkTracks_.size()))
                           .replace("%t", t.name),
                       juce::dontSendNotification);
    juce::Component::SafePointer<ProjectBoardPanel> self(this);
    const std::string sessionId = t.sessionId;
    controller_.renders().exportAudio(opts, [self, dir, format, sessionId](bool ok, juce::String msg) {
        if (self == nullptr || self->bulk_ != Bulk::Export) return;
        self->lastResult_[sessionId] =
            ok ? TRANS("exported: ") + juce::File(msg).getFileName() : TRANS("export failed: ") + msg;
        self->repaint();
        ++self->bulkIndex_;
        self->exportNext(dir, format);
    });
}

} // namespace khensu
