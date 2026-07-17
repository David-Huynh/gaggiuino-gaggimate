#include "WebUIPlugin.h"
#include "autotuning/AutoTuningTasteGoalJson.h"
#include <DNSServer.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <display/core/AutoTuning.h>
#include <display/core/Controller.h>
#include <display/core/EpochTime.h>
#include <display/core/FeatureFlags.h>
#include <display/core/ProfileManager.h>
#include <display/core/process/BrewProcess.h>
#include <display/core/process/GrindProcess.h>
#include <display/models/profile.h>
#include <display/plugins/BLEScalePlugin.h>
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
#include <display/plugins/HWScalePlugin.h>
#endif
#include <display/plugins/ShotHistoryPlugin.h>
#include <display/util/LittleFSUtil.h>
#include <display/util/PsramStlAllocator.h>
#include <display/util/PsramWsBuffer.h>
#include <display/webassets/web_ui_manifest.h>
#include <esp32-hal-psram.h>
#include <esp_core_dump.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <mbedtls/platform.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <version.h>

// Incoming WebSocket payloads (profile uploads reserve up to 64 KB) are
// reassembled here. Back the character storage with PSRAM so these large,
// transient buffers don't spike the scarce internal SRAM. The map nodes
// themselves stay on the default heap (tiny: an id + a string handle).
using PsramString = std::basic_string<char, std::char_traits<char>, PsramStlAllocator<char>>;
static std::unordered_map<uint32_t, PsramString> rxBuffers;
static std::unordered_map<uint32_t, unsigned long> rxBufferLastActivity;
static constexpr unsigned long RXBUFFER_IDLE_EVICT_MS = 5UL * 60UL * 1000UL;
static WebUIPlugin *g_webUIPlugin = nullptr;

// Serialize a JsonDocument straight into a PSRAM-backed WebSocket message
// buffer — one exact-sized allocation, off the internal heap. [GM-139]
static AsyncWebSocketSharedBuffer toWsBuffer(JsonDocument &doc) {
    const size_t len = measureJson(doc);
    auto buffer = makePsramWsBuffer(len);
    serializeJson(doc, buffer->data(), len);
    return buffer;
}

// Route mbedTLS allocations to PSRAM.
static void *mbedtlsPsramCalloc(size_t n, size_t size) { // NOSONAR
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return p;
}
static void mbedtlsPsramFree(void *p) { heap_caps_free(p); } // NOSONAR

#if defined(GAGGIMATE_DISABLE_OTA)
static constexpr bool OTA_ENABLED = false;
#else
static constexpr bool OTA_ENABLED = true;
#endif

#if defined(GAGGIMATE_UART_COMMS)
static constexpr bool CONTROLLER_OTA_ENABLED = false;
#else
static constexpr bool CONTROLLER_OTA_ENABLED = true;
#endif

#ifndef BUILD_GIT_REPOSITORY
#define BUILD_GIT_REPOSITORY "David-Huynh/gaggiuino-gaggimate"
#endif

static String otaReleaseUrlForChannel(const String &channel) {
    return String("https://github.com/") + BUILD_GIT_REPOSITORY + "/releases/" +
           (channel == "latest" ? "latest" : "tag/nightly");
}

static const char *otaDisplayFirmwareName() {
#if defined(GAGGIMATE_UART_COMMS) && defined(GAGGIMATE_N16R8)
    return "display-headless-uart-n16r8-firmware.bin";
#elif defined(GAGGIMATE_UART_COMMS)
    return "display-headless-uart-firmware.bin";
#elif defined(GAGGIMATE_HEADLESS)
    return "display-headless-firmware.bin";
#else
    return "display-firmware.bin";
#endif
}

static String defaultRLContextName() { return String("Bean ") + String(static_cast<long long>(EpochTime::now())); }

static String defaultRLGrinderContextName() { return String("Grinder ") + String(static_cast<long long>(EpochTime::now())); }

static String makeRLContextId(const String &prefix, const String &name) {
    String id = name;
    id.toLowerCase();
    id.replace(" ", "_");
    id.replace("/", "_");
    id.replace("\\", "_");
    id.replace(":", "_");
    id.replace("\"", "");
    if (id.isEmpty()) {
        id = prefix;
    }
    return prefix + "_" + id + "_" + String(static_cast<long long>(EpochTime::now())) + "_" +
           String(static_cast<unsigned long>(millis()));
}

static JsonArray loadRLContexts(JsonDocument &doc, const String &rawJson) {
    DeserializationError error = deserializeJson(doc, rawJson);
    if (error || !doc.is<JsonArray>()) {
        doc.clear();
        return doc.to<JsonArray>();
    }
    return doc.as<JsonArray>();
}

static String limitedRLString(JsonVariant value, size_t maxLength) {
    String text = value.as<String>();
    text.trim();
    if (text.length() > maxLength) {
        text = text.substring(0, maxLength);
    }
    return text;
}

static std::int64_t rlEpochOrZero(JsonVariantConst value) {
    return !value.is<bool>() && value.is<std::int64_t>() ? value.as<std::int64_t>() : 0;
}

static void copyRLStringArray(JsonDocument &target, const char *key, const String &rawJson) {
    JsonArray out = target[key].to<JsonArray>();
    JsonDocument sourceDoc;
    DeserializationError error = deserializeJson(sourceDoc, rawJson);
    if (error || !sourceDoc.is<JsonArray>()) {
        return;
    }

    int copied = 0;
    for (JsonVariant item : sourceDoc.as<JsonArray>()) {
        String text = limitedRLString(item, 160);
        if (text.isEmpty()) {
            continue;
        }
        out.add(text);
        copied++;
        if (copied >= 8) {
            break;
        }
    }
}

static void copyRLDiagnosticSteps(JsonDocument &target, const String &rawJson) {
    JsonArray out = target["rlAutoTuningDiagnosticSteps"].to<JsonArray>();
    JsonDocument sourceDoc;
    DeserializationError error = deserializeJson(sourceDoc, rawJson);
    if (error || !sourceDoc.is<JsonArray>()) {
        return;
    }

    int copied = 0;
    for (JsonVariant item : sourceDoc.as<JsonArray>()) {
        if (!item.is<JsonObject>()) {
            continue;
        }
        JsonObject src = item.as<JsonObject>();
        String key = limitedRLString(src["key"], 64);
        String label = limitedRLString(src["label"], 80);
        if (key.isEmpty() || label.isEmpty()) {
            continue;
        }

        JsonObject step = out.add<JsonObject>();
        step["key"] = key;
        step["label"] = label;
        step["state"] = limitedRLString(src["state"], 32);
        step["detail"] = limitedRLString(src["detail"], 160);
        copied++;
        if (copied >= 8) {
            break;
        }
    }
}

static void copyRLRecentShots(JsonDocument &target, const String &rawJson) {
    JsonArray out = target["rlRecentShots"].to<JsonArray>();
    JsonDocument recentDoc;
    DeserializationError error = deserializeJson(recentDoc, rawJson);
    if (error || !recentDoc.is<JsonArray>()) {
        return;
    }

    int copied = 0;
    for (JsonVariant item : recentDoc.as<JsonArray>()) {
        if (!item.is<JsonObject>()) {
            continue;
        }
        JsonObject src = item.as<JsonObject>();
        String shotId = limitedRLString(src["shot_id"], 160);
        if (shotId.isEmpty()) {
            continue;
        }

        JsonObject shot = out.add<JsonObject>();
        shot["shot_id"] = shotId;
        shot["timestamp"] = rlEpochOrZero(src["timestamp"]);
        shot["shot_type"] = limitedRLString(src["shot_type"], 40);
        shot["shot_time_s"] = src["shot_time_s"] | 0.0f;
        shot["beverage_out_g"] = src["beverage_out_g"] | 0.0f;
        shot["target_yield_g"] = src["target_yield_g"] | 0.0f;
        shot["exclude_from_local_optimization"] = src["exclude_from_local_optimization"] | false;
        shot["optimization_weight"] = src["optimization_weight"] | 0.0f;
        shot["profile_label"] = limitedRLString(src["profile_label"], 80);
        shot["profile_type"] = limitedRLString(src["profile_type"], 40);
        shot["final_phase_index"] = src["final_phase_index"] | 0;
        shot["final_phase_name"] = limitedRLString(src["final_phase_name"], 80);
        shot["final_phase_type"] = limitedRLString(src["final_phase_type"], 40);
        shot["final_phase_elapsed_s"] = src["final_phase_elapsed_s"] | 0.0f;
        shot["final_pump_target"] = limitedRLString(src["final_pump_target"], 40);
        shot["shot_end_state"] = limitedRLString(src["shot_end_state"], 40);
        shot["profile_flow_valid"] = src["profile_flow_valid"] | false;
        shot["profile_flow_masked"] = src["profile_flow_masked"] | false;
        copied++;
        if (copied >= 10) {
            break;
        }
    }
}

static int currentBagIndex(JsonArray contexts, const String &name) {
    int bag = 0;
    for (JsonObject context : contexts) {
        if (context["name"].as<String>() == name) {
            bag = std::max(bag, context["bag_index"] | 0);
        }
    }
    return bag;
}

constexpr size_t MAX_RL_BEAN_CONTEXTS = 16;
constexpr size_t MAX_RL_GRINDER_CONTEXTS = 8;
constexpr size_t MAX_RL_CONTEXT_JSON_BYTES = 3500;

static bool makeRoomForRLContext(JsonArray contexts, size_t maxContexts, Settings &settings, bool grinder) {
    while (contexts.size() >= maxContexts) {
        bool removed = false;
        for (size_t i = 0; i < contexts.size(); i++) {
            JsonObject context = contexts[i];
            if (context["status"].as<String>() != "active") {
                const String contextId = context["id"].as<String>();
                contexts.remove(i);
                if (grinder) {
                    AutoTuning::removeTasteGoalsForGrinderContext(settings, contextId);
                } else {
                    AutoTuning::removeTasteGoalsForBeanContext(settings, contextId);
                }
                removed = true;
                break;
            }
        }
        if (!removed) {
            return false;
        }
    }
    return true;
}

static JsonObject findRLContext(JsonArray contexts, const String &contextId) {
    for (JsonObject context : contexts) {
        if (context["id"].as<String>() == contextId) {
            return context;
        }
    }
    return JsonObject();
}

static bool removeRLContext(JsonArray contexts, const String &contextId) {
    for (size_t i = 0; i < contexts.size(); i++) {
        JsonObject context = contexts[i];
        if (context["id"].as<String>() == contextId) {
            contexts.remove(i);
            return true;
        }
    }
    return false;
}

static void markRLOtherContextsAvailable(JsonArray contexts, const String &activeId) {
    for (JsonObject context : contexts) {
        if (context["id"].as<String>() != activeId && context["status"].as<String>() == "active") {
            context["status"] = "available";
        }
    }
}

static JsonObject addRLContext(JsonArray contexts, Settings &settings, const String &id, const String &name, int bagIndex,
                               const char *status) {
    if (!makeRoomForRLContext(contexts, MAX_RL_BEAN_CONTEXTS, settings, false)) {
        return JsonObject();
    }
    JsonObject context = contexts.add<JsonObject>();
    context["id"] = id;
    context["name"] = name;
    context["bag_index"] = bagIndex;
    context["status"] = status;
    context["created_at"] = EpochTime::now();
    return context;
}

static JsonObject addRLGrinderContext(JsonArray contexts, Settings &settings, const String &id, const String &name,
                                      const char *status) {
    if (!makeRoomForRLContext(contexts, MAX_RL_GRINDER_CONTEXTS, settings, true)) {
        return JsonObject();
    }
    JsonObject context = contexts.add<JsonObject>();
    context["id"] = id;
    context["name"] = name;
    context["status"] = status;
    context["created_at"] = EpochTime::now();
    return context;
}

static bool isRLNumber(JsonVariant value) { return value.is<int>() || value.is<float>() || value.is<double>(); }

static void addRLRecipeDomain(JsonObject target, Settings const &settings) {
    AutoTuning::RecipeDomain const domain = settings.getRLRecipeDomain();
    JsonObject value = target["rlRecipeDomain"].to<JsonObject>();
    value["grindRadiusSteps"] = domain.grindRadiusSteps;
    value["doseMinG"] = domain.doseMinG;
    value["doseMaxG"] = domain.doseMaxG;
    value["targetOutputMinG"] = domain.targetOutputMinG;
    value["targetOutputMaxG"] = domain.targetOutputMaxG;
}

static bool sameRLRecipeDomain(AutoTuning::RecipeDomain const &left, AutoTuning::RecipeDomain const &right) {
    return fabsf(left.grindRadiusSteps - right.grindRadiusSteps) < 0.0001f &&
           fabsf(left.doseMinG - right.doseMinG) < 0.0001f && fabsf(left.doseMaxG - right.doseMaxG) < 0.0001f &&
           fabsf(left.targetOutputMinG - right.targetOutputMinG) < 0.0001f &&
           fabsf(left.targetOutputMaxG - right.targetOutputMaxG) < 0.0001f;
}

static float boundedRLFloat(JsonVariant value, float fallback, float minValue, float maxValue) {
    if (!isRLNumber(value)) {
        return fallback;
    }
    float parsed = value.as<float>();
    if (!std::isfinite(parsed)) {
        return fallback;
    }
    return std::min(std::max(parsed, minValue), maxValue);
}

static String normalizedStepDirection(JsonVariant value) {
    String direction = value.as<String>();
    direction.trim();
    if (direction != "higher_is_coarser") {
        direction = "higher_is_finer";
    }
    return direction;
}

static String normalizedGrinderAdjustmentMode(JsonVariant value) {
    String mode = value.as<String>();
    mode.trim();
    if (mode != "stepless") {
        mode = "stepped";
    }
    return mode;
}

static String grinderCalibrationMode(JsonObject context) {
    String mode = context["grinder_calibration_mode"].as<String>();
    if (mode == "absolute_display_calibrated" || mode == "relative_calibrated" || mode == "uncalibrated") {
        return mode;
    }
    if (isRLNumber(context["microns_per_step"])) {
        return isRLNumber(context["current_absolute_step"]) && isRLNumber(context["absolute_reference_step"])
                   ? "absolute_display_calibrated"
                   : "relative_calibrated";
    }
    return "uncalibrated";
}

static void applyRLGrinderCalibration(JsonObject context, JsonDocument &request) {
    const bool hasMicronsRequest = isRLNumber(request["microns_per_step"]);
    if (hasMicronsRequest) {
        context["microns_per_step"] = boundedRLFloat(request["microns_per_step"], 10.0f, 0.1f, 100.0f);
    }
    if (request["step_direction"].is<String>() || context["step_direction"].as<String>().isEmpty()) {
        context["step_direction"] = normalizedStepDirection(request["step_direction"]);
    }
    if (request["grinder_adjustment_mode"].is<String>() || context["grinder_adjustment_mode"].as<String>().isEmpty()) {
        context["grinder_adjustment_mode"] = normalizedGrinderAdjustmentMode(request["grinder_adjustment_mode"]);
    }
    String reference = limitedRLString(request["reference_label"], 80);
    if (!reference.isEmpty()) {
        context["reference_label"] = reference;
    } else if (limitedRLString(context["reference_label"], 80).isEmpty()) {
        context["reference_label"] = "Initial setting";
    }

    const bool hasRelativeRequest = isRLNumber(request["current_relative_step"]);
    const bool hasCurrentAbsoluteRequest = isRLNumber(request["current_absolute_step"]);

    if (isRLNumber(request["current_absolute_step"])) {
        context["current_absolute_step"] = boundedRLFloat(request["current_absolute_step"], 0.0f, -10000.0f, 10000.0f);
    }
    if (isRLNumber(request["absolute_reference_step"])) {
        context["absolute_reference_step"] = boundedRLFloat(request["absolute_reference_step"], 0.0f, -10000.0f, 10000.0f);
    } else if (hasCurrentAbsoluteRequest && !isRLNumber(context["absolute_reference_step"])) {
        context["absolute_reference_step"] = context["current_absolute_step"].as<float>();
    }

    if (isRLNumber(context["current_absolute_step"]) && isRLNumber(context["absolute_reference_step"])) {
        context["current_relative_step"] =
            context["current_absolute_step"].as<float>() - context["absolute_reference_step"].as<float>();
    } else if (hasRelativeRequest) {
        context["current_relative_step"] = boundedRLFloat(request["current_relative_step"], 0.0f, -10000.0f, 10000.0f);
    } else if (!isRLNumber(context["current_relative_step"])) {
        context["current_relative_step"] = 0.0f;
    }

    const bool hasMicrons = isRLNumber(context["microns_per_step"]);
    if (!hasMicrons) {
        context["grinder_calibration_mode"] = "uncalibrated";
    } else if (isRLNumber(context["current_absolute_step"]) && isRLNumber(context["absolute_reference_step"])) {
        context["grinder_calibration_mode"] = "absolute_display_calibrated";
    } else {
        context["grinder_calibration_mode"] = "relative_calibrated";
    }
}

static void copyRLGrinderCalibration(JsonObject out, JsonObject context) {
    out["grinder_calibration_mode"] = grinderCalibrationMode(context);
    if (isRLNumber(context["microns_per_step"])) {
        out["microns_per_step"] = context["microns_per_step"].as<float>();
    }
    out["step_direction"] = normalizedStepDirection(context["step_direction"]);
    out["grinder_adjustment_mode"] = normalizedGrinderAdjustmentMode(context["grinder_adjustment_mode"]);
    out["reference_label"] = limitedRLString(context["reference_label"], 80);
    out["current_relative_step"] = context["current_relative_step"] | 0.0f;
    if (isRLNumber(context["current_absolute_step"])) {
        out["current_absolute_step"] = context["current_absolute_step"].as<float>();
    }
    if (isRLNumber(context["absolute_reference_step"])) {
        out["absolute_reference_step"] = context["absolute_reference_step"].as<float>();
    }
}

static bool trimRLContextsForStorage(JsonDocument &contextsDoc, Settings &settings, size_t maxContexts, bool grinder) {
    JsonArray contexts = contextsDoc.as<JsonArray>();
    while (contexts.size() > maxContexts || measureJson(contextsDoc) > MAX_RL_CONTEXT_JSON_BYTES) {
        bool removed = false;
        for (size_t i = 0; i < contexts.size(); i++) {
            JsonObject context = contexts[i];
            if (context["status"].as<String>() == "active") {
                continue;
            }
            const String contextId = context["id"].as<String>();
            contexts.remove(i);
            if (grinder) {
                AutoTuning::removeTasteGoalsForGrinderContext(settings, contextId);
            } else {
                AutoTuning::removeTasteGoalsForBeanContext(settings, contextId);
            }
            removed = true;
            break;
        }
        if (!removed) {
            return false;
        }
    }
    return true;
}

static bool persistRLContexts(Settings &settings, JsonDocument &contextsDoc) {
    if (!trimRLContextsForStorage(contextsDoc, settings, MAX_RL_BEAN_CONTEXTS, false)) {
        return false;
    }
    String contextsJson;
    serializeJson(contextsDoc, contextsJson);
    settings.setRLBeanContextsJson(contextsJson);
    return true;
}

static bool persistRLGrinderContexts(Settings &settings, JsonDocument &contextsDoc) {
    if (!trimRLContextsForStorage(contextsDoc, settings, MAX_RL_GRINDER_CONTEXTS, true)) {
        return false;
    }
    String contextsJson;
    serializeJson(contextsDoc, contextsJson);
    settings.setRLGrinderContextsJson(contextsJson);
    return true;
}

static bool rlParticipationEnabled(Controller *controller) {
    return controller &&
           AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(), controller->getOptimizerTransport())
               .acceptActionableRecommendations();
}

WebUIPlugin::WebUIPlugin() : server(80), ws("/ws") { g_webUIPlugin = this; }

void WebUIPlugin::setup(Controller *_controller, PluginManager *_pluginManager) {
    // Redirect mbedTLS allocations to PSRAM before any TLS (OTA) handshake runs, so the
    // ~32 KB handshake buffers don't exhaust the scarce internal-DRAM pool. See mbedtlsPsramCalloc.
    (void)mbedtls_platform_set_calloc_free(mbedtlsPsramCalloc, mbedtlsPsramFree);
    this->controller = _controller;
    this->profileManager = _controller->getProfileManager();
    this->pluginManager = _pluginManager;
    this->ota = new GitHubOTA(
        BUILD_GIT_VERSION, controller->getSystemInfo().version, otaReleaseUrlForChannel(controller->getSettings().getOTAChannel()),
        [this](uint8_t phase) {
            pluginManager->trigger("ota:update:phase", "phase", phase);
            updateOTAProgress(phase, 0);
        },
        [this](uint8_t phase, int progress) {
            pluginManager->trigger("ota:update:progress", "progress", progress);
            updateOTAProgress(phase, progress);
        },
        otaDisplayFirmwareName(), "display-filesystem.bin", "board-firmware.bin");
    pluginManager->on("controller:wifi:connect", [this](Event const &event) {
        apMode = event.getInt("AP");
        start();
    });
    // Intentionally do NOT stop the server on a WiFi disconnect: the listen
    // socket survives a reconnect, and tearing it down only to rebind moments
    // later races AsyncTCP's async close (bind: -8) and churns sockets in the
    // recovery path. The server keeps listening; clients reconnect on their own.
    pluginManager->on("controller:wifi:disconnect", [this](Event const &) {
        ws.cleanupClients(); // drop dead websocket clients; keep the listener up
    });
    pluginManager->on("controller:ready", [this](Event const &) {
        ota->setControllerVersion(controller->getSystemInfo().version);
#ifndef GAGGIMATE_UART_COMMS
        ota->init(controller->getClientController()->getClient());
#endif
    });
    pluginManager->on("controller:autotune:result", [this](Event const &event) { sendAutotuneResult(); });
    pluginManager->on("controller:autotune:failed", [this](Event const &) { sendAutotuneFailed(); });

    pluginManager->on("rl:recommendation:received", [this](Event const &event) {
        if (!rlParticipationEnabled(controller)) {
            _pendRecJson = "";
            return;
        }

        AutoTuning::Recommendation const *recommendation = event.getPayload<AutoTuning::Recommendation>();
        if (!recommendation) {
            return;
        }

        const String status = AutoTuning::recommendationStatusKey(recommendation->status);
        rlLastRecommendationId = recommendation->recommendationId.c_str();
        rlRecommendationStatus = status;
        rlRecommendationMode = AutoTuning::recommendationModeKey(recommendation->mode);
        rlMode = rlRecommendationMode;
        rlRecommendationGrindDeltaStepsFromCurrent = recommendation->grindDeltaStepsFromCurrent;
        rlRecommendationGrindDeltaUmFromCurrent = recommendation->grindDeltaMicronsFromCurrent;
        rlRecommendationProjectedRelativeStepFromReference = recommendation->projectedRelativeStepFromReference;
        rlRecommendationProjectedRelativeGrindUmFromReference = recommendation->projectedRelativeMicronsFromReference;
        rlRecommendationNextDoseG = recommendation->nextDoseG;
        rlRecommendationTargetYieldG = recommendation->targetYieldG;
        rlRecommendationTargetRatio = recommendation->targetRatio;
        rlRecommendationHasCurrentAbsoluteStep = recommendation->currentAbsoluteStep.has_value();
        rlRecommendationCurrentAbsoluteStep = recommendation->currentAbsoluteStep.value_or(0.0f);
        rlRecommendationHasProjectedAbsoluteStep = recommendation->projectedAbsoluteStep.has_value();
        rlRecommendationProjectedAbsoluteStep = recommendation->projectedAbsoluteStep.value_or(0.0f);

        JsonDocument doc;
        doc["tp"] = "evt:rl:recommendation";
        doc["recommendation_id"] = rlLastRecommendationId;
        doc["shot_id"] = recommendation->sourceShotId.c_str();
        doc["install_id"] = recommendation->installId.c_str();
        doc["optimization_run_id"] = recommendation->optimizationRunId.c_str();
        doc["comparison_anchor_shot_id"] = recommendation->comparisonAnchorShotId.c_str();
        doc["comparison_mode"] = AutoTuning::comparisonModeKey(recommendation->comparisonMode);
        doc["preference_feedback_required"] = recommendation->preferenceFeedbackRequired;
        AutoTuning::writeTasteGoal(recommendation->tasteGoal, doc["taste_goal"].to<JsonObject>());
        doc["taste_goal_summary"] = AutoTuning::tasteGoalSummary(recommendation->tasteGoal);
        doc["status"] = status;
        doc["mode"] = rlRecommendationMode;
        const String stepDirection = recommendation->stepDirection.c_str();
        doc["step_direction"] = stepDirection == "higher_is_coarser" ? "higher_is_coarser" : "higher_is_finer";
        doc["grind_delta_steps_from_current"] = rlRecommendationGrindDeltaStepsFromCurrent;
        doc["grind_delta_um_from_current"] = rlRecommendationGrindDeltaUmFromCurrent;
        doc["projected_relative_step_from_reference"] = rlRecommendationProjectedRelativeStepFromReference;
        doc["projected_relative_grind_um_from_reference"] = rlRecommendationProjectedRelativeGrindUmFromReference;
        doc["next_dose_g"] = rlRecommendationNextDoseG;
        doc["target_yield_g"] = rlRecommendationTargetYieldG;
        doc["target_ratio"] = rlRecommendationTargetRatio;
        doc["has_current_absolute_step"] = rlRecommendationHasCurrentAbsoluteStep;
        doc["current_absolute_step"] = rlRecommendationCurrentAbsoluteStep;
        doc["has_projected_absolute_step"] = rlRecommendationHasProjectedAbsoluteStep;
        doc["projected_absolute_step"] = rlRecommendationProjectedAbsoluteStep;
        const String payload = doc.as<String>();
        // Cache as the reopen source of truth only while still promptable; a
        // resolved status (applied/ignored/etc) clears any pending prompt.
        const bool promptable = status.isEmpty() || status == "pending" || status == "shown";
        _pendRecJson = promptable ? payload : String("");
        ws.textAll(payload);
    });

    pluginManager->on("rl:recommendation:cleared", [this](Event const &) {
        rlLastRecommendationId = "";
        rlLastRecommendationAt = 0;
        rlRecommendationApplyStatus = "";
        rlRecommendationStatus = "";
        rlRecommendationMode = "";
        rlRecommendationGrindDeltaStepsFromCurrent = 0.0f;
        rlRecommendationGrindDeltaUmFromCurrent = 0.0f;
        rlRecommendationProjectedRelativeStepFromReference = 0.0f;
        rlRecommendationProjectedRelativeGrindUmFromReference = 0.0f;
        rlRecommendationNextDoseG = 0.0f;
        rlRecommendationTargetYieldG = 0.0f;
        rlRecommendationTargetRatio = 0.0f;
        rlRecommendationHasCurrentAbsoluteStep = false;
        rlRecommendationCurrentAbsoluteStep = 0.0f;
        rlRecommendationHasProjectedAbsoluteStep = false;
        rlRecommendationProjectedAbsoluteStep = 0.0f;
        _pendRecJson = "";

        JsonDocument doc;
        doc["tp"] = "evt:rl:recommendation-clear";
        ws.textAll(doc.as<String>());
    });

    pluginManager->on("rl:shot:complete", [this](Event const &event) {
        if (!rlParticipationEnabled(controller)) {
            clearPendingPreferencePrompt();
            _queuedPreferencePrompts.clear();
            return;
        }

        if (event.getInt("preference_feedback_required") <= 0) {
            return;
        }
        const String shotId = event.getString("shot_id");
        if (shotId == _pendingPreferenceShotId) {
            return;
        }
        for (Event const &pending : _queuedPreferencePrompts) {
            if (pending.getString("shot_id") == shotId) {
                return;
            }
        }
        if (_pendingPreferenceShotId.isEmpty()) {
            if (activatePreferencePrompt(event)) {
                sendPreferencePrompt(nullptr);
            }
        } else {
            _queuedPreferencePrompts.push_back(event);
        }
    });

    pluginManager->on("rl:dose-confirmation:required", [this](Event const &event) {
        if (!rlParticipationEnabled(controller)) {
            clearPendingDoseConfirmation();
            _queuedDoseConfirmations.clear();
            return;
        }
        const String shotId = event.getString("shot_id");
        const float targetG = event.getFloat("dose_target_g");
        if (shotId.isEmpty() || !std::isfinite(targetG) || targetG <= 0.0f) {
            return;
        }
        if (shotId == _pendingDoseShotId) {
            return;
        }
        for (Event const &pending : _queuedDoseConfirmations) {
            if (pending.getString("shot_id") == shotId) {
                return;
            }
        }
        if (_pendingDoseShotId.isEmpty()) {
            activateDoseConfirmation(event);
            sendDoseConfirmationPrompt(nullptr);
        } else {
            _queuedDoseConfirmations.push_back(event);
        }
    });

    pluginManager->on("rl:dose-confirmation:resolved", [this](Event const &event) {
        if (event.getString("shot_id") == _pendingDoseShotId) {
            clearPendingDoseConfirmation();
            advanceDoseConfirmation();
        }
        JsonDocument doc;
        doc["tp"] = "evt:rl:dose-confirmation-resolved";
        doc["shot_id"] = event.getString("shot_id");
        doc["followed"] = event.getInt("followed") == 1;
        doc["persisted"] = event.getInt("persisted") == 1;
        ws.textAll(doc.as<String>());
    });

    pluginManager->on("rl:prompts:invalidated", [this](Event const &event) {
        const String shotId = event.getString("shot_id");
        if (shotId.isEmpty()) {
            clearPendingPreferencePrompt();
            clearPendingDoseConfirmation();
            _queuedPreferencePrompts.clear();
            _queuedDoseConfirmations.clear();
            _pendRecJson = "";
        } else {
            if (_pendingPreferenceShotId == shotId) {
                clearPendingPreferencePrompt();
                advancePreferencePrompt();
            }
            if (_pendingDoseShotId == shotId) {
                clearPendingDoseConfirmation();
                advanceDoseConfirmation();
            }
            _queuedPreferencePrompts.erase(
                std::remove_if(_queuedPreferencePrompts.begin(), _queuedPreferencePrompts.end(),
                               [&shotId](Event const &pending) { return pending.getString("shot_id") == shotId; }),
                _queuedPreferencePrompts.end());
            _queuedDoseConfirmations.erase(
                std::remove_if(_queuedDoseConfirmations.begin(), _queuedDoseConfirmations.end(),
                               [&shotId](Event const &pending) { return pending.getString("shot_id") == shotId; }),
                _queuedDoseConfirmations.end());
        }

        JsonDocument doc;
        doc["tp"] = "evt:rl:prompts-clear";
        if (!shotId.isEmpty()) {
            doc["shot_id"] = shotId;
        }
        ws.textAll(doc.as<String>());
    });

    pluginManager->on("rl:preference", [this](Event const &event) {
        if (event.getInt("decision_persisted") == 1 && !_pendingPreferenceShotId.isEmpty() &&
            event.getString("install_id") == _pendPreferenceInstallId &&
            event.getString("optimization_run_id") == _pendPreferenceRunId &&
            event.getString("new_shot_id") == _pendingPreferenceShotId &&
            event.getString("anchor_shot_id") == _pendPreferenceAnchorShotId) {
            clearPendingPreferencePrompt();
            advancePreferencePrompt();
        }
    });

    pluginManager->on("rl:status:received", [this](Event const &event) {
        if (!AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(), controller->getOptimizerTransport())
                 .acceptOffBoardStatus()) {
            return;
        }

        rlStatusSeen = event.getInt("seen") > 0;
        rlAddonOnline = event.getInt("addon_online") > 0;
        rlLastStatusAt = event.getInt64("timestamp");
        rlLastShotId = event.getString("last_shot_id");
        rlLastShotAt = event.getInt64("last_shot_at");
        rlLastShotType = event.getString("last_shot_type");
        rlLastShotTimeS = event.getInt("last_shot_time_s_x10") / 10.0f;
        rlLastShotBeverageOutG = event.getInt("last_shot_beverage_out_g_x10") / 10.0f;
        rlLastShotTargetYieldG = event.getInt("last_shot_target_yield_g_x10") / 10.0f;
        rlLastRecommendationAt = event.getInt64("last_recommendation_at");
        const String statusRecommendationId = event.getString("last_recommendation_id");
        rlRecommendationApplyStatus =
            !rlLastRecommendationId.isEmpty() && statusRecommendationId == rlLastRecommendationId
                ? event.getString("recommendation_apply_status")
                : String("");
        rlMode = event.getString("mode");
        rlOptimizerProfileId = event.getString("optimizer_profile_id");
        rlOptimizerProfileLabel = event.getString("optimizer_profile_label");
        rlOptimizerConfiguredMode = event.getString("optimizer_configured_mode");
        rlOptimizerEffectiveMode = event.getString("optimizer_effective_mode");
        rlOptimizerFallbackReason = event.getString("optimizer_fallback_reason");
        rlCPBOEffectiveProfileName = event.getString("cpbo_profile_name");
        rlCPBOEffectiveComparisonMode = event.getString("cpbo_comparison_mode");
        rlLocalShotCount = event.getInt("local_shot_count");
        rlRuntimeHealthStatus = event.getString("runtime_health_status");
        rlRuntimeHealthSummary = event.getString("runtime_health_summary");
        rlRuntimeHealthWarningsJson =
            event.getString("runtime_health_warnings_json").isEmpty() ? "[]" : event.getString("runtime_health_warnings_json");
        rlRuntimeHealthWaitingReasonsJson = event.getString("runtime_health_waiting_reasons_json").isEmpty()
                                                ? "[]"
                                                : event.getString("runtime_health_waiting_reasons_json");
        rlAutoTuningDiagnosticStepsJson = event.getString("auto_tuning_diagnostic_steps_json").isEmpty()
                                              ? "[]"
                                              : event.getString("auto_tuning_diagnostic_steps_json");
        rlRuntimeHealthStorageBackend = event.getString("runtime_health_storage_backend");
        rlRuntimeHealthStorageAvailable = event.getInt("runtime_health_storage_available") > 0;
        rlGrinderCatalogSearchUrl = event.getString("grinder_catalog_search_url");
        rlRecentShotsJson = event.getString("recent_shots_json").isEmpty() ? "[]" : event.getString("recent_shots_json");

        JsonDocument doc;
        Settings const &settings = controller->getSettings();
        doc["tp"] = "evt:rl:status";
        AutoTuning::Router router(settings.getRLOptimizerConfiguration(), controller->getOptimizerTransport());
        doc["rlAutoTuningEnabled"] = router.enabled();
        doc["rlProviderMode"] = settings.getRLAutoTuningProviderMode();
        doc["rlProviderStatus"] = router.providerStatus();
        doc["rlProviderSummary"] = router.providerSummary();
        const int mqttPort = settings.getHomeAssistantPort();
        doc["rlMqttConfigured"] = !settings.getHomeAssistantIP().isEmpty() && mqttPort > 0 && mqttPort <= 65535;
        doc["rlMqttConnected"] = controller->getOptimizerTransport() && controller->getOptimizerTransport()->connected();
        doc["legacyHomeAssistantMqttAvailable"] = FeatureFlags::legacyHomeAssistantMqtt;
        doc["rlBeanContextId"] = settings.getRLBeanContextId();
        doc["rlBeanContextName"] = settings.getRLBeanContextName();
        doc["rlGrinderContextId"] = settings.getRLGrinderContextId();
        doc["rlGrinderContextName"] = settings.getRLGrinderContextName();
        JsonDocument activeTasteGoal;
        AutoTuning::activeTasteGoal(settings, activeTasteGoal);
        doc["rlTasteGoal"].set(activeTasteGoal.as<JsonVariantConst>());
        doc["rlTasteGoalSummary"] = AutoTuning::tasteGoalSummary(activeTasteGoal.as<JsonVariantConst>());
        doc["rlGrinderCatalogSearchUrl"] = rlGrinderCatalogSearchUrl;
        JsonDocument grinderContextsDoc;
        JsonArray grinderContexts = loadRLContexts(grinderContextsDoc, settings.getRLGrinderContextsJson());
        JsonObject activeGrinderContext = findRLContext(grinderContexts, settings.getRLGrinderContextId());
        if (!activeGrinderContext.isNull()) {
            JsonObject calibration = doc["rlActiveGrinderCalibration"].to<JsonObject>();
            copyRLGrinderCalibration(calibration, activeGrinderContext);
        }
        doc["rlOptimizationPaused"] = settings.isRLOptimizationPaused();
        doc["rlLocalOptimizationEnabled"] = router.optimizationActive();
        doc["rlStatusSeen"] = rlStatusSeen;
        doc["rlAddonOnline"] = rlAddonOnline;
        doc["rlLastStatusAt"] = rlLastStatusAt;
        doc["rlLastShotId"] = rlLastShotId;
        doc["rlLastShotAt"] = rlLastShotAt;
        doc["rlLastShotType"] = rlLastShotType;
        doc["rlLastShotTimeS"] = rlLastShotTimeS;
        doc["rlLastShotBeverageOutG"] = rlLastShotBeverageOutG;
        doc["rlLastShotTargetYieldG"] = rlLastShotTargetYieldG;
        doc["rlLastRecommendationId"] = rlLastRecommendationId;
        doc["rlLastRecommendationAt"] = rlLastRecommendationAt;
        doc["rlRecommendationApplyStatus"] = rlRecommendationApplyStatus;
        doc["rlRecommendationStatus"] = rlRecommendationStatus;
        doc["rlRecommendationMode"] = rlRecommendationMode;
        doc["rlRecommendationGrindDeltaStepsFromCurrent"] = rlRecommendationGrindDeltaStepsFromCurrent;
        doc["rlRecommendationGrindDeltaUmFromCurrent"] = rlRecommendationGrindDeltaUmFromCurrent;
        doc["rlRecommendationProjectedRelativeStepFromReference"] = rlRecommendationProjectedRelativeStepFromReference;
        doc["rlRecommendationProjectedRelativeGrindUmFromReference"] = rlRecommendationProjectedRelativeGrindUmFromReference;
        doc["rlRecommendationNextDoseG"] = rlRecommendationNextDoseG;
        doc["rlRecommendationTargetYieldG"] = rlRecommendationTargetYieldG;
        doc["rlRecommendationTargetRatio"] = rlRecommendationTargetRatio;
        doc["rlRecommendationHasCurrentAbsoluteStep"] = rlRecommendationHasCurrentAbsoluteStep;
        doc["rlRecommendationCurrentAbsoluteStep"] = rlRecommendationCurrentAbsoluteStep;
        doc["rlRecommendationHasProjectedAbsoluteStep"] = rlRecommendationHasProjectedAbsoluteStep;
        doc["rlRecommendationProjectedAbsoluteStep"] = rlRecommendationProjectedAbsoluteStep;
        doc["rlMode"] = rlMode;
        doc["rlOptimizerProfileId"] = rlOptimizerProfileId;
        doc["rlOptimizerProfileLabel"] = rlOptimizerProfileLabel;
        doc["rlOptimizerMode"] = settings.getRLOptimizerMode();
        doc["rlCPBOProfileName"] = settings.getRLCPBOProfileName();
        doc["rlCPBOComparisonMode"] = settings.getRLCPBOComparisonMode();
        doc["rlCPBOEffectiveProfileName"] = rlCPBOEffectiveProfileName;
        doc["rlCPBOEffectiveComparisonMode"] = rlCPBOEffectiveComparisonMode;
        doc["rlDoseTargetG"] = settings.getTargetGrindVolume();
        addRLRecipeDomain(doc.as<JsonObject>(), settings);
        doc["rlOptimizerConfiguredMode"] = rlOptimizerConfiguredMode;
        doc["rlOptimizerEffectiveMode"] = rlOptimizerEffectiveMode;
        doc["rlOptimizerFallbackReason"] = rlOptimizerFallbackReason;
        doc["rlLocalShotCount"] = rlLocalShotCount;
        doc["rlRuntimeHealthStatus"] = rlRuntimeHealthStatus;
        doc["rlRuntimeHealthSummary"] = rlRuntimeHealthSummary;
        copyRLStringArray(doc, "rlRuntimeHealthWarnings", rlRuntimeHealthWarningsJson);
        copyRLStringArray(doc, "rlRuntimeHealthWaitingReasons", rlRuntimeHealthWaitingReasonsJson);
        copyRLDiagnosticSteps(doc, rlAutoTuningDiagnosticStepsJson);
        doc["rlRuntimeHealthStorageBackend"] = rlRuntimeHealthStorageBackend;
        doc["rlRuntimeHealthStorageAvailable"] = rlRuntimeHealthStorageAvailable;
        doc["rlLocalDeliveryPendingCount"] = rlLocalDeliveryPendingCount;
        doc["rlLocalDeliveryRetryCount"] = rlLocalDeliveryRetryCount;
        doc["rlLocalDeliveryRejectedCount"] = rlLocalDeliveryRejectedCount;
        doc["rlLocalDeliveryLastError"] = rlLocalDeliveryLastError;
        copyRLRecentShots(doc, rlRecentShotsJson);
        ws.textAll(doc.as<String>());
    });

    pluginManager->on("rl:community-upload:status", [this](Event const &event) {
        communityUploadRequested = event.getInt("requested") > 0;
        communityUploadEffective = event.getInt("effective") > 0;
        communityUploadConfigured = event.getInt("configured") > 0;
        communityUploadStatus = event.getString("status");
        communityUploadSummary = event.getString("summary");
        communityUploadStorageBackend = event.getString("storage_backend");
        communityUploadStorageAvailable = event.getInt("storage_available") > 0;
        communityUploadPendingCount = event.getInt("pending_count");
        communityUploadRetryCount = event.getInt("failed_count");
        communityUploadRejectedCount = event.getInt("rejected_count");

        JsonDocument doc(&psramAllocator);
        Settings const &settings = controller->getSettings();
        doc["tp"] = "evt:community-upload:status";
        doc["rlCommunityUploadEnabled"] = settings.isRLCommunityUploadEnabled();
        doc["rlCommunityUploadPrompted"] = settings.isRLCommunityUploadPrompted();
        doc["rlUploadBaseUrl"] = settings.getRLUploadBaseUrl();
        doc["rlUploadCredentialConfigured"] = settings.hasRLUploadCredentials();
        doc["communityUploadRequested"] = communityUploadRequested;
        doc["communityUploadEffective"] = communityUploadEffective;
        doc["communityUploadConfigured"] = communityUploadConfigured;
        doc["communityUploadStatus"] = communityUploadStatus;
        doc["communityUploadSummary"] = communityUploadSummary;
        doc["communityUploadStorageBackend"] = communityUploadStorageBackend;
        doc["communityUploadStorageAvailable"] = communityUploadStorageAvailable;
        doc["communityUploadPendingCount"] = communityUploadPendingCount;
        doc["communityUploadRetryCount"] = communityUploadRetryCount;
        doc["communityUploadRejectedCount"] = communityUploadRejectedCount;
        broadcastJson(doc);
    });
    pluginManager->on("rl:local_store:status", [this](Event const &event) {
        rlLocalDeliveryPendingCount = event.getInt("delivery_pending_count");
        rlLocalDeliveryRetryCount = event.getInt("delivery_retry_count");
        rlLocalDeliveryRejectedCount = event.getInt("delivery_rejected_count");
        rlLocalDeliveryLastError = event.getString("delivery_last_error");

        JsonDocument doc;
        doc["tp"] = "evt:rl:status";
        doc["rlLocalDeliveryPendingCount"] = rlLocalDeliveryPendingCount;
        doc["rlLocalDeliveryRetryCount"] = rlLocalDeliveryRetryCount;
        doc["rlLocalDeliveryRejectedCount"] = rlLocalDeliveryRejectedCount;
        doc["rlLocalDeliveryLastError"] = rlLocalDeliveryLastError;
        broadcastJson(doc);
    });
    auto clearRLPromptsIfInactive = [this](Event const &) {
        if (rlParticipationEnabled(controller)) {
            return;
        }
        clearPendingPreferencePrompt();
        clearPendingDoseConfirmation();
        _queuedPreferencePrompts.clear();
        _queuedDoseConfirmations.clear();
        _pendRecJson = "";

        JsonDocument doc;
        doc["tp"] = "evt:rl:prompts-clear";
        ws.textAll(doc.as<String>());
    };
    pluginManager->on("settings:changed", clearRLPromptsIfInactive);
    pluginManager->on("rl:settings:changed", clearRLPromptsIfInactive);

    // Forward shot history rebuild progress events to WebSocket clients
    pluginManager->on("evt:history-rebuild-progress", [this](Event const &event) {
        JsonDocument doc(&psramAllocator);
        doc["tp"] = "evt:history-rebuild-progress";
        doc["total"] = event.getInt("total");
        doc["current"] = event.getInt("current");
        doc["status"] = event.getString("status");
        broadcastJson(doc);
    });

    // Forward "shot saved to history" events to WebSocket clients, so the
    // dashboard can refetch the recent-shots buffer at the right time.
    pluginManager->on("evt:history-shot-saved", [this](Event const &event) {
        JsonDocument doc(&psramAllocator);
        doc["tp"] = "evt:history-shot-saved";
        doc["id"] = event.getInt("id");
        broadcastJson(doc);
    });

    // Forward live shot-finished stats (pressure/flow) to WebSocket clients, so
    // the dashboard's finished card can show them without waiting for the
    // history file write.
    pluginManager->on("evt:shot-finished-stats", [this](Event const &event) {
        JsonDocument doc(&psramAllocator);
        doc["tp"] = "evt:shot-finished-stats";
        doc["maxPressure"] = event.getFloat("maxPressure");
        doc["avgFlow"] = event.getFloat("avgFlow");
        broadcastJson(doc);
    });

    // Subscribe to Bluetooth scale weight updates
    pluginManager->on("controller:volumetric-measurement:bluetooth:change",
                      [this](Event const &event) { this->currentBluetoothWeight = event.getFloat("value"); });
    // Hardware scale and predictive flow estimate are tracked alongside BLE so
    // the WebUI 'cw' field can mirror whichever source the user has selected.
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    pluginManager->on("controller:volumetric-measurement:hardware:change",
                      [this](Event const &event) { this->currentHardwareWeight = event.getFloat("value"); });
#endif
    pluginManager->on("controller:volumetric-measurement:estimation:change",
                      [this](Event const &event) { this->currentEstimatedWeight = event.getFloat("value"); });

#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    // Forward async scale events as WebSocket messages so the calibration page can
    // drive its live progress bars and result toasts directly.
    pluginManager->on("controller:scale:tare:progress", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:tare:progress";
        doc["samples"] = ev.getInt("samples");
        doc["stddevG"] = ev.getFloat("stddevG");
        ws.textAll(doc.as<String>());
    });
    pluginManager->on("controller:scale:tare:done", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:tare:done";
        doc["success"] = ev.getInt("success");
        doc["offset1"] = ev.getFloat("offset1");
        doc["offset2"] = ev.getFloat("offset2");
        doc["std1"] = ev.getFloat("std1");
        doc["std2"] = ev.getFloat("std2");
        doc["healthBits"] = ev.getInt("healthBits");
        ws.textAll(doc.as<String>());
    });
    pluginManager->on("controller:scale:cal:progress", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:cal:progress";
        doc["channel"] = ev.getInt("channel");
        doc["samples"] = ev.getInt("samples");
        doc["stddevG"] = ev.getFloat("stddevG");
        ws.textAll(doc.as<String>());
    });
    pluginManager->on("controller:scale:cal:done", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:cal:done";
        doc["channel"] = ev.getInt("channel");
        doc["factor"] = ev.getFloat("factor");
        doc["stddevG"] = ev.getFloat("stddevG");
        doc["success"] = ev.getInt("success");
        doc["errorCode"] = ev.getInt("errorCode");
        ws.textAll(doc.as<String>());
    });
    pluginManager->on("controller:scale:not-ready", [this](Event const &ev) {
        JsonDocument doc;
        doc["tp"] = "evt:scale:not-ready";
        doc["source"] = ev.getInt("source");
        ws.textAll(doc.as<String>());
    });
#endif

    setupServer();
}

void WebUIPlugin::loop() {
    if (otaUpdateCheckComplete.exchange(false, std::memory_order_acquire)) {
        pluginManager->trigger("ota:update:status", "value", ota->isUpdateAvailable());
        updateOTAStatus(ota->getCurrentVersion());
    }
    if (updating && !otaUpdateCheckInProgress.load(std::memory_order_acquire)) {
        // Pass which component is being flashed: a controller update streams the
        // firmware over BLE (wants a low-latency link), a display update is over
        // Wi-Fi (wants BLE to stay out of the radio's way). "" = both.
        pluginManager->trigger("ota:update:start", "component", updateComponent);
        ota->update(CONTROLLER_OTA_ENABLED && updateComponent != "display", updateComponent != "controller");
        pluginManager->trigger("ota:update:end");
        updating = false;
    }
    if (!serverRunning) {
        return;
    }
    const unsigned long now = millis();
    // Run the TLS check on a low-priority worker only while idle so it cannot
    // block loopTask and does not compete with an active process for memory.
    // Subtraction keeps the interval check millis()-rollover-safe.
    if (!updating && !controller->isActive() && (lastUpdateCheck == 0 || now - lastUpdateCheck > UPDATE_CHECK_INTERVAL)) {
        startOTAUpdateCheck(now);
    }
    if (now > lastStatus + STATUS_PERIOD && !ws.getClients().empty()) {
        lastStatus = now;
        statusDoc.clear();
        statusDoc["tp"] = "evt:status";
        statusDoc["ct"] = controller->getCurrentTemp();
        statusDoc["tt"] = controller->getTargetTemp();
        statusDoc["pr"] = controller->getCurrentPressure();
        statusDoc["fl"] = controller->getCurrentPumpFlow();
        statusDoc["pt"] = controller->getTargetPressure();
        statusDoc["m"] = controller->getMode();
        statusDoc["bsp"] = controller->isBrewStartPending() ? 1 : 0;
        statusDoc["p"] = controller->getProfileManager()->getSelectedProfile().label;
        statusDoc["puid"] = controller->getProfileManager()->getSelectedProfile().id;
        statusDoc["cp"] = controller->getSystemInfo().capabilities.pressure;
        statusDoc["cd"] = controller->getSystemInfo().capabilities.dimming;
        statusDoc["ctof"] = controller->getSystemInfo().capabilities.tof;
        statusDoc["gp"] = controller->getSystemInfo().capabilities.hasAddon(7);
        statusDoc["tw"] = profileManager->getSelectedProfile().getTotalVolume(); // total target weight for the process
        statusDoc["bta"] = controller->isVolumetricAvailable() ? 1 : 0;
        statusDoc["bt"] =
            controller->isVolumetricAvailable() && controller->getProfileManager()->getSelectedProfile().isVolumetric() ? 1 : 0;
        statusDoc["btd"] = profileManager->getSelectedProfile().getTotalDuration();
        statusDoc["led"] = controller->getSystemInfo().capabilities.ledControl;
        statusDoc["gtd"] = controller->getTargetGrindDuration();
        statusDoc["gtv"] = controller->getSettings().getTargetGrindVolume();
        statusDoc["gta"] = controller->isGrindVolumetricAvailable() ? 1 : 0;
        statusDoc["gt"] = controller->isGrindVolumetricAvailable() && controller->getSettings().isVolumetricTarget() ? 1 : 0;
        statusDoc["gact"] = controller->isGrindActive() ? 1 : 0;
        statusDoc["wl"] = controller->getWaterLevel();
        statusDoc["tof"] = controller->getTofDistance();
        statusDoc["rssi"] = 0;
        statusDoc["lat"] = -1; // BLE round-trip latency (ms); -1 = not yet measured
        statusDoc["pw"] = controller->getCurrentPumpPower();
        statusDoc["hp"] = controller->getCurrentHeaterPower();

#ifndef GAGGIMATE_UART_COMMS
        if (controller->getClientController()->getClient() != nullptr && controller->getClientController()->getClient()->isConnected()) {
            statusDoc["rssi"] = controller->getClientController()->getClient()->getRssi();
        }
#endif
        if (controller->getClientController()->hasLatency()) {
            statusDoc["lat"] = controller->getClientController()->getLatencyMs();
        }

        const bool bleConnected = BLEScales.isConnected();
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
        const ScaleSample sc = controller->getScaleSample();
        const bool hwScalePresent = controller->isHardwareScalePresent();
        const bool hwHealthy = controller->isHardwareScaleSampleHealthy(sc);

        const float displayHwG = sc.weightG;

        // 'cw' (currentWeight) mirrors the source the brew controller is acting
        // on (or would, when idle), so the WebUI chart and Process Controls card
        // show the same weight the brew targets against. 'bw' keeps its raw
        // BLE-only meaning.
        const VolumetricMeasurementSource brewSource =
            controller->isActive() ? controller->getCurrentVolumetricSource() : controller->getResolvedBrewSource();
        float cw = 0.0f;
        switch (brewSource) {
        case VolumetricMeasurementSource::HARDWARE_SCALE:
            cw = hwHealthy ? displayHwG : 0.0f;
            break;
        case VolumetricMeasurementSource::BLUETOOTH:
            cw = bleConnected ? this->currentBluetoothWeight : 0.0f;
            break;
        case VolumetricMeasurementSource::FLOW_ESTIMATION:
            cw = this->currentEstimatedWeight;
            break;
        default:
            cw = 0.0f;
            break;
        }

        statusDoc["bw"] = bleConnected ? this->currentBluetoothWeight : 0; // raw BLE weight
        statusDoc["hw"] = hwHealthy ? displayHwG : 0.0f;
        statusDoc["hwc"] = hwScalePresent;
        statusDoc["cw"] = cw;                                              // active-source weight
        statusDoc["bc"] = bleConnected;                                    // bluetooth scale connected status

        // Hardware scale: structured snapshot. Flat hw/hwc stay for existing UI.
        auto sObj = statusDoc["scale"].to<JsonObject>();
        // Never surface an implausible headline weight: an uncalibrated/un-tared
        // controller emits raw counts (tens of thousands of grams). Show 0 when
        // the sample is unhealthy or out of range; raw per-channel c1/c2 below
        // stay unclamped for diagnostics.
        sObj["w"] = (hwHealthy && std::isfinite(displayHwG)) ? displayHwG : 0.0f;
        sObj["sd"] = sc.stddevG;
        sObj["c1"] = sc.ch1G;
        sObj["c2"] = sc.ch2G;
        sObj["sd1"] = sc.ch1StdG;
        sObj["sd2"] = sc.ch2StdG;
        sObj["h"] = sc.healthBits;
        sObj["seq"] = sc.sampleSeq;
        sObj["pr"] = hwScalePresent;
        sObj["bl"] = 0.0f;
#else
        const VolumetricMeasurementSource brewSource =
            controller->isActive() ? controller->getCurrentVolumetricSource() : controller->getResolvedBrewSource();
        float cw = 0.0f;
        switch (brewSource) {
        case VolumetricMeasurementSource::BLUETOOTH:
            cw = bleConnected ? this->currentBluetoothWeight : 0.0f;
            break;
        case VolumetricMeasurementSource::FLOW_ESTIMATION:
            cw = this->currentEstimatedWeight;
            break;
        default:
            cw = 0.0f;
            break;
        }

        statusDoc["bw"] = bleConnected ? this->currentBluetoothWeight : 0; // raw BLE weight
        statusDoc["hw"] = 0.0f;
        statusDoc["hwc"] = false;
        statusDoc["cw"] = cw;                                              // active-source weight
        statusDoc["bc"] = bleConnected;                                    // bluetooth scale connected status
        auto sObj = statusDoc["scale"].to<JsonObject>();
        sObj["w"] = 0.0f;
        sObj["sd"] = 0.0f;
        sObj["c1"] = 0.0f;
        sObj["c2"] = 0.0f;
        sObj["sd1"] = 0.0f;
        sObj["sd2"] = 0.0f;
        sObj["h"] = 0;
        sObj["seq"] = 0;
        sObj["pr"] = false;
        sObj["bl"] = 0.0f;
#endif
        // Scale battery â€” only surfaced when the driver reports one and the
        // value isn't the UNKNOWN sentinel (255). UI omits the battery pill
        // entirely when `sbat` is absent, so disconnected/unknown scales don't
        // render a stale stub.
        if (bleConnected && BLEScales.hasBatteryLevel()) {
            const uint8_t pct = BLEScales.getBatteryLevel();
            if (pct != REMOTE_SCALES_BATTERY_UNKNOWN) {
                statusDoc["sbat"] = pct;
            }
        }

        // Scale source routing, so the UI can show which source each role uses
        // (enum: 0=none/INACTIVE, 1=predictive, 2=bluetooth, 3=hardware).
        statusDoc["brewSource"] = static_cast<int>(controller->getResolvedBrewSource());
        statusDoc["grindSource"] = static_cast<int>(controller->getResolvedGrindSource());
        statusDoc["activeSource"] = static_cast<int>(controller->getCurrentVolumetricSource());
        statusDoc["scaleCapable"] = controller->scaleAvailability().hardwareCapable;
#if defined(GAGGIMATE_UART_DIAGNOSTICS)
        statusDoc["err"] = controller->getError();
        statusDoc["ready"] = controller->isReady();
        const ControllerDiagnostics controllerDiagnostics = controller->getControllerDiagnostics();
        auto ctrlObj = statusDoc["ctrl"].to<JsonObject>();
        ctrlObj["e"] = controllerDiagnostics.errorCode;
        ctrlObj["te"] = controllerDiagnostics.thermocoupleError;
        ctrlObj["ts"] = controllerDiagnostics.thermocoupleStatus;
        ctrlObj["tec"] = controllerDiagnostics.thermocoupleErrorCount;
        ctrlObj["trc"] = controllerDiagnostics.thermocoupleReadCount;
        ctrlObj["traw"] = controllerDiagnostics.thermocoupleRawTemperature;
        ctrlObj["tf"] = controllerDiagnostics.thermocoupleTemperature;
        ctrlObj["ttask"] = controllerDiagnostics.thermocoupleTaskRunning;
        ctrlObj["hsp"] = controllerDiagnostics.heaterSetpoint;
        ctrlObj["hout"] = controllerDiagnostics.heaterOutput;
        ctrlObj["hr"] = controllerDiagnostics.heaterRelay;
        ctrlObj["bcmd"] = controllerDiagnostics.boilerCommandCount;
        ctrlObj["pcmd"] = controllerDiagnostics.pumpCommandCount;
        ctrlObj["rcmd"] = controllerDiagnostics.relayCommandCount;
        ctrlObj["ping"] = controllerDiagnostics.pingCommandCount;
        ctrlObj["tare"] = controllerDiagnostics.tareCommandCount;
        ctrlObj["lbsp"] = controllerDiagnostics.lastBoilerSetpoint;
        ctrlObj["lpp"] = controllerDiagnostics.lastPumpPower;
        ctrlObj["lro"] = controllerDiagnostics.lastRelayOpen;
        ctrlObj["urx"] = controllerDiagnostics.uartRxBytes;
        ctrlObj["utx"] = controllerDiagnostics.uartTxBytes;
        ctrlObj["uvf"] = controllerDiagnostics.uartValidFrames;
        ctrlObj["upe"] = controllerDiagnostics.uartParsedPayloads;
        ctrlObj["heap"] = controllerDiagnostics.freeHeap;
#if defined(GAGGIMATE_UART_COMMS)
        const UartDiagnostics uartDiagnostics = controller->getUartDiagnostics();
        statusDoc["uc"] = controller->getClientController()->isConnected();
        statusDoc["upe"] = uartDiagnostics.displayParsedEventCount;
        statusDoc["uro"] = uartDiagnostics.displayRxOverflowCount;
        statusDoc["utd"] = uartDiagnostics.displayTxDropCount;
        statusDoc["utb"] = uartDiagnostics.displayTxByteCount;
        statusDoc["urb"] = uartDiagnostics.displayRxByteCount;
        statusDoc["uvf"] = uartDiagnostics.displayValidFrameCount;
        statusDoc["umf"] = uartDiagnostics.displayMalformedFrameCount;
        statusDoc["ucrc"] = uartDiagnostics.displayCrcErrorCount;
        statusDoc["ulu"] = uartDiagnostics.displayLinkUpCount;
        statusDoc["uld"] = uartDiagnostics.displayLinkDownCount;
#endif
#endif

        // Deref under the process lock; other tasks can delete the process at any time (GM-147).
        // Released before broadcastJson so the ws send never runs under the lock.
        std::unique_lock<std::recursive_mutex> processGuard(controller->getProcessLock());
        Process *process = controller->getProcess();
        if (process == nullptr) {
            process = controller->getLastProcess();
        }
        if (process != nullptr) {
            auto pObj = statusDoc["process"].to<JsonObject>();
            pObj["a"] = controller->isActive() ? 1 : 0;
            statusDoc["pkr"] = controller->getCurrentPuckResistance();
            statusDoc["pf"] = controller->getCurrentPuckFlow();
            statusDoc["tf"] = controller->getTargetFlow();
            if (process->getType() == MODE_BREW) {
                auto *brew = static_cast<BrewProcess *>(process);
                unsigned long ts = brew->isActive() && controller->isActive() ? millis() : brew->finished;
                pObj["s"] = brew->currentPhase.phase == PhaseType::PHASE_TYPE_BREW ? "brew" : "infusion";
                pObj["l"] = brew->isActive() ? brew->currentPhase.name.c_str() : "Finished";
                pObj["e"] = ts - brew->processStarted;
                const bool isVolumetric = brew->target == ProcessTarget::VOLUMETRIC && brew->currentPhase.hasVolumetricTarget() &&
                                          controller->isVolumetricAvailable();
                pObj["tt"] = isVolumetric ? "volumetric" : "time";
                if (isVolumetric) {
                    Target t = brew->currentPhase.getVolumetricTarget();
                    pObj["pt"] = t.value;
                    pObj["pp"] = brew->currentVolume;
                } else {
                    pObj["pt"] = brew->getPhaseDuration();
                    pObj["pp"] = ts - brew->currentPhaseStarted;
                }
            } else if (process->getType() == MODE_GRIND) {
                auto *grind = static_cast<GrindProcess *>(process);
                unsigned long ts = grind->isActive() && controller->isActive() ? millis() : grind->finished;
                pObj["s"] = "grind";
                pObj["l"] = grind->isActive() ? "Grinding" : "Finished";
                pObj["e"] = ts - grind->started;
                const bool isVolumetric = grind->target == ProcessTarget::VOLUMETRIC && controller->isGrindVolumetricAvailable();
                pObj["tt"] = isVolumetric ? "volumetric" : "time";
                if (isVolumetric) {
                    pObj["pt"] = grind->grindVolume;
                    pObj["pp"] = grind->currentVolume;
                } else {
                    pObj["pt"] = grind->time;
                    pObj["pp"] = ts - grind->started;
                }
            }
        }
        processGuard.unlock();

        // Diagnostics for hang triage: heap usage, largest free block, historical
        // minimum, uptime in seconds. Watch for hl falling while hf stays high
        // (fragmentation) vs steady hf decline (leak).
        statusDoc["hf"] = ESP.getFreeHeap();
        statusDoc["hl"] = ESP.getMaxAllocHeap();
        statusDoc["hm"] = ESP.getMinFreeHeap();
        statusDoc["up"] = millis() / 1000;

        broadcastJson(statusDoc);
    }
    if (now > lastCleanup + CLEANUP_PERIOD) {
        lastCleanup = now;
        ws.cleanupClients();
        // Evict rxBuffers from clients that dropped TCP without a clean WS close
        // (mobile screen-lock, OS killing background tab). Otherwise these leak
        // until reboot.
        const unsigned long nowMs = millis();
        size_t evicted = 0;
        for (auto it = rxBufferLastActivity.begin(); it != rxBufferLastActivity.end();) {
            if (nowMs - it->second > RXBUFFER_IDLE_EVICT_MS) {
                rxBuffers.erase(it->first);
                it = rxBufferLastActivity.erase(it);
                ++evicted;
            } else {
                ++it;
            }
        }
        if (evicted > 0) {
            ESP_LOGI("WebUIPlugin", "Evicted %u idle rxBuffers", static_cast<unsigned>(evicted));
        }
    }
    if (now > lastDns + DNS_PERIOD && dnsServer != nullptr) {
        lastDns = now;
        dnsServer->processNextRequest();
    }
}

void WebUIPlugin::startOTAUpdateCheck(const unsigned long now) {
    bool expected = false;
    if (!otaUpdateCheckInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    otaUpdateCheckComplete.store(false, std::memory_order_release);
    if (xTaskCreatePinnedToCore(otaUpdateCheckTask, "OTA update check", 12288, this, 1, nullptr, 0) != pdPASS) {
        otaUpdateCheckInProgress.store(false, std::memory_order_release);
        ESP_LOGE("WebUIPlugin", "Unable to start OTA update check task");
        return;
    }
    lastUpdateCheck = now;
}

void WebUIPlugin::otaUpdateCheckTask(void *arg) {
    auto *plugin = static_cast<WebUIPlugin *>(arg);
    plugin->ota->checkForUpdates();
    plugin->otaUpdateCheckComplete.store(true, std::memory_order_release);
    plugin->otaUpdateCheckInProgress.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

// Linear lookup over the embedded asset table (~60 entries) â€” a couple of
// strcmps per request, negligible next to the network round-trip.
static const WebAsset *findWebAsset(const String &path) {
    for (size_t i = 0; i < WEB_ASSETS_COUNT; i++) {
        if (path == WEB_ASSETS[i].path) {
            return &WEB_ASSETS[i];
        }
    }
    return nullptr;
}

void WebUIPlugin::serveWebAsset(AsyncWebServerRequest *request) {
    String path = request->url();
    if (path.isEmpty() || path == "/") {
        path = WEB_UI_INDEX_PATH;
    }

    const WebAsset *asset = findWebAsset(path);
    if (asset == nullptr && !path.startsWith("/assets/")) {
        // SPA client-side routes (e.g. /settings, /profiles) aren't real files â€”
        // fall back to index.html. A miss under /assets/ is a genuine 404, not a
        // route, so it is not rewritten.
        asset = findWebAsset(WEB_UI_INDEX_PATH);
    }
    if (asset == nullptr) {
        request->send(404, "text/plain", "Not found");
        return;
    }

    // Serve straight from the memory-mapped flash blob â€” no copy into RAM, no
    // filesystem read. AsyncProgmemResponse streams from the pointer in chunks.
    AsyncWebServerResponse *response =
        request->beginResponse(200, asset->contentType, gWebUiBlobStart + asset->offset, asset->length);
    if (asset->gzip) {
        response->addHeader("Content-Encoding", "gzip");
    }
    // Content-hashed build assets (/assets/<hash>.js) never change for a given URL â€” cache them forever. index.html and
    // other unhashed files must revalidate so a new build is picked up after an update. [GM-83]
    if (path.startsWith("/assets/")) {
        response->addHeader("Cache-Control", "public, max-age=31536000, immutable");
    } else {
        response->addHeader("Cache-Control", "no-cache");
    }
    request->send(response);
}

void WebUIPlugin::setupServer() {
    server.on("/connecttest.txt", [](AsyncWebServerRequest *request) {
        request->redirect("http://logout.net");
    }); // windows 11 captive portal workaround
    server.on("/wpad.dat", [](AsyncWebServerRequest *request) {
        request->send(404);
    }); // Honestly don't understand what this is but a 404 stops win 10 keep calling this repeatedly and panicking the esp32
        // :)
    server.on("/generate_204",
              [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); }); // android captive portal redirect
    server.on("/redirect", [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); });            // microsoft redirect
    server.on("/hotspot-detect.html", [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); }); // apple call home
    server.on("/library/test/success.html",
              [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); }); // apple call home (newer iOS)
    server.on("/canonical.html",
              [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); });       // firefox captive portal call home
    server.on("/success.txt", [](AsyncWebServerRequest *request) { request->send(200); }); // firefox captive portal call home
    server.on("/ncsi.txt", [](AsyncWebServerRequest *request) { request->redirect(LOCAL_URL); }); // windows call home
    server.on("/api/settings", [this](AsyncWebServerRequest *request) { handleSettings(request); });
    server.on("/api/status", [this](AsyncWebServerRequest *request) {
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        JsonDocument doc(&psramAllocator);
        doc["mode"] = controller->getMode();
        doc["tt"] = controller->getTargetTemp();
        doc["ct"] = controller->getCurrentTemp();
        serializeJson(doc, *response);
        request->send(response);
    });
    server.on("/api/scales/list", [this](AsyncWebServerRequest *request) { handleBLEScaleList(request); });
    server.on("/api/scales/connect", [this](AsyncWebServerRequest *request) { handleBLEScaleConnect(request); });
    server.on("/api/scales/scan", [this](AsyncWebServerRequest *request) { handleBLEScaleScan(request); });
    server.on("/api/scales/info", [this](AsyncWebServerRequest *request) { handleBLEScaleInfo(request); });
    FS *fs = &LittleFS;
    if (controller->isSDCard()) {
        fs = &SD_MMC;
    }
    server.on("/api/history/index.bin", HTTP_GET, [fs](AsyncWebServerRequest *request) {
        // Serve the binary index file directly
        const bool indexExists =
            fs == &LittleFS ? LittleFSUtil::existsQuietly("/h/index.bin") : fs->exists("/h/index.bin");
        if (indexExists) {
            request->send(*fs, "/h/index.bin", "application/octet-stream");
        } else {
            request->send(404, "text/plain", "Index not found");
        }
    });
    server.on("/api/history/recent.bin", HTTP_GET, [this](AsyncWebServerRequest *request) {
        // The most recent non-deleted shots, newest first, as a regular shot
        // index (SIDX header + entries) — same binary format as index.bin,
        // just truncated, so clients reuse the index.bin parser.
        constexpr long MAX_RECENT_LIMIT = 50;
        long limit = 8;
        if (request->hasArg("limit")) {
            limit = constrain(request->arg("limit").toInt(), 1L, MAX_RECENT_LIMIT);
        }

        auto *entries = static_cast<ShotIndexEntry *>(ps_malloc(limit * sizeof(ShotIndexEntry)));
        if (entries == nullptr) {
            request->send(500, "text/plain", "Out of memory");
            return;
        }
        size_t count = ShotHistory.readRecentEntries(entries, limit);

        ShotIndexHeader header{};
        header.magic = SHOT_INDEX_MAGIC;
        header.version = SHOT_INDEX_VERSION;
        header.entrySize = SHOT_INDEX_ENTRY_SIZE;
        header.entryCount = count;
        header.nextId = 0; // meaningless for a partial view

        AsyncResponseStream *response = request->beginResponseStream("application/octet-stream");
        response->addHeader("Cache-Control", "no-store");
        response->write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
        response->write(reinterpret_cast<const uint8_t *>(entries), count * sizeof(ShotIndexEntry));
        free(entries);
        request->send(response);
    });
    // Exact generated endpoints must be registered before this broad file route.
    // Shot logs are raw binary files, so checking a nonexistent .gz sibling only
    // adds LittleFS opens and error logging for every history request.
    server.serveStatic("/api/history/", *fs, "/h/").setTryGzipFirst(false).setCacheControl("no-store");
    server.on("/api/core-dump", HTTP_GET, [this](AsyncWebServerRequest *request) { handleCoreDumpDownload(request); });
    // The web UI is embedded in firmware flash and served from the memory-mapped blob (see serveWebAsset). It is no
    // longer in LittleFS, so OTA never touches the partition holding profiles/shots. The catch-all onNotFound handles
    // every path not claimed by an explicit server.on()/api route above. [GM-106]
    server.onNotFound([this](AsyncWebServerRequest *request) { serveWebAsset(request); });
    ws.onEvent(
        [this](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
            if (type == WS_EVT_CONNECT) {
                // Close (and let the browser reconnect) a client whose send
                // queue backs up, instead of keeping it open. With it kept open
                // (false), a client that stalls under load â€” e.g. while the UI
                // is fetching many shot files for statistics â€” never has its
                // queued frames / AsyncTCP buffers reclaimed, so they accumulate
                // in internal DRAM until the whole IP stack starves (web + ICMP
                // die, no recovery). Reclaiming via close is the safer failure
                // mode. (Was the v1.8.1 behaviour.)
                client->setCloseClientOnQueueFull(true);
                ESP_LOGI("WebUIPlugin", "WebSocket client connected (%d open connections)", server->getClients().size());
                // Replay pending RL prompts so a reloaded/reconnected client restores
                // its reopen affordance (the WebUI dedupes/minimizes by id).
                if (!rlParticipationEnabled(controller)) {
                    clearPendingPreferencePrompt();
                    clearPendingDoseConfirmation();
                    _queuedPreferencePrompts.clear();
                    _queuedDoseConfirmations.clear();
                    _pendRecJson = "";
                }
                if (!_pendingDoseShotId.isEmpty()) {
                    sendDoseConfirmationPrompt(client);
                }
                if (!_pendingPreferenceShotId.isEmpty()) {
                    sendPreferencePrompt(client);
                }
                if (!_pendRecJson.isEmpty()) {
                    client->text(_pendRecJson);
                }
            } else if (type == WS_EVT_DISCONNECT) {
                ESP_LOGI("WebUIPlugin", "WebSocket client disconnected (%d open connections)", server->getClients().size());
                rxBuffers.erase(client->id());
            } else if (type == WS_EVT_DATA) {
                handleWebSocketData(server, client, type, arg, data, len);
            }
        });
    server.addHandler(&ws);
}

void WebUIPlugin::start() {
    if (serverRunning) {
        // Already listening. The 0.0.0.0:80 listen socket survives a WiFi
        // reconnect, so re-running end()+begin() only races AsyncTCP's async
        // socket close and fails to rebind ("bind: -8, port in use"). A transient
        // STA reconnect needs nothing done here.
        return;
    }
    server.begin();
    ESP_LOGI("WebUIPlugin", "Started webserver");
    if (apMode) {
        dnsServer = new DNSServer();
        dnsServer->setTTL(3600);
        dnsServer->start(53, "*", WIFI_AP_IP);
        ESP_LOGI("WebUIPlugin", "Started catchall DNS for captive portal");
    }
    lastUpdateCheck = millis();
    serverRunning = true;
}

void WebUIPlugin::stop() {
    if (!serverRunning)
        return;
    ws.closeAll();
    server.end();
    if (dnsServer != nullptr) {
        dnsServer->stop();
        delete dnsServer;
        dnsServer = nullptr;
    }
    serverRunning = false;
    ESP_LOGI("WebUIPlugin", "WebUIPlugin stopped (wifi disconnected)");
}

void WebUIPlugin::clearPendingPreferencePrompt() {
    _pendingPreferenceShotId = "";
    _pendingPreferenceRecommendationId = "";
    _pendPreferenceInstallId = "";
    _pendPreferenceRunId = "";
    _pendPreferenceAnchorShotId = "";
    _pendPreferenceComparisonMode = "";
    _pendPreferenceTasteGoal = AutoTuning::TasteGoal::balanced();
    _pendPreferenceTasteGoalSummary = "Balanced";
}

bool WebUIPlugin::activatePreferencePrompt(Event const &event) {
    AutoTuning::ShotCompletion const *completion = event.getPayload<AutoTuning::ShotCompletion>();
    if (!completion || !completion->recommendation.preferenceFeedbackRequired) {
        return false;
    }
    AutoTuning::RecommendationReference const &recommendation = completion->recommendation;
    const String shotId = completion->shotId.c_str();
    const String installId = recommendation.installId.c_str();
    const String runId = recommendation.optimizationRunId.c_str();
    const String anchorShotId = recommendation.anchorShotId.c_str();
    const String comparisonMode = AutoTuning::comparisonModeKey(recommendation.comparisonMode);
    const bool validMode = comparisonMode == "global_previous" || comparisonMode == "best_incumbent";
    if (shotId.isEmpty() || installId.isEmpty() || runId.isEmpty() || anchorShotId.isEmpty() || anchorShotId == shotId ||
        !validMode) {
        return false;
    }
    _pendingPreferenceShotId = shotId;
    _pendingPreferenceRecommendationId = recommendation.recommendationId.c_str();
    _pendPreferenceInstallId = installId;
    _pendPreferenceRunId = runId;
    _pendPreferenceAnchorShotId = anchorShotId;
    _pendPreferenceComparisonMode = comparisonMode;
    _pendPreferenceTasteGoal = recommendation.tasteGoal;
    _pendPreferenceTasteGoalSummary = AutoTuning::tasteGoalSummary(recommendation.tasteGoal);
    if (_pendPreferenceTasteGoalSummary.isEmpty()) {
        _pendPreferenceTasteGoalSummary = "Balanced";
    }
    return true;
}

void WebUIPlugin::advancePreferencePrompt() {
    while (_pendingPreferenceShotId.isEmpty() && !_queuedPreferencePrompts.empty()) {
        Event event = _queuedPreferencePrompts.front();
        _queuedPreferencePrompts.pop_front();
        if (activatePreferencePrompt(event)) {
            sendPreferencePrompt(nullptr);
            return;
        }
    }
}

void WebUIPlugin::clearPendingDoseConfirmation() {
    _pendingDoseShotId = "";
    _pendingDoseTargetG = 0.0f;
}

bool WebUIPlugin::activateDoseConfirmation(Event const &event) {
    const String shotId = event.getString("shot_id");
    const float targetG = event.getFloat("dose_target_g");
    if (shotId.isEmpty() || !std::isfinite(targetG) || targetG <= 0.0f) {
        return false;
    }
    _pendingDoseShotId = shotId;
    _pendingDoseTargetG = targetG;
    return true;
}

void WebUIPlugin::advanceDoseConfirmation() {
    while (_pendingDoseShotId.isEmpty() && !_queuedDoseConfirmations.empty()) {
        Event event = _queuedDoseConfirmations.front();
        _queuedDoseConfirmations.pop_front();
        if (activateDoseConfirmation(event)) {
            sendDoseConfirmationPrompt(nullptr);
            return;
        }
    }
}

void WebUIPlugin::sendDoseConfirmationPrompt(AsyncWebSocketClient *client) {
    if (_pendingDoseShotId.isEmpty() || _pendingDoseTargetG <= 0.0f) {
        return;
    }
    JsonDocument doc;
    doc["tp"] = "evt:rl:dose-confirmation";
    doc["shot_id"] = _pendingDoseShotId;
    doc["dose_target_g"] = _pendingDoseTargetG;
    const String payload = doc.as<String>();
    if (client) {
        client->text(payload);
    } else {
        ws.textAll(payload);
    }
}

void WebUIPlugin::sendPreferencePrompt(AsyncWebSocketClient *client) {
    if (_pendingPreferenceShotId.isEmpty()) {
        return;
    }
    JsonDocument doc;
    doc["tp"] = "evt:rl:shot-complete";
    doc["shot_id"] = _pendingPreferenceShotId;
    doc["recommendation_id"] = _pendingPreferenceRecommendationId;
    doc["preference_feedback_required"] = true;
    doc["install_id"] = _pendPreferenceInstallId;
    doc["optimization_run_id"] = _pendPreferenceRunId;
    doc["anchor_shot_id"] = _pendPreferenceAnchorShotId;
    doc["comparison_mode"] = _pendPreferenceComparisonMode;
    AutoTuning::writeTasteGoal(_pendPreferenceTasteGoal, doc["taste_goal"].to<JsonObject>());
    doc["taste_goal_summary"] = _pendPreferenceTasteGoalSummary;
    const String payload = doc.as<String>();
    if (client) {
        client->text(payload);
    } else {
        ws.textAll(payload);
    }
}

void WebUIPlugin::handleWebSocketData(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg,
                                      uint8_t *data, size_t len) {

    auto *info = static_cast<AwsFrameInfo *>(arg);
    const uint32_t cid = client->id();

    rxBufferLastActivity[cid] = millis();

    if (info->index == 0) {
        auto &buf = rxBuffers[cid];
        buf.clear();
        if (info->len <= 64 * 1024) {
            buf.reserve(info->len);
        }
    }

    auto &buf = rxBuffers[cid];
    buf.append(reinterpret_cast<const char *>(data), len);
    const bool isFinal = info->final && (info->index + len) == info->len;

    // If this is the final frame of the message, process and clear
    if (isFinal) {
        if (info->opcode == WS_TEXT) {
            ESP_LOGV("WebUIPlugin", "Received request: %.*s", (int)buf.size(), buf.c_str());
            JsonDocument doc(&psramAllocator);
            DeserializationError err = deserializeJson(doc, buf.c_str());
            if (!err) {
                String msgType = doc["tp"].as<String>();
                if (msgType.startsWith("req:profiles:")) {
                    handleProfileRequest(client->id(), doc);
                } else if (msgType == "req:ota-settings") {
                    handleOTASettings(client->id(), doc);
                } else if (msgType == "req:ota-start") {
                    handleOTAStart(client->id(), doc);
                } else if (msgType == "req:autotune-start") {
                    handleAutotuneStart(client->id(), doc);
                } else if (msgType == "req:rl:status:refresh" || msgType == "req:rl:context:list" ||
                           msgType == "req:rl:context:switch" || msgType == "req:rl:context:start-bean" ||
                           msgType == "req:rl:context:start-bag" || msgType == "req:rl:context:update" ||
                           msgType == "req:rl:context:retire" || msgType == "req:rl:context:delete" ||
                           msgType.startsWith("req:rl:grinder-context:") || msgType == "req:rl:local-optimization" ||
                           msgType == "req:rl:optimization:pause" || msgType == "req:rl:optimization:resume" ||
                           msgType == "req:rl:taste-goal:set" || msgType == "req:rl:dose-target:set" ||
                           msgType == "req:rl:recipe-domain:set" || msgType == "req:rl:cpbo-config:set" ||
                           msgType == "req:rl:local-reset") {
                    handleRLRequest(client->id(), doc);
                } else if (msgType == "req:rl:recommendation:use") {
                    if (rlParticipationEnabled(controller)) {
                        String recommendationId = doc["recommendation_id"].as<String>();
                        if (!recommendationId.isEmpty()) {
                            Event event;
                            event.id = "rl:recommendation:apply";
                            event.setString("recommendation_id", recommendationId);
                            pluginManager->trigger(event);
                            if (event.getInt("decision_persisted") == 1) {
                                _pendRecJson = ""; // Resolved; drop the reopen affordance.
                            }
                        }
                    }
                } else if (msgType == "req:rl:recommendation:ignore") {
                    if (rlParticipationEnabled(controller)) {
                        String recommendationId = doc["recommendation_id"].as<String>();
                        if (!recommendationId.isEmpty()) {
                            Event event;
                            event.id = "rl:recommendation:ignore";
                            event.setString("recommendation_id", recommendationId);
                            pluginManager->trigger(event);
                            if (event.getInt("decision_persisted") == 1) {
                                _pendRecJson = ""; // Resolved; drop the reopen affordance.
                            }
                        }
                    }
                } else if (msgType == "req:rl:shot:correction") {
                    JsonDocument resp;
                    resp["tp"] = "res:rl:shot:correction";
                    resp["rid"] = doc["rid"];
                    if (!rlParticipationEnabled(controller)) {
                        resp["error"] = F("Auto Tuning is disabled");
                    } else {
                        String shotId = doc["shot_id"].as<String>();
                        if (shotId.isEmpty()) {
                            shotId = rlLastShotId;
                        }
                        if (shotId.isEmpty()) {
                            resp["error"] = F("No shot available to correct");
                        } else {
                            AutoTuning::ShotCorrection correction;
                            correction.shotId = shotId.c_str();
                            correction.source = "gaggimate_webui";
                            if (doc["exclude_from_local_optimization"].is<bool>()) {
                                correction.excludeFromLocalOptimization = doc["exclude_from_local_optimization"].as<bool>();
                            }
                            if (doc["shot_type"].is<String>()) {
                                correction.shotType = doc["shot_type"].as<String>().c_str();
                            }
                            if (doc["grind_followed"].is<bool>()) {
                                correction.grindFollowed = doc["grind_followed"].as<bool>();
                            }
                            if (doc["dose_followed"].is<bool>()) {
                                correction.doseFollowed = doc["dose_followed"].as<bool>();
                            }
                            if (doc["yield_followed"].is<bool>()) {
                                correction.yieldFollowed = doc["yield_followed"].as<bool>();
                            }
                            if (doc["correction_tags"].is<JsonArray>()) {
                                for (JsonVariant tag : doc["correction_tags"].as<JsonArray>()) {
                                    String value = tag.as<String>();
                                    value.trim();
                                    if (!value.isEmpty() && value.length() <= 64 && correction.tags.size() < 16) {
                                        correction.tags.emplace_back(value.c_str());
                                    }
                                }
                            }
                            Event event;
                            event.id = "rl:shot:correction";
                            event.setString("shot_id", shotId);
                            event.setPayload(correction);
                            pluginManager->trigger(event);
                            resp["success"] = event.getInt("optimizer_persisted") == 1;
                            if (event.getInt("optimizer_persisted") != 1) {
                                resp["error"] = "Unable to persist optimizer correction";
                            }
                            resp["shot_id"] = shotId;
                        }
                    }
                    String msg;
                    serializeJson(resp, msg);
                    client->text(msg);
                } else if (msgType == "req:rl:dose-confirmation") {
                    const String shotId = doc["shot_id"].as<String>();
                    if (rlParticipationEnabled(controller) && !_pendingDoseShotId.isEmpty() && shotId == _pendingDoseShotId &&
                        doc["followed"].is<bool>()) {
                        Event event;
                        event.id = "rl:dose-confirmation";
                        event.setString("shot_id", shotId);
                        event.setInt("has_followed", 1);
                        event.setInt("followed", doc["followed"].as<bool>() ? 1 : 0);
                        pluginManager->trigger(event);
                    }
                } else if (msgType == "req:rl:preference") {
                    if (rlParticipationEnabled(controller) && !_pendingPreferenceShotId.isEmpty()) {
                        const String installId = doc["install_id"].as<String>();
                        const String runId = doc["optimization_run_id"].as<String>();
                        const String newShotId = doc["new_shot_id"].as<String>();
                        const String anchorShotId = doc["anchor_shot_id"].as<String>();
                        const String comparisonMode = doc["comparison_mode"].as<String>();
                        const String label = doc["label"].as<String>();
                        const bool validLabel = label == "new_better" || label == "anchor_better" || label == "tie";
                        const bool matchesPending = installId == _pendPreferenceInstallId && runId == _pendPreferenceRunId &&
                                                    newShotId == _pendingPreferenceShotId &&
                                                    anchorShotId == _pendPreferenceAnchorShotId &&
                                                    comparisonMode == _pendPreferenceComparisonMode;
                        if (validLabel && matchesPending && newShotId != anchorShotId) {
                            const auto parsedLabel = AutoTuning::preferenceLabelFromKey(label.c_str());
                            const auto parsedMode = AutoTuning::comparisonModeFromKey(comparisonMode.c_str());
                            if (!parsedLabel || !parsedMode) {
                                return;
                            }
                            AutoTuning::PreferenceFeedback feedback;
                            feedback.installId = installId.c_str();
                            feedback.optimizationRunId = runId.c_str();
                            feedback.newShotId = newShotId.c_str();
                            feedback.anchorShotId = anchorShotId.c_str();
                            feedback.label = *parsedLabel;
                            feedback.comparisonMode = *parsedMode;
                            feedback.tasteGoal = _pendPreferenceTasteGoal;
                            feedback.recommendationId = _pendingPreferenceRecommendationId.c_str();
                            Event event;
                            event.id = "rl:preference";
                            event.setString("install_id", installId);
                            event.setString("optimization_run_id", runId);
                            event.setString("new_shot_id", newShotId);
                            event.setString("anchor_shot_id", anchorShotId);
                            event.setString("label", label);
                            event.setString("comparison_mode", comparisonMode);
                            event.setString("recommendation_id", _pendingPreferenceRecommendationId);
                            event.setPayload(feedback);
                            pluginManager->trigger(event);
                        }
                    }
                } else if (msgType == "req:process:activate") {
                    controller->postCommand(CtrlCmd::ACTIVATE);
                } else if (msgType == "req:process:deactivate") {
                    controller->postCommand(CtrlCmd::DEACTIVATE_CLEAR);
                } else if (msgType == "req:process:clear") {
                    controller->postCommand(CtrlCmd::CLEAR);
                } else if (msgType == "req:grind:activate") {
                    controller->postCommand(CtrlCmd::ACTIVATE_GRIND);
                } else if (msgType == "req:grind:deactivate") {
                    controller->postCommand(CtrlCmd::DEACTIVATE_GRIND);
                } else if (msgType == "req:change-grind-target") {
                    if (doc["target"].is<uint8_t>()) {
                        auto target = doc["target"].as<uint8_t>();
                        controller->getSettings().setVolumetricTarget(target);
                    }
                } else if (msgType == "req:raise-temp") {
                    controller->postCommand(CtrlCmd::RAISE_TEMP);
                } else if (msgType == "req:lower-temp") {
                    controller->postCommand(CtrlCmd::LOWER_TEMP);
                } else if (msgType == "req:raise-grind-target") {
                    controller->postCommand(CtrlCmd::RAISE_GRIND_TARGET);
                } else if (msgType == "req:lower-grind-target") {
                    controller->postCommand(CtrlCmd::LOWER_GRIND_TARGET);
                } else if (msgType == "req:raise-brew-target") {
                    controller->raiseBrewTarget();
                } else if (msgType == "req:lower-brew-target") {
                    controller->lowerBrewTarget();
                } else if (msgType == "req:change-mode") {
                    if (doc["mode"].is<uint8_t>()) {
                        auto mode = doc["mode"].as<uint8_t>();
                        controller->postCommand(CtrlCmd::CHANGE_MODE, mode);
                    }
                } else if (msgType == "req:change-brew-target") {
                    if (doc["target"].is<uint8_t>()) {
                        auto target = doc["target"].as<uint8_t>();
                        controller->getSettings().setVolumetricTarget(target);
                    }
                } else if (msgType == "req:history:rebuild") {
                    // Handle rebuild asynchronously - send immediate ack, progress comes via events
                    JsonDocument resp(&psramAllocator);
                    resp["tp"] = "res:history:rebuild";
                    if (doc["rid"].is<const char *>()) {
                        resp["rid"] = doc["rid"];
                    }
                    resp["msg"] = "Rebuild started";
                    client->text(toWsBuffer(resp));
                    ShotHistory.startAsyncRebuild();
                } else if (msgType.startsWith("req:history")) {
                    JsonDocument resp(&psramAllocator);
                    ShotHistory.handleRequest(doc, resp);
                    client->text(toWsBuffer(resp));
                } else if (msgType == "req:flush:start") {
                    handleFlushStart(client->id(), doc);
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
                } else if (msgType == "req:scale:tare") {
                    controller->scaleTare();
                } else if (msgType == "req:scale:cal:start") {
                    uint8_t channel = doc["channel"] | 0;
                    float refWeight = doc["refWeight"] | 0.0f;
                    if ((channel == 1 || channel == 2) && refWeight > 0.0f) {
                        controller->getClientController()->startScaleCalibration(channel, refWeight);
                    }
#endif
                }
            }
        }
        // Done with this message
        rxBuffers.erase(cid);
        rxBufferLastActivity.erase(cid);
    }
}

void WebUIPlugin::handleOTASettings(uint32_t clientId, JsonDocument &request) {
    if (request["update"].as<bool>()) {
        if (!request["channel"].isNull()) {
            controller->getSettings().setOTAChannel(request["channel"].as<String>() == "latest" ? "latest" : "nightly");
            ota->setReleaseUrl(otaReleaseUrlForChannel(controller->getSettings().getOTAChannel()));
            lastUpdateCheck = 0;
        }
    }
    updateOTAStatus("Checking...");
}

void WebUIPlugin::handleOTAStart(uint32_t clientId, JsonDocument &request) {
    if (!OTA_ENABLED) {
        return;
    }

    String component = request["cp"].is<String>() ? request["cp"].as<String>() : "";
    if (component == "controller" && !CONTROLLER_OTA_ENABLED) {
        updateOTAStatus("Controller OTA unavailable");
        return;
    }
    if (component != "display" && component != "controller") {
        component = "";
    }
    updateComponent = component;
    updating = true;
    updateOTAStatus("Updating...");
    updateOTAProgress(component == "controller" ? PHASE_CONTROLLER_FW : PHASE_DISPLAY_FW, 0);
}

void WebUIPlugin::handleAutotuneStart(uint32_t clientId, JsonDocument &request) {
    int testTime = request["time"].as<int>();
    int samples = request["samples"].as<int>();
    // Heater wattage drives combinedKff = TUNER_OUTPUT_SPAN / wattage on the
    // controller. 0 = "skip combinedKff derivation" â€” happens when older Web
    // UI builds omit the field. WebUI form default is 680 W (Gaggia Classic
    // Pro 2019 / E24, 230 V boiler).
    int heaterWattage = request["wattage"] | 0;
    controller->autotune(testTime, samples, heaterWattage);
}

void WebUIPlugin::handleProfileRequest(uint32_t clientId, JsonDocument &request) {
    // Allocate the response node pool from PSRAM â€” list responses can be tens
    // of KB and would otherwise fragment the ~300 KB internal heap.
    JsonDocument response(&psramAllocator);
    auto type = request["tp"].as<String>();
    ESP_LOGI("WebUIPlugin", "Handling request: %s", type.c_str());
    response["tp"] = String("res:") + type.substring(4);
    response["rid"] = request["rid"].as<String>();

    if (type == "req:profiles:list") {
        auto arr = response["profiles"].to<JsonArray>();
        for (auto const &id : profileManager->listProfiles()) {
            Profile profile{};
            // Skip entries whose JSON couldn't be opened or failed validation
            // (parseProfile returns false for missing label/type/phases). Without
            // this, corrupt or partial profile files surface as blank cards in
            // the UI â€” the user reported "blank Simple cards" originating here.
            if (!profileManager->loadProfile(id, profile)) {
                ESP_LOGW("WebUIPlugin", "Skipping unreadable profile %s in list response", id.c_str());
                continue;
            }
            auto p = arr.add<JsonObject>();
            if (request["minimal"].as<bool>()) {
                p["id"] = profile.id;
                p["label"] = profile.label;
            } else {
                writeProfile(p, profile);
            }
        }
    } else if (type == "req:profiles:load") {
        auto id = request["id"].as<String>();
        Profile profile;
        if (profileManager->loadProfile(id, profile)) {
            auto obj = response["profile"].to<JsonObject>();
            writeProfile(obj, profile);
        } else {
            response["error"] = F("Profile not found");
        }
    } else if (type == "req:profiles:save") {
        auto obj = request["profile"].as<JsonObject>();
        Profile profile;
        parseProfile(obj, profile);
        if (!profileManager->saveProfile(profile)) {
            response["error"] = F("Save failed");
        }
        auto respObj = response["profile"].to<JsonObject>();
        writeProfile(respObj, profile);
    } else if (type == "req:profiles:delete") {
        auto id = request["id"].as<String>();
        if (!profileManager->deleteProfile(id)) {
            response["error"] = F("Delete failed");
        }
    } else if (type == "req:profiles:select") {
        auto id = request["id"].as<String>();
        profileManager->selectProfile(id);
    } else if (type == "req:profiles:favorite") {
        auto id = request["id"].as<String>();
        profileManager->addFavoritedProfile(id);
    } else if (type == "req:profiles:unfavorite") {
        auto id = request["id"].as<String>();
        profileManager->removeFavoritedProfile(id);
    } else if (type == "req:profiles:reorder") {
        // Expect an array of profile IDs in desired order
        if (request["order"].is<JsonArray>()) {
            std::vector<String> order;
            for (JsonVariant v : request["order"].as<JsonArray>()) {
                if (v.is<String>()) {
                    String id = v.as<String>();
                    if (!id.isEmpty() && std::find(order.begin(), order.end(), id) == order.end()) {
                        order.emplace_back(std::move(id));
                    }
                }
            }
            controller->getSettings().setProfileOrder(order);
        }
    }

    ws.text(clientId, toWsBuffer(response));
}

void WebUIPlugin::handleRLRequest(uint32_t clientId, JsonDocument &request) {
    JsonDocument response;
    auto type = request["tp"].as<String>();
    response["tp"] = String("res:") + type.substring(4);
    response["rid"] = request["rid"].as<String>();

    Settings &settings = controller->getSettings();
    if (!AutoTuning::Router(settings.getRLOptimizerConfiguration()).acceptUserCommands()) {
        response["error"] = F("Auto Tuning is disabled");
    } else {
        bool changed = false;
        bool tasteGoalChanged = false;
        bool optimizerConfigurationChanged = false;
        JsonDocument contextsDoc;
        JsonArray contexts = loadRLContexts(contextsDoc, settings.getRLBeanContextsJson());
        JsonDocument grinderContextsDoc;
        JsonArray grinderContexts = loadRLContexts(grinderContextsDoc, settings.getRLGrinderContextsJson());

        if (type == "req:rl:status:refresh") {
            pluginManager->trigger("rl:status:refresh");
        } else if (type == "req:rl:context:start-bean") {
            String name = limitedRLString(request["name"], 80);
            name.trim();
            if (name.isEmpty()) {
                name = defaultRLContextName();
            }
            JsonObject current = findRLContext(contexts, settings.getRLBeanContextId());
            if (!current.isNull()) {
                current["status"] = "retired";
                current["retired_at"] = EpochTime::now();
            }
            const int bagIndex = currentBagIndex(contexts, name) + 1;
            const String id = makeRLContextId("bean", name);
            markRLOtherContextsAvailable(contexts, id);
            addRLContext(contexts, settings, id, name, bagIndex, "active");
            persistRLContexts(settings, contextsDoc);
            settings.setRLBeanContextId(id);
            settings.setRLBeanContextName(name);
            settings.setRLOptimizationPaused(false);
            settings.setRLLocalOptimizationEnabled(true);
            settings.save(true);
            changed = true;
        } else if (type == "req:rl:context:start-bag") {
            String name = settings.getRLBeanContextName();
            name.trim();
            if (name.length() > 80) {
                name.remove(80);
            }
            if (name.isEmpty()) {
                name = defaultRLContextName();
            }
            JsonObject current = findRLContext(contexts, settings.getRLBeanContextId());
            if (!current.isNull()) {
                current["status"] = "retired";
                current["retired_at"] = EpochTime::now();
            }
            const int bagIndex = currentBagIndex(contexts, name) + 1;
            const String id = makeRLContextId("bean", name);
            markRLOtherContextsAvailable(contexts, id);
            addRLContext(contexts, settings, id, name, bagIndex, "active");
            persistRLContexts(settings, contextsDoc);
            settings.setRLBeanContextId(id);
            settings.setRLBeanContextName(name);
            settings.setRLOptimizationPaused(false);
            settings.setRLLocalOptimizationEnabled(true);
            settings.save(true);
            changed = true;
        } else if (type == "req:rl:context:switch") {
            const String id = request["id"].as<String>();
            JsonObject context = findRLContext(contexts, id);
            if (context.isNull()) {
                response["error"] = F("Context not found");
            } else {
                markRLOtherContextsAvailable(contexts, id);
                context["status"] = "active";
                settings.setRLBeanContextId(id);
                settings.setRLBeanContextName(limitedRLString(context["name"], 80));
                persistRLContexts(settings, contextsDoc);
                settings.save(true);
                changed = true;
            }
        } else if (type == "req:rl:context:update") {
            String id = request["id"].as<String>();
            if (id.isEmpty()) {
                id = settings.getRLBeanContextId();
            }
            JsonObject context = findRLContext(contexts, id);
            String name = limitedRLString(request["name"], 80);
            name.trim();
            if (context.isNull()) {
                response["error"] = F("Context not found");
            } else if (name.isEmpty()) {
                response["error"] = F("Bean name is required");
            } else {
                context["name"] = name;
                if (settings.getRLBeanContextId() == id) {
                    settings.setRLBeanContextName(name);
                }
                persistRLContexts(settings, contextsDoc);
                settings.save(true);
                changed = true;
            }
        } else if (type == "req:rl:context:retire") {
            String id = request["id"].as<String>();
            if (id.isEmpty()) {
                id = settings.getRLBeanContextId();
            }
            JsonObject context = findRLContext(contexts, id);
            if (!context.isNull()) {
                context["status"] = "retired";
                context["retired_at"] = EpochTime::now();
                persistRLContexts(settings, contextsDoc);
            }
            if (settings.getRLBeanContextId() == id) {
                settings.setRLBeanContextId("");
                settings.setRLBeanContextName("");
                settings.setRLLocalOptimizationEnabled(false);
            }
            settings.save(true);
            changed = true;
        } else if (type == "req:rl:context:delete") {
            String id = request["id"].as<String>();
            if (id.isEmpty()) {
                id = settings.getRLBeanContextId();
            }
            if (!removeRLContext(contexts, id)) {
                response["error"] = F("Context not found");
            } else {
                AutoTuning::removeTasteGoalsForBeanContext(settings, id);
                persistRLContexts(settings, contextsDoc);
                if (settings.getRLBeanContextId() == id) {
                    settings.setRLBeanContextId("");
                    settings.setRLBeanContextName("");
                    settings.setRLLocalOptimizationEnabled(false);
                }
                settings.save(true);
                changed = true;
            }
        } else if (type == "req:rl:grinder-context:create") {
            String name = limitedRLString(request["name"], 80);
            name.trim();
            if (name.isEmpty()) {
                name = defaultRLGrinderContextName();
            }
            const String id = makeRLContextId("grinder", name);
            markRLOtherContextsAvailable(grinderContexts, id);
            JsonObject context = addRLGrinderContext(grinderContexts, settings, id, name, "active");
            applyRLGrinderCalibration(context, request);
            persistRLGrinderContexts(settings, grinderContextsDoc);
            settings.setRLGrinderContextId(id);
            settings.setRLGrinderContextName(name);
            settings.save(true);
            changed = true;
        } else if (type == "req:rl:grinder-context:update") {
            String id = request["id"].as<String>();
            if (id.isEmpty()) {
                id = settings.getRLGrinderContextId();
            }
            JsonObject context = findRLContext(grinderContexts, id);
            String name = limitedRLString(request["name"], 80);
            name.trim();
            if (context.isNull()) {
                response["error"] = F("Grinder context not found");
            } else if (name.isEmpty()) {
                response["error"] = F("Grinder name is required");
            } else {
                context["name"] = name;
                applyRLGrinderCalibration(context, request);
                if (settings.getRLGrinderContextId() == id) {
                    settings.setRLGrinderContextName(name);
                }
                persistRLGrinderContexts(settings, grinderContextsDoc);
                settings.save(true);
                changed = true;
            }
        } else if (type == "req:rl:grinder-context:update-calibration") {
            String id = request["id"].as<String>();
            if (id.isEmpty()) {
                id = settings.getRLGrinderContextId();
            }
            JsonObject context = findRLContext(grinderContexts, id);
            if (context.isNull()) {
                response["error"] = F("Grinder context not found");
            } else {
                applyRLGrinderCalibration(context, request);
                persistRLGrinderContexts(settings, grinderContextsDoc);
                settings.save(true);
                changed = true;
            }
        } else if (type == "req:rl:grinder-context:switch") {
            const String id = request["id"].as<String>();
            JsonObject context = findRLContext(grinderContexts, id);
            if (context.isNull()) {
                response["error"] = F("Grinder context not found");
            } else {
                markRLOtherContextsAvailable(grinderContexts, id);
                context["status"] = "active";
                settings.setRLGrinderContextId(id);
                settings.setRLGrinderContextName(limitedRLString(context["name"], 80));
                persistRLGrinderContexts(settings, grinderContextsDoc);
                settings.save(true);
                changed = true;
            }
        } else if (type == "req:rl:grinder-context:retire") {
            String id = request["id"].as<String>();
            if (id.isEmpty()) {
                id = settings.getRLGrinderContextId();
            }
            JsonObject context = findRLContext(grinderContexts, id);
            if (!context.isNull()) {
                context["status"] = "retired";
                context["retired_at"] = EpochTime::now();
                persistRLGrinderContexts(settings, grinderContextsDoc);
            }
            if (settings.getRLGrinderContextId() == id) {
                settings.setRLGrinderContextId("");
                settings.setRLGrinderContextName("");
            }
            settings.save(true);
            changed = true;
        } else if (type == "req:rl:grinder-context:delete") {
            String id = request["id"].as<String>();
            if (id.isEmpty()) {
                id = settings.getRLGrinderContextId();
            }
            if (!removeRLContext(grinderContexts, id)) {
                response["error"] = F("Grinder context not found");
            } else {
                AutoTuning::removeTasteGoalsForGrinderContext(settings, id);
                persistRLGrinderContexts(settings, grinderContextsDoc);
                if (settings.getRLGrinderContextId() == id) {
                    settings.setRLGrinderContextId("");
                    settings.setRLGrinderContextName("");
                }
                settings.save(true);
                changed = true;
            }
        } else if (type == "req:rl:grinder-context:clear") {
            JsonObject current = findRLContext(grinderContexts, settings.getRLGrinderContextId());
            if (!current.isNull() && current["status"].as<String>() == "active") {
                current["status"] = "available";
                persistRLGrinderContexts(settings, grinderContextsDoc);
            }
            settings.setRLGrinderContextId("");
            settings.setRLGrinderContextName("");
            settings.save(true);
            changed = true;
        } else if (type == "req:rl:local-optimization") {
            settings.setRLLocalOptimizationEnabled(request["enabled"] | true);
            settings.save(true);
            changed = true;
        } else if (type == "req:rl:taste-goal:set") {
            String goalError;
            if (!AutoTuning::setTasteGoalForContext(settings, settings.getRLBeanContextId(), settings.getRLGrinderContextId(),
                                                    request["taste_goal"], goalError)) {
                response["error"] = goalError;
            } else {
                changed = true;
                tasteGoalChanged = true;
            }
        } else if (type == "req:rl:recipe-domain:set") {
            JsonVariant value = request["recipe_domain"];
            if (!value.is<JsonObject>() || value.as<JsonObjectConst>().size() != 5 || !isRLNumber(value["grind_radius_steps"]) ||
                !isRLNumber(value["dose_min_g"]) || !isRLNumber(value["dose_max_g"]) ||
                !isRLNumber(value["target_output_min_g"]) || !isRLNumber(value["target_output_max_g"])) {
                response["error"] = F("Recipe domain must contain five numeric fields");
            } else {
                AutoTuning::RecipeDomain domain{value["grind_radius_steps"].as<float>(), value["dose_min_g"].as<float>(),
                                                value["dose_max_g"].as<float>(), value["target_output_min_g"].as<float>(),
                                                value["target_output_max_g"].as<float>()};
                std::string reason;
                const float currentDose = settings.getTargetGrindVolume();
                if (!AutoTuning::validateRecipeDomain(domain, reason)) {
                    response["error"] = reason.c_str();
                } else if (currentDose < domain.doseMinG || currentDose > domain.doseMaxG) {
                    response["error"] = F("Recipe domain must include the current dose target");
                } else {
                    const bool configurationChanged = !sameRLRecipeDomain(settings.getRLRecipeDomain(), domain);
                    if (!settings.setRLRecipeDomain(domain)) {
                        response["error"] = F("Unable to save recipe domain");
                    } else if (configurationChanged) {
                        settings.save(true);
                        changed = true;
                        optimizerConfigurationChanged = true;
                    }
                }
            }
        } else if (type == "req:rl:cpbo-config:set") {
            const String profileName = request["cpbo_profile_name"].as<String>();
            const String comparisonMode = request["cpbo_comparison_mode"].as<String>();
            JsonVariant value = request["recipe_domain"];
            if (profileName != "application" && profileName != "paper_fidelity") {
                response["error"] = F("Unsupported CPBO compute profile");
            } else if (comparisonMode != "best_incumbent" && comparisonMode != "global_previous") {
                response["error"] = F("Unsupported CPBO comparison policy");
            } else if (!value.is<JsonObject>() || value.as<JsonObjectConst>().size() != 5 ||
                       !isRLNumber(value["grind_radius_steps"]) || !isRLNumber(value["dose_min_g"]) ||
                       !isRLNumber(value["dose_max_g"]) || !isRLNumber(value["target_output_min_g"]) ||
                       !isRLNumber(value["target_output_max_g"])) {
                response["error"] = F("Recipe domain must contain five numeric fields");
            } else {
                AutoTuning::RecipeDomain domain{value["grind_radius_steps"].as<float>(), value["dose_min_g"].as<float>(),
                                                value["dose_max_g"].as<float>(), value["target_output_min_g"].as<float>(),
                                                value["target_output_max_g"].as<float>()};
                std::string reason;
                const float currentDose = settings.getTargetGrindVolume();
                if (!AutoTuning::validateRecipeDomain(domain, reason)) {
                    response["error"] = reason.c_str();
                } else if (currentDose < domain.doseMinG || currentDose > domain.doseMaxG) {
                    response["error"] = F("Recipe domain must include the current dose target");
                } else {
                    const bool configurationChanged = !sameRLRecipeDomain(settings.getRLRecipeDomain(), domain) ||
                                                      settings.getRLCPBOProfileName() != profileName ||
                                                      settings.getRLCPBOComparisonMode() != comparisonMode;
                    if (!settings.setRLRecipeDomain(domain)) {
                        response["error"] = F("Unable to save recipe domain");
                    } else {
                        settings.setRLCPBOProfileName(profileName);
                        settings.setRLCPBOComparisonMode(comparisonMode);
                        if (configurationChanged) {
                            settings.save(true);
                            changed = true;
                            optimizerConfigurationChanged = true;
                        }
                    }
                }
            }
        } else if (type == "req:rl:dose-target:set") {
            JsonVariant value = request["dose_target_g"];
            const AutoTuning::RecipeDomain domain = settings.getRLRecipeDomain();
            if (!isRLNumber(value) || !std::isfinite(value.as<float>())) {
                response["error"] = F("Dose target must be numeric and finite");
            } else if (value.as<float>() < domain.doseMinG || value.as<float>() > domain.doseMaxG) {
                response["error"] = F("Dose target is outside the configured recipe domain");
            } else {
                controller->setTargetGrindVolume(value.as<float>());
                settings.save(true);
                changed = true;
            }
        } else if (type == "req:rl:optimization:pause") {
            settings.setRLOptimizationPaused(true);
            settings.save(true);
            changed = true;
        } else if (type == "req:rl:optimization:resume") {
            settings.setRLOptimizationPaused(false);
            settings.setRLLocalOptimizationEnabled(true);
            settings.save(true);
            changed = true;
        } else if (type == "req:rl:local-reset") {
            settings.setRLBeanContextId("");
            settings.setRLBeanContextName("");
            settings.setRLBeanContextsJson("[]");
            settings.setRLGrinderContextId("");
            settings.setRLGrinderContextName("");
            settings.setRLGrinderContextsJson("[]");
            AutoTuning::clearTasteGoals(settings);
            settings.setRLLocalOptimizationEnabled(false);
            settings.setRLOptimizationPaused(false);
            settings.save(true);
            Event event;
            event.id = "rl:local:reset";
            const bool dryRun = request["dry_run"] | false;
            event.setInt("dry_run", dryRun ? 1 : 0);
            pluginManager->trigger(event);
            contextsDoc.clear();
            grinderContextsDoc.clear();
            contexts = loadRLContexts(contextsDoc, settings.getRLBeanContextsJson());
            grinderContexts = loadRLContexts(grinderContextsDoc, settings.getRLGrinderContextsJson());
            changed = true;
        }

        if (changed) {
            pluginManager->trigger("settings:changed");
            Event settingsChanged;
            settingsChanged.id = "rl:settings:changed";
            settingsChanged.setInt("optimizer_configuration_changed", optimizerConfigurationChanged ? 1 : 0);
            pluginManager->trigger(settingsChanged);
            if (tasteGoalChanged) {
                pluginManager->trigger("rl:taste-goal:changed");
            }
        }

        JsonArray arr = response["contexts"].to<JsonArray>();
        for (JsonObject context : contexts) {
            JsonObject out = arr.add<JsonObject>();
            out["id"] = context["id"].as<String>();
            out["name"] = context["name"].as<String>();
            out["bag_index"] = context["bag_index"] | 1;
            out["status"] = context["status"].as<String>();
            out["active"] = context["id"].as<String>() == settings.getRLBeanContextId();
        }
        response["active_context_id"] = settings.getRLBeanContextId();
        response["active_context_name"] = settings.getRLBeanContextName();
        JsonArray grinderArr = response["grinder_contexts"].to<JsonArray>();
        for (JsonObject context : grinderContexts) {
            JsonObject out = grinderArr.add<JsonObject>();
            out["id"] = context["id"].as<String>();
            out["name"] = context["name"].as<String>();
            out["status"] = context["status"].as<String>();
            out["active"] = context["id"].as<String>() == settings.getRLGrinderContextId();
            copyRLGrinderCalibration(out, context);
        }
        response["active_grinder_context_id"] = settings.getRLGrinderContextId();
        response["active_grinder_context_name"] = settings.getRLGrinderContextName();
        response["optimizer_mode"] = settings.getRLOptimizerMode();
        response["cpbo_profile_name"] = settings.getRLCPBOProfileName();
        response["cpbo_comparison_mode"] = settings.getRLCPBOComparisonMode();
        response["dose_target_g"] = settings.getTargetGrindVolume();
        JsonObject responseDomain = response["recipe_domain"].to<JsonObject>();
        AutoTuning::RecipeDomain const recipeDomain = settings.getRLRecipeDomain();
        responseDomain["grind_radius_steps"] = recipeDomain.grindRadiusSteps;
        responseDomain["dose_min_g"] = recipeDomain.doseMinG;
        responseDomain["dose_max_g"] = recipeDomain.doseMaxG;
        responseDomain["target_output_min_g"] = recipeDomain.targetOutputMinG;
        responseDomain["target_output_max_g"] = recipeDomain.targetOutputMaxG;
        JsonDocument activeTasteGoal;
        AutoTuning::activeTasteGoal(settings, activeTasteGoal);
        response["taste_goal"].set(activeTasteGoal.as<JsonVariantConst>());
        response["taste_goal_summary"] = AutoTuning::tasteGoalSummary(activeTasteGoal.as<JsonVariantConst>());
        response["local_optimization_enabled"] = settings.isRLLocalOptimizationEnabled() && !settings.isRLOptimizationPaused() &&
                                                 !settings.getRLBeanContextId().isEmpty();
        response["optimization_paused"] = settings.isRLOptimizationPaused();
    }

    size_t bufferSize = measureJson(response);
    auto *buffer = ws.makeBuffer(bufferSize);
    serializeJson(response, buffer->get(), bufferSize);
    ws.text(clientId, buffer);
}

void WebUIPlugin::handleSettings(AsyncWebServerRequest *request) const {
    if (request->method() == HTTP_POST) {
        controller->getSettings().batchUpdate([request](Settings *settings) {
            if (request->hasArg("startupMode"))
                settings->setStartupMode(request->arg("startupMode") == "brew" ? MODE_BREW : MODE_STANDBY);
            if (request->hasArg("startupProfile"))
                settings->setStartupProfile(request->arg("startupProfile"));
            if (request->hasArg("targetSteamTemp"))
                settings->setTargetSteamTemp(request->arg("targetSteamTemp").toInt());
            if (request->hasArg("targetWaterTemp"))
                settings->setTargetWaterTemp(request->arg("targetWaterTemp").toInt());
            if (request->hasArg("temperatureOffset"))
                settings->setTemperatureOffset(request->arg("temperatureOffset").toInt());
            if (request->hasArg("pressureScaling"))
                settings->setPressureScaling(request->arg("pressureScaling").toFloat());
            if (request->hasArg("pid"))
                settings->setPid(request->arg("pid"));
            if (request->hasArg("pumpModelCoeffs"))
                settings->setPumpModelCoeffs(request->arg("pumpModelCoeffs"));
            if (request->hasArg("pumpSlipCoeffs"))
                settings->setPumpSlipCoeffs(request->arg("pumpSlipCoeffs"));
            if (request->hasArg("wifiSsid"))
                settings->setWifiSsid(request->arg("wifiSsid"));
            if (request->hasArg("mdnsName"))
                settings->setMdnsName(request->arg("mdnsName"));
            if (request->hasArg("wifiPassword") && request->arg("wifiPassword") != "---unchanged---")
                settings->setWifiPassword(request->arg("wifiPassword"));
            if (request->hasArg("apPassword") && request->arg("apPassword").length() >= WIFI_AP_PASSWORD_MIN_LENGTH)
                settings->setWifiApPassword(request->arg("apPassword"));
            settings->setHomekit(request->hasArg("homekit"));
            settings->setBoilerFillActive(request->hasArg("boilerFillActive"));
            if (request->hasArg("startupFillTime"))
                settings->setStartupFillTime(request->arg("startupFillTime").toInt() * 1000);
            if (request->hasArg("steamFillTime"))
                settings->setSteamFillTime(request->arg("steamFillTime").toInt() * 1000);
            settings->setSmartGrindActive(request->hasArg("smartGrindActive"));
            if (request->hasArg("smartGrindIp"))
                settings->setSmartGrindIp(request->arg("smartGrindIp"));
            if (request->hasArg("smartGrindMode"))
                settings->setSmartGrindMode(request->arg("smartGrindMode").toInt());
            const bool homeAssistantEnabled = request->hasArg("homeAssistant");
            settings->setHomeAssistant(homeAssistantEnabled);
            String rlProviderMode = request->hasArg("rlProviderMode")
                                        ? request->arg("rlProviderMode")
                                        : (request->hasArg("rlAutoTuningEnabled") ? String(AutoTuning::PROVIDER_OFF_BOARD)
                                                                                  : String(AutoTuning::PROVIDER_DISABLED));
            rlProviderMode = AutoTuning::normalizeProviderMode(rlProviderMode.c_str()).c_str();
            const bool rlEnabled = rlProviderMode != AutoTuning::PROVIDER_DISABLED;
            settings->setRLAutoTuningProviderMode(rlProviderMode);
            settings->setRLAutoTuningEnabled(rlEnabled);
            settings->setRLCommunityUploadEnabled(request->hasArg("rlCommunityUploadEnabled"));
            settings->setRLCommunityUploadPrompted(request->arg("rlCommunityUploadPrompted") == "1");
            if (request->hasArg("rlUploadBaseUrl"))
                settings->setRLUploadBaseUrl(request->arg("rlUploadBaseUrl"));
            if (!rlEnabled) {
                settings->setRLLocalOptimizationEnabled(false);
            }
            if (request->hasArg("haUser"))
                settings->setHomeAssistantUser(request->arg("haUser"));
            if (request->hasArg("haPassword"))
                settings->setHomeAssistantPassword(request->arg("haPassword"));
            if (request->hasArg("haIP"))
                settings->setHomeAssistantIP(request->arg("haIP"));
            if (request->hasArg("haPort"))
                settings->setHomeAssistantPort(request->arg("haPort").toInt());
            if (request->hasArg("haTopic"))
                settings->setHomeAssistantTopic(request->arg("haTopic"));
            settings->setMomentaryButtons(request->hasArg("momentaryButtons"));
            settings->setDelayAdjust(request->hasArg("delayAdjust"));
            if (request->hasArg("brewDelay"))
                settings->setBrewDelay(request->arg("brewDelay").toDouble());
            if (request->hasArg("hardwareBrewDelay"))
                settings->setHardwareBrewDelay(request->arg("hardwareBrewDelay").toDouble());
            if (request->hasArg("grindDelay"))
                settings->setGrindDelay(request->arg("grindDelay").toDouble());
            if (request->hasArg("timezone"))
                settings->setTimezone(request->arg("timezone"));
            settings->setClockFormat(request->hasArg("clock24hFormat"));
            if (request->hasArg("standbyTimeout"))
                settings->setStandbyTimeout(request->arg("standbyTimeout").toInt() * 1000);
            if (request->hasArg("mainBrightness"))
                settings->setMainBrightness(request->arg("mainBrightness").toInt());
            if (request->hasArg("standbyBrightness"))
                settings->setStandbyBrightness(request->arg("standbyBrightness").toInt());
            if (request->hasArg("standbyBrightnessTimeout"))
                settings->setStandbyBrightnessTimeout(request->arg("standbyBrightnessTimeout").toInt() * 1000);
            if (request->hasArg("steamPumpPercentage"))
                settings->setSteamPumpPercentage(request->arg("steamPumpPercentage").toFloat());
            if (request->hasArg("steamPumpCutoff"))
                settings->setSteamPumpCutoff(request->arg("steamPumpCutoff").toFloat());
            if (request->hasArg("themeMode"))
                settings->setThemeMode(request->arg("themeMode").toInt());
            if (request->hasArg("sunriseIdle"))
                settings->setSunriseIdle(request->arg("sunriseIdle"));
            if (request->hasArg("sunriseActive"))
                settings->setSunriseActive(request->arg("sunriseActive"));
            if (request->hasArg("sunriseFinished"))
                settings->setSunriseFinished(request->arg("sunriseFinished"));
            if (request->hasArg("sunriseError"))
                settings->setSunriseError(request->arg("sunriseError"));
            if (request->hasArg("sunriseExtBrightness"))
                settings->setSunriseExtBrightness(request->arg("sunriseExtBrightness").toInt());
            if (request->hasArg("emptyTankDistance"))
                settings->setEmptyTankDistance(request->arg("emptyTankDistance").toInt());
            if (request->hasArg("fullTankDistance"))
                settings->setFullTankDistance(request->arg("fullTankDistance").toInt());
            if (request->hasArg("altRelayFunction"))
                settings->setAltRelayFunction(request->arg("altRelayFunction").toInt());
            if (request->hasArg("buttonBehavior"))
                settings->setButtonBehaviorList(explode(request->arg("buttonBehavior"), ','));
            if (request->hasArg("commutationGain"))
                settings->setCommutationGain(request->arg("commutationGain").toFloat());
            if (request->hasArg("convergenceGain"))
                settings->setConvergenceGain(request->arg("convergenceGain").toFloat());
            if (request->hasArg("integralGain"))
                settings->setIntegralGain(request->arg("integralGain").toFloat());
            if (request->hasArg("maxPumpPower"))
                settings->setMaxPumpPower(request->arg("maxPumpPower").toFloat());
            if (request->hasArg("savedScale"))
                settings->setSavedScale(request->arg("savedScale"));
            if (request->hasArg("savedBrewScale"))
                settings->setSavedBrewScale(request->arg("savedBrewScale"));
            if (request->hasArg("savedGrindScale"))
                settings->setSavedGrindScale(request->arg("savedGrindScale"));
            if (request->hasArg("scaleSource"))
                settings->setScaleSource(request->arg("scaleSource").toInt());
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
            if (request->hasArg("scaleCalibration1"))
                settings->setScaleCalibration1(request->arg("scaleCalibration1").toFloat());
            if (request->hasArg("scaleCalibration2"))
                settings->setScaleCalibration2(request->arg("scaleCalibration2").toFloat());
            if (request->hasArg("scaleOffset1"))
                settings->setScaleOffset1(request->arg("scaleOffset1").toInt());
            if (request->hasArg("scaleOffset2"))
                settings->setScaleOffset2(request->arg("scaleOffset2").toInt());
#endif
            settings->setAutoWakeupEnabled(request->hasArg("autowakeupEnabled"));
            if (request->hasArg("autowakeupSchedules")) {
                // Handle schedule format with days
                String schedulesStr = request->arg("autowakeupSchedules");
                std::vector<AutoWakeupSchedule> schedules;

                if (schedulesStr.length() > 0) {
                    // Split semicolon-separated schedules
                    int start = 0;
                    int end = schedulesStr.indexOf(';');

                    while (end != -1 || start < schedulesStr.length()) {
                        String scheduleStr = (end != -1) ? schedulesStr.substring(start, end) : schedulesStr.substring(start);

                        int pipePos = scheduleStr.indexOf('|');
                        if (pipePos != -1) {
                            String timeStr = scheduleStr.substring(0, pipePos);
                            String daysStr = scheduleStr.substring(pipePos + 1);

                            AutoWakeupSchedule schedule;
                            schedule.time = timeStr;

                            if (daysStr.length() == 7) {
                                for (int i = 0; i < 7; i++) {
                                    schedule.days[i] = (daysStr.charAt(i) == '1');
                                }
                            }

                            schedules.push_back(schedule);
                        }

                        if (end == -1)
                            break;
                        start = end + 1;
                        end = schedulesStr.indexOf(';', start);
                    }
                }

                if (schedules.empty()) {
                    schedules.push_back(AutoWakeupSchedule("07:00")); // Default fallback
                }
                settings->setAutoWakeupSchedules(schedules);
            }
            settings->save(true);
        });
        pluginManager->trigger("settings:changed");
        pluginManager->trigger("rl:settings:changed");
        controller->setTargetTemp(controller->getTargetTemp());
        controller->setPumpModelCoeffs();
    }

    AsyncResponseStream *response = request->beginResponseStream("application/json");
    JsonDocument doc(&psramAllocator);
    Settings const &settings = controller->getSettings();
    doc["startupMode"] = settings.getStartupMode() == MODE_BREW ? "brew" : "standby";
    doc["startupProfile"] = settings.getStartupProfile();
    doc["targetSteamTemp"] = settings.getTargetSteamTemp();
    doc["targetWaterTemp"] = settings.getTargetWaterTemp();
    doc["homekit"] = settings.isHomekit();
    AutoTuning::Router router(settings.getRLOptimizerConfiguration(), controller->getOptimizerTransport());
    const bool rlOffBoardProvider = router.routeOffBoardTransport();
    const bool rlEnabled = router.enabled();
    doc["rlAutoTuningEnabled"] = rlEnabled;
    doc["rlProviderMode"] = settings.getRLAutoTuningProviderMode();
    doc["rlProviderStatus"] = router.providerStatus();
    doc["rlProviderSummary"] = router.providerSummary();
    const int mqttPort = settings.getHomeAssistantPort();
    doc["rlMqttConfigured"] = !settings.getHomeAssistantIP().isEmpty() && mqttPort > 0 && mqttPort <= 65535;
    doc["rlMqttConnected"] = controller->getOptimizerTransport() && controller->getOptimizerTransport()->connected();
    doc["legacyHomeAssistantMqttAvailable"] = FeatureFlags::legacyHomeAssistantMqtt;
    doc["rlBeanContextId"] = settings.getRLBeanContextId();
    doc["rlBeanContextName"] = settings.getRLBeanContextName();
    doc["rlGrinderContextId"] = settings.getRLGrinderContextId();
    doc["rlGrinderContextName"] = settings.getRLGrinderContextName();
    JsonDocument activeTasteGoal;
    AutoTuning::activeTasteGoal(settings, activeTasteGoal);
    doc["rlTasteGoal"].set(activeTasteGoal.as<JsonVariantConst>());
    doc["rlTasteGoalSummary"] = AutoTuning::tasteGoalSummary(activeTasteGoal.as<JsonVariantConst>());
    doc["rlGrinderCatalogSearchUrl"] = rlGrinderCatalogSearchUrl;
    doc["rlOptimizationPaused"] = settings.isRLOptimizationPaused();
    doc["rlLocalOptimizationEnabled"] = router.optimizationActive();
    JsonDocument contextsDoc;
    JsonArray contexts = loadRLContexts(contextsDoc, settings.getRLBeanContextsJson());
    JsonArray contextsOut = doc["rlBeanContexts"].to<JsonArray>();
    for (JsonObject context : contexts) {
        JsonObject out = contextsOut.add<JsonObject>();
        out["id"] = context["id"].as<String>();
        out["name"] = context["name"].as<String>();
        out["bag_index"] = context["bag_index"] | 1;
        out["status"] = context["status"].as<String>();
        out["active"] = context["id"].as<String>() == settings.getRLBeanContextId();
    }
    JsonDocument grinderContextsDoc;
    JsonArray grinderContexts = loadRLContexts(grinderContextsDoc, settings.getRLGrinderContextsJson());
    JsonArray grinderContextsOut = doc["rlGrinderContexts"].to<JsonArray>();
    for (JsonObject context : grinderContexts) {
        JsonObject out = grinderContextsOut.add<JsonObject>();
        out["id"] = context["id"].as<String>();
        out["name"] = context["name"].as<String>();
        out["status"] = context["status"].as<String>();
        out["active"] = context["id"].as<String>() == settings.getRLGrinderContextId();
        copyRLGrinderCalibration(out, context);
    }
    JsonObject activeGrinderContext = findRLContext(grinderContexts, settings.getRLGrinderContextId());
    if (!activeGrinderContext.isNull()) {
        JsonObject calibration = doc["rlActiveGrinderCalibration"].to<JsonObject>();
        copyRLGrinderCalibration(calibration, activeGrinderContext);
    }
    doc["rlStatusSeen"] = rlOffBoardProvider ? rlStatusSeen : rlEnabled;
    doc["rlAddonOnline"] = rlOffBoardProvider ? rlAddonOnline : router.providerAvailable();
    doc["rlLastStatusAt"] = rlLastStatusAt;
    doc["rlLastShotId"] = rlLastShotId;
    doc["rlLastShotAt"] = rlLastShotAt;
    doc["rlLastShotType"] = rlLastShotType;
    doc["rlLastShotTimeS"] = rlLastShotTimeS;
    doc["rlLastShotBeverageOutG"] = rlLastShotBeverageOutG;
    doc["rlLastShotTargetYieldG"] = rlLastShotTargetYieldG;
    doc["rlLastRecommendationId"] = rlLastRecommendationId;
    doc["rlLastRecommendationAt"] = rlLastRecommendationAt;
    doc["rlRecommendationApplyStatus"] = rlRecommendationApplyStatus;
    doc["rlRecommendationStatus"] = rlRecommendationStatus;
    doc["rlRecommendationMode"] = rlRecommendationMode;
    doc["rlRecommendationGrindDeltaStepsFromCurrent"] = rlRecommendationGrindDeltaStepsFromCurrent;
    doc["rlRecommendationGrindDeltaUmFromCurrent"] = rlRecommendationGrindDeltaUmFromCurrent;
    doc["rlRecommendationProjectedRelativeStepFromReference"] = rlRecommendationProjectedRelativeStepFromReference;
    doc["rlRecommendationProjectedRelativeGrindUmFromReference"] = rlRecommendationProjectedRelativeGrindUmFromReference;
    doc["rlRecommendationNextDoseG"] = rlRecommendationNextDoseG;
    doc["rlRecommendationTargetYieldG"] = rlRecommendationTargetYieldG;
    doc["rlRecommendationTargetRatio"] = rlRecommendationTargetRatio;
    doc["rlRecommendationHasCurrentAbsoluteStep"] = rlRecommendationHasCurrentAbsoluteStep;
    doc["rlRecommendationCurrentAbsoluteStep"] = rlRecommendationCurrentAbsoluteStep;
    doc["rlRecommendationHasProjectedAbsoluteStep"] = rlRecommendationHasProjectedAbsoluteStep;
    doc["rlRecommendationProjectedAbsoluteStep"] = rlRecommendationProjectedAbsoluteStep;
    doc["rlMode"] = rlMode;
    doc["rlOptimizerProfileId"] = rlOptimizerProfileId;
    doc["rlOptimizerProfileLabel"] = rlOptimizerProfileLabel;
    doc["rlOptimizerMode"] = settings.getRLOptimizerMode();
    doc["rlCPBOProfileName"] = settings.getRLCPBOProfileName();
    doc["rlCPBOComparisonMode"] = settings.getRLCPBOComparisonMode();
    doc["rlCPBOEffectiveProfileName"] = rlCPBOEffectiveProfileName;
    doc["rlCPBOEffectiveComparisonMode"] = rlCPBOEffectiveComparisonMode;
    doc["rlDoseTargetG"] = settings.getTargetGrindVolume();
    addRLRecipeDomain(doc.as<JsonObject>(), settings);
    doc["rlOptimizerConfiguredMode"] = rlOptimizerConfiguredMode;
    doc["rlOptimizerEffectiveMode"] = rlOptimizerEffectiveMode;
    doc["rlOptimizerFallbackReason"] = rlOptimizerFallbackReason;
    doc["rlLocalShotCount"] = rlLocalShotCount;
    doc["rlCommunityUploadEnabled"] = settings.isRLCommunityUploadEnabled();
    doc["rlCommunityUploadPrompted"] = settings.isRLCommunityUploadPrompted();
    doc["rlUploadBaseUrl"] = settings.getRLUploadBaseUrl();
    doc["rlUploadCredentialConfigured"] = settings.hasRLUploadCredentials();
    doc["communityUploadRequested"] = settings.isRLCommunityUploadEnabled();
    doc["communityUploadEffective"] = communityUploadEffective;
    doc["communityUploadConfigured"] = communityUploadConfigured;
    doc["communityUploadStatus"] = communityUploadStatus;
    doc["communityUploadSummary"] = communityUploadSummary;
    doc["communityUploadStorageBackend"] = communityUploadStorageBackend;
    doc["communityUploadStorageAvailable"] = communityUploadStorageAvailable;
    doc["communityUploadPendingCount"] = communityUploadPendingCount;
    doc["communityUploadRetryCount"] = communityUploadRetryCount;
    doc["communityUploadRejectedCount"] = communityUploadRejectedCount;
    doc["rlRuntimeHealthStatus"] = rlOffBoardProvider ? rlRuntimeHealthStatus : (rlEnabled ? "attention" : "waiting");
    doc["rlRuntimeHealthSummary"] =
        rlOffBoardProvider && !rlRuntimeHealthSummary.isEmpty() ? rlRuntimeHealthSummary : router.providerSummary();
    copyRLStringArray(doc, "rlRuntimeHealthWarnings", rlRuntimeHealthWarningsJson);
    copyRLStringArray(doc, "rlRuntimeHealthWaitingReasons", rlRuntimeHealthWaitingReasonsJson);
    copyRLDiagnosticSteps(doc, rlAutoTuningDiagnosticStepsJson);
    doc["rlRuntimeHealthStorageBackend"] = rlRuntimeHealthStorageBackend;
    doc["rlRuntimeHealthStorageAvailable"] = rlRuntimeHealthStorageAvailable;
    doc["rlLocalDeliveryPendingCount"] = rlLocalDeliveryPendingCount;
    doc["rlLocalDeliveryRetryCount"] = rlLocalDeliveryRetryCount;
    doc["rlLocalDeliveryRejectedCount"] = rlLocalDeliveryRejectedCount;
    doc["rlLocalDeliveryLastError"] = rlLocalDeliveryLastError;
    copyRLRecentShots(doc, rlRecentShotsJson);
    doc["homeAssistant"] = settings.isHomeAssistant();
    doc["haUser"] = settings.getHomeAssistantUser();
    doc["haPassword"] = settings.getHomeAssistantPassword();
    doc["haIP"] = settings.getHomeAssistantIP();
    doc["haPort"] = settings.getHomeAssistantPort();
    doc["haTopic"] = settings.getHomeAssistantTopic();
    doc["pid"] = settings.getPid();
    doc["pumpModelCoeffs"] = settings.getPumpModelCoeffs();
    doc["pumpSlipCoeffs"] = settings.getPumpSlipCoeffs();
    doc["wifiSsid"] = settings.getWifiSsid();
    doc["wifiPassword"] = apMode ? "---unchanged---" : settings.getWifiPassword();
    doc["apPassword"] = settings.getWifiApPassword();
    doc["mdnsName"] = settings.getMdnsName();
    doc["temperatureOffset"] = String(settings.getTemperatureOffset());
    doc["pressureScaling"] = String(settings.getPressureScaling());
    doc["boilerFillActive"] = settings.isBoilerFillActive();
    doc["startupFillTime"] = settings.getStartupFillTime() / 1000;
    doc["steamFillTime"] = settings.getSteamFillTime() / 1000;
    doc["smartGrindActive"] = settings.isSmartGrindActive();
    doc["smartGrindIp"] = settings.getSmartGrindIp();
    doc["smartGrindMode"] = settings.getSmartGrindMode();
    doc["momentaryButtons"] = settings.isMomentaryButtons();
    doc["brewDelay"] = settings.getBrewDelay();
    doc["hardwareBrewDelay"] = settings.getHardwareBrewDelay();
    doc["grindDelay"] = settings.getGrindDelay();
    doc["delayAdjust"] = settings.isDelayAdjust();
    doc["timezone"] = settings.getTimezone();
    doc["clock24hFormat"] = settings.isClock24hFormat();
    doc["standbyTimeout"] = settings.getStandbyTimeout() / 1000;
    doc["mainBrightness"] = settings.getMainBrightness();
    doc["standbyBrightness"] = settings.getStandbyBrightness();
    doc["standbyBrightnessTimeout"] = settings.getStandbyBrightnessTimeout() / 1000;
    doc["steamPumpPercentage"] = settings.getSteamPumpPercentage();
    doc["steamPumpCutoff"] = settings.getSteamPumpCutoff();
    doc["themeMode"] = settings.getThemeMode();
    doc["sunriseIdle"] = settings.getSunriseIdle();
    doc["sunriseActive"] = settings.getSunriseActive();
    doc["sunriseFinished"] = settings.getSunriseFinished();
    doc["sunriseError"] = settings.getSunriseError();
    doc["sunriseExtBrightness"] = settings.getSunriseExtBrightness();
    doc["emptyTankDistance"] = settings.getEmptyTankDistance();
    doc["fullTankDistance"] = settings.getFullTankDistance();
    doc["altRelayFunction"] = settings.getAltRelayFunction();
    doc["scaleSource"] = settings.getScaleSource();
    doc["scaleCalibration1"] = settings.getScaleCalibration1();
    doc["scaleCalibration2"] = settings.getScaleCalibration2();
    doc["scaleOffset1"] = settings.getScaleOffset1();
    doc["scaleOffset2"] = settings.getScaleOffset2();
    doc["scaleCalTimestamp1"] = settings.getScaleCalTimestamp1();
    doc["scaleCalTimestamp2"] = settings.getScaleCalTimestamp2();
    doc["scaleCalStddev1"] = settings.getScaleCalStddev1();
    doc["scaleCalStddev2"] = settings.getScaleCalStddev2();
    doc["hardwareScaleDisabled"] =
#ifdef GAGGIMATE_DISABLE_HARDWARE_SCALE
        true;
#else
        false;
#endif
    // Runtime truth for whether the connected controller actually has a hardware
    // scale (STM32 + HX711 over UART). The web UI gates the "Hardware" source
    // option on this rather than only the build-time hardwareScaleDisabled flag.
    doc["scaleCapable"] = controller->scaleAvailability().hardwareCapable;
    // Add auto-wakeup settings to response
    doc["autowakeupEnabled"] = settings.isAutoWakeupEnabled();
    doc["buttonBehavior"] = implode(settings.getButtonBehaviorList(), ",");
    doc["commutationGain"] = settings.getCommutationGain();
    doc["convergenceGain"] = settings.getConvergenceGain();
    doc["integralGain"] = settings.getIntegralGain();
    doc["maxPumpPower"] = settings.getMaxPumpPower();
    doc["savedScale"] = settings.getSavedScale();
    doc["savedBrewScale"] = settings.getSavedBrewScale();
    doc["savedGrindScale"] = settings.getSavedGrindScale();

    // Add schedule format with days
    std::vector<AutoWakeupSchedule> autowakeupSchedules = settings.getAutoWakeupSchedules();
    String schedulesStr = "";
    for (size_t i = 0; i < autowakeupSchedules.size(); i++) {
        if (i > 0)
            schedulesStr += ";";
        schedulesStr += autowakeupSchedules[i].time + "|";

        // Convert days array to 7-bit string
        for (int j = 0; j < 7; j++) {
            schedulesStr += autowakeupSchedules[i].days[j] ? "1" : "0";
        }
    }
    doc["autowakeupSchedules"] = schedulesStr;
    serializeJson(doc, *response);
    request->send(response);

    if (request->method() == HTTP_POST && request->hasArg("restart"))
        ESP.restart();
}

void WebUIPlugin::handleBLEScaleList(AsyncWebServerRequest *request) {
    JsonDocument doc(&psramAllocator);
    JsonArray scalesArray = doc.to<JsonArray>();
    const String brewUuid = BLEScales.getUUID(ScaleRole::BREW).c_str();
    const String grindUuid = BLEScales.getUUID(ScaleRole::GRIND).c_str();
    for (const DiscoveredDevice &device : BLEScales.getDiscoveredScales()) {
        const std::string address = device.getAddress().toString();
        const String uuid = address.c_str();
        JsonDocument scale(&psramAllocator);
        scale["uuid"] = uuid;
        scale["name"] = device.getName();
        scale["rssi"] = device.getRSSI();
        scale["brewAssigned"] = uuid == controller->getSettings().getSavedBrewScale();
        scale["grindAssigned"] = uuid == controller->getSettings().getSavedGrindScale();
        scale["brewConnected"] = BLEScales.isConnected(ScaleRole::BREW) && uuid == brewUuid;
        scale["grindConnected"] = BLEScales.isConnected(ScaleRole::GRIND) && uuid == grindUuid;
        scalesArray.add(scale);
    }
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

void WebUIPlugin::handleBLEScaleScan(AsyncWebServerRequest *request) {
    if (request->method() != HTTP_POST) {
        request->send(404);
        return;
    }
    BLEScales.scan();
    JsonDocument doc(&psramAllocator);
    doc["success"] = true;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

void WebUIPlugin::handleBLEScaleConnect(AsyncWebServerRequest *request) {
    if (request->method() != HTTP_POST) {
        request->send(404);
        return;
    }
    const String uuid = request->arg("uuid");
    const String role = request->hasArg("role") ? request->arg("role") : "both";
    if (role == "brew") {
        BLEScales.connect(uuid.c_str(), ScaleRole::BREW);
    } else if (role == "grind") {
        BLEScales.connect(uuid.c_str(), ScaleRole::GRIND);
    } else {
        BLEScales.connect(uuid.c_str());
    }
    JsonDocument doc(&psramAllocator);
    doc["success"] = true;
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

void WebUIPlugin::handleBLEScaleInfo(AsyncWebServerRequest *request) {
    JsonDocument doc(&psramAllocator);
    doc["connected"] = BLEScales.isConnected();
    doc["brewConnected"] = BLEScales.isConnected(ScaleRole::BREW);
    doc["grindConnected"] = BLEScales.isConnected(ScaleRole::GRIND);
    doc["name"] = BLEScales.getName();
    doc["uuid"] = BLEScales.getUUID();
    doc["brewName"] = BLEScales.getName(ScaleRole::BREW);
    doc["brewUuid"] = BLEScales.getUUID(ScaleRole::BREW);
    doc["grindName"] = BLEScales.getName(ScaleRole::GRIND);
    doc["grindUuid"] = BLEScales.getUUID(ScaleRole::GRIND);
    doc["savedBrewScale"] = controller->getSettings().getSavedBrewScale();
    doc["savedGrindScale"] = controller->getSettings().getSavedGrindScale();
    doc["rssi"] = BLEScales.getRSSI();
    doc["hasBattery"] = BLEScales.hasBatteryLevel();
    // Only surface the numeric when the scale reports one â€” a 255 sentinel
    // (REMOTE_SCALES_BATTERY_UNKNOWN) would otherwise render as a fake "255%".
    if (BLEScales.hasBatteryLevel()) {
        const uint8_t pct = BLEScales.getBatteryLevel();
        if (pct != REMOTE_SCALES_BATTERY_UNKNOWN) {
            doc["battery"] = pct;
        }
    }
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    serializeJson(doc, *response);
    request->send(response);
}

void WebUIPlugin::updateOTAStatus(const String &version) {
    if (ws.getClients().empty()) {
        return;
    }
    Settings const &settings = controller->getSettings();
    JsonDocument doc(&psramAllocator);
    doc["latestVersion"] = ota->getCurrentVersion();
    doc["tp"] = "res:ota-settings";
    doc["otaEnabled"] = OTA_ENABLED;
    doc["controllerOtaEnabled"] = CONTROLLER_OTA_ENABLED;
    doc["displayUpdateAvailable"] = OTA_ENABLED && ota->isUpdateAvailable(false);
    doc["controllerUpdateAvailable"] = OTA_ENABLED && CONTROLLER_OTA_ENABLED && ota->isUpdateAvailable(true);
    doc["displayVersion"] = BUILD_GIT_VERSION;
    doc["controllerVersion"] = controller->getSystemInfo().version;
    doc["hardware"] = controller->getSystemInfo().hardware;
    doc["latestVersion"] = ota->getCurrentVersion();
    doc["channel"] = settings.getOTAChannel();
    doc["updating"] = updating;
    // LittleFS usage metrics
    {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        size_t freeBytes = total > used ? (total - used) : 0;
        doc["spiffsTotal"] = static_cast<uint32_t>(total);
        doc["spiffsUsed"] = static_cast<uint32_t>(used);
        doc["spiffsFree"] = static_cast<uint32_t>(freeBytes);
        if (total > 0) {
            doc["spiffsUsedPct"] = static_cast<uint8_t>((used * 100) / total);
        }
    }
    // Memory usage metrics
    {
        size_t free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
        doc["heapFree"] = static_cast<uint32_t>(free);
        doc["heapLargest"] = static_cast<uint32_t>(largest);
        doc["heapTotal"] = static_cast<uint32_t>(total);
    }
    {
        size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        size_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        doc["psramFree"] = static_cast<uint32_t>(free);
        doc["psramLargest"] = static_cast<uint32_t>(largest);
        doc["psramTotal"] = static_cast<uint32_t>(total);
    }
    doc["controllerTaskHealth"] = controller->isTaskHealthy();
#ifndef GAGGIMATE_HEADLESS
    doc["uiTaskHealth"] = controller->getUI()->isTaskHealthy();
#endif
    if (controller->isSDCard()) {
        const uint64_t total = SD_MMC.cardSize();
        const uint64_t used = SD_MMC.usedBytes();
        const uint64_t freeBytes = total > used ? (total - used) : 0;
        doc["sdTotal"] = total;
        doc["sdUsed"] = used;
        doc["sdFree"] = freeBytes;
        if (total > 0) {
            // Provide integer percentage to avoid float JSON
            doc["sdUsedPct"] = static_cast<uint8_t>((used * 100) / total);
        }
    }
    broadcastJson(doc);
}

void WebUIPlugin::updateOTAProgress(uint8_t phase, int progress) {
    if (ws.getClients().empty()) {
        return;
    }
    progress = std::min(std::max(progress, 0), 100);
    JsonDocument doc(&psramAllocator);
    doc["tp"] = "evt:ota-progress";
    doc["phase"] = phase;
    doc["progress"] = progress;
    broadcastJson(doc);
}

void WebUIPlugin::broadcastJson(JsonDocument &doc) {
    if (ws.getClients().empty()) {
        return;
    }
    ws.textAll(toWsBuffer(doc));
}

void WebUIPlugin::sendAutotuneResult() {
    JsonDocument doc(&psramAllocator);
    doc["tp"] = "evt:autotune-result";
    doc["pid"] = controller->getSettings().getPid();
    broadcastJson(doc);
}

void WebUIPlugin::sendAutotuneFailed() {
    // Distinct WS event â€” Autotune page renders "timed out" error card
    // instead of stuck spinner. Fires on ERROR_CODE_AUTOTUNE_TIMEOUT.
    JsonDocument doc(&psramAllocator);
    doc["tp"] = "evt:autotune-failed";
    broadcastJson(doc);
}

void WebUIPlugin::handleFlushStart(uint32_t clientId, JsonDocument &request) {
    controller->postCommand(CtrlCmd::START_FLUSH);

    JsonDocument response(&psramAllocator);
    response["tp"] = "res:flush:start";
    response["rid"] = request["rid"];
    response["success"] = true;
    ws.text(clientId, toWsBuffer(response));
}

void WebUIPlugin::handleCoreDumpDownload(AsyncWebServerRequest *request) {
    // Check if core dump is available
    size_t coreAddr, coreSize;
    if (esp_core_dump_image_get(&coreAddr, &coreSize) != ESP_OK || coreSize == 0) {
        request->send(404, "text/plain", "No core dump available");
        return;
    }

    // Find the coredump partition
    const esp_partition_t *coredump_partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (coredump_partition == NULL) {
        request->send(500, "text/plain", "Core dump partition not found");
        return;
    }

    ESP_LOGI("WebUIPlugin", "Streaming core dump: %d bytes from 0x%x", coreSize, coreAddr);

    // Create a streaming response
    AsyncWebServerResponse *response =
        request->beginResponse("application/octet-stream", coreSize,
                               [coredump_partition, coreSize](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                                   // Calculate how much to read
                                   size_t remaining = coreSize - index;
                                   size_t toRead = (remaining < maxLen) ? remaining : maxLen;

                                   if (toRead == 0)
                                       return 0;

                                   // Read from partition
                                   esp_err_t err = esp_partition_read(coredump_partition, index, buffer, toRead);
                                   if (err != ESP_OK) {
                                       ESP_LOGE("WebUIPlugin", "Failed to read core dump: %s", esp_err_to_name(err));
                                       return 0;
                                   }

                                   return toRead;
                               });

    // Set appropriate headers
    response->addHeader("Content-Disposition", "attachment; filename=\"coredump.bin\"");
    response->addHeader("Cache-Control", "no-cache");

    request->send(response);
}
