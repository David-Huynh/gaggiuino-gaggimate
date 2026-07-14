#include "CommunityUploadPlugin.h"
#include "community/CommunityPayloadValidator.h"

#include "autotuning/AutoTuningTasteGoalJson.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <algorithm>
#include <cmath>
#include <display/core/AutoTuning.h>
#include <display/core/Controller.h>
#include <display/core/EpochTime.h>
#include <display/core/Event.h>
#include <display/core/PluginManager.h>
#include <display/core/Settings.h>
#include <display/util/PsramAllocator.h>
#include <esp_log.h>
#include <utility>

namespace {
struct StateLock {
    SemaphoreHandle_t mutex;
    bool locked;
    explicit StateLock(SemaphoreHandle_t value)
        : mutex(value), locked(value && xSemaphoreTakeRecursive(value, portMAX_DELAY) == pdTRUE) {}
    ~StateLock() {
        if (locked) {
            xSemaphoreGiveRecursive(mutex);
        }
    }
};

constexpr const char *LOG_TAG = "CommunityUpload";
constexpr unsigned long STATUS_INTERVAL_MS = 5000;
constexpr unsigned long REGISTER_INTERVAL_MS = 60000;
constexpr unsigned long UPLOAD_INTERVAL_MS = 10000;
constexpr uint32_t SHOT_UPLOAD_GRACE_SECONDS = 120;
constexpr uint32_t RECOMMENDATION_UPLOAD_GRACE_SECONDS = 5;
constexpr const char *REGISTER_PATH = "/functions/v1/espresso-rl-register";
constexpr const char *INGEST_PATH = "/functions/v1/espresso-rl-ingest";

static bool jsonNumber(JsonVariantConst value) {
    return !value.is<bool>() && (value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>());
}

using EpochSeconds = EpochTime::Seconds;

static EpochSeconds nowEpoch() { return EpochTime::now(); }

static bool timeLooksValid() { return nowEpoch() >= EpochTime::MIN_VALID; }

static String trimTrailingSlash(String value) {
    value.trim();
    while (value.endsWith("/")) {
        value.remove(value.length() - 1);
    }
    return value;
}

static String supabaseBaseFromSetting(String value) {
    value = trimTrailingSlash(value);
    const int functionsIndex = value.indexOf("/functions/v1/");
    if (functionsIndex >= 0) {
        value = value.substring(0, functionsIndex);
    }
    return trimTrailingSlash(value);
}

static String safeIdentifier(String value, size_t maxLen = 150) {
    value.trim();
    String out;
    out.reserve(std::min(value.length(), maxLen));
    for (size_t i = 0; i < value.length() && out.length() < maxLen; i++) {
        const char c = value.charAt(i);
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == '.' || c == ':' || c == '@';
        out += ok ? c : '_';
    }
    if (out.isEmpty()) {
        out = "record";
    }
    return out;
}

static bool addResampledNumeric(AutoTuning::ArrayView<const AutoTuning::ShotSample> samples,
                                float AutoTuning::ShotSample::*member, JsonObject sequence, const char *targetKey,
                                size_t outputCount, float minValue, float maxValue) {
    JsonArray output = sequence[targetKey].to<JsonArray>();
    size_t right = 1;
    const float firstTime = samples[0].elapsedMs;
    for (size_t index = 0; index < outputCount; ++index) {
        const float sampleTime = firstTime + static_cast<float>(index * 250);
        while (right < samples.size() && samples[right].elapsedMs < sampleTime) {
            ++right;
        }
        const size_t upper = std::min(right, samples.size() - 1);
        const size_t lower = upper > 0 ? upper - 1 : 0;
        const float lowerTime = samples[lower].elapsedMs;
        const float upperTime = samples[upper].elapsedMs;
        const float lowerValue = samples[lower].*member;
        const float upperValue = samples[upper].*member;
        if (!std::isfinite(lowerValue) || !std::isfinite(upperValue)) {
            return false;
        }
        const float fraction = upperTime > lowerTime ? (sampleTime - lowerTime) / (upperTime - lowerTime) : 0.0f;
        output.add(std::clamp(lowerValue + (upperValue - lowerValue) * fraction, minValue, maxValue));
    }
    return true;
}

static void addFixedCadenceSequence(AutoTuning::ShotRecord const &shot, JsonObject target) {
    const auto samples = shot.samples;
    if (samples.size() < 2 || samples.size() > 500) {
        return;
    }
    for (size_t index = 1; index < samples.size(); ++index) {
        if (samples[index].elapsedMs <= samples[index - 1].elapsedMs) {
            return;
        }
    }
    const size_t outputCount = static_cast<size_t>((samples[samples.size() - 1].elapsedMs - samples[0].elapsedMs) / 250U) + 1;
    if (outputCount < 2 || outputCount > 500) {
        return;
    }

    JsonObject sequence = target["fixed_cadence_sequence"].to<JsonObject>();
    sequence["sample_interval_ms"] = 250;
    const bool numericOk =
        addResampledNumeric(samples, &AutoTuning::ShotSample::pressure, sequence, "pressure_bar", outputCount, 0.0f, 15.0f) &&
        addResampledNumeric(samples, &AutoTuning::ShotSample::targetPressure, sequence, "pressure_target_bar", outputCount, 0.0f,
                            15.0f) &&
        addResampledNumeric(samples, &AutoTuning::ShotSample::pumpFlow, sequence, "pump_flow_ml_s", outputCount, 0.0f, 20.0f) &&
        addResampledNumeric(samples, &AutoTuning::ShotSample::targetFlow, sequence, "pump_flow_target_ml_s", outputCount, 0.0f,
                            20.0f) &&
        addResampledNumeric(samples, &AutoTuning::ShotSample::flow, sequence, "beverage_flow_g_s", outputCount, 0.0f, 20.0f) &&
        addResampledNumeric(samples, &AutoTuning::ShotSample::weight, sequence, "weight_g", outputCount, -1.0f,
                            AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G) &&
        addResampledNumeric(samples, &AutoTuning::ShotSample::temperature, sequence, "temperature_c", outputCount, 0.0f,
                            160.0f) &&
        addResampledNumeric(samples, &AutoTuning::ShotSample::targetTemperature, sequence, "temperature_target_c", outputCount,
                            0.0f, 160.0f);
    if (!numericOk) {
        target.remove("fixed_cadence_sequence");
        return;
    }

    JsonArray pumpMode = sequence["pump_target_mode"].to<JsonArray>();
    JsonArray valveOpen = sequence["valve_open"].to<JsonArray>();
    size_t sourceIndex = 0;
    for (size_t index = 0; index < outputCount; ++index) {
        const unsigned sampleTime = samples[0].elapsedMs + static_cast<unsigned>(index * 250);
        while (sourceIndex + 1 < samples.size() && samples[sourceIndex + 1].elapsedMs <= sampleTime) {
            ++sourceIndex;
        }
        pumpMode.add(static_cast<std::uint8_t>(samples[sourceIndex].pumpTargetMode));
        valveOpen.add(samples[sourceIndex].valveOpen);
    }
}

} // namespace

void CommunityUploadPlugin::setup(Controller *ctrl, PluginManager *pm) {
    controller = ctrl;
    pluginManager = pm;
    ctrl->setCommunityUpload(this);
    stateMutex = xSemaphoreCreateRecursiveMutex();
    refreshConfiguration();
    uploadQueue.begin();
    uploadQueue.recover();
    discardMismatchedQueueItems();
    xTaskCreatePinnedToCore(workerTask, "CommunityUpload", 12288, this, 1, &workerTaskHandle, 0);

    pm->on("settings:changed", [this](Event const &) {
        refreshConfiguration();
        discardMismatchedQueueItems();
        {
            StateLock lock(stateMutex);
            registrationReadyPending = uploadConfiguration.requested && uploadConfiguration.configured();
        }
        publishStatus();
    });
    pm->on("rl:settings:changed", [this](Event const &) {
        refreshConfiguration();
        discardMismatchedQueueItems();
        {
            StateLock lock(stateMutex);
            registrationReadyPending = uploadConfiguration.requested && uploadConfiguration.configured();
        }
        publishStatus();
    });
    pm->on("rl:status:refresh", [this](Event const &) { publishStatus(); });
    pm->on("controller:wifi:connect", [this](Event const &) {
        resetWorkerAttempts();
        publishStatus();
    });
    pm->on("rl:preference", [this](Event const &event) { handlePreference(event); });
    pm->on("rl:shot:correction", [this](Event const &event) { handleShotCorrection(event); });

    publishStatus();
}

bool CommunityUploadPlugin::enqueueShot(AutoTuning::ShotRecord const &shot) {
    if (!uploadRequested() || !uploadConfigured()) {
        return false;
    }
    String shotId;
    String payloadJson;
    return buildShotPayload(shot, shotId, payloadJson) &&
           enqueueValidatedPayload("shot", shotId, payloadJson, SHOT_UPLOAD_GRACE_SECONDS, true);
}

bool CommunityUploadPlugin::enqueueRecommendation(AutoTuning::Recommendation const &recommendation) {
    if (!uploadRequested() || !uploadConfigured()) {
        return false;
    }
    String recommendationId;
    String payloadJson;
    return buildRecommendationPayload(recommendation, recommendationId, payloadJson) &&
           enqueueValidatedPayload("recommendation", recommendationId, payloadJson, RECOMMENDATION_UPLOAD_GRACE_SECONDS, true);
}

bool CommunityUploadPlugin::enqueuePreference(AutoTuning::PreferenceFeedback const &preference) {
    if (!uploadRequested() || !uploadConfigured()) {
        return false;
    }
    String comparisonId;
    String payloadJson;
    return buildComparisonPayload(preference, comparisonId, payloadJson) &&
           enqueueValidatedPayload("comparison", comparisonId, payloadJson, RECOMMENDATION_UPLOAD_GRACE_SECONDS, true);
}

bool CommunityUploadPlugin::applyCorrection(AutoTuning::ShotCorrection const &correction) {
    if (!uploadRequested() || !uploadConfigured() || correction.shotId.empty()) {
        return false;
    }
    const EpochSeconds updatedAt = nowEpoch();
    const bool patched = uploadQueue.patchShotCorrection(
        correction.shotId.c_str(), correction.grindFollowed.has_value(), correction.grindFollowed.value_or(false),
        correction.doseFollowed.has_value(), correction.doseFollowed.value_or(false), correction.yieldFollowed.has_value(),
        correction.yieldFollowed.value_or(false), updatedAt,
        updatedAt + static_cast<EpochSeconds>(RECOMMENDATION_UPLOAD_GRACE_SECONDS));
    if (patched) {
        publishStatus();
    }
    return patched;
}

void CommunityUploadPlugin::loop() {
    bool applyCredentials = false;
    bool clearCredentials = false;
    bool registrationReady = false;
    bool publishRequested = false;
    String installId;
    String tokenId;
    String secret;
    {
        StateLock lock(stateMutex);
        applyCredentials = credentialUpdatePending;
        clearCredentials = credentialClearPending;
        installId = pendingInstallId;
        tokenId = pendingTokenId;
        secret = pendingSecret;
        credentialUpdatePending = false;
        credentialClearPending = false;
        pendingInstallId = "";
        pendingTokenId = "";
        pendingSecret = "";
        registrationReady = registrationReadyPending;
        registrationReadyPending = false;
        publishRequested = statusPublishRequested;
        statusPublishRequested = false;
    }

    if (controller && clearCredentials) {
        controller->getSettings().clearRLUploadCredentials();
        refreshConfiguration();
        discardMismatchedQueueItems();
        resetWorkerAttempts();
    } else if (controller && applyCredentials) {
        controller->getSettings().setRLUploadCredentials(installId, tokenId, secret);
        refreshConfiguration();
        discardMismatchedQueueItems();
        registrationReady = true;
    }

    if (registrationReady) {
        Event ready;
        ready.id = "rl:community-upload:ready";
        pluginManager->trigger(ready);
    }
    if (publishRequested || millis() - lastStatusMs >= STATUS_INTERVAL_MS) {
        publishStatus();
    }
}

void CommunityUploadPlugin::requestStatusPublish() {
    StateLock lock(stateMutex);
    statusPublishRequested = true;
}

void CommunityUploadPlugin::setLastError(const String &error) {
    StateLock lock(stateMutex);
    lastError = error;
}

String CommunityUploadPlugin::getLastError() const {
    StateLock lock(stateMutex);
    return lastError;
}

void CommunityUploadPlugin::incrementRejected() {
    StateLock lock(stateMutex);
    rejectedSinceBoot += 1;
}

int CommunityUploadPlugin::rejectedCount() const {
    StateLock lock(stateMutex);
    return rejectedSinceBoot;
}

bool CommunityUploadPlugin::uploadRequested() const { return configurationSnapshot().requested; }

bool CommunityUploadPlugin::uploadConfigured() const { return configurationSnapshot().configured(); }

String CommunityUploadPlugin::uploadBaseUrl() const { return configurationSnapshot().baseUrl; }

CommunityUploadPlugin::UploadConfiguration CommunityUploadPlugin::configurationSnapshot() const {
    StateLock lock(stateMutex);
    return uploadConfiguration;
}

void CommunityUploadPlugin::refreshConfiguration() {
    if (!controller) {
        return;
    }
    Settings const &settings = controller->getSettings();
    UploadConfiguration refreshed;
    refreshed.requested = settings.isRLCommunityUploadEnabled();
    refreshed.baseUrl = supabaseBaseFromSetting(settings.getRLUploadBaseUrl());
    refreshed.installId = settings.getRLUploadInstallId();
    refreshed.tokenId = settings.getRLUploadTokenId();
    refreshed.secret = settings.getRLUploadSecret();
    StateLock lock(stateMutex);
    uploadConfiguration = std::move(refreshed);
}

void CommunityUploadPlugin::resetWorkerAttempts() {
    StateLock lock(stateMutex);
    lastRegisterAttemptMs = 0;
    lastUploadAttemptMs = 0;
}

String CommunityUploadPlugin::registrationUrl() const {
    const String base = uploadBaseUrl();
    return base.isEmpty() ? "" : base + REGISTER_PATH;
}

String CommunityUploadPlugin::machineTopicId() const {
    String mac = WiFi.macAddress();
    mac.replace(":", "_");
    return mac;
}

String CommunityUploadPlugin::machineId() const { return "gaggimate:" + machineTopicId(); }

void CommunityUploadPlugin::handlePreference(Event const &event) {
    AutoTuning::PreferenceFeedback const *preference = event.getPayload<AutoTuning::PreferenceFeedback>();
    if (preference) {
        enqueuePreference(*preference);
    }
}

void CommunityUploadPlugin::handleShotCorrection(Event const &event) {
    AutoTuning::ShotCorrection const *correction = event.getPayload<AutoTuning::ShotCorrection>();
    if (correction) {
        applyCorrection(*correction);
    }
}

bool CommunityUploadPlugin::buildRecommendationPayload(AutoTuning::Recommendation const &recommendation, String &recommendationId,
                                                       String &payloadJson) {
    const UploadConfiguration configuration = configurationSnapshot();
    if (!controller || !configuration.configured() || recommendation.recommendationId.empty()) {
        return false;
    }
    recommendationId = recommendation.recommendationId.c_str();
    const EpochSeconds createdAt = recommendation.createdAt > 0 ? recommendation.createdAt : nowEpoch();

    JsonDocument outDoc(&psramAllocator);
    JsonObject out = outDoc.to<JsonObject>();
    out["event_type"] = "recommendation_record";
    out["schema_version"] = 1;
    out["recommendation_id"] = recommendationId;
    out["created_at"] = createdAt;
    out["updated_at"] = recommendation.updatedAt > 0 ? recommendation.updatedAt : nowEpoch();
    out["install_id"] = configuration.installId;
    out["machine_id"] = recommendation.machineId.empty() ? machineId() : recommendation.machineId.c_str();
    out["source_shot_id"] = recommendation.sourceShotId.c_str();
    AutoTuning::writeTasteGoal(recommendation.tasteGoal, out["taste_goal"].to<JsonObject>());

    const auto putString = [&out](const char *key, std::string const &value) {
        if (!value.empty()) {
            out[key] = value.c_str();
        }
    };
    const auto putNumber = [&out](const char *key, std::optional<float> value) {
        if (value.has_value()) {
            out[key] = *value;
        }
    };
    const auto putTimestamp = [&out](const char *key, std::optional<AutoTuning::Timestamp> value) {
        if (value.has_value()) {
            out[key] = *value;
        }
    };
    putString("bean_context_id", recommendation.beanContextId);
    putString("grinder_context_id", recommendation.grinderContextId);
    putString("profile_id", recommendation.profileId);
    putString("raw_profile_hash", recommendation.rawProfileHash);
    putString("grinder_calibration_mode", recommendation.grinderCalibrationMode);
    putString("grinder_adjustment_mode", recommendation.grinderAdjustmentMode);
    putString("step_direction", recommendation.stepDirection);
    putString("reference_label", recommendation.referenceLabel);
    out["mode"] = AutoTuning::recommendationModeKey(recommendation.mode);
    putString("reason", recommendation.reason);
    out["status"] = AutoTuning::recommendationStatusKey(recommendation.status);
    out["optimization_run_id"] = recommendation.optimizationRunId.c_str();
    out["comparison_anchor_shot_id"] = recommendation.comparisonAnchorShotId.c_str();
    out["comparison_mode"] = AutoTuning::comparisonModeKey(recommendation.comparisonMode);
    out["preference_feedback_required"] = recommendation.preferenceFeedbackRequired;
    putTimestamp("expires_at", recommendation.expiresAt);
    out["grind_delta_steps_from_current"] = recommendation.grindDeltaStepsFromCurrent;
    out["grind_delta_um_from_current"] = recommendation.grindDeltaMicronsFromCurrent;
    out["projected_relative_step_from_reference"] = recommendation.projectedRelativeStepFromReference;
    out["projected_relative_grind_um_from_reference"] = recommendation.projectedRelativeMicronsFromReference;
    putNumber("microns_per_step", recommendation.micronsPerStep);
    putNumber("current_absolute_step", recommendation.currentAbsoluteStep);
    putNumber("absolute_reference_step", recommendation.absoluteReferenceStep);
    putNumber("projected_absolute_step", recommendation.projectedAbsoluteStep);
    out["next_dose_g"] = recommendation.nextDoseG;
    out["target_yield_g"] = recommendation.targetYieldG;
    if (recommendation.nextDoseG > 0.0f) {
        out["target_ratio"] = recommendation.targetYieldG / recommendation.nextDoseG;
    }
    putNumber("confidence", recommendation.confidence);
    putTimestamp("accepted_at", recommendation.acceptedAt);
    putTimestamp("ignored_at", recommendation.ignoredAt);
    putTimestamp("edited_at", recommendation.editedAt);
    putTimestamp("used_at", recommendation.usedAt);
    putTimestamp("superseded_at", recommendation.supersededAt);
    putTimestamp("apply_acknowledged_at", recommendation.applyAcknowledgedAt);
    out["shown_count"] = recommendation.shownCount;
    putString("apply_status", recommendation.applyStatus);
    putString("apply_error", recommendation.applyError);

    String reason;
    if (!CommunityPayloadValidator::serializeValidated(out, "recommendation", recommendationId, payloadJson, reason)) {
        setLastError("upload preflight failed: " + reason);
        incrementRejected();
        ESP_LOGW(LOG_TAG, "%s", getLastError().c_str());
        return false;
    }
    return true;
}

bool CommunityUploadPlugin::buildComparisonPayload(AutoTuning::PreferenceFeedback const &preference, String &comparisonId,
                                                   String &payloadJson) {
    const UploadConfiguration configuration = configurationSnapshot();
    if (!controller || !configuration.configured() || preference.installId.empty() || preference.optimizationRunId.empty() ||
        preference.newShotId.empty() || preference.anchorShotId.empty() || preference.newShotId == preference.anchorShotId ||
        preference.comparisonMode == AutoTuning::ComparisonMode::None || !preference.tasteGoal.valid()) {
        setLastError("invalid comparison event");
        return false;
    }
    const String identity = configuration.installId + "|" + preference.optimizationRunId.c_str() + "|" +
                            preference.newShotId.c_str() + "|" + preference.anchorShotId.c_str();
    comparisonId = "comparison_" + CommunityHttpTransport::sha256Hex(identity).substring(0, 32);

    JsonDocument outDoc(&psramAllocator);
    JsonObject out = outDoc.to<JsonObject>();
    out["event_type"] = "comparison_record";
    out["schema_version"] = 1;
    out["comparison_id"] = comparisonId;
    out["optimization_run_id"] = preference.optimizationRunId.c_str();
    out["new_shot_id"] = preference.newShotId.c_str();
    out["anchor_shot_id"] = preference.anchorShotId.c_str();
    out["label"] = AutoTuning::preferenceLabelKey(preference.label);
    out["comparison_mode"] = AutoTuning::comparisonModeKey(preference.comparisonMode);
    out["created_at"] = nowEpoch();
    out["install_id"] = configuration.installId;
    out["machine_id"] = machineId();
    out["machine_adapter"] = "gaggimate";
    AutoTuning::writeTasteGoal(preference.tasteGoal, out["taste_goal"].to<JsonObject>());
    if (!preference.recommendationId.empty()) {
        out["recommendation_id"] = preference.recommendationId.c_str();
    }

    String reason;
    if (!CommunityPayloadValidator::serializeValidated(out, "comparison", comparisonId, payloadJson, reason)) {
        setLastError("upload preflight failed: " + reason);
        incrementRejected();
        ESP_LOGW(LOG_TAG, "%s", getLastError().c_str());
        return false;
    }
    return true;
}

bool CommunityUploadPlugin::buildShotPayload(AutoTuning::ShotRecord const &shot, String &shotId, String &payloadJson) {
    const UploadConfiguration configuration = configurationSnapshot();
    if (!controller || !configuration.configured() || shot.shotId.empty()) {
        return false;
    }
    shotId = shot.shotId.c_str();
    const bool hasDose = shot.measuredDoseG.has_value();
    const float dose = shot.measuredDoseG.value_or(0.0f);
    const float doseTarget = shot.recipe.doseTargetG.value_or(0.0f);
    const float targetYield = shot.recipe.targetYieldG.value_or(0.0f);
    const float profileTemp = shot.profile.temperatureC;
    const float finalTemp = shot.finalPhase.available ? shot.finalPhase.temperatureC : profileTemp;
    if ((hasDose && (dose < AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G || dose > AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G)) ||
        doseTarget < AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G || doseTarget > AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G ||
        targetYield < AutoTuning::RECIPE_DOMAIN_OUTPUT_MIN_G || targetYield > AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G) {
        setLastError("shot payload has invalid dose target, measured dose, or yield");
        return false;
    }

    JsonDocument outDoc(&psramAllocator);
    JsonObject out = outDoc.to<JsonObject>();
    const EpochSeconds timestamp = shot.timestamp > 0 ? shot.timestamp : nowEpoch();
    out["event_type"] = "shot_record";
    out["schema_version"] = 1;
    out["shot_id"] = shotId;
    out["timestamp"] = timestamp;
    out["created_at"] = timestamp;
    out["updated_at"] = nowEpoch();
    out["install_id"] = configuration.installId;
    out["machine_id"] = shot.machineId.empty() ? machineId() : shot.machineId.c_str();
    out["machine_adapter"] = shot.machineAdapter.c_str();
    out["shot_type"] = "espresso";
    if (hasDose) {
        out["dose_in_g"] = roundf(dose * 10.0f) / 10.0f;
    }
    out["dose_target_g"] = roundf(doseTarget * 10.0f) / 10.0f;
    out["target_yield_g"] = roundf(targetYield * 10.0f) / 10.0f;
    out["target_ratio"] = out["target_yield_g"].as<float>() / out["dose_target_g"].as<float>();
    out["profile_temperature_c"] = roundf(profileTemp * 10.0f) / 10.0f;
    out["final_phase_temperature_c"] = roundf(finalTemp * 10.0f) / 10.0f;
    AutoTuning::writeTasteGoal(shot.recipe.tasteGoal, out["taste_goal"].to<JsonObject>());

    const auto putString = [&out](const char *key, std::string const &value) {
        if (!value.empty()) {
            out[key] = value.c_str();
        }
    };
    const auto putNumber = [&out](const char *key, std::optional<float> value) {
        if (value.has_value()) {
            out[key] = *value;
        }
    };
    const auto putBool = [&out](const char *key, std::optional<bool> value) {
        if (value.has_value()) {
            out[key] = *value;
        }
    };

    putString("bean_context_id", shot.recipe.beanContextId);
    putString("grinder_context_id", shot.recipe.grinder.contextId);
    putString("profile_id", shot.profile.id);
    putString("profile_label", shot.profile.label);
    putString("profile_type", shot.profile.type);
    putString("raw_profile_hash", shot.profile.rawProfileHash);
    putString("grinder_calibration_mode", shot.recipe.grinder.calibrationMode);
    putString("grinder_adjustment_mode", shot.recipe.grinder.adjustmentMode);
    putString("step_direction", shot.recipe.grinder.stepDirection);
    putString("reference_label", shot.recipe.grinder.referenceLabel);
    putString("recommendation_id", shot.recommendation.recommendationId);
    putString("weight_source", shot.weightSource);
    putString("flow_source", shot.flowSource);
    putString("flow_units", shot.flowUnits);
    putString("pump_flow_source", shot.pumpFlowSource);
    putString("pump_flow_units", shot.pumpFlowUnits);
    putString("beverage_out_observation", shot.beverageOutObservation);
    out["beverage_out_g"] = shot.beverageOutG;
    out["shot_time_s"] = shot.shotTimeS;
    putNumber("predicted_final_beverage_out_g", shot.predictedFinalBeverageOutG);
    putNumber("predictive_stop_lead_g", shot.predictiveStopLeadG);
    putNumber("predictive_stop_delay_ms", shot.predictiveStopDelayMs);
    putNumber("predictive_stop_rate_g_per_s", shot.predictiveStopRateGPerS);
    putNumber("microns_per_step", shot.recipe.grinder.micronsPerStep);
    putNumber("relative_grind_steps_from_reference", shot.recipe.grinder.relativeStepsFromReference);
    putNumber("relative_grind_um_from_reference", shot.recipe.grinder.relativeMicronsFromReference);
    putNumber("current_absolute_step", shot.recipe.grinder.currentAbsoluteStep);
    putNumber("absolute_reference_step", shot.recipe.grinder.absoluteReferenceStep);
    out["profile_phase_count"] = shot.profile.phaseCount;
    if (shot.finalPhase.available) {
        out["final_phase_index"] = shot.finalPhase.index;
        putString("final_phase_name", shot.finalPhase.name);
        putString("final_phase_type", shot.finalPhase.type);
        putString("final_pump_target", shot.finalPhase.pumpTarget);
        putString("shot_end_state", shot.finalPhase.shotEndState);
        out["final_phase_elapsed_s"] = shot.finalPhase.elapsedS;
        putNumber("final_target_pressure", shot.finalPhase.targetPressure);
        putNumber("final_target_flow", shot.finalPhase.targetFlow);
        out["final_valve_open"] = shot.finalPhase.valveOpen;
    }
    if (shot.recommendation.present()) {
        out["recommended_grind_delta_steps_from_current"] = shot.recommendation.grindDeltaStepsFromCurrent;
        out["recommended_grind_delta_um_from_current"] = shot.recommendation.grindDeltaMicronsFromCurrent;
        out["recommended_projected_relative_step_from_reference"] = shot.recommendation.projectedRelativeStepFromReference;
        out["recommended_dose_g"] = shot.recommendation.nextDoseG;
        out["recommended_target_yield_g"] = shot.recommendation.targetYieldG;
        if (shot.recommendation.nextDoseG > 0.0f) {
            out["recommended_target_ratio"] = shot.recommendation.targetYieldG / shot.recommendation.nextDoseG;
        }
    }
    out["raw_profile_available"] = shot.profile.rawProfileAvailable;
    out["exclude_from_local_optimization"] = shot.excludeFromLocalOptimization;
    out["dose_observed"] = shot.doseObserved;
    if (shot.doseTargetConfirmed) {
        out["dose_target_confirmed"] = true;
    }
    putBool("grind_followed", shot.grindFollowed);
    putBool("dose_followed", shot.doseFollowed);
    putBool("yield_followed", shot.yieldFollowed);
    const AutoTuning::FollowThroughStatus followThrough =
        AutoTuning::deriveFollowThrough(shot.grindFollowed, shot.doseFollowed, shot.yieldFollowed);
    if (followThrough != AutoTuning::FollowThroughStatus::Unknown) {
        out["recommendation_followed"] = AutoTuning::followThroughStatusKey(followThrough);
    }
    out["predictive_stop_applied"] = shot.predictiveStopApplied;
    out["pump_flow_calibration_required"] = shot.pumpFlowCalibrationRequired;
    putBool("profile_flow_valid", shot.profile.flowValid);
    putBool("profile_flow_masked", shot.profile.flowMasked);

    const bool doseMeasured = shot.doseObserved && shot.measuredDoseG.has_value();
    const bool doseUsable = doseMeasured || shot.doseTargetConfirmed;
    const float ratioDose = doseMeasured ? dose : (shot.doseTargetConfirmed ? doseTarget : 0.0f);
    if (shot.beverageOutG > 0.0f && doseUsable && ratioDose > 0.0f) {
        out["brew_ratio"] = shot.beverageOutG / ratioDose;
    }
    JsonObject observed = out["action_observed"].to<JsonObject>();
    observed["grind"] = shot.recipe.grinder.observed;
    observed["dose"] = doseUsable;
    observed["target_yield"] = true;

    addFixedCadenceSequence(shot, out);
    if (!out["fixed_cadence_sequence"].isNull()) {
        out["raw_profile_available"] = true;
    }

    String reason;
    if (!CommunityPayloadValidator::serializeValidated(out, "shot", shotId, payloadJson, reason)) {
        setLastError("upload preflight failed: " + reason);
        incrementRejected();
        ESP_LOGW(LOG_TAG, "%s", getLastError().c_str());
        return false;
    }
    return true;
}

void CommunityUploadPlugin::maybeRegister() {
    const UploadConfiguration configuration = configurationSnapshot();
    if (!configuration.requested || configuration.baseUrl.isEmpty() || configuration.configured()) {
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    {
        StateLock lock(stateMutex);
        if (millis() - lastRegisterAttemptMs < REGISTER_INTERVAL_MS) {
            return;
        }
        lastRegisterAttemptMs = millis();
    }
    if (registerDevice()) {
        requestStatusPublish();
    }
}

bool CommunityUploadPlugin::registerDevice() {
    if (!controller) {
        return false;
    }
    const String url = registrationUrl();
    if (url.isEmpty()) {
        setLastError("upload base URL missing");
        return false;
    }

    const CommunityHttpTransport::Result result = httpTransport.registerDevice(url);
    if (!result.started) {
        setLastError(result.error);
        return false;
    }

    if (result.statusCode != 201 && result.statusCode != 200) {
        setLastError("registration HTTP " + String(result.statusCode));
        return false;
    }

    JsonDocument doc(&psramAllocator);
    if (deserializeJson(doc, result.response) || !doc.is<JsonObject>()) {
        setLastError("registration response invalid");
        return false;
    }
    const String installId = doc["install_id"].as<String>();
    const String tokenId = doc["upload_token_id"].as<String>();
    const String secret = doc["upload_secret"].as<String>();
    if (installId.isEmpty() || tokenId.isEmpty() || secret.length() < 32) {
        setLastError("registration response missing credentials");
        return false;
    }

    {
        StateLock lock(stateMutex);
        pendingInstallId = installId;
        pendingTokenId = tokenId;
        pendingSecret = secret;
        credentialUpdatePending = true;
    }
    setLastError("");
    ESP_LOGI(LOG_TAG, "Registered EspressoRL community upload credential");
    return true;
}

void CommunityUploadPlugin::maybeUpload() {
    const UploadConfiguration configuration = configurationSnapshot();
    if (!configuration.requested || !configuration.configured()) {
        return;
    }
    if (WiFi.status() != WL_CONNECTED || !timeLooksValid()) {
        return;
    }
    {
        StateLock lock(stateMutex);
        if (millis() - lastUploadAttemptMs < UPLOAD_INTERVAL_MS) {
            return;
        }
        lastUploadAttemptMs = millis();
    }
    if (uploadOne()) {
        requestStatusPublish();
    }
}

bool CommunityUploadPlugin::uploadOne() {
    const UploadConfiguration configuration = configurationSnapshot();
    if (!controller || !configuration.configured()) {
        return false;
    }

    const EpochSeconds now = nowEpoch();
    CommunityUploadQueue::Item selected;
    String payloadJson;
    if (!uploadQueue.selectReady(configuration.baseUrl, now, selected, payloadJson)) {
        return false;
    }

    JsonDocument queuedPayload(&psramAllocator);
    if (deserializeJson(queuedPayload, payloadJson) || !queuedPayload.is<JsonObject>()) {
        const auto result = uploadQueue.removeIfCurrent(selected, payloadJson);
        if (result == CommunityUploadQueue::MutationResult::Failed) {
            setLastError("invalid queue item could not be removed");
            return false;
        }
        incrementRejected();
        setLastError("queued upload is invalid JSON");
        return true;
    }

    bool migratedPayload = false;
    if (queuedPayload["taste_goal"].isNull()) {
        JsonDocument balancedGoal;
        AutoTuning::setBalancedTasteGoal(balancedGoal);
        queuedPayload["taste_goal"].set(balancedGoal.as<JsonVariantConst>());
        migratedPayload = true;
    }
    if (selected.recordType == "shot" && queuedPayload["dose_target_g"].isNull() && jsonNumber(queuedPayload["dose_in_g"])) {
        queuedPayload["dose_target_g"] = queuedPayload["dose_in_g"].as<float>();
        migratedPayload = true;
    }
    if (migratedPayload) {
        const CommunityUploadQueue::Item originalItem = selected;
        const String originalPayload = payloadJson;
        payloadJson = "";
        serializeJson(queuedPayload, payloadJson);
        if (selected.recordType != "recommendation") {
            const String migratedHash = CommunityHttpTransport::sha256Hex(payloadJson);
            selected.uploadId =
                safeIdentifier(selected.recordType + "_" + migratedHash.substring(0, 16) + "_" + selected.recordId, 220);
        }
        const auto result = uploadQueue.replaceIfCurrent(originalItem, originalPayload, selected, payloadJson);
        if (result == CommunityUploadQueue::MutationResult::Failed) {
            setLastError("queue migration write failed");
            return false;
        }
        if (result == CommunityUploadQueue::MutationResult::Stale) {
            return true;
        }
    }

    String preflightReason;
    if (!CommunityPayloadValidator::validateRecord(queuedPayload.as<JsonObjectConst>(), selected.recordType, selected.recordId,
                                                   preflightReason)) {
        const auto result = uploadQueue.removeIfCurrent(selected, payloadJson);
        if (result == CommunityUploadQueue::MutationResult::Failed) {
            setLastError("rejected queue item could not be removed");
            return false;
        }
        incrementRejected();
        setLastError("queued upload preflight failed: " + preflightReason);
        return true;
    }

    CommunityHttpTransport::UploadRequest request;
    request.url = selected.endpoint + INGEST_PATH;
    request.installId = configuration.installId;
    request.tokenId = configuration.tokenId;
    request.uploadId = selected.uploadId;
    request.uploadSecret = configuration.secret;
    request.payloadJson = payloadJson;
    request.timestamp = now;
    const CommunityHttpTransport::Result httpResult = httpTransport.upload(request);
    if (!httpResult.started) {
        setLastError(httpResult.error);
        return false;
    }
    const int code = httpResult.statusCode;

    if (code >= 200 && code < 300) {
        const auto result = uploadQueue.removeIfCurrent(selected, payloadJson);
        if (result == CommunityUploadQueue::MutationResult::Failed) {
            setLastError("uploaded queue item could not be removed");
            return false;
        }
        setLastError("");
        return true;
    }

    selected.attemptCount += 1;
    selected.status = CommunityUploadQueue::Status::Failed;
    const int cappedAttempts = std::min(selected.attemptCount, 6);
    selected.nextRetryAt = now + static_cast<EpochSeconds>(60L << cappedAttempts);
    setLastError("upload HTTP " + String(code));

    if (code == 400 || code == 401 || code == 403 || code == 413 || code == 422) {
        const auto result = uploadQueue.removeIfCurrent(selected, payloadJson);
        if (result == CommunityUploadQueue::MutationResult::Failed) {
            return false;
        }
        incrementRejected();
        if (code == 403 && httpResult.response.indexOf("unknown or revoked") >= 0) {
            StateLock lock(stateMutex);
            credentialClearPending = true;
        }
        return true;
    }

    const auto result = uploadQueue.replaceIfCurrent(selected, payloadJson, selected, payloadJson);
    return result != CommunityUploadQueue::MutationResult::Failed;
}

bool CommunityUploadPlugin::enqueueValidatedPayload(const String &recordType, const String &recordId, const String &payloadJson,
                                                    uint32_t delaySeconds, bool replaceRecord) {
    if (payloadJson.isEmpty() || payloadJson.length() > CommunityPayloadValidator::MAX_PAYLOAD_BYTES) {
        return false;
    }
    const String hash = CommunityHttpTransport::sha256Hex(payloadJson);
    if (hash.length() != 64) {
        return false;
    }
    const UploadConfiguration configuration = configurationSnapshot();
    if (!configuration.requested || !configuration.configured()) {
        return false;
    }
    const String uploadId = safeIdentifier(recordType + "_" + hash.substring(0, 16) + "_" + recordId, 220);
    const EpochSeconds createdAt = nowEpoch();
    if (!uploadQueue.enqueue(uploadId, recordType, recordId, payloadJson, configuration.baseUrl, createdAt,
                             createdAt + static_cast<EpochSeconds>(delaySeconds), replaceRecord)) {
        setLastError("queue write failed");
        return false;
    }
    publishStatus();
    return true;
}

void CommunityUploadPlugin::discardMismatchedQueueItems() {
    const UploadConfiguration configuration = configurationSnapshot();
    if (uploadQueue.discardMismatched(configuration.baseUrl, configuration.configured(), configuration.installId)) {
        setLastError("Upload queue cleared after endpoint identity changed");
    }
}

[[noreturn]] void CommunityUploadPlugin::workerTask(void *arg) {
    auto *plugin = static_cast<CommunityUploadPlugin *>(arg);
    vTaskDelay(pdMS_TO_TICKS(15000));
    while (true) {
        plugin->uploadQueue.removeOneLegacyItem();
        plugin->maybeRegister();
        plugin->maybeUpload();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void CommunityUploadPlugin::publishStatus() {
    if (!pluginManager) {
        return;
    }

    const UploadConfiguration configuration = configurationSnapshot();
    const bool requested = configuration.requested;
    const bool storageAvailable = uploadQueue.storageAvailable();
    const bool hasBase = !configuration.baseUrl.isEmpty();
    const bool configured = configuration.configured();
    const bool effective = requested && configured;
    CommunityUploadQueue::Stats stats = uploadQueue.stats();
    stats.rejected += rejectedCount();

    String status = "off";
    String summary = "Community upload disabled";
    if (requested && !hasBase) {
        status = "attention";
        summary = "Supabase upload base URL not configured";
    } else if (requested && !configured) {
        status = "attention";
        summary = "Device upload registration pending";
    } else if (effective && stats.failed > 0) {
        status = "attention";
        summary = "Upload queue retrying";
    } else if (effective) {
        status = "ready";
        summary = "Device upload ready";
    }

    Event event;
    event.id = "rl:community-upload:status";
    event.setInt("requested", requested ? 1 : 0);
    event.setInt("effective", effective ? 1 : 0);
    event.setInt("configured", configured ? 1 : 0);
    event.setInt("storage_available", storageAvailable ? 1 : 0);
    event.setInt("pending_count", stats.pending);
    event.setInt("failed_count", stats.failed);
    event.setInt("rejected_count", stats.rejected);
    event.setString("storage_backend", storageAvailable ? String("device") : String(""));
    event.setString("status", status);
    const String error = getLastError();
    event.setString("summary", error.isEmpty() ? summary : summary + " (" + error + ")");
    pluginManager->trigger(event);
    lastStatusMs = millis();
}
