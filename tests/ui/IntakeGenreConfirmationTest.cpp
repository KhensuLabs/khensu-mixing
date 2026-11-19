// tests/ui/IntakeGenreConfirmationTest.cpp
// UX-12 / P0-1: a genre the analysis filled into the Target panel is not a
// genre the user stated. Only the user's own pick (the genre box, a
// quick-start template, an onboarding preset) counts; a detection has to be confirmed.
#include <catch2/catch_test_macros.hpp>
#include "ui/SessionIntake/GenreEvidenceText.h"
#include "ui/SessionIntake/TargetSetupPanel.h"
#include "ui/SessionIntake/TargetOverrideEditor.h"
#include "support/FakeAppController.h"
#include <khensu/targets/GenreConfirmation.h>
#include <khensu/targets/GenreDatabase.h>

namespace {
khensu::GenreDatabase twoGenres()
{
    khensu::GenreDatabase db;
    REQUIRE(db.loadFromString(R"({"genres": [
        {"id": "pop", "name": "Pop", "lufs_target": -9.0, "true_peak_max": -1.0},
        {"id": "techno", "name": "Techno", "lufs_target": -7.0, "true_peak_max": -0.5}]})")
                .is_ok());
    return db;
}
} // namespace

TEST_CASE("Target panel: a detected genre is not a stated one (UX-12)", "[intake][genre]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto db = twoGenres();
    khensu::TargetSetupPanel panel(db);
    CHECK_FALSE(panel.genreStatedByUser());

    // The first analysis fills the panel in.
    panel.autoPopulateFromAnalysis("techno", {}, {}, 60.0f, 128.0f, "A minor");
    CHECK(panel.buildTarget().primary_genre == "techno");
    CHECK_FALSE(panel.genreStatedByUser());

    // So in "detected" mode, unconfirmed, nothing confirms the genre...
    khensu::genre_confirmation::IntakeChoice choice;
    choice.selectionMode = "detected";
    choice.statedGenreId = panel.genreStatedByUser() ? panel.buildTarget().primary_genre : std::string();
    CHECK(khensu::genre_confirmation::sourceForIntake(choice).empty());
    // ...until the user confirms it.
    choice.confirmDetected = true;
    CHECK(khensu::genre_confirmation::sourceForIntake(choice) == khensu::genre_confirmation::kConfirmedCandidate);

    // Picking the genre yourself (the onboarding preset goes this way, as
    // does the genre box) states it.
    panel.autoSelectGenre("pop", {});
    CHECK(panel.buildTarget().primary_genre == "pop");
    CHECK(panel.genreStatedByUser());
}

TEST_CASE("Intake leaves the intent targets at the genre norm (TGT-1)", "[intake][targets]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto db = twoGenres();
    khensu::TargetSetupPanel panel(db);

    // An analysis fills in what it measured, never what the user wants.
    panel.autoPopulateFromAnalysis({}, {}, "dark", 72.0f, 140.0f, "F minor");
    auto t = panel.buildTarget();
    CHECK(t.energy == 72.0f);
    CHECK(t.hook_emphasis == 50.0f);
    CHECK(t.chorus_payoff == 50.0f);
    CHECK(t.heaviness_target == 50.0f);
    CHECK(t.memorability_target == 50.0f);

    // The user's own choice sets them.
    panel.setIntentTargets(65.0f, 80.0f, 55.0f, 70.0f);
    t = panel.buildTarget();
    CHECK(t.hook_emphasis == 65.0f);
    CHECK(t.chorus_payoff == 80.0f);
    CHECK(t.heaviness_target == 55.0f);
    CHECK(t.memorability_target == 70.0f);
}

TEST_CASE("Intake offers the genre probe's pick as a suggestion (NEW-9)", "[intake][genre]")
{
    juce::ScopedJuceInitialiser_GUI init;
    auto candidate = [](const char* id) {
        auto* o = new juce::DynamicObject();
        o->setProperty("genre", id);
        return juce::var(o);
    };
    juce::Array<juce::var> candidates{candidate("grunge"), candidate("alt-rock"), candidate("emo-rock")};
    auto* acc = new juce::DynamicObject();
    acc->setProperty("family_top1", 0.62);
    auto* ctx = new juce::DynamicObject();
    ctx->setProperty("candidates", candidates);
    ctx->setProperty("family", "rock");
    ctx->setProperty("family_probability", 0.55);
    ctx->setProperty("classifier_accuracy", juce::var(acc));
    const juce::var context(ctx); // owns ctx from here on

    const auto s = khensu::intake_text::genreSuggestion(context);
    CHECK(s.genreId == "grunge");
    CHECK(s.text.contains("Suggested genre: Rock family (55%), closest Grunge"));
    CHECK(s.text.contains("Also possible: Alt Rock, Emo Rock"));
    CHECK(s.text.contains("about 6 times in 10"));

    // The hand-tuned ranker (no family) is called what it is.
    ctx->removeProperty("family");
    CHECK(khensu::intake_text::genreSuggestion(context).text.startsWith("Rough genre guess: Grunge"));
    CHECK(khensu::intake_text::genreSuggestion(juce::var()).genreId.isEmpty());
}

TEST_CASE("Confirm fills the suggested genre without stating it (NEW-9)", "[intake][genre]")
{
    juce::ScopedJuceInitialiser_GUI init;
    const auto db = twoGenres();
    khensu::TargetSetupPanel panel(db);
    panel.fillDetectedGenre("techno");
    CHECK(panel.buildTarget().primary_genre == "techno");
    CHECK_FALSE(panel.genreStatedByUser()); // confirmation, not a statement
}

TEST_CASE("Grouped subgenres and the user's examples show in the suggestion (GEN-1)", "[intake][genre]")
{
    auto* ctx = new juce::DynamicObject();
    const juce::var context(ctx);
    juce::Array<juce::var> candidates;
    auto* top = new juce::DynamicObject();
    top->setProperty("genre", "city-pop");
    top->setProperty("group", juce::Array<juce::var>{"city-pop", "j-pop"});
    candidates.add(juce::var(top));
    auto* next = new juce::DynamicObject();
    next->setProperty("genre", "indie-rock");
    candidates.add(juce::var(next));
    ctx->setProperty("candidates", candidates);
    ctx->setProperty("family", "pop");
    ctx->setProperty("family_probability", 0.5);
    ctx->setProperty("personal_examples", 2);

    const auto s = khensu::intake_text::genreSuggestion(context);
    CHECK(s.genreId == "city-pop");
    CHECK(s.text.contains("closest City Pop or J Pop"));
    CHECK(s.text.contains("Also possible: Indie Rock"));
    CHECK(s.text.contains("Tuned by 2 similar tracks you labelled."));
    CHECK(khensu::intake_text::candidateName(candidates[1]) == "Indie Rock");
}

TEST_CASE("Only a genre the user confirmed or chose teaches the probe (GEN-1e)", "[intake][genre]")
{
    namespace gc = khensu::genre_confirmation;
    khensu::CreativeTarget t;
    t.primary_genre = "grunge";
    t.genre_confirmed_source = gc::kConfirmedCandidate;
    CHECK(khensu::intake_text::genreExampleSource(t, "grunge") == "confirmed");
    CHECK(khensu::intake_text::genreExampleSource(t, "city-pop") == "chosen");
    t.genre_confirmed_source = gc::kUserStated;
    CHECK(khensu::intake_text::genreExampleSource(t, "") == "chosen");
    t.genre_confirmed_source = gc::kReferenceTrack;
    CHECK(khensu::intake_text::genreExampleSource(t, "grunge").isEmpty());
    t.genre_confirmed_source.clear(); // detected, never confirmed
    CHECK(khensu::intake_text::genreExampleSource(t, "grunge").isEmpty());
    t.genre_confirmed_source = gc::kUserStated;
    t.primary_genre.clear();
    CHECK(khensu::intake_text::genreExampleSource(t, "grunge").isEmpty());
}

TEST_CASE("Target overrides save once the edits settle, not on every drag value", "[intake][targets]")
{
    juce::ScopedJuceInitialiser_GUI init;
    khensu::test::FakeAppController controller;
    khensu::GenreEntry genre;
    genre.id = "pop";
    khensu::TargetOverrideEditor editor(controller, genre);
    editor.setSize(800, 600);
    for (int i = 1; i <= 10; ++i)
        editor.setRowValueForTesting(0, 0.2 * i); // a drag: ten values
    CHECK(editor.savesForTesting() == 0);      // nothing written mid-drag
    juce::MessageManager::getInstance()->runDispatchLoopUntil(800);
    CHECK(editor.savesForTesting() == 1); // one write once it settled
}
