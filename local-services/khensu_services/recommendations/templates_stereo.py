"""Stereo, phase and translation narratives for the recommendation text factory (ENG-6, from text_factory.py).

Each entry maps a field name to a callable over the finding context; see
text_factory.TEMPLATES for the contract."""
from __future__ import annotations

from collections.abc import Callable

TEMPLATES: dict[str, dict[str, Callable[[dict], str | list[str]]]] = {
    "stereo_too_narrow": {
        "what": lambda f: (
            f"Stereo width is {f['measured_value']:.2f}, "
            f"below the {f['genre_name']} minimum of {f['norm_value']:.2f}."
        ),
        "why_helps": lambda f: (
            "A narrow stereo image reduces spatial immersion and may sound mono on "
            "headphone listening, which is now the dominant playback format."
        ),
        "suggested_action": lambda f: (
            "Apply stereo widening to synth pads and room reverbs. "
            "Avoid widening bass elements below 120 Hz (preserves mono compatibility)."
        ),
        "engineer_note": lambda f: (
            f"Stereo width at {f['measured_value']:.2f} is narrower than the {f['genre_name']} "
            f"minimum of {f['norm_value']:.2f}. Apply width at the element level first — "
            "master bus stereo widening can cause phase issues."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Stereo width computed from M/S analysis.",
        "most_likely_cause": lambda f: "Mix lacks stereo reverb/delay on key elements, or is effectively mono.",
        "trade_off": lambda f: "Excessive widening artifacts can cause comb filtering.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} at {f['bpm']:.0f} BPM benefits from an enveloping stereo field."
        ),
        "comparison_to_similar_songs": lambda f: f"{f['genre_name']} tracks target width above {f['norm_value']:.2f}.",
        "alternative_strategies": lambda f: [
            "Haas effect: delay one side 15–30 ms on ambient elements.",
            "Mid-Side EQ: boost highs in the Side channel above 5 kHz.",
            "Add stereo reverb returns — keeps mono compatibility better than direct widening.",
        ],
        "how_to_verify": lambda f: (
            "Switch your monitoring to stereo after widening. The soundstage should extend "
            "noticeably beyond the speakers. Check mono compatibility — sum to mono and "
            "confirm no significant elements disappear."
        ),
    },
    "stereo_too_wide": {
        "what": lambda f: (
            f"Stereo width is {f['measured_value']:.2f}, "
            f"above the {f['genre_name']} maximum of {f['norm_value']:.2f}."
        ),
        "why_helps": lambda f: (
            "Extreme stereo width often causes phase cancellation in mono, losing "
            "crucial low-mid information on club systems and consumer mono speakers."
        ),
        "suggested_action": lambda f: (
            "Reduce master bus stereo width. Check elements causing extreme width — "
            "likely synth pads or room reverbs. Apply MS processing to narrow the sides."
        ),
        "engineer_note": lambda f: (
            f"Width at {f['measured_value']:.2f} exceeds the {f['genre_name']} norm of "
            f"{f['norm_value']:.2f}. Mono-check the mix — if content disappears on mono "
            "fold-down, width is causing phase cancellation."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Stereo width from M/S analysis; correlation check recommended.",
        "most_likely_cause": lambda f: "Excessive reverb or stereo widener plugin on the master bus.",
        "trade_off": lambda f: "Narrowing may reduce spatial immersion on headphone playback.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} production standards call for mono-compatible width."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"{f['genre_name']} references stay below {f['norm_value']:.2f} for mono compatibility."
        ),
        "alternative_strategies": lambda f: [
            "Apply mid-side EQ to reduce Side channel highs (key widening frequency range).",
            "Use a gentle Side-channel low-pass to focus imaging.",
        ],
        "how_to_verify": lambda f: (
            "Sum to mono. Check that no primary elements (lead vocal, kick, snare, bass) "
            "cancel or lose significant level. Run a correlation meter — stereo correlation "
            "below 0.3 indicates phase risk."
        ),
    },
    "phase_issue": {
        "what": lambda f: (
            f"Stereo correlation is {f['measured_value']:.2f}, "
            "indicating significant phase cancellation between left and right channels."
        ),
        "why_helps": lambda f: (
            "Negative correlation causes destructive interference when summed to mono, "
            "potentially removing bass and key harmonic content."
        ),
        "suggested_action": lambda f: (
            "Mono-check the mix. Identify elements with extreme stereo widening or "
            "misaligned microphone phases. Apply polarity inversion or phase rotation on offenders."
        ),
        "engineer_note": lambda f: (
            f"Correlation at {f['measured_value']:.2f} is in anti-phase territory. "
            "Solo elements in the stereo field and mono-check each to isolate the cause. "
            "Common culprits: chorus plugins, stereo wideners, or mis-phased room mics."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 — mono sum will suffer.",
        "confidence_reason": lambda f: (
            "Correlation is the normalized cross-correlation of the left and right channels: "
            "+1 = identical (mono), 0 = unrelated, -1 = polarity-inverted."
        ),
        "most_likely_cause": lambda f: "Stereo widener plugin or chorus effect causing phase inversion.",
        "trade_off": lambda f: "Fixing phase may narrow the stereo image on problem elements.",
        "bpm_mood_genre_relevance": lambda f: "Phase correlation issues affect all genres equally — technical defect.",
        "comparison_to_similar_songs": lambda f: "Published tracks maintain positive correlation (0.3–0.8 typical range).",
        "alternative_strategies": lambda f: [
            "Use a mid-side matrix to tame Side channel width.",
            "Replace problematic stereo widener with a Haas delay approach.",
            "Check room/ambience mic placement if tracking to real spaces.",
        ],
        "how_to_verify": lambda f: (
            "Switch monitoring to mono. Any element that loses significant volume or changes "
            "character dramatically has a phase issue. Then, back in stereo, a phase correlation "
            "meter on the full mix should read above about +0.3 (a mono signal always reads +1, "
            "so meter the stereo bus)."
        ),
    },
    "mono_compat_warning": {
        "what": lambda f: (
            "Stereo content has low mono compatibility — bass or stereo width may collapse "
            "or cancel when the mix is summed to mono."
        ),
        "why_helps": lambda f: (
            "Improving mono compatibility ensures the mix survives playback on mono speakers — "
            "phone speakers, club PA centre fills, TV sets, and any Bluetooth device in mono mode."
        ),
        "suggested_action": lambda f: (
            "Use a high-pass mid/side EQ to keep low-end content in the mid (mono) channel. "
            "Mono-ize all content below 100–120 Hz. Use a correlation meter to pinpoint "
            "the specific band causing cancellation."
        ),
        "engineer_note": lambda f: (
            "Per-band stereo correlation analysis identified bands with insufficient mono compatibility. "
            "Low correlation in the sub/bass bands is the most common cause — bass frequencies "
            "should always be anchored in the mid channel. Mono-izing below 120 Hz is standard "
            "professional mixing practice."
        ),
        "severity_context": lambda f: (
            f"Severity {f['severity']:.0f}/100. "
            + ("Severe — significant mono cancellation expected on bass frequencies." if f["severity"] > 65
               else "Moderate — mono sum may lose some low-end weight and punch.")
        ),
        "confidence_reason": lambda f: (
            f"Confidence {f['confidence']:.0%}. Per-band correlation computed via bandpass "
            "filtering and Pearson correlation analysis across critical frequency bands."
        ),
        "most_likely_cause": lambda f: (
            "Wide stereo bass elements, a stereo widener affecting low frequencies, "
            "or out-of-phase room microphones summing destructively in mono."
        ),
        "trade_off": lambda f: (
            "Mono-izing the low end may slightly reduce the perceived stereo width of bass "
            "elements on headphone playback. The gain in mono compatibility almost always "
            "outweighs the minor reduction in perceived width."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"In {f['genre_name']} at {f['bpm']:.0f} BPM, mono compatibility is especially "
            "critical — bass-heavy genres suffer most from sub-frequency phase cancellation in mono."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Professional {f['genre_name']} releases consistently pass mono compatibility checks, "
            "with bass content maintained within 3 dB of the stereo level when summed to mono."
        ),
        "alternative_strategies": lambda f: [
            "Use a dedicated bass mono plugin (e.g., SPAN Plus with correlation view, Brauer Motion).",
            "Apply M/S processing: high-pass the side channel above 120 Hz to keep bass in mono.",
            "Audit individual bus elements — often only one or two sources are responsible for the correlation issue.",
        ],
        "how_to_verify": lambda f: (
            "Sum the mix to mono in your DAW and compare to the stereo version at matched loudness. "
            "Low-end (kick, bass, sub) should be within 3 dB of the stereo level. "
            "Any element that disappears or loses significant volume has a phase or mono compatibility issue."
        ),
    },
    "translation_risk_high": {
        "what": lambda f: (
            "Translation risk detected for a specific playback environment — "
            "the mix may sound significantly different on this system type than on studio monitors."
        ),
        "why_helps": lambda f: (
            "Addressing environment-specific translation risks ensures the mix sounds intentional "
            "to the majority of listeners, not just those with access to studio monitoring."
        ),
        "suggested_action": lambda f: (
            "Reference the mix on the flagged playback environment. "
            "Address the specific frequency or dynamics imbalance indicated by companion "
            "recommendations such as harshness_detected, mono_compat_warning, or centroid issues."
        ),
        "engineer_note": lambda f: (
            "Translation risk is a composite estimate derived from spectral centroid, "
            "low-end balance, stereo correlation, and crest factor. It flags environments "
            "where these metrics predict an audible imbalance. Always verify by direct listening — "
            "the model is indicative, not definitive."
        ),
        "severity_context": lambda f: (
            f"Severity {f['severity']:.0f}/100. "
            + ("High — the mix is likely to sound noticeably different on this device type." if f["severity"] > 65
               else "Moderate — minor tonal or dynamic imbalance expected on the flagged environment.")
        ),
        "confidence_reason": lambda f: (
            f"Confidence {f['confidence']:.0%}. Translation risk estimated from mono compatibility, "
            "spectral centroid, and dynamics data. Use as a guide for targeted listening tests, "
            "not as a definitive measurement."
        ),
        "most_likely_cause": lambda f: (
            "Spectral imbalance or mono compatibility issues that are less obvious on "
            "studio monitors but become prominent on consumer playback systems."
        ),
        "trade_off": lambda f: (
            "Optimising for one playback environment can shift the balance for another — "
            "prioritise the most common listening context for your target audience."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"For {f['genre_name']} at {f['bpm']:.0f} BPM, the most critical environments are "
            "typically phone speakers and earbuds, which represent the majority of streams "
            "for this genre."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Reference {f['genre_name']} tracks pass translation checks across earbuds, "
            "phone speakers, and club PA by maintaining a controlled spectral balance "
            "and robust mono compatibility."
        ),
        "alternative_strategies": lambda f: [
            "A/B against a reference track directly on the flagged playback device.",
            "Use a speaker emulator plugin (Audified CR8, Waves NX) to simulate the environment in the DAW.",
            "Address underlying mono compat or harshness findings — they are often the root cause of high translation risk.",
        ],
        "how_to_verify": lambda f: (
            "Export and play back on the flagged environment at typical listening level. "
            "Compare perceived frequency balance and dynamics to a reference track in the same genre. "
            "A significant tonal or dynamic difference between your track and the reference confirms the risk."
        ),
    },
    "sub_bass_too_wide": {
        "what": lambda f: (
            "Sub-bass is too wide: side/mid energy ratio below 100 Hz is above the 0.30 threshold. "
            "Low-frequency content should be anchored in the mid (mono) channel."
        ),
        "why_helps": lambda f: (
            "Sub-bass frequencies with a high side/mid ratio cancel significantly when the stereo "
            "signal is summed to mono, reducing bass impact on club systems, phone speakers, "
            "and any mono playback context."
        ),
        "suggested_action": lambda f: (
            "Use a mid-side EQ to high-pass the side channel around 100 Hz so the sub is mono. "
            "Most DAWs have a built-in M/S EQ — apply a high-pass on the side channel at 80–120 Hz."
        ),
        "engineer_note": lambda f: (
            "M/S sub ratio computed from FFT energy below 100 Hz. "
            "Mono compatibility for bass depends on keeping sub content in the center channel."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Side/mid sub ratio computed from FFT energy below 100 Hz; deterministic.",
        "most_likely_cause": lambda f: "Wide stereo bass elements, stereo widener affecting low frequencies, or out-of-phase room mics.",
        "trade_off": lambda f: (
            "Mono-izing the low end may reduce perceived bass 'width' on headphones slightly, "
            "but gains significant mono compatibility on all mono and near-mono playback contexts."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"In {f['genre_name']}, mono-compatible sub-bass is essential for club and mobile playback."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Professional {f['genre_name']} releases keep sub-bass anchored in the mid channel "
            "to ensure bass survives mono fold-down."
        ),
        "alternative_strategies": lambda f: [
            "Apply a dedicated bass-mono plugin (e.g. Brauer Motion, SPAN Plus correlation view).",
            "High-pass the side (S) channel at 100 Hz using M/S processing on the master bus.",
        ],
        "how_to_verify": lambda f: (
            "Sum to mono and compare bass level to the stereo version. Bass should lose less than 3 dB "
            "when folding to mono. A correlation meter above 0.7 in the sub band confirms safe mono compatibility."
        ),
    },
}
