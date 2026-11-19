// desktop-app/Source/ui/Settings/ComponentsPanel.cpp
#include "ComponentsPanel.h"
#include "../shared/ServiceErrorHint.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <chrono>
#include <thread>

namespace khensu {

namespace af = api::fields;

namespace {
constexpr int kRowH = 40;
constexpr int kHeaderH = 22;
constexpr int kStatusH = 18;
} // namespace

class ComponentsPanel::RowView final : public juce::Component {
  public:
    RowView(ComponentsPanel& owner, Row row) : owner_(owner), row_(std::move(row))
    {
        install_.setTooltip(TRANS("Download and install it from its reviewed, hash-pinned package list"));
        install_.onClick = [this] { owner_.install(row_.id); };
        install_.setVisible(!row_.installed && row_.installable);
        addChildComponent(install_);
        setTitle(row_.name);
        setDescription(row_.installed ? TRANS("Installed") : TRANS("Not installed"));
    }
    void setInstallEnabled(bool on) { install_.setEnabled(on); }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().reduced(0, 2);
        g.setColour(KhensuTheme::bgRaised.withAlpha(0.5f));
        g.fillRoundedRectangle(b.toFloat(), 4.0f);
        b = b.reduced(10, 3);
        b.removeFromRight(install_.isVisible() ? 100 : 0);
        auto top = b.removeFromTop(18);
        g.setFont(KhensuTheme::bodyFont(13.0f).boldened());
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(row_.name, top.removeFromLeft(top.getWidth() / 2), juce::Justification::centredLeft, true);
        g.setFont(KhensuTheme::labelFont(11.0f));
        // Installed / missing is a state, not a severity: neutral colours.
        g.setColour(row_.installed ? KhensuTheme::textSecondary : KhensuTheme::textTertiary);
        juce::String state = row_.installed ? TRANS("Installed") : TRANS("Not installed");
        if (!row_.installed && !row_.installable)
            state << " - " << (row_.blocked.isNotEmpty() ? row_.blocked : row_.how);
        g.drawText(state, top, juce::Justification::centredRight, true);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont(11.5f));
        g.drawFittedText(row_.description, b, juce::Justification::topLeft, 1);
    }
    void resized() override
    {
        install_.setBounds(getLocalBounds().removeFromRight(96).withSizeKeepingCentre(90, KhensuTheme::buttonHSmall));
    }

  private:
    ComponentsPanel& owner_;
    Row row_;
    juce::TextButton install_{TRANS("Install")};
};

ComponentsPanel::ComponentsPanel(IAppController& controller) : controller_(controller)
{
    header_.setText(TRANS("Optional components"), juce::dontSendNotification);
    header_.setFont(KhensuTheme::labelFont(12.0f).boldened());
    addAndMakeVisible(header_);
    status_.setFont(KhensuTheme::labelFont(11.0f));
    addAndMakeVisible(status_);
    applyThemeColours();
}

ComponentsPanel::~ComponentsPanel() { *alive_ = false; }

void ComponentsPanel::applyThemeColours()
{
    header_.setColour(juce::Label::textColourId, KhensuTheme::textPrimary);
    status_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

int ComponentsPanel::preferredHeight() const noexcept
{
    return kHeaderH + kStatusH + juce::jmax(1, (int)rows_.size()) * kRowH + 8;
}

std::vector<ComponentsPanel::Row> ComponentsPanel::parse(const juce::var& response)
{
    std::vector<Row> out;
    if (auto* arr = response[af::ComponentsResponse::components].getArray())
        for (const auto& c : *arr) {
            Row r;
            r.id = c[af::ComponentInfo::id].toString();
            r.name = c[af::ComponentInfo::name].toString();
            r.description = c[af::ComponentInfo::description].toString();
            r.installed = (bool)c[af::ComponentInfo::installed];
            r.installable = (bool)c[af::ComponentInfo::installable];
            r.blocked = c[af::ComponentInfo::install_blocked].toString();
            r.how = c[af::ComponentInfo::how].toString();
            out.push_back(std::move(r));
        }
    return out;
}

void ComponentsPanel::refresh()
{
    status_.setText(TRANS("Checking what is installed..."), juce::dontSendNotification);
    juce::Component::SafePointer<ComponentsPanel> self(this);
    controller_.runInBackground([self] {
        auto result = ipc::HttpServiceBridge{}.listComponents();
        juce::MessageManager::callAsync([self, result] {
            if (self == nullptr) return;
            // The components are the machine's, not the session's: no epoch check.
            if (result.is_err()) {
                self->status_.setText(classifyServiceError(result.error()).message, juce::dontSendNotification);
                return;
            }
            self->status_.setText({}, juce::dontSendNotification);
            self->rebuild(parse(result.value()));
        });
    });
}

void ComponentsPanel::rebuild(std::vector<Row> rows)
{
    rows_.clear();
    for (auto& r : rows) {
        rows_.push_back(std::make_unique<RowView>(*this, std::move(r)));
        rows_.back()->setInstallEnabled(!installing_);
        addAndMakeVisible(*rows_.back());
    }
    if (auto* p = getParentComponent()) p->resized(); // the height changed
    resized();
}

void ComponentsPanel::install(const juce::String& id)
{
    if (installing_) return;
    installing_ = true;
    for (auto& r : rows_)
        r->setInstallEnabled(false);
    status_.setText(TRANS("Installing - this can take several minutes..."), juce::dontSendNotification);
    juce::Component::SafePointer<ComponentsPanel> self(this);
    auto alive = alive_;
    auto& work = controller_;
    // The long pool: an install polls for minutes.
    controller_.runLongInBackground([self, alive, id, &work] {
        ipc::HttpServiceBridge bridge;
        auto started = bridge.startComponentInstall(id);
        juce::String message;
        bool ok = false;
        if (started.is_err()) {
            message = classifyServiceError(started.error()).message;
        } else {
            const auto jobId = started.value()[af::ComponentInstallStart::job_id].toString();
            for (int i = 0; i < 30 * 60 && *alive && !work.isShuttingDown(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                auto st = bridge.componentInstallStatus(jobId);
                if (st.is_err()) continue;
                const auto status = st.value()[af::ComponentInstallStatus::status].toString();
                if (!api::isTerminalStatus(status)) continue;
                const auto res = st.value()[af::ComponentInstallStatus::result];
                ok = (bool)res[af::ComponentInstallResult::ok];
                message = ok ? TRANS("Installed. Restart the analysis engine to use it.")
                             : TRANS("Install failed:") + " " +
                                   (st.value()[af::ComponentInstallStatus::error].toString().isNotEmpty()
                                        ? st.value()[af::ComponentInstallStatus::error].toString()
                                        : juce::String((int)res[af::ComponentInstallResult::exit_code]));
                break;
            }
        }
        juce::MessageManager::callAsync([self, message, ok] {
            if (self == nullptr) return;
            self->installing_ = false;
            self->status_.setText(message, juce::dontSendNotification);
            if (ok) self->refresh();
            for (auto& r : self->rows_)
                r->setInstallEnabled(true);
        });
    });
}

void ComponentsPanel::resized()
{
    auto b = getLocalBounds();
    header_.setBounds(b.removeFromTop(kHeaderH));
    status_.setBounds(b.removeFromTop(kStatusH));
    for (auto& r : rows_)
        r->setBounds(b.removeFromTop(kRowH));
}

} // namespace khensu
