// desktop-app/Source/ui/shared/TransportWaveform.cpp
#include "TransportWaveform.h"
#include "../../IAppController.h"
#include "ChangedRegions.h"
#include "TimelineCache.h"
#include "KhensuTheme.h"
#include "PanelRunState.h"
#include <khensu/ipc/HttpServiceBridge.h>

namespace khensu {

namespace tw = transport_waveform;

namespace {
constexpr float kMarkerH = 5.0f;
constexpr float kMarkerGap = 2.0f;
constexpr juce::uint32 kRegionRetryMs = 15000;
} // namespace

TransportWaveform::TransportWaveform(IAppController& controller) : controller_(controller)
{
    setWantsKeyboardFocus(true);
    setMouseClickGrabsKeyboardFocus(true);
    setTitle(TRANS("Playback position"));
    setDescription(TRANS("Waveform of the audio being played. Click or drag to seek; shaded regions are what the "
                         "current fix changed."));
    setHelpText(TRANS("Left and Right seek 5 seconds (Shift: 1 second), Home and End jump, L loops the changed "
                      "region under the playhead."));
    setTooltip(TRANS("Click or drag to seek. Shaded: what the current fix changed - click a marker below the "
                     "waveform to loop it."));
    setRepaintsOnMouseActivity(false);
}

TransportWaveform::~TransportWaveform() = default;

void TransportWaveform::resized()
{
    columns_ = peaks_ != nullptr ? tw::columns(*peaks_, juce::roundToInt(waveArea().getWidth())) : std::vector<float>{};
}

juce::Rectangle<float> TransportWaveform::waveArea() const
{
    auto r = getLocalBounds().toFloat().reduced(0.0f, 2.0f);
    r.removeFromBottom(kMarkerH + kMarkerGap);
    return r;
}

juce::Rectangle<float> TransportWaveform::markerArea() const
{
    auto r = getLocalBounds().toFloat().reduced(0.0f, 2.0f);
    return r.removeFromBottom(kMarkerH);
}

void TransportWaveform::update()
{
    auto& playback = controller_.playback();
    const double dur = playback.getPlaybackDuration();
    const double pos = playback.getPlaybackPosition();
    bool dirty = false;
    if (std::abs(dur - duration_) > 1e-6) {
        duration_ = dur;
        dirty = true;
    }
    position_ = pos;

    // The file being auditioned and its peaks.
    const juce::String path = controller_.hasActiveSession() ? playback.currentAuditionPath() : juce::String();
    if (path != auditionPath_) {
        auditionPath_ = path;
        auditionKey_ = {};
        peaks_.reset();
        columns_.clear();
        dirty = true;
    }
    if (auditionPath_.isNotEmpty() && peaks_ == nullptr) {
        const juce::File file(auditionPath_);
        if (auditionKey_.isEmpty() && file.existsAsFile()) auditionKey_ = tw::cacheKey(file);
        if (auditionKey_.isNotEmpty()) {
            if (auto hit = cache_.find(auditionKey_)) {
                peaks_ = std::move(hit);
                resized();
                dirty = true;
            } else {
                requestPeaks(file, auditionKey_);
            }
        }
    }

    // The current fix's changed regions (against the version it was rendered from).
    const juce::String after = controller_.hasActiveSession() ? controller_.renders().currentRenderPath() : juce::String();
    if (after != regionsFor_) {
        regionsFor_ = after;
        if (!regions_.empty()) dirty = true;
        regions_.clear();
        hoverRegion_ = -1;
    }
    if (after.isNotEmpty() && (after != regionsAsked_ ||
                               (regionsRetryAt_ != 0 && juce::Time::getMillisecondCounter() >= regionsRetryAt_))) {
        const auto before = changed_regions::versionBefore(controller_.renders(), controller_.activeSourcePath());
        regionsAsked_ = after;
        regionsRetryAt_ = 0;
        if (before.isNotEmpty() && before != after) requestRegions(before, after);
    }

    const float x = tw::xForTime(dragging_ ? dragSeconds_ : position_, waveArea().getWidth(), duration_);
    if (dirty || std::abs(x - lastPlayheadX_) >= 0.5f) {
        lastPlayheadX_ = x;
        repaint();
    }
}

void TransportWaveform::requestPeaks(const juce::File& file, const juce::String& key)
{
    if (pendingPeaks_.count(key) != 0) return;
    pendingPeaks_.insert(key);
    IAppController* ctrl = &controller_;
    fetchQuietly(
        *this, controller_,
        [file, ctrl] {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
            if (reader == nullptr) return std::vector<float>{};
            return tw::computePeaks(*reader, kPeakCount, [ctrl] { return ctrl->isShuttingDown(); });
        },
        [key](TransportWaveform& self, std::vector<float> peaks) {
            if (peaks.empty()) return;
            self.cache_.put(key, std::move(peaks));
            if (self.auditionKey_ == key) {
                self.peaks_ = self.cache_.find(key);
                self.resized();
                self.repaint();
            }
        },
        [key](TransportWaveform& self) { self.pendingPeaks_.erase(key); });
}

void TransportWaveform::requestRegions(const juce::String& before, const juce::String& after)
{
    using Reply = std::pair<bool, tw::Regions>; // (answered, regions)
    fetchQuietly(
        *this, controller_,
        [before, after]() -> Reply {
            auto result = ipc::HttpServiceBridge{}.changedRegions(before, after);
            if (result.is_ok()) return {true, changed_regions::fromResponse(result.value())};
            // VIZ-3b: the engine could not answer - measure the regions here.
            const auto a = timeline_cache::measure(before);
            const auto b = timeline_cache::measure(after);
            if (a == nullptr || b == nullptr) return {false, {}};
            return {true, changed_regions::fromTimelines(*a, *b)};
        },
        [after](TransportWaveform& self, Reply reply) {
            // Another version became current meanwhile: not the fix on screen.
            if (self.controller_.renders().currentRenderPath() != after) return;
            if (!reply.first) {
                self.regionsRetryAt_ = juce::Time::getMillisecondCounter() + kRegionRetryMs; // service busy / down
                return;
            }
            self.regions_ = std::move(reply.second);
            self.updateTooltip();
            self.repaint();
        });
}

void TransportWaveform::paint(juce::Graphics& g)
{
    const auto wave = waveArea();
    const auto marks = markerArea();
    const float midY = wave.getCentreY();
    const float halfH = wave.getHeight() * 0.5f;

    // Lane
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.6f));
    g.fillRoundedRectangle(wave, 3.0f);

    // Changed regions (behind the waveform).
    for (size_t i = 0; i < regions_.size(); ++i) {
        const float x0 = wave.getX() + tw::xForTime(regions_[i].first, wave.getWidth(), duration_);
        const float x1 = wave.getX() + tw::xForTime(regions_[i].second, wave.getWidth(), duration_);
        const bool looping = controller_.shell().isLoopingRegion(regions_[i].first, regions_[i].second);
        const bool hot = static_cast<int>(i) == hoverRegion_ || looping;
        const auto band = juce::Rectangle<float>(x0, wave.getY(), juce::jmax(2.0f, x1 - x0), wave.getHeight());
        g.setColour(KhensuTheme::accentTeal.withAlpha(hot ? 0.22f : 0.12f));
        g.fillRect(band);
        g.setColour(KhensuTheme::accentTeal.withAlpha(hot ? 1.0f : 0.7f));
        g.fillRoundedRectangle(juce::Rectangle<float>(x0, marks.getY(), juce::jmax(3.0f, x1 - x0), marks.getHeight()),
                               1.5f);
        if (looping) {
            g.setColour(KhensuTheme::accentTeal);
            g.drawRect(band, 1.0f);
        }
    }

    const float playX = wave.getX() + tw::xForTime(dragging_ ? dragSeconds_ : position_, wave.getWidth(), duration_);
    if (!columns_.empty()) {
        // Neutral peaks; what has played is brighter. Bars every other pixel.
        const auto played = KhensuTheme::textPrimary.withAlpha(0.62f);
        const auto ahead = KhensuTheme::textSecondary.withAlpha(0.38f);
        for (size_t c = 0; c < columns_.size(); c += 2) {
            const float x = wave.getX() + static_cast<float>(c);
            const float v = juce::jmax(columns_[c], c + 1 < columns_.size() ? columns_[c + 1] : 0.0f);
            const float h = juce::jmax(1.0f, std::sqrt(v) * halfH * 0.94f); // sqrt: quiet passages stay visible
            g.setColour(x < playX ? played : ahead);
            g.fillRect(x, midY - h, 1.4f, h * 2.0f);
        }
    } else {
        // No peaks yet (or nothing loaded): a centre line.
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.5f));
        g.fillRect(wave.getX(), midY - 0.5f, wave.getWidth(), 1.0f);
        if (auditionPath_.isNotEmpty()) {
            g.setColour(KhensuTheme::accentTeal.withAlpha(0.35f));
            g.fillRect(wave.getX(), midY - 0.5f, playX - wave.getX(), 1.0f);
        }
    }

    // Hover cursor while scrubbing / pointing.
    if (hoverX_ >= 0.0f && !dragging_ && duration_ > 0.0) {
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.fillRect(hoverX_, wave.getY(), 1.0f, wave.getHeight());
    }

    // Playhead
    if (duration_ > 0.0) {
        g.setColour(KhensuTheme::accentTeal);
        g.fillRect(playX - 1.0f, wave.getY() - 1.0f, 2.0f, wave.getHeight() + 2.0f);
        juce::Path cap;
        cap.addTriangle(playX - 4.0f, wave.getY() - 1.0f, playX + 4.0f, wave.getY() - 1.0f, playX, wave.getY() + 4.0f);
        g.fillPath(cap);
    }

    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing);
        g.drawRoundedRectangle(wave.expanded(1.0f), 3.0f, 1.0f);
    }
}

void TransportWaveform::seekTo(double seconds)
{
    if (duration_ <= 0.0) return;
    position_ = juce::jlimit(0.0, duration_, seconds);
    controller_.playback().setPlaybackPosition(position_);
    repaint();
}

void TransportWaveform::loopRegion(int index)
{
    if (index < 0 || index >= static_cast<int>(regions_.size())) return;
    const auto [a, b] = regions_[static_cast<size_t>(index)];
    if (controller_.shell().isLoopingRegion(a, b))
        controller_.shell().stopLoopRegion();
    else
        controller_.shell().loopRegion(a, b);
    repaint();
}

void TransportWaveform::updateTooltip()
{
    if (hoverRegion_ >= 0 && hoverRegion_ < static_cast<int>(regions_.size())) {
        const auto& r = regions_[static_cast<size_t>(hoverRegion_)];
        setTooltip(TRANS("Changed region %n of %t (%a-%b): click the marker or double-click to loop it.")
                       .replace("%n", juce::String(hoverRegion_ + 1))
                       .replace("%t", juce::String(static_cast<int>(regions_.size())))
                       .replace("%a", changed_regions::clock(r.first))
                       .replace("%b", changed_regions::clock(r.second)));
        return;
    }
    setTooltip(regions_.empty()
                   ? TRANS("Click or drag to seek.")
                   : TRANS("Click or drag to seek. Shaded: what the current fix changed - click a marker below the "
                           "waveform to loop it."));
}

void TransportWaveform::mouseMove(const juce::MouseEvent& e)
{
    const auto wave = waveArea();
    hoverX_ = juce::jlimit(wave.getX(), wave.getRight(), e.position.x);
    const int region = tw::regionAt(regions_, tw::timeAtX(e.position.x - wave.getX(), wave.getWidth(), duration_));
    if (region != hoverRegion_) {
        hoverRegion_ = region;
        updateTooltip();
    }
    setMouseCursor(region >= 0 && e.position.y >= markerArea().getY() - kMarkerGap
                       ? juce::MouseCursor::PointingHandCursor
                       : juce::MouseCursor::NormalCursor);
    repaint();
}

void TransportWaveform::mouseExit(const juce::MouseEvent&)
{
    hoverX_ = -1.0f;
    hoverRegion_ = -1;
    updateTooltip();
    repaint();
}

void TransportWaveform::mouseDown(const juce::MouseEvent& e)
{
    const auto wave = waveArea();
    const double t = tw::timeAtX(e.position.x - wave.getX(), wave.getWidth(), duration_);
    // A changed region's marker loops it.
    if (e.position.y >= markerArea().getY() - kMarkerGap) {
        const int region = tw::regionAt(regions_, t);
        if (region >= 0) {
            loopRegion(region);
            return;
        }
    }
    dragging_ = true;
    dragSeconds_ = t;
    repaint();
}

void TransportWaveform::mouseDrag(const juce::MouseEvent& e)
{
    if (!dragging_) return;
    const auto wave = waveArea();
    dragSeconds_ = tw::timeAtX(e.position.x - wave.getX(), wave.getWidth(), duration_);
    hoverX_ = -1.0f;
    repaint();
}

void TransportWaveform::mouseUp(const juce::MouseEvent&)
{
    if (!dragging_) return;
    dragging_ = false;
    seekTo(dragSeconds_);
}

void TransportWaveform::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto wave = waveArea();
    loopRegion(tw::regionAt(regions_, tw::timeAtX(e.position.x - wave.getX(), wave.getWidth(), duration_)));
}

bool TransportWaveform::keyPressed(const juce::KeyPress& key)
{
    const int code = key.getKeyCode();
    if (code == 'L' || code == 'l') {
        const int region = tw::regionAt(regions_, position_);
        if (region < 0) return false;
        loopRegion(region);
        return true;
    }
    if (code != juce::KeyPress::leftKey && code != juce::KeyPress::rightKey && code != juce::KeyPress::homeKey &&
        code != juce::KeyPress::endKey)
        return false;
    const double step = key.getModifiers().isShiftDown() ? 1.0 : 5.0;
    seekTo(tw::keyboardSeek(position_, duration_, code, step));
    return true;
}

namespace {
class ScrubValue final : public juce::AccessibilityRangedNumericValueInterface {
  public:
    explicit ScrubValue(TransportWaveform& owner) : owner_(owner) {}
    bool isReadOnly() const override { return owner_.duration() <= 0.0; }
    double getCurrentValue() const override { return owner_.position(); }
    void setValue(double newValue) override { owner_.seekTo(newValue); }
    AccessibleValueRange getRange() const override
    {
        return {{0.0, juce::jmax(1.0, owner_.duration())}, 1.0};
    }

  private:
    TransportWaveform& owner_;
};
} // namespace

std::unique_ptr<juce::AccessibilityHandler> TransportWaveform::createAccessibilityHandler()
{
    return std::make_unique<juce::AccessibilityHandler>(
        *this, juce::AccessibilityRole::slider, juce::AccessibilityActions{},
        juce::AccessibilityHandler::Interfaces{
            std::unique_ptr<juce::AccessibilityValueInterface>(std::make_unique<ScrubValue>(*this))});
}

} // namespace khensu
