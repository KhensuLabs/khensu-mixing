// desktop-app/Source/ui/SessionIntake/NativeMeterTiles.cpp
#include "NativeMeterTiles.h"
#include "../shared/KhensuTheme.h"

#include <cmath>

namespace khensu {

namespace {

/// Decode the whole file into planar float channels. Returns false (with a
/// reason) when it can't be read or is beyond the native budget.
bool decodeFile(const juce::File& file, juce::AudioBuffer<float>& out, double& sampleRate, juce::String& reason)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file));
    if (reader == nullptr) {
        reason = TRANS("This format can't be read natively; the service will measure it.");
        return false;
    }
    sampleRate = reader->sampleRate;
    const auto numChannels = static_cast<int>(reader->numChannels);
    if (sampleRate <= 0.0 || numChannels <= 0 || numChannels > 8) {
        reason = TRANS("Channel layout not supported by the native meters.");
        return false;
    }
    const auto length = reader->lengthInSamples;
    if (length <= 0 || static_cast<double>(length) / sampleRate > NativeMeterTiles::kMaxSeconds) {
        reason = TRANS("Long file: the full analysis will report these numbers.");
        return false;
    }
    out.setSize(numChannels, static_cast<int>(length));
    if (!reader->read(&out, 0, static_cast<int>(length), 0, true, true)) {
        reason = TRANS("The file could not be decoded.");
        return false;
    }
    return true;
}

} // namespace

NativeMeterTiles::NativeMeterTiles(BackgroundRunner runInBackground) : runInBackground_(std::move(runInBackground))
{
    setTitle(TRANS("Instant meters"));
    setDescription(TRANS("Integrated loudness, true peak, loudness range and stereo width, measured on this "
                         "machine as soon as a file is dropped."));
}

NativeMeterTiles::~NativeMeterTiles()
{
    shared_->generation.fetch_add(1); // drop any in-flight result
}

void NativeMeterTiles::clear()
{
    shared_->generation.fetch_add(1);
    state_ = State::Idle;
    note_.clear();
    repaint();
}

void NativeMeterTiles::measure(const juce::File& file)
{
    const int gen = shared_->generation.fetch_add(1) + 1;
    state_ = State::Measuring;
    note_.clear();
    repaint();
    if (!runInBackground_) return;

    juce::Component::SafePointer<NativeMeterTiles> self(this);
    auto shared = shared_;
    runInBackground_([self, shared, gen, file] {
        if (shared->generation.load() != gen) return; // superseded before it started
        juce::AudioBuffer<float> buffer;
        double sr = 0.0;
        juce::String reason;
        native::NativeReport report;
        const bool ok = decodeFile(file, buffer, sr, reason);
        if (ok && shared->generation.load() == gen) {
            report = native::measureAll(buffer.getArrayOfReadPointers(), buffer.getNumChannels(),
                                        buffer.getNumSamples(), sr);
        }
        juce::MessageManager::callAsync([self, shared, gen, ok, reason, report = std::move(report)]() mutable {
            if (self == nullptr || shared->generation.load() != gen) return; // a newer drop owns the tiles
            self->state_ = ok ? State::Ready : State::Failed;
            self->note_ = reason;
            self->report_ = std::move(report);
            self->repaint();
        });
    });
}

juce::String NativeMeterTiles::formatValue(double v, int decimals)
{
    if (!std::isfinite(v)) return "--";
    return juce::String(v, decimals);
}

void NativeMeterTiles::paint(juce::Graphics& g)
{
    if (state_ == State::Idle) return;

    auto area = getLocalBounds();
    const auto caption = area.removeFromBottom(16);

    struct Tile {
        juce::String label, value, unit;
    };
    const auto& l = report_.loudness;
    const bool ready = state_ == State::Ready;
    const Tile tiles[4] = {
        {TRANS("Integrated"), ready ? formatValue(l.integratedLufs, 1) : juce::String("..."), "LUFS"},
        {TRANS("True peak"), ready ? formatValue(l.truePeakDbtp, 1) : juce::String("..."), "dBTP"},
        {TRANS("Loudness range"), ready ? formatValue(l.lra, 1) : juce::String("..."), "LU"},
        {TRANS("Stereo width"),
         ready ? (report_.stereo.stereo ? formatValue(report_.stereo.width, 2) : TRANS("mono")) : juce::String("..."),
         ""},
    };

    const int gap = KhensuTheme::sp2;
    const int tileW = (area.getWidth() - 3 * gap) / 4;
    for (int i = 0; i < 4; ++i) {
        auto r = area.removeFromLeft(tileW).toFloat();
        area.removeFromLeft(gap);
        g.setColour(KhensuTheme::bgRaised);
        g.fillRoundedRectangle(r, 6.0f);
        g.setColour(KhensuTheme::borderSubtle);
        g.drawRoundedRectangle(r.reduced(0.5f), 6.0f, 1.0f);

        auto inner = r.reduced(8.0f, 4.0f);
        g.setColour(KhensuTheme::textTertiary);
        g.setFont(KhensuTheme::bodyFont(11.0f));
        g.drawText(tiles[i].label, inner.removeFromTop(14.0f), juce::Justification::centredLeft, true);

        const bool truePeakHot = ready && i == 1 && std::isfinite(l.truePeakDbtp) && l.truePeakDbtp > -1.0;
        g.setColour(truePeakHot ? KhensuTheme::accentAmber : KhensuTheme::textPrimary);
        g.setFont(KhensuTheme::metricFont(18.0f));
        const auto valueText = tiles[i].unit.isEmpty() ? tiles[i].value : tiles[i].value + " " + tiles[i].unit;
        g.drawText(valueText, inner, juce::Justification::centredLeft, true);
    }

    g.setColour(KhensuTheme::textTertiary);
    g.setFont(KhensuTheme::bodyFont(10.5f));
    juce::String text;
    if (state_ == State::Measuring)
        text = TRANS("Measuring on this machine...");
    else if (state_ == State::Failed)
        text = note_;
    else
        text = TRANS("Measured on this machine (BS.1770-4); the full analysis follows.");
    g.drawText(text, caption, juce::Justification::centredLeft, true);
}

} // namespace khensu
