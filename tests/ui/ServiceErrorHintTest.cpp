// tests/ui/ServiceErrorHintTest.cpp
#include <catch2/catch_test_macros.hpp>
#include "ui/shared/ServiceErrorHint.h"

using khensu::classifyServiceError;
using Kind = khensu::ipc::BridgeError::Kind;

TEST_CASE("A missing-file 404 is not reported as a stale service build", "[ui][errors]")
{
    const auto fileMissing = classifyServiceError(
        {Kind::HttpError, "HTTP 404 from /analyze/mono_compat: File not found: C:/gone.wav", 404});
    CHECK(fileMissing.message.containsIgnoreCase("couldn't be found"));

    const auto routeMissing = classifyServiceError({Kind::HttpError, "HTTP 404 from /analyze/new: Not Found", 404});
    CHECK(routeMissing.message.containsIgnoreCase("older build"));

    const auto tooBig = classifyServiceError({Kind::HttpError, "HTTP 413 from /analyze/x: too large", 413});
    CHECK(tooBig.message.containsIgnoreCase("too large"));
}

TEST_CASE("403/409/429/503 get their own hints, using the FastAPI detail", "[ui][errors][audit068]")
{
    using khensu::isAbstainedRenderError;
    using khensu::serviceErrorDetail;

    const auto online = classifyServiceError(
        {Kind::HttpError, "HTTP 403 from /identify: Online feature 'acoustid' is disabled in Settings", 403});
    CHECK(online.message.containsIgnoreCase("turned off in Settings"));
    const auto outside = classifyServiceError(
        {Kind::HttpError, "HTTP 403 from /report: Output path is outside the allowed roots", 403});
    CHECK(outside.message.containsIgnoreCase("refused to write"));
    CHECK(outside.message.contains("outside the allowed roots"));

    const khensu::ipc::BridgeError abstained{
        Kind::HttpError, "HTTP 409 from /copilot/apply_chain/start: Genre-relative action was abstained: confirm", 409};
    CHECK(isAbstainedRenderError(abstained));
    CHECK(serviceErrorDetail(abstained) == "Genre-relative action was abstained: confirm");
    CHECK(classifyServiceError(abstained).message.startsWith("The render abstained"));
    const khensu::ipc::BridgeError aliasConflict{
        Kind::HttpError, "HTTP 409 from /copilot/apply_chain/start: Output aliases the source file", 409};
    CHECK_FALSE(isAbstainedRenderError(aliasConflict));
    CHECK(classifyServiceError(aliasConflict).message.containsIgnoreCase("conflicts"));
    // Abstention is a 409 thing only - the same words on a 422 are not one.
    CHECK_FALSE(isAbstainedRenderError({Kind::HttpError, "HTTP 422 from /x: confirm the value", 422}));

    CHECK(classifyServiceError({Kind::HttpError, "HTTP 429 from /analyze/start: too many jobs", 429})
              .message.containsIgnoreCase("too many"));

    const auto shuttingDown =
        classifyServiceError({Kind::HttpError, "HTTP 503 from /analyze/x: Service is shutting down.", 503});
    CHECK(shuttingDown.message.containsIgnoreCase("shutting down"));
    const auto missingExtra =
        classifyServiceError({Kind::HttpError, "HTTP 503 from /analyze/lyrics: faster-whisper is not installed", 503});
    CHECK(missingExtra.message.containsIgnoreCase("optional dependency"));
    CHECK(missingExtra.message.contains("faster-whisper"));

    // No detail kept (older bridge / Ignore mode): still the right kind.
    CHECK(serviceErrorDetail({Kind::HttpError, "HTTP 503 from /analyze/x", 503}).isEmpty());
    CHECK(classifyServiceError({Kind::HttpError, "HTTP 503 from /analyze/x", 503})
              .message.containsIgnoreCase("optional dependency"));
}
