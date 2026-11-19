// tests/ui/OnboardingPlanTest.cpp
// Onboarding: goal + genre + platform -> loudness targets and a starting
// chain whose parameters land on the built-in DSP chain's ids.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/OnboardingPlan.h"

using Catch::Approx;
using khensu::GenreEntry;
using khensu::OnboardingPlan;
using P = khensu::mixerchain::DspParamIds;

namespace {
GenreEntry genre(const char* id, float lufs, float lowWeight, float ratio, float tilt)
{
    GenreEntry g;
    g.id = id;
    g.name = id;
    g.lufs_target = lufs;
    g.true_peak_max = -1.0f;
    g.low_end_weight = lowWeight;
    g.compression_ratio_typical = ratio;
    g.spectral_tilt_target_db_per_octave = tilt;
    return g;
}
} // namespace

TEST_CASE("Platform sets loudness targets; club follows the genre", "[onboarding]")
{
    const auto trap = genre("trap", -7.0f, 0.9f, 4.0f, -5.0f);
    CHECK(OnboardingPlan::platformTargets(OnboardingPlan::Platform::Streaming, trap).first == Approx(-14.0f));
    CHECK(OnboardingPlan::platformTargets(OnboardingPlan::Platform::AppleMusic, trap).first == Approx(-16.0f));
    CHECK(OnboardingPlan::platformTargets(OnboardingPlan::Platform::Broadcast, trap).first == Approx(-23.0f));
    const auto club = OnboardingPlan::platformTargets(OnboardingPlan::Platform::Club, trap);
    CHECK(club.first == Approx(-7.0f));
    CHECK(club.second <= -0.3f);
}

TEST_CASE("Mastering plan ends in a limiter at the platform ceiling", "[onboarding]")
{
    const auto plan = OnboardingPlan::build(OnboardingPlan::Goal::MasterForRelease,
                                            genre("pop", -9.0f, 0.5f, 3.0f, -4.5f),
                                            OnboardingPlan::Platform::Streaming);
    REQUIRE(plan.chain.size() == 4);
    CHECK(plan.chain.back().name == "Limiter to target");
    const auto params = plan.combinedParams();
    CHECK(params.at(P::limOn) == 1.0f);
    CHECK(params.at(P::limThreshDb) == Approx(-1.0f));
    CHECK(params.at(P::compRatio) == Approx(3.0f));
    CHECK(plan.summary().contains("-14.0 LUFS"));
}

TEST_CASE("Bass-heavy genres keep a lower high-pass", "[onboarding]")
{
    const auto bassy = OnboardingPlan::build(OnboardingPlan::Goal::MixCheck, genre("trap", -8.0f, 1.0f, 4.0f, -5.0f),
                                             OnboardingPlan::Platform::Streaming);
    const auto lean = OnboardingPlan::build(OnboardingPlan::Goal::MixCheck, genre("folk", -14.0f, 0.0f, 2.0f, -4.0f),
                                            OnboardingPlan::Platform::Streaming);
    CHECK(bassy.combinedParams().at(P::hpfFreq) < lean.combinedParams().at(P::hpfFreq));
    CHECK(bassy.chain.front().params.empty()); // "Analyze first" is advice, not DSP
}

TEST_CASE("Every goal produces an explained chain", "[onboarding]")
{
    for (auto goal : {OnboardingPlan::Goal::MixCheck, OnboardingPlan::Goal::MasterForRelease,
                      OnboardingPlan::Goal::VocalPolish, OnboardingPlan::Goal::MatchReference}) {
        const auto plan = OnboardingPlan::build(goal, genre("rnb", -10.0f, 0.6f, 3.0f, -5.0f),
                                                OnboardingPlan::Platform::AppleMusic);
        REQUIRE_FALSE(plan.chain.empty());
        for (const auto& s : plan.chain) {
            CHECK(s.name.isNotEmpty());
            CHECK(s.why.isNotEmpty());
        }
        CHECK(OnboardingPlan::goalName(goal).isNotEmpty());
    }
    const auto vocal = OnboardingPlan::build(OnboardingPlan::Goal::VocalPolish, genre("rnb", -10.0f, 0.6f, 3.0f, -5.0f),
                                             OnboardingPlan::Platform::Streaming);
    CHECK(vocal.combinedParams().at(P::hpfFreq) == Approx(90.0f));
}

#include "ui/shared/OnboardingWizard.h"
#include <khensu/targets/GenreDatabase.h>

TEST_CASE("Onboarding wizard steps goal -> target -> chain and finishes", "[onboarding][wizard]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::GenreDatabase db;
    REQUIRE(db.loadFromString(R"({"genres": [
        {"id": "pop", "name": "Pop", "lufs_target": -9.0, "true_peak_max": -1.0},
        {"id": "techno", "name": "Techno", "lufs_target": -7.0, "true_peak_max": -0.5}]})")
                .is_ok());
    khensu::OnboardingWizard wizard(db);
    wizard.setBounds(0, 0, 900, 640);

    bool finished = false, pushed = false;
    OnboardingPlan result;
    wizard.onFinish = [&](const OnboardingPlan& plan, bool push) {
        finished = true;
        pushed = push;
        result = plan;
    };

    CHECK(wizard.currentStep() == 0);
    wizard.chooseGoal(OnboardingPlan::Goal::MasterForRelease);
    wizard.next();
    CHECK(wizard.currentStep() == 1);
    wizard.chooseGenreIndex(1);
    wizard.choosePlatform(OnboardingPlan::Platform::Club);
    wizard.next();
    CHECK(wizard.currentStep() == 2);

    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu-ui-snapshots");
    dir.createDirectory();
    const auto image = wizard.createComponentSnapshot(wizard.getLocalBounds());
    juce::PNGImageFormat png;
    auto file = dir.getChildFile("onboarding-step3.png");
    file.deleteFile();
    if (auto out = file.createOutputStream()) png.writeImageToStream(image, *out);

    wizard.back();
    CHECK(wizard.currentStep() == 1);
    wizard.next();
    wizard.next(); // Finish
    REQUIRE(finished);
    CHECK(pushed);
    CHECK(result.goal == OnboardingPlan::Goal::MasterForRelease);
    CHECK(result.genreId == "techno");
    CHECK(result.targetLufs == Approx(-7.0f));
}
