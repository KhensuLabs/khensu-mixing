// tests/plugin/CopilotServiceClientTest.cpp
// REF-6: the plugin's one service client arms the FIX-7 identity gate
// before it hands out a bridge, so no plugin request can skip the proof.
#include <catch2/catch_test_macros.hpp>
#include "../../plugin-copilot/Source/CopilotServiceClient.h"

using khensu::copilot::ServiceClient;
using khensu::ipc::HttpServiceBridge;

TEST_CASE("ServiceClient arms the identity gate before handing out a bridge (REF-6, FIX-7)", "[plugin][service]")
{
    const bool wasRequired = HttpServiceBridge::serviceIdentityRequired();
    const bool wasSelfVerify = HttpServiceBridge::serviceIdentitySelfVerify();
    HttpServiceBridge::setServiceIdentityRequired(false);
    HttpServiceBridge::setServiceIdentitySelfVerify(false);

    {
        auto bridge = ServiceClient::bridge(ServiceClient::kTransportRequestMs);
        CHECK(HttpServiceBridge::serviceIdentityRequired());
        CHECK(HttpServiceBridge::serviceIdentitySelfVerify());
    }
    STATIC_CHECK(ServiceClient::kTransportRequestMs < ServiceClient::kShortRequestMs);
    STATIC_CHECK(ServiceClient::kShortRequestMs <= ServiceClient::kJobRequestMs);

    HttpServiceBridge::setServiceIdentityRequired(wasRequired);
    HttpServiceBridge::setServiceIdentitySelfVerify(wasSelfVerify);
}
