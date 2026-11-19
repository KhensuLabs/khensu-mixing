// tests/ui/StereoFieldVizTest.cpp
// StereoFieldVizComponent itself now requires a live AppController (it became
// the 3D stem-sphere room), so the unit-testable surface of this feature is
// the Camera3D projection math it draws with.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "../../desktop-app/Source/ui/shared/Camera3D.h"

using namespace khensu;

TEST_CASE("Camera3D projects the origin to screen centre", "[stereofield][camera3d]")
{
    Camera3D cam;

    SECTION("at the default view angle")
    {
        auto pt = cam.project({0.0f, 0.0f, 0.0f}, 200.0f, 200.0f);
        REQUIRE(pt.x == Catch::Approx(100.0f).margin(0.5f));
        REQUIRE(pt.y == Catch::Approx(100.0f).margin(0.5f));
    }

    // Regression: the orbit-around-origin fix. With the old (rotate-after-
    // translate) view matrix the origin swung off-centre as soon as any
    // pitch/yaw was applied.
    SECTION("for any pitch/yaw (orbit-around-origin invariant)")
    {
        for (float pitch : {-1.0f, -0.3f, 0.0f, 0.5f, 1.2f}) {
            for (float yaw : {-2.0f, 0.0f, 0.7f, 3.0f}) {
                cam.pitch = pitch;
                cam.yaw = yaw;
                auto pt = cam.project({0.0f, 0.0f, 0.0f}, 300.0f, 200.0f);
                REQUIRE(pt.x == Catch::Approx(150.0f).margin(0.5f));
                REQUIRE(pt.y == Catch::Approx(100.0f).margin(0.5f));
            }
        }
    }
}

TEST_CASE("Camera3D projection preserves screen direction", "[stereofield][camera3d]")
{
    Camera3D cam;
    cam.pitch = 0.0f;
    cam.yaw = 0.0f;

    SECTION("+x maps right of centre, -x left")
    {
        REQUIRE(cam.project({1.0f, 0.0f, 0.0f}, 200.0f, 200.0f).x > 100.0f);
        REQUIRE(cam.project({-1.0f, 0.0f, 0.0f}, 200.0f, 200.0f).x < 100.0f);
    }
    SECTION("+y maps above centre (screen y grows downward)")
    {
        REQUIRE(cam.project({0.0f, 1.0f, 0.0f}, 200.0f, 200.0f).y < 100.0f);
        REQUIRE(cam.project({0.0f, -1.0f, 0.0f}, 200.0f, 200.0f).y > 100.0f);
    }
}

TEST_CASE("Camera3D clamps points behind the camera instead of mirroring", "[stereofield][camera3d]")
{
    Camera3D cam;
    // A point further along +z than the camera's orbit distance sits behind
    // the camera; pw would go negative and mirror both axes without the clamp.
    juce::Vector3D<float> behind{0.5f, 0.0f, cam.zoom + 5.0f};
    auto pt = cam.project(behind, 200.0f, 200.0f);
    // With the clamp, a +x point stays pushed off in the +x screen direction.
    REQUIRE(pt.x > 100.0f);
}
