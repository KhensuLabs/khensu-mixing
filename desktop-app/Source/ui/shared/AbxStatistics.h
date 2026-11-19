// desktop-app/Source/ui/shared/AbxStatistics.h
// Binomial statistics for the blind ABX trial (audit 2026-11-06 §10.3): a
// raw "7 / 10" says nothing about whether the listener really hears a
// difference. P(X >= correct) under pure guessing (p = 0.5) does.
// Header-only so it is unit-tested without the overlay's AppController.
#pragma once
#include <JuceHeader.h>
#include <cmath>

namespace khensu::abx_stats {

/// One-sided binomial p-value: P(X >= correct), X ~ Binomial(total, 0.5).
inline double binomialPValue(int correct, int total) noexcept
{
    if (total <= 0) return 1.0;
    correct = juce::jlimit(0, total, correct);
    double p = 0.0;
    for (int k = correct; k <= total; ++k) {
        const double logC = std::lgamma((double)total + 1.0) - std::lgamma((double)k + 1.0) -
                            std::lgamma((double)(total - k) + 1.0);
        p += std::exp(logC - (double)total * std::log(2.0));
    }
    return juce::jlimit(0.0, 1.0, p);
}

/// Fewest correct answers out of `total` with p < 0.05; -1 if impossible.
inline int correctNeededForSignificance(int total) noexcept
{
    for (int c = 0; c <= total; ++c)
        if (binomialPValue(c, total) < 0.05) return c;
    return -1;
}

/// Fewest trials in which a perfect run reaches p < 0.05 (5: 0.5^5 = 0.031).
inline int minimumTrialsForSignificance() noexcept
{
    for (int n = 1; n < 64; ++n)
        if (correctNeededForSignificance(n) >= 0) return n;
    return 64;
}

/// "Score: 7 / 8 (87%)  p = 0.035 - very unlikely to be guessing".
inline juce::String describeScore(int correct, int total)
{
    juce::String s = TRANS("Score:") + " " + juce::String(correct) + " / " + juce::String(total) + "  (" +
                     juce::String(total > 0 ? correct * 100 / total : 0) + "%)";
    if (total <= 0) return s;
    const double p = binomialPValue(correct, total);
    s << "  p = " << juce::String(p, 3) << " - ";
    if (p < 0.05) {
        s << TRANS("very unlikely to be guessing");
    } else {
        s << TRANS("could still be guessing");
        const int minTrials = minimumTrialsForSignificance();
        if (total < minTrials)
            s << " (" << TRANS("at least") << " " << juce::String(minTrials) << " " << TRANS("trials needed") << ")";
    }
    return s;
}

} // namespace khensu::abx_stats
