// desktop-app/Source/ui/SessionIntake/GenreEvidenceText.h
// Intake's genre-context text: labelled evidence with units, and readable
// genre names, instead of raw snake_case key=value floats. Ported from the
// retired worktree-agent-a8cf branch onto main's generated field names.
#pragma once

#include <array>

#include <JuceHeader.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/targets/GenreConfirmation.h>

namespace khensu::intake_text {

/// "hip_hop" -> "Hip Hop".
inline juce::String humanizeIdentifier(const juce::String& id)
{
    juce::StringArray words;
    words.addTokens(id.replaceCharacter('_', ' ').replaceCharacter('-', ' ').trim(), " ", "");
    words.removeEmptyStrings();
    for (auto& w : words)
        w = w.substring(0, 1).toUpperCase() + w.substring(1);
    return words.joinIntoString(" ");
}

/// The musical_context "evidence" object as one line: "Tempo 128 BPM,
/// Brightness 2.3 kHz, Stereo width 0.42". Missing keys are skipped.
inline juce::String formatGenreEvidence(const juce::var& evidence)
{
    namespace E = api::fields::GenreContextEvidence;
    struct Field {
        const char* key;
        const char* label; // NEEDS_TRANS
        const char* unit;
        int decimals;
        double scale;
    };
    static const std::array<Field, 9> kFields{{
        {E::bpm, NEEDS_TRANS("Tempo"), " BPM", 0, 1.0},
        {E::onset_density_per_sec, NEEDS_TRANS("Onsets"), "/s", 1, 1.0},
        {E::rhythm_peak_strength, NEEDS_TRANS("Rhythm strength"), "", 2, 1.0},
        {E::key_stability, NEEDS_TRANS("Key stability"), "", 2, 1.0},
        {E::spectral_centroid_hz, NEEDS_TRANS("Brightness"), " kHz", 1, 0.001},
        {E::spectral_rolloff_hz, NEEDS_TRANS("Roll-off"), " kHz", 1, 0.001},
        {E::chroma_change_rate, NEEDS_TRANS("Harmonic change"), "/s", 2, 1.0},
        {E::stereo_width, NEEDS_TRANS("Stereo width"), "", 2, 1.0},
        {E::low_band_correlation, NEEDS_TRANS("Low-end correlation"), "", 2, 1.0},
    }};
    juce::StringArray parts;
    for (const auto& f : kFields) {
        const auto v = evidence[f.key];
        if (!(v.isDouble() || v.isInt() || v.isInt64())) continue;
        const double x = static_cast<double>(v) * f.scale;
        // String(double, 0) means "as many places as needed": round explicitly.
        const auto num = f.decimals > 0 ? juce::String(x, f.decimals) : juce::String(juce::roundToInt(x));
        parts.add(juce::translate(f.label) + " " + num + f.unit);
    }
    return parts.joinIntoString(", ");
}

/// A candidate's display name: "City Pop", or for subgenres the probe
/// cannot tell apart (GEN-1a) "City Pop or J Pop" (at most three named).
inline juce::String candidateName(const juce::var& candidate)
{
    namespace C = api::fields::GenreCandidate;
    const auto group = candidate[C::group];
    if (!group.isArray() || group.size() < 2) return humanizeIdentifier(candidate[C::genre].toString());
    juce::StringArray names;
    for (int i = 0; i < juce::jmin(3, group.size()); ++i)
        names.add(humanizeIdentifier(group[i].toString()));
    names.removeEmptyStrings();
    const auto last = names[names.size() - 1];
    names.remove(names.size() - 1);
    return names.isEmpty() ? last : names.joinIntoString(", ") + " " + TRANS("or") + " " + last;
}

/// What Intake offers to confirm: the top candidate's catalog id and the
/// line that presents it as a suggestion, never as a known genre (NEW-9).
struct GenreSuggestion {
    juce::String genreId; ///< empty when the context has no candidate
    juce::String text;
};

/// The suggestion from a musical_context object. A CLAP-probe context names
/// the family with its probability and how often the probe gets the family
/// right; the hand-tuned ranker's top pick is called a rough guess.
inline GenreSuggestion genreSuggestion(const juce::var& context)
{
    namespace G = api::fields::GenreContextResponse;
    namespace C = api::fields::GenreCandidate;
    namespace A = api::fields::GenreClassifierAccuracy;
    GenreSuggestion s;
    const auto candidates = context[G::candidates];
    if (!candidates.isArray() || candidates.size() == 0) return s;
    s.genreId = candidates[0][C::genre].toString();
    if (s.genreId.isEmpty()) return s;
    const auto name = candidateName(candidates[0]);
    const auto family = context[G::family].toString();
    if (family.isEmpty()) {
        s.text = TRANS("Rough genre guess:") + " " + name + " " + TRANS("(not a trained classifier)");
        return s;
    }
    // Lead with the family: the probe's genre-level probabilities are flat
    // (a top genre at ~5% is usual), its family call is the useful part.
    s.text = TRANS("Suggested genre:") + " " + humanizeIdentifier(family) + " " + TRANS("family") + " (" +
             juce::String(juce::roundToInt(100.0 * double(context[G::family_probability]))) + "%), " +
             TRANS("closest") + " " + name;
    juce::StringArray also;
    for (int i = 1; i < juce::jmin(4, candidates.size()); ++i)
        also.add(candidateName(candidates[i]));
    also.removeEmptyStrings();
    if (!also.isEmpty()) s.text += ". " + TRANS("Also possible:") + " " + also.joinIntoString(", ");
    const auto acc = context[G::classifier_accuracy][A::family_top1];
    if (acc.isDouble() || acc.isInt())
        s.text += ". " + TRANS("The family is right about") + " " +
                  juce::String(juce::roundToInt(10.0 * double(acc))) + " " + TRANS("times in 10.");
    const int personal = int(context[G::personal_examples]);
    if (personal > 0)
        s.text += " " + TRANS("Tuned by") + " " + juce::String(personal) + " " +
                  (personal == 1 ? TRANS("similar track you labelled.") : TRANS("similar tracks you labelled."));
    return s;
}

/// What a new session tells the genre probe about its track (GEN-1e):
/// "confirmed" when the user kept the suggestion, "chosen" when they named
/// another genre, "" when the genre is not the user's (unconfirmed, from a
/// reference track, or empty).
inline juce::String genreExampleSource(const CreativeTarget& target, const juce::String& suggestedGenreId)
{
    namespace gc = genre_confirmation;
    if (target.primary_genre.empty()) return {};
    const auto& src = target.genre_confirmed_source;
    if (src != gc::kUserStated && src != gc::kConfirmedCandidate) return {};
    return juce::String(target.primary_genre) == suggestedGenreId ? "confirmed" : "chosen";
}

} // namespace khensu::intake_text
