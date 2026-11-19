// tests/ui/ChainReasoningPlainLanguageTest.cpp
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/ChainReasoningView.h"

using khensu::ChainReasoningView;

TEST_CASE("Plain-language and myth sections stay hidden until supplied", "[ui][reasoning]")
{
    ChainReasoningView view;
    view.setSize(400, 600);
    khensu::Recommendation rec;
    rec.what = "Master is too loud.";
    rec.suggested_action = "Lower the limiter.";
    view.setRecommendation(rec);
    const int before = view.getContentHeight();

    view.setPlainLanguage("It is louder than streaming will play it.", {}, "Limiter ceiling too hot.");
    const int withPlain = view.getContentHeight();
    CHECK(withPlain > before); // plain words + the cause it filled

    view.setPlainLanguage("It is louder than streaming will play it.",
                          "Loudness normalization turns loud masters down anyway.");
    CHECK(view.getContentHeight() > withPlain); // myth check added
}
