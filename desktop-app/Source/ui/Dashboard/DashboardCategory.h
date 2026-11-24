#pragma once
#include <JuceHeader.h>
#include <map>
#include <string>

namespace khensu::dashboard_category {

/// The Zone 3 category tabs, in order. The names are data (compared with
/// forPanel()), so they are not translated.
inline const char* const kTabs[] = {"Essentials", "All",           "Loudness", "Spectrum", "Spatial",
                                    "Song & Structure", "Song insights", "Stems",    "Quality",  "Advanced"};

/// CUT-3: speculative, not actionable - shown only on their own tab.
inline constexpr const char* kSongInsights = "Song insights";

/// OVH-7: research, forensic and experimental cards - on their own tab, and
/// in "All" only when the user turns on "Show advanced cards" (the
/// dashboard_show_advanced preference).
///
/// The rule (decides every card; keep it when adding one):
///   - A card stays in its workspace (Essentials / All) when it shows a
///     finding that has measured evidence on released music
///     (resources/calibration/finding_evidence.json; the card is
///     essentials::panelForFinding of that finding), or when it is a core
///     measurement every session has: loudness, tone, stereo and phase, the
///     song's facts, the reference, the mix notes.
///   - A card goes to Advanced when it is experimental (FeatureMaturity.h -
///     not yet checked on real material) or when it is a research or
///     forensic view that backs no finding with evidence and that no core
///     workflow needs: depth, reverb tails, groove timing, drum patterns, the
///     harmony timeline, tuning reference, benchmarks and the genre
///     distribution.
///   - The CUT-3 song insights keep their own tab (already out of "All").
///   - A Dashboard group (CardGroup) moves as one unit, so a group holding
///     an evidence-backed card stays (Provenance & source keeps Artifacts).
/// Nothing is deleted: Advanced is a tab, and the toggle shows it in "All".
inline constexpr const char* kAdvanced = "Advanced";

/// One workspace per Zone 3 card; every card maps to exactly one category
/// other than "Essentials" / "All" (unknown ids fall in "Quality").
inline juce::String forPanel(const std::string& id)
{
    static const std::map<std::string, const char*> kCategory = {
        // CON-2..8 group cards
        {"stereo_mono", "Spatial"},
        {"tone", "Spectrum"},
        {"loudness_delivery", "Loudness"},
        {"provenance_source", "Quality"},
        {"stems", "Stems"},
        {"loudness_arc", "Loudness"},
        {"loudness_curve", "Loudness"},
        {"translation", "Loudness"},
        {"streaming_norm", "Loudness"},
        {"source_forensics", "Quality"},
        {"tuning_ref", "Advanced"},       // OVH-7: research view, no finding
        {"mono_compat", "Spatial"},
        {"genre_fit", "Quality"}, // NEW-3
        {"mid_side_spec", "Spectrum"},
        {"phase_heatmap", "Spatial"},
        {"stem_energy_tl", "Stems"},
        {"ref_spec_delta", "Quality"},
        {"recreate_sound", "Song insights"},
        {"spectrogram", "Spectrum"},
        {"eq_curve", "Spectrum"},
        {"tonal_balance", "Spectrum"},
        {"heaviness", "Song insights"},
        {"phase", "Spatial"},
        {"spatial_audio", "Advanced"}, // OVH-7: experimental (was Spatial)
        {"depth_ladder", "Advanced"}, // OVH-7: research view, no finding (was Spatial)
        {"pan_balance", "Spatial"},
        {"sub_bass_phase", "Spatial"},
        {"reverb_tail", "Advanced"}, // OVH-7: research view, no finding (was Spatial)
        {"groove_timing", "Advanced"}, // OVH-7: research view, no finding (was Song & Structure)
        {"harmony_timeline", "Advanced"}, // OVH-7: research view, no finding (was Song & Structure)
        {"stem_phase", "Stems"},
        {"song_info", "Song & Structure"},
        {"vocal_info", "Song & Structure"},
        {"hooks", "Song insights"},
        {"memorability", "Song insights"},
        {"emotional_arc", "Song insights"},
        {"chords", "Song & Structure"},
        {"structural", "Advanced"}, // OVH-7: experimental (was Song & Structure)
        {"arrangement", "Song & Structure"},
        {"section_readiness", "Advanced"}, // OVH-7: experimental (was Song & Structure)
        {"drum_pattern", "Advanced"}, // OVH-7: research view, no finding (was Song & Structure)
        {"taxonomy_energy", "Stems"},
        {"taxonomy_masking", "Stems"},
        {"artifact", "Quality"},
        {"mix_notes", "Quality"},
        {"reference", "Quality"},
        {"benchmark", "Advanced"}, // OVH-7: research view, no finding (was Quality)
        {"genre_dist", "Advanced"}, // OVH-7: research view, no finding (was Quality)
        {"source_triage", "Quality"},
        {"signal_integrity", "Quality"},
        {"ai_artifacts", "Quality"},
    };
    auto it = kCategory.find(id);
    return it != kCategory.end() ? juce::String(it->second) : juce::String("Quality");
}

/// True for a card on the Advanced tab (OVH-7).
inline bool isAdvanced(const std::string& id) { return forPanel(id) == kAdvanced; }

/// Whether a card shows on a category tab (Essentials has its own rule).
/// "All" shows every card except the song insights (CUT-3) and - unless
/// `showAdvanced` - the advanced cards (OVH-7).
inline bool shownInTab(const juce::String& tab, const std::string& id, bool showAdvanced = false)
{
    const auto category = forPanel(id);
    if (tab == "All") return category != kSongInsights && (showAdvanced || category != kAdvanced);
    return category == tab;
}

} // namespace khensu::dashboard_category
