// desktop-app/Source/ui/shared/StereoFieldVizComponent.cpp
#include "StereoFieldVizComponent.h"
#include "PanelRunState.h"
#include "../../IAppController.h"
#include <khensu/feedback/FeedbackManager.h>
#include <algorithm>
#include <numeric>
#include "KhensuTheme.h"

namespace khensu {

StereoFieldVizComponent::StereoFieldVizComponent(IAppController& controller) : controller_(controller)
{
    // A more front-facing default (previously yaw=0.35 made the room look
    // skewed/off-centre before any interaction) - still angled enough via
    // pitch to read as 3D, but the L/R walls start roughly symmetric.
    camera_.pitch = 0.45f;
    camera_.yaw = 0.12f;
    camera_.zoom = 3.2f;

    initStems();
}

void StereoFieldVizComponent::setCorrelation(float c)
{
    correlation_ = c;
    repaint();
}
void StereoFieldVizComponent::setWidthPercent(float w)
{
    widthPct_ = w;
    repaint();
}
void StereoFieldVizComponent::setDepthPercent(float d)
{
    depthPct_ = d;
    repaint();
}
void StereoFieldVizComponent::setGenreTarget(float w, float d)
{
    genreWidthPct_ = w;
    genreDepthPct_ = d;
    repaint();
}

void StereoFieldVizComponent::initStems()
{
    stems_.clear();
    stems_.push_back({"Vocals", {0.0f, 0.2f, 0.4f}, KhensuTheme::accentTeal, 12.0f, {}});
    stems_.push_back({"Drums", {-0.3f, -0.2f, -0.3f}, KhensuTheme::accentBlue, 14.0f, {}});
    stems_.push_back({"Bass", {0.0f, -0.5f, -0.7f}, KhensuTheme::accentAmber, 15.0f, {}});
    stems_.push_back({"Other", {0.4f, 0.4f, 0.0f}, KhensuTheme::accentGreen, 11.0f, {}});

    if (auto* fm = controller_.analysis().getFeedbackManager()) {
        for (auto& s : stems_) {
            if (auto rX = fm->getPreference("stem_pan_" + s.name.toStdString(), s.pos.x); rX.is_ok())
                s.pos.x = rX.value();
            if (auto rY = fm->getPreference("stem_depth_" + s.name.toStdString(), s.pos.y); rY.is_ok())
                s.pos.y = rY.value();
        }
    }
}

namespace {
// Distinctive sentinel: FeedbackManager::getPreference() echoes this back
// verbatim when the key has never been written, letting us tell "no saved
// drag yet" apart from "a real stored value of 0".
constexpr float kNoSavedPref = -999.0f;
} // namespace

void StereoFieldVizComponent::checkAutoPositionStems()
{
    if (stemsAutoPositioned_ || stemsAnalysisPending_) return;
    if (!controller_.stems().hasStemsAvailable()) return;

    stemsAnalysisPending_ = true;
    analyzeStemsAsync();
}

void StereoFieldVizComponent::analyzeStemsAsync()
{
    // Snapshot which stems still need an automatic position (i.e. the user
    // has never manually dragged them) before jumping off the message thread.
    juce::StringArray needsAutoPan, needsAutoDepth;
    if (auto* fm = controller_.analysis().getFeedbackManager()) {
        for (auto& s : stems_) {
            auto rX = fm->getPreference("stem_pan_" + s.name.toStdString(), kNoSavedPref);
            auto rY = fm->getPreference("stem_depth_" + s.name.toStdString(), kNoSavedPref);
            if (!(rX.is_ok() && rX.value() != kNoSavedPref)) needsAutoPan.add(s.name);
            if (!(rY.is_ok() && rY.value() != kNoSavedPref)) needsAutoDepth.add(s.name);
        }
    }

    if (needsAutoPan.isEmpty() && needsAutoDepth.isEmpty()) {
        stemsAutoPositioned_ = true;
        stemsAnalysisPending_ = false;
        return;
    }

    auto stemsDir = controller_.stems().getStemsDirectory();
    juce::Component::SafePointer<StereoFieldVizComponent> safeThis(this);

    controller_.runInBackground([safeThis, &work = controller_, stemsDir, needsAutoPan, needsAutoDepth]() {
        struct StemMetrics {
            juce::String name;
            float pan = 0.0f;
            float rms = 0.0f;
        };
        std::vector<StemMetrics> metrics;

        juce::AudioFormatManager fmMgr;
        fmMgr.registerBasicFormats();

        for (const char* raw : {"vocals", "drums", "bass", "other"}) {
            juce::String display = juce::String(raw).substring(0, 1).toUpperCase() + juce::String(raw).substring(1);
            auto file = stemsDir.getChildFile(juce::String(raw) + ".wav");
            if (!file.existsAsFile()) continue;

            std::unique_ptr<juce::AudioFormatReader> reader(fmMgr.createReaderFor(file));
            if (reader == nullptr) continue;

            const int numChannels = (int)reader->numChannels;
            const juce::int64 totalLen = reader->lengthInSamples;
            if (totalLen <= 0) continue;

            // Decimated read: sample ~2s worth of blocks spread across the
            // file rather than the whole thing, to keep this fast even for
            // long masters.
            const int blockSize = 4096;
            const int numBlocksToSample = 40;
            juce::int64 stride = juce::jmax((juce::int64)blockSize, totalLen / numBlocksToSample);

            double sumL = 0.0, sumR = 0.0, sumSq = 0.0;
            juce::int64 samplesRead = 0;
            juce::AudioBuffer<float> block(juce::jmax(2, numChannels), blockSize);

            for (juce::int64 pos = 0; pos < totalLen; pos += stride) {
                int toRead = (int)juce::jmin((juce::int64)blockSize, totalLen - pos);
                if (toRead <= 0) break;
                reader->read(&block, 0, toRead, pos, true, true);

                const float* l = block.getReadPointer(0);
                const float* r = numChannels > 1 ? block.getReadPointer(1) : l;
                for (int i = 0; i < toRead; ++i) {
                    sumL += std::abs(l[i]);
                    sumR += std::abs(r[i]);
                    sumSq += 0.5 * (l[i] * l[i] + r[i] * r[i]);
                }
                samplesRead += toRead;
            }

            if (samplesRead <= 0) continue;

            float pan = 0.0f;
            if (sumL + sumR > 1.0e-9) pan = (float)((sumL - sumR) / (sumL + sumR));
            float rms = (float)std::sqrt(sumSq / (double)samplesRead);

            metrics.push_back({display, juce::jlimit(-1.0f, 1.0f, pan * 2.0f), rms});
        }

        if (metrics.empty()) {
            postIfCurrent(safeThis, work, [](StereoFieldVizComponent& panel) {
                panel.stemsAutoPositioned_ = true;
            }, [](StereoFieldVizComponent& panel) { panel.stemsAnalysisPending_ = false; });
            return;
        }

        // Rank by loudness: the loudest stem sits nearest the listener
        // (depth toward +0.4), the quietest sits furthest back (-0.5).
        std::vector<size_t> order(metrics.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return metrics[a].rms > metrics[b].rms; });

        std::vector<float> depthByRank(metrics.size());
        for (size_t rank = 0; rank < order.size(); ++rank) {
            float t = order.size() > 1 ? (float)rank / (float)(order.size() - 1) : 0.0f;
            depthByRank[order[rank]] = juce::jmap(t, 0.0f, 1.0f, 0.4f, -0.5f);
        }

        postIfCurrent(safeThis, work, [metrics, depthByRank, needsAutoPan, needsAutoDepth](StereoFieldVizComponent& panel) {
            auto* fm = panel.controller_.analysis().getFeedbackManager();

            for (size_t i = 0; i < metrics.size(); ++i) {
                for (auto& s : panel.stems_) {
                    if (s.name != metrics[i].name) continue;

                    if (needsAutoPan.contains(s.name)) {
                        s.pos.x = metrics[i].pan;
                        if (fm) fm->setPreference("stem_pan_" + s.name.toStdString(), s.pos.x);
                    }
                    if (needsAutoDepth.contains(s.name)) {
                        s.pos.y = depthByRank[i];
                        if (fm) fm->setPreference("stem_depth_" + s.name.toStdString(), s.pos.y);
                    }
                }
            }

            panel.stemsAutoPositioned_ = true;
            panel.stemsAnalysisPending_ = false;
            panel.updateProjectedPositions();
            panel.repaint();
        }, [](StereoFieldVizComponent& panel) { panel.stemsAnalysisPending_ = false; });
    });
}

void StereoFieldVizComponent::updateProjectedPositions()
{
    float w = (float)getWidth();
    float h = (float)getHeight();

    for (auto& s : stems_) {
        s.projectedPos = camera_.project(s.pos, w, h);
    }
}

void StereoFieldVizComponent::resized() { updateProjectedPositions(); }

void StereoFieldVizComponent::mouseDown(const juce::MouseEvent& e)
{
    selectedStemIdx_ = -1;

    if (controller_.stems().hasStemsAvailable()) {
        float minDist = 99999.0f;
        const auto clickPos = e.position;

        for (int i = 0; i < (int)stems_.size(); ++i) {
            float dist = clickPos.getDistanceFrom(stems_[i].projectedPos);
            if (dist <= stems_[i].radius * 2.0f && dist < minDist) {
                minDist = dist;
                selectedStemIdx_ = i;
            }
        }
    }

    // A click that didn't land on a stem sphere (or stems aren't available
    // yet, e.g. the "Your Mix"/"Genre Avg" fallback view) orbits the camera
    // instead - previously a background drag did nothing at all, so the
    // fixed initial angle was the only view ever shown.
    orbitingCamera_ = selectedStemIdx_ < 0;
    lastCameraDragPos_ = e.position;
}

void StereoFieldVizComponent::mouseDrag(const juce::MouseEvent& e)
{
    if (orbitingCamera_) {
        const auto delta = e.position - lastCameraDragPos_;
        camera_.yaw += delta.x * 0.01f;
        camera_.pitch = juce::jlimit(-1.4f, 1.4f, camera_.pitch + delta.y * 0.01f);
        lastCameraDragPos_ = e.position;
        updateProjectedPositions();
        repaint();
        return;
    }

    if (selectedStemIdx_ < 0 || selectedStemIdx_ >= (int)stems_.size()) return;

    float w = (float)getWidth();
    float h = (float)getHeight();
    if (w <= 0.0f || h <= 0.0f) return; // degenerate bounds: the back-mapping below divides by these

    float cx = w * 0.5f;
    float cy = h * 0.5f;

    // Direct back-mapping delta to room coordinates
    float dx = (e.position.x - cx) / (w * 0.4f);
    float dy = -(e.position.y - cy) / (h * 0.4f);

    auto& s = stems_[selectedStemIdx_];
    s.pos.x = juce::jlimit(-1.0f, 1.0f, dx);
    s.pos.y = juce::jlimit(-1.0f, 1.0f, dy);

    if (auto* fm = controller_.analysis().getFeedbackManager()) {
        fm->setPreference("stem_pan_" + s.name.toStdString(), s.pos.x);
        fm->setPreference("stem_depth_" + s.name.toStdString(), s.pos.y);
    }

    updateProjectedPositions();
    repaint();
}

void StereoFieldVizComponent::mouseUp(const juce::MouseEvent&)
{
    selectedStemIdx_ = -1;
    orbitingCamera_ = false;
}

void StereoFieldVizComponent::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    camera_.zoom = juce::jlimit(1.5f, 8.0f, camera_.zoom - wheel.deltaY * 1.5f);
    updateProjectedPositions();
    repaint();
}

void StereoFieldVizComponent::paint(juce::Graphics& g)
{
    auto b = getLocalBounds();
    g.setColour(KhensuTheme::bgSurface.withAlpha(0.6f));
    g.fillRect(b);

    float w = (float)b.getWidth();
    float h = (float)b.getHeight();

    // 1. Draw 3D Room Grid and Walls
    g.setColour(KhensuTheme::borderSubtle.withAlpha(0.2f));

    auto projectAndDrawLine = [&](juce::Vector3D<float> p1, juce::Vector3D<float> p2) {
        auto pt1 = camera_.project(p1, w, h);
        auto pt2 = camera_.project(p2, w, h);
        g.drawLine(pt1.x, pt1.y, pt2.x, pt2.y, 0.8f);
    };

    // Draw floor grid (Z = -0.8)
    float zFloor = -0.8f;
    float zCeiling = 0.8f;
    for (float gridPos = -1.0f; gridPos <= 1.001f; gridPos += 0.5f) {
        projectAndDrawLine({gridPos, -1.0f, zFloor}, {gridPos, 1.0f, zFloor});
        projectAndDrawLine({-1.0f, gridPos, zFloor}, {1.0f, gridPos, zFloor});
    }

    // Draw vertical room corners
    projectAndDrawLine({-1, -1, zFloor}, {-1, -1, zCeiling});
    projectAndDrawLine({1, -1, zFloor}, {1, -1, zCeiling});
    projectAndDrawLine({1, 1, zFloor}, {1, 1, zCeiling});
    projectAndDrawLine({-1, 1, zFloor}, {-1, 1, zCeiling});

    // Draw ceiling frame
    projectAndDrawLine({-1, -1, zCeiling}, {1, -1, zCeiling});
    projectAndDrawLine({1, -1, zCeiling}, {1, 1, zCeiling});
    projectAndDrawLine({1, 1, zCeiling}, {-1, 1, zCeiling});
    projectAndDrawLine({-1, 1, zCeiling}, {-1, -1, zCeiling});

    // Draw room back wall boundary (Y = 1.0)
    g.setColour(KhensuTheme::borderSubtle.withAlpha(0.1f));
    auto bl = camera_.project({-1, 1, zFloor}, w, h);
    auto br = camera_.project({1, 1, zFloor}, w, h);
    auto tl = camera_.project({-1, 1, zCeiling}, w, h);
    auto tr = camera_.project({1, 1, zCeiling}, w, h);
    juce::Path wall;
    wall.startNewSubPath(bl.x, bl.y);
    wall.lineTo(br.x, br.y);
    wall.lineTo(tr.x, tr.y);
    wall.lineTo(tl.x, tl.y);
    wall.closeSubPath();
    g.fillPath(wall);

    // Coordinate axes labels projected in 3D
    g.setFont(KhensuTheme::labelFont(8.5f));
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.6f));
    auto pLeft = camera_.project({-1.2f, 0.0f, zFloor}, w, h);
    auto pRight = camera_.project({1.2f, 0.0f, zFloor}, w, h);
    auto pFront = camera_.project({0.0f, -1.2f, zFloor}, w, h);
    auto pBack = camera_.project({0.0f, 1.2f, zFloor}, w, h);

    g.drawText("L", (int)pLeft.x - 10, (int)pLeft.y - 6, 20, 12, juce::Justification::centred);
    g.drawText("R", (int)pRight.x - 10, (int)pRight.y - 6, 20, 12, juce::Justification::centred);
    g.drawText("FRONT", (int)pFront.x - 20, (int)pFront.y - 6, 40, 12, juce::Justification::centred);
    g.drawText("BACK", (int)pBack.x - 20, (int)pBack.y - 6, 40, 12, juce::Justification::centred);

    // 2. Render Objects (Stems or Overall Mix)
    bool stemsAvailable = controller_.stems().hasStemsAvailable();
    if (stemsAvailable) checkAutoPositionStems();
    updateProjectedPositions();

    auto getViewDepth = [&](const juce::Vector3D<float>& p) -> float {
        Matrix4x4 view = camera_.getViewMatrix();
        return p.x * view.m[2] + p.y * view.m[6] + p.z * view.m[10] + view.m[14];
    };

    struct RenderableSphere {
        juce::String label;
        juce::Vector3D<float> roomPos;
        juce::Point<float> screenPos;
        juce::Colour colour;
        float radius;
        bool isGenreTarget;
    };

    std::vector<RenderableSphere> objects;

    if (stemsAvailable) {
        for (const auto& s : stems_) {
            objects.push_back({s.name, s.pos, s.projectedPos, s.colour, s.radius, false});
        }
    } else {
        // Draw standard "Your Mix" and "Genre Target" dots
        float mixX = (widthPct_ - 50.0f) / 50.0f;
        float mixY = (depthPct_ - 50.0f) / 50.0f;
        float mixZ = correlation_ * 0.5f;

        float genreX = (genreWidthPct_ - 50.0f) / 50.0f;
        float genreY = (genreDepthPct_ - 50.0f) / 50.0f;
        float genreZ = 0.2f;

        objects.push_back({"Your Mix",
                           {mixX, mixY, mixZ},
                           camera_.project({mixX, mixY, mixZ}, w, h),
                           KhensuTheme::accentTeal,
                           12.0f,
                           false});
        objects.push_back({"Genre Avg",
                           {genreX, genreY, genreZ},
                           camera_.project({genreX, genreY, genreZ}, w, h),
                           KhensuTheme::accentAmber,
                           10.0f,
                           true});
    }

    // Sort back-to-front (Painters algorithm)
    std::sort(objects.begin(), objects.end(), [&](const RenderableSphere& a, const RenderableSphere& b) {
        return getViewDepth(a.roomPos) > getViewDepth(b.roomPos);
    });

    for (const auto& obj : objects) {
        float rad = obj.radius;

        // Draw drop shadow on floor (Z = zFloor)
        auto floorProj = camera_.project({obj.roomPos.x, obj.roomPos.y, zFloor}, w, h);
        g.setColour(khensu::KhensuTheme::shadowColour.withAlpha(0.4f));
        g.fillEllipse(floorProj.x - rad * 0.8f, floorProj.y - rad * 0.4f, rad * 1.6f, rad * 0.8f);

        // Draw shadow line connecting sphere to floor
        g.setColour(khensu::KhensuTheme::shadowColour.withAlpha(0.2f));
        g.drawLine(obj.screenPos.x, obj.screenPos.y, floorProj.x, floorProj.y, 1.0f);

        // 3D sphere gradient glow
        juce::ColourGradient glow(obj.colour.withAlpha(0.35f), obj.screenPos.x, obj.screenPos.y,
                                  juce::Colours::transparentBlack, obj.screenPos.x + rad * 2.0f, obj.screenPos.y, true);
        g.setGradientFill(glow);
        g.fillEllipse(obj.screenPos.x - rad * 2.0f, obj.screenPos.y - rad * 2.0f, rad * 4.0f, rad * 4.0f);

        // Solid core fill
        g.setColour(obj.colour);
        g.fillEllipse(obj.screenPos.x - rad, obj.screenPos.y - rad, rad * 2.0f, rad * 2.0f);

        // Specular highlight
        g.setColour(juce::Colours::white.withAlpha(0.6f)); // theme-exempt(specular light on a coloured sphere - physical, not themed)
        g.fillEllipse(obj.screenPos.x - rad * 0.5f, obj.screenPos.y - rad * 0.5f, rad * 0.5f, rad * 0.5f);

        // Label
        g.setFont(KhensuTheme::labelFont(9.0f));
        g.setColour(KhensuTheme::textPrimary);
        g.drawText(obj.label, (int)(obj.screenPos.x - 50.0f), (int)(obj.screenPos.y + rad + 2.0f), 100, 12,
                   juce::Justification::centred);
    }

    // Legend at bottom
    auto legendArea = getLocalBounds().removeFromBottom(16).toFloat();
    float lx = legendArea.getCentreX() - 90.0f;
    g.setFont(KhensuTheme::labelFont(9.0f));

    if (stemsAvailable) {
        g.setColour(KhensuTheme::textTertiary);
        g.drawText(stemsAnalysisPending_ ? "3D Soundstage: positioning stems from analysis..."
                                         : "3D Soundstage: auto-positioned - drag to fine-tune",
                   (int)lx - 20, (int)legendArea.getY(), 240, 14, juce::Justification::centred, false);
    } else {
        g.setColour(KhensuTheme::accentTeal);
        g.fillEllipse(lx, legendArea.getCentreY() - 3.0f, 6.0f, 6.0f);
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("Your mix", (int)(lx + 10.0f), (int)legendArea.getY(), 60, 14, juce::Justification::left, false);

        float lx2 = legendArea.getCentreX() + 20.0f;
        g.setColour(KhensuTheme::accentAmber);
        g.drawEllipse(lx2, legendArea.getCentreY() - 3.0f, 6.0f, 6.0f, 1.0f);
        g.setColour(KhensuTheme::textTertiary);
        g.drawText("Genre avg", (int)(lx2 + 10.0f), (int)legendArea.getY(), 60, 14, juce::Justification::left, false);
    }
}

} // namespace khensu
