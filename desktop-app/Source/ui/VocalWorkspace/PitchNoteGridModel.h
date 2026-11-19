// desktop-app/Source/ui/VocalWorkspace/PitchNoteGridModel.h
// Pure editing model behind the graphical pitch-correction note grid.
// Parses POST /analyze/pitch_curve, holds the user's per-note corrections,
// and emits the corrections list for POST /render/pitch_correct. No JUCE
// components - unit-tested directly in tests/ui/VocalEditingModelsTest.cpp.
#pragma once
#include <JuceHeader.h>
#include "MixTuningReference.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>
#include <khensu/analysis/MusicTheory.h>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

class PitchNoteGridModel {
  public:
    struct Note {
        int id = 0;
        double startSec = 0.0, endSec = 0.0;
        double midi = 0.0;  ///< detected mean (fractional) pitch, on the response's reference grid
        int targetMidi = 0; ///< nearest semitone on that grid (MIDI 69 == tuning().gridHz)
        double shift = 0.0; ///< user correction in semitones (0 = untouched)
        /// True once the user moved this note by hand (drag / nudge / snap):
        /// the Retune slider then leaves it alone (UIX-13).
        bool manual = false;

        double correctedMidi() const noexcept { return midi + shift; }
        double durationSec() const noexcept { return endSec - startSec; }
        /// Detected pitch's deviation from its nearest grid semitone, cents.
        double detectedCents() const noexcept { return (midi - targetMidi) * 100.0; }
        /// Corrected pitch's deviation from ITS nearest semitone, cents.
        double correctedCents() const noexcept
        {
            const double c = correctedMidi();
            return (c - std::round(c)) * 100.0;
        }
    };

    struct Correction {
        double startSec = 0.0, endSec = 0.0, shiftSemitones = 0.0;
    };

    static constexpr double kMaxShift = 12.0;

    /// Replaces the model from a /analyze/pitch_curve response. Returns false
    /// (leaving the model empty) when the payload has no usable notes/curve.
    bool loadFromResponse(const juce::var& v)
    {
        clear();
        durationSec_ = static_cast<double>(v[api::fields::PitchCurveResponse::duration_sec]);
        hopSec_ = static_cast<double>(v[api::fields::PitchCurveResponse::hop_sec]);
        if (auto* curve = v[api::fields::PitchCurveResponse::f0_midi].getArray()) {
            curve_.reserve(static_cast<size_t>(curve->size()));
            for (const auto& f : *curve)
                curve_.push_back(f.isVoid() || f.isUndefined() ? std::nullopt
                                                               : std::optional<double>(static_cast<double>(f)));
        }
        if (auto* arr = v[api::fields::PitchCurveResponse::notes].getArray()) {
            for (const auto& nv : *arr) {
                Note n;
                n.id = static_cast<int>(nv[api::fields::PitchCurveNote::id]);
                n.startSec = static_cast<double>(nv[api::fields::PitchCurveNote::start_sec]);
                n.endSec = static_cast<double>(nv[api::fields::PitchCurveNote::end_sec]);
                n.midi = static_cast<double>(nv[api::fields::PitchCurveNote::midi]);
                n.targetMidi = static_cast<int>(nv[api::fields::PitchCurveNote::target_midi]);
                if (n.endSec > n.startSec) notes_.push_back(n);
            }
        }
        tuning_ = TuningReading::fromResponse(v);
        std::sort(notes_.begin(), notes_.end(), [](const Note& a, const Note& b) { return a.startSec < b.startSec; });
        return !notes_.empty();
    }

    void clear()
    {
        notes_.clear();
        curve_.clear();
        undo_.clear();
        durationSec_ = hopSec_ = 0.0;
        tuning_ = {};
    }

    // --- Key awareness (UIX-13) ---------------------------------------------
    /// The session's detected key ("A minor", "Bb major"); unparseable /
    /// "unknown" clears it. Drives note names and snap-to-key.
    void setKey(const std::string& keyName) { key_ = theory::parseKey(keyName); }
    const std::optional<theory::ParsedKey>& key() const noexcept { return key_; }
    /// Snap targets (Retune, 's', Up/Down) use the key's scale instead of
    /// the chromatic semitone grid. Ignored while no key is known.
    void setSnapToKey(bool on) noexcept { snapToKey_ = on; }
    bool snapToKey() const noexcept { return snapToKey_ && key_.has_value(); }

    /// Where Retune / 's' pull note `n`: its nearest semitone on the grid, or
    /// with snap-to-key the nearest pitch of the key's scale (minor keys
    /// include the raised leading tone).
    int snapTargetFor(const Note& n) const
    {
        if (snapToKey()) return theory::nearestScaleMidi(n.midi, *key_);
        return n.targetMidi;
    }

    // --- Undo (UIX-13) -----------------------------------------------------
    /// Records the current corrections; call before each user edit. Keeps
    /// the last kUndoDepth states.
    void pushUndo()
    {
        Snapshot snap;
        snap.reserve(notes_.size());
        for (const auto& n : notes_)
            snap.push_back({n.shift, n.manual});
        undo_.push_back(std::move(snap));
        if (undo_.size() > kUndoDepth) undo_.erase(undo_.begin());
    }
    bool canUndo() const noexcept { return !undo_.empty(); }
    /// Restores the corrections recorded by the last pushUndo().
    bool undo()
    {
        if (undo_.empty()) return false;
        const auto snap = std::move(undo_.back());
        undo_.pop_back();
        if (snap.size() != notes_.size()) return false;
        for (size_t i = 0; i < notes_.size(); ++i) {
            notes_[i].shift = snap[i].shift;
            notes_[i].manual = snap[i].manual;
        }
        return true;
    }

    const std::vector<Note>& notes() const noexcept { return notes_; }
    const std::vector<std::optional<double>>& curve() const noexcept { return curve_; }
    double durationSec() const noexcept { return durationSec_; }
    double hopSec() const noexcept { return hopSec_; }
    bool empty() const noexcept { return notes_.empty(); }
    /// Which concert A the notes/targets are on, plus the off-grid caveat.
    const TuningReading& tuning() const noexcept { return tuning_; }

    /// Classic "retune amount": pulls every note `amount` (0..1) of the way
    /// to its snap target ON THE RESPONSE'S GRID - the mix's concert A when
    /// the editor requested it (AUDIT-016), so an A=432 track is tuned to
    /// 432, not pulled 32 cents sharp to 440. 1.0 = hard-tuned. Notes the
    /// user moved by hand keep their manual correction (UIX-13: the slider
    /// used to overwrite every manual move).
    void retuneAll(double amount)
    {
        amount = juce::jlimit(0.0, 1.0, amount);
        for (auto& n : notes_)
            if (!n.manual) n.shift = clampShift((snapTargetFor(n) - n.midi) * amount);
    }

    /// Moves a note so its corrected pitch lands on `targetMidi` exactly
    /// (a manual move: Retune leaves it alone afterwards).
    bool snapNoteTo(size_t index, int targetMidi)
    {
        if (index >= notes_.size()) return false;
        notes_[index].shift = clampShift(targetMidi - notes_[index].midi);
        notes_[index].manual = true;
        return true;
    }

    /// Sets a note's correction directly (e.g. restoring the pre-drag state
    /// when a drag returns to where it started).
    bool setShift(size_t index, double shift, bool manual)
    {
        if (index >= notes_.size()) return false;
        notes_[index].shift = clampShift(shift);
        notes_[index].manual = manual;
        return true;
    }

    /// Snaps a note to its snap target (nearest semitone, or the nearest
    /// scale pitch with snap-to-key).
    bool snapNoteToTarget(size_t index)
    {
        if (index >= notes_.size()) return false;
        return snapNoteTo(index, snapTargetFor(notes_[index]));
    }

    /// Moves a note by `steps` relative to its current snapped position
    /// (keyboard Up/Down): whole semitones, or scale steps with snap-to-key.
    bool nudgeNote(size_t index, int steps)
    {
        if (index >= notes_.size()) return false;
        if (snapToKey() && steps != 0) {
            double at = notes_[index].correctedMidi();
            int target = static_cast<int>(std::lround(at));
            for (int i = 0; i < std::abs(steps); ++i) {
                target = theory::stepScaleMidi(at, *key_, steps);
                at = static_cast<double>(target);
            }
            return snapNoteTo(index, target);
        }
        const int current = static_cast<int>(std::lround(notes_[index].correctedMidi()));
        return snapNoteTo(index, current + steps);
    }

    bool resetNote(size_t index)
    {
        if (index >= notes_.size()) return false;
        notes_[index].shift = 0.0;
        notes_[index].manual = false;
        return true;
    }

    void resetAll()
    {
        for (auto& n : notes_) {
            n.shift = 0.0;
            n.manual = false;
        }
    }

    /// Index of the note under `timeSec`, or -1.
    int noteAt(double timeSec) const noexcept
    {
        for (size_t i = 0; i < notes_.size(); ++i)
            if (timeSec >= notes_[i].startSec && timeSec <= notes_[i].endSec) return static_cast<int>(i);
        return -1;
    }

    /// Notes the user actually moved (|shift| >= 0.01 st), ready for render.
    std::vector<Correction> corrections() const
    {
        std::vector<Correction> out;
        for (const auto& n : notes_)
            if (std::abs(n.shift) >= 0.01) out.push_back({n.startSec, n.endSec, n.shift});
        return out;
    }

    juce::var correctionsAsVar() const
    {
        juce::Array<juce::var> arr;
        for (const auto& c : corrections()) {
            auto* o = new juce::DynamicObject();
            o->setProperty("start_sec", c.startSec);
            o->setProperty("end_sec", c.endSec);
            o->setProperty("shift_semitones", c.shiftSemitones);
            arr.add(juce::var(o));
        }
        return arr;
    }

    /// Visible MIDI range with a semitone of headroom, covering both detected
    /// and corrected pitches. Falls back to C3..C5 when empty.
    juce::Range<int> displayRange() const
    {
        if (notes_.empty()) return {48, 72};
        double lo = 1e9, hi = -1e9;
        for (const auto& n : notes_) {
            lo = std::min({lo, n.midi, n.correctedMidi()});
            hi = std::max({hi, n.midi, n.correctedMidi()});
        }
        const int l = static_cast<int>(std::floor(lo)) - 2;
        const int h = static_cast<int>(std::ceil(hi)) + 2;
        return {l, std::max(h, l + 12)};
    }

    /// Note name without a key: sharps (C4 = 60).
    static juce::String midiName(int midi) { return juce::String(theory::midiNoteName(midi, std::nullopt)); }

    /// Key-aware note name (THY-14): spelled from its scale degree in the
    /// session key (Bb4, not A#4, in F major; the leading tone C#5 in D
    /// minor); sharps when no key is known.
    juce::String noteName(int midi) const { return juce::String(theory::midiNoteName(midi, key_)); }

    static constexpr size_t kUndoDepth = 64;

  private:
    static double clampShift(double s) noexcept { return juce::jlimit(-kMaxShift, kMaxShift, s); }

    struct NoteState {
        double shift = 0.0;
        bool manual = false;
    };
    using Snapshot = std::vector<NoteState>;
    std::vector<Snapshot> undo_;
    std::optional<theory::ParsedKey> key_;
    bool snapToKey_ = false;

    std::vector<Note> notes_;
    std::vector<std::optional<double>> curve_;
    double durationSec_ = 0.0, hopSec_ = 0.0;
    TuningReading tuning_;
};

} // namespace khensu
