// desktop-app/Source/ui/Dashboard/ChordsPanel.h
#pragma once
#include <JuceHeader.h>
#include "../shared/PanelCard.h"
#include "../shared/ButtonRole.h"
#include "ChordChart.h"
#include <memory>
#include <optional>
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/analysis/MusicTheory.h>
#include <algorithm>
#include <vector>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu {

class ChordsPanel final : public PanelCard {
  public:
    explicit ChordsPanel(PanelCardController& c) : PanelCard(c, "CHORD PROGRESSION")
    {
        setExplainer(TRANS("Chords"), TRANS("Predicted chord progression estimated from note events and downbeat timing."));

        textBox_.setMultiLine(true);
        textBox_.setReadOnly(true);
        textBox_.setCaretVisible(false);
        textBox_.setScrollbarsShown(true);
        textBox_.setColour(juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
        textBox_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        textBox_.setFont(
            juce::Font(juce::FontOptions{juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain}));
        addAndMakeVisible(textBox_);

        // Chart / MIDI export of the service's chord analysis.
        copyButton_.setTooltip(TRANS("Copy a bar-aligned chord chart (lead-sheet text) to the clipboard."));
        midiButton_.setTooltip(TRANS("Save the chords as a MIDI file with tempo and time signature."));
        copyButton_.onClick = [this] { copyChart(); };
        midiButton_.onClick = [this] { exportMidi(); };
        addChildComponent(copyButton_);
        addChildComponent(midiButton_);
    }

    void resized() override
    {
        auto b = bodyArea();
        if (copyButton_.isVisible()) {
            auto row = b.removeFromTop(KhensuTheme::buttonHSmall);
            midiButton_.setBounds(row.removeFromRight(150));
            row.removeFromRight(6);
            copyButton_.setBounds(row.removeFromRight(110));
            b.removeFromTop(4);
        }
        textBox_.setBounds(b);
    }

    void setData(const AnalysisResult& r)
    {
        textBox_.setText(buildText(r), false);
        chords_.reset();
        if (r.chord_analysis.has_value() && !r.chord_analysis->chords.empty()) chords_ = r.chord_analysis;
        downbeats_.clear();
        // The parsed var must outlive the loop: getArray() points into it (a
        // temporary in the `if` condition was freed before the loop read it).
        if (!r.python_data.downbeat_times_json.empty()) {
            const juce::var parsed = juce::JSON::parse(juce::String(r.python_data.downbeat_times_json));
            if (const auto* arr = parsed.getArray())
                for (const auto& t : *arr)
                    downbeats_.push_back(static_cast<double>(t));
        }
        timeSignature_ = juce::String(r.python_data.time_signature);
        bpm_ = r.python_data.bpm_detected;
        copyButton_.setVisible(chords_.has_value());
        midiButton_.setVisible(chords_.has_value());
        resized();
    }

    /// The panel's text for `r` - pure, so the chord chart is unit-testable.
    static juce::String buildText(const AnalysisResult& r)
    {
        // Prefer the service's chromagram chord analysis (/analyze/chords) -
        // it was fetched on every run but previously had no consumer while
        // this panel re-derived chords locally (duplicated logic that could
        // disagree with it). The local detector below remains as fallback.
        if (r.chord_analysis.has_value() && !r.chord_analysis->chords.empty()) return serviceChart(*r.chord_analysis);

        if (r.python_data.pitch_notes_json.empty() || r.python_data.pitch_notes_json == "[]") {
            // No data: say so. A hard-coded "Demo Mode" progression used to
            // render here, indistinguishable at a glance from real analysis.
            return TRANS("No chord analysis yet - it appears once deep analysis completes.");
        }
        return localChart(r.python_data);
    }

  private:
    juce::TextEditor textBox_;
    juce::TextButton copyButton_{TRANS("Copy chart")};
    juce::TextButton midiButton_{TRANS("Export chords .mid")};
    std::unique_ptr<juce::FileChooser> chooser_;
    std::optional<ChordAnalysis> chords_; ///< the service chord analysis behind the export buttons
    std::vector<double> downbeats_;
    juce::String timeSignature_;
    double bpm_ = 0.0;

    juce::String songTitle() const
    {
        if (const auto* s = controller_.getActiveSession(); s != nullptr && !s->name.empty())
            return juce::String(s->name);
        return "chords";
    }

    chordchart::ChartContext chartContext() const
    {
        chordchart::ChartContext ctx;
        ctx.title = songTitle();
        if (chords_) {
            ctx.key = juce::String(chords_->key + " " + chords_->mode).trim();
            ctx.camelot = juce::String(chords_->camelot);
        }
        ctx.timeSignature = chordchart::parseTimeSignature(timeSignature_);
        ctx.bpm = bpm_;
        ctx.downbeats = downbeats_;
        if (const auto* s = controller_.getActiveSession(); s != nullptr && !s->files.empty())
            ctx.durationSec = s->files.front().duration_seconds;
        return ctx;
    }

    void copyChart()
    {
        if (!chords_) return;
        juce::SystemClipboard::copyTextToClipboard(chordchart::buildChart(*chords_, chartContext()));
        copyButton_.setButtonText(TRANS("Copied"));
        juce::Component::SafePointer<ChordsPanel> self(this);
        juce::Timer::callAfterDelay(1500, [self] {
            if (self != nullptr) self->copyButton_.setButtonText(TRANS("Copy chart"));
        });
    }

    void exportMidi()
    {
        if (!chords_) return;
        const auto midi = chordchart::buildMidi(*chords_, chartContext());
        const auto defaultName = juce::File::createLegalFileName(songTitle() + "_chords.mid");
        chooser_ = std::make_unique<juce::FileChooser>(
            TRANS("Export chords as MIDI"),
            juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(defaultName), "*.mid");
        juce::Component::SafePointer<ChordsPanel> self(this);
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                  juce::FileBrowserComponent::warnAboutOverwriting,
                              [self, midi](const juce::FileChooser& fc) {
                                  if (self == nullptr) return;
                                  auto file = fc.getResult();
                                  if (file == juce::File()) return;
                                  file = file.withFileExtension(".mid");
                                  file.deleteFile();
                                  bool ok = false;
                                  {
                                      juce::FileOutputStream out(file);
                                      ok = out.openedOk() && midi.writeTo(out);
                                      out.flush();
                                  }
                                  self->midiButton_.setButtonText(ok ? TRANS("Saved .mid") : TRANS("Export failed"));
                                  self->midiButton_.setTooltip(ok ? file.getFullPathName()
                                                                  : TRANS("Could not write ") + file.getFullPathName());
                                  juce::Timer::callAfterDelay(2500, [self] {
                                      if (self != nullptr)
                                          self->midiButton_.setButtonText(TRANS("Export chords .mid"));
                                  });
                              });
    }

    static juce::String serviceChart(const ChordAnalysis& ca)
    {
        juce::String text;
        const bool keyKnown = !ca.key.empty() && ca.key != "unknown";
        text << TRANS("Key: ") << (keyKnown ? juce::String(ca.key) + " " + juce::String(ca.mode) : TRANS("unknown"));
        // Camelot / relative key / confidence (AUDIT-052).
        if (!ca.camelot.empty()) text << "  [" << juce::String(ca.camelot) << "]";
        if (!ca.relative_key.empty()) text << "  " << TRANS("relative") << " " << juce::String(ca.relative_key);
        text << "  (" << TRANS("confidence") << " " << juce::String(juce::roundToInt(ca.key_confidence * 100.0f))
             << "%" << (ca.key_confidence < 0.3f ? juce::String(", ") + TRANS("low - treat the key as a guess") : "")
             << ")\n\n";

        // THY-05: label each non-diatonic chord by its function - an applied
        // chord (D7 = V7/V in C) is "secondary", only modal mixture (bVII,
        // bVI, iv in major) is "borrowed"; everything else is "chromatic".
        // Every chord used to read "(borrowed)".
        const auto key = keyKnown ? theory::parseKey(ca.key, ca.mode) : std::nullopt;
        juce::String lastChord;
        for (const auto& ev : ca.chords) {
            // The bridge maps the service's "N" class to chart notation; map
            // here too so a raw "N" can never reach the screen.
            const juce::String chord = ev.chord == "N" ? juce::String("N.C.") : juce::String(ev.chord);
            if (chord == lastChord) continue; // collapse repeats
            text << juce::String::formatted("%6.1fs  ", ev.time_sec) << chord.paddedRight(' ', 8);
            if (!ev.roman.empty()) text << juce::String(ev.roman);
            const juce::String label = functionLabel(ev, key);
            if (label.isNotEmpty()) text << "  (" << label << ")";
            text << "\n";
            lastChord = chord;
        }
        return text;
    }

    /// Display label for a chord's function; "" for diatonic chords, "N" and
    /// an unknown key. Uses the shared-core port of the service's
    /// _harmony.chord_function (same rules, unit-tested against it).
    static juce::String functionLabel(const ChordEvent& ev, const std::optional<theory::ParsedKey>& key)
    {
        if (ev.chord.empty() || ev.chord == "N" || ev.chord == "N.C.") return {};
        // The service's own function label (af::ChordEvent::function) wins
        // when present: it was computed against the key the service detected,
        // with the full chord vocabulary. The local port is the fallback for
        // older services.
        if (!ev.function.empty()) {
            if (ev.function == "secondary") return TRANS("secondary");
            if (ev.function == "borrowed") return TRANS("borrowed");
            if (ev.function == "chromatic") return TRANS("chromatic");
            if (ev.function == "diatonic") return {};
            // An unknown future label: fall through to the local rules.
        }
        if (!key.has_value()) return {};
        switch (theory::chordFunction(ev.chord, *key)) {
        case theory::ChordFunction::Secondary:
            return TRANS("secondary");
        case theory::ChordFunction::Borrowed:
            return TRANS("borrowed");
        case theory::ChordFunction::Chromatic:
            return TRANS("chromatic");
        case theory::ChordFunction::Unknown:
            // A symbol the port can't parse but the service flagged as
            // non-diatonic still gets a neutral note.
            return ev.has_diatonic && !ev.diatonic ? TRANS("non-diatonic") : juce::String();
        case theory::ChordFunction::Diatonic:
        default:
            return {};
        }
    }

    static std::vector<double> parseTimes(const std::string& json)
    {
        std::vector<double> out;
        if (json.empty()) return out;
        const auto v = juce::JSON::parse(juce::String(json));
        if (auto* arr = v.getArray())
            for (const auto& item : *arr)
                out.push_back(static_cast<double>(item));
        return out;
    }

    // Local fallback from the deep analysis' note events (AUDIT-084): bars
    // come from the downbeat grid (no drift, pickup = bar 0), chords include
    // sevenths (Am7 is not "C") and are spelled for the detected key.
    static juce::String localChart(const AnalysisResult::PythonData& pd)
    {
        struct Note {
            double start = 0.0, end = 0.0;
            int pitch = 0;
            bool pitchClassOnly = false;
        };
        std::vector<Note> notes;
        const auto notesVar = juce::JSON::parse(juce::String(pd.pitch_notes_json));
        if (auto* arr = notesVar.getArray())
            for (const auto& item : *arr) {
                const auto& pco = item[api::fields::PitchNote::pitch_class_only];
                notes.push_back({static_cast<double>(item[api::fields::PitchNote::start_sec]),
                                 static_cast<double>(item[api::fields::PitchNote::end_sec]),
                                 static_cast<int>(item[api::fields::PitchNote::midi_pitch]),
                                 pco.isBool() && static_cast<bool>(pco)});
            }

        double duration = 0.0;
        for (const auto& n : notes)
            duration = std::max(duration, n.end);

        const auto key = theory::parseKey(pd.pitch_key);
        const auto bars = theory::segmentMeasures(parseTimes(pd.downbeat_times_json), parseTimes(pd.beat_times_json),
                                                  theory::beatsPerBar(pd.time_signature), duration);

        juce::String chordText;
        bool anyChord = false;
        juce::String last;
        for (const auto& bar : bars) {
            std::vector<int> active;
            bool octaveless = false;
            for (const auto& n : notes)
                if (n.start < bar.end_sec && n.end > bar.start_sec) {
                    active.push_back(n.pitch);
                    octaveless = octaveless || n.pitchClassOnly;
                }
            // Chroma-fallback notes all sit at 60 + pitch class: their lowest
            // note is not a bass, so no slash chord (Am, not "Am/C" - THY-08).
            const juce::String chord =
                active.empty() ? juce::String("N.C.") : juce::String(theory::nameChord(active, key, !octaveless));
            anyChord = anyChord || !active.empty();

            const juce::String timeStr = juce::String::formatted("%.1fs", bar.start_sec);
            if (bar.number > 0) {
                chordText << "M" << bar.number << " (" << timeStr << "): " << chord << "\n";
            } else if (bar.number == 0) {
                if (!active.empty()) chordText << TRANS("Pickup") << " (" << timeStr << "): " << chord << "\n";
            } else if (chord != last) {
                // No bar grid: label by time, never as invented measure numbers.
                chordText << timeStr << ": " << chord << "\n";
            }
            last = chord;
        }

        if (!anyChord) return TRANS("No chord progression detected.");
        return chordText;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChordsPanel)
};

} // namespace khensu
