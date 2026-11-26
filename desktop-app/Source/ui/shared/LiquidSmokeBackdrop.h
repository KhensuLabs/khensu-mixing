// desktop-app/Source/ui/shared/LiquidSmokeBackdrop.h
// LG-1 / LG-2 / LG-3 / LG-4: Direct2D-native animated smoke / volumetric plume
// backdrop + shared blurred glass-refraction frame.
//
// Renders a slow, organic domain-warped smoke field into a tiny offscreen
// software buffer (160x90, matching renderBlurredBackdropSoftware's ~0.12x
// scale). Upscaled bilinearly by Direct2D for the window background, and
// 1-pass blurred + tinted for every PanelCard / chrome rail so all glass
// surfaces automatically refract the moving smoke behind them at minimal CPU.
#pragma once
#include <JuceHeader.h>
#include "KhensuTheme.h"
#include "ReducedMotion.h"
#include <atomic>
#include <cmath>
#include <cstdint>

namespace khensu {

class LiquidSmokeBackdrop final {
  public:
    static constexpr int kBufW = 160;
    static constexpr int kBufH = 90;

    static LiquidSmokeBackdrop& instance()
    {
        static LiquidSmokeBackdrop inst;
        return inst;
    }

    /// Settings toggle (LG-4): persisted as ui.ambient_backdrop (default true).
    static void setAmbientMotionEnabled(bool enabled) noexcept
    {
        ambientEnabled_.store(enabled, std::memory_order_relaxed);
    }

    static bool isAmbientMotionEnabled() noexcept { return ambientEnabled_.load(std::memory_order_relaxed); }

    /// True when the smoke should animate (motion toggle on + reduced motion off).
    static bool shouldAnimate() noexcept { return isAmbientMotionEnabled() && !ui::motion::reduced(); }

    /// Advances the smoke simulation by `dtSeconds` with optional live audio
    /// energy in [0, 1] (LG-3). Re-renders the tiny 160x90 smoke buffer and
    /// increments the frame generation so glass backdrops refresh.
    void advance(float dtSeconds, float audioEnergy01 = 0.0f)
    {
        dtSeconds = juce::jlimit(0.0f, 0.1f, dtSeconds);
        const float targetEnergy = juce::jlimit(0.0f, 1.0f, audioEnergy01);
        // Smooth low-pass filter so audio reactivity feels liquid, never jittery.
        const float alpha = 1.0f - std::exp(-dtSeconds * 4.5f);
        smoothedEnergy_ += (targetEnergy - smoothedEnergy_) * alpha;

        // Subtle speed boost while audio is playing (1.0x idle -> 1.65x loud).
        const float speed = 1.0f + 0.65f * smoothedEnergy_;
        phase_ += dtSeconds * speed;
        if (phase_ > 10000.0f) phase_ -= 10000.0f;

        renderFrame();
    }

    /// Ensures a valid frame exists for the current theme (used in reduced-motion
    /// mode, tests, or before the first VBlank tick).
    void ensureFrame()
    {
        const auto key = currentPaletteKey();
        if (!smokeNative_.isValid() || paletteKey_ != key) {
            renderFrame();
        }
    }

    /// The native 160x90 smoke image for fast hardware bilinear upscale.
    const juce::Image& smokeImage()
    {
        ensureFrame();
        return smokeNative_;
    }

    /// The 160x90 software smoke image (used to build blurred glass backdrops).
    const juce::Image& smokeSoftwareImage()
    {
        ensureFrame();
        return smokeSoft_;
    }

    /// Monotonically increasing counter bumped whenever the smoke buffer or
    /// theme palette changes, so cached glass backdrops know when to refresh.
    juce::uint64 generation()
    {
        ensureFrame();
        return generation_;
    }

    /// Deterministic tileable 128x128 monochrome micro-grain texture (LG-7)
    /// for frosted crystal surfaces (eliminates dark-mode gradient banding).
    static const juce::Image& microGrainTexture()
    {
        static juce::Image grain = []() {
            constexpr int kSize = 128;
            juce::Image img(juce::Image::ARGB, kSize, kSize, true, juce::SoftwareImageType());
            juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);

            // Fast high-frequency interleaved gradient noise (blue-noise-like
            // spatial distribution without clumping).
            for (int y = 0; y < kSize; ++y) {
                juce::uint8* row = bd.getLinePointer(y);
                for (int x = 0; x < kSize; ++x) {
                    const float fx = static_cast<float>(x);
                    const float fy = static_cast<float>(y);
                    const float n1 =
                        std::fmod(52.9829189f * std::fmod(0.06711056f * fx + 0.00583715f * fy, 1.0f), 1.0f);
                    const float n2 =
                        std::fmod(43.7585453f * std::fmod(0.03191f * fx + 0.09137f * fy + 0.37f, 1.0f), 1.0f);
                    const float v = juce::jlimit(0.0f, 1.0f, 0.5f * (n1 + n2));
                    const auto lum = static_cast<juce::uint8>(v * 255.0f);
                    // Premultiplied ARGB with subtle alpha (~9 / 255 ≈ 3.5%).
                    constexpr juce::uint8 a = 9;
                    const auto p = static_cast<juce::uint8>((static_cast<int>(lum) * a) / 255);
                    juce::uint8* px = row + x * 4;
                    px[0] = p;
                    px[1] = p;
                    px[2] = p;
                    px[3] = a;
                }
            }
            return juce::NativeImageType().convert(img);
        }();
        return grain;
    }

  private:
    LiquidSmokeBackdrop() = default;

    static juce::uint64 currentPaletteKey()
    {
        juce::uint64 key = static_cast<juce::uint64>(KhensuTheme::currentTheme());
        for (auto c : {KhensuTheme::bgVoid, KhensuTheme::bgBase, KhensuTheme::accentTeal, KhensuTheme::accentMagenta})
            key = key * 1000003u ^ c.getARGB();
        return key;
    }

    void renderFrame()
    {
        if (!smokeSoft_.isValid())
            smokeSoft_ = juce::Image(juce::Image::ARGB, kBufW, kBufH, false, juce::SoftwareImageType());

        const auto theme = KhensuTheme::currentTheme();
        const bool isLight = (theme == KhensuTheme::Theme::Light);
        const bool isHC = (theme == KhensuTheme::Theme::HighContrast);

        // Base studio void and lifted obsidian/alabaster endpoints.
        const auto cVoid = KhensuTheme::bgVoid;
        const auto cBase = KhensuTheme::bgBase;
        const auto cTeal = KhensuTheme::accentTeal;
        const auto cIndigo = isLight ? KhensuTheme::accentBlue.interpolatedWith(cBase, 0.70f)
                                     : KhensuTheme::accentBlue.interpolatedWith(cVoid, 0.75f);
        const auto cMagenta = KhensuTheme::accentMagenta;

        // Pro-audio guardrails (LG-4): dark, calm, studio-grade contrast.
        const float energyLift = 1.0f + 0.25f * smoothedEnergy_;
        const float tealStrength = (isHC ? 0.045f : isLight ? 0.085f : 0.135f) * energyLift;
        const float indigoStrength = (isHC ? 0.030f : isLight ? 0.095f : 0.160f) * energyLift;
        const float magentaStrength = (isHC ? 0.008f : isLight ? 0.025f : 0.042f) * energyLift;

        const float t = phase_;
        const float t1 = t * 0.22f;
        const float t2 = t * 0.15f;
        const float t3 = t * 0.31f;

        const float baseR0 = cVoid.getFloatRed(), baseG0 = cVoid.getFloatGreen(), baseB0 = cVoid.getFloatBlue();
        const float baseR1 = cBase.getFloatRed(), baseG1 = cBase.getFloatGreen(), baseB1 = cBase.getFloatBlue();
        const float tealR = cTeal.getFloatRed(), tealG = cTeal.getFloatGreen(), tealB = cTeal.getFloatBlue();
        const float indR = cIndigo.getFloatRed(), indG = cIndigo.getFloatGreen(), indB = cIndigo.getFloatBlue();
        const float magR = cMagenta.getFloatRed(), magG = cMagenta.getFloatGreen(), magB = cMagenta.getFloatBlue();

        juce::Image::BitmapData out(smokeSoft_, juce::Image::BitmapData::writeOnly);

        constexpr float invW = 1.0f / static_cast<float>(kBufW);
        constexpr float invH = 1.0f / static_cast<float>(kBufH);

        // Precompute gentle drifting plume centres.
        const float p1x = 0.18f + 0.14f * std::sin(t1 * 0.9f);
        const float p1y = 0.20f + 0.16f * std::cos(t2 * 1.1f);
        const float p2x = 0.78f + 0.12f * std::cos(t1 * 0.7f + 1.2f);
        const float p2y = 0.72f + 0.15f * std::sin(t2 * 0.85f + 0.5f);
        const float p3x = 0.52f + 0.18f * std::sin(t2 * 0.65f + 2.4f);
        const float p3y = 0.35f + 0.18f * std::cos(t1 * 0.8f + 1.9f);

        for (int y = 0; y < kBufH; ++y) {
            const float ny = (static_cast<float>(y) + 0.5f) * invH;
            juce::uint8* row = out.getLinePointer(y);

            for (int x = 0; x < kBufW; ++x) {
                const float nx = (static_cast<float>(x) + 0.5f) * invW;

                // Domain-warped coordinates: two low-frequency curls create
                // slow-moving volumetric smoke ribbons without sharp artifacts.
                const float wx = nx + 0.16f * std::sin(ny * 4.2f + t1) + 0.08f * std::cos((nx + ny) * 6.5f - t3);
                const float wy = ny + 0.16f * std::cos(nx * 3.8f - t2) + 0.08f * std::sin((nx - ny) * 6.0f + t1);

                // Layered smooth smoke density waves.
                const float f1 = 0.5f + 0.5f * std::sin(wx * 5.2f + wy * 2.9f + t1 * 1.3f);
                const float f2 = 0.5f + 0.5f * std::cos(wx * 3.4f - wy * 5.6f - t2 * 1.4f);
                const float f3 = 0.5f + 0.5f * std::sin((wx + wy) * 7.8f + t3 * 0.8f);
                const float smoke = f1 * 0.50f + f2 * 0.35f + f3 * 0.15f;

                // Volumetric plume falloffs (Gaussian-like).
                const float d1x = wx - p1x, d1y = (wy - p1y) * 0.75f;
                const float plumeTeal = std::exp(-(d1x * d1x + d1y * d1y) * 3.2f) * (0.45f + 0.55f * smoke);

                const float d2x = wx - p2x, d2y = (wy - p2y) * 0.75f;
                const float plumeIndigo = std::exp(-(d2x * d2x + d2y * d2y) * 2.6f) * (0.40f + 0.60f * f2);

                const float d3x = wx - p3x, d3y = (wy - p3y) * 0.80f;
                const float plumeMagenta = std::exp(-(d3x * d3x + d3y * d3y) * 4.5f) * (0.35f + 0.65f * f1 * f3);

                // Subtle top-left base wash from bgBase -> bgVoid.
                const float wash = juce::jlimit(0.0f, 1.0f, 1.0f - (nx * 0.55f + ny * 0.75f));
                float r = baseR0 + (baseR1 - baseR0) * wash;
                float g = baseG0 + (baseG1 - baseG0) * wash;
                float b = baseB0 + (baseB1 - baseB0) * wash;

                // Composite drifting smoke layers.
                const float wTeal = juce::jlimit(0.0f, 1.0f, plumeTeal * tealStrength);
                const float wInd = juce::jlimit(0.0f, 1.0f, plumeIndigo * indigoStrength);
                const float wMag = juce::jlimit(0.0f, 1.0f, plumeMagenta * magentaStrength);

                r += (tealR - r) * wTeal + (indR - r) * wInd + (magR - r) * wMag;
                g += (tealG - g) * wTeal + (indG - g) * wInd + (magG - g) * wMag;
                b += (tealB - b) * wTeal + (indB - b) * wInd + (magB - b) * wMag;

                juce::uint8* px = row + x * 4;
                // JUCE SoftwareImageType ARGB is BGRA in memory on little-endian Windows.
                px[0] = static_cast<juce::uint8>(juce::jlimit(0.0f, 1.0f, b) * 255.0f);
                px[1] = static_cast<juce::uint8>(juce::jlimit(0.0f, 1.0f, g) * 255.0f);
                px[2] = static_cast<juce::uint8>(juce::jlimit(0.0f, 1.0f, r) * 255.0f);
                px[3] = 255;
            }
        }

        smokeNative_ = juce::NativeImageType().convert(smokeSoft_);
        paletteKey_ = currentPaletteKey();
        ++generation_;
    }

    juce::Image smokeSoft_;
    juce::Image smokeNative_;
    juce::uint64 paletteKey_ = 0;
    juce::uint64 generation_ = 0;
    float phase_ = 1.4f; // non-zero initial phase for a rich static frame on frame 0
    float smoothedEnergy_ = 0.0f;
    static inline std::atomic<bool> ambientEnabled_{true};
};

} // namespace khensu
