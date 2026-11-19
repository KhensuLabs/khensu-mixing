// tests/shared-core/test_crossover.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/dsp/LinkwitzRileyCrossover3Band.h>
#include <vector>
#include <cmath>

TEST_CASE("LinkwitzRileyCrossover3Band splits and sums phase-coherently", "[crossover]")
{
    using namespace khensu;

    const double sampleRate = 44100.0;
    const int numSamples = 4096;
    const float lowCutoff = 200.0f;
    const float highCutoff = 2000.0f;
    const double pi = 3.14159265358979323846;

    juce::dsp::ProcessSpec spec{sampleRate, static_cast<juce::uint32>(numSamples), 1};

    LinkwitzRileyCrossover3Band crossover;
    crossover.prepare(spec);
    crossover.setCutoffs(lowCutoff, highCutoff);

    // Audio buffers for processing
    juce::AudioBuffer<float> inputBuffer(1, numSamples);
    juce::AudioBuffer<float> lowBuffer(1, numSamples);
    juce::AudioBuffer<float> midBuffer(1, numSamples);
    juce::AudioBuffer<float> highBuffer(1, numSamples);

    juce::dsp::AudioBlock<float> inputBlock(inputBuffer);
    juce::dsp::AudioBlock<float> lowBlock(lowBuffer);
    juce::dsp::AudioBlock<float> midBlock(midBuffer);
    juce::dsp::AudioBlock<float> highBlock(highBuffer);

    juce::dsp::ProcessContextReplacing<float> context(inputBlock);

    SECTION("Low frequency splitting (50 Hz)")
    {
        // Generate a 50 Hz sine wave (well below 200 Hz lowCutoff)
        auto* writePtr = inputBuffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            writePtr[i] = static_cast<float>(std::sin(2.0 * pi * 50.0 * i / sampleRate));
        }

        crossover.reset();
        crossover.process(context, lowBlock, midBlock, highBlock);

        // After settling (skip first 2000 samples for filter transient)
        float maxLow = 0.0f;
        float maxMid = 0.0f;
        float maxHigh = 0.0f;
        for (int i = 2000; i < numSamples; ++i) {
            maxLow = std::max(maxLow, std::abs(lowBuffer.getSample(0, i)));
            maxMid = std::max(maxMid, std::abs(midBuffer.getSample(0, i)));
            maxHigh = std::max(maxHigh, std::abs(highBuffer.getSample(0, i)));
        }

        // Low band should contain almost all energy
        REQUIRE(maxLow > 0.90f);
        REQUIRE(maxMid < 0.10f);
        REQUIRE(maxHigh < 0.05f);
    }

    SECTION("Mid frequency splitting (1000 Hz)")
    {
        // Generate a 1000 Hz sine wave (between 200 Hz and 2000 Hz)
        auto* writePtr = inputBuffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            writePtr[i] = static_cast<float>(std::sin(2.0 * pi * 1000.0 * i / sampleRate));
        }

        crossover.reset();
        crossover.process(context, lowBlock, midBlock, highBlock);

        float maxLow = 0.0f;
        float maxMid = 0.0f;
        float maxHigh = 0.0f;
        for (int i = 2000; i < numSamples; ++i) {
            maxLow = std::max(maxLow, std::abs(lowBuffer.getSample(0, i)));
            maxMid = std::max(maxMid, std::abs(midBuffer.getSample(0, i)));
            maxHigh = std::max(maxHigh, std::abs(highBuffer.getSample(0, i)));
        }

        // Mid band should contain almost all energy
        REQUIRE(maxLow < 0.10f);
        REQUIRE(maxMid > 0.90f);
        REQUIRE(maxHigh < 0.10f);
    }

    SECTION("High frequency splitting (8000 Hz)")
    {
        // Generate a 8000 Hz sine wave (well above 2000 Hz highCutoff)
        auto* writePtr = inputBuffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            writePtr[i] = static_cast<float>(std::sin(2.0 * pi * 8000.0 * i / sampleRate));
        }

        crossover.reset();
        crossover.process(context, lowBlock, midBlock, highBlock);

        float maxLow = 0.0f;
        float maxMid = 0.0f;
        float maxHigh = 0.0f;
        for (int i = 2000; i < numSamples; ++i) {
            maxLow = std::max(maxLow, std::abs(lowBuffer.getSample(0, i)));
            maxMid = std::max(maxMid, std::abs(midBuffer.getSample(0, i)));
            maxHigh = std::max(maxHigh, std::abs(highBuffer.getSample(0, i)));
        }

        // High band should contain almost all energy
        REQUIRE(maxLow < 0.05f);
        REQUIRE(maxMid < 0.10f);
        REQUIRE(maxHigh > 0.90f);
    }

    SECTION("Flat summation magnitude")
    {
        // Generate a sine wave at crossover overlap (e.g. 500 Hz)
        auto* writePtr = inputBuffer.getWritePointer(0);
        for (int i = 0; i < numSamples; ++i) {
            writePtr[i] = static_cast<float>(std::sin(2.0 * pi * 500.0 * i / sampleRate));
        }

        crossover.reset();
        crossover.process(context, lowBlock, midBlock, highBlock);

        // Sum the bands: sum = low + mid + high
        juce::AudioBuffer<float> sumBuffer(1, numSamples);
        sumBuffer.clear();
        sumBuffer.addFrom(0, 0, lowBuffer, 0, 0, numSamples);
        sumBuffer.addFrom(0, 0, midBuffer, 0, 0, numSamples);
        sumBuffer.addFrom(0, 0, highBuffer, 0, 0, numSamples);

        // Verify that the steady-state summed output magnitude is 1.0 (flat 0 dB)
        float maxSum = 0.0f;
        for (int i = 2000; i < numSamples; ++i) {
            maxSum = std::max(maxSum, std::abs(sumBuffer.getSample(0, i)));
        }

        // Should be extremely close to 1.0
        REQUIRE_THAT(maxSum, Catch::Matchers::WithinAbs(1.0, 0.01));
    }
}
