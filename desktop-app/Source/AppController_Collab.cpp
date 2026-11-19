// desktop-app/Source/AppController_Collab.cpp
// CE-COL-1: .khensu session bundles (export / import) and the revision report,
// reached from the command palette (ICommandPaletteHost::runCollabCommand).
// The service does the zip work and every safety check (collab/bundle.py);
// this file only picks files, creates the session for an imported bundle and
// reports the outcome.
#include "AppController.h"
#include "ui/shared/AbxTrialOverlay.h"
#include "ui/shared/ChainReasoningOverlay.h"
#include "ui/shared/ExplainerOverlay.h"
#include "ui/shared/MetricDiffOverlay.h"
#include "ui/shared/OnboardingWizard.h"
#include "ui/shared/PanelRunState.h"
#include "ui/shared/ReferenceTrackRitualOverlay.h"
#include "ui/shared/RevisionReportView.h"
#include "ui/shared/ServiceErrorHint.h"
#include "ui/shared/SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/session/AudioFileProber.h>

namespace apif = khensu::api::fields;

namespace khensu {

namespace {

juce::File bundleImportRoot()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Khensu")
        .getChildFile("Imported bundles");
}

juce::String errorText(const khensu::ipc::BridgeError& e) { return serviceErrorLine(e); }

} // namespace

void AppController::runCollabCommand(CollabCommand command)
{
    juce::Component::SafePointer<AppController> self(this);

    if (command == CollabCommand::CompareRevisions) {
        const juce::String original = activeSourcePath();
        if (original.isEmpty()) {
            showExplainer(TRANS("Compare revisions"), TRANS("Revisions"), TRANS("Open a session first."));
            return;
        }
        const juce::String current = renders_.currentRenderPath();
        if (current.isNotEmpty() && current != original) {
            RevisionReportView::show(*this, original, current, TRANS("Original"), TRANS("Current render"), this);
            return;
        }
        // No accepted render yet: compare the original with a file the user picks.
        auto chooser = std::make_shared<juce::FileChooser>(TRANS("Choose the newer version to compare"),
                                                           juce::File(original).getParentDirectory(),
                                                           "*.wav;*.flac;*.aif;*.aiff;*.mp3;*.ogg;*.m4a");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [self, chooser, original](const juce::FileChooser& fc) {
                                 if (self == nullptr) return;
                                 const auto picked = fc.getResult();
                                 if (!picked.existsAsFile()) return;
                                 RevisionReportView::show(*self, original, picked.getFullPathName(),
                                                          TRANS("Original"), picked.getFileName(), self.getComponent());
                             });
        return;
    }

    if (command == CollabCommand::ExportBundle) {
        if (!sessions_.state().session) {
            showExplainer(TRANS("Session bundle"), TRANS("Export"), TRANS("Open a session first."));
            return;
        }
        const juce::String sessionId(sessions_.state().session->id);
        const juce::String sessionName(sessions_.state().session->name);
        juce::StringArray sources;
        for (const auto& f : sessions_.state().session->files)
            sources.add(juce::String(f.path));
        const auto defaultFile = juce::File(activeSourcePath())
                                     .getParentDirectory()
                                     .getChildFile(juce::File::createLegalFileName(sessionName) + ".khensu");
        auto chooser = std::make_shared<juce::FileChooser>(TRANS("Export session bundle"), defaultFile, "*.khensu");
        chooser->launchAsync(
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
            [self, chooser, sessionId, sessionName, sources](const juce::FileChooser& fc) {
                if (self == nullptr) return;
                auto target = fc.getResult();
                if (target == juce::File()) return;
                if (!target.hasFileExtension(".khensu")) target = target.withFileExtension(".khensu");
                juce::AlertWindow::showYesNoCancelBox(
                    juce::MessageBoxIconType::QuestionIcon, TRANS("Include the audio?"),
                    TRANS("Include the source audio and every render in the bundle? Without audio the bundle "
                          "carries the analysis, comments and version names only (much smaller)."),
                    TRANS("With audio"), TRANS("Without audio"), TRANS("Cancel"), self.getComponent(),
                    juce::ModalCallbackFunction::create([self, target, sessionId, sessionName, sources](int choice) {
                        if (self == nullptr || choice == 0) return;
                        const bool withAudio = (choice == 1);
                        self->runLongInBackground([self, target, sessionId, sessionName, sources, withAudio,
                                                   sm = self->sessions_.sharedManager()] {
                            juce::var sessionExport;
                            if (sm != nullptr && sm->isOpen()) {
                                const auto exported = sm->exportSessionJson(sessionId.toStdString());
                                if (exported.is_ok()) sessionExport = juce::JSON::parse(juce::String(exported.value()));
                            }
                            juce::DynamicObject::Ptr body = new juce::DynamicObject();
                            body->setProperty(apif::BundleExportRequest::session_id, sessionId);
                            body->setProperty(apif::BundleExportRequest::output_path, target.getFullPathName());
                            body->setProperty(apif::BundleExportRequest::session_name, sessionName);
                            if (sessionExport.isObject())
                                body->setProperty(apif::BundleExportRequest::session_export, sessionExport);
                            juce::Array<juce::var> paths;
                            for (const auto& s : sources)
                                paths.add(s);
                            body->setProperty(apif::BundleExportRequest::source_paths, paths);
                            body->setProperty(apif::BundleExportRequest::include_audio, withAudio);
                            const auto result = ipc::HttpServiceBridge{}.postCollab("/bundle/export",
                                                                                    juce::var(body.get()));
                            juce::String message;
                            if (result.is_err()) {
                                message = TRANS("Export failed: ") + errorText(result.error());
                            } else {
                                const auto& v = result.value();
                                message = TRANS("Bundle written to:") + "\n" +
                                          v[apif::BundleExportResponse::output_path].toString() + "\n" +
                                          juce::String((int)v[apif::BundleExportResponse::comments]) +
                                          TRANS(" comment(s), ") +
                                          juce::String((int)v[apif::BundleExportResponse::versions]) +
                                          TRANS(" version(s), ") +
                                          juce::String((int)v[apif::BundleExportResponse::audio_files]) +
                                          TRANS(" audio file(s).");
                            }
                            juce::MessageManager::callAsync([self, message] {
                                if (self == nullptr) return;
                                // A finished export is reported even if the session changed:
                                // the file was written for the session the user chose.
                                self->showExplainer(TRANS("Session bundle"), TRANS("Export"), message);
                            });
                        });
                    }));
            });
        return;
    }

    // ImportBundle
    auto chooser = std::make_shared<juce::FileChooser>(TRANS("Import session bundle"), juce::File(), "*.khensu");
    chooser->launchAsync(
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [self, chooser](const juce::FileChooser& fc) {
            if (self == nullptr) return;
            const auto bundle = fc.getResult();
            if (!bundle.existsAsFile()) return;
            const auto destRoot = bundleImportRoot();
            // Step 1 onwards: extract, create the session, attach the notes.
            auto importBundle = [self, bundle, destRoot] {
                if (self == nullptr) return;
                self->runLongInBackground([self, bundle, destRoot] {
                    destRoot.createDirectory();
                    // Step 1: validate + extract the audio (no session yet).
                    juce::DynamicObject::Ptr req = new juce::DynamicObject();
                    req->setProperty(apif::BundleImportRequest::bundle_path, bundle.getFullPathName());
                    req->setProperty(apif::BundleImportRequest::dest_dir, destRoot.getFullPathName());
                    const auto extracted = ipc::HttpServiceBridge{}.postCollab("/bundle/import", juce::var(req.get()));
                    if (extracted.is_err()) {
                        const auto message = TRANS("Import failed: ") + errorText(extracted.error());
                        juce::MessageManager::callAsync([self, message] {
                            if (self != nullptr) self->showExplainer(TRANS("Session bundle"), TRANS("Import"), message);
                        });
                        return;
                    }
                    const auto& v = extracted.value();
                    juce::String name = v[apif::BundleImportResponse::session_name].toString();
                    if (name.isEmpty()) name = bundle.getFileNameWithoutExtension();
                    std::vector<AudioFileRef> files;
                    AudioFileProber prober;
                    if (const auto* list = v[apif::BundleImportResponse::extracted].getArray())
                        for (const auto& e : *list)
                            if (e[apif::BundleExtractedFile::role].toString() == "source") {
                                auto probed = prober.probe(e[apif::BundleExtractedFile::path].toString().toStdString());
                                if (probed.is_ok()) files.push_back(probed.value());
                            }

                    juce::MessageManager::callAsync([self, bundle, destRoot, name, files] {
                        if (self == nullptr) return;
                        juce::String sessionId;
                        if (!files.empty()) {
                            // Step 2: a new session for the bundle's audio.
                            if (!self->startSession(name + " " + TRANS("(imported)"), files, CreativeTarget{})) {
                                self->showExplainer(TRANS("Session bundle"), TRANS("Import"),
                                                    TRANS("The audio was extracted, but a session could not be created."));
                                return;
                            }
                            sessionId = juce::String(self->sessions_.state().session->id);
                        } else if (self->sessions_.state().session) {
                            // No audio in the bundle: attach its notes to the open session.
                            sessionId = juce::String(self->sessions_.state().session->id);
                        } else {
                            self->showExplainer(TRANS("Session bundle"), TRANS("Import"),
                                                TRANS("This bundle has no audio. Open the session it belongs to first, "
                                                      "then import it again to attach its comments and versions."));
                            return;
                        }
                        // Step 3: attach comments, versions and branches to that session.
                        self->runLongInBackground([self, &work = *self, bundle, destRoot, sessionId] {
                            juce::DynamicObject::Ptr attach = new juce::DynamicObject();
                            attach->setProperty(apif::BundleImportRequest::bundle_path, bundle.getFullPathName());
                            attach->setProperty(apif::BundleImportRequest::dest_dir, destRoot.getFullPathName());
                            attach->setProperty(apif::BundleImportRequest::session_id, sessionId);
                            const auto done = ipc::HttpServiceBridge{}.postCollab("/bundle/import", juce::var(attach.get()));
                            juce::String message;
                            if (done.is_err()) {
                                message = TRANS("The session was created, but its comments could not be attached: ") +
                                          errorText(done.error());
                            } else {
                                const auto& d = done.value();
                                message = TRANS("Imported ") +
                                          juce::String((int)d[apif::BundleImportResponse::comments_imported]) +
                                          TRANS(" comment(s) and ") +
                                          juce::String((int)d[apif::BundleImportResponse::versions_imported]) +
                                          TRANS(" version(s).");
                                const int skipped = (int)d[apif::BundleImportResponse::versions_skipped];
                                if (skipped > 0)
                                    message << " " << juce::String(skipped)
                                            << TRANS(" version(s) came without audio and were skipped.");
                            }
                            postIfCurrent(self, work, [message](AppController& app) {
                                app.showExplainer(TRANS("Session bundle"), TRANS("Import"), message);
                                app.renders_.notifyRenderTreeChanged(); // re-sync versions
                            });
                        });
                    });
                });
            };
            // Step 0 (REF-9): read the bundle's manifest first - say what it
            // holds and ask, and refuse an audio-less bundle with no session
            // open before anything is extracted.
            self->runLongInBackground([self, bundle, importBundle] {
                juce::DynamicObject::Ptr q = new juce::DynamicObject();
                q->setProperty(apif::BundleInspectRequest::bundle_path, bundle.getFullPathName());
                const auto info = ipc::HttpServiceBridge{}.postCollab("/bundle/inspect", juce::var(q.get()));
                juce::MessageManager::callAsync([self, bundle, info, importBundle] {
                    if (self == nullptr) return;
                    if (info.is_err()) {
                        self->showExplainer(TRANS("Session bundle"), TRANS("Import"),
                                            TRANS("Import failed: ") + errorText(info.error()));
                        return;
                    }
                    const auto& v = info.value();
                    const auto* audio = v[apif::BundleInspectResponse::audio].getArray();
                    const int audioCount = audio != nullptr ? audio->size() : 0;
                    if (audioCount == 0 && !self->sessions_.state().session) {
                        self->showExplainer(TRANS("Session bundle"), TRANS("Import"),
                                            TRANS("This bundle has no audio. Open the session it belongs to first, "
                                                  "then import it again to attach its comments and versions."));
                        return;
                    }
                    juce::String name = v[apif::BundleInspectResponse::session_name].toString();
                    if (name.isEmpty()) name = bundle.getFileNameWithoutExtension();
                    const juce::String summary =
                        name + "\n" + juce::String(audioCount) + TRANS(" audio file(s), ") +
                        juce::String((int)v[apif::BundleInspectResponse::comments]) + TRANS(" comment(s), ") +
                        juce::String((int)v[apif::BundleInspectResponse::versions]) + TRANS(" version(s).") +
                        (audioCount == 0 ? "\n" + TRANS("No audio: its notes attach to the open session.")
                                         : "\n" + TRANS("Its audio becomes a new session."));
                    juce::AlertWindow::showOkCancelBox(
                        juce::MessageBoxIconType::QuestionIcon, TRANS("Import session bundle?"), summary,
                        TRANS("Import"), TRANS("Cancel"), self.getComponent(),
                        juce::ModalCallbackFunction::create([importBundle](int ok) {
                            if (ok != 0) importBundle();
                        }));
                });
            });
        });
}

} // namespace khensu
