#include "LiveAnalysisFifo.h"

#include <algorithm>
#include <cstring>

namespace khensu::live {

LiveAnalysisFifo::LiveAnalysisFifo(int numSlots)
    : slots_(static_cast<size_t>(juce::jmax(2, numSlots))), fifo_(juce::jmax(2, numSlots))
{
}

bool LiveAnalysisFifo::push(const float* const* channels, int numChannels, int numSamples, double timeSec,
                            double sampleRate) noexcept
{
    if (channels == nullptr || numChannels <= 0 || numSamples <= 0 || sampleRate <= 0.0) return true;
    const int chans = std::min(numChannels, kMaxChannels);
    bool complete = true;
    for (int offset = 0; offset < numSamples; offset += kSlotSamples) {
        const int count = std::min(kSlotSamples, numSamples - offset);
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo_.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 + size2 < 1) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            complete = false;
            continue;
        }
        auto& slot = slots_[static_cast<size_t>(size1 > 0 ? start1 : start2)];
        slot.timeSec = timeSec + static_cast<double>(offset) / sampleRate;
        slot.sampleRate = sampleRate;
        slot.numSamples = count;
        slot.numChannels = chans;
        for (int c = 0; c < chans; ++c)
            std::memcpy(slot.data[static_cast<size_t>(c)].data(), channels[c] + offset,
                        sizeof(float) * static_cast<size_t>(count));
        fifo_.finishedWrite(1);
    }
    return complete;
}

bool LiveAnalysisFifo::pop(Slot& out) noexcept
{
    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo_.prepareToRead(1, start1, size1, start2, size2);
    if (size1 + size2 < 1) return false;
    const auto& slot = slots_[static_cast<size_t>(size1 > 0 ? start1 : start2)];
    out.timeSec = slot.timeSec;
    out.sampleRate = slot.sampleRate;
    out.numSamples = slot.numSamples;
    out.numChannels = slot.numChannels;
    for (int c = 0; c < slot.numChannels; ++c)
        std::memcpy(out.data[static_cast<size_t>(c)].data(), slot.data[static_cast<size_t>(c)].data(),
                    sizeof(float) * static_cast<size_t>(slot.numSamples));
    fifo_.finishedRead(1);
    return true;
}

} // namespace khensu::live
