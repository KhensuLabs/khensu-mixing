// tests/ui/MultitrackStemBusesTest.cpp
// WF-7: a multitrack session's tracks summed into the four stem slots.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "session/MultitrackStemBuses.h"
#include "session/StemSeparationInfo.h"
#include <cmath>

using namespace khensu;

namespace {

struct TempDir {
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("khensu_mt_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

/// A constant-value file: easy to check a sum.
juce::String writeConstant(const juce::File& f, float value, int channels, double rate, int samples)
{
    juce::AudioBuffer<float> b(channels, samples);
    for (int ch = 0; ch < channels; ++ch)
        juce::FloatVectorOperations::fill(b.getWritePointer(ch), value, samples);
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os = f.createOutputStream();
    REQUIRE(os != nullptr);
    auto w =
        wav.createWriterFor(os, juce::AudioFormatWriterOptions{}
                                    .withSampleRate(rate)
                                    .withNumChannels(channels)
                                    .withBitsPerSample(32)
                                    .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    REQUIRE(w != nullptr);
    REQUIRE(w->writeFromAudioSampleBuffer(b, 0, samples));
    return f.getFullPathName();
}

AudioFileRef track(const juce::String& path, const char* role)
{
    AudioFileRef r;
    r.path = path.toStdString();
    r.track_role = role;
    return r;
}

float sampleAt(const juce::File& f, int channel, int index, juce::int64* length = nullptr)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(f));
    REQUIRE(r != nullptr);
    if (length != nullptr) *length = r->lengthInSamples;
    juce::AudioBuffer<float> b(2, 1);
    r->read(&b, 0, 1, index, true, true);
    return b.getSample(channel, 0);
}

} // namespace

TEST_CASE("multitrack roles map onto the four stem slots (WF-7)", "[multitrack][stems]")
{
    using multitrack_stems::slotForRole;
    CHECK(slotForRole("vocals") == "vocals");
    CHECK(slotForRole("kick") == "drums");
    CHECK(slotForRole("snare") == "drums");
    CHECK(slotForRole("percussion") == "drums");
    CHECK(slotForRole("drums") == "drums");
    CHECK(slotForRole("bass") == "bass");
    CHECK(slotForRole("synth") == "other");
    CHECK(slotForRole("atmosphere_fx") == "other");
    CHECK(slotForRole("something_new") == "other");
    CHECK(slotForRole("mix").isEmpty());
    CHECK(slotForRole("").isEmpty());

    const auto groups = multitrack_stems::groupBySlot(
        {track("mix.wav", "mix"), track("kick.wav", "kick"), track("snare.wav", "snare"), track("vox.wav", "vocals")});
    CHECK(groups.count("drums") == 1);
    CHECK(groups.at("drums").size() == 2);
    CHECK(groups.at("vocals") == juce::StringArray("vox.wav"));
    CHECK(groups.count("bass") == 0);
}

TEST_CASE("multitrack tracks are summed into stem files the stem tools read (WF-7)", "[multitrack][stems]")
{
    TempDir tmp;
    const int n = 48000;
    const auto lead = writeConstant(tmp.dir.getChildFile("lead.wav"), 0.25f, 2, 48000.0, n);
    const auto dbl = writeConstant(tmp.dir.getChildFile("double.wav"), 0.125f, 2, 48000.0, n / 2);
    const auto bass = writeConstant(tmp.dir.getChildFile("bass.wav"), 0.1f, 1, 48000.0, n);
    const auto odd = writeConstant(tmp.dir.getChildFile("kick_44k.wav"), 0.5f, 2, 44100.0, n);
    const auto stems = tmp.dir.getChildFile("stems");

    const auto outcome = multitrack_stems::writeStemBuses(
        {track(lead, "mix"), track(lead, "vocals"), track(dbl, "vocals"), track(bass, "bass"), track(odd, "kick")},
        stems);
    REQUIRE(outcome.ok);

    // Vocals: lead + double where both play, the lead alone after the double ends.
    juce::int64 length = 0;
    CHECK(sampleAt(stems.getChildFile("vocals.wav"), 0, 10, &length) == Catch::Approx(0.375f));
    CHECK(sampleAt(stems.getChildFile("vocals.wav"), 1, n - 10) == Catch::Approx(0.25f));
    CHECK(length == n);
    // A mono track is centred on both channels.
    CHECK(sampleAt(stems.getChildFile("bass.wav"), 1, 100) == Catch::Approx(0.1f));
    // The 44.1 kHz kick is left out (no resampling); drums and other are silent.
    CHECK(sampleAt(stems.getChildFile("drums.wav"), 0, 100) == Catch::Approx(0.0f));
    CHECK(stems.getChildFile("other.wav").existsAsFile());
    CHECK(outcome.message.contains("kick_44k.wav"));
    CHECK(outcome.empty.contains("other"));

    // Real stems, not a guess: the stem consumers' check passes.
    const auto info = StemSeparationInfo::readFrom(stems);
    CHECK(info.method == multitrack_stems::kMethod);
    CHECK_FALSE(info.isApproximate());

    // A second run leaves what is there.
    const auto before = stems.getChildFile("vocals.wav").getLastModificationTime();
    REQUIRE(multitrack_stems::writeStemBuses({track(lead, "vocals")}, stems).ok);
    CHECK(stems.getChildFile("vocals.wav").getLastModificationTime() == before);
}
