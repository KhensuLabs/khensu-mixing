// desktop-app/Source/ui/ExportDelivery/AlbumAssemblyPanel.cpp
#include "../shared/ServiceErrorHint.h"
#include "AlbumAssemblyPanel.h"
#include "AlbumMasterDialog.h"
#include "../shared/KhensuLookAndFeel.h"
#include "../shared/KhensuTheme.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace af = khensu::api::fields;

namespace khensu {

// Every cached palette colour on this component, in one place (UI-01).
void AlbumAssemblyPanel::applyThemeColours()
{
    chooseButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
    chooseButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    normSimButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    normSimButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    masterButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
    masterButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    for (auto* b : {&upButton_, &downButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::bgOverlay);
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::textSecondary);
    }
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

AlbumAssemblyPanel::AlbumAssemblyPanel(IAppController& c) : PanelCard(c, "Album Assembly")
{
    applyThemeColours();
    setExplainer(TRANS("Album Assembly"), TRANS("The album is the product, not the song. Tracks mastered on different days "
                                   "drift in loudness, tonal tilt, and density - and the listener hears "
                                   "every seam. Load the tracklist in album order and this measures each track "
                                   "and flags the discontinuities an album pass would fix: loudness jumps "
                                   "(>2 LU between neighbors), tonal shifts (>1.5 dB/oct tilt difference), and "
                                   "density mismatches (>4 dB crest difference).\n\n"
                                   "Album level is set by the loudest track - trim the others toward "
                                   "it rather than normalizing each song separately."));

    chooseButton_.onClick = [this] { chooseTracks(); };
    addAndMakeVisible(chooseButton_);

    normSimButton_.onClick = [this] { runNormalizationSim(); };
    addAndMakeVisible(normSimButton_);

    masterButton_.setTooltip(TRANS("Sequence the album (gaps, crossfades, ISRCs) and render a CD master: "
                                   "44.1 kHz / 16-bit WAV, cue sheet and DDP 2.0 image."));
    masterButton_.onClick = [this] {
        if (trackPaths_.isEmpty()) {
            flagsLabel_.setText(TRANS("Add tracks first."), juce::dontSendNotification);
            return;
        }
        auto* ctl = &controller_;
        AlbumMasterDialog::launch(
            trackPaths_, [ctl](std::function<void()> job) { ctl->runInBackground(std::move(job)); },
            [ctl](std::function<void()> job) { ctl->runLongInBackground(std::move(job)); });
    };
    addAndMakeVisible(masterButton_);

    upButton_.setTooltip(TRANS("Move the selected track earlier in the album (click a track row to select it)."));
    downButton_.setTooltip(TRANS("Move the selected track later in the album (click a track row to select it)."));
    upButton_.onClick = [this] { moveSelected(-1); };
    downButton_.onClick = [this] { moveSelected(+1); };
    upButton_.setEnabled(false);
    downButton_.setEnabled(false);
    addAndMakeVisible(upButton_);
    addAndMakeVisible(downButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(10.0f));
    statusLabel_.setText(TRANS("Load your album's masters in track order."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    flagsLabel_.setFont(KhensuTheme::labelFont(9.5f));
    flagsLabel_.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(flagsLabel_);
}

void AlbumAssemblyPanel::chooseTracks()
{
    // FEAT-6: the project's tracks when this session is in one, else files.
    auto db = controller_.sessions().sessionManagerPtr();
    const auto* session = controller_.getActiveSession();
    if (db == nullptr || session == nullptr) return browseTracks();
    const std::string sessionId = session->id;
    juce::Component::SafePointer<AlbumAssemblyPanel> self(this);
    controller_.runInBackground([self, db, sessionId] {
        auto snap = project_tracks::load(*db, sessionId);
        juce::MessageManager::callAsync([self, snap] {
            if (self == nullptr) return;
            if (!snap.current || snap.tracks.size() < 2) return self->browseTracks();
            juce::PopupMenu menu;
            menu.addItem(1, TRANS("Use %p's %n tracks (current masters)")
                                .replace("%p", juce::String(snap.current->name))
                                .replace("%n", juce::String((int)snap.tracks.size())));
            menu.addItem(2, TRANS("Choose files..."));
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&self->chooseButton_),
                               [self, snap](int id) {
                                   if (self == nullptr) return;
                                   if (id == 1) self->useProject(snap);
                                   else if (id == 2) self->browseTracks();
                               });
        });
    });
}

void AlbumAssemblyPanel::useProject(const project_tracks::Snapshot& snap)
{
    if (!snap.current) return;
    trackPaths_.clear();
    trackSessionIds_.clear();
    juce::StringArray missing;
    for (const auto& t : snap.tracks) {
        if (t.masterMissing) {
            missing.add(t.name);
            continue;
        }
        trackPaths_.add(t.masterPath);
        trackSessionIds_.push_back(t.sessionId);
    }
    projectId_ = snap.current->id;
    selectedTrack_ = -1;
    if (!missing.isEmpty())
        flagsLabel_.setText(TRANS("Left out (file not found):") + " " + missing.joinIntoString(", "),
                            juce::dontSendNotification);
    runAnalysis();
}

void AlbumAssemblyPanel::browseTracks()
{
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Select album tracks"),
                                                   juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                   "*.wav;*.aiff;*.flac;*.mp3");
    juce::Component::SafePointer<AlbumAssemblyPanel> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectMultipleItems,
                          [self](const juce::FileChooser& fc) {
                              if (self == nullptr) return;
                              juce::Array<juce::File> files;
                              for (const auto& f : fc.getResults())
                                  if (f.existsAsFile()) files.add(f);
                              // Cancelled (or nothing usable): keep the tracks and
                              // matrix already shown (UIX-17).
                              if (files.isEmpty()) return;
                              // A deterministic starting order - by file name, so
                              // "01 ...", "02 ..." land in sequence; Move up / down
                              // then sets the real album order.
                              std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
                                  return a.getFileName().compareNatural(b.getFileName()) < 0;
                              });
                              self->trackPaths_.clear();
                              self->trackSessionIds_.clear();
                              self->projectId_.clear();
                              for (const auto& f : files)
                                  self->trackPaths_.add(f.getFullPathName());
                              self->selectedTrack_ = -1;
                              self->runAnalysis();
                          });
}

void AlbumAssemblyPanel::moveSelected(int delta)
{
    const int from = selectedTrack_;
    const int to = from + delta;
    if (from < 0 || to < 0 || from >= trackPaths_.size() || to >= trackPaths_.size()) return;
    trackPaths_.move(from, to);
    // A project's order is its album sequence: save it (FEAT-6).
    if (!projectId_.empty() && trackSessionIds_.size() == (size_t)trackPaths_.size()) {
        std::swap(trackSessionIds_[(size_t)from], trackSessionIds_[(size_t)to]);
        if (auto db = controller_.sessions().sessionManagerPtr()) {
            juce::Component::SafePointer<AlbumAssemblyPanel> self(this);
            controller_.runInBackground([self, db, projectId = projectId_, order = trackSessionIds_] {
                db->setProjectOrder(projectId, order);
                juce::MessageManager::callAsync([self] {
                    if (self != nullptr) self->controller_.sessions().notifyProjectsChanged();
                });
            });
        }
    }
    if ((size_t)from < tracks_.size() && (size_t)to < tracks_.size()) std::swap(tracks_[(size_t)from], tracks_[(size_t)to]);
    selectedTrack_ = to;
    repaint();
    runAnalysis(); // the seam flags depend on which tracks are neighbours
}

juce::Rectangle<int> AlbumAssemblyPanel::matrixArea() const
{
    auto b = bodyArea();
    b.removeFromTop(28);
    b.removeFromTop(16);
    b.removeFromBottom(56);
    b.removeFromTop(16); // column header + gap
    return b;
}

void AlbumAssemblyPanel::mouseDown(const juce::MouseEvent& e)
{
    const auto area = matrixArea();
    if (tracks_.empty() || !area.contains(e.getPosition())) {
        PanelCard::mouseDown(e);
        return;
    }
    const int rowH = juce::jmax(16, juce::jmin(26, area.getHeight() / juce::jmax(1, (int)tracks_.size())));
    const int row = (e.getPosition().y - area.getY()) / rowH;
    selectedTrack_ = row < (int)tracks_.size() ? row : -1;
    upButton_.setEnabled(selectedTrack_ > 0);
    downButton_.setEnabled(selectedTrack_ >= 0 && selectedTrack_ < (int)tracks_.size() - 1);
    repaint();
}

void AlbumAssemblyPanel::runAnalysis()
{
    statusLabel_.setText(TRANS("Measuring %n tracks...").replace("%n", juce::String(trackPaths_.size())),
                         juce::dontSendNotification);
    chooseButton_.setEnabled(false);
    upButton_.setEnabled(false);
    downButton_.setEnabled(false);
    const int requestId = ++analysisRequest_;

    std::vector<std::string> paths;
    for (const auto& p : trackPaths_)
        paths.push_back(p.toStdString());

    juce::Component::SafePointer<AlbumAssemblyPanel> self(this);
    // Decodes every track: the long pool (WIRE-04). Not epoch-guarded: the
    // album is the user's own file list, not session data, so a session
    // switch must not leave "Measuring N tracks..." up forever (UIX-17).
    controller_.runLongInBackground([self, paths, requestId]() {
        ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 600000);
        auto result = bridge.analyzeAlbum(paths);
        juce::MessageManager::callAsync([self, result, requestId]() {
            if (self == nullptr) return;
            if (requestId != self->analysisRequest_) return; // a newer order / track list is being measured
            self->chooseButton_.setEnabled(true);
            self->upButton_.setEnabled(self->selectedTrack_ > 0);
            self->downButton_.setEnabled(self->selectedTrack_ >= 0
                                         && self->selectedTrack_ < self->trackPaths_.size() - 1);
            if (result.is_err()) {
                self->statusLabel_.setText(TRANS("Analysis failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            self->tracks_.clear();
            if (auto* arr = v[af::AlbumResponse::tracks].getArray()) {
                for (const auto& tv : *arr) {
                    Track t;
                    t.name = juce::File(tv[af::AlbumTrack::path].toString()).getFileNameWithoutExtension();
                    t.lufs = (float)(double)tv[af::AlbumTrack::lufs];
                    t.crest = (float)(double)tv[af::AlbumTrack::crest_db];
                    t.tilt = (float)(double)tv[af::AlbumTrack::tilt_db_per_octave];
                    self->tracks_.push_back(t);
                }
            }
            self->verdict_ = v[af::AlbumResponse::verdict].toString();

            juce::StringArray lines;
            if (auto* arr = v[af::AlbumResponse::flags].getArray())
                for (size_t i = 0; i < arr->size() && i < 3; ++i)
                    lines.add(arr->getReference((int)i)[af::AlbumFlag::message].toString());
            if (auto* arr = v[af::AlbumResponse::flags].getArray(); arr != nullptr && arr->size() > 3)
                lines.add(TRANS("+%n more").replace("%n", juce::String(arr->size() - 3)));
            self->flagsLabel_.setColour(juce::Label::textColourId,
                                        lines.isEmpty() ? KhensuTheme::accentTeal : KhensuTheme::accentAmber);
            self->flagsLabel_.setText(lines.isEmpty() ? TRANS("No seams flagged.") : lines.joinIntoString("\n"),
                                      juce::dontSendNotification);

            self->statusLabel_.setText(TRANS("Verdict: %v  -  LUFS spread %s LU")
                                           .replace("%v", self->verdict_)
                                           .replace("%s", juce::String((float)(double)v[af::AlbumResponse::lufs_spread], 1)),
                                       juce::dontSendNotification);
            self->repaint();
        });
    });
}

namespace {
// Median of a copy - small N (album tracks), so a sort is fine.
float medianOf(std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return (v.size() % 2 == 0) ? 0.5f * (v[m - 1] + v[m]) : v[m];
}
// Cell colour by absolute deviation from the album median: teal (consistent)
// -> amber (a seam) -> red (jarring). Thresholds mirror the backend's flag
// gates so the matrix and the flag list agree.
juce::Colour cellColour(float dev, float warnT, float badT)
{
    const float a = std::abs(dev);
    if (a >= badT) return KhensuTheme::accentRed.withAlpha(juce::jlimit(0.30f, 0.70f, a / (badT * 2.0f)));
    if (a >= warnT) return KhensuTheme::accentAmber.withAlpha(0.45f);
    return KhensuTheme::accentTeal.withAlpha(juce::jlimit(0.10f, 0.30f, 0.30f - a / warnT * 0.20f));
}
} // namespace

void AlbumAssemblyPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    if (tracks_.empty()) return;

    auto b = bodyArea();
    b.removeFromTop(28);
    b.removeFromTop(16);
    b.removeFromBottom(56);

    // Album medians per metric - the matrix colours deviation from these.
    std::vector<float> lv, cv, tv;
    lv.reserve(tracks_.size());
    cv.reserve(tracks_.size());
    tv.reserve(tracks_.size());
    for (const auto& t : tracks_) {
        lv.push_back(t.lufs);
        cv.push_back(t.crest);
        tv.push_back(t.tilt);
    }
    const float medL = medianOf(lv), medC = medianOf(cv), medT = medianOf(tv);

    const int nameW = juce::jmin(150, b.getWidth() / 3);
    const int cellW = juce::jmax(48, (b.getWidth() - nameW) / 3);

    // Column header row.
    auto header = b.removeFromTop(14);
    header.removeFromLeft(nameW);
    g.setFont(KhensuTheme::labelFont(8.5f));
    g.setColour(KhensuTheme::textTertiary);
    const char* cols[3] = {"LUFS", "TILT", "CREST"};
    for (int c = 0; c < 3; ++c)
        g.drawText(cols[c], header.removeFromLeft(cellW).reduced(3, 0), juce::Justification::centred, false);
    b.removeFromTop(2);

    const int rowH = juce::jmax(16, juce::jmin(26, b.getHeight() / juce::jmax(1, (int)tracks_.size())));

    int i = 0;
    for (const auto& t : tracks_) {
        auto row = b.removeFromTop(rowH).reduced(0, 2);
        if (row.isEmpty()) break;
        if (i == selectedTrack_) {
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.12f));
            g.fillRoundedRectangle(row.toFloat(), 3.0f);
        }
        g.setFont(KhensuTheme::labelFont(9.5f));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(juce::String(i + 1) + ". " + t.name, row.removeFromLeft(nameW), juce::Justification::centredLeft,
                   true);

        struct Cell {
            float dev, warn, bad;
            juce::String text;
        };
        const Cell cells[3] = {
            {t.lufs - medL, 2.0f, 4.0f, juce::String(t.lufs, 1)},
            {t.tilt - medT, 1.5f, 3.0f, juce::String(t.tilt, 1)},
            {t.crest - medC, 4.0f, 8.0f, juce::String(juce::roundToInt(t.crest))},
        };
        for (const auto& cell : cells) {
            auto cb = row.removeFromLeft(cellW).reduced(2, 1);
            g.setColour(cellColour(cell.dev, cell.warn, cell.bad));
            g.fillRoundedRectangle(cb.toFloat(), 3.0f);
            g.setFont(KhensuTheme::monoFont(9.0f));
            g.setColour(KhensuTheme::textPrimary);
            g.drawText(cell.text, cb, juce::Justification::centred, false);
        }
        ++i;
    }
}

void AlbumAssemblyPanel::runNormalizationSim()
{
    if (trackPaths_.isEmpty()) {
        flagsLabel_.setText(TRANS("Add tracks first."), juce::dontSendNotification);
        return;
    }
    normSimButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Simulating platform normalization..."), juce::dontSendNotification);
    const juce::StringArray paths = trackPaths_;
    juce::Component::SafePointer<AlbumAssemblyPanel> self(this);
    // Decodes every track: the long pool (WIRE-04); album data is global,
    // not session-scoped, so no epoch guard (UIX-17).
    controller_.runLongInBackground([self, paths]() {
        ipc::HttpServiceBridge bridge(ipc::HttpServiceBridge::kDefaultBaseUrl, 600000);
        auto res = bridge.albumNormalization(paths, "spotify");
        juce::MessageManager::callAsync([self, res]() {
            if (self == nullptr) return;
            self->normSimButton_.setEnabled(true);
            self->statusLabel_.setText(TRANS("Spotify track-mode vs album-mode loudness jumps."), juce::dontSendNotification);
            if (res.is_err()) {
                self->flagsLabel_.setText(TRANS("Normalization sim unavailable."), juce::dontSendNotification);
                return;
            }
            auto v = res.value();
            juce::StringArray lines;
            lines.add(TRANS("Album %l LUFS, gain %g dB. Max album-mode jump: %j LU")
                          .replace("%l", juce::String((double)v[af::AlbumNormalizationResponse::album_lufs], 1))
                          .replace("%g", juce::String((double)v[af::AlbumNormalizationResponse::album_gain_db], 1))
                          .replace("%j", juce::String((double)v[af::AlbumNormalizationResponse::summary][af::AlbumNormalizationSummary::max_album_mode_jump_lu], 1))
                      + ((bool)v[af::AlbumNormalizationResponse::summary][af::AlbumNormalizationSummary::biggest_jump_notable]
                             ? "  (!) " + TRANS("notable")
                             : juce::String()));
            if (auto* arr = v[af::AlbumNormalizationResponse::tracks].getArray())
                for (auto& t : *arr) {
                    const auto jump = t[af::AlbumNormalizationTrack::album_mode_jump_lu];
                    lines.add("  " + t[af::AlbumNormalizationTrack::name].toString() + ": " + juce::String((double)t[af::AlbumNormalizationTrack::lufs], 1) + " LUFS" +
                              (jump.isVoid() ? juce::String() : "  jump " + juce::String((double)jump, 1) + " LU"));
                }
            self->flagsLabel_.setText(lines.joinIntoString("\n"), juce::dontSendNotification);
        });
    });
}

void AlbumAssemblyPanel::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto controls = b.removeFromTop(24);
    chooseButton_.setBounds(controls.removeFromLeft(150));
    controls.removeFromLeft(6);
    normSimButton_.setBounds(controls.removeFromLeft(130));
    controls.removeFromLeft(6);
    masterButton_.setBounds(controls.removeFromLeft(120));
    controls.removeFromLeft(6);
    upButton_.setBounds(controls.removeFromLeft(70));
    controls.removeFromLeft(4);
    downButton_.setBounds(controls.removeFromLeft(80));
    b.removeFromTop(2);
    statusLabel_.setBounds(b.removeFromTop(14));
    flagsLabel_.setBounds(b.removeFromBottom(72));
}

} // namespace khensu
