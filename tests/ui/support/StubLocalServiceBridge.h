// tests/ui/support/StubLocalServiceBridge.h
// A do-nothing ILocalServiceBridge: every method fails/returns empty. Test
// doubles derive from this and override only the handful of methods the code
// under test actually calls, instead of restating all 17 pure virtuals.
#pragma once
#include <khensu/ipc/ILocalServiceBridge.h>

namespace khensu::test {

class StubLocalServiceBridge : public ipc::ILocalServiceBridge {
  public:
    khensu::Result<ipc::DeepAnalysisResponse, ipc::BridgeError> analyze(const std::string&, AnalysisMode) override
    {
        return khensu::Result<ipc::DeepAnalysisResponse, ipc::BridgeError>::err({});
    }
    khensu::Result<khensu::SectionAnalysis, ipc::BridgeError> analyzeSections(const std::string&) override
    {
        return khensu::Result<khensu::SectionAnalysis, ipc::BridgeError>::err({});
    }
    khensu::Result<khensu::ReferenceDeltas, ipc::BridgeError> analyzeReference(const std::string&,
                                                                               const std::string&) override
    {
        return khensu::Result<khensu::ReferenceDeltas, ipc::BridgeError>::err({});
    }
    khensu::Result<std::string, ipc::BridgeError> startAnalysisJob(const std::string&, AnalysisMode) override
    {
        return khensu::Result<std::string, ipc::BridgeError>::err({});
    }
    khensu::Result<ipc::AnalysisJobStatus, ipc::BridgeError> getAnalysisProgress(const std::string&) override
    {
        return khensu::Result<ipc::AnalysisJobStatus, ipc::BridgeError>::err({});
    }
    void cancelAnalysisJob(const std::string&) override {}
    khensu::Result<std::string, ipc::BridgeError> startSeparationJob(const std::string&, const std::string&,
                                                                     const std::string&) override
    {
        return khensu::Result<std::string, ipc::BridgeError>::err({});
    }
    khensu::Result<ipc::SeparationJobStatus, ipc::BridgeError> getSeparationProgress(const std::string&) override
    {
        return khensu::Result<ipc::SeparationJobStatus, ipc::BridgeError>::err({});
    }
    void cancelSeparationJob(const std::string&) override {}
    khensu::Result<ipc::IntegrityResult, ipc::BridgeError> checkIntegrity(const std::string&) override
    {
        return khensu::Result<ipc::IntegrityResult, ipc::BridgeError>::err({});
    }
    khensu::Result<std::vector<ipc::ExportTarget>, ipc::BridgeError> getExportTargets() override
    {
        return khensu::Result<std::vector<ipc::ExportTarget>, ipc::BridgeError>::err({});
    }
    khensu::Result<std::vector<ipc::GenreReferencePoint>, ipc::BridgeError>
    getReferenceLibraryPoints(const std::string&) override
    {
        return khensu::Result<std::vector<ipc::GenreReferencePoint>, ipc::BridgeError>::err({});
    }
    khensu::Result<ipc::StemMaskingResult, ipc::BridgeError> analyzeStemMasking(const std::string&, const std::string&,
                                                                                const std::string&,
                                                                                const std::string&) override
    {
        return khensu::Result<ipc::StemMaskingResult, ipc::BridgeError>::err({});
    }
    khensu::Result<ipc::SpectrotemporalResult, ipc::BridgeError> analyzeSpectrotemporal(const std::string&, int,
                                                                                        int) override
    {
        return khensu::Result<ipc::SpectrotemporalResult, ipc::BridgeError>::err({});
    }
    bool is_service_available() const noexcept override { return true; }
    void seekTransport(float) override {}
};

} // namespace khensu::test
