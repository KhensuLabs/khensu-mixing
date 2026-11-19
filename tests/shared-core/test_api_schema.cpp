// tests/shared-core/test_api_schema.cpp
// The generated API layer (scripts/gen_cpp_api.py): route and field-name
// constants match the wire names, and the generated JobStatus list. The typed
// fromVar parsers are no longer generated (ENG-4: nothing called them); their
// tests come back with `gen_cpp_api.py --typed`.
#include <catch2/catch_test_macros.hpp>
#include <khensu/ipc/generated/ApiJobStatus.h>
#include <khensu/ipc/generated/ApiSchema.h>

#include <cstring>

using namespace khensu;

TEST_CASE("ApiSchema: field and route constants match the wire names", "[apischema]")
{
    CHECK(std::strcmp(api::fields::AnalyzeResponse::key_confidence, "key_confidence") == 0);
    CHECK(std::strcmp(api::fields::HookInstance::start_sec, "start_sec") == 0);
    CHECK(std::strcmp(api::routes::post_analyze.method, "POST") == 0);
    CHECK(std::strcmp(api::routes::post_analyze.path, "/analyze") == 0);
    CHECK(std::strcmp(api::routes::get_analyze_status_by_job_id.path, "/analyze/status/{job_id}") == 0);
}

TEST_CASE("ApiJobStatus: parse, terminal and cancellable states", "[apischema]")
{
    using api::JobStatus;
    CHECK(api::parseJobStatus("completed") == JobStatus::Completed);
    CHECK(api::parseJobStatus("interrupted") == JobStatus::Interrupted);
    CHECK_FALSE(api::parseJobStatus("done").has_value()); // the legacy string
    CHECK_FALSE(api::parseJobStatus("").has_value());

    for (auto s : {JobStatus::Completed, JobStatus::Failed, JobStatus::Cancelled, JobStatus::Interrupted})
        CHECK(api::isTerminal(s));
    for (auto s : {JobStatus::Pending, JobStatus::Queued, JobStatus::Running, JobStatus::Cancelling})
        CHECK_FALSE(api::isTerminal(s));

    CHECK(api::isCancellable(JobStatus::Running));
    CHECK_FALSE(api::isCancellable(JobStatus::Completed));

    CHECK(api::isTerminalStatus("cancelled"));
    CHECK_FALSE(api::isTerminalStatus("cancelling"));
    CHECK_FALSE(api::isTerminalStatus("done")); // unknown -> keep polling
    CHECK(api::isActiveStatus("running"));
    CHECK_FALSE(api::isActiveStatus("bogus"));
    CHECK(api::isStatus("failed", JobStatus::Failed));
    CHECK(juce::String(api::toString(JobStatus::Cancelling)) == api::job_status::cancelling);
}
