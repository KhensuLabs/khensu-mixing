// desktop-app/Source/ui/VocalWorkspace/MixMapCanvas.cpp
#include "../shared/ServiceErrorHint.h"
#include "MixMapCanvas.h"
#include "../shared/PanelRunState.h"
#include "../../IAppController.h"
#include <khensu/ipc/HttpServiceBridge.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/ui/VizFrequencyAxis.h>

namespace khensu {

namespace af = api::fields; // generated service field names (ApiSchema.h)

namespace {
constexpr int kToolbarH = 26;
constexpr int kStatusH = 16;
constexpr float kStripH = 54.0f;
// VIS-3: the standard frequency axis (20 Hz - 20 kHz).
constexpr double kMinHz = ui::viz::kFreqMinHz, kMaxHz = ui::viz::kFreqMaxHz;

juce::String hzLabel(double hz)
{
    return hz >= 1000.0 ? juce::String(hz / 1000.0, hz >= 10000.0 ? 0 : 1) + "k" : juce::String(juce::roundToInt(hz));
}
} // namespace

juce::Colour MixMapCanvas::stemColour(const juce::String& stem)
{
    const auto s = stem.toLowerCase();
    if (s.contains("vocal")) return KhensuTheme::accentTeal;
    if (s.contains("drum")) return KhensuTheme::accentAmber;
    if (s.contains("bass")) return KhensuTheme::accentBlue;
    return KhensuTheme::accentMagenta;
}

void MixMapCanvas::applyThemeColours()
{
    for (auto* b : {&refreshButton_, &averageButton_}) {
        b->setColour(juce::TextButton::buttonColourId, KhensuTheme::accentTeal.withAlpha(0.15f));
        b->setColour(juce::TextButton::buttonOnColourId, KhensuTheme::accentTeal.withAlpha(0.45f));
        b->setColour(juce::TextButton::textColourOffId, KhensuTheme::accentTeal);
        b->setColour(juce::TextButton::textColourOnId, KhensuTheme::textPrimary);
    }
    statusLabel_.setColour(juce::Label::textColourId, KhensuTheme::textTertiary);
}

MixMapCanvas::MixMapCanvas(IAppController& c) : PanelCard(c, "Mix Map")
{
    applyThemeColours();
    setExplainer(TRANS("Mix Map"),
                 TRANS("One picture of how the stems share the mix. Each circle is a stem: left-to-right is where it "
                       "lives in the spectrum, size is its share of the mix's energy. Lines join stems that mask each "
                       "other - thicker and redder means a worse collision, labelled with the band where they clash. "
                       "The strip underneath is the energy timeline: the map follows playback, or click/drag the "
                       "strip to inspect any moment (masking is shown for that section when the analysis has it). "
                       "Song Average shows the whole track. Keyboard: Left/Right step 1 s, A toggles the average."));
    setWantsKeyboardFocus(true);
    setTitle(TRANS("Mix map"));
    setDescription(TRANS("Stems as nodes by spectrum position and energy, with masking links."));

    refreshButton_.setTooltip(TRANS("Measure stem energy and masking (uses the separated stems)."));
    refreshButton_.onClick = [this] { fetch(); };
    refreshButton_.setEnabled(false);
    addAndMakeVisible(refreshButton_);

    averageButton_.setClickingTogglesState(true);
    averageButton_.setTooltip(TRANS("Show the whole-song average instead of the current moment (A)."));
    averageButton_.onClick = [this] {
        followPlayback_ = !averageButton_.getToggleState();
        timeSec_ = averageButton_.getToggleState() ? -1.0 : controller_.playback().getPlaybackPosition();
        updateStatus();
        repaint();
    };
    addAndMakeVisible(averageButton_);

    statusLabel_.setFont(KhensuTheme::labelFont(type::caption));
    statusLabel_.setText(TRANS("Separate stems to build the mix map."), juce::dontSendNotification);
    addAndMakeVisible(statusLabel_);
    startTimerHz(15);
}

MixMapCanvas::~MixMapCanvas() { stopTimer(); }

void MixMapCanvas::setStemPaths(const juce::String& vocals, const juce::String& drums, const juce::String& bass,
                                const juce::String& other)
{
    const juce::String next[4] = {vocals, drums, bass, other};
    bool changed = false;
    for (int i = 0; i < 4; ++i)
        if (paths_[i] != next[i]) {
            paths_[i] = next[i];
            changed = true;
        }
    const bool any = vocals.isNotEmpty() || drums.isNotEmpty() || bass.isNotEmpty() || other.isNotEmpty();
    refreshButton_.setEnabled(any && !fetching_);
    // Both endpoints cache server-side by (path, mtime, size), so an
    // automatic build on new stems costs nothing on repeat visits.
    if (changed && any) fetch();
}

void MixMapCanvas::fetch()
{
    if (fetching_) return;
    fetching_ = true;
    refreshButton_.setEnabled(false);
    statusLabel_.setText(TRANS("Measuring stem energy and masking..."), juce::dontSendNotification);
    const juce::String v = paths_[0], d = paths_[1], b = paths_[2], o = paths_[3];
    fetchQuietly(*this, controller_, [v, d, b, o] {
        ipc::HttpServiceBridge bridge;
        auto masking = bridge.analyzeStemMasking(v.toStdString(), d.toStdString(), b.toStdString(), o.toStdString());
        auto* body = new juce::DynamicObject();
        body->setProperty("vocals_path", v);
        body->setProperty("drums_path", d);
        body->setProperty("bass_path", b);
        body->setProperty("other_path", o);
        auto energy = bridge.postAnalyzeJson("/analyze/stem_energy_timeline", juce::var(body));
        return std::make_pair(std::move(masking), std::move(energy));
    }, [](MixMapCanvas& self, auto&& fetched) {
        auto& [masking, energy] = fetched;
        if (masking.is_err() && energy.is_err()) {
            self.statusLabel_.setText(TRANS("Mix map unavailable: ") + serviceErrorLine(masking.error()),
                                       juce::dontSendNotification);
            return;
        }
        if (masking.is_ok()) {
            std::vector<MixMapModel::Pair> pairs;
            for (const auto& p : masking.value().pairs) {
                MixMapModel::Pair mp;
                mp.a = juce::String(p.stem_a);
                mp.b = juce::String(p.stem_b);
                mp.score = p.collision_score;
                mp.bandScores = p.band_scores;
                for (const auto& w : p.collisions_over_time)
                    mp.windows.push_back({w.start_sec, w.end_sec, w.collision_score});
                pairs.push_back(std::move(mp));
            }
            self.model_.setMasking(std::move(pairs), masking.value().bands_hz);
        }
        if (energy.is_ok()) {
            std::vector<MixMapModel::EnergyFrame> frames;
            if (auto* arr = energy.value()[af::StemEnergyTimelineResponse::frames].getArray())
                for (const auto& fv : *arr) {
                    MixMapModel::EnergyFrame f;
                    f.timeSec = static_cast<double>(fv[af::StemEnergyFrame::time_sec]);
                    if (auto* obj = fv[af::StemEnergyFrame::fractions].getDynamicObject())
                        for (const auto& prop : obj->getProperties())
                            f.fractions[prop.name.toString()] = static_cast<double>(prop.value);
                    frames.push_back(std::move(f));
                }
            self.model_.setEnergy(std::move(frames));
        }
        self.updateStatus();
        self.repaint();
    }, [](MixMapCanvas& self) {
        self.fetching_ = false;
        self.refreshButton_.setEnabled(true);
    });
}

juce::Rectangle<float> MixMapCanvas::mapArea() const
{
    auto b = bodyArea();
    b.removeFromTop(kToolbarH + 2 + kStatusH + 2);
    auto f = b.toFloat().reduced(4.0f);
    f.removeFromBottom(kStripH + 6.0f);
    return f;
}

juce::Rectangle<float> MixMapCanvas::stripArea() const
{
    auto f = bodyArea().toFloat().reduced(4.0f);
    return f.removeFromBottom(kStripH);
}

float MixMapCanvas::xForHz(double hz, const juce::Rectangle<float>& r) const
{
    const double frac = std::log(juce::jlimit(kMinHz, kMaxHz, hz) / kMinHz) / std::log(kMaxHz / kMinHz);
    return r.getX() + 30.0f + static_cast<float>(frac) * (r.getWidth() - 60.0f);
}

juce::Point<float> MixMapCanvas::nodeCentre(size_t index, size_t count, double hz,
                                            const juce::Rectangle<float>& r) const
{
    // Stagger rows so stems with similar spectral homes don't stack.
    const float row = count <= 1 ? 0.5f : static_cast<float>(index) / static_cast<float>(count - 1);
    const float y = r.getY() + 34.0f + row * (r.getHeight() - 68.0f);
    return {xForHz(hz, r), y};
}

void MixMapCanvas::timerCallback()
{
    if (!isShowing() || !followPlayback_ || model_.empty()) return;
    if (controller_.playback().isPlaying()) {
        const double t = controller_.playback().getPlaybackPosition();
        if (std::abs(t - timeSec_) > 0.05) {
            timeSec_ = t;
            updateStatus();
            repaint();
        }
    }
}

void MixMapCanvas::updateStatus()
{
    if (model_.empty()) return;
    const auto edges = model_.edges(timeSec_);
    juce::String s = timeSec_ < 0.0 ? juce::String("Song average") : "At " + juce::String(timeSec_, 1) + " s";
    if (!edges.empty())
        s << " - worst clash: " << edges.front().a << " / " << edges.front().b << " ("
          << juce::String(edges.front().score, 2) << ")";
    if (hoverText_.isNotEmpty()) s << "   " << hoverText_;
    statusLabel_.setText(s, juce::dontSendNotification);
}

void MixMapCanvas::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    const auto area = mapArea();
    if (area.getHeight() < 60.0f) return;
    if (model_.empty()) {
        drawEmptyState(g, "No mix map yet", "Separate stems, then Build Mix Map to see energy and masking at once.",
                       kToolbarH + kStatusH + 4);
        return;
    }

    g.setColour(KhensuTheme::bgVoid.withAlpha(0.35f));
    g.fillRoundedRectangle(area, 6.0f);

    // Frequency grid.
    g.setFont(KhensuTheme::labelFont(KhensuTheme::kFontLabel));
    for (const float tick : ui::viz::kFreqTicksHz) {
        const double hz = tick;
        const float x = xForHz(hz, area);
        g.setColour(KhensuTheme::borderSubtle.withAlpha(0.5f));
        g.drawVerticalLine(juce::roundToInt(x), area.getY() + 14.0f, area.getBottom());
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(ui::viz::freqTickLabel(tick), juce::Rectangle<float>(x - 20.0f, area.getY() + 1.0f, 40.0f, 12.0f),
                   juce::Justification::centred);
    }

    const auto nodes = model_.nodes(timeSec_);
    std::map<juce::String, juce::Point<float>> centres;
    std::map<juce::String, float> radii;
    for (size_t i = 0; i < nodes.size(); ++i) {
        centres[nodes[i].stem] = nodeCentre(i, nodes.size(), nodes[i].centreHz, area);
        radii[nodes[i].stem] = 10.0f + 38.0f * static_cast<float>(std::sqrt(nodes[i].energy));
    }

    // Masking edges first, so nodes sit on top.
    for (const auto& e : model_.edges(timeSec_, 0.05)) {
        if (!centres.count(e.a) || !centres.count(e.b)) continue;
        const auto pa = centres[e.a], pb = centres[e.b];
        const int sev = MixMapModel::severity(e.score);
        const auto colour = sev >= 3   ? KhensuTheme::accentRed
                            : sev == 2 ? KhensuTheme::accentAmber
                                       : KhensuTheme::accentBlue;
        g.setColour(colour.withAlpha(0.35f + 0.5f * static_cast<float>(e.score)));
        juce::Path link;
        const auto mid = (pa + pb) * 0.5f + juce::Point<float>(0.0f, -18.0f); // gentle arc
        link.startNewSubPath(pa);
        link.quadraticTo(mid, pb);
        g.strokePath(link, juce::PathStrokeType(1.0f + 9.0f * static_cast<float>(e.score)));
        if (sev >= 1 && e.clashHz > 0.0) {
            const auto label = "~" + hzLabel(e.clashHz) + "Hz " + juce::String(e.score, 2);
            const juce::Rectangle<float> box(mid.x - 38.0f, mid.y - 7.0f, 76.0f, 14.0f);
            g.setColour(KhensuTheme::bgOverlay.withAlpha(0.85f));
            g.fillRoundedRectangle(box, 3.0f);
            g.setColour(colour);
            g.drawText(label, box, juce::Justification::centred);
        }
    }

    for (const auto& n : nodes) {
        const auto c = centres[n.stem];
        const float r = radii[n.stem];
        const auto col = stemColour(n.stem);
        g.setColour(col.withAlpha(0.25f));
        g.fillEllipse(c.x - r, c.y - r, 2 * r, 2 * r);
        g.setColour(col);
        g.drawEllipse(c.x - r, c.y - r, 2 * r, 2 * r, 2.0f);
        g.setColour(KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.drawText(n.stem.toUpperCase(), juce::Rectangle<float>(c.x - 50.0f, c.y - 8.0f, 100.0f, 14.0f),
                   juce::Justification::centred);
        g.setFont(KhensuTheme::labelFont(type::caption));
        g.setColour(KhensuTheme::textSecondary);
        g.drawText(juce::String(juce::roundToInt(n.energy * 100.0)) + "%",
                   juce::Rectangle<float>(c.x - 30.0f, c.y + 5.0f, 60.0f, 12.0f), juce::Justification::centred);
    }

    // Energy timeline strip (stacked area) + time cursor.
    const auto strip = stripArea();
    g.setColour(KhensuTheme::bgVoid.withAlpha(0.5f));
    g.fillRect(strip);
    const auto& frames = model_.energyFrames();
    const double dur = model_.durationSec();
    if (frames.size() > 1 && dur > 0.0) {
        const auto stems = model_.stems();
        std::vector<float> base(frames.size(), 0.0f);
        for (const auto& stem : stems) {
            juce::Path band;
            juce::Path top;
            for (size_t i = 0; i < frames.size(); ++i) {
                const float x = strip.getX() + static_cast<float>(frames[i].timeSec / dur) * strip.getWidth();
                const auto it = frames[i].fractions.find(stem);
                const float frac = it == frames[i].fractions.end() ? 0.0f : static_cast<float>(it->second);
                const float y = strip.getBottom() - (base[i] + frac) * strip.getHeight();
                if (i == 0)
                    top.startNewSubPath(x, y);
                else
                    top.lineTo(x, y);
            }
            band = top;
            for (size_t i = frames.size(); i-- > 0;) {
                const float x = strip.getX() + static_cast<float>(frames[i].timeSec / dur) * strip.getWidth();
                band.lineTo(x, strip.getBottom() - base[i] * strip.getHeight());
                const auto it = frames[i].fractions.find(stem);
                base[i] += it == frames[i].fractions.end() ? 0.0f : static_cast<float>(it->second);
            }
            band.closeSubPath();
            g.setColour(stemColour(stem).withAlpha(0.55f));
            g.fillPath(band);
        }
        if (timeSec_ >= 0.0) {
            const float cx = strip.getX() + static_cast<float>(timeSec_ / dur) * strip.getWidth();
            g.setColour(KhensuTheme::textPrimary);
            g.drawLine(cx, strip.getY(), cx, strip.getBottom(), 1.5f);
        }
    }
    if (hasKeyboardFocus(false)) {
        g.setColour(KhensuTheme::focusRing.withAlpha(0.7f));
        g.drawRect(area, 1.0f);
    }
}

void MixMapCanvas::resized()
{
    PanelCard::resized();
    auto b = bodyArea();
    auto bar = b.removeFromTop(kToolbarH);
    refreshButton_.setBounds(bar.removeFromLeft(130).reduced(1));
    bar.removeFromLeft(8);
    averageButton_.setBounds(bar.removeFromLeft(120).reduced(1));
    b.removeFromTop(2);
    statusLabel_.setBounds(b.removeFromTop(kStatusH));
}

void MixMapCanvas::setTimeFromStrip(float x)
{
    const auto strip = stripArea();
    const double dur = model_.durationSec();
    if (dur <= 0.0) return;
    timeSec_ = juce::jlimit(0.0, dur, static_cast<double>((x - strip.getX()) / strip.getWidth()) * dur);
    followPlayback_ = false;
    averageButton_.setToggleState(false, juce::dontSendNotification);
    updateStatus();
    repaint();
}

void MixMapCanvas::mouseDown(const juce::MouseEvent& e)
{
    if (stripArea().contains(e.position) && !model_.empty()) {
        grabKeyboardFocus();
        setTimeFromStrip(e.position.x);
        return;
    }
    if (mapArea().contains(e.position)) grabKeyboardFocus();
    PanelCard::mouseDown(e);
}

void MixMapCanvas::mouseDrag(const juce::MouseEvent& e)
{
    if (stripArea().contains(e.mouseDownPosition) && !model_.empty()) {
        setTimeFromStrip(e.position.x);
        return;
    }
    PanelCard::mouseDrag(e);
}

void MixMapCanvas::mouseMove(const juce::MouseEvent& e)
{
    const auto area = mapArea();
    const auto nodes = model_.nodes(timeSec_);
    juce::String text;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto c = nodeCentre(i, nodes.size(), nodes[i].centreHz, area);
        const float r = 10.0f + 38.0f * static_cast<float>(std::sqrt(nodes[i].energy));
        if (c.getDistanceFrom(e.position) <= r) {
            text = nodes[i].stem + ": " + juce::String(juce::roundToInt(nodes[i].energy * 100.0)) +
                   "% of energy, centred ~" + hzLabel(nodes[i].centreHz) + "Hz";
            break;
        }
    }
    if (text != hoverText_) {
        hoverText_ = text;
        updateStatus();
    }
}

bool MixMapCanvas::keyPressed(const juce::KeyPress& key)
{
    if (model_.empty()) return false;
    const double dur = model_.durationSec();
    if (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey) {
        const double step = key.getKeyCode() == juce::KeyPress::leftKey ? -1.0 : 1.0;
        timeSec_ = juce::jlimit(0.0, dur, (timeSec_ < 0.0 ? 0.0 : timeSec_) + step);
        followPlayback_ = false;
        averageButton_.setToggleState(false, juce::dontSendNotification);
    } else if (key.getTextCharacter() == 'a' || key.getTextCharacter() == 'A') {
        averageButton_.setToggleState(!averageButton_.getToggleState(), juce::sendNotificationSync);
        return true;
    } else {
        return false;
    }
    updateStatus();
    repaint();
    return true;
}

} // namespace khensu
