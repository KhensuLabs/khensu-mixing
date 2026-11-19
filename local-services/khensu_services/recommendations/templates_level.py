"""Loudness, dynamics and signal-integrity narratives for the recommendation text factory (ENG-6, from text_factory.py).

Each entry maps a field name to a callable over the finding context; see
text_factory.TEMPLATES for the contract."""
from __future__ import annotations

from collections.abc import Callable


def _tp_severity_context(measured: float, ceiling: float) -> str:
    """Differentiate true peak severity text by overage magnitude.

    A value just over -1 dBTP is a marginal fix; +1 dBTP is an audible defect.
    """
    overage = measured - ceiling
    if overage >= 1.0:
        return (
            f"Critical — {measured:.2f} dBTP is {overage:.2f} dB over the ceiling. "
            "Audible inter-sample distortion on AAC/MP3 delivery is likely. "
            "Do not release without fixing."
        )
    elif overage >= 0.3:
        return (
            f"High — {measured:.2f} dBTP is {overage:.2f} dB over ceiling. "
            "Codec distortion possible on streaming platforms. Fix before release."
        )
    else:
        return (
            f"Marginal — {measured:.2f} dBTP is {overage:.2f} dB over ceiling. "
            "Unlikely to cause audible distortion but will fail streaming QC checks. "
            "Lower limiter ceiling by 0.2–0.5 dB."
        )


TEMPLATES: dict[str, dict[str, Callable[[dict], str | list[str]]]] = {
    "lufs_too_loud": {
        "what": lambda f: (
            f"Integrated loudness is {f['measured_value']:.1f} LUFS, "
            f"above the {f['genre_name']} ceiling of {f['norm_value']:.1f} LUFS."
        ),
        "why_helps": lambda f: (
            "Streaming loudness normalization is a static gain change: a master above the "
            "platform target is simply turned down. The extra limiting spent getting it that "
            "loud then buys no playback level and only costs transient punch and adds distortion."
        ),
        "suggested_action": lambda f: (
            f"Lower the master bus output by approximately "
            f"{abs(f['measured_value'] - f['norm_value']):.1f} dB, "
            "then re-check integrated loudness with a LUFS meter."
        ),
        "engineer_note": lambda f: (
            f"This mix reads {f['measured_value']:.1f} LUFS — hotter than the "
            f"{f['genre_name']} norm ({f['norm_value']:.1f} LUFS). "
            "On Spotify at -14 LUFS normalization that extra loudness is pulled back anyway, "
            "leaving only the tonal trade-offs of brickwall limiting. "
            "Lower the gain driving the limiter (or its threshold) rather than touching the ceiling."
        ),
        "severity_context": lambda f: (
            f"At {f['severity']:.0f}/100 severity: "
            + ("severe — likely to trigger streaming normalization." if f["severity"] > 70
               else "moderate — may create harshness on some platforms.")
        ),
        "confidence_reason": lambda f: "Integrated LUFS is a deterministic measurement with no estimation uncertainty.",
        "most_likely_cause": lambda f: "Too much input gain driving the master limiter/clipper (gain staging too hot).",
        "trade_off": lambda f: (
            "Reducing loudness means less limiting, which usually restores transient punch; "
            "the trade-off is a master that reads quieter before playback normalization."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} at {f['bpm']:.0f} BPM typically targets {f['norm_value']:.1f} LUFS. "
            + (f"A {f['mood']} mood often benefits from controlled dynamics over raw loudness." if f.get("mood") else "")
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Reference {f['genre_name']} tracks on Spotify typically read "
            f"{f['norm_value']:.1f}–{f['norm_value'] + 2:.1f} LUFS integrated."
        ),
        "alternative_strategies": lambda f: [
            "Use a true-peak limiter ceiling of -1.0 dBTP and reduce input gain instead.",
            "Back off the limiter/clipper drive rather than adding parallel compression - "
            "parallel compression raises density and integrated LUFS, it does not lower it.",
            "Stem mastering — address loudness per stem before the master bus.",
        ],
        "how_to_verify": lambda f: (
            "Re-measure integrated LUFS with a metering plugin (Youlean, LUFS Meter). "
            "The reading should now sit within 1.5 LU of your target. "
            f"Target: {f.get('norm_value', -14):.1f} LUFS."
        ),
    },
    "lufs_too_quiet": {
        "what": lambda f: (
            f"Integrated loudness is {f['measured_value']:.1f} LUFS, "
            f"below the {f['genre_name']} floor of {f['norm_value']:.1f} LUFS."
        ),
        "why_helps": lambda f: (
            "Tracks significantly below genre norm may be perceived as low-energy "
            "when heard back-to-back with genre peers on playlists."
        ),
        "suggested_action": lambda f: (
            f"Raise master bus output by approximately "
            f"{abs(f['measured_value'] - f['norm_value']):.1f} dB. "
            "Use a true-peak limiter to catch peaks at -1.0 dBTP."
        ),
        "engineer_note": lambda f: (
            f"Integrated loudness reads {f['measured_value']:.1f} LUFS — "
            f"softer than the {f['genre_name']} floor of {f['norm_value']:.1f} LUFS. "
            "Check that the master bus is not under-driven and that no silent tail "
            "is inflating the measurement gate."
        ),
        "severity_context": lambda f: (
            f"At {f['severity']:.0f}/100: "
            + ("may reduce playlist competitiveness." if f["severity"] < 50
               else "likely to sound noticeably quiet against genre peers.")
        ),
        "confidence_reason": lambda f: "LUFS is a deterministic measurement.",
        "most_likely_cause": lambda f: (
            "Master bus output gain set conservatively, or a long silent tail depressing the integrated reading."
        ),
        "trade_off": lambda f: "Raising loudness with a limiter may reduce dynamic range and transient clarity.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} at {f['bpm']:.0f} BPM expects energy-forward loudness. "
            "A quieter master may suit downtempo or acoustic variants."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Similar {f['genre_name']} tracks typically read {f['norm_value']:.1f}–{f['norm_value'] + 3:.1f} LUFS."
        ),
        "alternative_strategies": lambda f: [
            "Trim silent tails before the master bus loudness measurement.",
            "Use parallel saturation to add RMS density before limiting.",
            "Check stem gain staging — quiet masters often trace back to a quiet mix bus.",
        ],
        "how_to_verify": lambda f: (
            "Re-measure integrated LUFS after the gain adjustment. "
            "A-B the track against a reference at matched loudness levels to confirm "
            "the energy level is appropriate before increasing loudness further."
        ),
    },
    "lra_too_compressed": {
        "what": lambda f: (
            f"Loudness Range (LRA) is {f['measured_value']:.1f} LU, "
            f"below the {f['genre_name']} minimum of {f['norm_value']:.1f} LU."
        ),
        "why_helps": lambda f: (
            "A compressed LRA reduces punch and section distinction, "
            "making the mix feel flat across its duration."
        ),
        "suggested_action": lambda f: (
            "Reduce bus compression ratio or increase release time. "
            "Consider automation to let quiet sections breathe more."
        ),
        "engineer_note": lambda f: (
            f"LRA at {f['measured_value']:.1f} LU is tighter than the {f['genre_name']} "
            f"minimum of {f['norm_value']:.1f} LU. This often traces to an over-compressed "
            "mix or master bus — check attack/release times and consider parallel compression "
            "to retain transient dynamics."
        ),
        "severity_context": lambda f: (
            f"Severity {f['severity']:.0f}/100. Low LRA in a dynamic genre reduces perceived life."
        ),
        "confidence_reason": lambda f: "LRA is measured per EBU Tech 3342 (K-weighted, gated short-term loudness, 10th-95th percentile spread); deterministic.",
        "most_likely_cause": lambda f: (
            "Over-compressed mix bus, master limiter with slow release, or excessive multiband compression."
        ),
        "trade_off": lambda f: (
            "Releasing compression may require adjusting other mix elements for balance."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"At {f['bpm']:.0f} BPM, {f['genre_name']} relies on rhythmic dynamics — "
            "a compressed LRA flattens the perceived pulse."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Reference {f['genre_name']} tracks typically have LRA {f['norm_value']:.1f}–{f['norm_value'] + 4:.1f} LU."
        ),
        "alternative_strategies": lambda f: [
            "Use clip gain automation to lower verses relative to the chorus.",
            "Swap serial compression for parallel compression on the mix bus.",
            "Check the mastering limiter: less gain reduction and a slower release preserve "
            "transients. Lookahead only lets the limiter react earlier (cleaner but softer "
            "attacks) - it does not restore punch.",
        ],
        "how_to_verify": lambda f: (
            "Check the LRA reading (Loudness Range) in your LUFS meter. "
            "Listen for transients regaining punch — snare crack and pluck attacks should feel more present. "
            "If the mix sounds 'bigger' without being louder, the compression was relieved."
        ),
    },
    "lra_too_dynamic": {
        "what": lambda f: (
            f"Loudness Range (LRA) is {f['measured_value']:.1f} LU, "
            f"above the {f['genre_name']} maximum of {f['norm_value']:.1f} LU."
        ),
        "why_helps": lambda f: (
            "Excessive LRA in a genre that expects tight dynamics may cause quiet sections "
            "to feel disconnected or inaudible on mobile listening."
        ),
        "suggested_action": lambda f: (
            "Apply gentle bus compression (2:1–3:1) to even out dynamics. "
            "Automate vocals/drums up during quieter sections."
        ),
        "engineer_note": lambda f: (
            f"LRA reads {f['measured_value']:.1f} LU — wider than the {f['genre_name']} "
            f"norm of {f['norm_value']:.1f} LU max. Use automation before compression "
            "to preserve musicality."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "LRA is measured per EBU Tech 3342 (K-weighted, gated short-term loudness, 10th-95th percentile spread); deterministic.",
        "most_likely_cause": lambda f: (
            "Unbalanced mix with very loud drops and very quiet intros/outros."
        ),
        "trade_off": lambda f: (
            "Compressing dynamics may reduce the emotional impact of quiet-to-loud transitions."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} at {f['bpm']:.0f} BPM favors consistent energy over wide dynamic swings."
        ),
        "comparison_to_similar_songs": lambda f: f"Genre peers maintain LRA around {f['norm_value']:.1f} LU max.",
        "alternative_strategies": lambda f: [
            "Use volume automation on verses/bridges rather than bus compression.",
            "Use gentle broadband bus compression with a slow release (or upward compression) to lift quiet passages rather than flattening the loud ones.",
        ],
        "how_to_verify": lambda f: (
            "Check LRA after tightening dynamics. Listen for section-to-section energy consistency — "
            "the verse and chorus should feel intentionally different rather than erratically different."
        ),
    },
    "true_peak_over": {
        "what": lambda f: (
            f"True peak is {f['measured_value']:.2f} dBTP, "
            f"exceeding the {f['genre_name']} ceiling of {f['norm_value']:.1f} dBTP."
        ),
        "why_helps": lambda f: (
            "True peak overages cause inter-sample clipping on lossy codecs (AAC, MP3), "
            "introducing audible distortion on all streaming delivery."
        ),
        "suggested_action": lambda f: (
            f"Lower the limiter ceiling to {f['norm_value']:.1f} dBTP. "
            "Use a true-peak limiter (not sample-peak) to prevent inter-sample overs."
        ),
        "engineer_note": lambda f: (
            f"True peak at {f['measured_value']:.2f} dBTP crosses the safety ceiling. "
            "This causes codec clipping on streaming delivery. A true-peak compliant limiter "
            "with a ceiling of -1.0 dBTP is non-negotiable for digital release."
        ),
        "severity_context": lambda f: (
            # Differentiate: overage magnitude determines user urgency
            _tp_severity_context(f.get("measured_value") or 0.0, f.get("norm_value") or -1.0)
        ),
        "confidence_reason": lambda f: "True peak is computed by the desktop app's ITU-R BS.1770-4 4x-oversampled meter; deterministic.",
        "most_likely_cause": lambda f: "Sample-peak limiter used instead of true-peak limiter, or limiter bypassed on export.",
        "trade_off": lambda f: "Correcting true peak often requires reducing master loudness by 0.2–0.5 dB.",
        "bpm_mood_genre_relevance": lambda f: "True peak compliance is required regardless of genre, BPM, or mood.",
        "comparison_to_similar_songs": lambda f: "Many commercial masters exceed -1.0 dBTP, but streaming platforms recommend a -1.0 dBTP ceiling (-2.0 dBTP for very loud masters) to avoid lossy-codec clipping.",
        "alternative_strategies": lambda f: [
            "Use a dedicated true-peak limiter plugin (Fabfilter Pro-L2, iZotope Ozone Maximizer).",
            "Leave more headroom: pull the mix bus down 0.5 dB before the mastering limiter.",
        ],
        "how_to_verify": lambda f: (
            "Enable true peak mode on your limiter or metering plugin. "
            "The true peak ceiling should read no higher than -1.0 dBTP after the fix. "
            "Check using a streaming compliance meter (e.g., Nugen MasterCheck or Loudness Penalty)."
        ),
    },
    "over_compressed": {
        "what": lambda f: (
            f"Crest factor is {f['measured_value']:.1f} dB, "
            "below the 6 dB minimum, indicating heavy dynamic compression."
        ),
        "why_helps": lambda f: (
            "Very low crest factor removes transient definition, which can reduce "
            "groove, punch, and the perceived energy of rhythmic elements."
        ),
        "suggested_action": lambda f: (
            "Reduce bus compressor ratio to 2:1–3:1 and increase attack time to "
            "let transients through. Consider parallel compression to retain punch."
        ),
        "engineer_note": lambda f: (
            f"Crest factor at {f['measured_value']:.1f} dB indicates the signal has been "
            "heavily compressed. Check that it is intentional — not the result of an "
            "over-driven limiter or stacked compressors with cumulative gain reduction exceeding 10 dB."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Crest factor computed from peak-to-RMS ratio; deterministic.",
        "most_likely_cause": lambda f: "Stacked bus compressors, an over-limited master, or a too-fast compressor attack clamping transients.",
        "trade_off": lambda f: "Releasing compression to recover transients may require re-adjusting master loudness.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} at {f['bpm']:.0f} BPM: check if low crest factor is "
            "genre-intentional (EDM, trap) or an artifact of over-processing."
        ),
        "comparison_to_similar_songs": lambda f: "Most genres target crest factor above 6 dB for clear transient definition.",
        "alternative_strategies": lambda f: [
            "Use NY-style parallel compression: blend dry signal with compressed.",
            "Reduce gain reduction on the master limiter by lowering the gain driving into it (the ceiling only sets the output peak).",
        ],
        "how_to_verify": lambda f: (
            "Check crest factor: peak RMS difference should be at least 6 dB. "
            "Listen for transients — a snare hit or kick should have audible snap/crack. "
            "If the mix sounds 'loud but flat', over-compression persists."
        ),
    },
    "under_compressed": {
        "what": lambda f: (
            f"Loudness range (LRA) is {f['measured_value']:.1f} LU, "
            f"above the {f['genre_name']} expected maximum, indicating minimal compression."
        ),
        "why_helps": lambda f: (
            "Tracks with very high dynamic range may sound inconsistent across sections "
            "on consumer playback, with quiet passages too soft and loud sections peaking."
        ),
        "suggested_action": lambda f: (
            "Add light bus compression (2:1–3:1, medium attack/release) to even out dynamics."
        ),
        "engineer_note": lambda f: (
            f"Gated loudness range (LRA) of {f['measured_value']:.1f} LU exceeds genre norms, suggesting "
            f"minimal or no bus compression. In a {f['genre_name']} context, some glue and "
            "dynamic control is expected."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: (
            "Loudness range (LRA) computed per EBU Tech 3342: the 10th-95th percentile spread "
            "of gated short-term loudness, in LU."
        ),
        "most_likely_cause": lambda f: "No bus compression applied, or compressor bypassed.",
        "trade_off": lambda f: "Compressing may reduce the perceived musicality and natural feel of performances.",
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} expects moderate dynamic control at {f['bpm']:.0f} BPM."
        ),
        "comparison_to_similar_songs": lambda f: f"Genre references show tighter dynamics suited to {f['genre_name']} energy.",
        "alternative_strategies": lambda f: [
            "Use VCA-style bus compression for transparent glue.",
            "Automate loud sections down rather than compressing the whole track.",
        ],
        "how_to_verify": lambda f: (
            "Check LRA (Loudness Range). Verify that the loudest and quietest sections "
            "are within the genre-appropriate LRA target. The mix should feel controlled "
            "without sounding lifeless."
        ),
    },
    "clipping_detected": {
        "what": lambda f: "Digital clipping detected: one or more sections exceed 0 dBFS.",
        "why_helps": lambda f: (
            "Clipping introduces harsh distortion artifacts that accumulate through "
            "codec encoding and are audible on all playback systems."
        ),
        "suggested_action": lambda f: (
            "Lower master bus gain or insert a true-peak limiter with -1.0 dBTP ceiling before export."
        ),
        "engineer_note": lambda f: (
            "Clipping detected in the audio file. Even brief clips (1–2 samples) "
            "cause audible artifacts after MP3/AAC encoding. Do not attempt to 'fix' "
            "with saturation — that compounds the distortion."
        ),
        "severity_context": lambda f: "Fixed severity 95/100 — technical defect requiring correction before release.",
        "confidence_reason": lambda f: (
            "Clipping detected from runs of samples at full scale. A master limited to exactly "
            "0 dBFS can also match and inter-sample overs are not counted here, so confirm by ear "
            "and with a true-peak meter."
        ),
        "most_likely_cause": lambda f: "Export level too hot, or limiter bypassed. Master bus gain staging error.",
        "trade_off": lambda f: "Correcting requires either lowering gain or remastering from a lower-level mix.",
        "bpm_mood_genre_relevance": lambda f: "Clipping is genre-agnostic — a hard technical defect.",
        "comparison_to_similar_songs": lambda f: "No commercial release should contain digital clipping.",
        "alternative_strategies": lambda f: [
            "Use a clip restoration tool (iZotope RX Declip).",
            "Lower pre-limiter gain by 2 dB and re-export.",
        ],
        "how_to_verify": lambda f: (
            "Enable clip detection in your DAW and scan the affected region. "
            "The waveform should no longer show flat-topped peaks. "
            "Play the section at high volume and listen for digital distortion."
        ),
    },
    "noise_floor_high": {
        "what": lambda f: (
            f"Noise floor is {f['measured_value']:.1f} dB, above the -50 dB threshold. "
            "Audible noise may be present in quiet sections."
        ),
        "why_helps": lambda f: (
            "A high noise floor is audible during pauses and fades, and can mask "
            "fine detail in quiet passages."
        ),
        "suggested_action": lambda f: (
            "Apply noise reduction to the stem carrying the noise. Gate quiet passages. "
            "Check the recording environment."
        ),
        "engineer_note": lambda f: (
            f"Noise floor at {f['measured_value']:.1f} dB is above the -50 dB threshold. "
            "Common sources: room noise in recordings, plugin noise from analog-emulation "
            "units, or DAW audio engine noise. Identify the stem source before applying "
            "noise reduction to the master."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "Noise floor estimated from quiet frames; heuristic-based.",
        "most_likely_cause": lambda f: "Room noise in vocal/instrument recordings, or plugin self-noise.",
        "trade_off": lambda f: "Aggressive noise reduction can introduce artifacts on program material.",
        "bpm_mood_genre_relevance": lambda f: (
            f"Noise floor issues affect acoustic genres most. In {f['genre_name']}, noise may be "
            "partially masked by a dense arrangement."
        ),
        "comparison_to_similar_songs": lambda f: "Professional releases typically have noise floors below -60 dB.",
        "alternative_strategies": lambda f: [
            "Gate quiet tracks at the stem level before mixing.",
            "Use spectral repair tools to remove tonal noise components.",
        ],
        "how_to_verify": lambda f: (
            "Measure noise floor in silence between sections using a noise floor meter. "
            "The floor should be below -60 dBFS (ideally -80 dBFS or lower for studio material). "
            "Listen in headphones at high volume during a silent passage."
        ),
    },
    "dc_offset": {
        "what": lambda f: (
            f"DC offset: the waveform is centred at {f['measured_value']:+.4f} instead of zero "
            "(mean sample value)."
        ),
        "why_helps": lambda f: (
            "A DC offset wastes headroom on one side of the waveform, makes edits and fades click, "
            "and pushes compressors and limiters asymmetrically."
        ),
        "suggested_action": lambda f: (
            "Remove it with a DC-removal filter or a high-pass at 5-20 Hz on the source track, "
            "or on the mix if every track carries it."
        ),
        "engineer_note": lambda f: (
            "DC usually comes from one track - a faulty interface input, an asymmetric saturator or "
            "a sample library. Fix it there; a high-pass on the master also works but hides the source."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100.",
        "confidence_reason": lambda f: "The mean sample value over the whole file; a direct measurement.",
        "most_likely_cause": lambda f: "One track recorded or processed with an offset.",
        "trade_off": lambda f: "None audible: a 5-20 Hz high-pass removes only the offset and subsonic content.",
        "bpm_mood_genre_relevance": lambda f: "A DC offset is a fault in every genre.",
        "comparison_to_similar_songs": lambda f: "Released masters sit centred on zero (mean under 0.002).",
        "alternative_strategies": lambda f: [
            "Run DC removal on each track that shows an offset before mixing.",
            "High-pass the mix bus at 5-10 Hz if the source cannot be found.",
        ],
        "how_to_verify": lambda f: (
            "Re-measure: the mean sample value should read under 0.002 and the waveform should sit "
            "centred on the zero line."
        ),
    },
    "silent_track": {
        "what": lambda f: "This track is silent or nearly silent across the whole file.",
        "why_helps": lambda f: (
            "A silent track adds nothing to the mix; usually it is a mis-exported stem, a muted "
            "channel or a track bounced from the wrong range."
        ),
        "suggested_action": lambda f: (
            "Check the export: re-bounce the stem from the full song range with the channel unmuted, "
            "or remove the file from the session if it is not needed."
        ),
        "engineer_note": lambda f: (
            "Multitrack sessions are checked per file; a silent file also skews the mix-level averages "
            "built from the stems."
        ),
        "severity_context": lambda f: f"Severity {f.get('severity', 0.0):.0f}/100.",
        "confidence_reason": lambda f: "The file's level over its whole length; a direct measurement.",
        "most_likely_cause": lambda f: "An export of a muted channel or the wrong time range.",
        "trade_off": lambda f: "None.",
        "bpm_mood_genre_relevance": lambda f: "A silent stem is an export fault in every genre.",
        "comparison_to_similar_songs": lambda f: "Every stem of a released multitrack carries signal.",
        "alternative_strategies": lambda f: [
            "Re-export the stem from the DAW with the full song selected.",
            "Remove the file from the session.",
        ],
        "how_to_verify": lambda f: "Re-run the multitrack check: the file should show a measurable level.",
    },
    "ai_artifacts": {
        "what": lambda f: (
            "Potential AI-generation artifacts detected: quantisation noise, formant smearing, "
            "or unnatural spectral patterns characteristic of generative audio models."
        ),
        "why_helps": lambda f: (
            "AI-generated audio artifacts cause listener fatigue and may reduce "
            "perceived authenticity on careful listening."
        ),
        "suggested_action": lambda f: (
            "Re-render the AI-generated stem with higher bit-depth/sample-rate settings. "
            "Apply light saturation to add organic character. Humanize timing and velocity."
        ),
        "engineer_note": lambda f: (
            "Spectral patterns consistent with generative model quantization or frame-boundary "
            "artifacts were detected. This is probabilistic — check around energy transitions "
            "and fade regions for telltale smearing."
        ),
        "severity_context": lambda f: "Fixed severity 80/100 — probabilistic detection, requires manual check.",
        "confidence_reason": lambda f: (
            f"Confidence {f['confidence']:.0%}. Spectral artifact detection is heuristic-based; confirm manually."
        ),
        "most_likely_cause": lambda f: "Generative audio model (vocal synthesis, sample generation) artifacts.",
        "trade_off": lambda f: "Treating AI artifacts may introduce other processing artifacts.",
        "bpm_mood_genre_relevance": lambda f: (
            f"In {f['genre_name']}, AI-generated elements may or may not be audible depending "
            "on arrangement density."
        ),
        "comparison_to_similar_songs": lambda f: "Human-performed source material avoids this class of artifact.",
        "alternative_strategies": lambda f: [
            "Replace AI-generated stems with performed or sample-library alternatives.",
            "Apply spectral repair in RX to smooth frame boundaries.",
        ],
        "how_to_verify": lambda f: (
            "Listen in headphones at moderate volume to sustained notes and decays. "
            "Formant smearing sounds like a 'watery' or 'phasy' quality on held vowels. "
            "If artifacts persist, check the AI processing chain and re-generate the affected section."
        ),
    },
    "crest_above_genre_target": {
        "what": lambda f: (
            f"Crest factor is {f['measured_value']:.1f} dB - above the {f['genre_name']} "
            f"target of {f['norm_value']:.1f} dB (+/-3 dB tolerance)."
        ),
        "why_helps": lambda f: (
            "Crest factor (peak-to-RMS distance) is part of a genre's dynamic character. A dense, "
            "heavily-limited style reads wrong with this much headroom left in: it sounds loose and "
            "small next to references even after loudness normalisation."
        ),
        "suggested_action": lambda f: (
            f"Add gentle bus compression or limiting to bring crest factor down toward "
            f"{f['norm_value']:.1f} dB - 1-3 dB of gain reduction at a time, re-measuring after each step."
        ),
        "engineer_note": lambda f: (
            "This is a genre-character check, not a loudness-safety one. Reach for glue compression "
            "(2:1, slow attack) before a limiter so transients are shaped rather than clipped off."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 - a stylistic deviation, not a defect.",
        "confidence_reason": lambda f: (
            "Crest factor is a deterministic measurement; the genre target is a catalogue norm, "
            "so the judgement is only as good as the genre match."
        ),
        "most_likely_cause": lambda f: "Little or no bus compression/limiting for a genre that expects a dense master.",
        "trade_off": lambda f: (
            "Lowering crest factor trades transient snap for density; overdo it and the mix flattens "
            "and pumps."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} productions typically sit near {f['norm_value']:.1f} dB crest factor."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Reference {f['genre_name']} masters cluster within about 3 dB of "
            f"{f['norm_value']:.1f} dB crest factor."
        ),
        "alternative_strategies": lambda f: [
            "Parallel compression on the drum bus raises density without squashing the mix bus.",
            "A soft clipper before the limiter shaves peaks transparently.",
            "If the dynamics are an intentional choice for this track, keep them - this is a taste call.",
        ],
        "how_to_verify": lambda f: (
            "Re-measure crest factor after the change: it should sit within +/-3 dB of the genre "
            "target. A/B against a reference at matched loudness and check the punch survives."
        ),
    },
    "crest_below_genre_target": {
        "what": lambda f: (
            f"Crest factor is {f['measured_value']:.1f} dB - below the {f['genre_name']} "
            f"target of {f['norm_value']:.1f} dB (+/-3 dB tolerance)."
        ),
        "why_helps": lambda f: (
            "A dynamic or airy genre reads wrong when squashed flat: transients lose their impact and "
            "the mix tires the ear. Loudness normalisation removes any loudness advantage the extra "
            "limiting bought."
        ),
        "suggested_action": lambda f: (
            f"Ease off bus compression/limiting to bring crest factor up toward {f['norm_value']:.1f} dB - "
            "lower the limiter input gain or raise the compressor threshold first."
        ),
        "engineer_note": lambda f: (
            "This is a genre-relative check; the absolute 6 dB over-compressed floor still applies on "
            "its own. Check each compression stage's gain reduction - stacked stages add up."
        ),
        "severity_context": lambda f: f"Severity {f['severity']:.0f}/100 - a stylistic deviation, not a defect.",
        "confidence_reason": lambda f: (
            "Crest factor is a deterministic measurement; the genre target is a catalogue norm, "
            "so the judgement is only as good as the genre match."
        ),
        "most_likely_cause": lambda f: "Heavy limiting or several stacked compression stages on the mix/master bus.",
        "trade_off": lambda f: (
            "Restoring crest factor lowers integrated loudness before normalisation; on streaming "
            "platforms that costs nothing, but a hot-master client may notice."
        ),
        "bpm_mood_genre_relevance": lambda f: (
            f"{f['genre_name']} productions typically keep about {f['norm_value']:.1f} dB of crest factor."
        ),
        "comparison_to_similar_songs": lambda f: (
            f"Reference {f['genre_name']} masters cluster within about 3 dB of "
            f"{f['norm_value']:.1f} dB crest factor."
        ),
        "alternative_strategies": lambda f: [
            "Move gain reduction from the master limiter to individual buses so peaks survive the master.",
            "Use a slower attack on the bus compressor to let transients through.",
            "If the density is the intended sound, keep it - this is a taste call.",
        ],
        "how_to_verify": lambda f: (
            "Re-measure crest factor after the change: it should sit within +/-3 dB of the genre "
            "target. Listen for drum transients returning without the mix sounding thin."
        ),
    },
}
