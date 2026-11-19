// tests/ui/EditViewStateTest.cpp
// Waveform/spectrogram editing view state: zoom, scroll, scrub mapping,
// selection, and annotation persistence.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/EditViewState.h"

using Catch::Approx;
using khensu::EditViewState;

TEST_CASE("Edit view zooms around an anchor and clamps to the file", "[editview]")
{
    EditViewState s;
    s.setDuration(100.0);
    CHECK(s.viewLength() == Approx(100.0));
    s.zoomAround(50.0, 4.0);
    CHECK(s.viewLength() == Approx(25.0));
    CHECK(s.viewStart() == Approx(37.5)); // anchor stays at the same relative x
    s.scrollBy(1000.0);
    CHECK(s.viewEnd() == Approx(100.0));
    s.scrollBy(-1000.0);
    CHECK(s.viewStart() == Approx(0.0));
    s.zoomAround(0.0, 1e9);
    CHECK(s.viewLength() == Approx(EditViewState::kMinVisibleSec));
    s.zoomAround(0.0, 1e-9);
    CHECK(s.viewLength() == Approx(100.0));
    s.zoomTo(80.0, 60.0); // brush-to-zoom, reversed drag
    CHECK(s.viewStart() == Approx(60.0));
    CHECK(s.viewLength() == Approx(20.0));
    s.zoomToFit();
    CHECK(s.viewStart() == 0.0);
}

TEST_CASE("Edit view maps pixels to time and follows the playhead", "[editview]")
{
    EditViewState s;
    s.setDuration(10.0);
    s.zoomTo(2.0, 4.0);
    CHECK(s.timeForX(0.0f, 200.0f) == Approx(2.0));
    CHECK(s.timeForX(100.0f, 200.0f) == Approx(3.0));
    CHECK(s.xForTime(3.5, 200.0f) == Approx(150.0f));
    CHECK(s.timeForX(-50.0f, 0.0f) == Approx(2.0)); // zero width is safe

    s.setPlayhead(3.0);
    CHECK_FALSE(s.followPlayhead());
    s.setPlayhead(9.0);
    CHECK(s.followPlayhead());
    CHECK(s.playhead() >= s.viewStart());
    CHECK(s.playhead() <= s.viewEnd());
    s.setPlayhead(-3.0);
    CHECK(s.playhead() == 0.0);
}

TEST_CASE("Edit view selection and annotations", "[editview]")
{
    EditViewState s;
    s.setDuration(60.0);
    s.setSelection(20.0, 10.0);
    REQUIRE(s.hasSelection());
    CHECK(s.selectionStart() == Approx(10.0));
    const int ranged = s.addAnnotation("  harsh S  ");
    REQUIRE(ranged > 0);
    CHECK(s.annotations()[0].text == "harsh S");
    CHECK(s.addAnnotation("   ") == -1);

    s.clearSelection();
    s.setPlayhead(5.0);
    const int point = s.addAnnotation("count-in");
    REQUIRE(s.annotations().size() == 2);
    CHECK(s.annotations()[0].id == point); // sorted by start time
    CHECK(s.annotationAt(5.02, 0.05) == point);
    CHECK(s.annotationAt(15.0, 0.0) == ranged);
    CHECK(s.annotationAt(40.0, 0.5) == -1);
    CHECK(s.nextAnnotationAfter(5.0) == Approx(10.0));
    CHECK(s.previousAnnotationBefore(12.0) == Approx(10.0));
    CHECK(s.nextAnnotationAfter(30.0) == Approx(30.0));

    const auto saved = juce::JSON::parse(juce::JSON::toString(s.annotationsToVar()));
    EditViewState restored;
    restored.setDuration(60.0);
    CHECK(restored.annotationsFromVar(saved) == 2);
    CHECK(restored.annotations()[1].endSec == Approx(20.0));
    CHECK(restored.removeAnnotation(restored.annotations()[0].id));
    CHECK_FALSE(restored.removeAnnotation(999));
    CHECK(restored.annotations().size() == 1);
}
