#pragma once
#include <JuceHeader.h>
#include <map>
#include <string>

namespace khensu::dashboard_category {

/// The Zone 3 category tabs, in order. The names are data (compared with
/// forPanel()), so they are not translated.
inline const char* const kTabs[] = {"Essentials", "All", "Loudness", "Spectrum", "Spatial",
                                    "Song & Structure", "Song insights", "Stems", "Quality"};

/// CUT-3: speculative, not actionable - shown only on their own tab.
inline constexpr const char* kSongInsights = "Song insights";

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
        {"tuning_ref", "Song & Structure"},
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
        {"spatial_audio", "Spatial"},
        {"depth_ladder", "Spatial"},
        {"pan_balance", "Spatial"},
        {"sub_bass_phase", "Spatial"},
        {"reverb_tail", "Spatial"},
        {"groove_timing", "Song & Structure"},
        {"harmony_timeline", "Song & Structure"},
        {"stem_phase", "Stems"},
        {"song_info", "Song & Structure"},
        {"vocal_info", "Song & Structure"},
        {"hooks", "Song insights"},
        {"memorability", "Song insights"},
        {"emotional_arc", "Song insights"},
        {"chords", "Song & Structure"},
        {"structural", "Song & Structure"},
        {"arrangement", "Song & Structure"},
        {"section_readiness", "Song & Structure"},
        {"drum_pattern", "Song & Structure"},
        {"taxonomy_energy", "Stems"},
        {"taxonomy_masking", "Stems"},
        {"artifact", "Quality"},
        {"mix_notes", "Quality"},
        {"reference", "Quality"},
        {"benchmark", "Quality"},
        {"genre_dist", "Quality"},
        {"source_triage", "Quality"},
        {"signal_integrity", "Quality"},
        {"ai_artifacts", "Quality"},
    };
    auto it = kCategory.find(id);
    return it != kCategory.end() ? juce::String(it->second) : juce::String("Quality");
}

/// Whether a card shows on a category tab (Essentials has its own rule).
/// "All" shows every card except the song insights (CUT-3).
inline bool shownInTab(const juce::String& tab, const std::string& id)
{
    const auto category = forPanel(id);
    if (tab == "All") return category != kSongInsights;
    return category == tab;
}

} // namespace khensu::dashboard_category
