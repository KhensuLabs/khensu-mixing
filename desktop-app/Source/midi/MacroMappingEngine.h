// desktop-app/Source/midi/MacroMappingEngine.h
// Pure mapping logic for MIDI/OSC macro control: which hardware control or
// OSC address drives which macro, over what range and response curve, plus
// "learn" mode (arm a macro, move a control, it binds). No I/O - the
// MacroControlRouter feeds it decoded events and applies its output.
// Unit-tested in tests/ui/MacroMappingEngineTest.cpp.
#pragma once
#include <JuceHeader.h>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace khensu {

class MacroMappingEngine {
  public:
    enum class SourceType { MidiCC, MidiNote, MidiPitchBend, Osc };
    enum class Curve { Linear, Exponential, Logarithmic, SCurve };

    struct Source {
        SourceType type = SourceType::MidiCC;
        int channel = -1;        ///< 0-15, or -1 for any channel
        int number = 0;          ///< CC number / note number (unused for pitch bend / OSC)
        juce::String oscAddress; ///< e.g. "/khensu/macro/1" (OSC only)

        bool sameControlAs(const Source& o) const;
        juce::String describe() const;
    };

    struct Mapping {
        Source source;
        juce::String target;     ///< macro name, e.g. "Presence"
        double minValue = 0.0;   ///< macro value at control minimum
        double maxValue = 100.0; ///< macro value at control maximum
        Curve curve = Curve::Linear;
        bool invert = false;

        /// Maps a normalised 0..1 control position to the macro's value.
        double apply(double normalised) const;
    };

    struct Output {
        juce::String target;
        double value = 0.0;
    };

    explicit MacroMappingEngine(juce::StringArray targets = {});

    const juce::StringArray& targets() const noexcept { return targets_; }
    const std::vector<Mapping>& mappings() const noexcept { return mappings_; }

    /// Adds or replaces: one control drives at most one macro, so an existing
    /// mapping for the same control is replaced. Unknown targets are refused.
    bool setMapping(const Mapping& m);
    bool removeMapping(size_t index);
    bool updateMapping(size_t index, const Mapping& m);
    void clear() { mappings_.clear(); }

    /// Learn mode: the next incoming control binds to `target` (keeping the
    /// target's previous range/curve if it had a mapping).
    bool armLearn(const juce::String& target);
    void cancelLearn() noexcept { learnTarget_.reset(); }
    std::optional<juce::String> learnTarget() const { return learnTarget_; }

    /// Feeds one control movement (`normalised` in 0..1). While learning it
    /// binds and returns the first value; otherwise returns every mapped
    /// macro's new value (normally zero or one).
    std::vector<Output> handle(const Source& source, double normalised);

    /// Persistence as a JSON-compatible var (array of objects).
    juce::var toVar() const;
    /// Replaces the mappings from toVar() output; skips malformed/unknown rows.
    /// Returns how many were loaded.
    int fromVar(const juce::var& v);

    static juce::String curveName(Curve c);
    static Curve curveFromName(const juce::String& s);
    static double normaliseMidi(std::uint32_t value, std::uint8_t valueBits);
    /// OSC args: floats are taken as 0..1, ints as 0..127.
    static double normaliseOsc(const juce::var& arg);

  private:
    juce::StringArray targets_;
    std::vector<Mapping> mappings_;
    std::optional<juce::String> learnTarget_;
};

} // namespace khensu
