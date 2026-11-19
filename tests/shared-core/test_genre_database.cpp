#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <khensu/targets/GenreDatabase.h>

using namespace khensu;

// Minimal genre JSON to avoid filesystem dependency.
static const char* kMinimalGenreJson = R"({
    "genres": [
        {
            "id": "trap",
            "name": "Trap",
            "parent": "hip-hop",
            "lufs_target": -8.0,
            "lufs_range": [-10.0, -7.0],
            "lra_range": [4.0, 7.0],
            "true_peak_max": -0.5,
            "spectral_centroid_hz": 2800.0,
            "low_end_weight": 0.65,
            "stereo_width_range": [0.55, 0.9],
            "compression_ratio_typical": 6.0,
            "heaviness_distortion_norm": 0.1,
            "heaviness_weight_norm": 0.8,
            "heaviness_clarity_norm": 0.5,
            "heaviness_extremity_norm": 0.15,
            "hook_density_norm": 0.8,
            "chorus_contrast_norm": 0.6,
            "typical_bpm_range": [130.0, 160.0],
            "vocal_presence_db": 2.0,
            "notes": "Sub-heavy, 808-driven. Vocal clarity critical."
        },
        {
            "id": "ambient",
            "name": "Ambient",
            "parent": "electronic",
            "lufs_target": -23.0,
            "lufs_range": [-26.0, -18.0],
            "lra_range": [12.0, 20.0],
            "true_peak_max": -1.0,
            "spectral_centroid_hz": 1500.0,
            "low_end_weight": 0.25,
            "stereo_width_range": [0.7, 1.0],
            "compression_ratio_typical": 1.5,
            "heaviness_distortion_norm": 0.0,
            "heaviness_weight_norm": 0.1,
            "heaviness_clarity_norm": 0.9,
            "heaviness_extremity_norm": 0.0,
            "hook_density_norm": 0.1,
            "chorus_contrast_norm": 0.2,
            "typical_bpm_range": [60.0, 100.0],
            "vocal_presence_db": -3.0,
            "notes": "Wide, spacious. Dynamics preserved, minimal compression."
        }
    ]
})";

TEST_CASE("GenreDatabase::loadFromString succeeds with valid JSON", "[genre]")
{
    GenreDatabase db;
    auto result = db.loadFromString(kMinimalGenreJson);
    REQUIRE(result.is_ok());
    CHECK(db.isLoaded());
    CHECK(db.size() == 2);
}

TEST_CASE("GenreDatabase::loadFromString fails on invalid JSON", "[genre]")
{
    GenreDatabase db;
    auto result = db.loadFromString("{not valid json");
    CHECK(result.is_err());
    CHECK_FALSE(db.isLoaded());
}

TEST_CASE("GenreDatabase::loadFromString fails when genres key is missing", "[genre]")
{
    GenreDatabase db;
    auto result = db.loadFromString(R"({"not_genres": []})");
    CHECK(result.is_err());
}

TEST_CASE("GenreDatabase::findById returns correct entry", "[genre]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(kMinimalGenreJson).is_ok());

    auto entry = db.findById("trap");
    REQUIRE(entry.has_value());
    CHECK(entry->id == "trap");
    CHECK(entry->name == "Trap");
    CHECK(entry->parent == "hip-hop");
}

TEST_CASE("GenreDatabase::findById returns nullopt for unknown id", "[genre]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(kMinimalGenreJson).is_ok());

    auto entry = db.findById("shoegaze");
    CHECK_FALSE(entry.has_value());
}

TEST_CASE("GenreDatabase::findByName is case-insensitive", "[genre]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(kMinimalGenreJson).is_ok());

    CHECK(db.findByName("TRAP").has_value());
    CHECK(db.findByName("Trap").has_value());
    CHECK(db.findByName("trap").has_value());
    CHECK(db.findByName("ambient").has_value());
    CHECK(db.findByName("AMBIENT").has_value());
}

TEST_CASE("GenreDatabase::findByName returns nullopt for unknown name", "[genre]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(kMinimalGenreJson).is_ok());

    CHECK_FALSE(db.findByName("metal").has_value());
}

TEST_CASE("GenreDatabase scalar fields parsed correctly for trap", "[genre]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(kMinimalGenreJson).is_ok());

    auto entry = db.findById("trap");
    REQUIRE(entry.has_value());

    CHECK_THAT(entry->lufs_target, Catch::Matchers::WithinAbs(-8.0f, 0.001f));
    CHECK_THAT(entry->lufs_range_min, Catch::Matchers::WithinAbs(-10.0f, 0.001f));
    CHECK_THAT(entry->lufs_range_max, Catch::Matchers::WithinAbs(-7.0f, 0.001f));
    CHECK_THAT(entry->lra_range_min, Catch::Matchers::WithinAbs(4.0f, 0.001f));
    CHECK_THAT(entry->lra_range_max, Catch::Matchers::WithinAbs(7.0f, 0.001f));
    CHECK_THAT(entry->true_peak_max, Catch::Matchers::WithinAbs(-0.5f, 0.001f));
    CHECK_THAT(entry->spectral_centroid_hz, Catch::Matchers::WithinAbs(2800.0f, 0.001f));
    CHECK_THAT(entry->low_end_weight, Catch::Matchers::WithinAbs(0.65f, 0.001f));
    CHECK_THAT(entry->stereo_width_min, Catch::Matchers::WithinAbs(0.55f, 0.001f));
    CHECK_THAT(entry->stereo_width_max, Catch::Matchers::WithinAbs(0.9f, 0.001f));
    CHECK_THAT(entry->hook_density_norm, Catch::Matchers::WithinAbs(0.8f, 0.001f));
    CHECK_THAT(entry->heaviness_weight_norm, Catch::Matchers::WithinAbs(0.8f, 0.001f));
    CHECK_THAT(entry->typical_bpm_min, Catch::Matchers::WithinAbs(130.0f, 0.001f));
    CHECK_THAT(entry->typical_bpm_max, Catch::Matchers::WithinAbs(160.0f, 0.001f));
    CHECK_THAT(entry->vocal_presence_db, Catch::Matchers::WithinAbs(2.0f, 0.001f));
}

TEST_CASE("GenreDatabase::listAll returns all entries in insertion order", "[genre]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(kMinimalGenreJson).is_ok());

    const auto& all = db.listAll();
    REQUIRE(all.size() == 2);
    CHECK(all[0].id == "trap");
    CHECK(all[1].id == "ambient");
}

TEST_CASE("GenreDatabase can be reloaded with new content", "[genre]")
{
    GenreDatabase db;
    REQUIRE(db.loadFromString(kMinimalGenreJson).is_ok());
    CHECK(db.size() == 2);

    static const char* kSingleGenre = R"({"genres":[{"id":"pop","name":"Pop"}]})";
    REQUIRE(db.loadFromString(kSingleGenre).is_ok());
    CHECK(db.size() == 1);
    CHECK(db.findById("trap").has_value() == false);
    CHECK(db.findById("pop").has_value());
}

TEST_CASE("GenreDatabase is not loaded before first load call", "[genre]")
{
    GenreDatabase db;
    CHECK_FALSE(db.isLoaded());
    CHECK(db.size() == 0);
}

TEST_CASE("GenreDatabase::loadFromFile fails for non-existent path", "[genre]")
{
    GenreDatabase db;
    auto result = db.loadFromFile("C:/does_not_exist/genre_targets.json");
    CHECK(result.is_err());
    CHECK_FALSE(db.isLoaded());
}
