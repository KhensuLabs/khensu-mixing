// desktop-app/Source/ui/shared/Camera3D.h
#pragma once
#include <JuceHeader.h>
#include <cmath>

namespace khensu {

struct Matrix4x4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static Matrix4x4 identity() { return Matrix4x4(); }

    static Matrix4x4 translation(float x, float y, float z)
    {
        Matrix4x4 mat;
        mat.m[12] = x;
        mat.m[13] = y;
        mat.m[14] = z;
        return mat;
    }

    static Matrix4x4 scale(float sx, float sy, float sz)
    {
        Matrix4x4 mat;
        mat.m[0] = sx;
        mat.m[5] = sy;
        mat.m[10] = sz;
        return mat;
    }

    static Matrix4x4 rotationX(float angle)
    {
        Matrix4x4 mat;
        float c = std::cos(angle);
        float s = std::sin(angle);
        mat.m[5] = c;
        mat.m[6] = s;
        mat.m[9] = -s;
        mat.m[10] = c;
        return mat;
    }

    static Matrix4x4 rotationY(float angle)
    {
        Matrix4x4 mat;
        float c = std::cos(angle);
        float s = std::sin(angle);
        mat.m[0] = c;
        mat.m[2] = -s;
        mat.m[8] = s;
        mat.m[10] = c;
        return mat;
    }

    static Matrix4x4 perspective(float fovDeg, float aspect, float nearClip, float farClip)
    {
        Matrix4x4 mat;
        float f = 1.0f / std::tan(fovDeg * 0.0174532925f * 0.5f);
        mat.m[0] = f / aspect;
        mat.m[5] = f;
        mat.m[10] = (farClip + nearClip) / (nearClip - farClip);
        mat.m[11] = -1.0f;
        mat.m[14] = (2.0f * farClip * nearClip) / (nearClip - farClip);
        mat.m[15] = 0.0f;
        return mat;
    }

    Matrix4x4 multipliedBy(const Matrix4x4& o) const
    {
        Matrix4x4 r;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += m[i + k * 4] * o.m[k + j * 4];
                }
                r.m[i + j * 4] = sum;
            }
        }
        return r;
    }
};

struct Camera3D {
    float pitch = 0.3f;
    float yaw = 0.0f;
    float zoom = 3.5f;
    float fov = 45.0f;
    /// STU-12: the point the camera orbits and looks at (default the origin,
    /// which every view used before; SonicOrbit's fly-to moves it).
    float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f;

    Matrix4x4 getViewMatrix() const
    {
        // Orbit-around-origin camera: rotate the point into camera-relative
        // orientation FIRST, then push it back along the camera's local Z by
        // zoom. The previous order (rotate-after-translate) rotated the
        // already-translated point around the *world* origin instead of the
        // camera's own axis, so the origin itself - meant to be the fixed
        // point every one of these views orbits around - swung away from
        // screen-centre by an amount proportional to zoom*sin(pitch)/sin(yaw)
        // as soon as any rotation was applied, pushing most of the scene
        // (StereoField's room, SonicOrbit's point cloud, etc.) off-screen at
        // anything but a dead-on pitch=0,yaw=0 view. With T applied last, a
        // point at the origin now stays at (0,0,-zoom) - screen-centre - for
        // any pitch/yaw.
        Matrix4x4 t = Matrix4x4::translation(0, 0, -zoom);
        Matrix4x4 rx = Matrix4x4::rotationX(pitch);
        Matrix4x4 ry = Matrix4x4::rotationY(yaw);
        Matrix4x4 view = t.multipliedBy(rx).multipliedBy(ry);
        if (targetX != 0.0f || targetY != 0.0f || targetZ != 0.0f)
            view = view.multipliedBy(Matrix4x4::translation(-targetX, -targetY, -targetZ));
        return view;
    }

    Matrix4x4 getProjectionMatrix(float aspectRatio) const
    {
        return Matrix4x4::perspective(fov, aspectRatio, 0.1f, 100.0f);
    }

    juce::Point<float> project(const juce::Vector3D<float>& p, float width, float height) const
    {
        Matrix4x4 view = getViewMatrix();
        Matrix4x4 proj = getProjectionMatrix(width / (height > 0 ? height : 1.0f));

        // Multiply point by view matrix
        float vx = p.x * view.m[0] + p.y * view.m[4] + p.z * view.m[8] + view.m[12];
        float vy = p.x * view.m[1] + p.y * view.m[5] + p.z * view.m[9] + view.m[13];
        float vz = p.x * view.m[2] + p.y * view.m[6] + p.z * view.m[10] + view.m[14];
        float vw = p.x * view.m[3] + p.y * view.m[7] + p.z * view.m[11] + view.m[15];

        if (std::abs(vw) < 0.0001f) vw = 1.0f;
        vx /= vw;
        vy /= vw;
        vz /= vw;

        // Multiply by projection matrix
        float px = vx * proj.m[0] + vy * proj.m[4] + vz * proj.m[8] + proj.m[12];
        float py = vx * proj.m[1] + vy * proj.m[5] + vz * proj.m[9] + proj.m[13];
        float pw = vx * proj.m[2] + vy * proj.m[6] + vz * proj.m[10] + proj.m[14];

        // pw is (roughly) the negated view-space depth. A point that swings
        // behind the camera - routine during SonicOrbitComponent's continuous
        // auto-rotation, or a mouse-drag pitch/yaw on the other 3D panels -
        // makes pw go negative. Dividing by a negative pw sign-flips both
        // axes, snapping the point to a wildly mirrored screen position far
        // outside the panel instead of just being off in the (correct)
        // direction. Clamping pw to a small positive floor keeps those
        // points pushed harmlessly off in one consistent direction rather
        // than flickering to a mirrored "out of bounds" position.
        constexpr float kMinPw = 0.05f;
        if (pw < kMinPw) pw = kMinPw;
        px /= pw;
        py /= pw;

        // Map to viewport coordinates
        float cx = width * 0.5f;
        float cy = height * 0.5f;
        return {cx + px * cx, cy - py * cy};
    }
};

class ShaderCompiler {
  public:
    static std::unique_ptr<juce::OpenGLShaderProgram>
    compile(juce::OpenGLContext& openGLContext, const juce::String& vertexShader, const juce::String& fragmentShader)
    {
        auto program = std::make_unique<juce::OpenGLShaderProgram>(openGLContext);
        if (program->addVertexShader(vertexShader) && program->addFragmentShader(fragmentShader)) {
            if (program->link()) {
                return program;
            }
        }
        return nullptr;
    }
};

} // namespace khensu
