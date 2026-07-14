#include "LocalAutoTuningContextStore.h"

#include "LocalAutoTuningFiles.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <display/core/AutoTuning.h>
#include <display/core/EpochTime.h>
#include <display/core/Settings.h>
#include <display/util/LittleFSUtil.h>
#include <display/util/PsramAllocator.h>

namespace {

constexpr const char *STORE_DIR = "/rll";
constexpr const char *CONTEXT_PATH = "/rll/contexts.json";

static void addParsedContextJson(JsonObject target, const char *key, const String &rawJson) {
    if (rawJson.isEmpty()) {
        return;
    }
    JsonDocument document(&psramAllocator);
    if (!deserializeJson(document, rawJson)) {
        target[key].set(document.as<JsonVariantConst>());
    } else {
        target[String(key) + "_json"] = rawJson;
    }
}

} // namespace

bool LocalAutoTuningContextStore::begin() const {
    const bool available = LocalAutoTuningFiles::ensureDirectory(STORE_DIR);
    if (available) {
        LocalAutoTuningFiles::recoverDirectory(STORE_DIR);
    }
    return available;
}

bool LocalAutoTuningContextStore::save(Settings const &settings) const {
    if (!begin()) {
        return false;
    }
    JsonDocument document(&psramAllocator);
    JsonObject root = document.to<JsonObject>();
    root["event_type"] = "local_context_snapshot";
    root["schema_version"] = 1;
    root["updated_at"] = EpochTime::now();
    const String providerMode = AutoTuning::normalizeProviderMode(settings.getRLAutoTuningProviderMode().c_str()).c_str();
    root["provider_mode"] = providerMode;
    root["optimizer_mode"] = settings.getRLOptimizerMode();
    root["cpbo_profile_name"] = settings.getRLCPBOProfileName();
    root["cpbo_comparison_mode"] = settings.getRLCPBOComparisonMode();
    root["bean_context_id"] = settings.getRLBeanContextId();
    root["bean_context_name"] = settings.getRLBeanContextName();
    root["grinder_context_id"] = settings.getRLGrinderContextId();
    root["grinder_context_name"] = settings.getRLGrinderContextName();
    root["local_optimization_enabled"] = settings.isRLLocalOptimizationEnabled();
    root["optimization_paused"] = settings.isRLOptimizationPaused();
    addParsedContextJson(root, "bean_contexts", settings.getRLBeanContextsJson());
    addParsedContextJson(root, "grinder_contexts", settings.getRLGrinderContextsJson());
    return LocalAutoTuningFiles::writeJson(CONTEXT_PATH, document);
}

bool LocalAutoTuningContextStore::clear() const {
    if (!LittleFSUtil::existsQuietly(CONTEXT_PATH)) {
        return true;
    }
    return LittleFS.remove(CONTEXT_PATH);
}

size_t LocalAutoTuningContextStore::bytes() const {
    File file = LittleFS.open(CONTEXT_PATH, FILE_READ);
    if (!file) {
        return 0;
    }
    const size_t result = file.size();
    file.close();
    return result;
}
