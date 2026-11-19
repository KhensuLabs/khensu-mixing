// desktop-app/Source/session/MultitrackStemBuses.cpp
#include "MultitrackStemBuses.h"
#include "StemSeparationInfo.h"
#include <algorithm>
#include <memory>

namespace khensu::multitrack_stems {

juce::String slotForRole(const juce::String& role)
{
    const auto r = role.trim().toLowerCase();
    if (r.isEmpty() || r == "mix") return {};
    if (r == "vocals" || r == "vocal") return "vocals";
    if (r == "drums" || r == "kick" || r == "snare" || r == "percussion") return "drums";
    if (r == "bass") return "bass";
    return "other";
}

std::map<juce::String, juce::StringArray> groupBySlot(const std::vector<AudioFileRef>& files)
{
    std::map<juce::String, juce::StringArray> out;
    for (const auto& f : files) {
        const auto slot = slotForRole(juce::String(f.track_role));
        if (slot.isNotEmpty()) out[slot].add(juce::String(f.path));
    }
    return out;
}

namespace {

constexpr int kBlock = 1 << 15;

bool writeSlot(juce::AudioFormatManager& formats, const juce::StringArray& tracks, const juce::File& out,
               double sampleRate, juce::int64 lengthSamples, juce::StringArray& skipped)
{
    std::vector<std::unique_ptr<juce::AudioFormatReader>> readers;
    for (const auto& t : tracks) {
        std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(juce::File(t)));
        if (r == nullptr || std::abs(r->sampleRate - sampleRate) > 0.5) {
            skipped.add(juce::File(t).getFileName());
            continue;
        }
        readers.push_back(std::move(r));
    }
    out.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = out.createOutputStream();
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    // 32-bit float: a sum of tracks may pass full scale, as the mix's own bus did.
    auto writer =
        wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                        .withSampleRate(sampleRate)
                                        .withNumChannels(2)
                                        .withBitsPerSample(32)
                                        .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    if (writer == nullptr) return false;

    juce::AudioBuffer<float> sum(2, kBlock), part(2, kBlock);
    for (juce::int64 pos = 0; pos < lengthSamples; pos += kBlock) {
        const int n = static_cast<int>(std::min<juce::int64>(kBlock, lengthSamples - pos));
        sum.clear();
        for (auto& r : readers) {
            if (pos >= r->lengthInSamples) continue;
            part.clear();
            r->read(&part, 0, n, pos, true, true);
            // A mono track reads into channel 0 only: centre it on both.
            if (r->numChannels == 1) part.copyFrom(1, 0, part, 0, 0, n);
            for (int ch = 0; ch < 2; ++ch)
                sum.addFrom(ch, 0, part, ch, 0, n);
        }
        if (!writer->writeFromAudioSampleBuffer(sum, 0, n)) return false;
    }
    return true;
}

} // namespace

Outcome writeStemBuses(const std::vector<AudioFileRef>& files, const juce::File& dir, bool overwrite)
{
    Outcome outcome;
    const auto slots = groupBySlot(files);
    if (slots.empty()) {
        outcome.message = TRANS("No role-tagged tracks to sum.");
        return outcome;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    // The bus format: the first readable track's rate; the longest track's length.
    double sampleRate = 0.0;
    juce::int64 length = 0;
    for (const auto& [slot, tracks] : slots)
        for (const auto& t : tracks)
            if (std::unique_ptr<juce::AudioFormatReader> r{formats.createReaderFor(juce::File(t))}) {
                if (sampleRate <= 0.0) sampleRate = r->sampleRate;
                if (std::abs(r->sampleRate - sampleRate) <= 0.5) length = std::max(length, r->lengthInSamples);
            }
    if (sampleRate <= 0.0 || length <= 0) {
        outcome.message = TRANS("None of the tracks could be read.");
        return outcome;
    }
    if (!dir.createDirectory()) {
        outcome.message = TRANS("Could not create the stems folder.");
        return outcome;
    }

    juce::StringArray skipped;
    for (const char* slot : {"vocals", "drums", "bass", "other"}) {
        const auto file = dir.getChildFile(juce::String(slot) + ".wav");
        if (file.existsAsFile() && !overwrite) continue;
        const auto it = slots.find(slot);
        const juce::StringArray tracks = it != slots.end() ? it->second : juce::StringArray();
        if (tracks.isEmpty()) outcome.empty.add(slot);
        if (!writeSlot(formats, tracks, file, sampleRate, length, skipped)) {
            outcome.message = TRANS("Could not write %s.").replace("%s", file.getFileName());
            return outcome;
        }
    }

    juce::DynamicObject::Ptr info = new juce::DynamicObject();
    info->setProperty("method", kMethod);
    juce::String reason;
    if (!outcome.empty.isEmpty())
        reason << "no tracks for " << outcome.empty.joinIntoString(", ") << " (written silent)";
    if (!skipped.isEmpty())
        reason << (reason.isEmpty() ? "" : "; ")
               << "left out at another sample rate or unreadable: " << skipped.joinIntoString(", ");
    info->setProperty("reason", reason);
    dir.getChildFile(StemSeparationInfo::kFileName).replaceWithText(juce::JSON::toString(juce::var(info.get())));

    outcome.ok = true;
    outcome.message = TRANS("Stems summed from the session's tracks.");
    if (!skipped.isEmpty()) outcome.message << " " << TRANS("Left out:") << " " << skipped.joinIntoString(", ");
    return outcome;
}

} // namespace khensu::multitrack_stems
