// desktop-app/Source/ui/VocalWorkspace/StemEnergyPanel.cpp
#include "StemEnergyPanel.h"
#include "../shared/PanelRunState.h"
#include "../shared/SessionEpoch.h"
#include "../shared/KhensuLookAndFeel.h"
#include <khensu/session/AudioFileProber.h>

namespace khensu {

StemEnergyPanel::StemEnergyPanel(IAppController& c) : PanelCard(c, "Stem Energy Balance")
{
    setExplainer(TRANS("Stem Energy Balance"), TRANS("RMS energy share across the four separated stems, read directly off "
                                        "the stem files -- shows the mix balance at a glance instead of "
                                        "requiring you to solo each stem and judge loudness by ear."));
    setAnalysisState(PanelAnalysisState::NotApplicable);
}

void StemEnergyPanel::setStemPaths(const juce::String& vocals, const juce::String& drums, const juce::String& bass,
                                   const juce::String& other)
{
    stemPaths_[0] = vocals;
    stemPaths_[1] = drums;
    stemPaths_[2] = bass;
    stemPaths_[3] = other;

    const juce::String key = vocals + "|" + drums + "|" + bass + "|" + other;
    if (key == lastComputedKey_) return;
    lastComputedKey_ = key;
    computeEnergy();
}

void StemEnergyPanel::computeEnergy()
{
    std::array<juce::String, 4> paths{stemPaths_[0], stemPaths_[1], stemPaths_[2], stemPaths_[3]};
    for (const auto& p : paths)
        if (p.isEmpty() || !juce::File(p).existsAsFile()) {
            haveData_ = false;
            repaint();
            return;
        }

    fetchQuietly(*this, controller_, [paths]() {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();

        std::array<float, 4> energy{0.0f, 0.0f, 0.0f, 0.0f};
        for (int i = 0; i < 4; ++i) {
            std::unique_ptr<juce::AudioFormatReader> reader =
                AudioFileProber::openReader(fm, juce::File(paths[(size_t)i]));
            if (reader == nullptr) continue;

            const juce::int64 numSamples = reader->lengthInSamples;
            const int blockSize = 1 << 16;
            double sumSquares = 0.0;
            juce::int64 totalRead = 0;
            juce::AudioBuffer<float> buf(1, blockSize);

            for (juce::int64 pos = 0; pos < numSamples; pos += blockSize) {
                const int toRead = (int)juce::jmin((juce::int64)blockSize, numSamples - pos);
                reader->read(&buf, 0, toRead, pos, true, reader->numChannels > 1);
                const float* data = buf.getReadPointer(0);
                for (int s = 0; s < toRead; ++s)
                    sumSquares += (double)data[s] * (double)data[s];
                totalRead += toRead;
            }
            energy[(size_t)i] = totalRead > 0 ? (float)std::sqrt(sumSquares / (double)totalRead) : 0.0f;
        }

        const float total = energy[0] + energy[1] + energy[2] + energy[3];
        std::array<float, 4> share{0.0f, 0.0f, 0.0f, 0.0f};
        if (total > 0.0f)
            for (int i = 0; i < 4; ++i)
                share[(size_t)i] = energy[(size_t)i] / total;

        return share;
    }, [](StemEnergyPanel& self, std::array<float, 4> share) {
        self.rmsShare_ = share;
        self.haveData_ = true;
        self.repaint();
    });
}

void StemEnergyPanel::resized() { PanelCard::resized(); }

void StemEnergyPanel::paint(juce::Graphics& g)
{
    PanelCard::paint(g);
    auto body = bodyArea();

    if (!haveData_) {
        g.setColour(KhensuTheme::textSecondary.withAlpha(0.5f));
        g.setFont(khensu::KhensuTheme::uiFont(10.0f, juce::Font::italic));
        g.drawText("Requires separated stems", body, juce::Justification::centred, false);
        return;
    }

    const int n = 4;
    const int rowH = body.getHeight() / n;
    for (int i = 0; i < n; ++i) {
        auto row = juce::Rectangle<int>(body.getX(), body.getY() + i * rowH, body.getWidth(), rowH).reduced(0, 4);
        auto labelArea = row.removeFromLeft(60);
        g.setColour(KhensuTheme::textSecondary);
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::bold));
        g.drawText(kLabels[i], labelArea, juce::Justification::centredLeft, false);

        auto barArea = row.toFloat();
        g.setColour(KhensuTheme::bgSurface.brighter(0.05f));
        g.fillRect(barArea);
        g.setColour(juce::Colour(kColours[i]).withAlpha(0.85f));
        g.fillRect(barArea.withWidth(barArea.getWidth() * rmsShare_[(size_t)i]));

        g.setColour(khensu::KhensuTheme::textPrimary.withAlpha(0.85f));
        g.setFont(khensu::KhensuTheme::uiFont(9.0f, juce::Font::plain));
        g.drawText(juce::String(juce::roundToInt(rmsShare_[(size_t)i] * 100.0f)) + "%", barArea, juce::Justification::centredRight,
                   false);
    }
}

} // namespace khensu
