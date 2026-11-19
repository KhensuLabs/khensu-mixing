// desktop-app/Source/ui/ExportDelivery/AlbumSequence.h
// The album master's editable tracklist, independent of any component so the
// order / metadata / trim logic is unit-testable (tests/ui/AlbumSequenceTest.cpp).
//
// The ISRC and UPC/EAN rules mirror local-services/khensu_services/analysis/
// album_master.py (normalize_isrc / normalize_catalog). The service stays the
// authority (it re-validates and answers 422); these checks only let the UI
// flag a typo as it is typed. Keep the two in lock-step.
#pragma once
#include <juce_core/juce_core.h>
#include <khensu/ipc/HttpServiceBridge.h>
#include <optional>
#include <vector>
#include <khensu/ipc/generated/ApiSchema.h>

namespace khensu::album_seq {

using TrackSpec = ipc::HttpServiceBridge::AlbumTrackSpec;

/// Canonical 12-character ISRC (spaces/hyphens dropped, upper case), or
/// nullopt. Empty input is "no ISRC", which is valid: returns "".
inline std::optional<juce::String> canonicalIsrc(const juce::String& raw)
{
    const auto s = raw.removeCharacters(" -").toUpperCase();
    if (s.isEmpty()) return juce::String();
    if (s.length() != 12) return std::nullopt;
    for (int i = 0; i < 12; ++i) {
        const auto c = s[i];
        const bool letter = c >= 'A' && c <= 'Z';
        const bool digit = c >= '0' && c <= '9';
        if (i < 2 && !letter) return std::nullopt;      // country code
        if (i >= 2 && i < 5 && !(letter || digit)) return std::nullopt; // registrant
        if (i >= 5 && !digit) return std::nullopt;      // year + designation
    }
    return s;
}

/// UPC-A (12) / EAN-13 (13) with a correct GTIN check digit. Empty = valid (none).
inline bool isValidCatalog(const juce::String& raw)
{
    const auto s = raw.removeCharacters(" -");
    if (s.isEmpty()) return true;
    if ((s.length() != 12 && s.length() != 13) || !s.containsOnly("0123456789")) return false;
    int total = 0;
    for (int i = s.length() - 2, w = 0; i >= 0; --i, ++w)
        total += ((int)s[i] - (int)'0') * (w % 2 == 0 ? 3 : 1);
    return (10 - total % 10) % 10 == (int)s[s.length() - 1] - (int)'0';
}

/// CD time MM:SS:FF (75 frames/s).
inline juce::String msfFromFrames(int frames)
{
    frames = juce::jmax(0, frames);
    return juce::String(frames / 4500).paddedLeft('0', 2) + ":" + juce::String(frames / 75 % 60).paddedLeft('0', 2) +
           ":" + juce::String(frames % 75).paddedLeft('0', 2);
}

class AlbumSequence {
  public:
    void setPaths(const juce::StringArray& paths)
    {
        tracks_.clear();
        for (const auto& p : paths) {
            TrackSpec t;
            t.path = p;
            t.title = juce::File(p).getFileNameWithoutExtension();
            tracks_.push_back(t);
        }
    }

    const std::vector<TrackSpec>& tracks() const noexcept { return tracks_; }
    std::vector<TrackSpec>& tracks() noexcept { return tracks_; }
    int size() const noexcept { return (int)tracks_.size(); }

    bool moveUp(int i)
    {
        if (i <= 0 || i >= size()) return false;
        std::swap(tracks_[(size_t)i], tracks_[(size_t)i - 1]);
        return true;
    }
    bool moveDown(int i) { return moveUp(i + 1); }
    bool remove(int i)
    {
        if (i < 0 || i >= size()) return false;
        tracks_.erase(tracks_.begin() + i);
        return true;
    }

    /// Copy gain_db from an /analyze/album/consistency response onto the
    /// matching tracks (matched by path, so a reorder since the measurement is
    /// harmless). Returns how many tracks received a trim.
    int applyConsistency(const juce::var& response)
    {
        int applied = 0;
        if (auto* arr = response[khensu::api::fields::AlbumConsistencyResponse::tracks].getArray())
            for (const auto& row : *arr)
                for (auto& t : tracks_)
                    if (t.path == row[khensu::api::fields::AlbumConsistencyTrack::path].toString()) {
                        t.gainDb = (double)row[khensu::api::fields::AlbumConsistencyTrack::gain_db];
                        ++applied;
                    }
        return applied;
    }

    /// Problems the user can fix before asking the service (empty = ready).
    juce::StringArray validationErrors(const juce::String& catalog) const
    {
        juce::StringArray errs;
        if (tracks_.empty()) errs.add(TRANS("Add at least one track."));
        if (tracks_.size() > 99) errs.add(TRANS("A CD holds at most 99 tracks."));
        if (!isValidCatalog(catalog)) errs.add(TRANS("Catalogue number is not a valid UPC-A / EAN-13."));
        juce::StringArray seen;
        for (size_t i = 0; i < tracks_.size(); ++i) {
            const auto& t = tracks_[i];
            const auto isrc = canonicalIsrc(t.isrc);
            const auto n = TRANS("Track") + " " + juce::String((int)i + 1) + ": ";
            if (!isrc) errs.add(n + TRANS("ISRC must look like CC-XXX-YY-NNNNN."));
            else if (isrc->isNotEmpty()) {
                if (seen.contains(*isrc)) errs.add(n + TRANS("this ISRC is already used by another track."));
                seen.add(*isrc);
            }
            if (t.gapSec < 0.0 || t.crossfadeSec < 0.0 || t.fadeInSec < 0.0 || t.fadeOutSec < 0.0)
                errs.add(n + TRANS("times must not be negative."));
        }
        return errs;
    }

  private:
    std::vector<TrackSpec> tracks_;
};

} // namespace khensu::album_seq
