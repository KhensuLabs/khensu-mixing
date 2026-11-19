// desktop-app/Source/ui/shared/SonicOrbitComponent.cpp
#include "SonicOrbitComponent.h"
#include "PanelRunState.h"
#include <juce_opengl/juce_opengl.h>
#include "KhensuLookAndFeel.h"
#include "KhensuTheme.h"
#include "../../IAppController.h"
#include <khensu/analysis/AudioAnalyzer.h>
#include <khensu/session/AudioFileRef.h>
#include <khensu/targets/GenreEntry.h>
#include <cmath>
#include <numeric>
#include <khensu/targets/GenreDatabase.h>

namespace khensu {

static const char* vertexShader = "#version 120\n"
                                  "attribute vec3 position;\n"
                                  "attribute vec4 color;\n"
                                  "varying vec4 vColor;\n"
                                  "uniform mat4 projectionMatrix;\n"
                                  "uniform mat4 viewMatrix;\n"
                                  "void main() {\n"
                                  "    vColor = color;\n"
                                  "    gl_Position = projectionMatrix * viewMatrix * vec4(position, 1.0);\n"
                                  "}\n";

static const char* fragmentShader = "#version 120\n"
                                    "varying vec4 vColor;\n"
                                    "void main() {\n"
                                    "    gl_FragColor = vColor;\n"
                                    "}\n";

SonicOrbitComponent::SonicOrbitComponent(IAppController& controller) : controller_(controller)
{
    camera_.pitch = 0.35f;
    camera_.yaw = 0.0f;
    camera_.zoom = 3.5f;

    openGLContext_.setRenderer(this);
    openGLContext_.attachTo(*this);

    loadGenrePointCloud();
    startTimer(33); // ~30 FPS animation rate
}

SonicOrbitComponent::~SonicOrbitComponent()
{
    openGLContext_.detach();
    openGLContext_.setRenderer(nullptr);
}

void SonicOrbitComponent::loadGenrePointCloud()
{
    genrePointCloud3D_.clear();
    const auto genres = controller_.genreDatabase().listAll();

    for (const auto& g : genres) {
        // X: width/correlation mapping
        float avgWidth = (g.stereo_width_min + g.stereo_width_max) * 0.5f;
        float x = juce::jlimit(-1.0f, 1.0f, 1.0f - avgWidth * 2.0f);

        // Y: Crest factor dynamics mapping
        float y = juce::jlimit(-1.0f, 1.0f, (g.crest_factor_target_db - 12.0f) / 6.0f);

        // Z: Loudness mapping
        float z = juce::jlimit(-1.0f, 1.0f, (g.lufs_target + 15.0f) / 9.0f);

        // Visual coloring based on genre family
        juce::Colour colour = KhensuTheme::textTertiary.withAlpha(0.5f);
        if (g.parent == "pop")
            colour = KhensuTheme::accentTeal.withAlpha(0.6f);
        else if (g.parent == "hip-hop")
            colour = KhensuTheme::accentBlue.withAlpha(0.6f);
        else if (g.parent == "electronic")
            colour = KhensuTheme::accentRed.withAlpha(0.6f);
        else if (g.parent == "rock" || g.parent == "metal")
            colour = KhensuTheme::accentAmber.withAlpha(0.6f);

        genrePointCloud3D_.push_back({x, y, z, colour, g.name, false});
    }
}

void SonicOrbitComponent::setAnalysisData(const AnalysisResult& r)
{
    lastAnalysisResult_ = r;

    points3D_.clear();
    sectionTimeRanges_.clear();

    if (r.section_analysis && !r.section_analysis->sections.empty()) {
        totalDuration_ = r.section_analysis->total_duration_sec;
        const auto& sections = r.section_analysis->sections;
        const int n = (int)sections.size();

        for (int i = 0; i < n; ++i) {
            const auto& s = sections[(size_t)i];
            sectionTimeRanges_.emplace_back(s.start_sec, s.end_sec);

            float x = juce::jlimit(-1.0f, 1.0f, s.stereo_correlation);
            float crest = s.crest_db > 0.0f ? s.crest_db : 12.0f;
            float y = juce::jlimit(-1.0f, 1.0f, (crest - 12.0f) / 6.0f);
            float lufs = s.lufs_integrated < 0.0f ? s.lufs_integrated : -14.0f;
            float z = juce::jlimit(-1.0f, 1.0f, (lufs + 15.0f) / 9.0f);

            float t = n > 1 ? (float)i / (float)(n - 1) : 0.0f;
            juce::Colour colour = KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentBlue, t);

            points3D_.push_back({x, y, z, colour, s.label, false});
        }

        points3D_.push_back({0.0f, 0.0f, 0.2f, KhensuTheme::accentAmber, "Reference Target", true});
    } else {
        totalDuration_ = 225.0;
        sectionTimeRanges_ = {{0.0, 30.0}, {30.0, 90.0}, {90.0, 150.0}, {150.0, 210.0}, {210.0, 225.0}};
        points3D_ = {{0.15f, -0.2f, -0.3f, KhensuTheme::accentTeal, "Intro (Demo)", false},
                     {0.35f, 0.1f, 0.1f, KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentBlue, 0.25f),
                      "Verse 1 (Demo)", false},
                     {0.65f, 0.4f, 0.5f, KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentBlue, 0.5f),
                      "Chorus 1 (Demo)", false},
                     {0.45f, 0.0f, 0.0f, KhensuTheme::accentTeal.interpolatedWith(KhensuTheme::accentBlue, 0.75f),
                      "Verse 2 (Demo)", false},
                     {0.75f, 0.5f, 0.6f, KhensuTheme::accentBlue, "Chorus 2 (Demo)", false},
                     {0.0f, 0.0f, 0.2f, KhensuTheme::accentAmber, "Reference Target (Demo)", true}};
    }

    if (!renderHistoryMode_) {
        projectPoints();
        repaint();
    }
}

void SonicOrbitComponent::setRenderHistoryMode(bool enabled)
{
    if (renderHistoryMode_ == enabled) return;
    renderHistoryMode_ = enabled;

    if (enabled) {
        loadRenderHistoryPoints();
    } else {
        projectPoints();
        repaint();
    }
}

void SonicOrbitComponent::loadRenderHistoryPoints()
{
    const auto history = controller_.renders().getAppliedRenderHistory();
    renderHistoryPoints3D_.clear();

    if (history.empty()) {
        projectPoints();
        repaint();
        return;
    }

    renderHistoryLoading_ = true;
    repaint();

    std::vector<juce::String> paths(history.begin(), history.end());
    fetchQuietly(*this, controller_, [paths]() {
        std::vector<Point3D> pts;
        AudioAnalyzer analyzer;
        const int n = (int)paths.size();
        for (int i = 0; i < n; ++i) {
            AudioFileRef ref;
            ref.path = paths[(size_t)i].toStdString();
            auto result = analyzer.analyze(ref);
            if (!result.is_ok()) continue;
            const auto& ar = result.value();

            const float x = juce::jlimit(-1.0f, 1.0f, ar.stereo.correlation);
            const float crest = ar.dynamics.crest_factor_db > 0.0f ? ar.dynamics.crest_factor_db : 12.0f;
            const float y = juce::jlimit(-1.0f, 1.0f, (crest - 12.0f) / 6.0f);
            const float lufs = ar.loudness.lufs_integrated < 0.0f ? ar.loudness.lufs_integrated : -14.0f;
            const float z = juce::jlimit(-1.0f, 1.0f, (lufs + 15.0f) / 9.0f);

            const float t = n > 1 ? (float)i / (float)(n - 1) : 1.0f;
            const juce::Colour colour = KhensuTheme::textTertiary.interpolatedWith(KhensuTheme::accentTeal, t);

            pts.push_back({x, y, z, colour, "Render " + juce::String(i + 1), false});
        }
        pts.push_back({0.0f, 0.0f, 0.2f, KhensuTheme::accentAmber, "Reference Target", true});

        return pts;
    }, [](SonicOrbitComponent& self, std::vector<Point3D>&& pts) {
        self.renderHistoryPoints3D_ = std::move(pts);
        if (self.renderHistoryMode_) {
            self.projectPoints();
            self.repaint();
        }
    }, [](SonicOrbitComponent& self) { self.renderHistoryLoading_ = false; });
}

void SonicOrbitComponent::projectPoints()
{
    projected_.clear();
    const auto& active = renderHistoryMode_ ? renderHistoryPoints3D_ : points3D_;
    if (active.empty()) return;

    float w = (float)getWidth();
    float h = (float)getHeight();

    for (const auto& p : active) {
        auto pt = camera_.project({p.x, p.y, p.z}, w, h);

        // Depth mapping Z after camera transform
        Matrix4x4 view = camera_.getViewMatrix();
        float depth = p.x * view.m[2] + p.y * view.m[6] + p.z * view.m[10] + view.m[14];
        float scale = 3.5f / (3.5f + depth);

        projected_.push_back({pt.x, pt.y, scale, depth, p.colour, p.label, p.isReference});
    }
}

void SonicOrbitComponent::timerCallback()
{
    // Camera3D::getViewMatrix() always looks at the origin - the amber
    // "Reference Target" point placed there in setAnalysisData()/
    // loadRenderHistoryPoints() is the fixed object the view orbits around.
    // Auto-rotate only when the user isn't actively panning it themselves.
    if (!dragging_) {
        camera_.yaw += 0.006f;
        if (camera_.yaw > juce::MathConstants<float>::twoPi) camera_.yaw -= juce::MathConstants<float>::twoPi;
    }

    projectPoints();
    repaint();
}

void SonicOrbitComponent::mouseDown(const juce::MouseEvent& e)
{
    dragging_ = true;
    lastDragPos_ = e.position;
}

void SonicOrbitComponent::mouseDrag(const juce::MouseEvent& e)
{
    const auto delta = e.position - lastDragPos_;
    camera_.yaw += delta.x * 0.01f;
    camera_.pitch = juce::jlimit(-1.4f, 1.4f, camera_.pitch + delta.y * 0.01f);
    lastDragPos_ = e.position;
    projectPoints();
    repaint();
}

void SonicOrbitComponent::mouseUp(const juce::MouseEvent&) { dragging_ = false; }

void SonicOrbitComponent::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    camera_.zoom = juce::jlimit(1.5f, 8.0f, camera_.zoom - wheel.deltaY * 1.5f);
    projectPoints();
    repaint();
}

void SonicOrbitComponent::newOpenGLContextCreated()
{
    auto program = std::make_unique<juce::OpenGLShaderProgram>(openGLContext_);
    if (program->addVertexShader(vertexShader) && program->addFragmentShader(fragmentShader) && program->link()) {
        shaderProgram_ = std::move(program);
        projectionUniform_ = std::make_unique<juce::OpenGLShaderProgram::Uniform>(*shaderProgram_, "projectionMatrix");
        viewUniform_ = std::make_unique<juce::OpenGLShaderProgram::Uniform>(*shaderProgram_, "viewMatrix");
    } else {
        // Compile/link failed (old driver, no GLSL support): drop the program
        // so renderOpenGL() takes the fixed-function path. It previously kept a
        // non-null but UNLINKED program, whose null uniforms were then
        // dereferenced on the very next frame - a hard crash on exactly the
        // machines the fallback path exists for (kimi audit P0 #11).
        shaderProgram_.reset();
        projectionUniform_.reset();
        viewUniform_.reset();
    }
}

void SonicOrbitComponent::openGLContextClosing()
{
    shaderProgram_.reset();
    projectionUniform_.reset();
    viewUniform_.reset();
}

struct Vertex {
    float x, y, z;
    float r, g, b, a;
};

static void drawVertices(const std::vector<Vertex>& vertices, GLenum mode, float sizeOrWidth,
                         juce::OpenGLShaderProgram* shaderProgram)
{
    using namespace juce::gl;
    if (vertices.empty()) return;

    if (mode == GL_POINTS) {
        glPointSize(sizeOrWidth);
    } else {
        glLineWidth(sizeOrWidth);
    }

    if (shaderProgram != nullptr) {
        GLint posAttrib = glGetAttribLocation(shaderProgram->getProgramID(), "position");
        GLint colAttrib = glGetAttribLocation(shaderProgram->getProgramID(), "color");

        if (posAttrib >= 0 && colAttrib >= 0) {
            glEnableVertexAttribArray((GLuint)posAttrib);
            glEnableVertexAttribArray((GLuint)colAttrib);

            glVertexAttribPointer((GLuint)posAttrib, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), &vertices[0].x);
            glVertexAttribPointer((GLuint)colAttrib, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), &vertices[0].r);

            glDrawArrays(mode, 0, (GLsizei)vertices.size());

            glDisableVertexAttribArray((GLuint)posAttrib);
            glDisableVertexAttribArray((GLuint)colAttrib);
            return;
        }
    }

    // Fallback if shader is not bound or attributes not found
    glBegin(mode);
    for (const auto& v : vertices) {
        glColor4f(v.r, v.g, v.b, v.a);
        glVertex3f(v.x, v.y, v.z);
    }
    glEnd();
}

void SonicOrbitComponent::renderOpenGL()
{
    frameTimer_.beginFrame();

    using namespace juce::gl;
    // Clear screen background
    juce::OpenGLHelpers::clear(KhensuTheme::bgVoid);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    float w = (float)getWidth();
    float h = (float)getHeight();
    float aspect = w / (h > 0 ? h : 1.0f);

    Matrix4x4 proj = camera_.getProjectionMatrix(aspect);
    Matrix4x4 view = camera_.getViewMatrix();

    if (shaderProgram_ != nullptr && projectionUniform_ != nullptr && viewUniform_ != nullptr) {
        shaderProgram_->use();
        projectionUniform_->setMatrix4(proj.m, 1, false);
        viewUniform_->setMatrix4(view.m, 1, false);
    } else {
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(proj.m);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(view.m);
    }

    // 1. Draw grid room axes
    std::vector<Vertex> axisVerts = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.8f, 0.8f, 0.3f}, {1.2f, 0.0f, 0.0f, 0.0f, 0.8f, 0.8f, 0.3f}, // Width (X)
        {0.0f, 0.0f, 0.0f, 0.8f, 0.4f, 0.8f, 0.3f}, {0.0f, 1.2f, 0.0f, 0.8f, 0.4f, 0.8f, 0.3f}, // Dynamics (Y)
        {0.0f, 0.0f, 0.0f, 0.8f, 0.8f, 0.0f, 0.3f}, {0.0f, 0.0f, 1.2f, 0.8f, 0.8f, 0.0f, 0.3f}  // Loudness (Z)
    };
    drawVertices(axisVerts, GL_LINES, 1.5f, shaderProgram_.get());

    // 2. Draw reference polar/floor lines
    std::vector<Vertex> floorVerts;
    floorVerts.reserve(32);
    for (int i = 0; i < 32; ++i) {
        float ang = (float)i * juce::MathConstants<float>::twoPi / 32.0f;
        floorVerts.push_back({std::cos(ang) * 1.0f, std::sin(ang) * 1.0f, -0.8f, 0.3f, 0.3f, 0.3f, 0.15f});
    }
    drawVertices(floorVerts, GL_LINE_LOOP, 1.0f, shaderProgram_.get());

    // 3. Draw Genre Point Cloud (Galaxy Background)
    if (showPointCloud_) {
        std::vector<Vertex> cloudVerts;
        cloudVerts.reserve(genrePointCloud3D_.size());
        for (const auto& p : genrePointCloud3D_) {
            cloudVerts.push_back(
                {p.x, p.y, p.z, p.colour.getFloatRed(), p.colour.getFloatGreen(), p.colour.getFloatBlue(), 0.3f});
        }
        drawVertices(cloudVerts, GL_POINTS, 4.0f, shaderProgram_.get());
    }

    // 4. Draw Active Track Trails / Sections
    const auto& active = renderHistoryMode_ ? renderHistoryPoints3D_ : points3D_;
    if (!active.empty()) {
        // Draw connecting line trail
        std::vector<Vertex> trailVerts;
        trailVerts.reserve(active.size());
        for (const auto& p : active) {
            if (p.isReference) continue;
            trailVerts.push_back(
                {p.x, p.y, p.z, p.colour.getFloatRed(), p.colour.getFloatGreen(), p.colour.getFloatBlue(), 0.6f});
        }
        drawVertices(trailVerts, GL_LINE_STRIP, 2.0f, shaderProgram_.get());

        // Draw points/nodes
        std::vector<Vertex> nodeVerts;
        nodeVerts.reserve(active.size());
        for (const auto& p : active) {
            nodeVerts.push_back(
                {p.x, p.y, p.z, p.colour.getFloatRed(), p.colour.getFloatGreen(), p.colour.getFloatBlue(), 0.9f});
        }
        drawVertices(nodeVerts, GL_POINTS, 8.0f, shaderProgram_.get());
    }

    frameTimer_.endFrame();
}

void SonicOrbitComponent::paint(juce::Graphics& g)
{
    // Draw transparent HUD backing overlay
    auto b = getLocalBounds();
    g.setColour(KhensuTheme::borderSubtle);
    g.drawRoundedRectangle(b.toFloat(), 6.0f, 1.0f);

    float w = (float)b.getWidth();
    float h = (float)b.getHeight();

    const auto& active = renderHistoryMode_ ? renderHistoryPoints3D_ : points3D_;
    if (active.empty()) {
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(KhensuTheme::bodyFont(12.0f));
        g.drawText(renderHistoryLoading_ ? "Analyzing render history..."
                   : renderHistoryMode_  ? "No applied renders yet in this session."
                                         : "Awaiting track analysis...",
                   b, juce::Justification::centred, false);
        return;
    }

    // HUD Header Title
    g.setFont(KhensuTheme::labelFont(9.0f));
    g.setColour(KhensuTheme::textTertiary.withAlpha(0.8f));
    g.drawText(renderHistoryMode_ ? "RENDER HISTORY (GL 3D)" : "TIMBRE SPACE (GL 3D)",
               b.removeFromTop(20).reduced(6, 0), juce::Justification::centredLeft, false);

    // Label 3D projected endpoints of coordinate axes
    auto pX = camera_.project({1.2f, 0.0f, 0.0f}, w, h);
    auto pY = camera_.project({0.0f, 1.2f, 0.0f}, w, h);
    auto pZ = camera_.project({0.0f, 0.0f, 1.2f}, w, h);

    g.setFont(KhensuTheme::labelFont(8.0f));
    g.setColour(KhensuTheme::textTertiary);
    g.drawText("ST WIDTH", (int)pX.x - 25, (int)pX.y - 6, 50, 12, juce::Justification::centred);
    g.drawText("DYNAMICS", (int)pY.x - 25, (int)pY.y - 6, 50, 12, juce::Justification::centred);
    g.drawText("LOUDNESS", (int)pZ.x - 25, (int)pZ.y - 6, 50, 12, juce::Justification::centred);

    // Dynamic pulse for active section
    int activeSectionIndex = -1;
    if (controller_.playback().isPlaying() && totalDuration_ > 0.0) {
        double pos = controller_.playback().getPlaybackPosition();
        for (size_t i = 0; i < sectionTimeRanges_.size(); ++i) {
            if (pos >= sectionTimeRanges_[i].first && pos <= sectionTimeRanges_[i].second) {
                activeSectionIndex = (int)i;
                break;
            }
        }
    }

    // Draw text labels next to projected coordinates of active items
    for (size_t i = 0; i < projected_.size(); ++i) {
        const auto& p = projected_[i];
        bool isActive = (static_cast<int>(i) == activeSectionIndex);

        if (isActive) {
            // Pulse circle overlay in 2D
            float pulse = 1.0f + 0.15f * std::sin((float)juce::Time::getMillisecondCounter() * 0.007f);
            float radius = 8.0f * p.scale * pulse;
            g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.3f));
            g.drawEllipse(p.x - radius, p.y - radius, radius * 2.0f, radius * 2.0f, 1.5f);
        }

        // Draw HUD Labels next to projected points
        g.setFont(KhensuTheme::labelFont(8.5f));
        g.setColour(p.isReference ? KhensuTheme::accentAmber : KhensuTheme::textPrimary.withAlpha(0.85f));
        g.drawText(p.label, (int)(p.x - 50.0f), (int)(p.y + 6.0f), 100, 10, juce::Justification::centred);
    }

#if JUCE_DEBUG
    // GPU frame-timing HUD (debug builds only) - surfaces avg/peak
    // renderOpenGL() cost so regressions under heavy analysis load (e.g.
    // Render History mode re-analyzing several queued renders at once) are
    // visible without attaching an external profiler.
    {
        juce::String timing = "GL " + juce::String(frameTimer_.getAverageFrameMs(), 1) + "ms avg / " +
                              juce::String(frameTimer_.getPeakFrameMs(), 1) + "ms peak (" +
                              juce::String(juce::roundToInt(frameTimer_.getAverageFps())) + " fps)";
        g.setFont(KhensuTheme::labelFont(8.0f));
        g.setColour(KhensuTheme::textTertiary.withAlpha(0.6f));
        g.drawText(timing, getLocalBounds().removeFromBottom(14).reduced(6, 0), juce::Justification::centredRight,
                   false);
    }
#endif
}

void SonicOrbitComponent::resized() { projectPoints(); }

} // namespace khensu
