// tests/ui/IntakeTextTest.cpp
// Intake fixes ported from the retired worktree-agent-a8cf branch: labelled
// genre evidence, readable genre names, and the playback intent stored as
// its data id rather than the combo's (translatable) label.
#include <catch2/catch_test_macros.hpp>

#include "ui/SessionIntake/GenreEvidenceText.h"
#include "ui/SessionIntake/TargetSetupPanel.h"
#include <khensu/recommendations/PlaybackIntentTargets.h>

namespace it = khensu::intake_text;

TEST_CASE("Intake genre evidence is labelled with units, not raw key=value", "[intake]")
{
    juce::DynamicObject::Ptr ev = new juce::DynamicObject();
    ev->setProperty("bpm", 127.6);
    ev->setProperty("spectral_centroid_hz", 2345.0);
    ev->setProperty("stereo_width", 0.4213);
    ev->setProperty("spectral_rolloff_hz", juce::var()); // null: skipped
    const auto text = it::formatGenreEvidence(juce::var(ev.get()));
    CHECK(text == "Tempo 128 BPM, Brightness 2.3 kHz, Stereo width 0.42");
    CHECK_FALSE(text.contains("_"));
    CHECK_FALSE(text.contains("="));
}

TEST_CASE("Intake shows genre ids as words", "[intake]")
{
    CHECK(it::humanizeIdentifier("hip_hop") == "Hip Hop");
    CHECK(it::humanizeIdentifier("dungeon-synth") == "Dungeon Synth");
    CHECK(it::humanizeIdentifier("") == "");
}

TEST_CASE("Intake stores the playback intent as its data id", "[intake][intent]")
{
    using khensu::TargetSetupPanel;
    CHECK(TargetSetupPanel::playbackIntentIdForComboId(1) == "streaming");
    CHECK(TargetSetupPanel::playbackIntentIdForComboId(2) == "club");
    CHECK(TargetSetupPanel::playbackIntentIdForComboId(3) == "broadcast");
    CHECK(TargetSetupPanel::playbackIntentIdForComboId(4) == "headphone");
    CHECK(TargetSetupPanel::playbackIntentIdForComboId(5) == "reference");
    CHECK(TargetSetupPanel::playbackIntentIdForComboId(0).empty());
    CHECK(TargetSetupPanel::playbackIntentIdForComboId(6).empty());
    // Every id the combo can store is one the loudness mapping understands.
    for (int id = 2; id <= 4; ++id)
        CHECK(khensu::loudnessOverrideForIntent(TargetSetupPanel::playbackIntentIdForComboId(id)).has_value());
}
