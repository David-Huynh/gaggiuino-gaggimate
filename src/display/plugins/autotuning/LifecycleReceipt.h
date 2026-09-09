#ifndef LIFECYCLERECEIPT_H
#define LIFECYCLERECEIPT_H

#include <ArduinoJson.h>
#include <optional>
#include <string_view>

namespace AutoTuning {

// A broker acknowledgement is deliberately insufficient to release local data.
inline std::optional<bool> acceptedLifecycleReceipt(JsonObjectConst receipt,
                                                    std::string_view machineId,
                                                    std::string_view awaitingDigest) {
    if (receipt.size() != 5 || receipt["event_type"] != "lifecycle_ack" ||
        !receipt["schema_version"].is<int>() || receipt["schema_version"].as<int>() != 1 ||
        !receipt["machine_id"].is<const char *>() ||
        machineId != receipt["machine_id"].as<const char *>() ||
        !receipt["delivery_id"].is<const char *>() ||
        (receipt["outcome"] != "accepted" && receipt["outcome"] != "permanent_rejection")) return {};
    const std::string_view digest = receipt["delivery_id"].as<const char *>();
    if (digest.size() != 64 || digest != awaitingDigest) return {};
    for (const char value : digest)
        if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) return {};
    return receipt["outcome"] == "accepted";
}

} // namespace AutoTuning
#endif
