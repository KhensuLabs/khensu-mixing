#include <catch2/catch_test_macros.hpp>
#include "support/FakeAppController.h"
#include "../../desktop-app/Source/ui/ExportDelivery/BatchRenderPanel.h"
#include "../../desktop-app/Source/ui/ExportDelivery/PlatformMastersPanel.h"
#include "../../desktop-app/Source/ui/ExportDelivery/DeliveryQaPanel.h"

TEST_CASE("Export-delivery panels construct against the narrow controller seam", "[exportdelivery][integration]")
{
    khensu::test::FakeAppController controller;

    SECTION("batch render exposes its missing-input guard")
    {
        khensu::BatchRenderPanel panel(controller);
        panel.run();
        CHECK(panel.statusTextForTesting().containsIgnoreCase("folder"));
    }

    SECTION("platform masters exposes its missing-session guard")
    {
        khensu::PlatformMastersPanel panel(controller);
        panel.run();
        CHECK(panel.statusTextForTesting().containsIgnoreCase("session"));
    }

    SECTION("delivery QA runs no background work without an active session")
    {
        khensu::DeliveryQaPanel panel(controller);
        panel.run("Codec audit", [] { return juce::String("unexpected"); });
        CHECK(panel.resultTextForTesting() == "No active session.");
        CHECK(controller.runInBackgroundCallCount == 0);
    }
}
