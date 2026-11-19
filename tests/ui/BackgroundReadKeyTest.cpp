#include <catch2/catch_test_macros.hpp>

#include "BackgroundReadKey.h"

using khensu::BackgroundReadKey;

TEST_CASE("Delayed waveform reader is dropped after file switch", "[cq06][background]")
{
    const BackgroundReadKey delayed{"session-a", "C:/mix/old.wav", "waveform", 7};
    const BackgroundReadKey current{"session-a", "C:/mix/new.wav", "waveform", 8};

    REQUIRE_FALSE(delayed.matches(current));
}

TEST_CASE("Delayed reader is dropped after session switch", "[cq06][background]")
{
    const BackgroundReadKey delayed{"session-a", "C:/mix/shared.wav", "vocals", 3};
    const BackgroundReadKey current{"session-b", "C:/mix/shared.wav", "vocals", 3};

    REQUIRE_FALSE(delayed.matches(current));
}

TEST_CASE("Delayed stem analysis is dropped after stem switch", "[cq06][background]")
{
    const BackgroundReadKey delayed{"session-a", "C:/stems/vocals.wav", "vocals", 4};
    const BackgroundReadKey current{"session-a", "C:/stems/drums.wav", "drums", 5};

    REQUIRE_FALSE(delayed.matches(current));
}

TEST_CASE("Unchanged background read remains publishable", "[cq06][background]")
{
    const BackgroundReadKey requested{"session-a", "C:/mix/master.wav", "delta", 11};
    const BackgroundReadKey current{"session-a", "C:/mix/master.wav", "delta", 11};

    REQUIRE(requested.matches(current));
}
