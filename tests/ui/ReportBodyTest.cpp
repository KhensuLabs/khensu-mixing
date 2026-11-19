// tests/ui/ReportBodyTest.cpp
// REF-8: the /report request body is a pure function of the session and the
// analysis, testable without a RenderService.
#include <catch2/catch_test_macros.hpp>
#include <JuceHeader.h>
#include "../../desktop-app/Source/session/ReportBody.h"
#include "../../desktop-app/Source/ui/shared/KhensuTheme.h"
#include <khensu/analysis/AnalysisResult.h>
#include <khensu/ipc/generated/ApiSchema.h>
#include <khensu/session/Session.h>

namespace apif = khensu::api::fields;

TEST_CASE("Report body: none without a session (REF-8)", "[report]")
{
    CHECK(khensu::report::buildRequestBody(khensu::AnalysisResult{}, nullptr, "out.html", "full").isEmpty());
}

TEST_CASE("Report body: the session, the numbers and the theme (REF-8)", "[report]")
{
    khensu::Session session;
    session.id = "s-1";
    session.name = "My Song";
    khensu::AudioFileRef file;
    file.path = "C:/music/song.wav";
    file.title = "Song";
    session.files.push_back(file);
    khensu::AnalysisResult analysis;
    analysis.loudness.lufs_integrated = -9.5f;

    const auto body = khensu::report::buildRequestBody(analysis, &session, "C:/out/report.html", "full");
    const auto v = juce::JSON::parse(body);
    REQUIRE(v.isObject());
    CHECK(v[apif::ReportRequest::session_name].toString() == "My Song");
    CHECK(v[apif::ReportRequest::session_id].toString() == "s-1");
    CHECK(v[apif::ReportRequest::file_name].toString() == "Song");
    CHECK(v[apif::ReportRequest::report_mode].toString() == "full");
    CHECK(v[apif::ReportRequest::output_path].toString() == "C:/out/report.html");
    CHECK(static_cast<double>(v[apif::ReportRequest::lufs_integrated]) == -9.5);
    const auto theme = v[apif::ReportRequest::theme];
    REQUIRE(theme.isObject());
    CHECK(theme[apif::ReportTheme::bg_base].toString().startsWith("#"));
}
