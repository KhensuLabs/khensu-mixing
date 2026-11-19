// desktop-app/Source/ui/ReferenceLibrary/SimilarReferencesPanel.cpp
#include "SimilarReferencesPanel.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <algorithm>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
juce::String backendNote(const juce::var& response)
{
    const juce::var b = response[af::EmbeddingSimilarResponse::backend];
    const juce::String name = b[af::EmbeddingBackendInfo::backend].toString();
    const juce::String reason = b[af::EmbeddingBackendInfo::reason].toString();
    const juce::String label = name == "clap" ? TRANS("CLAP embedding") : TRANS("hand-built embedding");
    return reason.isEmpty() ? label : label + " - " + reason;
}
} // namespace

void SimilarReferencesPanel::applyThemeColours()
{
    titleLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
    list_.setColour(juce::ListBox::backgroundColourId, KhensuTheme::bgBase);
    describeEditor_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgBase);
    describeEditor_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
    describeEditor_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    describeEditor_.setTextToShowWhenEmpty(TRANS("Describe a sound: warm, wide, slow..."), KhensuTheme::textTertiary);
    sessionRefLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    for (auto* b : {&addButton_, &findButton_, &useButton_, &removeButton_, &describeButton_, &libraryButton_,
                    &tagsButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.2f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    }
}

SimilarReferencesPanel::SimilarReferencesPanel(IAppController& controller, std::string baseUrl)
    : controller_(controller), baseUrl_(std::move(baseUrl))
{
    applyThemeColours();
    titleLabel_.setText(TRANS("Your references"), juce::dontSendNotification);
    titleLabel_.setFont(KhensuTheme::labelFont(13.0f));
    addAndMakeVisible(titleLabel_);
    addChildComponent(experimentalBadge_);
    experimentalBadge_.forFeature("similar_references");

    statusLabel_.setFont(KhensuTheme::bodyFont(11.0f));
    statusLabel_.setJustificationType(juce::Justification::topLeft);
    statusLabel_.setMinimumHorizontalScale(1.0f);
    statusLabel_.setText(TRANS("Add your reference tracks, then find the ones closest to this mix."),
                         juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);

    addButton_.setTooltip(TRANS("Index reference audio files on this machine for similarity search"));
    findButton_.setTooltip(TRANS("Rank the indexed references by how similar they sound to the loaded track"));
    useButton_.setTooltip(TRANS("Make the selected reference the session's reference track"));
    addButton_.onClick = [this] { addReferences(); };
    findButton_.onClick = [this] { findSimilar(); };
    useButton_.onClick = [this] { useSelectedAsReference(); };
    useButton_.setEnabled(false);
    removeButton_.setTooltip(TRANS("Remove the selected track from your reference library (the file itself is kept)"));
    removeButton_.onClick = [this] { removeSelected(); };
    removeButton_.setEnabled(false);
    for (auto* b : {&addButton_, &findButton_, &useButton_, &removeButton_})
        addAndMakeVisible(*b);

    // FEAT-5: tags and a genre per library entry, and a filter over them.
    tagsButton_.setTooltip(TRANS("Tag the selected reference and give it a genre"));
    tagsButton_.onClick = [this] { editSelectedTags(); };
    tagsButton_.setEnabled(false);
    addAndMakeVisible(tagsButton_);
    filterBox_.setTitle(TRANS("Show references with a tag or genre"));
    filterBox_.setTooltip(TRANS("Limit the library, Find similar and the description search to a tag or genre"));
    filterBox_.onChange = [this] { showLibrary(); };
    rebuildFilterChoices();
    addAndMakeVisible(filterBox_);

    // NEW-5: find references by description (/embeddings/search_text).
    describeEditor_.setTitle(TRANS("Describe the sound you are looking for"));
    describeEditor_.setTooltip(TRANS("Search your reference library by description - needs the CLAP model"));
    describeEditor_.onReturnKey = [this] { searchByDescription(); };
    addAndMakeVisible(describeEditor_);
    describeButton_.setTooltip(TRANS("Rank your references by how well they match the description"));
    describeButton_.onClick = [this] { searchByDescription(); };
    addAndMakeVisible(describeButton_);

    // FEAT-5: the library itself, and the session's pick.
    sessionRefLabel_.setFont(KhensuTheme::labelFont(11.0f));
    addAndMakeVisible(sessionRefLabel_);
    libraryButton_.setTooltip(TRANS("List every reference in your library"));
    libraryButton_.onClick = [this] { showLibrary(); };
    addAndMakeVisible(libraryButton_);
    refreshSessionReference();

    list_.setModel(this);
    list_.setRowHeight(22);
    list_.setTitle(TRANS("Similar reference tracks"));
    addAndMakeVisible(list_);
}

SimilarReferencesPanel::~SimilarReferencesPanel() { list_.setModel(nullptr); }

void SimilarReferencesPanel::resetForSession()
{
    rows_.clear();
    list_.updateContent();
    useButton_.setEnabled(false);
    removeButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Add your reference tracks, then find the ones closest to this mix."),
                         juce::dontSendNotification);
    repaint();
}

void SimilarReferencesPanel::setBusy(bool busy, const juce::String& status)
{
    busy_ = busy;
    addButton_.setEnabled(!busy);
    findButton_.setEnabled(!busy);
    describeButton_.setEnabled(!busy);
    useButton_.setEnabled(!busy && list_.getSelectedRow() >= 0);
    removeButton_.setEnabled(!busy && list_.getSelectedRow() >= 0);
    tagsButton_.setEnabled(!busy && list_.getSelectedRow() >= 0);
    statusLabel_.setText(status, juce::dontSendNotification);
}

SimilarReferencesPanel::Row SimilarReferencesPanel::rowFrom(const juce::var& e, double similarity)
{
    Row row;
    row.id = e[af::EmbeddingMatch::id].toString();
    row.path = e[af::EmbeddingMatch::path].toString();
    row.label = e[af::EmbeddingMatch::label].toString();
    row.similarity = similarity;
    row.exists = (bool)e[af::EmbeddingMatch::exists];
    row.genre = e[af::EmbeddingMatch::genre].toString();
    if (auto* tags = e[af::EmbeddingMatch::tags].getArray())
        for (const auto& t : *tags)
            row.tags.add(t.toString());
    return row;
}

ipc::HttpServiceBridge::EmbeddingFilter SimilarReferencesPanel::currentFilter() const
{
    ipc::HttpServiceBridge::EmbeddingFilter f;
    const int i = filterBox_.getSelectedId() - 2; // id 1 = All
    if (i >= 0 && i < (int)filterChoices_.size()) {
        if (filterChoices_[(size_t)i].first)
            f.genre = filterChoices_[(size_t)i].second;
        else
            f.tag = filterChoices_[(size_t)i].second;
    }
    return f;
}

void SimilarReferencesPanel::rebuildFilterChoices()
{
    // Keep the current pick when it still exists in the library.
    const auto keep = currentFilter();
    juce::StringArray genres, tags;
    for (const auto& r : rows_) {
        if (r.genre.isNotEmpty()) genres.addIfNotAlreadyThere(r.genre);
        for (const auto& t : r.tags)
            tags.addIfNotAlreadyThere(t);
    }
    if (keep.genre.isNotEmpty()) genres.addIfNotAlreadyThere(keep.genre);
    if (keep.tag.isNotEmpty()) tags.addIfNotAlreadyThere(keep.tag);
    genres.sortNatural();
    tags.sortNatural();
    filterChoices_.clear();
    filterBox_.clear(juce::dontSendNotification);
    filterBox_.addItem(TRANS("All references"), 1);
    int selected = 1;
    for (const auto& g : genres) {
        filterChoices_.push_back({true, g});
        const int id = (int)filterChoices_.size() + 1;
        filterBox_.addItem(TRANS("Genre:") + " " + g, id);
        if (keep.genre == g) selected = id;
    }
    for (const auto& t : tags) {
        filterChoices_.push_back({false, t});
        const int id = (int)filterChoices_.size() + 1;
        filterBox_.addItem(TRANS("Tag:") + " " + t, id);
        if (keep.tag == t) selected = id;
    }
    filterBox_.setSelectedId(selected, juce::dontSendNotification);
}

void SimilarReferencesPanel::editSelectedTags()
{
    const int r = list_.getSelectedRow();
    if (r < 0 || r >= (int)rows_.size() || rows_[(size_t)r].id.isEmpty() || busy_) return;
    const Row row = rows_[(size_t)r];
    tagsDialog_ = std::make_unique<juce::AlertWindow>(TRANS("Tags and genre"), row.label,
                                                      juce::MessageBoxIconType::NoIcon, this);
    tagsDialog_->addTextEditor("tags", row.tags.joinIntoString(", "), TRANS("Tags (comma-separated)"));
    tagsDialog_->addTextEditor("genre", row.genre, TRANS("Genre"));
    tagsDialog_->addButton(TRANS("Save"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    tagsDialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<SimilarReferencesPanel> self(this);
    tagsDialog_->enterModalState(
        true, juce::ModalCallbackFunction::create([self, id = row.id](int choice) {
            if (self == nullptr || self->tagsDialog_ == nullptr) return;
            juce::StringArray tags;
            tags.addTokens(self->tagsDialog_->getTextEditorContents("tags"), ",", "\"");
            tags.trim();
            tags.removeEmptyStrings();
            const juce::String genre = self->tagsDialog_->getTextEditorContents("genre").trim();
            self->tagsDialog_.reset();
            if (choice != 1) return;
            self->setBusy(true, TRANS("Saving tags..."));
            self->controller_.runInBackground([self, id, tags, genre, baseUrl = self->baseUrl_] {
                auto result = ipc::HttpServiceBridge(baseUrl).embeddingsSetMeta(id, tags, genre);
                juce::MessageManager::callAsync([self, result, id] {
                    if (self == nullptr) return;
                    // The library is per machine, not per session: no epoch check.
                    if (result.is_err()) {
                        self->setBusy(false, classifyServiceError(result.error()).message);
                        return;
                    }
                    for (auto& row : self->rows_)
                        if (row.id == id) {
                            const auto updated = rowFrom(result.value(), row.similarity);
                            row.tags = updated.tags;
                            row.genre = updated.genre;
                        }
                    self->rebuildFilterChoices();
                    self->list_.repaint();
                    self->setBusy(false, TRANS("Tags saved."));
                });
            });
        }),
        false);
}

void SimilarReferencesPanel::addReferences()
{
    chooser_ = std::make_unique<juce::FileChooser>(TRANS("Add reference tracks"), juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a");
    juce::Component::SafePointer<SimilarReferencesPanel> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectMultipleItems,
                          [self](const juce::FileChooser& fc) {
                              if (self == nullptr) return;
                              juce::StringArray paths;
                              for (const auto& f : fc.getResults())
                                  if (f.existsAsFile()) paths.add(f.getFullPathName());
                              if (paths.isEmpty()) return;
                              self->setBusy(true, TRANS("Indexing references..."));
                              self->controller_.runInBackground([self, paths, baseUrl = self->baseUrl_] {
                                  auto result = ipc::HttpServiceBridge(baseUrl).embeddingsIndex(paths);
                                  juce::MessageManager::callAsync([self, result] {
                                      if (self == nullptr) return;
                                      // The index is per machine, not per session: no epoch check.
                                      if (result.is_err()) {
                                          self->setBusy(false, classifyServiceError(result.error()).message);
                                          return;
                                      }
                                      const juce::var v = result.value();
                                      int ok = 0, failed = 0;
                                      if (auto* arr = v[af::EmbeddingIndexResponse::results].getArray())
                                          for (const auto& r : *arr)
                                              (r[af::EmbeddingIndexResult::status].toString() == "failed" ? failed
                                                                                                          : ok)++;
                                      juce::String msg = juce::String(ok) + TRANS(" indexed");
                                      if (failed > 0) msg << ", " << failed << TRANS(" failed");
                                      msg << " (" << (int)v[af::EmbeddingIndexResponse::index_size]
                                          << TRANS(" in library)");
                                      self->setBusy(false, msg);
                                  });
                              });
                          });
}

void SimilarReferencesPanel::findSimilar()
{
    const juce::String path = controller_.activeSourcePath();
    if (path.isEmpty()) {
        statusLabel_.setText(TRANS("Load a session first."), juce::dontSendNotification);
        return;
    }
    setBusy(true, TRANS("Comparing with your references..."));
    fetchQuietly(*this, controller_, [path, filter = currentFilter(), baseUrl = baseUrl_] {
        auto result = ipc::HttpServiceBridge(baseUrl).embeddingsSimilar(path, 10, filter);
        return result;
    }, [](SimilarReferencesPanel& self, auto&& result) {
        if (result.is_err()) {
            self.statusLabel_.setText(classifyServiceError(result.error()).message, juce::dontSendNotification);
            return;
        }
        const juce::var v = result.value();
        self.rows_.clear();
        if (auto* arr = v[af::EmbeddingSimilarResponse::results].getArray())
            for (const auto& r : *arr)
                self.rows_.push_back(rowFrom(r, (double)r[af::EmbeddingMatch::similarity]));
        self.list_.updateContent();
        self.list_.deselectAllRows();
        const int indexed = (int)v[af::EmbeddingSimilarResponse::index_size];
        self.statusLabel_.setText(indexed == 0 ? TRANS("No references indexed yet - add some first.")
                                                : backendNote(v),
                                   juce::dontSendNotification);
        self.repaint();
    }, [](SimilarReferencesPanel& self) { self.setBusy(false, {}); });
}

void SimilarReferencesPanel::searchByDescription()
{
    const juce::String text = describeEditor_.getText().trim();
    if (text.isEmpty() || busy_) return;
    setBusy(true, TRANS("Searching your references..."));
    juce::Component::SafePointer<SimilarReferencesPanel> self(this);
    controller_.runInBackground([self, text, filter = currentFilter(), baseUrl = baseUrl_] {
        auto result = ipc::HttpServiceBridge(baseUrl).embeddingsSearchText(text, 10, filter);
        juce::MessageManager::callAsync([self, result] {
            if (self == nullptr) return;
            // The library is per machine, not per session: no epoch check.
            self->setBusy(false, {});
            if (result.is_err()) {
                // 503 = the CLAP backend is not available: say what is needed.
                const auto& e = result.error();
                self->statusLabel_.setText(e.http_status == 503
                                               ? TRANS("Searching by description needs the CLAP model "
                                                       "(the AI detection component).")
                                               : classifyServiceError(e).message,
                                           juce::dontSendNotification);
                return;
            }
            self->showMatches(result.value());
        });
    });
}

void SimilarReferencesPanel::showMatches(const juce::var& v)
{
    rows_.clear();
    if (auto* arr = v[af::EmbeddingSimilarResponse::results].getArray())
        for (const auto& r : *arr)
            rows_.push_back(rowFrom(r, (double)r[af::EmbeddingMatch::similarity]));
    list_.updateContent();
    list_.deselectAllRows();
    const int indexed = (int)v[af::EmbeddingSimilarResponse::index_size];
    statusLabel_.setText(indexed == 0 ? TRANS("No references indexed yet - add some first.") : backendNote(v),
                         juce::dontSendNotification);
    repaint();
}

void SimilarReferencesPanel::refreshSessionReference()
{
    const juce::String ref = controller_.analysis().getReferenceTrack();
    sessionRefLabel_.setText(ref.isEmpty() ? TRANS("This session has no reference track yet.")
                                           : TRANS("Session reference:") + " " + juce::File(ref).getFileName(),
                             juce::dontSendNotification);
}

void SimilarReferencesPanel::showLibrary()
{
    refreshSessionReference();
    if (busy_) return;
    setBusy(true, TRANS("Reading your library..."));
    juce::Component::SafePointer<SimilarReferencesPanel> self(this);
    const auto filter = currentFilter();
    controller_.runInBackground([self, filter, baseUrl = baseUrl_] {
        auto result = ipc::HttpServiceBridge(baseUrl).embeddingsList(filter);
        juce::MessageManager::callAsync([self, result, filtered = filter.tag.isNotEmpty() || filter.genre.isNotEmpty()] {
            if (self == nullptr) return;
            // The library is per machine, not per session: no epoch check.
            self->setBusy(false, {});
            if (result.is_err()) {
                self->statusLabel_.setText(classifyServiceError(result.error()).message, juce::dontSendNotification);
                return;
            }
            const juce::var v = result.value();
            self->rows_.clear();
            if (auto* arr = v[af::EmbeddingIndexListResponse::entries].getArray())
                for (const auto& e : *arr)
                    self->rows_.push_back(rowFrom(e, -1.0)); // a library row, not a match
            if (!filtered) self->rebuildFilterChoices(); // the whole library: every tag and genre
            self->list_.updateContent();
            self->list_.deselectAllRows();
            const int n = (int)self->rows_.size();
            self->statusLabel_.setText(n == 0 ? TRANS("Your library is empty - add reference tracks.")
                                              : juce::String(n) + " " + TRANS("references in your library. Find "
                                                                              "similar ranks them against this mix."),
                                       juce::dontSendNotification);
            self->repaint();
        });
    });
}

void SimilarReferencesPanel::useSelectedAsReference()
{
    const int r = list_.getSelectedRow();
    if (r < 0 || r >= (int)rows_.size()) return;
    if (!rows_[(size_t)r].exists) {
        statusLabel_.setText(TRANS("That file has moved or been deleted."), juce::dontSendNotification);
        return;
    }
    controller_.analysis().setReferenceTrack(rows_[(size_t)r].path);
    statusLabel_.setText(TRANS("Reference set: ") + rows_[(size_t)r].label, juce::dontSendNotification);
    refreshSessionReference();
}

void SimilarReferencesPanel::removeSelected()
{
    const int r = list_.getSelectedRow();
    if (r < 0 || r >= (int)rows_.size() || rows_[(size_t)r].id.isEmpty()) return;
    const juce::String id = rows_[(size_t)r].id;
    const juce::String label = rows_[(size_t)r].label;
    setBusy(true, TRANS("Removing from your library..."));
    juce::Component::SafePointer<SimilarReferencesPanel> self(this);
    controller_.runInBackground([self, id, label, baseUrl = baseUrl_] {
        auto result = ipc::HttpServiceBridge(baseUrl).embeddingsRemove(juce::StringArray{id});
        juce::MessageManager::callAsync([self, result, id, label] {
            if (self == nullptr) return;
            // The index is per machine, not per session: no epoch check.
            if (result.is_err()) {
                self->setBusy(false, classifyServiceError(result.error()).message);
                return;
            }
            self->rows_.erase(std::remove_if(self->rows_.begin(), self->rows_.end(),
                                             [&id](const Row& row) { return row.id == id; }),
                              self->rows_.end());
            self->list_.updateContent();
            self->list_.deselectAllRows();
            self->setBusy(false, TRANS("Removed: ") + label + " (" +
                                     juce::String((int)result.value()[af::EmbeddingRemoveResponse::index_size]) +
                                     TRANS(" in library)"));
        });
    });
}

int SimilarReferencesPanel::getNumRows() { return (int)rows_.size(); }

void SimilarReferencesPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= (int)rows_.size()) return;
    const auto& r = rows_[(size_t)row];
    g.fillAll(selected ? KhensuTheme::bgRaised : KhensuTheme::bgBase);
    const bool isMatch = r.similarity >= 0.0; // a library listing has no score (FEAT-5)
    const int pctW = isMatch ? 52 : 0;
    const float sim = (float)juce::jlimit(0.0, 1.0, r.similarity);
    if (isMatch) {
        g.setColour(KhensuTheme::accentTeal.withAlpha(0.25f));
        g.fillRect(juce::Rectangle<float>((float)(width - pctW), 4.0f, (float)pctW * sim, (float)height - 8.0f));
    }
    g.setColour(r.exists ? (selected ? KhensuTheme::textPrimary : KhensuTheme::textSecondary)
                         : KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::bodyFont(12.0f));
    juce::String meta = r.genre;
    if (!r.tags.isEmpty()) meta << (meta.isEmpty() ? "" : " - ") << r.tags.joinIntoString(", ");
    const int textW = width - pctW - 14;
    const int metaW = meta.isEmpty() ? 0 : juce::jmin(textW / 2, 220);
    g.drawText(r.label, 8, 0, textW - metaW, height, juce::Justification::centredLeft, true);
    if (metaW > 0) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(11.0f));
        g.drawText(meta, 8 + textW - metaW, 0, metaW, height, juce::Justification::centredRight, true);
    }
    if (!isMatch) return;
    g.setFont(KhensuTheme::metricFont(11.0f));
    g.drawText(juce::String(juce::roundToInt(sim * 100.0f)) + "%", width - pctW, 0, pctW - 4, height,
               juce::Justification::centredRight);
}

void SimilarReferencesPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    list_.selectRow(row);
    useSelectedAsReference();
}

void SimilarReferencesPanel::selectedRowsChanged(int lastRowSelected)
{
    useButton_.setEnabled(!busy_ && lastRowSelected >= 0);
    removeButton_.setEnabled(!busy_ && lastRowSelected >= 0);
    tagsButton_.setEnabled(!busy_ && lastRowSelected >= 0);
}

void SimilarReferencesPanel::paint(juce::Graphics& g)
{
    g.setColour(KhensuTheme::bgSurface);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
}

void SimilarReferencesPanel::resized()
{
    auto b = getLocalBounds().reduced(8);
    auto header = b.removeFromTop(24);
    removeButton_.setBounds(header.removeFromRight(80));
    header.removeFromRight(6);
    useButton_.setBounds(header.removeFromRight(120));
    header.removeFromRight(6);
    findButton_.setBounds(header.removeFromRight(100));
    header.removeFromRight(6);
    addButton_.setBounds(header.removeFromRight(130));
    titleLabel_.setBounds(experimentalBadge_.placeAtRight(header));
    b.removeFromTop(4);
    auto sessionRow = b.removeFromTop(22);
    tagsButton_.setBounds(sessionRow.removeFromRight(70));
    sessionRow.removeFromRight(6);
    filterBox_.setBounds(sessionRow.removeFromRight(170));
    sessionRow.removeFromRight(6);
    libraryButton_.setBounds(sessionRow.removeFromRight(120));
    sessionRow.removeFromRight(6);
    sessionRefLabel_.setBounds(sessionRow);
    b.removeFromTop(4);
    auto search = b.removeFromTop(24);
    describeButton_.setBounds(search.removeFromRight(80));
    search.removeFromRight(6);
    describeEditor_.setBounds(search);
    b.removeFromTop(4);
    statusLabel_.setBounds(b.removeFromTop(30));
    list_.setBounds(b);
}

} // namespace khensu
