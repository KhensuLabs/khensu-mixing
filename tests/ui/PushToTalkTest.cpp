// tests/ui/PushToTalkTest.cpp
// CE-AG-4 push-to-talk: the hold / tap / keyboard state machine, the WAV
// the recording is written to, and the user-facing error messages.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "ui/shared/PushToTalk.h"

#include <cmath>

using khensu::push_to_talk::Machine;
using Action = Machine::Action;
using State = Machine::State;

TEST_CASE("Push-to-talk: hold, speak, release sends", "[ui][ptt]")
{
    Machine m;
    REQUIRE(m.state() == State::Idle);
    CHECK(m.press(1000.0) == Action::StartRecording);
    CHECK(m.state() == State::Recording);
    CHECK(m.release(1000.0 + 2500.0) == Action::StopAndSend);
    CHECK(m.state() == State::Transcribing);
    // Busy: further input is ignored until the request finishes.
    CHECK(m.press(5000.0) == Action::None);
    CHECK(m.release(5100.0) == Action::None);
    CHECK(m.activate() == Action::None);
    m.reset();
    CHECK(m.state() == State::Idle);
}

TEST_CASE("Push-to-talk: a short tap latches, the next tap sends", "[ui][ptt]")
{
    Machine m;
    CHECK(m.press(0.0) == Action::StartRecording);
    CHECK(m.release(Machine::kTapMs - 50.0) == Action::None); // a tap
    CHECK(m.state() == State::Recording);
    CHECK(m.latched());
    CHECK(m.press(4000.0) == Action::StopAndSend); // second tap stops
    CHECK(m.state() == State::Transcribing);
    CHECK(m.release(4050.0) == Action::None); // that tap's release is swallowed
    m.reset();
    // ...and the swallowed release does not leak into the next hold.
    CHECK(m.press(10000.0) == Action::StartRecording);
    CHECK(m.release(13000.0) == Action::StopAndSend);
}

TEST_CASE("Push-to-talk: keyboard activation toggles", "[ui][ptt]")
{
    Machine m;
    CHECK(m.activate() == Action::StartRecording);
    CHECK(m.latched());
    CHECK(m.release(1.0) == Action::None); // a stray mouse-up does not stop it
    CHECK(m.activate() == Action::StopAndSend);
    CHECK(m.state() == State::Transcribing);
}

TEST_CASE("Push-to-talk: no input device resets to idle", "[ui][ptt]")
{
    Machine m;
    CHECK(m.press(0.0) == Action::StartRecording);
    m.reset(); // the recorder could not open a microphone
    CHECK(m.state() == State::Idle);
    CHECK(m.release(5000.0) == Action::None);
    CHECK(m.press(6000.0) == Action::StartRecording);
}

TEST_CASE("Push-to-talk: the recording is written as a 16-bit mono WAV", "[ui][ptt]")
{
    const juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("khensu_ptt_test");
    dir.createDirectory();
    const juce::File wav = dir.getNonexistentChildFile("ptt", ".wav");

    constexpr double sr = 16000.0;
    std::vector<float> samples((size_t)sr); // 1 s of 440 Hz at -6 dBFS
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.5f * (float)std::sin(2.0 * juce::MathConstants<double>::pi * 440.0 * (double)i / sr);
    REQUIRE(khensu::push_to_talk::writeWav(wav, samples, sr).isEmpty());

    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader(format.createReaderFor(wav.createInputStream().release(), true));
    REQUIRE(reader != nullptr);
    CHECK(reader->numChannels == 1);
    CHECK(reader->bitsPerSample == 16);
    CHECK(reader->sampleRate == Catch::Approx(sr));
    CHECK(reader->lengthInSamples == (juce::int64)samples.size());
    juce::AudioBuffer<float> back(1, (int)samples.size());
    reader->read(&back, 0, (int)samples.size(), 0, true, false);
    CHECK(back.getMagnitude(0, 0, back.getNumSamples()) == Catch::Approx(0.5f).margin(0.01f));
    reader.reset();

    CHECK(khensu::push_to_talk::writeWav(wav, {}, sr).isNotEmpty());      // nothing recorded
    CHECK(khensu::push_to_talk::writeWav(wav, samples, 0.0).isNotEmpty()); // no sample rate
    dir.deleteRecursively();
}

TEST_CASE("Push-to-talk: voice errors become clear messages", "[ui][ptt]")
{
    using khensu::push_to_talk::voiceErrorMessage;
    const auto noAsr = voiceErrorMessage(503, "Voice commands need the ASR extra: faster-whisper is not installed");
    CHECK(noAsr.contains("[asr]"));
    CHECK(noAsr.contains("faster-whisper is not installed"));
    CHECK(voiceErrorMessage(503, "Service is shutting down") == "Service is shutting down");
    CHECK(voiceErrorMessage(422, "No speech was recognised in the recording.").contains("No speech"));
    CHECK(voiceErrorMessage(422, {}).isNotEmpty());
    CHECK(voiceErrorMessage(500, {}).isNotEmpty());
}
