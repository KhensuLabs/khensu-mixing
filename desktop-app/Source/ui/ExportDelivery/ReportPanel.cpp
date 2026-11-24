// desktop-app/Source/ui/ExportDelivery/ReportPanel.cpp
#include "ReportPanel.h"
#include "VisualReport.h"
#include "../../analysis/ResultDerivedData.h"
#include "../shared/PanelRunState.h"
#include "../shared/TimelineCache.h"
#include <khensu/targets/GenreDatabase.h>
#include "../shared/PanelRunState.h"
#include "../shared/KhensuTheme.h"
#include "../../IAppController.h"
#include "../../PythonServiceManager.h"
#include "../shared/ServiceErrorHint.h"
#include "../shared/SessionEpoch.h"
#include "EvidenceReportBody.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

// The panel's RESTING palette colours, in one place (UI-01).
//
// statusLabel_/pdfStatusLabel_ are also tinted transiently by the generate and
// PDF paths (green on success, red on failure). Those are deliberately not
// restored here: re-applying the resting tone would wipe a success/failure
// state the user is still reading. They re-resolve from the palette on the next
// status change, so at worst a completed status keeps the previous theme's
// green/red until the next action.
void ReportPanel::applyThemeColours()
{
    quickExportButton_.setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.18f));
    quickExportButton_.setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
    modeLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    outputDirLabel_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    outputPathDisplay_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
}

ReportPanel::ReportPanel(IAppController& c) : PanelCard(c, "REPORT")
{
    applyThemeColours();
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    pdfStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
    setExplainer(
        TRANS("Delivery"),
        TRANS("Generate an HTML or PDF analysis report from the current session's analysis data.\n\nQuick Export: one click "
        "saves a report to your Desktop and opens it in the browser immediately.\n\nBrowse + Generate HTML: choose a "
        "custom save location.\n\nSave as PDF: requires the analysis engine to be running."));

    quickExportButton_.onClick = [this]() { onQuickExport(); };
    addAndMakeVisible(quickExportButton_);

    modeLabel_.setText(TRANS("Report Mode:"), juce::dontSendNotification);
    modeLabel_.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::bold));
    addAndMakeVisible(modeLabel_);

    modeControl_.addSegment("CLIENT SUMMARY");
    modeControl_.addSegment("ENGINEER DETAIL");
    // UX-9: one detail level for the whole app - this control shows and sets it.
    modeControl_.setSelected(controller_.shell().getReportMode() == ReportMode::EngineerDetail ? 1 : 0);
    modeControl_.onChange = [this](int i) {
        controller_.shell().setReportMode(i == 1 ? ReportMode::EngineerDetail : ReportMode::ClientSummary);
    };
    reportModeListener_ = listen(controller_.shell(), this, &IShell::addReportModeListener,
                                 &IShell::removeReportModeListener, [this](ReportMode mode) {
                                     modeControl_.setSelected(mode == ReportMode::EngineerDetail ? 1 : 0);
                                 });
    addAndMakeVisible(modeControl_);

    outputDirLabel_.setText(TRANS("Output file:"), juce::dontSendNotification);
    outputDirLabel_.setFont(khensu::KhensuTheme::uiFont(12.0f, juce::Font::bold));
    addAndMakeVisible(outputDirLabel_);

    outputPathDisplay_.setText(TRANS("(no output path chosen)"), juce::dontSendNotification);
    outputPathDisplay_.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    addAndMakeVisible(outputPathDisplay_);

    chooseOutputButton_.onClick = [this]() { onChooseOutput(); };
    addAndMakeVisible(chooseOutputButton_);

    generateButton_.onClick = [this]() { onGenerate(); };
    addAndMakeVisible(generateButton_);

    evidenceButton_.setTooltip(TRANS("Save every recommendation with the measurements, limits and limit sources "
                                     "behind it, as a client-readable HTML page (plus JSON)"));
    evidenceButton_.onClick = [this]() { onExportEvidence(); };
    addAndMakeVisible(evidenceButton_);

    statusLabel_.setText(TRANS("Idle"), juce::dontSendNotification);
    statusLabel_.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    addAndMakeVisible(statusLabel_);

    openButton_.setEnabled(false);
    openButton_.onClick = [this]() { onOpen(); };
    addAndMakeVisible(openButton_);

    savePdfButton_.onClick = [this]() { onSavePdf(); };
    addAndMakeVisible(savePdfButton_);

    visualButton_.setTooltip(TRANS("Save one HTML page with the charts - tonal balance against released tracks, the "
                                   "measurements, loudness over time, what the current version changed and the open "
                                   "findings - to send to a client or a collaborator"));
    visualButton_.onClick = [this]() { onSaveVisual(); };
    addAndMakeVisible(visualButton_);

    pdfStatusLabel_.setText("", juce::dontSendNotification);
    pdfStatusLabel_.setFont(khensu::KhensuTheme::uiFont(11.0f, juce::Font::plain));
    addAndMakeVisible(pdfStatusLabel_);
}

void ReportPanel::resized()
{
    auto b = bodyArea();
    const int rowH = 32;
    const int gap = 6;

    // Quick Export button at the very top
    quickExportButton_.setBounds(b.removeFromTop(rowH));
    b.removeFromTop(gap);

    modeLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(2);
    modeControl_.setBounds(b.removeFromTop(28));
    b.removeFromTop(gap);

    outputDirLabel_.setBounds(b.removeFromTop(18));
    b.removeFromTop(2);

    auto pathRow = b.removeFromTop(rowH);
    chooseOutputButton_.setBounds(pathRow.removeFromRight(80));
    pathRow.removeFromRight(4);
    outputPathDisplay_.setBounds(pathRow);
    b.removeFromTop(gap);

    {
        auto genRow = b.removeFromTop(rowH);
        evidenceButton_.setBounds(genRow.removeFromRight(genRow.getWidth() / 2).withTrimmedLeft(gap / 2));
        generateButton_.setBounds(genRow.withTrimmedRight(gap / 2));
    }
    b.removeFromTop(gap);

    statusLabel_.setBounds(b.removeFromTop(16));
    b.removeFromTop(gap);

    openButton_.setBounds(b.removeFromTop(rowH));
    b.removeFromTop(gap);

    savePdfButton_.setBounds(b.removeFromTop(rowH));
    b.removeFromTop(gap);
    visualButton_.setBounds(b.removeFromTop(rowH));
    b.removeFromTop(2);

    pdfStatusLabel_.setBounds(b.removeFromTop(16));
}

void ReportPanel::onChooseOutput()
{
    juce::String base = "khensu_report";
    if (const auto* s = controller_.getActiveSession()) base = juce::File::createLegalFileName(juce::String(s->name).trim()) + "_report";
    chooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Save Report As..."),
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(base + ".html"), "*.html");

    const auto fileBrowserFlags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                  juce::FileBrowserComponent::warnAboutOverwriting;

    juce::Component::SafePointer<ReportPanel> self(this);
    chooser_->launchAsync(fileBrowserFlags, [self](const juce::FileChooser& fc) {
        if (self == nullptr) return;
        auto result = fc.getResult();
        if (result != juce::File{}) {
            self->reportOutputPath_ = result.getFullPathName();
            self->outputPathDisplay_.setText(result.getFileName(), juce::dontSendNotification);
            self->outputPathDisplay_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
        }
    });
}

void ReportPanel::onSaveVisual()
{
    if (!controller_.hasActiveSession() || !controller_.hasAnalysis()) {
        statusLabel_.setText(TRANS("Analyse a track first."), juce::dontSendNotification);
        return;
    }
    juce::String base = "khensu_visual_report";
    if (const auto* s = controller_.getActiveSession()) base = juce::File::createLegalFileName(juce::String(s->name).trim()) + "_visual";
    chooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Save Visual Report As..."),
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(base + ".html"), "*.html");
    juce::Component::SafePointer<ReportPanel> self(this);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [self](const juce::FileChooser& fc) {
                              if (self != nullptr && fc.getResult() != juce::File{}) self->writeVisual(fc.getResult());
                          });
}

void ReportPanel::writeVisual(const juce::File& file)
{
    // Everything the message thread owns is read here; the decode and the
    // page are built on a worker.
    auto in = std::make_shared<visual_report::Input>();
    const auto* session = controller_.getActiveSession();
    in->trackName = session != nullptr ? juce::String(session->name) : TRANS("Track");
    std::optional<GenreEntry> genre;
    if (session != nullptr) genre = controller_.genreDatabase().findById(session->target.primary_genre);
    if (genre) {
        in->genreName = juce::String(genre->name.empty() ? genre->id : genre->name);
        in->lufsWindow = std::make_pair(genre->lufs_range_min, genre->lufs_range_max);
    }
    in->generatedAt = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M");
    const auto& derived = controller_.analysis().derivedResultData();
    in->scorecard = derived.scorecard;
    if (const auto* spread = controller_.analysis().genreSpread().find(genre ? genre->id : std::string()))
        in->scorecardSpread = spread->scorecard;
    const auto issues = controller_.issues().list();
    in->hero = hero_spectrum::build(controller_.currentAnalysis(), genre, issues, controller_.analysis().currentVerification(),
                                    &controller_.analysis().genreSpread());
    if (controller_.currentAnalysis().section_analysis) in->sections = controller_.currentAnalysis().section_analysis->sections;
    for (const auto& i : issues) {
        if (i.settled() || i.rec.type == RecommendationType::Strength) continue;
        visual_report::Finding f;
        f.title = juce::String(juce::CharPointer_UTF8((i.rec.what.empty() ? i.rec.suggested_action : i.rec.what).c_str()));
        f.action = juce::String(juce::CharPointer_UTF8(i.rec.suggested_action.c_str()));
        f.severity = i.rec.severity;
        if (i.rec.time_range) f.moment = std::make_pair(static_cast<double>(i.rec.time_range->start_sec),
                                                       static_cast<double>(i.rec.time_range->end_sec));
        in->findings.push_back(f);
    }
    std::sort(in->findings.begin(), in->findings.end(), [](const auto& a, const auto& b) { return a.severity > b.severity; });
    const auto original = controller_.activeSourcePath();
    const auto current = controller_.renders().currentRenderPath();
    if (current.isNotEmpty() && current != original) in->versionName = juce::File(current).getFileNameWithoutExtension();
    statusLabel_.setText(TRANS("Writing the visual report..."), juce::dontSendNotification);
    visualButton_.setEnabled(false);
    fetchQuietly(
        *this, controller_,
        [in, file, original, current] {
            const auto tl = timeline_cache::measure(current.isNotEmpty() ? current : original);
            in->timeline = tl.get();
            fix_difference::Result diff;
            if (current.isNotEmpty() && current != original)
                if (const auto a = timeline_cache::measure(original); a != nullptr && tl != nullptr)
                    diff = fix_difference::compute(*a, *tl);
            in->difference = diff.empty() ? nullptr : &diff;
            const bool ok = file.replaceWithText(visual_report::buildHtml(*in), false, false, "\n");
            in->timeline = nullptr;
            in->difference = nullptr;
            return ok;
        },
        [file](ReportPanel& self, bool ok) {
            self.statusLabel_.setText(ok ? TRANS("Visual report saved:") + " " + file.getFileName()
                                         : TRANS("The visual report could not be written."),
                                      juce::dontSendNotification);
            if (ok) file.startAsProcess();
        },
        [](ReportPanel& self) { self.visualButton_.setEnabled(true); }, true);
}

void ReportPanel::onGenerate()
{
    if (reportOutputPath_.isEmpty()) {
        statusLabel_.setText(TRANS("Choose an output path first."), juce::dontSendNotification);
        return;
    }
    generateButton_.setEnabled(false);
    openButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Generating..."), juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);

    juce::String reportMode = (modeControl_.getSelected() == 0) ? "client" : "engineer";
    juce::Component::SafePointer<ReportPanel> self(this);
    controller_.renders().generateReport(reportOutputPath_, reportMode, [self](bool ok, juce::String msg) {
        if (self == nullptr) return;
        self->generateButton_.setEnabled(true);
        if (ok) {
            self->statusLabel_.setText(TRANS("Saved: ") + juce::File(msg).getFileName(), juce::dontSendNotification);
            self->statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentGreen);
            self->openButton_.setEnabled(true);
        } else {
            self->statusLabel_.setText(TRANS("Error: ") + msg, juce::dontSendNotification);
            self->statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentRed);
        }
    });
}

void ReportPanel::onOpen()
{
    if (reportOutputPath_.isEmpty()) return;
    juce::File(reportOutputPath_).startAsProcess();
}

void ReportPanel::onSavePdf()
{
    if (reportOutputPath_.isEmpty()) {
        pdfStatusLabel_.setText(TRANS("Choose an output path via Browse... first."), juce::dontSendNotification);
        pdfStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentRed);
        return;
    }

    // Derive PDF path from the HTML path (same directory, .pdf extension) -
    // never silently replacing an earlier PDF (UIX-21).
    const juce::File pdfFile = juce::File(reportOutputPath_).withFileExtension("pdf");
    juce::String pdfPath = (pdfFile.exists() ? pdfFile.getNonexistentSibling(false) : pdfFile).getFullPathName();

    savePdfButton_.setEnabled(false);
    pdfStatusLabel_.setText(TRANS("Generating PDF..."), juce::dontSendNotification);
    pdfStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);

    juce::String reportMode = (modeControl_.getSelected() == 0) ? "client" : "engineer";
    juce::Component::SafePointer<ReportPanel> self(this);
    controller_.renders().generatePdf(pdfPath, reportMode, [self](bool ok, juce::String msg) {
        if (self == nullptr) return;
        self->savePdfButton_.setEnabled(true);
        if (ok) {
            self->pdfStatusLabel_.setText(TRANS("PDF saved: ") + juce::File(msg).getFileName(), juce::dontSendNotification);
            self->pdfStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentGreen);
        } else {
            self->pdfStatusLabel_.setText(TRANS("Error: ") + msg, juce::dontSendNotification);
            self->pdfStatusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentRed);
        }
    });
}

void ReportPanel::onQuickExport()
{
    // Auto-generates an HTML report to the Desktop with the session name as filename.
    // No Browse step needed - one click from any screen.
    auto desktopDir = juce::File::getSpecialLocation(juce::File::userDesktopDirectory);
    juce::String sessionName = "khensu_report";
    // createLegalFileName: "Mix: v2?" broke the old space/slash-only
    // sanitising on Windows (UIX-21).
    if (const auto* s = controller_.getActiveSession()) {
        const auto legal = juce::File::createLegalFileName(juce::String(s->name).trim().replace(" ", "_"));
        if (legal.isNotEmpty()) sessionName = legal;
    }
    // One click, no prompt - so never overwrite an earlier report silently.
    const juce::File outFile = desktopDir.getChildFile(sessionName + "_report.html");
    auto outPath = (outFile.exists() ? outFile.getNonexistentSibling(false) : outFile).getFullPathName();
    reportOutputPath_ = outPath;
    outputPathDisplay_.setText(juce::File(outPath).getFileName(), juce::dontSendNotification);
    outputPathDisplay_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);

    quickExportButton_.setEnabled(false);
    generateButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Quick export generating..."), juce::dontSendNotification);
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);

    juce::String reportMode = (modeControl_.getSelected() == 0) ? "client" : "engineer";
    juce::Component::SafePointer<ReportPanel> self(this);
    controller_.renders().generateReport(outPath, reportMode, [self](bool ok, juce::String msg) {
        if (self == nullptr) return;
        self->quickExportButton_.setEnabled(true);
        self->generateButton_.setEnabled(true);
        if (ok) {
            self->statusLabel_.setText(TRANS("Saved to Desktop: ") + juce::File(msg).getFileName(), juce::dontSendNotification);
            self->statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentGreen);
            self->openButton_.setEnabled(true);
            // Auto-open the report in the browser
            juce::File(msg).startAsProcess();
        } else {
            self->statusLabel_.setText(TRANS("Export failed: ") + msg, juce::dontSendNotification);
            self->statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentRed);
        }
    });
}

void ReportPanel::onExportEvidence()
{
    const auto* s = controller_.getActiveSession();
    if (s == nullptr || !controller_.hasAnalysis()) {
        statusLabel_.setText(TRANS("Analyse a track first."), juce::dontSendNotification);
        return;
    }
    juce::String base = juce::File::createLegalFileName(juce::String(s->name).trim());
    if (base.isEmpty()) base = "khensu";
    chooser_ = std::make_unique<juce::FileChooser>(
        TRANS("Save Evidence Report As..."),
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(base + "_evidence.html"),
        "*.html");
    const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                       juce::FileBrowserComponent::warnAboutOverwriting;
    juce::Component::SafePointer<ReportPanel> self(this);
    chooser_->launchAsync(flags, [self](const juce::FileChooser& fc) {
        if (self == nullptr || fc.getResult() == juce::File{}) return;
        const auto* session = self->controller_.getActiveSession();
        if (session == nullptr || !self->controller_.hasAnalysis()) return;
        const auto html = fc.getResult().withFileExtension(".html");
        const auto& r = self->controller_.currentAnalysis();
        EvidenceReportMeta meta;
        meta.sessionName = juce::String(session->name);
        meta.fileName = session->files.empty() ? juce::String()
                                               : juce::File(juce::String(session->files[0].path)).getFileName();
        meta.genre = r.python_data.genre_detected.empty() ? juce::String(session->target.primary_genre)
                                                          : juce::String(r.python_data.genre_detected);
        meta.outputPath = html.getFullPathName();
        meta.jsonPath = html.withFileExtension(".json").getFullPathName();
        const juce::var body = buildEvidenceReportBody(r, meta);

        self->evidenceButton_.setEnabled(false);
        self->statusLabel_.setText(TRANS("Writing evidence report..."), juce::dontSendNotification);
        self->statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textSecondary);
        fetchQuietly(*self.getComponent(), self->controller_, [body] {
            const auto result =
                ipc::HttpServiceBridge(PythonServiceManager::kBaseUrl).postCollab("/report/evidence", body, 120000);
            return result;
        }, [](ReportPanel& panel, auto&& result) {
            if (result.is_err()) {
                panel.statusLabel_.setText(TRANS("Evidence report failed: ") + serviceErrorLine(result.error()),
                                           juce::dontSendNotification);
                panel.statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentRed);
                return;
            }
            const auto saved = result.value()[api::fields::EvidenceReportResponse::saved_path].toString();
            panel.statusLabel_.setText(TRANS("Evidence report saved: ") + juce::File(saved).getFileName(),
                                       juce::dontSendNotification);
            panel.statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::accentGreen);
            panel.reportOutputPath_ = saved;
            panel.openButton_.setEnabled(true);
        }, [](ReportPanel& panel) {
            panel.evidenceButton_.setEnabled(true); // busy state first
            // The session may have changed while the report was written
            // (FIX-6): the file is on disk where the user chose, but "Open"
            // and the status line would belong to another track - so clear
            // the line here; a current result sets it again.
            panel.statusLabel_.setText({}, juce::dontSendNotification);
        }, true);
    });
}

} // namespace khensu
