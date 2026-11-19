// shared-core/src/ipc/HttpServiceBridge_Delivery.cpp
// Broadcast / podcast delivery compliance (POST /analyze/delivery_compliance).
#include <khensu/ipc/HttpServiceBridge.h>
#include "HttpServiceBridgeInternal.h"
#include <khensu/ipc/generated/ApiSchema.h>

// Service JSON field names, generated from resources/api/openapi.json:
// a renamed Python field is a compile error here, not a silent 0.
namespace apif = khensu::api::fields;

namespace khensu::ipc {

namespace {

std::optional<double> optionalNumber(const juce::var& obj, const char* key)
{
    const auto v = obj.getProperty(key, {});
    if (v.isVoid() || v.isUndefined() || !(v.isDouble() || v.isInt() || v.isInt64())) return std::nullopt;
    return static_cast<double>(v);
}

std::string str(const juce::var& obj, const char* key) { return obj.getProperty(key, {}).toString().toStdString(); }

} // namespace

khensu::Result<HttpServiceBridge::DeliveryComplianceResult, BridgeError>
DeliveryApi::analyzeDeliveryCompliance(const std::string& file_path, const std::vector<std::string>& target_ids)
{
    juce::DynamicObject::Ptr reqObj = new juce::DynamicObject();
    reqObj->setProperty(apif::DeliveryComplianceRequest::file_path, juce::String(file_path));
    juce::Array<juce::var> ids;
    for (const auto& id : target_ids)
        ids.add(juce::String(id));
    reqObj->setProperty(apif::DeliveryComplianceRequest::target_ids, ids);

    const auto resp = postJson("/analyze/delivery_compliance", juce::var(reqObj.get()), objectResponse());
    if (resp.is_err()) return forwardError<DeliveryComplianceResult>(resp);

    const auto& body = resp.value();
    const auto* results = body.getProperty(apif::DeliveryComplianceResponse::results, {}).getArray();
    if (results == nullptr) return shapeError<DeliveryComplianceResult>("Response missing results array");

    DeliveryComplianceResult out;
    out.channels = static_cast<int>(body.getProperty(apif::DeliveryComplianceResponse::channels, 0));
    // Every key through the generated constants (WIRE-09) - a renamed Python
    // field is a compile error, not a column of blanks.
    for (const auto& r : *results) {
        DeliveryTargetResult t;
        t.targetId = str(r, apif::DeliveryTargetResult::target_id);
        t.name = str(r, apif::DeliveryTargetResult::name);
        t.category = str(r, apif::DeliveryTargetResult::category);
        t.measurement = str(r, apif::DeliveryTargetResult::measurement);
        t.overall = str(r, apif::DeliveryTargetResult::overall);
        if (const auto* checks = r.getProperty(apif::DeliveryTargetResult::checks, {}).getArray()) {
            for (const auto& c : *checks) {
                DeliveryCheck dc;
                dc.id = str(c, apif::DeliveryCheck::id);
                dc.status = str(c, apif::DeliveryCheck::status);
                dc.measured = optionalNumber(c, apif::DeliveryCheck::measured);
                dc.target = optionalNumber(c, apif::DeliveryCheck::target);
                dc.delta = optionalNumber(c, apif::DeliveryCheck::delta);
                dc.note = str(c, apif::DeliveryCheck::note);
                dc.fix = str(c, apif::DeliveryCheck::fix);
                t.checks.push_back(std::move(dc));
            }
        }
        out.results.push_back(std::move(t));
    }
    return khensu::Result<DeliveryComplianceResult, BridgeError>::ok(std::move(out));
}

} // namespace khensu::ipc
