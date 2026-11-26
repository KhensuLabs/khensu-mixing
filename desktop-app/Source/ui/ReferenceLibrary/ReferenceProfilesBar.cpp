// desktop-app/Source/ui/ReferenceLibrary/ReferenceProfilesBar.cpp
#include "ReferenceProfilesBar.h"
#include "../../IBackgroundWork.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace apif = api::fields;
// The collab ReferenceProfile schema shares its short name with an analyzer
// model, so the generator qualifies it; one alias keeps a rename a one-line fix.
namespace rpf = api::fields::UserReferenceProfile;

void ReferenceProfilesBar::applyThemeColours()
{
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

ReferenceProfilesBar::ReferenceProfilesBar(IBackgroundWork& controller, std::function<juce::String()> selectedGenre,
                                           std::string baseUrl)
    : controller_(controller), selectedGenre_(std::move(selectedGenre)), baseUrl_(std::move(baseUrl))
{
    applyThemeColours();
    analyseButton_.setTooltip(TRANS("Measure a reference track and keep its numbers (never its audio) in your library"));
    exportButton_.setTooltip(TRANS("Save your reference profiles as a hashed JSON file to share"));
    importButton_.setTooltip(TRANS("Add profiles from a shared JSON file (the hash is checked; numbers only)"));
    browseButton_.setTooltip(TRANS("Browse a shared profile index at an https URL (needs the online switches)"));
    statusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    statusLabel_.setMinimumHorizontalScale(1.0f);
    analyseButton_.onClick = [this] { analyseReference(); };
    exportButton_.onClick = [this] { exportProfiles(); };
    importButton_.onClick = [this] { importProfiles(); };
    browseButton_.onClick = [this] { browseIndex(); };
    for (auto* c : std::initializer_list<juce::Component*>{&analyseButton_, &exportButton_, &importButton_,
                                                            &browseButton_, &statusLabel_})
        addAndMakeVisible(c);
    addChildComponent(experimentalBadge_);
    experimentalBadge_.forFeature("reference_profiles");
}

ReferenceProfilesBar::~ReferenceProfilesBar() = default;

void ReferenceProfilesBar::resized()
{
    auto b = getLocalBounds();
    auto row = experimentalBadge_.placeAtRight(b.removeFromTop(26));
    analyseButton_.setBounds(row.removeFromLeft(140).reduced(1));
    exportButton_.setBounds(row.removeFromLeft(124).reduced(1));
    importButton_.setBounds(row.removeFromLeft(124).reduced(1));
    browseButton_.setBounds(row.removeFromLeft(116).reduced(1));
    statusLabel_.setBounds(b);
}

void ReferenceProfilesBar::setStatus(const juce::String& text) { statusLabel_.setText(text, juce::dontSendNotification); }

void ReferenceProfilesBar::runPost(const std::string& route, juce::var body,
                                   std::function<void(const juce::var&)> onOk)
{
    setStatus(TRANS("Working..."));
    juce::Component::SafePointer<ReferenceProfilesBar> self(this);
    controller_.runLongInBackground([self, route, body, onOk = std::move(onOk), baseUrl = baseUrl_] {
        const auto result = ipc::HttpServiceBridge(baseUrl).postCollab(route, body, 120000);
        juce::MessageManager::callAsync([self, result, onOk] {
            if (self == nullptr) return;
            if (result.is_err()) {
                const auto& e = result.error();
                self->setStatus(e.http_status == 403
                                    ? TRANS("Blocked by Settings: turn on the master online switch and "
                                            "'Shared reference-profile index' to use an index.")
                                    : serviceErrorLine(e));
                return;
            }
            onOk(result.value());
        });
    });
}

void ReferenceProfilesBar::refresh(const juce::String& genreId)
{
    lastGenre_ = genreId;
    juce::Component::SafePointer<ReferenceProfilesBar> self(this);
    controller_.runInBackground([self, genreId, baseUrl = baseUrl_] {
        const auto result = ipc::HttpServiceBridge(baseUrl)
                                .getCollab("/references/profiles?genre=" + juce::URL::addEscapeChars(genreId, true));
        juce::MessageManager::callAsync([self, result, genreId] {
            if (self == nullptr || self->lastGenre_ != genreId || result.is_err()) return;
            std::vector<UserProfile> profiles;
            if (auto* arr = result.value()[apif::ReferenceProfileList::profiles].getArray()) {
                for (const auto& p : *arr) {
                    const auto m = p[rpf::metrics];
                    UserProfile u;
                    u.name = p[rpf::name].toString();
                    u.point.genreId = genreId.toStdString();
                    u.point.lufs_integrated = m[apif::ReferenceProfileMetrics::lufs_integrated];
                    u.point.lra = m[apif::ReferenceProfileMetrics::lra];
                    u.point.spectral_tilt_db_per_octave = m[apif::ReferenceProfileMetrics::spectral_tilt_db_per_octave];
                    u.point.crest_factor_db = m[apif::ReferenceProfileMetrics::crest_factor_db];
                    profiles.push_back(std::move(u));
                }
            }
            if (!profiles.empty() && self->statusLabel_.getText().isEmpty())
                self->setStatus(TRANS("Your profiles in this genre: ") + juce::String((int)profiles.size()));
            if (self->onProfilesChanged) self->onProfilesChanged(genreId, std::move(profiles));
        });
    });
}

void ReferenceProfilesBar::analyseReference()
{
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Choose a reference track to measure"), juce::File{},
                                                   "*.wav;*.flac;*.aif;*.aiff;*.mp3;*.ogg;*.m4a");
    juce::Component::SafePointer<ReferenceProfilesBar> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [self](const juce::FileChooser& fc) {
                              if (self == nullptr || fc.getResult() == juce::File{}) return;
                              const auto file = fc.getResult();
                              juce::DynamicObject::Ptr o = new juce::DynamicObject();
                              o->setProperty(apif::ReferenceProfileAnalyzeRequest::file_path, file.getFullPathName());
                              o->setProperty(apif::ReferenceProfileAnalyzeRequest::name,
                                             file.getFileNameWithoutExtension());
                              o->setProperty(apif::ReferenceProfileAnalyzeRequest::genre,
                                             self->selectedGenre_ ? self->selectedGenre_() : juce::String());
                              self->runPost("/references/profiles/analyze", juce::var(o.get()),
                                            [self](const juce::var& v) {
                                                if (self == nullptr) return;
                                                self->setStatus(TRANS("Added profile: ") + v[rpf::name].toString());
                                                self->refresh(self->lastGenre_);
                                            });
                          });
}

void ReferenceProfilesBar::exportProfiles()
{
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Export reference profiles"),
                                                   juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
                                                       .getChildFile("khensu_reference_profiles.json"),
                                                   "*.json");
    juce::Component::SafePointer<ReferenceProfilesBar> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [self](const juce::FileChooser& fc) {
                              if (self == nullptr || fc.getResult() == juce::File{}) return;
                              const auto out = fc.getResult().withFileExtension(".json");
                              juce::DynamicObject::Ptr o = new juce::DynamicObject();
                              o->setProperty(apif::ReferenceProfileExportRequest::output_path, out.getFullPathName());
                              self->runPost("/references/profiles/export", juce::var(o.get()),
                                            [self](const juce::var& v) {
                                                if (self == nullptr) return;
                                                self->setStatus(
                                                    TRANS("Exported %n profile(s) to ")
                                                        .replace("%n", v[apif::ReferenceProfileExportResponse::count]
                                                                           .toString()) +
                                                    v[apif::ReferenceProfileExportResponse::output_path].toString());
                                            });
                          });
}

namespace {
juce::String importSummary(const juce::var& v)
{
    return TRANS("Imported %i profile(s), %d duplicate(s) skipped.")
        .replace("%i", v[apif::ReferenceProfileImportResponse::imported].toString())
        .replace("%d", v[apif::ReferenceProfileImportResponse::skipped_duplicates].toString());
}
} // namespace

void ReferenceProfilesBar::importProfiles()
{
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Import reference profiles"), juce::File{}, "*.json");
    juce::Component::SafePointer<ReferenceProfilesBar> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [self](const juce::FileChooser& fc) {
                              if (self == nullptr || fc.getResult() == juce::File{}) return;
                              juce::DynamicObject::Ptr o = new juce::DynamicObject();
                              o->setProperty(apif::ReferenceProfileImportRequest::path,
                                             fc.getResult().getFullPathName());
                              self->runPost("/references/profiles/import", juce::var(o.get()),
                                            [self](const juce::var& v) {
                                                if (self == nullptr) return;
                                                self->setStatus(importSummary(v));
                                                self->refresh(self->lastGenre_);
                                            });
                          });
}

void ReferenceProfilesBar::browseIndex()
{
    urlDialog_ = std::make_unique<juce::AlertWindow>(
        TRANS("Browse a shared profile index"),
        TRANS("Enter the https address of a Khensu reference-profile index. Only numbers are downloaded, and only "
              "when the online switches allow it."),
        juce::MessageBoxIconType::NoIcon);
    urlDialog_->addTextEditor("url", "https://", TRANS("Index URL"));
    urlDialog_->addButton(TRANS("Browse"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    urlDialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<ReferenceProfilesBar> self(this);
    urlDialog_->enterModalState(true, juce::ModalCallbackFunction::create([self](int result) {
        if (self == nullptr || self->urlDialog_ == nullptr) return;
        const auto url = self->urlDialog_->getTextEditorContents("url").trim();
        self->urlDialog_.reset();
        if (result != 1 || url.isEmpty()) return;
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty(apif::ReferenceIndexBrowseRequest::index_url, url);
        self->runPost("/references/index/browse", juce::var(o.get()), [self](const juce::var& v) {
            if (self == nullptr) return;
            auto* entries = v[apif::ReferenceIndexBrowseResponse::entries].getArray();
            if (entries == nullptr || entries->isEmpty()) {
                self->setStatus(TRANS("The index lists no profiles."));
                return;
            }
            self->setStatus(TRANS("%n profile pack(s) in the index.").replace("%n", juce::String(entries->size())));
            juce::PopupMenu menu;
            std::vector<std::pair<juce::String, juce::String>> picks;
            for (const auto& e : *entries) {
                juce::String text = e[apif::ReferenceIndexEntry::name].toString();
                const auto genre = e[apif::ReferenceIndexEntry::genre].toString();
                if (genre.isNotEmpty()) text << "  [" << genre << "]";
                picks.push_back({e[apif::ReferenceIndexEntry::url].toString(),
                                 e[apif::ReferenceIndexEntry::sha256].toString()});
                menu.addItem((int)picks.size(), text);
            }
            menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&self->browseButton_),
                               [self, picks](int id) {
                                   if (self == nullptr || id <= 0 || id > (int)picks.size()) return;
                                   self->fetchIndexEntry(picks[(size_t)id - 1].first, picks[(size_t)id - 1].second);
                               });
        });
    }));
}

void ReferenceProfilesBar::fetchIndexEntry(const juce::String& url, const juce::String& sha256)
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty(apif::ReferenceIndexFetchRequest::url, url);
    o->setProperty(apif::ReferenceIndexFetchRequest::sha256, sha256);
    juce::Component::SafePointer<ReferenceProfilesBar> self(this);
    runPost("/references/index/fetch", juce::var(o.get()), [self](const juce::var& v) {
        if (self == nullptr) return;
        self->setStatus(importSummary(v));
        self->refresh(self->lastGenre_);
    });
}

} // namespace khensu
