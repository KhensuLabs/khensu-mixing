"""Tonal-balance narratives for the recommendation text factory (ENG-6, from text_factory.py).

Each entry maps a field name to a callable over the finding context; see
text_factory.TEMPLATES for the contract."""
from __future__ import annotations

from collections.abc import Callable

TEMPLATES: dict[str, dict[str, Callable[[dict], str | list[str]]]] = {
    "centroid_too_bright": {
        "what": lambda f: (
            f"Spectral centroid is {f['measured_value']:.0f} Hz, "
            f"above the {f['genre_name']} norm of {f['norm_value']:.0f} Hz (25% brighter than expected)."
        ),
        "why_helps": lambda f: (
            "An overly bright centroid causes ear fatigue on extended listening and may "
            "indicate high-frequency harshness or sibilance."
        ),
        "suggested_action": lambda f: (
            "Apply gentle high-shelf cut (2–3 dB) above 8 kHz on the master bus. "
            "Check for harsh resonances in the 3–8 kHz range."
        ),
        "engineer_note": lambda f: (
            f"Centroid at {f['measured_value']:.0f} Hz sits above the {f['genre_name']} "
            f"norm of {f['norm_value']:.0f} Hz. Compare on translation systems — NS10s or "
            "earbuds can expose harshness that sounds fine on studio monitors."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 — monitor translation risk.",
        "confidence_reason": lambda f: "Spectral centroid is calculated deterministically from the FFT.",
        "most_likely_cause": lambda f: "Excessive high-shelf boost, over-excited cymbals, or bright lead synthesis.",
        "trade_off": lambda f: "Rolling off highs may reduce perceived air and detail on high-quality playback systems.",
        "bpm_mood_genre_relevance": lambda f: (
            f"In {f['genre_name']}, brightness norms ensure consistent translation across listening environments."
        ),
        "comparison_to_similar_songs": lambda f: f"{f['genre_name']} references typically center around {f['norm_value']:.0f} Hz.",
        "alternative_strategies": lambda f: [
            "Use a dynamic EQ to cut highs only when transients spike.",
            "Check and reduce excessive reverb tail brightness.",
            "De-ess the master bus to tame high-frequency peaks.",
        ],
        "how_to_verify": lambda f: (
            "Listen on a consumer earphone at normal listening level for 90 seconds. "
            "If listener fatigue sets in before the 90-second mark, the top-end is still too aggressive. "
            "Check the spectral centroid again — it should fall closer to the genre median."
        ),
    },
    "centroid_too_dark": {
        "what": lambda f: (
            f"Spectral centroid is {f['measured_value']:.0f} Hz, "
            f"below the {f['genre_name']} norm of {f['norm_value']:.0f} Hz (25% darker than expected)."
        ),
        "why_helps": lambda f: (
            "A centroid too far below the genre norm makes a track sound dull, "
            "muffled, or lacking in presence and definition."
        ),
        "suggested_action": lambda f: (
            "Apply a gentle high-shelf boost (1–2 dB) above 8 kHz or add presence "
            "in the 3–5 kHz range. Check for over-use of low-pass filtering."
        ),
        "engineer_note": lambda f: (
            f"Centroid reads {f['measured_value']:.0f} Hz vs {f['genre_name']} norm "
            f"of {f['norm_value']:.0f} Hz. Common causes: heavy low-pass filtering, "
            "over-boosted low mids, or dark sample palette."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Spectral centroid computed deterministically from FFT.",
        "most_likely_cause": lambda f: "Over-aggressive low-pass filtering, muddy low-mids, or dark sample palette.",
        "trade_off": lambda f: "Adding brightness may expose noise or harshness in certain elements.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} requires brightness around {f['norm_value']:.0f} Hz for genre-appropriate clarity."
        ),
        "comparison_to_similar_songs": lambda f: f"Reference {f['genre_name']} tracks sit around {f['norm_value']:.0f} Hz centroid.",
        "alternative_strategies": lambda f: [
            "Reduce low-mid buildup in the 200–400 Hz range to allow highs to cut through.",
            "Check high-pass filter cutoffs on individual tracks — over-cutting lowers centroid.",
        ],
        "how_to_verify": lambda f: (
            "Test on earbuds and laptop speakers. The mix should have presence and definition "
            "on these devices without sounding muffled. A/B against a reference track in the same genre."
        ),
    },
    "low_end_heavy": {
        "what": lambda f: (
            f"Low-end energy share is {f['measured_value']:.2f} (below 250 Hz), "
            f"exceeding the {f['genre_name']} norm by >0.20."
        ),
        "why_helps": lambda f: (
            "Excess low-end weight causes muddiness on smaller speakers and masks "
            "mid-frequency information critical for vocal clarity."
        ),
        "suggested_action": lambda f: (
            "Apply a gentle high-pass filter on the bass/kick group or reduce low-shelf "
            "gain by 1–2 dB on the master bus. Check sub bass alignment between kick and bass."
        ),
        "engineer_note": lambda f: (
            f"Low-frequency energy share is {f['measured_value']:.2f} vs {f['genre_name']} "
            f"norm. This level typically indicates overlapping sub energy between kick, bass, "
            "and/or 808. Use a spectrum analyzer to identify the dominant offender."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Low-end balance is computed from octave band energy.",
        "most_likely_cause": lambda f: "Kick and bass subs overlapping without frequency separation or sidechain.",
        "trade_off": lambda f: "Reducing low end may thin the track on large subwoofer systems.",
        "bpm_mood_genre_relevance": lambda f: (
            f"In {f['genre_name']}, low-end weight norms exist for translation across listening environments."
        ),
        "comparison_to_similar_songs": lambda f: f"{f['genre_name']} tracks have lower low-end weight than detected here.",
        "alternative_strategies": lambda f: [
            "Sidechain compress the bass from the kick to create rhythmic sub movement.",
            "Use mid-side EQ to cut lows in the sides channel.",
            "High-pass any elements above 60 Hz that shouldn't have sub content.",
        ],
        "how_to_verify": lambda f: (
            "Sum to mono and play at moderate volume on a single laptop speaker or phone. "
            "If the low end becomes undefined or bloomy rather than punchy, excess remains. "
            "The kick fundamental should be clearly distinguishable from the sub."
        ),
    },
    "low_end_light": {
        "what": lambda f: (
            f"Low-end energy share is {f['measured_value']:.2f}, "
            f"below the {f['genre_name']} norm by >0.20."
        ),
        "why_helps": lambda f: (
            "Insufficient low-end weight makes the track feel thin and lacking impact "
            "on full-range systems and club environments."
        ),
        "suggested_action": lambda f: (
            "Boost low shelf by 1–2 dB at 60–80 Hz on the master bus. "
            "Check sub octave content on kick and bass elements."
        ),
        "engineer_note": lambda f: (
            f"Low-end share reads {f['measured_value']:.2f} vs {f['genre_name']} norm. "
            "Always reference on a system with sub extension. "
            "Add sub content at the mix level rather than broad low-shelf boosts at mastering."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Low-end balance computed from octave band analysis.",
        "most_likely_cause": lambda f: "Over-aggressive high-passing on bass/kick, or thinly produced bass elements.",
        "trade_off": lambda f: "Adding low end may reduce definition on laptop speakers.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} at {f['bpm']:.0f} BPM relies on sub energy for physical impact."
        ),
        "comparison_to_similar_songs": lambda f: f"{f['genre_name']} references show more low-end weight than detected here.",
        "alternative_strategies": lambda f: [
            "Layer a sub sine or sub octave under the bass to add weight.",
            "Check that high-pass filters on bass elements aren't too aggressive.",
        ],
        "how_to_verify": lambda f: (
            "Play on studio monitors and check the low-end with a spectrum analyser. "
            "The sub/bass region should have comparable energy to genre reference tracks. "
            "A-B against a reference at matched loudness."
        ),
    },
    "harshness_detected": {
        "what": lambda f: (
            "Harsh energy detected in the 2.5–5 kHz region — "
            "elevated ear-fatigue frequencies identified in the master bus output."
        ),
        "why_helps": lambda f: (
            "Reducing ear-fatigue frequencies in the 2.5–5 kHz range improves long-term "
            "listening comfort and translates better on consumer earbuds and laptop speakers, "
            "where this region is particularly prominent."
        ),
        "suggested_action": lambda f: (
            "Apply a narrow bell EQ cut around 3–4 kHz on the master bus, "
            "or use a de-esser targeting the 2.5–5 kHz region. "
            "Start with no more than 1.5–2 dB of reduction."
        ),
        "engineer_note": lambda f: (
            "Harshness score is computed from 2.5–5 kHz RMS energy relative to the surrounding "
            "broadband mid-band level. This region is particularly fatiguing on earbuds and "
            "small speakers. Identify the specific offending element — often cymbals, distorted "
            "guitars, or aggressive vocals — before cutting on the master bus."
        ),
        "severity_context": lambda f: (
            f"Severity {f['severity']:.0f}/100. "
            + ("High — likely audible on earbuds within 60 seconds of listening." if f["severity"] > 65
               else "Moderate — may cause fatigue on extended listening sessions.")
        ),
        "confidence_reason": lambda f: (
            f"Confidence {f['confidence']:.0%}. Harshness ratio computed from 2.5–5 kHz RMS "
            "vs broadband mid-band energy. Moderate certainty — verify by ear."
        ),
        "most_likely_cause": lambda f: (
            "Excessive high-mid energy from cymbals, distorted guitars, vocal sibilance, "
            "or an over-boosted presence shelf on the master bus."
        ),
        "trade_off": lambda f: (
            "Cutting the 2.5–5 kHz region can reduce hi-hat brightness, consonant clarity, "
            "and perceived attack on percussion. Cut conservatively (≤2 dB) and verify "
            "on full-range studio monitors after applying the fix."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"In {f['genre_name']} at {f['bpm']:.0f} BPM, harshness is most problematic "
            "on streaming platforms where loudness normalisation can accentuate "
            "high-frequency imbalances on low-quality earbuds."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Well-mastered {f['genre_name']} tracks maintain controlled 2.5–5 kHz energy "
            "to ensure listening comfort across all playback systems."
        ),
        "alternative_strategies": lambda f: [
            "Use multiband compression targeting the 2.5–5 kHz band only during transients.",
            "Identify and treat the harsh source element at the channel level rather than on the master bus.",
            "A dynamic EQ set to cut only when the 3.5 kHz peak exceeds a threshold is less destructive than a static notch.",
        ],
        "how_to_verify": lambda f: (
            "Listen on consumer earbuds at moderate volume for 90 seconds. "
            "If listening fatigue sets in before 90 seconds, harshness remains. "
            "The 2.5–5 kHz region should not visibly dominate the spectral balance display."
        ),
    },
    "ms_brightness_imbalance": {
        "what": lambda f: (
            "Stereo side channel is significantly brighter than the mid above 4 kHz. "
            "Exaggerated side-channel brightness creates an unnatural stereo field."
        ),
        "why_helps": lambda f: (
            "Excessive side-channel brightness above 4 kHz creates an overly wide high-frequency "
            "field that can sound fatiguing on headphones and collapse oddly on mono systems."
        ),
        "suggested_action": lambda f: (
            "Apply a high-shelf cut on the side channel above 6–8 kHz using M/S EQ. "
            "Aim to bring the side/mid high-frequency ratio below 1.3."
        ),
        "engineer_note": lambda f: (
            "M/S brightness imbalance detected. Side/mid ratio above 4 kHz computed from FFT."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Side/mid spectral ratio computed from FFT above 4 kHz; deterministic.",
        "most_likely_cause": lambda f: "Stereo reverb tails, chorus effects, or master bus stereo widener boosting the side channel.",
        "trade_off": lambda f: "Cutting the side channel's high frequencies may reduce perceived air and spaciousness on headphones.",
        "bpm_mood_genre_relevance": lambda f: (
            f"In {f['genre_name']}, natural stereo balance ensures consistent imaging across headphones and speakers."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Well-balanced {f['genre_name']} releases maintain comparable M/S energy in the high frequency band."
        ),
        "alternative_strategies": lambda f: [
            "Use M/S EQ to cut highs in the side channel above 8 kHz.",
            "Address individual wide-field elements (stereo reverbs, chorus) before the master bus.",
        ],
        "how_to_verify": lambda f: (
            "Check the M/S balance on a spectrum analyser — the side channel's high-frequency "
            "energy should be below or comparable to the mid channel. "
            "Listen on headphones and confirm the stereo field sounds natural rather than exaggerated."
        ),
    },
    "resonance_hotspot": {
        "what": lambda f: (
            "Narrow resonance peak detected — a specific frequency has excessive prominence "
            "above the surrounding spectrum, creating an audible tonal imbalance."
        ),
        "why_helps": lambda f: (
            "Resonance peaks cause a 'boomy' or 'ringy' tone at the problem frequency, "
            "drawing unwanted attention and fatiguing the ear over extended listening."
        ),
        "suggested_action": lambda f: (
            "Use a narrow bell EQ cut (Q 4–8) at the flagged frequency. "
            "Start with a 2–3 dB cut and confirm improvement with a spectrum analyser. "
            "Trace the resonance back to its source element if possible and address it there."
        ),
        "engineer_note": lambda f: (
            "Resonance prominence is measured above the surrounding spectral floor using "
            "peak detection with an 8 dB minimum prominence threshold. Verify by listening — "
            "some prominences are intentional (kick fundamental, bass note body). "
            "Solo a narrow EQ boost at the reported frequency and listen before cutting."
        ),
        "severity_context": lambda f: (
            f"Severity {f['severity']:.0f}/100. "
            + ("High prominence — likely audible as a 'ringing' tone on sustained material." if f["severity"] > 65
               else "Moderate — may add subtle colouration that fatigues over time.")
        ),
        "confidence_reason": lambda f: (
            f"Confidence {f['confidence']:.0%}. Prominence computed from spectral magnitude "
            "peak detection. 72% confidence accounts for the possibility that the peak "
            "is an intentional musical element rather than an unwanted resonance."
        ),
        "most_likely_cause": lambda f: (
            "Room modes captured in a recording, an instrument body resonance, "
            "or a narrow frequency boost left in an EQ plugin."
        ),
        "trade_off": lambda f: (
            "A targeted notch cut may thin the body of a specific instrument if the resonance "
            "falls on a musical fundamental. Using a dynamic EQ that cuts only when the "
            "resonance exceeds a threshold is less destructive than a static notch."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"Resonance hotspots in {f['genre_name']} are most problematic in the bass and "
            "low-mid range, where narrow peaks mask rhythmic clarity and create unwanted "
            "low-frequency emphasis."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Professionally mixed {f['genre_name']} tracks maintain a clean spectral profile "
            "without prominent isolated peaks standing above the surrounding spectral floor."
        ),
        "alternative_strategies": lambda f: [
            "Use a dynamic EQ (Fabfilter Pro-MB) to cut the resonance only when it exceeds a threshold.",
            "Trace the resonance to the source instrument and address it on the channel strip, not the master bus.",
            "Use a real-time spectrum analyser to confirm the peak frequency before applying any cut.",
        ],
        "how_to_verify": lambda f: (
            "Solo a narrow EQ boost at the reported frequency to confirm the resonance location by ear. "
            "Then apply the cut and verify the peak drops by at least 4 dB on a spectrum analyser. "
            "Listen for loss of body on the affected element — if it sounds thin, widen the Q or reduce the cut depth."
        ),
    },
    "tilt_too_bright": {
        "what": lambda f: (
            f"Overall tonal balance slopes at {f['measured_value']:.1f} dB/octave - brighter than the "
            f"{f['genre_name']} target of {f['norm_value']:.1f} dB/octave."
        ),
        "why_helps": lambda f: (
            "Spectral tilt is the mix's overall brightness curve, not a single frequency. A tilt "
            "brighter than the genre norm sounds thin and fatiguing at volume and harsh on bright "
            "playback systems."
        ),
        "suggested_action": lambda f: (
            "Apply a broad, gentle high-shelf cut (or a tilt EQ) across the top octaves to bring the "
            f"slope down toward {f['norm_value']:.1f} dB/octave. Start with 1-2 dB."
        ),
        "engineer_note": lambda f: (
            "Tilt is the regression slope of the whole band curve: fix it with a broad shelf over "
            "several octaves, not a narrow bell. Check whether the low end is simply missing first."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 - a tonal-balance taste call.",
        "confidence_reason": lambda f: (
            "The slope is measured deterministically; the target is a genre norm, so this is a "
            "style comparison rather than a defect detection."
        ),
        "most_likely_cause": lambda f: (
            "Top-end boosts stacked across many channels, or a thin low end that tips the curve up."
        ),
        "trade_off": lambda f: "Darkening the tilt can dull presence and air if the shelf starts too low.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} mixes usually sit near a {f['norm_value']:.1f} dB/octave tilt."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Reference {f['genre_name']} tracks fall within about 2 dB/octave of "
            f"{f['norm_value']:.1f} dB/octave."
        ),
        "alternative_strategies": lambda f: [
            "Use a tilt EQ pivoting around 1 kHz for a single-control correction.",
            "Pull back the brightest individual elements instead of EQ-ing the master.",
            "Add low-end weight if the tilt is bright because the bottom is thin.",
        ],
        "how_to_verify": lambda f: (
            "Re-measure spectral tilt: it should land within +/-2 dB/octave of the genre target. "
            "A/B against a loudness-matched reference on both headphones and speakers."
        ),
    },
    "tilt_too_dark": {
        "what": lambda f: (
            f"Overall tonal balance slopes at {f['measured_value']:.1f} dB/octave - darker than the "
            f"{f['genre_name']} target of {f['norm_value']:.1f} dB/octave."
        ),
        "why_helps": lambda f: (
            "A tilt darker than the genre norm sounds muffled and distant, and loses definition on "
            "small speakers where low-frequency energy can't be reproduced."
        ),
        "suggested_action": lambda f: (
            "Apply a broad high-shelf boost, or reduce low/low-mid build-up, to bring the slope up "
            f"toward {f['norm_value']:.1f} dB/octave. Start with 1-2 dB."
        ),
        "engineer_note": lambda f: (
            "Tilt is the regression slope of the whole band curve: a broad shelf over several "
            "octaves is the right tool. Often the cure is cutting 200-500 Hz mud, not boosting air."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 - a tonal-balance taste call.",
        "confidence_reason": lambda f: (
            "The slope is measured deterministically; the target is a genre norm, so this is a "
            "style comparison rather than a defect detection."
        ),
        "most_likely_cause": lambda f: "Low-mid build-up across many tracks, or dull sources and heavy low-pass filtering.",
        "trade_off": lambda f: "Brightening the tilt can expose harshness and sibilance; watch 3-8 kHz.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} mixes usually sit near a {f['norm_value']:.1f} dB/octave tilt."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Reference {f['genre_name']} tracks fall within about 2 dB/octave of "
            f"{f['norm_value']:.1f} dB/octave."
        ),
        "alternative_strategies": lambda f: [
            "Use a tilt EQ pivoting around 1 kHz for a single-control correction.",
            "Cut low-mid mud on the busiest buses before adding any top end.",
            "Brighten the key elements (vocal, snare) rather than the whole master.",
        ],
        "how_to_verify": lambda f: (
            "Re-measure spectral tilt: it should land within +/-2 dB/octave of the genre target. "
            "Check small-speaker playback for restored definition without new harshness."
        ),
    },
}
