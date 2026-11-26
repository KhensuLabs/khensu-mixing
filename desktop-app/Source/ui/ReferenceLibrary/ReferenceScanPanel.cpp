// desktop-app/Source/ui/ReferenceLibrary/ReferenceScanPanel.cpp
#include "ReferenceScanPanel.h"
#include "../../IAppController.h"
#include "../shared/ButtonRole.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

// Every service call here runs on the long pool (runLongInBackground): the
// short pool is full of the Dashboard's follow-up scans while a track is
// analysed, and library reads queued behind them for minutes (UXR-4).

juce::String referenceGenreName(const GenreDatabase& db, const juce::String& id)
{
    if (id.isEmpty()) return {};
    if (auto e = db.findById(id.toStdString())) return juce::String(e->name);
    // A family that is not itself a catalog genre ("hip-hop", "electronic").
    juce::StringArray parts;
    parts.addTokens(id, "-", "");
    for (auto& p : parts)
        p = p.substring(0, 1).toUpperCase() + p.substring(1);
    return parts.joinIntoString("-");
}

namespace {
constexpr int kRowH = 52;
constexpr int kThumb = 40;
constexpr size_t kMaxThumbs = 400;

juce::String sourceText(const juce::String& source)
{
    if (source == "tag") return TRANS("from the file's genre tag");
    if (source == "folder") return TRANS("from the folder name");
    if (source == "pack") return TRANS("from its pack");
    if (source == "probe") return TRANS("from the genre model");
    if (source == "ranker") return TRANS("a guess from the sound");
    if (source == "user") return TRANS("set by you");
    return {};
}
} // namespace

void ReferenceScanPanel::applyThemeColours()
{
    folderCaption_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    folderPath_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    summaryLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    rescanToggle_.setColour(juce::ToggleButton::textColourId, KhensuTheme::textSecondary);
    rescanToggle_.setColour(juce::ToggleButton::tickColourId, KhensuTheme::accentTeal);
    list_.setColour(juce::ListBox::backgroundColourId, KhensuTheme::bgSurface);
    searchEditor_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgBase);
    searchEditor_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    searchEditor_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    searchEditor_.setTextToShowWhenEmpty(TRANS("Search title, artist, pack..."), KhensuTheme::textTertiary);
    progressBar_.setColour(juce::ProgressBar::foregroundColourId, KhensuTheme::accentTeal);
    progressBar_.setColour(juce::ProgressBar::backgroundColourId, KhensuTheme::bgBase);
    for (auto* tab : {&songsTab_, &samplesTab_, &packsTab_}) {
        tab->setColour(juce::TextButton::buttonOnColourId, KhensuTheme::accentTeal.withAlpha(0.25f));
        tab->setColour(juce::TextButton::textColourOnId, KhensuTheme::textPrimary);
        tab->setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    }
}

ReferenceScanPanel::ReferenceScanPanel(IAppController& controller, std::string baseUrl)
    : controller_(controller), baseUrl_(std::move(baseUrl))
{
    applyThemeColours();
    folderCaption_.setText(TRANS("Reference folder:"), juce::dontSendNotification);
    folderCaption_.setFont(KhensuTheme::labelFont(type::secondary));
    addAndMakeVisible(folderCaption_);
    folderPath_.setFont(KhensuTheme::bodyFont(type::secondary));
    folderPath_.setText(TRANS("none chosen yet"), juce::dontSendNotification);
    folderPath_.setMinimumHorizontalScale(0.8f);
    addAndMakeVisible(folderPath_);

    chooseButton_.setTooltip(TRANS("Pick the folder to scan for references - your music folder or a sample library"));
    chooseButton_.onClick = [this] { chooseFolder(); };
    addAndMakeVisible(chooseButton_);
    scanButton_.setTooltip(TRANS("Scan the folder again: new and changed files are read, removed ones forgotten"));
    scanButton_.onClick = [this] { startScan(); };
    button_role::markPrimary(scanButton_);
    addAndMakeVisible(scanButton_);
    cancelButton_.setTooltip(TRANS("Stop the scan (what was read so far is kept)"));
    cancelButton_.onClick = [this] { cancelScan(); };
    addChildComponent(cancelButton_);
    forgetButton_.setTooltip(TRANS("Stop using this folder and forget what was found in it (your files stay)"));
    forgetButton_.onClick = [this] { forgetFolder(); };
    addChildComponent(forgetButton_);
    rescanToggle_.setTooltip(TRANS("Scan the folder for new files each time Khensu starts"));
    rescanToggle_.onClick = [this] { setRescanOnLaunch(rescanToggle_.getToggleState()); };
    addAndMakeVisible(rescanToggle_);
    addChildComponent(progressBar_);

    summaryLabel_.setFont(KhensuTheme::labelFont(type::secondary));
    addAndMakeVisible(summaryLabel_);
    statusLabel_.setFont(KhensuTheme::bodyFont(type::caption));
    statusLabel_.setMinimumHorizontalScale(0.8f);
    addAndMakeVisible(statusLabel_);

    int tabId = 0;
    for (auto* tab : {&songsTab_, &samplesTab_, &packsTab_}) {
        tab->setClickingTogglesState(true);
        tab->setRadioGroupId(0x5245);
        const View v = tabId == 0 ? View::Songs : (tabId == 1 ? View::Samples : View::Packs);
        tab->onClick = [this, v] { setView(v); };
        addAndMakeVisible(*tab);
        ++tabId;
    }
    songsTab_.setTooltip(TRANS("Full songs - use one as the session's reference"));
    samplesTab_.setTooltip(TRANS("Loops and one-shots from your sample packs"));
    packsTab_.setTooltip(TRANS("Sample and construction packs, one row each"));
    songsTab_.setToggleState(true, juce::dontSendNotification);

    searchEditor_.setTitle(TRANS("Search the scanned references"));
    searchEditor_.setTooltip(TRANS("Filter by title, artist, author, pack or album"));
    searchEditor_.onTextChange = [this] {
        startTimer(250);
        listDirty_ = true;
    };
    addAndMakeVisible(searchEditor_);

    list_.setModel(this);
    list_.setRowHeight(kRowH);
    list_.setTitle(TRANS("Scanned references"));
    addAndMakeVisible(list_);

    useButton_.setTooltip(TRANS("Make the selected song the session's reference track"));
    useButton_.onClick = [this] { useSelectedAsReference(); };
    genreButton_.setTooltip(TRANS("Give the selected item (or its whole pack) another genre"));
    genreButton_.onClick = [this] { correctSelectedGenre(); };
    revealButton_.setTooltip(TRANS("Show the selected file in Explorer"));
    revealButton_.onClick = [this] {
        const int r = list_.getSelectedRow();
        if (r < 0 || r >= (int)rows_.size()) return;
        const auto& row = rows_[(size_t)r];
        juce::File(row.path.isNotEmpty() ? row.path : row.packPath).revealToUser();
    };
    for (auto* b : {&useButton_, &genreButton_, &revealButton_})
        addAndMakeVisible(*b);

    addChildComponent(emptyState_);
    updateButtons();
    updateEmptyState();
}

ReferenceScanPanel::~ReferenceScanPanel()
{
    stopTimer();
    list_.setModel(nullptr);
}

// --- data ------------------------------------------------------------------------

ReferenceScanPanel::Row ReferenceScanPanel::rowFromItem(const juce::var& v)
{
    Row r;
    r.id = v[af::ReferenceScanItem::id].toString();
    r.path = v[af::ReferenceScanItem::path].toString();
    r.title = v[af::ReferenceScanItem::title].toString();
    r.author = v[af::ReferenceScanItem::author].toString();
    r.album = v[af::ReferenceScanItem::album].toString();
    r.pack = v[af::ReferenceScanItem::pack].toString();
    r.packPath = v[af::ReferenceScanItem::pack_path].toString();
    r.genre = v[af::ReferenceScanItem::genre].toString();
    r.family = v[af::ReferenceScanItem::family].toString();
    r.source = v[af::ReferenceScanItem::genre_source].toString();
    r.kind = v[af::ReferenceScanItem::kind].toString();
    r.sampleType = v[af::ReferenceScanItem::sample_type].toString();
    r.key = v[af::ReferenceScanItem::key].toString();
    r.artworkPath = v[af::ReferenceScanItem::artwork_path].toString();
    r.confidence = static_cast<float>((double)v[af::ReferenceScanItem::genre_confidence]);
    r.bpm = (double)v[af::ReferenceScanItem::bpm];
    r.durationS = (double)v[af::ReferenceScanItem::duration_s];
    r.exists = (bool)v[af::ReferenceScanItem::exists];
    return r;
}

ReferenceScanPanel::Row ReferenceScanPanel::rowFromPack(const juce::var& v)
{
    Row r;
    r.kind = "pack";
    r.title = v[af::ReferenceScanPack::pack].toString();
    r.pack = r.title;
    r.packPath = v[af::ReferenceScanPack::pack_path].toString();
    r.author = v[af::ReferenceScanPack::author].toString();
    r.genre = v[af::ReferenceScanPack::genre].toString();
    r.family = v[af::ReferenceScanPack::family].toString();
    r.artworkPath = v[af::ReferenceScanPack::artwork_path].toString();
    r.samples = (int)v[af::ReferenceScanPack::samples];
    r.loops = (int)v[af::ReferenceScanPack::loops];
    return r;
}

ipc::HttpServiceBridge::ReferenceScanFilter ReferenceScanPanel::currentFilter() const
{
    ipc::HttpServiceBridge::ReferenceScanFilter f;
    f.genre = scope_.genre;
    f.family = scope_.family;
    f.unsorted = scope_.unsorted;
    f.text = searchEditor_.getText().trim();
    if (view_ == View::Songs) f.kind = "song";
    if (view_ == View::Samples) f.kind = "sample";
    f.limit = 500;
    return f;
}

void ReferenceScanPanel::setGenreScope(const GenreScope& scope)
{
    if (scope == scope_ && loadedOnce_) return;
    scope_ = scope;
    loadList();
}

void ReferenceScanPanel::refresh()
{
    loadRootsAndSummary();
    loadList();
    // A scan already running (the launch rescan, or one started before the
    // screen was rebuilt): follow it.
    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    controller_.runLongInBackground([self, baseUrl = baseUrl_] {
        auto result = ipc::HttpServiceBridge(baseUrl).referenceScanCurrent();
        juce::MessageManager::callAsync([self, result] {
            if (self == nullptr || result.is_err()) return;
            // The library is per machine, not per session: no epoch check.
            const juce::var v = result.value();
            const juce::String status = v[af::ReferenceScanCurrentResponse::status].toString();
            if (api::isActiveStatus(status)) self->followJob(v[af::ReferenceScanCurrentResponse::job_id].toString());
        });
    });
}

void ReferenceScanPanel::loadRootsAndSummary()
{
    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    controller_.runLongInBackground([self, baseUrl = baseUrl_] {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto roots = bridge.referenceScanRoots();
        auto summary = bridge.referenceScanSummary();
        juce::MessageManager::callAsync([self, roots, summary] {
            if (self == nullptr) return;
            // The library is per machine, not per session: no epoch check.
            if (roots.is_err()) {
                self->statusLabel_.setText(classifyServiceError(roots.error()).message, juce::dontSendNotification);
                return;
            }
            self->rootPath_.clear();
            bool rescan = false;
            if (auto* arr = roots.value()[af::ReferenceScanRootsResponse::roots].getArray())
                if (!arr->isEmpty()) {
                    const auto& r = arr->getReference(0);
                    self->rootPath_ = r[af::ReferenceScanRoot::path].toString();
                    rescan = (bool)r[af::ReferenceScanRoot::rescan_on_launch];
                    if (!(bool)r[af::ReferenceScanRoot::exists])
                        self->statusLabel_.setText(TRANS("The reference folder is missing - choose it again."),
                                                   juce::dontSendNotification);
                }
            self->folderPath_.setText(self->rootPath_.isEmpty() ? TRANS("none chosen yet") : self->rootPath_,
                                      juce::dontSendNotification);
            self->folderPath_.setTooltip(self->rootPath_);
            self->rescanToggle_.setToggleState(rescan, juce::dontSendNotification);
            if (summary.is_ok()) {
                const juce::var s = summary.value();
                self->songs_ = (int)s[af::ReferenceScanSummaryResponse::songs];
                self->samples_ = (int)s[af::ReferenceScanSummaryResponse::samples];
                self->packs_ = (int)s[af::ReferenceScanSummaryResponse::packs];
                if (self->onSummary) self->onSummary(s);
            }
            if (!self->scanning_)
                self->summaryLabel_.setText(self->rootPath_.isEmpty() && self->songs_ + self->samples_ == 0
                                                ? juce::String()
                                                : TRANS("%s songs, %m samples, %k packs found")
                                                      .replace("%s", juce::String(self->songs_))
                                                      .replace("%m", juce::String(self->samples_))
                                                      .replace("%k", juce::String(self->packs_)),
                                            juce::dontSendNotification);
            self->updateButtons();
            self->updateEmptyState();
        });
    });
}

void ReferenceScanPanel::loadList()
{
    if (listInFlight_) {
        listDirty_ = true;
        return;
    }
    listInFlight_ = true;
    listDirty_ = false;
    const auto filter = currentFilter();
    const bool packs = view_ == View::Packs;
    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    controller_.runLongInBackground([self, filter, packs, baseUrl = baseUrl_] {
        ipc::HttpServiceBridge bridge(baseUrl);
        auto result = packs ? bridge.referenceScanPacks(filter) : bridge.referenceScanItems(filter);
        juce::MessageManager::callAsync([self, result, packs] {
            if (self == nullptr) return;
            // The library is per machine, not per session: no epoch check.
            self->listInFlight_ = false;
            if (result.is_err()) {
                // The engine is starting or busy: say so and ask again shortly
                // (a few times), instead of an empty pane.
                if (ipc::engineDidNotAnswer(result.error()) && self->listRetries_ < 6) {
                    ++self->listRetries_;
                    self->statusLabel_.setText(TRANS("Waiting for the analysis engine..."), juce::dontSendNotification);
                    juce::Timer::callAfterDelay(5000, [self] {
                        if (self != nullptr && self->isShowing()) self->refresh();
                    });
                    return;
                }
                self->loadedOnce_ = true;
                self->statusLabel_.setText(classifyServiceError(result.error()).message, juce::dontSendNotification);
            } else if ((self->view_ == View::Packs) == packs) {
                self->loadedOnce_ = true;
                self->listRetries_ = 0;
                const juce::var v = result.value();
                self->rows_.clear();
                if (packs) {
                    if (auto* arr = v[af::ReferenceScanPacksResponse::packs].getArray())
                        for (const auto& p : *arr)
                            self->rows_.push_back(rowFromPack(p));
                    self->listTotal_ = (int)self->rows_.size();
                } else {
                    if (auto* arr = v[af::ReferenceScanItemsResponse::items].getArray())
                        for (const auto& it : *arr)
                            self->rows_.push_back(rowFromItem(it));
                    self->listTotal_ = (int)v[af::ReferenceScanItemsResponse::total];
                }
                self->list_.updateContent();
                self->list_.deselectAllRows();
                if (!self->scanning_ && self->listTotal_ > (int)self->rows_.size())
                    self->statusLabel_.setText(TRANS("Showing the first %n of %t - search to narrow it down.")
                                                   .replace("%n", juce::String((int)self->rows_.size()))
                                                   .replace("%t", juce::String(self->listTotal_)),
                                               juce::dontSendNotification);
                else if (!self->scanning_)
                    self->statusLabel_.setText({}, juce::dontSendNotification);
            }
            self->updateButtons();
            self->updateEmptyState();
            self->repaint();
            if (self->listDirty_) self->loadList();
        });
    });
}

// --- the scan ------------------------------------------------------------------------

void ReferenceScanPanel::chooseFolder()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Choose your reference folder"), rootPath_.isNotEmpty()
                                                   ? juce::File(rootPath_)
                                                   : juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [self](const juce::FileChooser& fc) {
                              if (self == nullptr) return;
                              const juce::File dir = fc.getResult();
                              if (!dir.isDirectory()) return;
                              const bool rescan = self->rescanToggle_.getToggleState();
                              self->statusLabel_.setText(TRANS("Saving the folder..."), juce::dontSendNotification);
                              self->controller_.runLongInBackground(
                                  [self, path = dir.getFullPathName(), rescan, baseUrl = self->baseUrl_] {
                                      auto result =
                                          ipc::HttpServiceBridge(baseUrl).referenceScanSetRoot(path, rescan, true);
                                      juce::MessageManager::callAsync([self, result] {
                                          if (self == nullptr) return;
                                          // The library is per machine, not per session: no epoch check.
                                          if (result.is_err()) {
                                              self->statusLabel_.setText(classifyServiceError(result.error()).message,
                                                                         juce::dontSendNotification);
                                              return;
                                          }
                                          self->rootPath_ = result.value()[af::ReferenceScanRoot::path].toString();
                                          self->folderPath_.setText(self->rootPath_, juce::dontSendNotification);
                                          self->folderPath_.setTooltip(self->rootPath_);
                                          self->startScan(); // a new folder is scanned straight away
                                      });
                                  });
                          });
}

void ReferenceScanPanel::startScan()
{
    if (scanning_) return;
    if (rootPath_.isEmpty()) {
        chooseFolder();
        return;
    }
    scanning_ = true;
    updateButtons();
    statusLabel_.setText(TRANS("Starting the scan..."), juce::dontSendNotification);
    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    controller_.runLongInBackground([self, baseUrl = baseUrl_] {
        auto result = ipc::HttpServiceBridge(baseUrl).referenceScanStart(true);
        juce::MessageManager::callAsync([self, result] {
            if (self == nullptr) return;
            // The library is per machine, not per session: no epoch check.
            if (result.is_err()) {
                self->scanning_ = false;
                self->updateButtons();
                self->statusLabel_.setText(classifyServiceError(result.error()).message, juce::dontSendNotification);
                return;
            }
            self->followJob(result.value()[af::ReferenceScanStartResponse::job_id].toString());
        });
    });
}

void ReferenceScanPanel::followJob(const juce::String& jobId)
{
    if (jobId.isEmpty()) return;
    jobId_ = jobId;
    scanning_ = true;
    progress_ = -1.0; // indeterminate until the walk has counted the files
    progressBar_.setVisible(true);
    updateButtons();
    resized();
    startTimer(500);
}

void ReferenceScanPanel::cancelScan()
{
    if (jobId_.isEmpty()) return;
    statusLabel_.setText(TRANS("Stopping the scan..."), juce::dontSendNotification);
    controller_.runLongInBackground(
        [jobId = jobId_, baseUrl = baseUrl_] { (void)ipc::HttpServiceBridge(baseUrl).referenceScanCancel(jobId); });
}

void ReferenceScanPanel::forgetFolder()
{
    if (rootPath_.isEmpty() || scanning_) return;
    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    juce::AlertWindow::showOkCancelBox(
        juce::AlertWindow::QuestionIcon, TRANS("Forget the reference folder?"),
        TRANS("Khensu stops scanning %p and forgets the songs and samples it found there. "
              "The files themselves are not touched.")
            .replace("%p", rootPath_),
        TRANS("Forget"), TRANS("Cancel"), this, juce::ModalCallbackFunction::create([self](int result) {
            if (self == nullptr || result != 1) return; // 1 = "Forget"
            self->controller_.runLongInBackground([self, path = self->rootPath_, baseUrl = self->baseUrl_] {
                auto r = ipc::HttpServiceBridge(baseUrl).referenceScanRemoveRoot(path, true);
                juce::MessageManager::callAsync([self, r] {
                    if (self == nullptr) return;
                    // The library is per machine, not per session: no epoch check.
                    self->statusLabel_.setText(
                        r.is_err()
                            ? classifyServiceError(r.error()).message
                            : TRANS("Folder forgotten (%n items).")
                                  .replace("%n",
                                           juce::String(
                                               (int)r.value()[af::ReferenceScanRootRemoveResponse::forgotten_items])),
                        juce::dontSendNotification);
                    self->loadRootsAndSummary();
                    self->loadList();
                });
            });
        }));
}

void ReferenceScanPanel::setRescanOnLaunch(bool on)
{
    if (rootPath_.isEmpty()) return; // saved with the folder when one is chosen
    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    controller_.runLongInBackground([self, on, path = rootPath_, baseUrl = baseUrl_] {
        auto result = ipc::HttpServiceBridge(baseUrl).referenceScanSetRoot(path, on, false);
        juce::MessageManager::callAsync([self, result] {
            if (self == nullptr || result.is_ok()) return;
            // The library is per machine, not per session: no epoch check.
            self->statusLabel_.setText(classifyServiceError(result.error()).message, juce::dontSendNotification);
        });
    });
}

void ReferenceScanPanel::timerCallback()
{
    if (listDirty_ && !scanning_) {
        stopTimer();
        loadList();
        return;
    }
    if (listDirty_) loadList();
    if (!scanning_ || jobId_.isEmpty()) {
        stopTimer();
        return;
    }
    if (pollInFlight_) return;
    pollInFlight_ = true;
    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    controller_.runLongInBackground([self, jobId = jobId_, baseUrl = baseUrl_] {
        auto result = ipc::HttpServiceBridge(baseUrl).referenceScanStatus(jobId);
        juce::MessageManager::callAsync([self, result] {
            if (self == nullptr) return;
            // The library is per machine, not per session: no epoch check.
            self->pollInFlight_ = false;
            if (result.is_err()) {
                if (result.error().http_status == 404) { // evicted: the scan is long over
                    self->scanning_ = false;
                    self->progressBar_.setVisible(false);
                    self->updateButtons();
                    self->refresh();
                }
                return;
            }
            self->showProgress(result.value());
        });
    });
}

void ReferenceScanPanel::showProgress(const juce::var& status)
{
    const juce::String state = status[af::ReferenceScanStatusResponse::status].toString();
    const juce::var p = status[af::ReferenceScanStatusResponse::progress];
    const juce::String phase = p[af::ReferenceScanProgress::phase].toString();
    const int done = (int)p[af::ReferenceScanProgress::done];
    const int total = (int)p[af::ReferenceScanProgress::total];
    if (!api::isTerminalStatus(state)) {
        progress_ = (phase == "walking" || total <= 0) ? -1.0 : juce::jlimit(0.0, 1.0, done / (double)total);
        juce::String text;
        if (phase == "walking")
            text = TRANS("Looking for audio files... %n found").replace("%n", juce::String(total));
        else if (phase == "indexing")
            text = TRANS("Adding songs to your reference library: %d of %t")
                       .replace("%d", juce::String(done))
                       .replace("%t", juce::String(total));
        else
            text = TRANS("Reading %d of %t: %f")
                       .replace("%d", juce::String(done))
                       .replace("%t", juce::String(total))
                       .replace("%f", p[af::ReferenceScanProgress::current].toString());
        statusLabel_.setText(text, juce::dontSendNotification);
        summaryLabel_.setText(TRANS("%s songs, %m samples so far")
                                  .replace("%s", juce::String((int)p[af::ReferenceScanProgress::songs]))
                                  .replace("%m", juce::String((int)p[af::ReferenceScanProgress::samples])),
                              juce::dontSendNotification);
        return;
    }
    // Finished, failed or cancelled.
    scanning_ = false;
    stopTimer();
    progressBar_.setVisible(false);
    resized();
    const juce::var r = status[af::ReferenceScanStatusResponse::result];
    juce::String text;
    if (state == api::job_status::failed)
        text = TRANS("The scan failed:") + " " + status[af::ReferenceScanStatusResponse::error].toString();
    else if (state == api::job_status::cancelled)
        text = TRANS("Scan stopped - what was read so far is kept.");
    else {
        text = TRANS("Scan finished: %n new or changed, %u unchanged")
                   .replace("%n", juce::String((int)r[af::ReferenceScanResult::processed]))
                   .replace("%u", juce::String((int)r[af::ReferenceScanResult::unchanged]));
        if ((int)r[af::ReferenceScanResult::failed] > 0)
            text << ", "
                 << TRANS("%f could not be read").replace("%f", juce::String((int)r[af::ReferenceScanResult::failed]));
        if ((int)r[af::ReferenceScanResult::removed] > 0)
            text << ", " << TRANS("%r removed").replace("%r", juce::String((int)r[af::ReferenceScanResult::removed]));
        if ((bool)r[af::ReferenceScanResult::truncated]) text << " " << TRANS("(stopped at the file limit)");
        text << ".";
    }
    statusLabel_.setText(text, juce::dontSendNotification);
    updateButtons();
    loadRootsAndSummary();
    loadList();
    if (onScanFinished) onScanFinished();
}

void ReferenceScanPanel::setView(View v)
{
    if (view_ == v) return;
    view_ = v;
    rows_.clear();
    list_.updateContent();
    loadList();
}

// --- actions -------------------------------------------------------------------------

void ReferenceScanPanel::useSelectedAsReference()
{
    const int r = list_.getSelectedRow();
    if (r < 0 || r >= (int)rows_.size() || rows_[(size_t)r].kind != "song") return;
    const auto& row = rows_[(size_t)r];
    if (!row.exists) {
        statusLabel_.setText(TRANS("That file has moved or been deleted."), juce::dontSendNotification);
        return;
    }
    controller_.analysis().setReferenceTrack(row.path);
    statusLabel_.setText(TRANS("Reference set:") + " " + row.title, juce::dontSendNotification);
}

void ReferenceScanPanel::correctSelectedGenre()
{
    const int r = list_.getSelectedRow();
    if (r < 0 || r >= (int)rows_.size()) return;
    const Row row = rows_[(size_t)r];
    const bool isPack = row.kind == "pack";
    const bool isSample = row.kind == "sample";

    // Families as submenus, their genres inside; ids index `choices`.
    juce::PopupMenu menu;
    std::vector<juce::String> choices;
    menu.addSectionHeader(isPack || isSample ? TRANS("Genre for the whole pack") : TRANS("Genre for this song"));
    std::map<juce::String, juce::PopupMenu> byFamily;
    std::map<juce::String, bool> familyHasEntry;
    const auto& db = controller_.genreDatabase();
    for (const auto& g : db.listAll()) {
        const juce::String fam = juce::String(g.parent.empty() ? g.id : g.parent);
        auto& sub = byFamily[fam];
        if (!familyHasEntry[fam]) {
            choices.push_back(fam);
            sub.addItem((int)choices.size(), TRANS("Any %f").replace("%f", referenceGenreName(db, fam)));
            sub.addSeparator();
            familyHasEntry[fam] = true;
        }
        if (juce::String(g.id) == fam) continue;
        choices.push_back(juce::String(g.id));
        sub.addItem((int)choices.size(), juce::String(g.name), true, juce::String(g.id) == row.genre);
    }
    for (auto& [fam, sub] : byFamily)
        menu.addSubMenu(referenceGenreName(db, fam), sub, true, juce::Image(), fam == row.family);
    menu.addSeparator();
    choices.push_back({});
    const int clearId = (int)choices.size();
    menu.addItem(clearId, TRANS("Clear the genre"));

    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&genreButton_),
                       [self, row, choices, isPack](int result) {
                           if (self == nullptr || result <= 0 || result > (int)choices.size()) return;
                           self->applyGenre(row, choices[(size_t)(result - 1)], isPack);
                       });
}

void ReferenceScanPanel::applyGenre(const Row& row, const juce::String& genre, bool wholePack)
{
    juce::String id = row.id;
    if (wholePack) {
        // A pack row has no item id: correct through any sample in it.
        id.clear();
    }
    statusLabel_.setText(TRANS("Saving the genre..."), juce::dontSendNotification);
    juce::Component::SafePointer<ReferenceScanPanel> self(this);
    controller_.runLongInBackground([self, id, row, genre, wholePack, baseUrl = baseUrl_] {
        ipc::HttpServiceBridge bridge(baseUrl);
        juce::String itemId = id;
        if (itemId.isEmpty()) {
            ipc::HttpServiceBridge::ReferenceScanFilter f;
            f.pack = row.packPath;
            f.kind = "sample";
            f.limit = 1;
            auto items = bridge.referenceScanItems(f);
            if (items.is_ok())
                if (auto* arr = items.value()[af::ReferenceScanItemsResponse::items].getArray())
                    if (!arr->isEmpty()) itemId = arr->getReference(0)[af::ReferenceScanItem::id].toString();
        }
        if (itemId.isEmpty()) {
            juce::MessageManager::callAsync([self] {
                if (self != nullptr)
                    self->statusLabel_.setText(TRANS("That pack has no samples left - scan the folder again."),
                                               juce::dontSendNotification);
            });
            return;
        }
        // A sample's genre is its pack's: correcting one corrects the pack.
        auto result = bridge.referenceScanSetGenre(itemId, genre, wholePack || row.kind == "sample");
        juce::MessageManager::callAsync([self, result, genre] {
            if (self == nullptr) return;
            // The library is per machine, not per session: no epoch check.
            if (result.is_err()) {
                self->statusLabel_.setText(classifyServiceError(result.error()).message, juce::dontSendNotification);
                return;
            }
            const int n = (int)result.value()[af::ReferenceScanGenreResponse::updated];
            juce::String text = genre.isEmpty()
                                    ? TRANS("Genre cleared.")
                                    : TRANS("Genre set to %g")
                                          .replace("%g", referenceGenreName(self->controller_.genreDatabase(), genre));
            if (n > 1) text << " (" << TRANS("%n items").replace("%n", juce::String(n)) << ")";
            if ((bool)result.value()[af::ReferenceScanGenreResponse::learned])
                text << " " << TRANS("The genre model will learn from it.");
            self->statusLabel_.setText(text, juce::dontSendNotification);
            self->loadRootsAndSummary();
            self->loadList();
        });
    });
}

void ReferenceScanPanel::updateButtons()
{
    const int r = list_.getSelectedRow();
    const bool has = r >= 0 && r < (int)rows_.size();
    useButton_.setEnabled(has && rows_[(size_t)r].kind == "song");
    genreButton_.setEnabled(has);
    revealButton_.setEnabled(has);
    scanButton_.setEnabled(!scanning_);
    chooseButton_.setEnabled(!scanning_);
    cancelButton_.setVisible(scanning_);
    forgetButton_.setVisible(!scanning_ && rootPath_.isNotEmpty());
    scanButton_.setButtonText(rootPath_.isEmpty() ? TRANS("Scan a folder...") : TRANS("Scan now"));
    resized(); // the folder row's buttons depend on the state
}

void ReferenceScanPanel::updateEmptyState()
{
    if (!rows_.empty()) {
        emptyState_.setVisible(false);
        return;
    }
    if (!loadedOnce_) {
        emptyState_.setMessage(TRANS("Reading the reference folder..."),
                               TRANS("This waits for the analysis engine, which may still be starting."));
        emptyState_.clearAction();
    } else if (rootPath_.isEmpty() && songs_ + samples_ == 0) {
        emptyState_.setMessage(TRANS("No reference folder yet"),
                               TRANS("Choose a folder - your music or a sample library - and Khensu sorts its songs "
                                     "and samples into genres, with artist, pack and artwork."));
        emptyState_.setAction(TRANS("Choose folder..."), [this] { chooseFolder(); });
    } else if (scanning_) {
        emptyState_.setMessage(TRANS("Scanning..."), TRANS("Songs and samples appear here as the scan sorts them."));
        emptyState_.clearAction();
    } else if (searchEditor_.getText().trim().isNotEmpty()) {
        emptyState_.setMessage(TRANS("No matches"), TRANS("Nothing in this genre matches the search."));
        emptyState_.clearAction();
    } else {
        const juce::String what =
            view_ == View::Songs ? TRANS("songs") : (view_ == View::Samples ? TRANS("samples") : TRANS("packs"));
        emptyState_.setMessage(TRANS("No %w in this genre").replace("%w", what),
                               TRANS("Pick another genre, or scan the folder again to pick up new files."));
        emptyState_.setAction(TRANS("Scan now"), [this] { startScan(); });
    }
    emptyState_.setVisible(true);
}

// --- list ----------------------------------------------------------------------------

int ReferenceScanPanel::getNumRows() { return (int)rows_.size(); }

juce::Image ReferenceScanPanel::thumbnailFor(const juce::String& path)
{
    if (path.isEmpty()) return {};
    if (auto it = thumbs_.find(path); it != thumbs_.end()) return it->second;
    if (thumbs_.size() >= kMaxThumbs) thumbs_.clear();
    // A cached 160 px JPEG under the config dir, read on first paint only.
    const juce::Image img = juce::ImageFileFormat::loadFrom(juce::File(path));
    thumbs_[path] = img;
    return img;
}

void ReferenceScanPanel::drawChip(juce::Graphics& g, juce::Rectangle<int>& area, const juce::String& text,
                                  juce::Colour colour) const
{
    if (text.isEmpty()) return;
    const auto font = KhensuTheme::bodyFont(type::caption);
    const int w =
        juce::jmin(area.getWidth(), juce::roundToInt(juce::GlyphArrangement::getStringWidth(font, text)) + 12);
    if (w < 24) return;
    auto chip = area.removeFromLeft(w).toFloat();
    area.removeFromLeft(4);
    g.setColour(colour.withAlpha(0.18f));
    g.fillRoundedRectangle(chip, 8.0f);
    g.setColour(colour);
    g.setFont(font);
    g.drawText(text, chip, juce::Justification::centred, true);
}

void ReferenceScanPanel::paintListBoxItem(int rowNumber, juce::Graphics& g, int width, int height, bool selected)
{
    if (rowNumber < 0 || rowNumber >= (int)rows_.size()) return;
    const auto& r = rows_[(size_t)rowNumber];
    g.fillAll(selected ? KhensuTheme::bgRaised : KhensuTheme::bgSurface);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawHorizontalLine(height - 1, 0.0f, (float)width);

    // Thumbnail (or a placeholder tile with the first letter).
    auto thumbR = juce::Rectangle<int>(8, (height - kThumb) / 2, kThumb, kThumb);
    const juce::Image img = thumbnailFor(r.artworkPath);
    if (img.isValid()) {
        g.drawImage(img, thumbR.toFloat(),
                    juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
    } else {
        g.setColour(KhensuTheme::bgOverlay);
        g.fillRoundedRectangle(thumbR.toFloat(), 4.0f);
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::title));
        g.drawText(r.title.substring(0, 1).toUpperCase(), thumbR, juce::Justification::centred);
    }

    auto text = juce::Rectangle<int>(thumbR.getRight() + 10, 6, width - thumbR.getRight() - 18, height - 12);
    auto line1 = text.removeFromTop(18);
    auto line2 = text.removeFromTop(16);

    // Right side of line 1: confidence; line 2: chips.
    const auto& db = controller_.genreDatabase();
    if (r.kind != "pack" && (r.genre.isNotEmpty() || r.family.isNotEmpty())) {
        const juce::String conf =
            r.source == "user" ? TRANS("yours") : juce::String(juce::roundToInt(r.confidence * 100.0f)) + "%";
        g.setColour(r.confidence >= 0.6f || r.source == "user" ? KhensuTheme::textSecondary
                                                               : KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::metricFont(type::caption));
        g.drawText(conf, line1.removeFromRight(48), juce::Justification::centredRight);
    }
    g.setColour(r.exists ? KhensuTheme::textPrimary : KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::labelFont(type::secondary));
    g.drawText(r.title, line1, juce::Justification::centredLeft, true);

    juce::String meta;
    if (r.kind == "pack") {
        meta = r.author;
        meta << (meta.isEmpty() ? "" : "  -  ")
             << TRANS("%n samples, %l loops")
                    .replace("%n", juce::String(r.samples))
                    .replace("%l", juce::String(r.loops));
    } else if (r.kind == "sample") {
        meta = r.author;
        if (r.pack.isNotEmpty()) meta << (meta.isEmpty() ? "" : "  -  ") << r.pack;
        meta << (meta.isEmpty() ? "" : "  -  ") << (r.sampleType == "one_shot" ? TRANS("one-shot") : TRANS("loop"));
        if (r.bpm > 0.0) meta << ", " << juce::String(juce::roundToInt(r.bpm)) << " BPM";
        if (r.key.isNotEmpty()) meta << ", " << r.key;
    } else {
        meta = r.author.isNotEmpty() ? r.author : TRANS("Unknown artist");
        if (r.album.isNotEmpty()) meta << "  -  " << r.album;
    }
    auto chips = line2.removeFromRight(juce::jmin(260, line2.getWidth() / 2));
    g.setColour(KhensuTheme::textSecondary);
    g.setFont(KhensuTheme::bodyFont(type::caption));
    g.drawText(meta, line2, juce::Justification::centredLeft, true);

    // Genre / subgenre chips: the family, then the catalog genre under it.
    auto chipRow = chips;
    if (r.genre.isEmpty() && r.family.isEmpty()) {
        drawChip(g, chipRow, TRANS("Unsorted"), KhensuTheme::textTertiary);
    } else {
        if (r.family.isNotEmpty()) drawChip(g, chipRow, referenceGenreName(db, r.family), KhensuTheme::accentBlue);
        if (r.genre.isNotEmpty() && r.genre != r.family)
            drawChip(g, chipRow, referenceGenreName(db, r.genre), KhensuTheme::accentTeal);
    }
    if (!r.exists && r.kind != "pack") {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(type::caption));
        g.drawText(TRANS("missing"), text, juce::Justification::centredLeft, true);
    }
}

juce::String ReferenceScanPanel::getTooltipForRow(int row)
{
    if (row < 0 || row >= (int)rows_.size()) return {};
    const auto& r = rows_[(size_t)row];
    juce::String tip = r.kind == "pack" ? r.packPath : r.path;
    const juce::String why = sourceText(r.source);
    if (why.isNotEmpty()) tip << "\n" << TRANS("Genre") << ": " << why;
    return tip;
}

void ReferenceScanPanel::selectedRowsChanged(int) { updateButtons(); }

void ReferenceScanPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    list_.selectRow(row);
    if (row >= 0 && row < (int)rows_.size() && rows_[(size_t)row].kind == "song") useSelectedAsReference();
    if (row >= 0 && row < (int)rows_.size() && rows_[(size_t)row].kind == "pack") {
        // Open the pack: its samples.
        searchEditor_.setText({}, juce::dontSendNotification);
        samplesTab_.setToggleState(true, juce::dontSendNotification);
        view_ = View::Samples;
        searchEditor_.setText(rows_[(size_t)row].pack, juce::dontSendNotification);
        loadList();
    }
}

// --- layout --------------------------------------------------------------------------

void ReferenceScanPanel::paint(juce::Graphics& g)
{
    g.setColour(KhensuTheme::bgSurface);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
}

void ReferenceScanPanel::resized()
{
    auto b = getLocalBounds().reduced(8);
    auto folderRow = b.removeFromTop(KhensuTheme::buttonHSmall);
    if (scanning_) {
        cancelButton_.setBounds(folderRow.removeFromRight(80));
        folderRow.removeFromRight(6);
    } else if (rootPath_.isNotEmpty()) {
        forgetButton_.setBounds(folderRow.removeFromRight(72));
        folderRow.removeFromRight(6);
    }
    scanButton_.setBounds(folderRow.removeFromRight(116));
    folderRow.removeFromRight(6);
    chooseButton_.setBounds(folderRow.removeFromRight(90));
    folderRow.removeFromRight(6);
    folderCaption_.setBounds(folderRow.removeFromLeft(112));
    folderPath_.setBounds(folderRow);

    b.removeFromTop(4);
    auto infoRow = b.removeFromTop(22);
    rescanToggle_.setBounds(infoRow.removeFromRight(150));
    summaryLabel_.setBounds(infoRow.removeFromLeft(juce::jmin(300, infoRow.getWidth())));
    if (progressBar_.isVisible()) progressBar_.setBounds(infoRow.reduced(6, 4));
    b.removeFromTop(2);
    statusLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(4);

    auto filterRow = b.removeFromTop(KhensuTheme::buttonHSmall);
    songsTab_.setBounds(filterRow.removeFromLeft(80));
    samplesTab_.setBounds(filterRow.removeFromLeft(84));
    packsTab_.setBounds(filterRow.removeFromLeft(76));
    filterRow.removeFromLeft(8);
    searchEditor_.setBounds(filterRow);
    b.removeFromTop(6);

    auto actions = b.removeFromBottom(KhensuTheme::buttonHSmall);
    b.removeFromBottom(6);
    useButton_.setBounds(actions.removeFromLeft(130));
    actions.removeFromLeft(6);
    genreButton_.setBounds(actions.removeFromLeft(130));
    actions.removeFromLeft(6);
    revealButton_.setBounds(actions.removeFromLeft(100));

    list_.setBounds(b);
    emptyState_.setBounds(b);
}

} // namespace khensu
