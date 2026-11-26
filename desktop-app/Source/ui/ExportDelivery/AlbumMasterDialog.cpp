// desktop-app/Source/ui/ExportDelivery/AlbumMasterDialog.cpp
#include "AlbumMasterDialog.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/JobPoller.h"
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace af = khensu::api::fields;

namespace khensu {

namespace {

/// One editable table cell. Commits on return / focus loss via onTextChange.
class EditableCell final : public juce::Label {
  public:
    EditableCell() { setEditable(false, true, false); }
    int row = 0, col = 0;
};

double parseSecondsOr(const juce::String& text, double fallback)
{
    const auto t = text.trim();
    if (t.isEmpty()) return 0.0;
    if (!t.containsOnly("0123456789.-+")) return fallback;
    return t.getDoubleValue();
}

} // namespace

void AlbumMasterDialog::launch(const juce::StringArray& trackPaths, Runner shortJobs, Runner longJobs)
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(new AlbumMasterDialog(trackPaths, std::move(shortJobs), std::move(longJobs)));
    options.dialogTitle = TRANS("Master Album - CD / DDP");
    options.dialogBackgroundColour = KhensuTheme::bgSurface;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    options.content->setSize(820, 560);
    options.launchAsync();
}

void AlbumMasterDialog::applyThemeColours()
{
    for (auto* l : {&titleLabel_, &performerLabel_, &catalogLabel_})
        l->setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    for (auto* e : {&titleEdit_, &performerEdit_, &catalogEdit_, &detailText_}) {
        e->setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgRaised);
        e->setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
        e->setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    }
    table_.setColour(juce::ListBox::backgroundColourId, KhensuTheme::bgRaised);
    table_.getHeader().setColour(juce::TableHeaderComponent::backgroundColourId, KhensuTheme::bgOverlay);
    table_.getHeader().setColour(juce::TableHeaderComponent::textColourId, KhensuTheme::textSecondary);
    exportButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.25f));
    exportButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    progressBar_.setColour(juce::ProgressBar::foregroundColourId, KhensuTheme::accentTeal);
    progressBar_.setColour(juce::ProgressBar::backgroundColourId, KhensuTheme::bgOverlay);
}

AlbumMasterDialog::AlbumMasterDialog(const juce::StringArray& trackPaths, Runner shortJobs, Runner longJobs)
    : shortJobs_(std::move(shortJobs)), longJobs_(std::move(longJobs))
{
    sequence_.setPaths(trackPaths);
    applyThemeColours();

    titleLabel_.setText(TRANS("Album title"), juce::dontSendNotification);
    performerLabel_.setText(TRANS("Performer"), juce::dontSendNotification);
    catalogLabel_.setText(TRANS("UPC / EAN"), juce::dontSendNotification);
    for (auto* l : {&titleLabel_, &performerLabel_, &catalogLabel_}) {
        l->setFont(KhensuTheme::labelFont(type::caption));
        addAndMakeVisible(*l);
    }
    titleEdit_.setTitle(TRANS("Album title"));
    performerEdit_.setTitle(TRANS("Album performer"));
    catalogEdit_.setTitle(TRANS("UPC or EAN catalogue number"));
    catalogEdit_.setTooltip(TRANS("Optional. The disc's 12-digit UPC-A or 13-digit EAN-13 barcode number."));
    catalogEdit_.setInputRestrictions(14, "0123456789 -");
    for (auto* e : {&titleEdit_, &performerEdit_, &catalogEdit_})
        addAndMakeVisible(*e);

    auto& header = table_.getHeader();
    header.addColumn("#", colNum, 32, 32, 32, juce::TableHeaderComponent::notResizable);
    header.addColumn(TRANS("Title"), colTitle, 220, 80);
    header.addColumn(TRANS("ISRC"), colIsrc, 130, 100);
    header.addColumn(TRANS("Gap s"), colGap, 64, 50);
    header.addColumn(TRANS("Xfade s"), colXfade, 64, 50);
    header.addColumn(TRANS("Fade out s"), colFadeOut, 76, 50);
    header.addColumn(TRANS("Trim dB"), colTrim, 64, 50);
    table_.setRowHeight(24);
    table_.setTitle(TRANS("Album track list"));
    table_.setTooltip(TRANS("Double-click a cell to edit. Gap is the silence before a track; a crossfade replaces it."));
    addAndMakeVisible(table_);

    upButton_.onClick = [this] { moveSelected(-1); };
    downButton_.onClick = [this] { moveSelected(+1); };
    removeButton_.onClick = [this] { removeSelected(); };
    trimsButton_.onClick = [this] { suggestTrims(); };
    trimsButton_.setTooltip(TRANS("Measure every track and suggest static trims that bring the loudest passages "
                                  "within 1.5 LU - album-mode, nothing is normalised to a target."));
    planButton_.onClick = [this] { checkLayout(); };
    planButton_.setTooltip(TRANS("Validate ISRCs and the catalogue number and show every CD index point."));
    folderButton_.onClick = [this] { chooseOutputDir(); };
    exportButton_.onClick = [this] { startExport(); };
    cancelButton_.onClick = [this] { cancelExport(); };
    cancelButton_.setEnabled(false);
    for (auto* b : {&upButton_, &downButton_, &removeButton_, &trimsButton_, &planButton_, &folderButton_,
                    &exportButton_, &cancelButton_})
        addAndMakeVisible(*b);

    for (auto* t : {&wavToggle_, &cueToggle_, &ddpToggle_, &cdTextToggle_}) {
        t->setToggleState(true, juce::dontSendNotification);
        addAndMakeVisible(*t);
    }
    ddpToggle_.setTooltip(TRANS("DDP 2.0 image for CD replication. Check it in a DDP player before sending it to "
                                "a plant - this writer has not been validated against one."));

    addAndMakeVisible(progressBar_);
    statusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    statusLabel_.setText(TRANS("Order the tracks, set gaps and ISRCs, then render."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    detailText_.setMultiLine(true);
    detailText_.setReadOnly(true);
    detailText_.setFont(KhensuTheme::monoFont(type::caption));
    detailText_.setTitle(TRANS("Album master details"));
    addAndMakeVisible(detailText_);
}

AlbumMasterDialog::~AlbumMasterDialog()
{
    alive_->store(false);
    // Closing the window mid-render cancels the job rather than leaving a
    // render nobody is watching (and a half-written DDP) behind.
    if (activeJobId_.isNotEmpty() && shortJobs_)
        shortJobs_([id = activeJobId_] { ipc::HttpServiceBridge{}.cancelAlbumMasterJob(id); });
}

void AlbumMasterDialog::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgSurface); }

void AlbumMasterDialog::resized()
{
    auto b = getLocalBounds().reduced(12);
    auto meta = b.removeFromTop(26);
    auto place = [&meta](juce::Label& l, juce::TextEditor& e, int lw, int ew) {
        l.setBounds(meta.removeFromLeft(lw));
        e.setBounds(meta.removeFromLeft(ew).reduced(0, 1));
        meta.removeFromLeft(10);
    };
    place(titleLabel_, titleEdit_, 72, 200);
    place(performerLabel_, performerEdit_, 66, 170);
    place(catalogLabel_, catalogEdit_, 64, juce::jmax(80, meta.getWidth() - 64));
    b.removeFromTop(8);

    auto bottom = b.removeFromBottom(150);
    auto rowButtons = b.removeFromBottom(28);
    b.removeFromBottom(6);
    table_.setBounds(b);

    rowButtons.removeFromTop(4);
    for (auto* btn : {&upButton_, &downButton_, &removeButton_}) {
        btn->setBounds(rowButtons.removeFromLeft(86));
        rowButtons.removeFromLeft(6);
    }
    rowButtons.removeFromLeft(12);
    trimsButton_.setBounds(rowButtons.removeFromLeft(150));
    rowButtons.removeFromLeft(6);
    planButton_.setBounds(rowButtons.removeFromLeft(110));

    auto outputs = bottom.removeFromTop(28);
    outputs.removeFromTop(4);
    for (auto* t : {&wavToggle_, &cueToggle_, &ddpToggle_, &cdTextToggle_})
        t->setBounds(outputs.removeFromLeft(t == &cueToggle_ ? 96 : 84));
    outputs.removeFromLeft(8);
    folderButton_.setBounds(outputs.removeFromLeft(120));
    cancelButton_.setBounds(outputs.removeFromRight(110));
    outputs.removeFromRight(6);
    exportButton_.setBounds(outputs.removeFromRight(120));

    bottom.removeFromTop(6);
    progressBar_.setBounds(bottom.removeFromTop(14));
    bottom.removeFromTop(4);
    statusLabel_.setBounds(bottom.removeFromTop(18));
    detailText_.setBounds(bottom);
}

// --- Table ---------------------------------------------------------------------

int AlbumMasterDialog::getNumRows() { return sequence_.size(); }

void AlbumMasterDialog::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll(selected ? KhensuTheme::accentTeal.withAlpha(0.18f)
                       : (row % 2 == 0 ? KhensuTheme::bgRaised : KhensuTheme::bgSurface));
}

void AlbumMasterDialog::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool)
{
    if (col != colNum) return; // the other columns are EditableCell children
    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::monoFont(type::caption));
    g.drawText(juce::String(row + 1), 0, 0, w - 4, h, juce::Justification::centredRight, false);
}

juce::String AlbumMasterDialog::cellText(int row, int col) const
{
    if (row < 0 || row >= sequence_.size()) return {};
    const auto& t = sequence_.tracks()[(size_t)row];
    auto num = [](double v) { return v == 0.0 ? juce::String("0") : juce::String(v, 2); };
    switch (col) {
    case colTitle: return t.title;
    case colIsrc: return t.isrc;
    case colGap: return row == 0 ? juce::String("2") : num(t.gapSec);
    case colXfade: return num(t.crossfadeSec);
    case colFadeOut: return num(t.fadeOutSec);
    case colTrim: return num(t.gainDb);
    default: return {};
    }
}

void AlbumMasterDialog::setCellText(int row, int col, const juce::String& text)
{
    if (row < 0 || row >= sequence_.size()) return;
    auto& t = sequence_.tracks()[(size_t)row];
    switch (col) {
    case colTitle: t.title = text.trim(); break;
    case colIsrc: {
        const auto canon = album_seq::canonicalIsrc(text);
        t.isrc = canon ? *canon : text.trim(); // keep the typo visible; validation flags it
        break;
    }
    // Track 1's gap is the fixed 2 s Red Book pre-gap: the cell is read-only
    // and never stores an edit it would not honour (UIX-21).
    case colGap:
        if (row > 0) t.gapSec = juce::jmax(0.0, parseSecondsOr(text, t.gapSec));
        break;
    case colXfade: t.crossfadeSec = row == 0 ? 0.0 : juce::jmax(0.0, parseSecondsOr(text, t.crossfadeSec)); break;
    case colFadeOut: t.fadeOutSec = juce::jmax(0.0, parseSecondsOr(text, t.fadeOutSec)); break;
    case colTrim: t.gainDb = juce::jlimit(-24.0, 24.0, parseSecondsOr(text, t.gainDb)); break;
    default: break;
    }
    table_.updateContent();
    table_.repaint();
}

juce::Component* AlbumMasterDialog::refreshComponentForCell(int row, int col, bool, juce::Component* existing)
{
    if (col == colNum) {
        delete existing;
        return nullptr;
    }
    auto* cell = dynamic_cast<EditableCell*>(existing);
    if (cell == nullptr) {
        delete existing;
        cell = new EditableCell();
        cell->setFont(KhensuTheme::labelFont(type::caption));
        cell->onTextChange = [this, cell] { setCellText(cell->row, cell->col, cell->getText()); };
    }
    cell->row = row;
    cell->col = col;
    cell->setText(cellText(row, col), juce::dontSendNotification);
    // Track 1: the gap is the fixed 2 s pre-gap and it cannot crossfade, so
    // those cells are read-only instead of accepting edits that are ignored.
    const bool fixedCell = row == 0 && (col == colGap || col == colXfade);
    cell->setEditable(false, !fixedCell, false);
    cell->setTooltip(fixedCell ? TRANS("Track 1 always starts after the fixed 2 s CD pre-gap and cannot crossfade.")
                               : juce::String());
    const bool badIsrc = col == colIsrc && !album_seq::canonicalIsrc(cell->getText()).has_value();
    cell->setColour(juce::Label::textColourId, badIsrc      ? KhensuTheme::accentRed
                                               : fixedCell ? KhensuTheme::textTertiary
                                                           : KhensuTheme::textPrimary);
    cell->setTitle(table_.getHeader().getColumnName(col) + " " + TRANS("track") + " " + juce::String(row + 1));
    return cell;
}

void AlbumMasterDialog::moveSelected(int delta)
{
    const int row = table_.getSelectedRow();
    const bool moved = delta < 0 ? sequence_.moveUp(row) : sequence_.moveDown(row);
    if (!moved) return;
    if (!sequence_.tracks().empty()) sequence_.tracks()[0].crossfadeSec = 0.0; // track 1 can't crossfade
    table_.updateContent();
    table_.selectRow(row + delta);
    table_.repaint();
}

void AlbumMasterDialog::removeSelected()
{
    if (!sequence_.remove(table_.getSelectedRow())) return;
    if (!sequence_.tracks().empty()) sequence_.tracks()[0].crossfadeSec = 0.0;
    table_.updateContent();
    table_.repaint();
}

// --- Service calls -------------------------------------------------------------------

ipc::HttpServiceBridge::AlbumMasterSpec AlbumMasterDialog::buildSpec() const
{
    ipc::HttpServiceBridge::AlbumMasterSpec spec;
    spec.tracks = sequence_.tracks();
    spec.title = titleEdit_.getText().trim();
    spec.performer = performerEdit_.getText().trim();
    spec.catalog = catalogEdit_.getText().removeCharacters(" -");
    spec.outputDir = outputDir_;
    spec.wav = wavToggle_.getToggleState();
    spec.cue = cueToggle_.getToggleState();
    spec.ddp = ddpToggle_.getToggleState();
    spec.cdText = cdTextToggle_.getToggleState();
    return spec;
}

void AlbumMasterDialog::setBusy(bool busy, const juce::String& status)
{
    for (auto* b : {&upButton_, &downButton_, &removeButton_, &trimsButton_, &planButton_, &folderButton_,
                    &exportButton_})
        b->setEnabled(!busy);
    statusLabel_.setText(status, juce::dontSendNotification);
}

void AlbumMasterDialog::showLines(const juce::StringArray& lines, juce::Colour colour)
{
    detailText_.setColour(juce::TextEditor::textColourId, colour);
    detailText_.setText(lines.joinIntoString("\n"), false);
}

void AlbumMasterDialog::suggestTrims()
{
    juce::StringArray paths;
    for (const auto& t : sequence_.tracks())
        paths.add(t.path);
    if (paths.isEmpty()) return;
    setBusy(true, TRANS("Measuring every track for album-mode trims..."));
    juce::Component::SafePointer<AlbumMasterDialog> self(this);
    shortJobs_([self, paths] {
        auto res = ipc::HttpServiceBridge{}.albumConsistency(paths);
        juce::MessageManager::callAsync([self, res] {
            if (self == nullptr) return;
            self->setBusy(false, {});
            if (res.is_err()) {
                self->statusLabel_.setText(TRANS("Trim suggestion failed: ") + serviceErrorLine(res.error()),
                                           juce::dontSendNotification);
                return;
            }
            const auto& v = res.value();
            const int n = self->sequence_.applyConsistency(v);
            juce::StringArray lines;
            lines.add(TRANS("Loudest-passage spread") + ": " + juce::String((double)v[af::AlbumConsistencyResponse::spread_before_lu], 1) +
                      " LU -> " + juce::String((double)v[af::AlbumConsistencyResponse::spread_after_lu], 1) + " LU");
            if (auto* arr = v[af::AlbumConsistencyResponse::tracks].getArray())
                for (const auto& row : *arr)
                    lines.add("  " + row[af::AlbumConsistencyTrack::name].toString() + ": " + juce::String((double)row[af::AlbumConsistencyTrack::gain_db], 1) +
                              " dB  (" + row[af::AlbumConsistencyTrack::reason].toString() + ")");
            self->showLines(lines, KhensuTheme::textPrimary);
            self->statusLabel_.setText(TRANS("%n trims applied - edit the Trim column to adjust.").replace("%n", juce::String(n)),
                                       juce::dontSendNotification);
            self->table_.updateContent();
            self->table_.repaint();
        });
    });
}

void AlbumMasterDialog::checkLayout()
{
    const auto errs = sequence_.validationErrors(catalogEdit_.getText());
    if (!errs.isEmpty()) {
        showLines(errs, KhensuTheme::accentAmber);
        return;
    }
    setBusy(true, TRANS("Laying the album out on the CD frame grid..."));
    juce::Component::SafePointer<AlbumMasterDialog> self(this);
    shortJobs_([self, spec = buildSpec()] {
        auto res = ipc::HttpServiceBridge{}.albumMasterPlan(spec);
        juce::MessageManager::callAsync([self, res] {
            if (self == nullptr) return;
            self->setBusy(false, {});
            if (res.is_err()) {
                self->showLines({serviceErrorLine(res.error())}, KhensuTheme::accentAmber);
                return;
            }
            const auto& v = res.value();
            juce::StringArray lines;
            lines.add(TRANS("Total") + " " + v[af::AlbumPlanResponse::total_msf].toString() + "  (" +
                      juce::String((double)v[af::AlbumPlanResponse::total_sec] / 60.0, 1) + " min)");
            if (auto* arr = v[af::AlbumPlanResponse::tracks].getArray())
                for (const auto& t : *arr)
                    lines.add(juce::String((int)t[af::AlbumPlanTrack::number]).paddedLeft('0', 2) + "  " +
                              TRANS("index 00") + " " + t[af::AlbumPlanTrack::index00_msf].toString() + "  " + TRANS("index 01") + " " +
                              t[af::AlbumPlanTrack::index01_msf].toString() + "  " + t[af::AlbumPlanTrack::isrc].toString() + "  " +
                              t[af::AlbumPlanTrack::title].toString());
            if (auto* w = v[af::AlbumPlanResponse::warnings].getArray())
                for (const auto& line : *w)
                    lines.add("! " + line.toString());
            self->showLines(lines, KhensuTheme::textPrimary);
            self->statusLabel_.setText(TRANS("Layout is valid."), juce::dontSendNotification);
        });
    });
}

void AlbumMasterDialog::chooseOutputDir()
{
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Choose the album master folder"),
                                                   juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    juce::Component::SafePointer<AlbumMasterDialog> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [self](const juce::FileChooser& fc) {
                              if (self == nullptr || fc.getResult() == juce::File()) return;
                              self->outputDir_ = fc.getResult().getFullPathName();
                              self->statusLabel_.setText(TRANS("Output folder") + ": " + self->outputDir_,
                                                         juce::dontSendNotification);
                          });
}

void AlbumMasterDialog::startExport()
{
    const auto errs = sequence_.validationErrors(catalogEdit_.getText());
    if (!errs.isEmpty()) {
        showLines(errs, KhensuTheme::accentAmber);
        return;
    }
    if (!wavToggle_.getToggleState() && !ddpToggle_.getToggleState()) {
        showLines({TRANS("Choose at least one of WAV or DDP.")}, KhensuTheme::accentAmber);
        return;
    }
    progress_ = 0.0;
    setBusy(true, TRANS("Starting the album render..."));
    juce::Component::SafePointer<AlbumMasterDialog> self(this);
    shortJobs_([self, spec = buildSpec()] {
        auto res = ipc::HttpServiceBridge{}.startAlbumMasterJob(spec);
        juce::MessageManager::callAsync([self, res] {
            if (self == nullptr) return;
            if (res.is_err()) {
                self->setBusy(false, TRANS("Could not start the render."));
                self->showLines({serviceErrorLine(res.error())}, KhensuTheme::accentAmber);
                return;
            }
            self->activeJobId_ = res.value();
            self->cancelButton_.setEnabled(true);
            self->pollJob(self->activeJobId_);
        });
    });
}

void AlbumMasterDialog::cancelExport()
{
    if (activeJobId_.isEmpty()) return;
    cancelButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Cancelling..."), juce::dontSendNotification);
    shortJobs_([id = activeJobId_] { ipc::HttpServiceBridge{}.cancelAlbumMasterJob(id); });
}

void AlbumMasterDialog::pollJob(const juce::String& jobId)
{
    juce::Component::SafePointer<AlbumMasterDialog> self(this);
    longJobs_([self, jobId, alive = alive_] {
        ipc::HttpServiceBridge bridge;
        // The shared JobPoller: CONSECUTIVE poll errors are bounded (a
        // success resets the count - five scattered transient errors over a
        // long render used to end in "Lost track of the render", UIX-17) and
        // a 404 is terminal (UIX-07).
        using Poller = JobPoller<ipc::HttpServiceBridge::AlbumMasterJobStatus>;
        Poller::Hooks hooks;
        hooks.poll = [&bridge](const std::string& id) { return bridge.getAlbumMasterJobStatus(juce::String(id)); };
        hooks.cancel = [&bridge](const std::string& id) { bridge.cancelAlbumMasterJob(juce::String(id)); };
        hooks.isTerminal = [](const auto& st) { return api::isTerminalStatus(st.status); };
        hooks.onProgress = [self](const auto& st) {
            if (api::isTerminalStatus(st.status)) return; // the final status is handled below
            juce::MessageManager::callAsync([self, st] {
                if (self == nullptr) return;
                self->progress_ = st.total > 0 ? (double)st.done / (double)st.total : 0.0;
                self->statusLabel_.setText(TRANS("Rendering") + " " + st.current + "  (" + juce::String(st.done) + "/"
                                               + juce::String(st.total) + ")",
                                           juce::dontSendNotification);
            });
        };
        hooks.shouldStop = [alive] { return !alive->load(); };
        Poller::Options opts;
        opts.pollIntervalMs = 500;
        opts.maxConsecutiveErrors = 5;
        opts.deadlineSeconds = 2 * 60 * 60;
        const auto out = Poller::run(hooks, opts, jobId.toStdString());
        if (out.outcome == JobPollOutcome::Stopped) return; // the dialog closed (and cancelled the job)

        juce::MessageManager::callAsync([self, out] {
            if (self == nullptr) return;
            self->activeJobId_.clear();
            self->cancelButton_.setEnabled(false);
            if (!out.finished() || !out.status.has_value()) {
                const juce::String why = out.outcome == JobPollOutcome::ServiceLost ? serviceErrorLine(out.lastError)
                                                                                    : out.describe();
                self->setBusy(false, TRANS("Lost track of the render: ") + why);
                return;
            }
            const auto& s = *out.status;
            if (api::isStatus(s.status, api::JobStatus::Completed)) {
                const auto& r = s.result;
                juce::StringArray lines;
                if (r[af::AlbumMasterResult::wav_path].isString()) lines.add("WAV   " + r[af::AlbumMasterResult::wav_path].toString());
                if (r[af::AlbumMasterResult::cue_path].isString()) lines.add("CUE   " + r[af::AlbumMasterResult::cue_path].toString());
                if (r[af::AlbumMasterResult::ddp_dir].isString()) {
                    lines.add("DDP   " + r[af::AlbumMasterResult::ddp_dir].toString());
                    lines.add((bool)r[af::AlbumMasterResult::ddp_verified]
                                  ? TRANS("DDP self-check passed (checksums, PQ, lengths). Verify it in a DDP "
                                          "player before replication.")
                                  : TRANS("DDP self-check FAILED - do not send this image."));
                    if (auto* p = r[af::AlbumMasterResult::ddp_problems].getArray())
                        for (const auto& line : *p)
                            lines.add("! " + line.toString());
                }
                const int clipped = (int)r[af::AlbumMasterResult::render][af::AlbumRenderStats::clipped_samples];
                if (clipped > 0)
                    lines.add("! " + TRANS("%n samples clipped at 16-bit - lower the trims on the loudest tracks.")
                                         .replace("%n", juce::String(clipped)));
                self->progress_ = 1.0;
                self->showLines(lines, (bool)r[af::AlbumMasterResult::ddp_verified] || !r[af::AlbumMasterResult::ddp_dir].isString()
                                           ? KhensuTheme::textPrimary
                                           : KhensuTheme::accentRed);
                self->setBusy(false, TRANS("Album master written."));
            } else if (api::isStatus(s.status, api::JobStatus::Failed)) {
                self->showLines({s.error}, KhensuTheme::accentRed);
                self->setBusy(false, TRANS("The render failed."));
            } else {
                self->setBusy(false, TRANS("Render cancelled - partial files were removed."));
            }
        });
    });
}

} // namespace khensu
