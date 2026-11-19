"""One-off script: expand bootstrap_references.json from 5 seeded genres to
all 129 in genre_targets.json.

genre_targets.json has no spectral_tilt_db_per_octave/crest_factor_db fields
(those are computed at load time, not stored as targets). This mirrors the
*exact* derivation shared-core/src/targets/GenreDatabase.cpp already applies
(derivedTilt/derivedCrest + the same hand-tuned per-genre overrides), so the
Reference Library's point cloud lines up with the tilt/crest targets every
other panel (SpectralPanel, DynamicsRecommender, etc.) scores against,
instead of introducing a second, divergent formula. Keep the two in sync if
either changes.

Still explicitly not real reference-track data — same as the original
5-genre seed. Re-run after adding more genres to genre_targets.json.
"""
import json
import pathlib
import random

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
TARGETS_PATH = REPO_ROOT / "resources" / "genre_norms" / "genre_targets.json"
BOOTSTRAP_PATH = (REPO_ROOT / "local-services" / "khensu_services" / "analysis"
                  / "reference_library" / "bootstrap_references.json")

N_POINTS_PER_GENRE = 12

# 10*log10(2): GenreDatabase.cpp's tilt formula and overrides are written in
# the old per-bin-density convention and shifted by this into the
# octave-band-power convention the analyzers measure (AUDIT-009,
# kTiltDensityToBandPowerDb). Applied to every tilt this script emits.
TILT_DENSITY_TO_BAND_POWER_DB = 3.0103

# Mirrors GenreDatabase.cpp's tiltCrestOverrides() exactly (density
# convention; converted in main()).
TILT_CREST_OVERRIDES = {
    "shoegaze":      (-3.0, 8.0),
    "hyperpop":      (-1.0, 6.0),
    "dream-pop":     (-5.0, 10.0),
    "ambient":       (-7.0, 14.0),
    "dark-ambient":  (-8.0, 15.0),
    "lo-fi-hip-hop": (-8.0, 12.0),
    "lo-fi-beats":   (-8.0, 12.0),
}


def sample_around(mean: float, spread: float, n: int) -> list[float]:
    return [round(random.gauss(mean, spread), 2) for _ in range(n)]


def derive_tilt(centroid_hz: float) -> float:
    # Mirrors GenreDatabase.cpp's derivedTilt (density convention).
    return max(-9.0, min(-1.0, -4.5 + (centroid_hz - 3000.0) / 3000.0 * 2.0))


def derive_crest(compression_ratio: float) -> float:
    # Mirrors GenreDatabase.cpp's derivedCrest.
    return max(5.0, min(16.0, 14.0 - compression_ratio * 1.2))


def main():
    targets = json.loads(TARGETS_PATH.read_text(encoding="utf-8"))["genres"]
    bootstrap = json.loads(BOOTSTRAP_PATH.read_text(encoding="utf-8"))

    random.seed(42)  # reproducible generation
    added = 0
    for target in targets:
        genre_id = target["id"]
        if genre_id in bootstrap["genres"]:
            continue  # keep the 5 hand-seeded genres untouched

        lufs_mean = target["lufs_target"]
        lra_mean = sum(target["lra_range"]) / 2.0

        if genre_id in TILT_CREST_OVERRIDES:
            tilt_mean, crest_mean = TILT_CREST_OVERRIDES[genre_id]
        else:
            tilt_mean = derive_tilt(target["spectral_centroid_hz"])
            crest_mean = derive_crest(target["compression_ratio_typical"])
        tilt_mean += TILT_DENSITY_TO_BAND_POWER_DB

        points = []
        for _ in range(N_POINTS_PER_GENRE):
            points.append({
                "lufs_integrated": sample_around(lufs_mean, 1.0, 1)[0],
                "lra": sample_around(lra_mean, 1.5, 1)[0],
                "spectral_tilt_db_per_octave": sample_around(tilt_mean, 0.8, 1)[0],
                "crest_factor_db": sample_around(crest_mean, 1.2, 1)[0],
            })
        bootstrap["genres"][genre_id] = points
        added += 1

    bootstrap["note"] = (
        bootstrap["note"] + " Expanded 2026-07-09 to cover all 129 genres in "
        "genre_targets.json: lufs/lra sampled around genre_targets.json's own "
        "lufs_target/lra_range; tilt/crest derived via the exact same formula "
        "(+ hand-tuned overrides) as GenreDatabase.cpp's derivedTilt/derivedCrest, "
        "so this point cloud agrees with every other panel's genre-target curve "
        "(see expand_bootstrap_references.py) — still not real analyzed "
        "reference tracks."
    )
    BOOTSTRAP_PATH.write_text(json.dumps(bootstrap, indent=2) + "\n", encoding="utf-8")
    print(f"Added {added} genres; bootstrap now covers {len(bootstrap['genres'])} genres")


if __name__ == "__main__":
    main()
