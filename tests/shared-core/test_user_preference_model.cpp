#include <catch2/catch_test_macros.hpp>
#include <khensu/targets/UserPreferenceModel.h>
#include <khensu/feedback/FeedbackManager.h>
#include <khensu/targets/GenreEntry.h>

using namespace khensu;

TEST_CASE("UserPreferenceModel load save adapt and learn", "[preference]")
{
    FeedbackManager fm(":memory:");
    UserPreferenceModel model;

    // Default offsets are 0.0f
    REQUIRE(model.getLufsOffset() == 0.0f);
    REQUIRE(model.getLraOffset() == 0.0f);
    REQUIRE(model.getStereoWidthOffset() == 0.0f);
    REQUIRE(model.getLowEndWeightOffset() == 0.0f);

    // Adapt should not modify targets when offsets are 0
    GenreEntry genre;
    genre.lufs_target = -14.0f;
    genre.lufs_range_min = -16.0f;
    genre.lufs_range_max = -11.0f;
    genre.lra_range_min = 5.0f;
    genre.lra_range_max = 10.0f;
    genre.stereo_width_min = 0.4f;
    genre.stereo_width_max = 0.8f;
    genre.low_end_weight = 0.5f;

    model.adapt(genre);
    REQUIRE(genre.lufs_target == -14.0f);
    REQUIRE(genre.lra_range_min == 5.0f);
    REQUIRE(genre.stereo_width_min == 0.4f);
    REQUIRE(genre.low_end_weight == 0.5f);

    // Test learning from action
    // rejected lufs_too_loud -> user wants it louder -> positive LUFS offset
    bool changed = model.learnFromAction("lufs_too_loud_123", false);
    REQUIRE(changed);
    REQUIRE(model.getLufsOffset() > 0.0f);

    // Save and load from database
    model.saveToDatabase(fm);

    UserPreferenceModel loadedModel;
    loadedModel.loadFromDatabase(fm);
    REQUIRE(loadedModel.getLufsOffset() == model.getLufsOffset());

    // Adapt with the positive offset
    GenreEntry genre2;
    genre2.lufs_target = -14.0f;
    loadedModel.adapt(genre2);
    REQUIRE(genre2.lufs_target > -14.0f); // target should be pushed louder (closer to 0)
}
