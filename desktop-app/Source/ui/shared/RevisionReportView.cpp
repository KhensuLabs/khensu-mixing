// desktop-app/Source/ui/shared/RevisionReportView.cpp
#include "RevisionReportView.h"
#include "PanelRunState.h"
#include "../../IBackgroundWork.h"
#include "ServiceErrorHint.h"
#include "SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu {

RevisionReportView::RevisionReportView(IBackgroundWork& controller, juce::String aPath, juce::String bPath,
                                       juce::String aLabel, juce::String bLabel)
    : controller_(controller), aPath_(std::move(aPath)), bPath_(std::move(bPath)), aLabel_(std::move(aLabel)),
      bLabel_(std::move(bLabel))
{
    setTitle(TRANS("Compare revisions"));
    setDescription(TRANS("What changed between two versions: findings fixed, improved, worse or new, and the "
                         "measurements that moved."));
    text_.setMultiLine(true, true);
    text_.setReadOnly(true);
    text_.setCaretVisible(false);
    text_.setScrollbarsShown(true);
    text_.setTitle(TRANS("Revision report"));
    text_.setTooltip(TRANS("Revision report text. Select to copy."));
    addAndMakeVisible(text_);
    applyThemeColours();
    setSize(520, 440);
    render();
    fetch();
}

void RevisionReportView::show(IBackgroundWork& controller, juce::String aPath, juce::String bPath, juce::String aLabel,
                              juce::String bLabel, juce::Component* anchor)
{
    auto view = std::make_unique<RevisionReportView>(controller, std::move(aPath), std::move(bPath),
                                                     std::move(aLabel), std::move(bLabel));
    if (anchor != nullptr && anchor->isShowing()) {
        juce::CallOutBox::launchAsynchronously(std::move(view), anchor->getScreenBounds(), nullptr);
        return;
    }
    juce::DialogWindow::LaunchOptions o;
    o.dialogTitle = TRANS("Compare revisions");
    o.dialogBackgroundColour = KhensuTheme::bgOverlay;
    o.content.setOwned(view.release());
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void RevisionReportView::applyThemeColours()
{
    text_.setColour(juce::TextEditor::backgroundColourId, KhensuTheme::bgSurface);
    text_.setColour(juce::TextEditor::outlineColourId, KhensuTheme::borderSubtle);
    text_.setColour(juce::TextEditor::focusedOutlineColourId, KhensuTheme::focusRing);
    text_.setColour(juce::TextEditor::textColourId, KhensuTheme::textPrimary);
}

void RevisionReportView::fetch()
{
    juce::Component::SafePointer<RevisionReportView> self(this);
    const auto a = aPath_, b = bPath_, al = aLabel_, bl = bLabel_;
    controller_.runLongInBackground([self, &work = controller_, a, b, al, bl] {
        juce::DynamicObject::Ptr body = new juce::DynamicObject();
        body->setProperty(apif::RevisionReportRequest::a_path, a);
        body->setProperty(apif::RevisionReportRequest::b_path, b);
        body->setProperty(apif::RevisionReportRequest::a_label, al);
        body->setProperty(apif::RevisionReportRequest::b_label, bl);
        auto result = ipc::HttpServiceBridge{}.postCollab("/revision/report", juce::var(body.get()));
        postIfCurrent(self, work, [result](RevisionReportView& panel) {
            if (result.is_err())
                panel.error_ = TRANS("Comparison failed: ") + serviceErrorLine(result.error());
            else
                panel.report_ = result.value();
            panel.render();
        }, [](RevisionReportView& panel) { panel.busy_ = false; }, // reset busy state first
        [](RevisionReportView& panel) {
            panel.error_ = TRANS("The session changed before the comparison finished.");
            panel.render();
        });
    });
}

void RevisionReportView::render()
{
    text_.clear();
    auto put = [this](const juce::String& s, juce::Colour c, bool heading = false) {
        text_.setFont(heading ? KhensuTheme::uiFont(KhensuTheme::kFontH2, juce::Font::bold) : KhensuTheme::bodyFont());
        text_.setColour(juce::TextEditor::textColourId, c);
        text_.moveCaretToEnd();
        text_.insertTextAtCaret(s + "\n");
    };
    put(TRANS("Before: ") + aLabel_ + "   " + TRANS("After: ") + bLabel_, KhensuTheme::textSecondary);
    if (busy_) {
        put(TRANS("Measuring both versions..."), KhensuTheme::textSecondary);
        return;
    }
    if (error_.isNotEmpty()) {
        put(error_, KhensuTheme::accentRed);
        return;
    }
    put(report_[apif::RevisionReportResponse::summary].toString(), KhensuTheme::textPrimary);

    auto section = [&](const juce::String& title, const char* key, juce::Colour colour, const char* textKey) {
        put("", KhensuTheme::textPrimary);
        put(title, colour, true);
        const auto* arr = report_[key].getArray();
        if (arr == nullptr || arr->isEmpty()) {
            put(TRANS("  (none)"), KhensuTheme::textTertiary);
            return;
        }
        for (const auto& item : *arr)
            put("  - " + item[textKey].toString(), colour);
    };
    section(TRANS("Fixed"), apif::RevisionReportResponse::fixed, KhensuTheme::accentGreen,
            apif::RevisionFindingChange::text);
    section(TRANS("Better but still present"), apif::RevisionReportResponse::improved, KhensuTheme::accentAmber,
            apif::RevisionFindingChange::text);
    section(TRANS("Got worse or new"), apif::RevisionReportResponse::got_worse, KhensuTheme::accentRed,
            apif::RevisionFindingChange::text);
    section(TRANS("What changed"), apif::RevisionReportResponse::changed, KhensuTheme::textPrimary,
            apif::RevisionMetricChange::text);
    text_.moveCaretToTop(false);
}

void RevisionReportView::paint(juce::Graphics& g) { g.fillAll(KhensuTheme::bgOverlay); }

void RevisionReportView::resized() { text_.setBounds(getLocalBounds().reduced(8)); }

} // namespace khensu
