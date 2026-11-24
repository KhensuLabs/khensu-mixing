#include <khensu/dsp/DemoSong.h>

#include <cmath>
#include <vector>

namespace khensu::demo_song {

namespace {

constexpr double kTwoPi = 6.283185307179586;

double midiHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

/// One-pole low-pass state.
struct OnePole {
    double z = 0.0;
    double process(double x, double coeff) noexcept { return z += coeff * (x - z); }
};

double lpCoeff(double hz, double sr) { return 1.0 - std::exp(-kTwoPi * hz / sr); }

enum class Part { Intro, Verse, Pre, Chorus, Bridge, Outro };

Part partOf(int bar)
{
    for (const auto& s : arrangement())
        if (bar >= s.firstBar && bar < s.firstBar + s.bars) {
            const juce::String l(s.label);
            if (l.startsWith("Intro")) return Part::Intro;
            if (l.startsWith("Verse")) return Part::Verse;
            if (l.startsWith("Pre")) return Part::Pre;
            if (l.startsWith("Chorus")) return Part::Chorus;
            if (l.startsWith("Bridge")) return Part::Bridge;
            return Part::Outro;
        }
    return Part::Outro;
}

bool lastChorus(int bar) { return bar >= 96 && bar < 104; }

} // namespace

const std::vector<Section>& arrangement()
{
    static const std::vector<Section> a{{"Intro", 0, 8},     {"Verse 1", 8, 16},   {"Pre-chorus", 24, 8},
                                        {"Chorus 1", 32, 16}, {"Verse 2", 48, 16},  {"Chorus 2", 64, 16},
                                        {"Bridge", 80, 8},    {"Chorus 3", 88, 16}, {"Outro", 104, 8}};
    return a;
}

juce::AudioBuffer<float> render(double sr)
{
    const int total = static_cast<int>(kDurationSec * sr);
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    auto* L = out.getWritePointer(0);
    auto* R = out.getWritePointer(1);
    const double beat = 60.0 / kBpm;
    const int beatN = static_cast<int>(beat * sr);
    const int barN = 4 * beatN;
    juce::Random rng(20261124);

    // Chords: Am F C G (root MIDI notes, minor/major triads).
    const int roots[4] = {57, 53, 48, 55};
    const bool minor[4] = {true, false, false, false};

    auto addMono = [&](int i, double v, double pan) {
        if (i < 0 || i >= total) return;
        const double l = std::cos((pan + 1.0) * 0.25 * juce::MathConstants<double>::pi);
        const double r = std::sin((pan + 1.0) * 0.25 * juce::MathConstants<double>::pi);
        L[i] += static_cast<float>(v * l);
        R[i] += static_cast<float>(v * r);
    };

    for (int bar = 0; bar < kBars; ++bar) {
        const Part part = partOf(bar);
        const int chord = bar % 4;
        const int barStart = bar * barN;
        const bool chorus = part == Part::Chorus;
        const double push = lastChorus(bar) ? 1.5 : 1.0;

        // ---- Kick: beats 1 and 3, four on the floor in the choruses.
        if (part != Part::Intro || bar >= 4)
            for (int b = 0; b < 4; ++b) {
                if (!chorus && (b % 2) == 1) continue;
                const int start = barStart + b * beatN;
                double phase = 0.0;
                for (int n = 0; n < static_cast<int>(0.35 * sr); ++n) {
                    const double t = n / sr;
                    phase += kTwoPi * (45.0 + 75.0 * std::exp(-t * 30.0)) / sr;
                    addMono(start + n, 0.55 * push * std::sin(phase) * std::exp(-t * 9.0), 0.0);
                }
            }

        // ---- Snare: beats 2 and 4 from the verse on.
        if (part != Part::Intro && part != Part::Outro)
            for (int b : {1, 3}) {
                const int start = barStart + b * beatN;
                OnePole hp, lp;
                for (int n = 0; n < static_cast<int>(0.2 * sr); ++n) {
                    const double t = n / sr;
                    const double noise = rng.nextDouble() * 2.0 - 1.0;
                    const double hi = noise - hp.process(noise, lpCoeff(900.0, sr));
                    const double body = lp.process(hi, lpCoeff(6000.0, sr));
                    const double tone = std::sin(kTwoPi * 185.0 * t);
                    addMono(start + n, 0.3 * (body + 0.6 * tone) * std::exp(-t * 18.0), 0.05);
                }
            }

        // ---- Hats: eighths; bright sixteenths in the choruses.
        if (part != Part::Intro) {
            const int steps = chorus ? 16 : 8;
            const double gain = chorus ? 0.16 : 0.09;
            for (int s = 0; s < steps; ++s) {
                const int start = barStart + s * barN / steps;
                OnePole hp;
                for (int n = 0; n < static_cast<int>(0.045 * sr); ++n) {
                    const double t = n / sr;
                    const double noise = rng.nextDouble() * 2.0 - 1.0;
                    const double hi = noise - hp.process(noise, lpCoeff(7000.0, sr));
                    addMono(start + n, gain * hi * std::exp(-t * 70.0), 0.35);
                }
            }
        }

        // ---- Bass: the root in eighths; heavier in the choruses.
        if (part != Part::Intro) {
            const double f = midiHz(roots[chord] - 24);
            const double gain = chorus ? 0.42 : 0.26;
            OnePole lp;
            double phase = 0.0;
            for (int e = 0; e < 8; ++e) {
                const int start = barStart + e * barN / 8;
                for (int n = 0; n < barN / 8; ++n) {
                    const double t = n / sr;
                    phase += f / sr;
                    const double saw = 2.0 * (phase - std::floor(phase)) - 1.0;
                    const double env = std::min(1.0, t * 200.0) * std::exp(-t * 3.0);
                    addMono(start + n, gain * push * lp.process(saw, lpCoeff(chorus ? 520.0 : 380.0, sr)) * env, 0.0);
                }
            }
        }

        // ---- Pad: the chord, detuned left and right (the stereo width).
        if (part != Part::Outro || bar < 108) {
            const int third = minor[chord] ? 3 : 4;
            const int notes[3] = {roots[chord], roots[chord] + third, roots[chord] + 7};
            const double gain = part == Part::Intro ? 0.07 : part == Part::Bridge ? 0.11 : 0.06;
            OnePole lpL, lpR;
            for (int n = 0; n < barN; ++n) {
                const int i = barStart + n;
                const double t = i / sr;
                double l = 0.0, r = 0.0;
                for (const int note : notes) {
                    const double f = midiHz(note);
                    const double pl = t * f * 1.003, pr = t * f * 0.997;
                    l += 2.0 * (pl - std::floor(pl)) - 1.0;
                    r += 2.0 * (pr - std::floor(pr)) - 1.0;
                }
                const double env = std::min(1.0, n / (0.3 * sr));
                const double c = lpCoeff(1800.0, sr);
                if (i < total) {
                    L[i] += static_cast<float>(gain * env * lpL.process(l, c));
                    R[i] += static_cast<float>(gain * env * lpR.process(r, c));
                }
            }
        }

        // ---- Lead: a hook in the choruses and the bridge.
        if (chorus || part == Part::Bridge) {
            static const int hook[8] = {0, 3, 7, 10, 7, 3, 5, 3};
            for (int s = 0; s < 8; ++s) {
                const double f = midiHz(roots[chord] + 12 + hook[s]);
                const int start = barStart + s * barN / 8;
                for (int n = 0; n < barN / 8; ++n) {
                    const double t = n / sr;
                    const double vib = 1.0 + 0.004 * std::sin(kTwoPi * 5.5 * t);
                    const double x = std::sin(kTwoPi * f * vib * t);
                    const double sq = std::tanh(2.5 * x);
                    addMono(start + n, 0.12 * push * sq * std::min(1.0, t * 80.0) * std::exp(-t * 2.5), -0.25);
                }
            }
        }
    }

    // Master: a gentle bus saturation; the last chorus runs hot on purpose.
    for (int i = 0; i < total; ++i) {
        L[i] = static_cast<float>(std::tanh(L[i] * 1.3));
        R[i] = static_cast<float>(std::tanh(R[i] * 1.3));
    }
    // Fade in / out.
    const int fade = static_cast<int>(2.0 * sr);
    for (int i = 0; i < fade && i < total; ++i) {
        const float g = static_cast<float>(i) / static_cast<float>(fade);
        L[i] *= g;
        R[i] *= g;
        L[total - 1 - i] *= g;
        R[total - 1 - i] *= g;
    }
    return out;
}

bool write(const juce::File& file, double sampleRate)
{
    const auto buffer = render(sampleRate);
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr) return false;
    juce::WavAudioFormat wav;
    const auto options =
        juce::AudioFormatWriterOptions{}.withSampleRate(sampleRate).withNumChannels(2).withBitsPerSample(24);
    auto writer = wav.createWriterFor(stream, options);
    if (writer == nullptr) return false;
    return writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
}

} // namespace khensu::demo_song
