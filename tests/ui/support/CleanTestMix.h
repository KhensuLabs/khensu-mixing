// tests/ui/support/CleanTestMix.h
// OVH-9: the deterministic "clean mix" and RBJ shelf defect of the MEAS-4
// injected-defect benchmark (tests/shared-core/test_defect_benchmark.cpp,
// KhensuCoreTests), copied for the KhensuTests end-to-end fix loop: the two
// test executables do not share sources. Keep the two in step.
#pragma once
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace khensu::test::clean_mix {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 44100;
constexpr int kSeconds = 16;
constexpr int kFrames = kRate * kSeconds;

using Stereo = std::vector<std::vector<float>>; // [channel][frame]

inline float onePole(float x, float& state, float coeff)
{
    state += coeff * (x - state);
    return state;
}

/// Kick, bass, a stereo pad, a centred lead, hats and a stereo "air" bed at
/// 120 BPM, a quiet verse then a full chorus: tuned so the clean mix sits
/// inside the default genre's targets (centroid, tilt, width, LRA, low end),
/// which is what lets a defect be the only thing that moves a finding.
/// Deterministic (fixed seed).
inline Stereo cleanMix()
{
    Stereo y(2, std::vector<float>(kFrames, 0.0f));
    std::mt19937 rng(20261113);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const int beat = kRate / 2; // 120 BPM
    const double bassNotes[] = {55.0, 55.0, 73.42, 65.41};
    const double padRoots[] = {220.0, 220.0, 293.66, 261.63};
    float bassLp = 0.0f, hatHp = 0.0f, hatPrev = 0.0f;
    float airPrev[2] = {0.0f, 0.0f}, airHp[2] = {0.0f, 0.0f};
    double bassPhase = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        const int bar = (i / (beat * 4)) % 4;
        const int inBeat = i % beat;
        const double tb = static_cast<double>(inBeat) / kRate;

        // Kick on every beat: a 120 -> 50 Hz sweep with a fast decay.
        const double kickF = 50.0 + 70.0 * std::exp(-tb * 30.0);
        const float kick = static_cast<float>(0.25 * std::exp(-tb * 9.0) * std::sin(2.0 * kPi * kickF * tb));

        // Bass: a band-limited saw-ish tone, low-passed.
        bassPhase += bassNotes[bar] / kRate;
        const double frac = bassPhase - std::floor(bassPhase);
        const float saw = static_cast<float>(2.0 * frac - 1.0);
        const float bass = 0.09f * onePole(saw, bassLp, 0.05f);

        // Pad: a triad with harmonics, voiced differently per channel (root and
        // fifth left, third and octave right) and detuned: a wide image.
        float pad[2] = {0.0f, 0.0f};
        for (int ch = 0; ch < 2; ++ch) {
            const double detune = ch == 0 ? 0.99 : 1.01;
            for (const double ratio : (ch == 0 ? std::vector<double>{1.0, 1.5, 2.52} : std::vector<double>{1.26, 2.0, 3.0}))
                for (int h = 1; h <= 8; ++h)
                    pad[ch] += static_cast<float>(0.035 / h *
                                                  std::sin(2.0 * kPi * padRoots[bar] * ratio * h * detune * t + ch * 0.7 * h));
        }

        // Lead: a vibrato tone in the vocal range, centred, phrased in bars.
        const double leadEnv = 0.5 - 0.5 * std::cos(2.0 * kPi * std::fmod(t, 2.0) / 2.0);
        const float lead = static_cast<float>(0.12 * leadEnv *
                                              std::sin(2.0 * kPi * (523.25 * t + 0.02 * std::sin(2.0 * kPi * 5.0 * t) / (2.0 * kPi * 5.0) * 523.25)));

        // Hats on the off-eighths: high-passed noise bursts, panned right.
        const int eighth = beat / 2;
        const bool offEighth = ((i / eighth) % 2) == 1;
        const double te = static_cast<double>(i % eighth) / kRate;
        const float n = noise(rng);
        const float hp = n - hatPrev + 0.6f * hatHp; // a crude high-pass
        hatPrev = n;
        hatHp = hp;
        const float hat = offEighth ? static_cast<float>(0.16 * std::exp(-te * 40.0)) * hp : 0.0f;

        // A light noise bed so the noise-floor and correlation detectors see
        // a real recording, not digital silence between notes.
        // An independent high-passed noise "air" per channel: the top octaves
        // and the decorrelated part of the image a real room or reverb gives.
        float air[2];
        for (int ch = 0; ch < 2; ++ch) {
            const float a = noise(rng);
            airHp[ch] = a - airPrev[ch] + 0.3f * airHp[ch];
            airPrev[ch] = a;
            air[ch] = 0.004f * airHp[ch];
        }

        // Two hard-panned plucks (left on the beat, right on the off-beat):
        // the side signal a real stereo arrangement has.
        const double pluckEnv = std::exp(-tb * 12.0);
        const float pluckL = static_cast<float>(0.27 * pluckEnv * std::sin(2.0 * kPi * padRoots[bar] * 3.0 * tb));
        const double tOff = static_cast<double>((i + beat / 2) % beat) / kRate;
        const float pluckR = static_cast<float>(0.27 * std::exp(-tOff * 12.0) * std::sin(2.0 * kPi * padRoots[bar] * 4.0 * tOff));

        // Verse (first half) about 4 dB down without the kick; chorus full.
        const bool chorus = i >= kFrames / 2;
        const float section = chorus ? 1.0f : 0.55f;
        const float rhythm = chorus ? 1.0f : 0.0f;
        y[0][static_cast<size_t>(i)] = section * (rhythm * kick + bass + pad[0] + lead + 0.6f * hat + air[0] + pluckL);
        y[1][static_cast<size_t>(i)] = section * (rhythm * kick + bass + pad[1] + lead + 1.0f * hat + air[1] + pluckR);
    }
    // A gentle "master": soft saturation brings the crest factor near a
    // finished mix's, peaking at -3 dBFS (no sample reaches full scale).
    float peak = 0.0f;
    for (const auto& ch : y)
        for (const float s : ch) peak = std::max(peak, std::abs(s));
    const float drive = 2.0f / std::max(peak, 1e-6f);
    const float norm = 0.7f / std::tanh(2.0f);
    for (auto& ch : y)
        for (auto& s : ch) s = norm * std::tanh(drive * s);
    return y;
}

// --- defects -----------------------------------------------------------------

/// RBJ shelf (low or high), applied to both channels.
inline void shelf(Stereo& y, bool high, double freq, double gainDb)
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * freq / kRate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / 2.0 * std::sqrt(2.0); // S = 1
    const double sq = 2.0 * std::sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    if (!high) {
        b0 = A * ((A + 1) - (A - 1) * cw + sq);
        b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - sq);
        a0 = (A + 1) + (A - 1) * cw + sq;
        a1 = -2 * ((A - 1) + (A + 1) * cw);
        a2 = (A + 1) + (A - 1) * cw - sq;
    } else {
        b0 = A * ((A + 1) + (A - 1) * cw + sq);
        b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - sq);
        a0 = (A + 1) - (A - 1) * cw + sq;
        a1 = 2 * ((A - 1) - (A + 1) * cw);
        a2 = (A + 1) - (A - 1) * cw - sq;
    }
    for (auto& ch : y) {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        for (auto& s : ch) {
            const double x0 = s;
            const double out = (b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
            x2 = x1;
            x1 = x0;
            y2 = y1;
            y1 = out;
            s = static_cast<float>(out);
        }
    }
}

} // namespace khensu::test::clean_mix
