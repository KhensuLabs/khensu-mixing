"""Song-level narratives: hooks, arc, structure, heaviness for the recommendation text factory (ENG-6, from text_factory.py).

Each entry maps a field name to a callable over the finding context; see
text_factory.TEMPLATES for the contract."""
from __future__ import annotations

from collections.abc import Callable

TEMPLATES: dict[str, dict[str, Callable[[dict], str | list[str]]]] = {
    "no_hooks_detected": {
        "what": lambda f: "No hook segments detected in the audio.",
        "why_helps": lambda f: (
            "Hooks are the primary retention mechanism for listener replay. "
            "Tracks without identifiable hooks typically have lower stream-to-listener ratios."
        ),
        "suggested_action": lambda f: (
            "Introduce a recurring melodic, rhythmic, or textural motif. "
            "Repetition with variation across sections builds hook identity."
        ),
        "engineer_note": lambda f: (
            "The hook detector found no recurring segments above the strength threshold. "
            "This can mean the arrangement genuinely lacks hooks (common in ambient/experimental) "
            "or that the hooks are too subtle for the detector."
        ),
        "severity_context": lambda f: "Fixed severity 85/100 — high impact on replay likelihood for hook-driven genres.",
        "confidence_reason": lambda f: "Hook detection uses pattern analysis; 70% confidence for deep-analysis.",
        "most_likely_cause": lambda f: (
            "Arrangement lacks a clear recurring motif, or hook segments vary too much across repeats."
        ),
        "trade_off": lambda f: "Adding explicit hooks may reduce artistic complexity or experimental quality.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} at {f['bpm']:.0f} BPM expects identifiable hook moments for listener retention."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Most {f['genre_name']} tracks have 1–3 distinct hook segments per track."
        ),
        "alternative_strategies": lambda f: [
            "Build a short 2–4 bar melodic loop as a pre-chorus hook.",
            "Use a signature drum break or instrumental fill as a structural hook.",
            "Layer a distinctive synth or vocal motif that recurs across sections.",
        ],
        "how_to_verify": lambda f: (
            "Listen to the track and identify any recurring melodic or rhythmic segments. "
            "A hook should feel intentionally memorable and distinct from surrounding material. "
            "If no clear hook emerges after 2–3 listens, the detection is likely accurate."
        ),
    },
    "hook_density_low": {
        "what": lambda f: (
            f"Hook density is {f['measured_value']:.2f}, "
            f"below the target threshold of {f['norm_value']:.2f} for this setting."
        ),
        "why_helps": lambda f: (
            "Higher hook density correlates with stronger listener retention and is a "
            "key driver of replay behavior in streaming data analysis."
        ),
        "suggested_action": lambda f: (
            "Add repetition of your strongest melodic or rhythmic motif. "
            "Consider calling back the hook in bridge and outro sections."
        ),
        "engineer_note": lambda f: (
            f"Hook density at {f['measured_value']:.2f} is below threshold ({f['norm_value']:.2f}). "
            "Consider whether hooks are present but too varied between occurrences — slight "
            "variation is good but fundamental identity should be consistent across repeats."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Hook analysis uses deep ML model; 70% confidence.",
        "most_likely_cause": lambda f: "Hook motifs vary too much between repetitions, or too few hook segments overall.",
        "trade_off": lambda f: "Increasing repetition may reduce perceived freshness if overused.",
        "bpm_mood_genre_relevance": lambda f: (
            f"At {f['bpm']:.0f} BPM in {f['genre_name']}, listeners expect higher hook frequency."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Top {f['genre_name']} tracks score hook density around {f['norm_value']:.2f}."
        ),
        "alternative_strategies": lambda f: [
            "Reprise the intro hook in the outro for a full-circle structural hook.",
            "Add a 4-bar hook instrument loop before drop sections.",
        ],
        "how_to_verify": lambda f: (
            "Count memorable moments in the first 90 seconds. For most commercial genres, "
            "there should be at least one strong hook event before the 45-second mark. "
            "This is a creative guideline, not a hard technical requirement."
        ),
    },
    "hook_strength_low": {
        "what": lambda f: (
            f"Hook strength is {f['measured_value']:.2f}, "
            f"below the target threshold of {f['norm_value']:.2f}."
        ),
        "why_helps": lambda f: (
            "Strong hooks — with melodic salience, rhythmic clarity, and tonal distinction "
            "— are more likely to be consciously recalled by listeners after the track ends."
        ),
        "suggested_action": lambda f: (
            "Strengthen the hook melody with clearer tonal movement. "
            "Increase hook contrast vs. verse — louder, brighter, or rhythmically more pronounced."
        ),
        "engineer_note": lambda f: (
            f"Hook strength at {f['measured_value']:.2f} — below threshold {f['norm_value']:.2f}. "
            "Work on the arrangement: raise the hook section relative to verses by 2–3 dB "
            "and ensure the melody has strong tonal identity."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Hook strength uses ensemble detection; 70% confidence.",
        "most_likely_cause": lambda f: "Hook melody lacks spectral or energy contrast against surrounding sections.",
        "trade_off": lambda f: "Increasing hook prominence may reduce the impact of the drop if overdone.",
        "bpm_mood_genre_relevance": lambda f: (
            f"In {f['genre_name']} at {f['bpm']:.0f} BPM, hooks need to cut through a dense arrangement."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Top {f['genre_name']} hooks score above {f['norm_value']:.2f} in strength analysis."
        ),
        "alternative_strategies": lambda f: [
            "Automate hook section up by 2 dB for energy contrast.",
            "Double the lead melody to increase spectral thickness during hook.",
            "Add a distinctive counter-melody to reinforce the hook identity.",
        ],
        "how_to_verify": lambda f: (
            "Play the track to someone unfamiliar with it. Ask them to hum or recall the main "
            "melody 30 minutes later. If they cannot, the hook may not be strong enough. "
            "This is ultimately a creative decision — use your judgment."
        ),
    },
    "memorability_low": {
        "what": lambda f: (
            f"Memorability score is {f['measured_value']:.2f}, "
            f"below the target threshold of {f['norm_value']:.2f}."
        ),
        "why_helps": lambda f: (
            "Memorability predicts how likely listeners are to recall and seek out a track "
            "after first exposure — a core driver of organic playlist growth."
        ),
        "suggested_action": lambda f: (
            "Focus on a single, strong melodic or rhythmic signature. "
            "Repetition with subtle variation across the track strengthens memorability."
        ),
        "engineer_note": lambda f: (
            f"Memorability at {f['measured_value']:.2f} is below the target of {f['norm_value']:.2f}. "
            "The most impactful improvements come from composition: a melody with clear tonal "
            "identity, rhythmic consistency, and emotional resonance."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Memorability uses deep-analysis model; 70% confidence.",
        "most_likely_cause": lambda f: "Lack of recurring melodic or rhythmic motif; overly varied arrangement.",
        "trade_off": lambda f: "Simplifying for memorability may reduce compositional complexity.",
        "bpm_mood_genre_relevance": lambda f: (
            f"At {f['bpm']:.0f} BPM in {f['genre_name']}, memorability depends on emotional peak placement."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"High-performing {f['genre_name']} tracks score above {f['norm_value']:.2f} in memorability."
        ),
        "alternative_strategies": lambda f: [
            "Identify the most melodically interesting moment and repeat it 3× across the track.",
            "Add a rhythmic pattern that acts as a mnemonic anchor.",
        ],
        "how_to_verify": lambda f: (
            "Play the track, then play something else, then come back to it. "
            "Does any element stick in your mind? Memorability is highly subjective — "
            "treat this as a prompt for creative revision, not a technical fix."
        ),
    },
    "earworm_likelihood_low": {
        "what": lambda f: (
            f"Earworm likelihood is {f['measured_value']:.2f}, "
            f"below the target threshold of {f['norm_value']:.2f}."
        ),
        "why_helps": lambda f: (
            "High earworm likelihood predicts involuntary recall — the phenomenon where "
            "a melody gets stuck in the listener's head, driving organic sharing and return listens."
        ),
        "suggested_action": lambda f: (
            "Create a concise 4–8 note melodic hook with a slight rhythmic syncopation. "
            "Unexpected intervals or a gap-fill pattern increase earworm potential."
        ),
        "engineer_note": lambda f: (
            f"Earworm likelihood at {f['measured_value']:.2f} is below {f['norm_value']:.2f}. "
            "Earworm-prone melodies share features: syncopated rhythm, ascending then descending "
            "phrase shape, 4–7 notes per bar, and moderate tempo."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Earworm model is statistical; 70% confidence.",
        "most_likely_cause": lambda f: "Main melody lacks syncopation, predictable arc, or note salience.",
        "trade_off": lambda f: "Earworm-optimized melodies may sacrifice harmonic complexity.",
        "bpm_mood_genre_relevance": lambda f: (
            f"At {f['bpm']:.0f} BPM, earworm potential is highest when the melody's "
            "rhythmic feel aligns with track tempo."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Chart-topping {f['genre_name']} tracks often have earworm scores above {f['norm_value']:.2f}."
        ),
        "alternative_strategies": lambda f: [
            "Introduce a 2-bar gap-fill pattern — melodic call and response.",
            "Use a note just outside the key for a brief unexpected interval.",
            "Layer a simple counter-melody to reinforce the main hook's identity.",
        ],
        "how_to_verify": lambda f: (
            "After listening, wait 30 minutes and try to recall the main melody without hearing it. "
            "Can you hum it from memory? Earworm potential is highest when the melody is hard to forget. "
            "A strong syncopated hook is a good sign."
        ),
    },
    "arc_quality_low": {
        "what": lambda f: (
            f"Structural arc quality is {f['measured_value']:.2f}, "
            "below the 0.50 threshold, indicating a weak energy/tension arc."
        ),
        "why_helps": lambda f: (
            "A strong structural arc — with defined build, peak, and release — keeps "
            "listeners engaged across the full duration and makes transitions feel intentional."
        ),
        "suggested_action": lambda f: (
            "Map energy level across sections. Ensure chorus/drop is clearly louder and "
            "denser than verse. Use automation to build tension into transitions."
        ),
        "engineer_note": lambda f: (
            f"Arc quality at {f['measured_value']:.2f} suggests the energy profile lacks "
            "clear structural direction. Arrangement fixes — not mixing — are usually the "
            "right lever here."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Arc analysis uses energy contour model; 70% confidence.",
        "most_likely_cause": lambda f: "Flat energy across sections; no clear peak or build structure.",
        "trade_off": lambda f: "Imposing a strong arc may require significant arrangement changes.",
        "bpm_mood_genre_relevance": lambda f: (
            f"At {f['bpm']:.0f} BPM, a {f.get('mood', 'neutral')} {f['genre_name']} track "
            "benefits from a clear tension-release arc for emotional engagement."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Well-structured {f['genre_name']} tracks score above 0.65 in arc quality."
        ),
        "alternative_strategies": lambda f: [
            "Add a pre-drop tension build using filter automation or riser elements.",
            "Strip elements from verse sections to increase contrast at the chorus.",
            "Use a bridge section with a key or tempo shift to create contrast.",
        ],
        "how_to_verify": lambda f: (
            "Listen to the track as a whole and note where your attention flags. "
            "A well-flowing track should build tension and release it at least once before the midpoint. "
            "This is a creative judgment call."
        ),
    },
    "heaviness_extremity_mismatch": {
        "what": lambda f: (
            f"Heaviness extremity is {f['measured_value']:.2f}; "
            f"target threshold is {f['norm_value']:.2f} (±0.20 tolerance)."
        ),
        "why_helps": lambda f: (
            "Heaviness extremity reflects the overall intensity of the production. "
            "Misalignment with genre and target expectations makes the track feel out of place."
        ),
        "suggested_action": lambda f: (
            "Adjust distortion, compression saturation, and rhythmic density to align with the "
            "intended heaviness target."
            if f["measured_value"] < f["norm_value"]
            else "Reduce distortion, saturators, or parallel compression to bring heaviness in range."
        ),
        "engineer_note": lambda f: (
            f"Heaviness extremity at {f['measured_value']:.2f} vs target {f['norm_value']:.2f}. "
            "Extremity is a holistic score from distortion, weight, and energy density — "
            "typically an arrangement/production decision rather than a mixing fix."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Heaviness uses deep ML analysis; 70% confidence.",
        "most_likely_cause": lambda f: "Production intensity doesn't align with genre or creative target.",
        "trade_off": lambda f: "Changing heaviness extremity fundamentally alters the character of the track.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} at {f['bpm']:.0f} BPM expects extremity around {f['norm_value']:.2f}."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"{f['genre_name']} references cluster around extremity {f['norm_value']:.2f}."
        ),
        "alternative_strategies": lambda f: [
            "Adjust guitar/synth distortion amount on key elements.",
            "Alter parallel compression blend on the drum bus.",
        ],
        "how_to_verify": lambda f: (
            "A/B against a genre reference with the target heaviness profile. "
            "Does your track feel appropriately heavy/light for its genre context? "
            "Heaviness is partially objective (distortion, sub energy) but also perceptual."
        ),
    },
    "heaviness_weight_low": {
        "what": lambda f: (
            f"Heaviness weight is {f['measured_value']:.2f}, "
            f"below the target threshold of {f['norm_value']:.2f}."
        ),
        "why_helps": lambda f: (
            "Perceptual weight reflects the physical impact of low frequencies and density. "
            "Below-target weight feels thin relative to the intended heaviness."
        ),
        "suggested_action": lambda f: (
            "Increase sub-bass content, compress the low end harder, or add parallel "
            "compression on the drum bus to increase low-frequency density."
        ),
        "engineer_note": lambda f: (
            f"Heaviness weight at {f['measured_value']:.2f} is below target {f['norm_value']:.2f}. "
            "Weight is driven by low-frequency energy and compression density. "
            "Check 808/bass sub content, kick transient weight, and low-end saturation."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Heaviness weight from deep-analysis model; 70% confidence.",
        "most_likely_cause": lambda f: "Thin sub-bass content, over-filtered kick, or insufficient drum compression.",
        "trade_off": lambda f: "Adding low-end weight may muddy mixes on small speakers.",
        "bpm_mood_genre_relevance": lambda f: (
            f"At {f['bpm']:.0f} BPM, {f['genre_name']} depends on physical weight for impact."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"{f['genre_name']} references typically have weight of {f['norm_value']:.2f}."
        ),
        "alternative_strategies": lambda f: [
            "Layer a sub sine under the 808/bass to add weight.",
            "Use a transient designer on drums to increase attack weight.",
        ],
        "how_to_verify": lambda f: (
            "Compare the low-end impact on studio monitors and a subwoofer system. "
            "The kick and sub should have clear, defined weight at the fundamental frequency. "
            "A/B against a reference track with the target weight to confirm alignment."
        ),
    },
    "flat_loudness_arc": {
        "what": lambda f: (
            f"Loudness arc is flat — the loudest section is only {f['measured_value']:.1f} LU louder "
            "than the quietest section. Chorus/drop may not have a clear energy advantage over verse."
        ),
        "why_helps": lambda f: (
            "A loudness arc of at least 2–4 LU between verse and chorus creates a sense of build "
            "and release that keeps listeners engaged. A flat arc makes sections feel interchangeable."
        ),
        "suggested_action": lambda f: (
            "Raise chorus/drop sections by 2–4 dB relative to verse sections using volume automation. "
            "Alternatively, reduce verse density to create more contrast."
        ),
        "engineer_note": lambda f: (
            "Chorus/verse LUFS delta below 2 LU. Per-section integrated LUFS from /analyze/sections endpoint."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 — creative/arrangement note.",
        "confidence_reason": lambda f: "65% confidence — flat arcs may be intentional in ambient, drone, or minimalist genres.",
        "most_likely_cause": lambda f: "Flat arrangement with similar energy levels across verse, chorus, and bridge.",
        "trade_off": lambda f: "Increasing loudness contrast may require changing arrangement density or applying volume automation.",
        "bpm_mood_genre_relevance": lambda f: (
            f"At {f['bpm']:.0f} BPM in {f['genre_name']}, listeners expect energetic differentiation between sections."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Most {f['genre_name']} tracks have at least 2–4 LU of energy contrast between verse and chorus."
        ),
        "alternative_strategies": lambda f: [
            "Add layers (elements, doubles) to chorus sections to increase density and perceived loudness.",
            "Use low-cut automation on verse sections to reduce low-end density and create more contrast.",
            "Consider a sidechain effect on the verse bassline to open up space for the chorus.",
        ],
        "how_to_verify": lambda f: (
            "Check section-level LUFS in a loudness metering tool. Chorus/drop sections should read "
            "at least 2 LUFS louder than the average verse. Listen for the chorus feeling energetically "
            "'bigger' — not just different in texture."
        ),
    },
    "structural_flow_poor": {
        "what": lambda f: (
            f"Track dynamic arc quality is low (score: {f.get('measured_value', 0):.2f}/1.00) — "
            "tension and release may not build toward a satisfying climax."
        ),
        "why_helps": lambda f: (
            "A clear dynamic arc — with defined build, peak, and release — keeps listeners "
            "engaged across the full track duration and makes section transitions feel "
            "purposeful rather than arbitrary."
        ),
        "suggested_action": lambda f: (
            "Review the arrangement. Ensure verse→chorus energy contrast is at least 4–6 dB "
            "of perceived loudness. Automate volume, reverb send, and high-frequency content "
            "to build tension into transitions and release it at drops or choruses."
        ),
        "engineer_note": lambda f: (
            "Arc quality score is computed from section-level RMS variance and structural "
            "contrast across the track. A score below 0.50 indicates a flat energy profile "
            "without clear directional momentum. Arrangement fixes — not mixing — are usually "
            "the right lever for improving arc quality."
        ),
        "severity_context": lambda f: (
            f"Severity {f['severity']:.0f}/100. "
            + ("High — listeners may lose interest before the midpoint without a structural arc." if f["severity"] > 65
               else "Moderate — track could benefit from stronger energy contrast between sections.")
        ),
        "confidence_reason": lambda f: (
            f"Confidence {f['confidence']:.0%}. Arc analysis uses section-level RMS contour "
            "modelling. 70% confidence — creative interpretation of the arc is always valid "
            "for intentionally static genres."
        ),
        "most_likely_cause": lambda f: (
            "Flat energy across sections with no clear build or peak structure. "
            "Verse and chorus may be at similar dynamic levels, removing the sense of forward motion."
        ),
        "trade_off": lambda f: (
            "Forcing a strict dynamic arc may feel mechanical for ambient, lo-fi, or drone genres "
            "where static texture is intentional. Use creative judgment before making "
            "arrangement changes based on this score."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"At {f['bpm']:.0f} BPM, a {f.get('mood', 'neutral')} {f['genre_name']} track "
            "benefits from a tension-release arc that supports the emotional narrative "
            "and sustains engagement across its full duration."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Well-structured {f['genre_name']} tracks score above 0.65 in arc quality, "
            "with clear energy rises into choruses and drops at breakdowns."
        ),
        "alternative_strategies": lambda f: [
            "Add a pre-drop tension build using filter automation or a rising element.",
            "Strip elements from verse sections to increase contrast at the chorus or drop.",
            "Use a bridge section with a key, texture, or tempo shift to create directional contrast.",
        ],
        "how_to_verify": lambda f: (
            "Draw an RMS envelope over the full track and check that energy visibly rises "
            "into choruses and drops at breakdowns. "
            "Listen to the track as a whole — if attention flags before the halfway mark, "
            "the arc needs work. A well-structured track should build tension and release it "
            "at least once before the midpoint."
        ),
    },
    "heaviness_distortion_mismatch": {
        "what": lambda f: (
            f"Distortion content is {f['measured_value']:.2f}, "
            f"vs {f['genre_name']} norm of {f['norm_value']:.2f} (±0.25 tolerance)."
        ),
        "why_helps": lambda f: (
            "Matching distortion character to genre norms ensures the track sounds appropriate "
            "in playlist contexts and meets listener expectations for the style."
        ),
        "suggested_action": lambda f: (
            "Reduce saturation on buses and key elements."
            if f["measured_value"] > f["norm_value"]
            else "Add light saturation or harmonic excitation to key elements for more character."
        ),
        "engineer_note": lambda f: (
            f"Distortion content at {f['measured_value']:.2f} vs genre norm {f['norm_value']:.2f}. "
            "Distortion here refers to intentional harmonic content from saturation, overdrive, "
            "and clipping — not artifact distortion. Adjust at the stem level for best control."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Distortion analysis from deep-analysis model; 70% confidence.",
        "most_likely_cause": lambda f: "Over-saturation of the mix bus, or insufficient saturation on key elements.",
        "trade_off": lambda f: "Reducing distortion may make the track feel cleaner but less characterful.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} targets distortion around {f['norm_value']:.2f} for genre character."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"{f['genre_name']} references cluster around distortion score {f['norm_value']:.2f}."
        ),
        "alternative_strategies": lambda f: [
            "Use tape saturation plugins for subtle harmonic content.",
            "Apply parallel distortion blended at 20–30% for controlled character.",
        ],
        "how_to_verify": lambda f: (
            "Reference against a genre-appropriate track at similar BPM and mood. "
            "Listen for saturation character on drums and bass — does it match the reference? "
            "Adjust saturation plugin settings and compare until the harmonic content aligns."
        ),
    },
    "section_arc_too_flat": {
        "what": lambda f: (
            f"Loudness arc across sections is only {f['measured_value']:.1f} LU - sections are "
            "uniformly loud with no dynamic contrast."
        ),
        "why_helps": lambda f: (
            "Listeners feel the difference between verse and chorus partly through level: 3-6 LU of "
            "contrast makes choruses and drops hit harder."
        ),
        "suggested_action": lambda f: (
            "Use clip-gain automation or fader rides to create at least 3 LU of contrast between the "
            "quietest and loudest sections."
        ),
        "engineer_note": lambda f: (
            "If bus compression is flattening the arc, ease its ratio/attack in the verses or "
            "automate its makeup gain. Per-section LUFS comes from the section analysis."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 - an arrangement/automation note.",
        "confidence_reason": lambda f: (
            "Measured from per-section integrated LUFS; a deliberately static arrangement (ambient, "
            "drone, loop-based) can be intentional."
        ),
        "most_likely_cause": lambda f: "Heavy bus compression or limiting flattening the arrangement's energy.",
        "trade_off": lambda f: "A wider arc lowers the quieter sections' loudness on normalised platforms.",
        "bpm_mood_genre_relevance": lambda f: (
            f"At {f['bpm']:.0f} BPM, {f['genre_name']} listeners expect sections to differ in energy."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Most {f['genre_name']} tracks carry 3-6 LU between their quietest and loudest sections."
        ),
        "alternative_strategies": lambda f: [
            "Thin the verse arrangement instead of riding levels.",
            "Automate the master bus compressor's threshold per section.",
            "Add layers to the chorus/drop to raise its density.",
        ],
        "how_to_verify": lambda f: (
            "Re-run section analysis: the loudest-to-quietest section gap should be at least 3 LU, "
            "and the chorus should feel bigger, not just different."
        ),
    },
    "section_arc_too_wide": {
        "what": lambda f: (
            f"Loudness arc is {f['measured_value']:.1f} LU - the jump between the quietest and "
            "loudest sections may feel jarring."
        ),
        "why_helps": lambda f: (
            "Very large section-to-section jumps feel abrupt and fatiguing; on normalised playback "
            "the quiet sections end up very quiet."
        ),
        "suggested_action": lambda f: (
            "Bring the loudest section down and/or the quietest up so the arc lands under 9 LU - "
            "split the difference between the two."
        ),
        "engineer_note": lambda f: (
            "A 4-8 LU arc suits most genres. Above 9 LU, check for extreme fader automation or a "
            "breakdown that drops out almost entirely."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: (
            "Measured from per-section integrated LUFS; cinematic or classical material can "
            "legitimately exceed this range."
        ),
        "most_likely_cause": lambda f: "Extreme fader automation or very unbalanced section production energy.",
        "trade_off": lambda f: "Narrowing the arc reduces the impact of the loudest section.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} rarely needs more than about 8 LU of section contrast."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Reference {f['genre_name']} tracks typically keep sections within 4-8 LU of each other."
        ),
        "alternative_strategies": lambda f: [
            "Add a sustained element to the quiet section to lift its floor.",
            "Use gentle bus compression on the loud section rather than a fader cut.",
        ],
        "how_to_verify": lambda f: (
            "Re-run section analysis: the loudest-to-quietest gap should be under 9 LU, and the "
            "transitions should feel intentional rather than abrupt."
        ),
    },
    "section_peak_too_quiet": {
        "what": lambda f: (
            f"Even the loudest section reaches only {f['measured_value']:.1f} LUFS - below the "
            f"{f['genre_name']} floor of {f['norm_value']:.1f} LUFS."
        ),
        "why_helps": lambda f: (
            "If the loudest section is below the genre floor, the whole track sounds quiet and "
            "low-energy against references, including where it should peak."
        ),
        "suggested_action": lambda f: (
            f"Raise the master output - the loudest section needs about "
            f"{f['norm_value'] - f['measured_value']:.1f} dB more gain before the limiter."
        ),
        "engineer_note": lambda f: (
            "Check the limiter's output ceiling and makeup gain; a conservative ceiling or a missing "
            "final gain stage is the usual cause."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: (
            "Per-section integrated LUFS against the genre's loudness floor - a deterministic "
            "comparison."
        ),
        "most_likely_cause": lambda f: "Insufficient makeup gain after limiting, or a limiter ceiling set too low.",
        "trade_off": lambda f: "Pushing the loudest section harder into the limiter can cost transient punch.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} peaks are expected to reach about {f['norm_value']:.1f} LUFS."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"The loudest sections of reference {f['genre_name']} tracks sit at or above "
            f"{f['norm_value']:.1f} LUFS."
        ),
        "alternative_strategies": lambda f: [
            "Raise the gain into the limiter rather than the ceiling.",
            "Use a clipper before the limiter to gain level transparently.",
        ],
        "how_to_verify": lambda f: (
            "Re-run section analysis: the loudest section should reach at least the genre floor. "
            "Check true peak stays within the delivery ceiling after the gain change."
        ),
    },
}
