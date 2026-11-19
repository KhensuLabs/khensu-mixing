#include <catch2/catch_test_macros.hpp>
#include <khensu/systembrain/SystemBrainDatabase.h>

using namespace khensu;

static const char* kJson = R"({
  "version": "1.0",
  "features": [
    {
      "id": "analysis_dashboard", "name": "Analysis Dashboard", "category": "Analysis",
      "short_description": "Central hub showing all measured properties.",
      "detailed_description": "Aggregates every computed metric.",
      "why_it_exists": "See the full picture.", "when_to_use": "After loading audio.",
      "inputs": ["Loaded audio session"], "tags": ["technical", "visual", "desktop-only"],
      "desktop_location": "Dashboard screen", "plugin_available": false,
      "related_features": ["mix_notes_checklist"]
    },
    {
      "id": "integrated_loudness", "name": "Integrated Loudness", "category": "Analysis",
      "short_description": "Average perceived loudness.",
      "detailed_description": "Integrated LUFS.",
      "why_it_exists": "Loudness matters.", "when_to_use": "Always.",
      "inputs": ["Audio"], "tags": ["loudness"],
      "desktop_location": "Dashboard", "plugin_available": false,
      "related_features": [],
      "visualization": { "type": "bar", "min": -30.0, "max": -4.0, "labels": ["quiet", "hot"] }
    },
    {
      "id": "hook_dashboard", "name": "Hook Dashboard", "category": "Musical Psychology",
      "short_description": "Measures hook strength and density.",
      "detailed_description": "Computes multiple hook dimensions.",
      "why_it_exists": "Hooks predict retention.", "when_to_use": "When evaluating catchiness.",
      "inputs": ["Deep analysis results"], "tags": ["musical-psychology", "ai-assisted"],
      "desktop_location": "Dashboard > Hook Dashboard", "plugin_available": false,
      "related_features": ["memorability_panel"]
    }
  ]
})";

TEST_CASE("SystemBrainDatabase loads entries from JSON", "[sysbrain]")
{
    SystemBrainDatabase db;
    auto r = db.loadFromString(kJson);
    REQUIRE(r.is_ok());
    CHECK(db.isLoaded());
    CHECK(db.size() == 3);

    auto entry = db.findById("hook_dashboard");
    REQUIRE(entry.has_value());
    CHECK(entry->name == "Hook Dashboard");
    CHECK(entry->category == "Musical Psychology");
    CHECK(entry->plugin_available == false);
    REQUIRE(entry->tags.size() == 2);
    CHECK(entry->tags[0] == "musical-psychology");
    CHECK(entry->inputs.size() == 1);
    CHECK(entry->related_features.size() == 1);
    CHECK_FALSE(entry->visualization.hasVisualization());

    auto vizEntry = db.findById("integrated_loudness");
    REQUIRE(vizEntry.has_value());
    CHECK(vizEntry->visualization.type == "bar");
    CHECK(vizEntry->visualization.min == -30.0f);
    CHECK(vizEntry->visualization.max == -4.0f);
    REQUIRE(vizEntry->visualization.labels.size() == 2);
    CHECK(vizEntry->visualization.labels[0] == "quiet");
    CHECK(vizEntry->visualization.hasVisualization());
}

TEST_CASE("SystemBrainDatabase findById returns nullopt for unknown id", "[sysbrain]")
{
    SystemBrainDatabase db;
    db.loadFromString(kJson);
    CHECK_FALSE(db.findById("nope").has_value());
}

TEST_CASE("SystemBrainDatabase search matches name description and tags case-insensitively", "[sysbrain]")
{
    SystemBrainDatabase db;
    db.loadFromString(kJson);

    CHECK(db.search("hook").size() == 1);        // matches name "Hook Dashboard"
    CHECK(db.search("MEASURED").size() == 1);    // matches analysis_dashboard short_description
    CHECK(db.search("ai-assisted").size() == 1); // matches a tag
    CHECK(db.search("").size() == 3);            // empty -> all
    CHECK(db.search("zzz").empty());
}

TEST_CASE("SystemBrainDatabase filterByCategory and categories", "[sysbrain]")
{
    SystemBrainDatabase db;
    db.loadFromString(kJson);

    CHECK(db.filterByCategory("Analysis").size() == 2);
    CHECK(db.filterByCategory("analysis").size() == 2); // case-insensitive
    CHECK(db.filterByCategory("Nonexistent").empty());

    auto cats = db.categories();
    REQUIRE(cats.size() == 2);
    CHECK(cats[0] == "Analysis");
    CHECK(cats[1] == "Musical Psychology");
}

TEST_CASE("SystemBrainDatabase load errors on malformed root", "[sysbrain]")
{
    SystemBrainDatabase db;
    CHECK(db.loadFromString("not json").is_err());
    CHECK(db.loadFromString(R"({"version":"1.0"})").is_err()); // missing features array
}
