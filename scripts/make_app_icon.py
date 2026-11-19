"""Draws the Khensu Mixing app icon (v1.0.0, 2026-11-18).

"Khensu" in the app's own typeface (Space Grotesk Bold, OFL - resources/fonts)
over a galaxy: deep-space gradient, teal / magenta / violet nebula clouds, a
spiral galaxy and a seeded star field. Sizes of 32 px and under carry a "K"
monogram instead, since the word cannot be read there.

Writes:
  resources/icons/khensu_icon_4096.png  the 4K master: JUCE ICON_BIG, from which the .exe icon
                                        (Explorer, taskbar, window) and the macOS .icns are made
  resources/icons/khensu_icon_1024.png  the releases page and documentation
  resources/icons/khensu_icon_256.png   JUCE ICON_SMALL
  resources/icons/khensu.ico            16-256 px - the installer, its shortcuts, Settings > Apps

    python scripts/make_app_icon.py

The star field and nebulae come from a fixed seed, so the output is the same
on every run.
"""
from __future__ import annotations

import math
import random
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "resources" / "icons"
FONT = ROOT / "resources" / "fonts" / "SpaceGrotesk" / "SpaceGrotesk-Bold.ttf"
SEED = 1308  # fixed: the same sky on every run

TEAL = (34, 211, 197)
MAGENTA = (240, 48, 143)
VIOLET = (124, 92, 255)
SPACE_TOP = (10, 8, 30)
SPACE_BOTTOM = (2, 3, 10)


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def galaxy(s: int) -> Image.Image:
    """The square sky at s x s pixels (no tile mask yet)."""
    rng = random.Random(SEED)
    sky = Image.new("RGB", (s, s))
    d = ImageDraw.Draw(sky)
    for y in range(s):
        d.line([(0, y), (s, y)], fill=lerp(SPACE_TOP, SPACE_BOTTOM, y / s))

    # Nebula clouds: soft colour blobs, screened onto the sky.
    neb = Image.new("RGB", (s, s), (0, 0, 0))
    nd = ImageDraw.Draw(neb)
    for colour, cx, cy, r in [(VIOLET, 0.30, 0.30, 0.42), (MAGENTA, 0.78, 0.72, 0.36),
                              (TEAL, 0.20, 0.80, 0.30), (VIOLET, 0.85, 0.20, 0.25)]:
        for _ in range(14):
            ox = (cx + rng.uniform(-0.12, 0.12)) * s
            oy = (cy + rng.uniform(-0.12, 0.12)) * s
            rr = r * s * rng.uniform(0.35, 0.7)
            c = lerp((0, 0, 0), colour, rng.uniform(0.18, 0.38))
            nd.ellipse([ox - rr, oy - rr, ox + rr, oy + rr], fill=c)
    neb = neb.filter(ImageFilter.GaussianBlur(s * 0.08))
    sky = ImageChops.screen(sky, neb)

    # Spiral galaxy: two logarithmic arms of dust behind the word.
    arms = Image.new("RGB", (s, s), (0, 0, 0))
    ad = ImageDraw.Draw(arms)
    gx, gy = s * 0.5, s * 0.5
    for arm in range(2):
        for i in range(2600):
            t = i / 2600 * 3.2 * math.pi
            rad = s * 0.035 * math.exp(0.32 * t)
            if rad > s * 0.62:
                break
            ang = t + arm * math.pi + rng.gauss(0, 0.22)
            jitter = rng.gauss(0, s * 0.018)
            x = gx + (rad + jitter) * math.cos(ang)
            y = gy + (rad + jitter) * math.sin(ang) * 0.62  # tilted disc
            mix = min(1.0, rad / (s * 0.5))
            c = lerp((255, 236, 250), lerp(TEAL, MAGENTA, (arm + mix) / 2), mix)
            c = lerp((0, 0, 0), c, rng.uniform(0.25, 0.75) * (1.0 - 0.5 * mix))
            pr = s * rng.uniform(0.002, 0.006)
            ad.ellipse([x - pr, y - pr, x + pr, y + pr], fill=c)
    core = s * 0.09
    ad.ellipse([gx - core, gy - core * 0.62, gx + core, gy + core * 0.62], fill=(200, 170, 230))
    arms = arms.filter(ImageFilter.GaussianBlur(s * 0.006))
    glow = arms.filter(ImageFilter.GaussianBlur(s * 0.03))
    sky = ImageChops.screen(sky, glow)
    sky = ImageChops.screen(sky, arms)

    # Star field: many faint points, a few bright ones with diffraction spikes.
    stars = Image.new("RGB", (s, s), (0, 0, 0))
    sd = ImageDraw.Draw(stars)
    for _ in range(int(900 * (s / 1024) ** 0.5) + 40):
        x, y = rng.uniform(0, s), rng.uniform(0, s)
        b = rng.random() ** 2.2
        pr = s * (0.0008 + 0.0022 * b)
        tint = rng.choice([(255, 255, 255), (200, 225, 255), (255, 220, 240), (210, 255, 248)])
        sd.ellipse([x - pr, y - pr, x + pr, y + pr], fill=lerp((0, 0, 0), tint, 0.35 + 0.65 * b))
    for _ in range(7):
        x, y = rng.uniform(0.08, 0.92) * s, rng.uniform(0.08, 0.92) * s
        if abs(y - s * 0.5) < s * 0.14:  # keep the bright ones off the word
            y += s * 0.22 * (1 if y > s * 0.5 else -1)
        L = s * rng.uniform(0.025, 0.05)
        w = max(1, round(s * 0.0016))
        sd.line([(x - L, y), (x + L, y)], fill=(230, 240, 255), width=w)
        sd.line([(x, y - L), (x, y + L)], fill=(230, 240, 255), width=w)
        pr = s * 0.005
        sd.ellipse([x - pr, y - pr, x + pr, y + pr], fill=(255, 255, 255))
    sky = ImageChops.screen(sky, stars.filter(ImageFilter.GaussianBlur(s * 0.0012)))
    return sky


def wordmark(s: int, text: str, width_frac: float) -> tuple[Image.Image, Image.Image]:
    """(glow layer, text mask) for `text` centred at width_frac of s."""
    size = 10
    font = ImageFont.truetype(str(FONT), size)
    while font.getbbox(text)[2] - font.getbbox(text)[0] < s * width_frac:
        size += 4
        font = ImageFont.truetype(str(FONT), size)
    left, top, right, bottom = font.getbbox(text)
    x = (s - (right - left)) / 2 - left
    y = (s - (bottom - top)) / 2 - top
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).text((x, y), text, font=font, fill=255)
    # Glow: teal on the left, magenta on the right, blurred behind the letters.
    grad = Image.new("RGB", (s, s))
    gd = ImageDraw.Draw(grad)
    for xx in range(s):
        gd.line([(xx, 0), (xx, s)], fill=lerp(TEAL, MAGENTA, xx / s))
    glow = Image.new("RGB", (s, s), (0, 0, 0))
    glow.paste(grad, (0, 0), mask.filter(ImageFilter.GaussianBlur(s * 0.03)))
    return glow, mask


def draw(size: int, ss: int = 4) -> Image.Image:
    s = size * ss
    sky = galaxy(s)
    text, frac = ("K", 0.42) if size <= 32 else ("Khensu", 0.78)
    glow, mask = wordmark(s, text, frac)
    # A soft dark halo first, so the letters stay crisp over the bright core.
    halo = mask.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(s * 0.012))
    sky = Image.composite(Image.new("RGB", (s, s), (6, 4, 18)), sky, halo.point(lambda v: v * 0.7))
    sky = ImageChops.screen(sky, glow)
    sky = ImageChops.screen(sky, glow)
    sky.paste((250, 250, 255), (0, 0), mask)

    # Rounded tile with a thin teal-to-magenta rim.
    out = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    margin = round(s * 0.03)
    radius = round(s * 0.22)
    tile = Image.new("L", (s, s), 0)
    ImageDraw.Draw(tile).rounded_rectangle([margin, margin, s - margin, s - margin], radius=radius, fill=255)
    out.paste(sky, (0, 0), tile)
    rim = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    rd = ImageDraw.Draw(rim)
    rw = max(1, round(s * 0.008))
    for i in range(rw):
        c = lerp(TEAL, MAGENTA, i / max(1, rw - 1))
        rd.rounded_rectangle([margin + i, margin + i, s - margin - i, s - margin - i], radius=radius - i,
                             outline=c + (170,), width=1)
    out.alpha_composite(rim)
    return out.resize((size, size), Image.Resampling.LANCZOS)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    master = draw(4096, ss=1)
    master.save(OUT / "khensu_icon_4096.png", optimize=True)
    master.resize((1024, 1024), Image.Resampling.LANCZOS).save(OUT / "khensu_icon_1024.png", optimize=True)
    draw(256).save(OUT / "khensu_icon_256.png")
    sizes = [16, 24, 32, 48, 64, 128, 256]
    # Each .ico size drawn at its own resolution so small sizes stay crisp.
    frames = [draw(n) for n in sizes]
    frames[-1].save(OUT / "khensu.ico", format="ICO", sizes=[(n, n) for n in sizes], append_images=frames[:-1])
    print(f"wrote {OUT / 'khensu_icon_4096.png'}, khensu_icon_1024.png, khensu_icon_256.png, khensu.ico")


if __name__ == "__main__":
    main()
