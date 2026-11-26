// tests/ui/AnalysisRevealTest.cpp
// ENH-7: every finding has a card on the Analysis screen and a reveal widens
// the view only when it must; BUG-1: the genre the findings were judged
// against is named the same everywhere; BUG-4: one Top findings chip per kind
// of finding, with a count, cycling on click; the verdict's "Start here"
// links to its card.
#include <catch2/catch_test_macros.hpp>
#include "ui/Dashboard/FindingCardMap.h"
#include "ui/Dashboard/KhensuVerdictCard.h"
#include "ui/shared/GenreInUse.h"
#include "ui/shared/HeroSpectrumModel.h"
#include "ui/shared/HeroSpectrumView.h"
#include <khensu/recommendations/IssueCodes.h>

using namespace khensu;

TEST_CASE("Every engine finding code has a card on the Analysis screen (ENH-7)", "[enh7]")
{
    for (const auto& c : recommendations::kIssueCodes) {
        if (c.origin == recommendations::IssueOrigin::Legacy) continue;
        INFO(std::string(c.code));
        const auto card = finding_card::forFinding(c.code);
        CHECK_FALSE(card.empty());
        // A recommendation id (<code>_<file id>) finds the same card.
        CHECK(finding_card::forFinding(std::string(c.code) + "_f1") == card);
    }
    CHECK(finding_card::forFinding("translation_risk_high_f2") == "translation");
    CHECK(finding_card::forFinding("lufs_too_loud_f1") == "loudness");
    CHECK(finding_card::isCorePanel("loudness"));
    CHECK_FALSE(finding_card::isCorePanel("translation"));
    CHECK(finding_card::forFinding("not_a_finding").empty());
}

TEST_CASE("A reveal widens the view only when the card is not in it (ENH-7)", "[enh7]")
{
    const std::vector<std::string> essentials{"mix_notes", "translation", "tone"};
    // In Essentials already: stays.
    CHECK(finding_card::viewForReveal("Essentials", "translation", essentials, false) == "Essentials");
    // Not in Essentials: All.
    CHECK(finding_card::viewForReveal("Essentials", "stereo_mono", essentials, false) == "All");
    // A category view that holds it stays; one that does not goes to All.
    CHECK(finding_card::viewForReveal("Spatial", "stereo_mono", essentials, false) == "Spatial");
    CHECK(finding_card::viewForReveal("Loudness", "stereo_mono", essentials, false) == "All");
    // Song insights are never in All: their own view.
    CHECK(finding_card::viewForReveal("Essentials", "hooks", essentials, false) == "Song insights");
    // An advanced card: All when advanced cards are shown there, else Advanced.
    CHECK(finding_card::viewForReveal("All", "structural", essentials, false) == "Advanced");
    CHECK(finding_card::viewForReveal("All", "structural", essentials, true) == "All");
}

TEST_CASE("The genre in use is named the way the findings used it (BUG-1)", "[bug1]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(R"({"genres": [{"id": "glitch_hop", "name": "Glitch Hop"},
                                             {"id": "dubstep", "name": "Dubstep"}]})")
                .is_ok());
    const auto& all = db.listAll();
    REQUIRE(all.size() == 2);

    AnalysisResult r;
    r.is_partial = false;
    r.genre_blend = {{all[0].id, "", 0.7f}, {all[1].id, "", 0.3f}};

    // Nothing stated, a blend detected: the detected genre, said so.
    const auto detected = genre_in_use::resolve("", &r, db);
    CHECK(detected.source == genre_in_use::InUse::Source::Detected);
    CHECK(genre_in_use::label(detected).contains("detected, not confirmed"));
    CHECK(genre_in_use::label(detected).startsWith(juce::String(all[0].name.empty() ? all[0].id : all[0].name)));
    CHECK(genre_in_use::blendEntries("", r.genre_blend, db).size() == 2);
    CHECK(genre_in_use::detectedName("Glitch Hop") == "detected Glitch Hop");

    // The user's own pick wins, as is.
    const auto stated = genre_in_use::resolve(all[1].id, &r, db);
    CHECK(stated.source == genre_in_use::InUse::Source::Stated);
    CHECK_FALSE(genre_in_use::label(stated).contains("detected"));

    // No analysis, or a single detected genre: the generic target.
    CHECK(genre_in_use::resolve("", nullptr, db).source == genre_in_use::InUse::Source::Generic);
    r.genre_blend.resize(1);
    const auto generic = genre_in_use::resolve("", &r, db);
    CHECK(generic.source == genre_in_use::InUse::Source::Generic);
    CHECK(genre_in_use::label(generic).contains("generic"));
}

namespace {
IIssues::Issue issueFor(const std::string& code, const std::string& fileId, float severity)
{
    IIssues::Issue i;
    i.recId = code + "_" + fileId;
    i.code = code;
    i.rec.id = i.recId;
    i.rec.what = code;
    i.rec.severity = severity;
    return i;
}
} // namespace

TEST_CASE("Top findings: one chip per kind with a count, a click cycles (BUG-4)", "[bug4]")
{
    juce::ScopedJuceInitialiser_GUI init;
    HeroSpectrumView view;
    view.setSize(1100, 400);
    AnalysisResult r;
    r.sample_rate = 48000.0;
    r.spectral.rms_per_octave_db = std::vector<float>(10, -30.0f);
    const std::vector<IIssues::Issue> issues{issueFor("translation_risk_high", "a", 60.0f),
                                             issueFor("translation_risk_high", "b", 40.0f),
                                             issueFor("translation_risk_high", "c", 20.0f),
                                             issueFor("dc_offset", "a", 30.0f)};
    view.setModel(hero_spectrum::build(r, std::nullopt, issues, nullptr), {}, false);

    const auto chips = view.chipSummary();
    REQUIRE(chips.size() == 2);
    CHECK(chips[0].second == 3); // Translation x3, the worst kind first
    CHECK(chips[1].second == 1);
    CHECK(chips[0].first != chips[1].first);

    const auto row = view.findingsArea();
    const juce::Point<float> chip(row.getX() + 110.0f, row.getCentreY());
    CHECK(view.findingAt(chip) == "translation_risk_high_a"); // the worst first
    view.setSelected("translation_risk_high_a");
    CHECK(view.findingAt(chip) == "translation_risk_high_b");
    view.setSelected("translation_risk_high_c");
    CHECK(view.findingAt(chip) == "translation_risk_high_a"); // wraps
}

TEST_CASE("The verdict's Start here links to its card (ENH-7)", "[enh7]")
{
    juce::ScopedJuceInitialiser_GUI init;
    KhensuVerdictCard card;
    card.setSize(900, KhensuVerdictCard::kHeight);
    std::string revealed;
    card.onStartHereClicked = [&revealed](const std::string& id) { revealed = id; };
    MixScorecard sc;
    sc.overall_score = 70.0f;
    verdict::StartHere start;
    start.recId = "translation_risk_high_a";
    start.text = "Translation risk";
    card.setData(sc, {}, start);
    juce::Image img(juce::Image::ARGB, 900, KhensuVerdictCard::kHeight, true);
    juce::Graphics g(img);
    card.paintEntireComponent(g, false); // lays out the Start here row
    const auto area = card.startHereArea();
    REQUIRE_FALSE(area.isEmpty());
    const auto at = area.getCentre().toFloat();
    card.mouseUp(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, {}, 1.0f, 0.0f, 0.0f, 0.0f,
                                  0.0f, &card, &card, juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(), 1,
                                  false));
    CHECK(revealed == "translation_risk_high_a");
}
