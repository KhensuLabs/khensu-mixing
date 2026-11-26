// desktop-app/Source/ui/SessionIntake/RecurringIssuesPanel.cpp
#include "RecurringIssuesPanel.h"
#include "../shared/KhensuTheme.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/ipc/generated/ApiSchema.h>
#include <map>
#include <mutex>
#include <set>

namespace khensu {

namespace ri = recurring_issues;
namespace af = api::fields;

namespace {

/// Each file's measured findings, kept for the life of the app (path, size
/// and modification time): measuring decodes the whole file.
struct FindingsCache {
    std::mutex mutex;
    std::map<juce::String, std::vector<ri::Finding>> byKey;
};

FindingsCache& findingsCache()
{
    static FindingsCache cache;
    return cache;
}

juce::String cacheKey(const juce::File& f)
{
    return f.getFullPathName() + "|" + juce::String(f.getSize()) + "|" +
           juce::String(f.getLastModificationTime().toMilliseconds());
}

std::vector<ri::Finding> parseFindings(const juce::var& reply)
{
    std::vector<ri::Finding> out;
    if (auto* arr = reply[af::AgentFindingsResponse::findings].getArray())
        for (const auto& f : *arr) {
            ri::Finding fd;
            fd.code = f[af::AgentFinding::id].toString();
            fd.title = f[af::AgentFinding::title].toString();
            fd.severity = static_cast<double>(f[af::AgentFinding::severity]);
            if (fd.code.isNotEmpty()) out.push_back(std::move(fd));
        }
    return out;
}

constexpr int kHeaderH = 18;
constexpr int kStatusH = 16;
constexpr int kRowH = 34;
constexpr int kRowGap = 4;
/// A scan older than this runs again when the screen is shown.
constexpr juce::int64 kStaleMs = 10 * 60 * 1000;

} // namespace

// One recurring finding: its name, then how often / when / which way. A
// Button, so it takes focus, reads as a button and clicks from the keyboard.
class RecurringIssuesPanel::RowButton final : public juce::Button {
  public:
    explicit RowButton(const ri::Recurring& r) : juce::Button(r.title), row_(r)
    {
        setTitle(r.title);
        setTooltip(TRANS("Open") + " \"" + r.latestSessionName + "\", " +
                   TRANS("the newest of your tracks with this finding."));
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    juce::String detailText() const
    {
        return juce::String(row_.count) + " " + TRANS("of") + " " + juce::String(row_.scanned) + " " + TRANS("tracks") +
               "  -  " + TRANS("last") + " " + row_.lastSeen.substring(0, 10) + "  -  " + ri::trendText(row_.trend);
    }

    void paintButton(juce::Graphics& g, bool highlighted, bool down) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour(KhensuTheme::bgRaised.withAlpha(down ? 0.9f : (highlighted ? 0.75f : 0.5f)));
        g.fillRoundedRectangle(b, 5.0f);
        if (hasKeyboardFocus(false)) {
            g.setColour(KhensuTheme::focusRing);
            g.drawRoundedRectangle(b.reduced(0.5f), 5.0f, 1.2f);
        }
        // Severity mark: amber when the finding tends to be large, blue otherwise.
        g.setColour(row_.meanSeverity >= 0.5 ? KhensuTheme::accentAmber : KhensuTheme::accentBlue);
        g.fillRoundedRectangle(b.removeFromLeft(4.0f).reduced(0.0f, 5.0f), 1.5f);
        auto text = getLocalBounds().reduced(10, 3);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::bodyFont(type::secondary));
        g.drawText(row_.title, text.removeFromTop(15), juce::Justification::centredLeft, true);
        g.setColour(row_.trend == ri::Trend::MoreOften ? KhensuTheme::accentAmber : KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(detailText(), text, juce::Justification::centredLeft, true);
    }

    const ri::Recurring& row() const noexcept { return row_; }

  private:
    ri::Recurring row_;
};

RecurringIssuesPanel::RecurringIssuesPanel(Runner runLong, std::string baseUrl)
    : runLong_(std::move(runLong)), baseUrl_(std::move(baseUrl))
{
    status_ = TRANS("Measured across your recent tracks once the engine is up.");
    refreshButton_.setTooltip(TRANS("Measure your recent tracks again (only new or changed files are decoded)."));
    refreshButton_.onClick = [this] { refresh(true); };
    addAndMakeVisible(refreshButton_);
}

RecurringIssuesPanel::~RecurringIssuesPanel() { stopTimer(); }

void RecurringIssuesPanel::visibilityChanged()
{
    if (!isShowing()) {
        stopTimer();
        return;
    }
    const bool stale = juce::Time::currentTimeMillis() - lastRunMs_ > kStaleMs;
    if (!loadedOnce_ || stale) refresh(false);
}

void RecurringIssuesPanel::timerCallback()
{
    // Waiting for the engine or the analysis: try again while shown.
    if (!isShowing()) {
        stopTimer();
        return;
    }
    if (canRunNow == nullptr || canRunNow()) {
        stopTimer();
        refresh(false);
    }
}

void RecurringIssuesPanel::refresh(bool force)
{
    if (running_ || runLong_ == nullptr) return;
    if (!force && loadedOnce_ && juce::Time::currentTimeMillis() - lastRunMs_ <= kStaleMs) return;
    if (canRunNow != nullptr && !canRunNow()) {
        if (!loadedOnce_) {
            status_ = TRANS("Waiting for the analysis engine (it runs when nothing else is)...");
            repaint();
        }
        startTimer(3000);
        return;
    }
    running_ = true;
    refreshButton_.setEnabled(false);
    lastRunMs_ = juce::Time::currentTimeMillis();
    const int generation = ++generation_;
    status_ = TRANS("Looking at your recent tracks...");
    repaint();

    juce::Component::SafePointer<RecurringIssuesPanel> self(this);
    runLong_([self, generation, baseUrl = baseUrl_] {
        ipc::HttpServiceBridge bridge(baseUrl);
        const auto post = [self, generation](std::function<void(RecurringIssuesPanel&)> fn) {
            juce::MessageManager::callAsync([self, generation, fn = std::move(fn)] {
                if (self != nullptr && self->generation_ == generation) fn(*self);
            });
        };
        const auto listed = bridge.listAgentSessions(60);
        if (listed.is_err()) {
            const auto why = serviceErrorLine(listed.error());
            post([why](RecurringIssuesPanel& p) {
                p.running_ = false;
                p.refreshButton_.setEnabled(true);
                p.status_ = TRANS("Could not list your sessions:") + " " + why;
                p.repaint();
                if (p.isShowing()) p.startTimer(5000); // the engine may still be starting: try again
            });
            return;
        }

        // Newest first; one entry per audio file (a track opened in several
        // sessions is still one mix), only files still on disk.
        struct Pick {
            ri::SessionFindings session;
            juce::File file;
        };
        std::vector<Pick> picks;
        std::set<juce::String> seenPaths;
        if (auto* arr = listed.value()[af::AgentSessionsResponse::sessions].getArray())
            for (const auto& s : *arr) {
                if (static_cast<int>(picks.size()) >= kMaxTracks) break;
                auto* files = s[af::AgentSession::file_paths].getArray();
                if (files == nullptr || files->isEmpty()) continue;
                const juce::File file(files->getReference(0).toString());
                const auto key = file.getFullPathName().toLowerCase();
                if (!file.existsAsFile() || !seenPaths.insert(key).second) continue;
                Pick pick;
                pick.session.sessionId = s[af::AgentSession::id].toString().toStdString();
                pick.session.sessionName = s[af::AgentSession::name].toString();
                pick.session.updatedAt = s[af::AgentSession::updated_at].toString();
                pick.file = file;
                picks.push_back(std::move(pick));
            }

        std::vector<ri::SessionFindings> scanned;
        int failed = 0;
        for (size_t i = 0; i < picks.size(); ++i) {
            if (self == nullptr) return; // the screen went away
            auto& pick = picks[i];
            const auto key = cacheKey(pick.file);
            bool cached = false;
            {
                auto& cache = findingsCache();
                const std::lock_guard<std::mutex> lock(cache.mutex);
                if (auto it = cache.byKey.find(key); it != cache.byKey.end()) {
                    pick.session.findings = it->second;
                    cached = true;
                }
            }
            if (!cached) {
                const int n = static_cast<int>(picks.size());
                const int at = static_cast<int>(i) + 1;
                post([at, n](RecurringIssuesPanel& p) {
                    p.status_ = TRANS("Measuring your recent tracks") + " (" + juce::String(at) + " " + TRANS("of") +
                                " " + juce::String(n) + ")...";
                    p.repaint();
                });
                const auto reply = bridge.agentFindings(pick.file.getFullPathName());
                if (reply.is_err()) {
                    ++failed;
                    continue;
                }
                pick.session.findings = parseFindings(reply.value());
                auto& cache = findingsCache();
                const std::lock_guard<std::mutex> lock(cache.mutex);
                cache.byKey[key] = pick.session.findings;
            }
            scanned.push_back(pick.session);
        }
        auto rows = ri::aggregate(scanned);
        const int count = static_cast<int>(scanned.size());
        post([rows = std::move(rows), count, failed](RecurringIssuesPanel& p) mutable {
            p.running_ = false;
            p.refreshButton_.setEnabled(true);
            p.showResults(std::move(rows), count, failed);
        });
    });
}

void RecurringIssuesPanel::setResultsForTesting(std::vector<ri::Recurring> rows, int scanned)
{
    showResults(std::move(rows), scanned, 0);
}

void RecurringIssuesPanel::showResults(std::vector<ri::Recurring> rows, int scanned, int failed)
{
    loadedOnce_ = true;
    rows_ = std::move(rows);
    rowButtons_.clear();
    for (size_t i = 0; i < rows_.size() && static_cast<int>(i) < kMaxRows; ++i) {
        auto button = std::make_unique<RowButton>(rows_[i]);
        const std::string id = rows_[i].latestSessionId;
        button->onClick = [this, id] {
            // Next turn: opening a session may rebuild this screen's lists.
            juce::Component::SafePointer<RecurringIssuesPanel> self(this);
            juce::MessageManager::callAsync([self, id] {
                if (self != nullptr && self->onOpenSession) self->onOpenSession(id);
            });
        };
        addAndMakeVisible(*button);
        rowButtons_.push_back(std::move(button));
    }
    if (scanned == 0)
        status_ = failed > 0 ? TRANS("Your recent tracks could not be measured - is the engine running?")
                             : TRANS("No tracks to compare yet. Analyse a few and the findings that recur show here.");
    else if (rows_.empty())
        status_ = TRANS("Nothing comes back across your last") + " " + juce::String(scanned) + " " + TRANS("tracks.");
    else
        status_ =
            TRANS("Across your last") + " " + juce::String(scanned) + " " + TRANS("tracks") +
            (failed > 0 ? " (" + juce::String(failed) + " " + TRANS("could not be measured") + ")" : juce::String()) +
            ". " + TRANS("Click one to open the newest track with it.");
    resized();
    repaint();
}

void RecurringIssuesPanel::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(KhensuTheme::bgSurface.withAlpha(0.25f));
    g.fillRoundedRectangle(bounds, 8.0f);
    juce::ColourGradient borderGrad(KhensuTheme::highlight.withAlpha(0.18f), bounds.getTopLeft(),
                                    KhensuTheme::bgVoid.withAlpha(0.60f), bounds.getBottomRight(), false);
    g.setGradientFill(borderGrad);
    g.drawRoundedRectangle(bounds, 8.0f, 1.2f);

    auto b = getLocalBounds().reduced(KhensuTheme::sp2, KhensuTheme::sp2);
    g.setFont(KhensuTheme::uiFont(type::body, juce::Font::bold));
    g.setColour(KhensuTheme::accentTeal);
    g.drawText(TRANS("RECURRING ISSUES IN YOUR MIXES"), b.removeFromTop(kHeaderH).withTrimmedRight(90),
               juce::Justification::centredLeft, true);
    b.removeFromTop(KhensuTheme::sp1);
    g.setFont(KhensuTheme::labelFont(type::caption));
    g.setColour(KhensuTheme::textTertiary);
    g.drawFittedText(status_, b.removeFromTop(rowButtons_.empty() ? b.getHeight() : kStatusH),
                     rowButtons_.empty() ? juce::Justification::topLeft : juce::Justification::centredLeft, 3);
}

void RecurringIssuesPanel::resized()
{
    auto b = getLocalBounds().reduced(KhensuTheme::sp2, KhensuTheme::sp2);
    auto header = b.removeFromTop(kHeaderH);
    refreshButton_.setBounds(header.removeFromRight(84).withSizeKeepingCentre(84, 24));
    b.removeFromTop(KhensuTheme::sp1 + kStatusH + KhensuTheme::sp1);
    for (auto& row : rowButtons_) {
        const bool fits = b.getHeight() >= kRowH; // no room: the list shows what fits
        row->setVisible(fits);
        if (!fits) continue;
        row->setBounds(b.removeFromTop(kRowH));
        b.removeFromTop(kRowGap);
    }
}

} // namespace khensu
