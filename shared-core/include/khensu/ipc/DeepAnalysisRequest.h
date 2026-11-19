#pragma once
#include <khensu/ipc/ILocalServiceBridge.h>
#include <juce_core/juce_core.h>
#include <string>

namespace khensu::ipc {

struct DeepAnalysisRequest {
    std::string file_path;
    ILocalServiceBridge::AnalysisMode mode = ILocalServiceBridge::AnalysisMode::Advanced;

    juce::String to_json() const;
};

} // namespace khensu::ipc
