// desktop-app/Source/ui/shared/SessionCommentsLane.cpp
#include "SessionCommentsLane.h"
#include "PanelRunState.h"
#include "../../IAppController.h"
#include "ServiceErrorHint.h"
#include "SessionEpoch.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace apif = khensu::api::fields;

namespace khensu {

namespace {
constexpr int kAddW = 76;
constexpr int kListW = 86;

juce::String commentAuthor()
{
    auto name = juce::SystemStats::getFullUserName().trim();
    if (name.isEmpty()) name = juce::SystemStats::getLogonName().trim();
    return name;
}

std::vector<SessionCommentsLane::Comment> parseComments(const juce::var& v)
{
    std::vector<SessionCommentsLane::Comment> out;
    if (auto* arr = v[apif::SessionCommentList::comments].getArray()) {
        for (const auto& c : *arr) {
            SessionCommentsLane::Comment x;
            x.id = c[apif::SessionComment::id].toString();
            x.author = c[apif::SessionComment::author].toString();
            x.text = c[apif::SessionComment::text].toString();
            x.start = (double)c[apif::SessionComment::start_s];
            const auto end = c[apif::SessionComment::end_s];
            if (!end.isVoid()) x.end = (double)end;
            x.resolved = (bool)c[apif::SessionComment::resolved];
            out.push_back(std::move(x));
        }
    }
    return out;
}
} // namespace

// ---------------------------------------------------------------------------
// The list callout
// ---------------------------------------------------------------------------

class SessionCommentsListView final : public juce::Component {
  public:
    explicit SessionCommentsListView(SessionCommentsLane& lane) : lane_(&lane)
    {
        setTitle(TRANS("Session comments"));
        viewport_.setViewedComponent(&content_, false);
        viewport_.setScrollBarsShown(true, false);
        addAndMakeVisible(viewport_);
        rebuild();
        setSize(460, 320);
    }

    void rebuild()
    {
        rows_.clear();
        content_.removeAllChildren();
        if (lane_ == nullptr) return;
        for (const auto& c : lane_->comments()) {
            auto row = std::make_unique<Row>(c, lane_);
            content_.addAndMakeVisible(*row);
            rows_.push_back(std::move(row));
        }
        layoutRows();
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(KhensuTheme::bgOverlay);
        if (rows_.empty()) {
            g.setColour(KhensuTheme::textSecondary);
            g.setFont(KhensuTheme::bodyFont());
            g.drawFittedText(TRANS("No comments on this session yet. Use Comment to pin one to the playhead "
                                   "or the selection."),
                             getLocalBounds().reduced(12), juce::Justification::centred, 3);
        }
    }

    void resized() override
    {
        viewport_.setBounds(getLocalBounds().reduced(4));
        layoutRows();
    }

  private:
    struct Row final : public juce::Component {
        Row(const SessionCommentsLane::Comment& c, juce::Component::SafePointer<SessionCommentsLane> lane)
            : id(c.id), start(c.start), lane_(lane)
        {
            juce::String when = SessionCommentsLane::formatTime(c.start);
            if (c.end) when << "-" << SessionCommentsLane::formatTime(*c.end);
            text.setText(when + "  " + (c.author.isNotEmpty() ? c.author + ": " : juce::String()) + c.text,
                         juce::dontSendNotification);
            text.setFont(KhensuTheme::bodyFont());
            text.setColour(juce::Label::textColourId, c.resolved ? KhensuTheme::textTertiary : KhensuTheme::textPrimary);
            text.setTooltip(c.text);
            addAndMakeVisible(text);

            jump.setButtonText(TRANS("Jump"));
            jump.setTooltip(TRANS("Move the playhead to this comment."));
            jump.onClick = [this] {
                if (lane_ != nullptr && lane_->onSeek) lane_->onSeek(start);
            };
            resolve.setButtonText(c.resolved ? TRANS("Reopen") : TRANS("Resolve"));
            resolve.setTooltip(c.resolved ? TRANS("Mark this comment as open again.")
                                          : TRANS("Mark this comment as dealt with."));
            resolve.onClick = [this, wasResolved = c.resolved] {
                if (lane_ != nullptr) lane_->setResolved(id, !wasResolved);
            };
            del.setButtonText(TRANS("Delete"));
            del.setTooltip(TRANS("Delete this comment."));
            del.onClick = [this] {
                if (lane_ != nullptr) lane_->remove(id);
            };
            for (auto* b : {&jump, &resolve, &del}) {
                b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
                b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
                addAndMakeVisible(*b);
            }
        }
        void resized() override
        {
            auto b = getLocalBounds().reduced(2);
            del.setBounds(b.removeFromRight(56).reduced(1));
            resolve.setBounds(b.removeFromRight(64).reduced(1));
            jump.setBounds(b.removeFromRight(48).reduced(1));
            text.setBounds(b);
        }
        juce::String id;
        double start;
        juce::Component::SafePointer<SessionCommentsLane> lane_;
        juce::Label text;
        juce::TextButton jump, resolve, del;
    };

    void layoutRows()
    {
        const int w = juce::jmax(100, viewport_.getMaximumVisibleWidth());
        int y = 0;
        for (auto& r : rows_) {
            r->setBounds(0, y, w, 28);
            y += 28;
        }
        content_.setSize(w, juce::jmax(y, 1));
    }

    juce::Component::SafePointer<SessionCommentsLane> lane_;
    juce::Viewport viewport_;
    juce::Component content_;
    std::vector<std::unique_ptr<Row>> rows_;

    friend class SessionCommentsLane;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionCommentsListView)
};

// ---------------------------------------------------------------------------
// The lane
// ---------------------------------------------------------------------------

juce::String SessionCommentsLane::formatTime(double s)
{
    s = juce::jmax(0.0, s);
    const int m = static_cast<int>(s / 60.0);
    return juce::String(m) + ":" + juce::String(s - m * 60.0, 1).paddedLeft('0', 4);
}

void SessionCommentsLane::applyThemeColours()
{
    for (auto* b : {&addButton_, &listButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentAmber.withAlpha(0.15f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentAmber);
    }
}

SessionCommentsLane::SessionCommentsLane(IAppController& controller) : controller_(controller)
{
    applyThemeColours();
    setTitle(TRANS("Session comments lane"));
    setDescription(TRANS("Comments pinned to times in this session. Click a marker to jump to it."));
    addButton_.setTitle(TRANS("Add comment"));
    addButton_.setTooltip(TRANS("Pin a comment to the selection, or to the playhead if nothing is selected. "
                                "Comments are saved with the session and travel in .khensu bundles."));
    addButton_.onClick = [this] { addAtCursor(); };
    listButton_.setTitle(TRANS("Show comments"));
    listButton_.setTooltip(TRANS("List this session's comments: jump to, resolve or delete them."));
    listButton_.onClick = [this] { showList(); };
    addAndMakeVisible(addButton_);
    addAndMakeVisible(listButton_);
    startTimerHz(10);
}

SessionCommentsLane::~SessionCommentsLane()
{
    stopTimer();
    if (dialog_ != nullptr) dialog_->exitModalState(0);
}

juce::String SessionCommentsLane::activeSessionId() const
{
    const auto* s = controller_.getActiveSession();
    return s != nullptr ? juce::String(s->id) : juce::String();
}

void SessionCommentsLane::status(const juce::String& s)
{
    if (onStatus) onStatus(s);
}

void SessionCommentsLane::timerCallback()
{
    const auto sid = activeSessionId();
    if (sid != loadedSessionId_) {
        loadedSessionId_ = sid;
        comments_.clear();
        repaint();
        if (sid.isNotEmpty()) reload();
    }
    if (viewProvider) {
        const auto v = viewProvider();
        if (!(v == view_)) {
            view_ = v;
            repaint();
        }
    }
}

void SessionCommentsLane::reload()
{
    const auto sid = activeSessionId();
    if (sid.isEmpty() || loading_) return;
    loading_ = true;
    juce::Component::SafePointer<SessionCommentsLane> self(this);
    controller_.runInBackground([self, &work = controller_, sid] {
        auto result = ipc::HttpServiceBridge{}.listComments(sid);
        postIfCurrent(self, work, [sid, result](SessionCommentsLane& panel) {
            if (sid != panel.activeSessionId()) return;
            if (result.is_err()) {
                panel.status(TRANS("Comments unavailable: ") + serviceErrorLine(result.error()));
                return;
            }
            panel.comments_ = parseComments(result.value());
            panel.repaint();
            if (panel.openList_ != nullptr) panel.openList_->rebuild();
        }, [](SessionCommentsLane& panel) { panel.loading_ = false; }); // reset busy state first
    });
}

void SessionCommentsLane::addAtCursor()
{
    const auto sid = activeSessionId();
    if (sid.isEmpty()) {
        status(TRANS("Open a session to comment on it."));
        return;
    }
    View v = viewProvider ? viewProvider() : View{};
    const bool range = v.selEnd > v.selStart;
    const double start = range ? v.selStart : v.playhead;
    const double end = range ? v.selEnd : -1.0;

    dialog_ = std::make_unique<juce::AlertWindow>(
        TRANS("Add comment"),
        range ? TRANS("Comment on ") + formatTime(start) + " - " + formatTime(end)
              : TRANS("Comment at ") + formatTime(start),
        juce::MessageBoxIconType::NoIcon, this);
    dialog_->addTextEditor("text", {}, TRANS("Comment"));
    dialog_->addTextEditor("author", commentAuthor(), TRANS("Your name"));
    dialog_->addButton(TRANS("Add"), 1, juce::KeyPress(juce::KeyPress::returnKey));
    dialog_->addButton(TRANS("Cancel"), 0, juce::KeyPress(juce::KeyPress::escapeKey));
    juce::Component::SafePointer<SessionCommentsLane> self(this);
    dialog_->enterModalState(true, juce::ModalCallbackFunction::create([self, sid, start, end](int r) {
        if (self == nullptr || self->dialog_ == nullptr) return;
        const auto text = self->dialog_->getTextEditorContents("text").trim();
        const auto author = self->dialog_->getTextEditorContents("author").trim();
        self->dialog_.reset();
        if (r != 1 || text.isEmpty()) return;
        self->controller_.runInBackground([self, &work = self->controller_, sid, text, author, start, end] {
            auto result = ipc::HttpServiceBridge{}.addComment(sid, text, start, end, author);
            postIfCurrent(self, work, [result](SessionCommentsLane& panel) {
                if (result.is_err()) {
                    panel.status(TRANS("Comment not saved: ") + serviceErrorLine(result.error()));
                    return;
                }
                panel.status(TRANS("Comment added."));
                panel.reload();
            });
        });
    }));
}

void SessionCommentsLane::setResolved(const juce::String& id, bool resolved)
{
    juce::Component::SafePointer<SessionCommentsLane> self(this);
    controller_.runInBackground([self, &work = controller_, id, resolved] {
        juce::DynamicObject::Ptr body = new juce::DynamicObject();
        body->setProperty(apif::CommentUpdateRequest::resolved, resolved);
        auto result = ipc::HttpServiceBridge{}.updateComment(id, juce::var(body.get()));
        postIfCurrent(self, work, [result](SessionCommentsLane& panel) {
            if (result.is_err()) panel.status(TRANS("Comment not updated: ") + serviceErrorLine(result.error()));
            panel.reload();
        });
    });
}

void SessionCommentsLane::remove(const juce::String& id)
{
    juce::Component::SafePointer<SessionCommentsLane> self(this);
    controller_.runInBackground([self, &work = controller_, id] {
        const bool ok = ipc::HttpServiceBridge{}.deleteComment(id);
        postIfCurrent(self, work, [ok](SessionCommentsLane& panel) {
            if (!ok) panel.status(TRANS("Comment not deleted (service unavailable)."));
            panel.reload();
        });
    });
}

void SessionCommentsLane::showList()
{
    auto view = std::make_unique<SessionCommentsListView>(*this);
    openList_ = view.get();
    juce::CallOutBox::launchAsynchronously(std::move(view), listButton_.getScreenBounds(), nullptr);
    reload();
}

juce::Rectangle<float> SessionCommentsLane::markerArea() const
{
    auto b = getLocalBounds();
    b.removeFromRight(kAddW + kListW + 4);
    return b.toFloat();
}

int SessionCommentsLane::commentAt(juce::Point<float> p) const
{
    const auto area = markerArea();
    const double len = view_.viewEnd - view_.viewStart;
    if (!area.contains(p) || len <= 0.0) return -1;
    auto xFor = [&](double t) { return area.getX() + (float)((t - view_.viewStart) / len) * area.getWidth(); };
    for (int i = (int)comments_.size() - 1; i >= 0; --i) {
        const auto& c = comments_[(size_t)i];
        const float x0 = xFor(c.start) - 4.0f;
        const float x1 = (c.end ? xFor(*c.end) : xFor(c.start)) + 4.0f;
        if (p.x >= x0 && p.x <= x1) return i;
    }
    return -1;
}

void SessionCommentsLane::paint(juce::Graphics& g)
{
    const auto area = markerArea();
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRect(area);
    const double len = view_.viewEnd - view_.viewStart;
    if (comments_.empty() || len <= 0.0) {
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(9.5f));
        g.drawText(loadedSessionId_.isEmpty() ? TRANS("Comments: no session") : TRANS("Comments: none yet"),
                   area.reduced(6.0f, 0.0f), juce::Justification::centredLeft, true);
        return;
    }
    auto xFor = [&](double t) { return area.getX() + (float)((t - view_.viewStart) / len) * area.getWidth(); };
    for (const auto& c : comments_) {
        const double cEnd = c.end ? *c.end : c.start;
        if (cEnd < view_.viewStart || c.start > view_.viewEnd) continue;
        const auto colour = c.resolved ? KhensuTheme::textTertiary : KhensuTheme::accentAmber;
        const float x0 = xFor(c.start);
        if (c.end) {
            const float x1 = xFor(*c.end);
            g.setColour(colour.withAlpha(0.35f));
            g.fillRect(juce::Rectangle<float>(x0, area.getY() + 5.0f, juce::jmax(2.0f, x1 - x0), area.getHeight() - 10.0f));
            g.setColour(colour);
            g.drawRect(juce::Rectangle<float>(x0, area.getY() + 5.0f, juce::jmax(2.0f, x1 - x0), area.getHeight() - 10.0f), 1.0f);
        }
        juce::Path tri;
        tri.addTriangle(x0 - 4.0f, area.getY() + 2.0f, x0 + 4.0f, area.getY() + 2.0f, x0, area.getY() + 9.0f);
        g.setColour(colour);
        g.fillPath(tri);
        g.drawVerticalLine(juce::roundToInt(x0), area.getY() + 9.0f, area.getBottom() - 2.0f);
    }
}

void SessionCommentsLane::resized()
{
    auto b = getLocalBounds();
    listButton_.setBounds(b.removeFromRight(kListW).reduced(1));
    b.removeFromRight(2);
    addButton_.setBounds(b.removeFromRight(kAddW).reduced(1));
}

void SessionCommentsLane::mouseDown(const juce::MouseEvent& e)
{
    const int i = commentAt(e.position);
    if (i < 0) return;
    const auto& c = comments_[(size_t)i];
    if (onSeek) onSeek(c.start);
    status(formatTime(c.start) + "  " + (c.author.isNotEmpty() ? c.author + ": " : juce::String()) + c.text +
           (c.resolved ? TRANS(" (resolved)") : juce::String()));
}

void SessionCommentsLane::mouseMove(const juce::MouseEvent& e)
{
    const int i = commentAt(e.position);
    const juce::String tip =
        i >= 0 ? (comments_[(size_t)i].author.isNotEmpty() ? comments_[(size_t)i].author + ": " : juce::String()) +
                     comments_[(size_t)i].text
               : TRANS("Session comments. Click a marker to jump to it.");
    if (tip != getTooltip()) setTooltip(tip);
}

} // namespace khensu
