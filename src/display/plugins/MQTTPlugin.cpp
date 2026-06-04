#include "MQTTPlugin.h"
#include "../core/Controller.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <display/core/ProfileManager.h>
#include <display/models/profile.h>
#include <display/util/PsramAllocator.h>
#include <esp_log.h>

const String LOG_TAG = F("MQTTPlugin");
constexpr const char *FLOW_CALIBRATION_URL = "https://ggazzo.github.io/gaggimate-pump-flow-calibration/";
constexpr float MAX_CANONICAL_FLOW_G_PER_S = 20.0f;

static void addCsvTags(JsonDocument &doc, const char *field, const String &csvTags) {
    JsonArray tags = doc[field].to<JsonArray>();
    int start = 0;
    while (start < csvTags.length()) {
        int end = csvTags.indexOf(',', start);
        if (end < 0) {
            end = csvTags.length();
        }
        String tag = csvTags.substring(start, end);
        tag.trim();
        if (!tag.isEmpty()) {
            tags.add(tag);
        }
        start = end + 1;
    }
}

static void addTasteTags(JsonDocument &doc, const String &csvTags) { addCsvTags(doc, "taste_tags", csvTags); }

bool MQTTPlugin::connect(Controller *controller) {
    const Settings settings = controller->getSettings();
    const String ip = settings.getHomeAssistantIP();
    const int haPort = settings.getHomeAssistantPort();
    const String clientId = "GaggiMate";
    const String haUser = settings.getHomeAssistantUser();
    const String haPassword = settings.getHomeAssistantPassword();

    client.begin(ip.c_str(), haPort, net);
    client.setKeepAlive(10);
    ESP_LOGI(LOG_TAG.c_str(), "Connecting to %s:%d", ip.c_str(), haPort);
    for (int i = 0; i < MQTT_CONNECTION_RETRIES; i++) {
        ESP_LOGD(LOG_TAG.c_str(), "Attempt (%d/%d)", i + 1, MQTT_CONNECTION_RETRIES);
        if (client.connect(clientId.c_str(), haUser.c_str(), haPassword.c_str())) {
            ESP_LOGI(LOG_TAG.c_str(), "Successfully connected");
            if (settings.isRLRatingEnabled()) {
                client.subscribe("gaggimate/" + machineTopicId() + "/rl/recommendation");
                client.subscribe("gaggimate/" + machineTopicId() + "/rl/status");
            }
            return true;
        }
        delay(MQTT_CONNECTION_DELAY);
    }
    ESP_LOGW(LOG_TAG.c_str(), "Connection failed");
    return false;
}

void MQTTPlugin::publishDiscovery(Controller *controller) {
    if (!client.connected())
        return;
    const Settings settings = controller->getSettings();
    const String haTopic = settings.getHomeAssistantTopic();
    String mac = machineTopicId();
    const char *cmac = mac.c_str();

    JsonDocument device(&psramAllocator);
    JsonDocument origin(&psramAllocator);
    JsonDocument components(&psramAllocator);

    device["ids"] = cmac;
    device["name"] = "GaggiMate";
    device["mf"] = "GaggiMate";
    device["mdl"] = "GaggiMate";
    device["sn"] = cmac;
    device["sw"] = controller->getSystemInfo().version;
    device["hw"] = controller->getSystemInfo().hardware;

    origin["name"] = "GaggiMate";
    origin["sw"] = controller->getSystemInfo().version;
    origin["url"] = "https://gaggimate.eu/";

    // Components information
    JsonDocument cmps(&psramAllocator);
    JsonDocument boilerTemperature(&psramAllocator);
    JsonDocument boilerTargetTemperature(&psramAllocator);
    JsonDocument mode(&psramAllocator);

    boilerTemperature["name"] = "Boiler Temperature";
    boilerTemperature["p"] = "sensor";
    boilerTemperature["device_class"] = "temperature";
    boilerTemperature["unit_of_measurement"] = "°C";
    boilerTemperature["value_template"] = "{{ value_json.temperature | round(2) }}";
    boilerTemperature["unique_id"] = "boiler0Tmp";
    boilerTemperature["state_topic"] = "gaggimate/" + String(cmac) + "/boilers/0/temperature";

    boilerTargetTemperature["name"] = "Boiler Target Temperature";
    boilerTargetTemperature["p"] = "sensor";
    boilerTargetTemperature["device_class"] = "temperature";
    boilerTargetTemperature["unit_of_measurement"] = "°C";
    boilerTargetTemperature["value_template"] = "{{ value_json.temperature | round(2) }}";
    boilerTargetTemperature["unique_id"] = "boiler0TargetTmp";
    boilerTargetTemperature["state_topic"] = "gaggimate/" + String(cmac) + "/boilers/0/targetTemperature";

    mode["name"] = "Mode";
    mode["p"] = "text";
    mode["device_class"] = "text";
    mode["value_template"] = "{{ value_json.mode_str }}";
    mode["unique_id"] = "mode";
    mode["state_topic"] = "gaggimate/" + String(cmac) + "/controller/mode";

    cmps["boiler"] = boilerTemperature;
    cmps["boiler_target"] = boilerTargetTemperature;
    cmps["mode"] = mode;

    // Prepare the payload for Home Assistant discovery
    JsonDocument payload(&psramAllocator);
    payload["dev"] = device;
    payload["o"] = origin;
    payload["cmps"] = cmps;
    payload["state_topic"] = "gaggimate/" + String(cmac) + "/state";
    payload["qos"] = 2;

    char publishTopic[80];
    snprintf(publishTopic, sizeof(publishTopic), "%s/device/%s/config", haTopic.c_str(), cmac);

    String payloadStr;
    serializeJson(payload, payloadStr);

    ESP_LOGD(LOG_TAG.c_str(), "Publishing discovery %s: %s", publishTopic, payloadStr.c_str());
    client.publish(publishTopic, payloadStr);
}

void MQTTPlugin::publish(const std::string &topic, const std::string &message) {
    if (!client.connected())
        return;
    String mac = machineTopicId();
    const char *cmac = mac.c_str();
    char publishTopic[80];
    snprintf(publishTopic, sizeof(publishTopic), "gaggimate/%s/%s", cmac, topic.c_str());

    ESP_LOGD(LOG_TAG.c_str(), "Publishing %s: %s", publishTopic, message.c_str());
    client.publish(publishTopic, message.c_str());
}

void MQTTPlugin::publishBrewState(const char *state) {
    char json[100];
    std::time_t now = std::time(nullptr);
    snprintf(json, sizeof(json), R"({"state":"%s","timestamp":%ld})", state, now);
    publish("controller/brew/state", json);
}

void MQTTPlugin::publishMachineState(const char *state) {
    if (!isAutoTuningParticipating())
        return;

    JsonDocument doc;
    doc["event_type"] = "machine_state";
    doc["schema_version"] = 1;
    doc["machine_id"] = machineId();
    doc["machine_adapter"] = "gaggimate";
    doc["timestamp"] = static_cast<long>(std::time(nullptr));
    doc["state"] = state;
    doc["local_optimization_enabled"] = localOptimizationEnabled();
    addRecipeMetadata(doc);

    String json;
    serializeJson(doc, json);
    publish("machine/state", json.c_str());
}

void MQTTPlugin::loop() {
    client.loop();
    if (!isBrewing)
        return;
    if (!isAutoTuningParticipating()) {
        resetShotCapture();
        return;
    }
    unsigned long elapsed = millis() - brewStartMs;
    if (elapsed - lastSampleMs >= SHOT_SAMPLE_INTERVAL_MS && pressureSamples.size() < SHOT_MAX_SAMPLES) {
        recordShotSample();
        lastSampleMs = elapsed;
    }
}

void MQTTPlugin::resetShotCapture() {
    isBrewing = false;
    brewStartMs = 0;
    lastSampleMs = 0;
    currentShotId = "";
    pressureSamples.clear();
    targetPressureSamples.clear();
    flowSamples.clear();
    pumpFlowSamples.clear();
    targetFlowSamples.clear();
    weightSamples.clear();
    timeSamples.clear();
}

void MQTTPlugin::recordShotSample() {
    const uint16_t elapsedMs = static_cast<uint16_t>(millis() - brewStartMs);
    const float shotWeightG = currentShotWeightG();
    const float beverageFlowGPerS = currentShotFlowGPerS(shotWeightG, elapsedMs);

    pressureSamples.push_back(controller->getCurrentPressure());
    targetPressureSamples.push_back(controller->getTargetPressure());
    flowSamples.push_back(beverageFlowGPerS);
    pumpFlowSamples.push_back(controller->getCurrentPumpFlow());
    targetFlowSamples.push_back(controller->getTargetFlow());
    weightSamples.push_back(shotWeightG);
    timeSamples.push_back(elapsedMs);
}

void MQTTPlugin::publishShotProfile() {
    if (!isAutoTuningParticipating())
        return;
    if (pressureSamples.empty())
        return;

    const float beverageOutG = currentShotWeightG();
    if (!weightSamples.empty()) {
        weightSamples.back() = beverageOutG;
    }

    JsonDocument doc;
    doc["event_type"] = "shot_profile";
    doc["schema_version"] = 1;
    doc["shot_id"] = currentShotId.isEmpty() ? makeShotId() : currentShotId;
    doc["machine_id"] = machineId();
    doc["machine_adapter"] = "gaggimate";
    doc["timestamp"] = static_cast<long>(std::time(nullptr));
    doc["n_samples"] = pressureSamples.size();
    doc["shot_type"] = "espresso";
    doc["utility"] = false;
    doc["exclude_from_local_optimization"] = false;
    doc["local_optimization_enabled"] = true;
    doc["optimization_weight"] = 1.0f;
    doc["rating_prompt_allowed"] = true;
    doc["weight_source"] = weightSourceName();
    doc["flow_source"] = flowSourceName();
    doc["flow_units"] = "g_per_s";
    doc["pump_flow_source"] = "gaggimate_pump_model";
    doc["pump_flow_units"] = "ml_per_s";
    doc["pump_flow_interpretation"] = "available pump flow at current pressure scaled by pump duty";
    doc["pump_flow_calibration_required"] = pumpFlowCalibrationRequired();
    if (pumpFlowCalibrationRequired()) {
        doc["pump_flow_calibration_url"] = FLOW_CALIBRATION_URL;
        doc["predictive_weight_interpretation"] = "estimated beverage output from pump/puck model";
    }
    doc["beverage_out_g"] = roundf(beverageOutG * 10.0f) / 10.0f;
    doc["shot_time_s"] = roundf(((millis() - brewStartMs) / 1000.0f) * 10.0f) / 10.0f;
    addRecipeMetadata(doc);

    if (hasRecommendation) {
        doc["recommendation_id"] = latestRecommendationId;
        doc["recommended_grind_delta_steps"] = latestRecommendationGrindDeltaSteps;
        doc["recommended_grind_delta_um"] = latestRecommendationGrindDeltaUm;
        doc["recommended_next_grind_steps"] = latestRecommendationNextGrindSteps;
        doc["recommended_next_grind_um"] = latestRecommendationNextGrindUm;
        doc["recommended_dose_g"] = latestRecommendationNextDoseG;
        doc["recommended_target_yield_g"] = latestRecommendationTargetYieldG;
        doc["recommended_target_ratio"] = latestRecommendationTargetRatio;
    }

    JsonArray p = doc["pressure"].to<JsonArray>();
    JsonArray tp = doc["target_pressure"].to<JsonArray>();
    JsonArray f = doc["flow"].to<JsonArray>();
    JsonArray pf = doc["pump_flow"].to<JsonArray>();
    JsonArray tf = doc["target_flow"].to<JsonArray>();
    JsonArray w = doc["weight"].to<JsonArray>();
    JsonArray t = doc["time_ms"].to<JsonArray>();

    for (size_t i = 0; i < pressureSamples.size(); i++) {
        p.add(roundf(pressureSamples[i] * 10.0f) / 10.0f);
        tp.add(roundf(targetPressureSamples[i] * 10.0f) / 10.0f);
        f.add(roundf(flowSamples[i] * 100.0f) / 100.0f);
        pf.add(roundf(pumpFlowSamples[i] * 100.0f) / 100.0f);
        tf.add(roundf(targetFlowSamples[i] * 100.0f) / 100.0f);
        w.add(roundf(weightSamples[i] * 10.0f) / 10.0f);
        t.add(timeSamples[i]);
    }

    String json;
    serializeJson(doc, json);
    publish("shot/profile", json.c_str());
}

void MQTTPlugin::handleRecommendation(const String &payload) {
    if (!pluginManager || !isAutoTuningParticipating()) {
        clearLatestRecommendation();
        return;
    }
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error)
        return;

    hasRecommendation = true;
    latestRecommendationId = doc["recommendation_id"].as<String>();
    latestRecommendationSourceShotId = doc["shot_id"].as<String>();
    latestRecommendationGrindDeltaSteps = doc["grind_delta_steps"] | 0;
    latestRecommendationGrindDeltaUm = doc["grind_delta_um"] | 0.0f;
    latestRecommendationNextGrindSteps = doc["next_grind_steps"] | 0.0f;
    latestRecommendationNextGrindUm = doc["next_grind_um"] | 0.0f;
    latestRecommendationNextDoseG = doc["next_dose_g"] | 0.0f;
    latestRecommendationTargetYieldG = doc["target_yield_g"] | 0.0f;
    latestRecommendationTargetRatio = doc["target_ratio"] | 0.0f;
    latestRecommendationStatus = doc["status"].as<String>();

    Event event;
    event.id = "rl:recommendation:received";
    event.setString("shot_id", latestRecommendationSourceShotId);
    event.setString("recommendation_id", latestRecommendationId);
    event.setInt("grind_delta_steps", latestRecommendationGrindDeltaSteps);
    event.setFloat("grind_delta_um", latestRecommendationGrindDeltaUm);
    event.setFloat("next_grind_steps", latestRecommendationNextGrindSteps);
    event.setFloat("next_grind_um", latestRecommendationNextGrindUm);
    event.setFloat("next_dose_g", latestRecommendationNextDoseG);
    event.setFloat("target_yield_g", latestRecommendationTargetYieldG);
    event.setFloat("target_ratio", latestRecommendationTargetRatio);
    event.setString("status", latestRecommendationStatus);
    event.setString("mode", doc["mode"].as<String>());
    pluginManager->trigger(event);
}

void MQTTPlugin::handleStatus(const String &payload) {
    if (!pluginManager || !isAutoTuningEnabled())
        return;
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error)
        return;

    latestStatusSeen = true;
    latestAddonOnline = doc["addon_online"] | false;
    latestStatusTimestamp = doc["timestamp"] | 0;
    latestStatusLastShotId = doc["last_shot_id"].as<String>();
    latestStatusLastShotAt = doc["last_shot_at"] | 0;
    latestStatusLastShotType = doc["last_shot_type"].as<String>();
    latestStatusLastShotTimeS = doc["last_shot_time_s"] | 0.0f;
    latestStatusLastShotBeverageOutG = doc["last_shot_beverage_out_g"] | 0.0f;
    latestStatusLastShotTargetYieldG = doc["last_shot_target_yield_g"] | 0.0f;
    latestStatusLastShotHumanRating = doc["last_shot_human_rating"] | 0;
    latestStatusLastRecommendationId = doc["last_recommendation_id"].as<String>();
    latestStatusLastRecommendationAt = doc["last_recommendation_at"] | 0;
    latestStatusRecommendationApplyStatus = doc["recommendation_apply_status"].as<String>();
    latestStatusMode = doc["mode"].as<String>();
    latestStatusLocalShotCount = doc["local_shot_count"] | 0;
    latestStatusRatedShotCount = doc["rated_shot_count"] | 0;
    latestStatusUploadQueueCount = doc["upload_queue_count"] | 0;
    latestStatusUploadQueueRejectedCount = doc["upload_queue_rejected_count"] | 0;
    latestStatusUploadQueueLastRejectedId = doc["upload_queue_last_rejected_id"].as<String>();
    latestStatusUploadQueueLastRejectedRecordId = doc["upload_queue_last_rejected_record_id"].as<String>();
    latestStatusUploadQueueLastRejectedError = doc["upload_queue_last_rejected_error"].as<String>();
    latestStatusCommunityUploadEnabled = doc["community_upload_enabled"] | false;
    latestStatusBestKnownRecipe = "";
    if (doc["best_known_recipe"].is<JsonObject>()) {
        JsonObject best = doc["best_known_recipe"].as<JsonObject>();
        float dose = best["dose_g"] | 0.0f;
        float yield = best["target_yield_g"] | 0.0f;
        float grind = best["grind_steps"] | 0.0f;
        int rating = best["rating"] | 0;
        char buffer[96];
        if (rating > 0) {
            snprintf(buffer, sizeof(buffer), "%.1fg in / %.1fg out / grind %.1f / %d stars", dose, yield, grind, rating);
        } else {
            snprintf(buffer, sizeof(buffer), "%.1fg in / %.1fg out / grind %.1f", dose, yield, grind);
        }
        latestStatusBestKnownRecipe = buffer;
    }

    Event event;
    event.id = "rl:status:received";
    event.setInt("seen", latestStatusSeen ? 1 : 0);
    event.setInt("addon_online", latestAddonOnline ? 1 : 0);
    event.setInt("timestamp", static_cast<int>(latestStatusTimestamp));
    event.setString("last_shot_id", latestStatusLastShotId);
    event.setInt("last_shot_at", static_cast<int>(latestStatusLastShotAt));
    event.setString("last_shot_type", latestStatusLastShotType);
    event.setInt("last_shot_time_s_x10", static_cast<int>(latestStatusLastShotTimeS * 10.0f));
    event.setInt("last_shot_beverage_out_g_x10", static_cast<int>(latestStatusLastShotBeverageOutG * 10.0f));
    event.setInt("last_shot_target_yield_g_x10", static_cast<int>(latestStatusLastShotTargetYieldG * 10.0f));
    event.setInt("last_shot_human_rating", latestStatusLastShotHumanRating);
    event.setString("last_recommendation_id", latestStatusLastRecommendationId);
    event.setInt("last_recommendation_at", static_cast<int>(latestStatusLastRecommendationAt));
    event.setString("recommendation_apply_status", latestStatusRecommendationApplyStatus);
    event.setString("mode", latestStatusMode);
    event.setInt("local_shot_count", latestStatusLocalShotCount);
    event.setInt("rated_shot_count", latestStatusRatedShotCount);
    event.setInt("upload_queue_count", latestStatusUploadQueueCount);
    event.setInt("upload_queue_rejected_count", latestStatusUploadQueueRejectedCount);
    event.setString("upload_queue_last_rejected_id", latestStatusUploadQueueLastRejectedId);
    event.setString("upload_queue_last_rejected_record_id", latestStatusUploadQueueLastRejectedRecordId);
    event.setString("upload_queue_last_rejected_error", latestStatusUploadQueueLastRejectedError);
    event.setInt("community_upload_enabled", latestStatusCommunityUploadEnabled ? 1 : 0);
    event.setString("best_known_recipe", latestStatusBestKnownRecipe);
    pluginManager->trigger(event);
}

void MQTTPlugin::applyLatestRecommendation() {
    if (!hasRecommendation || !controller || !isAutoTuningParticipating())
        return;

    bool doseApplied = false;
    bool yieldApplied = false;
    bool yieldFailed = false;

    if (latestRecommendationNextDoseG > 0.0f && canApplyGrindByWeightTarget()) {
        controller->setTargetGrindVolume(latestRecommendationNextDoseG);
        doseApplied = true;
    }

    if (latestRecommendationTargetYieldG > 0.0f && controller->getProfileManager()) {
        Profile &profile = controller->getProfileManager()->getSelectedProfile();
        float currentTarget = profile.getTotalVolume();
        if (currentTarget > 0.0f) {
            profile.adjustVolumetricTarget(latestRecommendationTargetYieldG - currentTarget);
            controller->getProfileManager()->saveProfile(profile);
            yieldApplied = true;
        } else {
            yieldFailed = true;
        }
    } else if (latestRecommendationTargetYieldG > 0.0f) {
        yieldFailed = true;
    }

    publishRecommendationDecision("accepted", true);
    publishRecommendationApply(doseApplied, yieldApplied, yieldFailed);
    publishMachineState("idle");
}

void MQTTPlugin::ignoreLatestRecommendation() {
    if (!hasRecommendation || !isAutoTuningParticipating())
        return;
    publishRecommendationDecision("ignored", false);
    clearLatestRecommendation();
    publishMachineState("idle");
}

void MQTTPlugin::clearLatestRecommendation() {
    hasRecommendation = false;
    latestRecommendationId = "";
    latestRecommendationSourceShotId = "";
    latestRecommendationGrindDeltaSteps = 0;
    latestRecommendationGrindDeltaUm = 0.0f;
    latestRecommendationNextGrindSteps = 0.0f;
    latestRecommendationNextGrindUm = 0.0f;
    latestRecommendationNextDoseG = 0.0f;
    latestRecommendationTargetYieldG = 0.0f;
    latestRecommendationTargetRatio = 0.0f;
    latestRecommendationStatus = "";
}

void MQTTPlugin::publishRecommendationDecision(const char *decision, bool includeEditedFields) {
    if (!isAutoTuningParticipating() || latestRecommendationId.isEmpty())
        return;

    JsonDocument doc;
    doc["event_type"] = "recommendation_decision";
    doc["schema_version"] = 1;
    doc["recommendation_id"] = latestRecommendationId;
    doc["decision"] = decision;
    doc["source"] = "gaggimate_mqtt";
    doc["timestamp"] = static_cast<long>(std::time(nullptr));

    JsonObject edited = doc["edited_fields"].to<JsonObject>();
    if (includeEditedFields) {
        edited["next_dose_g"] = latestRecommendationNextDoseG;
        edited["target_yield_g"] = latestRecommendationTargetYieldG;
    }

    String json;
    serializeJson(doc, json);
    publish("rl/recommendation/decision", json.c_str());
}

void MQTTPlugin::publishRecommendationApply(bool doseApplied, bool yieldApplied, bool yieldFailed) {
    if (!isAutoTuningParticipating() || latestRecommendationId.isEmpty())
        return;

    const bool grindManual = latestRecommendationGrindDeltaSteps != 0;
    const bool doseManual = latestRecommendationNextDoseG > 0.0f && !doseApplied;
    const bool hasManual = grindManual || doseManual;
    const bool hasApplied = doseApplied || yieldApplied;

    const char *status = "manual_required";
    if (yieldFailed && !hasApplied) {
        status = "failed";
    } else if (hasApplied && (hasManual || yieldFailed)) {
        status = "partially_applied";
    } else if (hasApplied) {
        status = "applied";
    }

    JsonDocument doc;
    doc["event_type"] = "recommendation_apply";
    doc["schema_version"] = 1;
    doc["recommendation_id"] = latestRecommendationId;
    doc["machine_id"] = machineId();
    doc["status"] = status;
    doc["source"] = "gaggimate_mqtt";
    doc["timestamp"] = static_cast<long>(std::time(nullptr));

    JsonObject applied = doc["applied_fields"].to<JsonObject>();
    if (doseApplied) {
        applied["next_dose_g"] = latestRecommendationNextDoseG;
    }
    if (yieldApplied) {
        applied["target_yield_g"] = latestRecommendationTargetYieldG;
        applied["target_ratio"] = latestRecommendationTargetRatio;
    }

    JsonArray manual = doc["manual_fields"].to<JsonArray>();
    if (grindManual) {
        manual.add("next_grind_steps");
    }
    if (doseManual) {
        manual.add("next_dose_g");
    }

    JsonObject failed = doc["failed_fields"].to<JsonObject>();
    if (yieldFailed) {
        failed["target_yield_g"] = latestRecommendationTargetYieldG;
    }

    if (yieldFailed && !hasApplied) {
        doc["message"] = "Target yield could not be applied; use the recommendation manually.";
    } else if (hasManual) {
        doc["message"] = "Some recommendation fields require manual action.";
    } else {
        doc["message"] = "Recommendation fields applied.";
    }

    String json;
    serializeJson(doc, json);
    publish("rl/recommendation/apply", json.c_str());
}

void MQTTPlugin::publishShotCorrection(Event const &event) {
    if (!isAutoTuningParticipating())
        return;

    String shotId = event.getString("shot_id");
    if (shotId.isEmpty()) {
        shotId = latestStatusLastShotId;
    }
    if (shotId.isEmpty()) {
        return;
    }

    JsonDocument doc;
    doc["event_type"] = "shot_correction";
    doc["schema_version"] = 1;
    doc["shot_id"] = shotId;
    doc["machine_id"] = machineId();
    doc["source"] = event.getString("source").isEmpty() ? "gaggimate_mqtt" : event.getString("source");
    doc["timestamp"] = static_cast<long>(std::time(nullptr));

    if (event.getInt("has_exclude_from_local_optimization") == 1) {
        doc["exclude_from_local_optimization"] = event.getInt("exclude_from_local_optimization") == 1;
    }
    if (!event.getString("shot_type").isEmpty()) {
        doc["shot_type"] = event.getString("shot_type");
    }
    if (event.getInt("has_grind_followed") == 1) {
        doc["grind_followed"] = event.getInt("grind_followed") == 1;
    }
    if (event.getInt("has_dose_followed") == 1) {
        doc["dose_followed"] = event.getInt("dose_followed") == 1;
    }
    if (event.getInt("has_yield_followed") == 1) {
        doc["yield_followed"] = event.getInt("yield_followed") == 1;
    }
    addCsvTags(doc, "correction_tags", event.getString("correction_tags"));

    String json;
    serializeJson(doc, json);
    publish("rl/shot/correction", json.c_str());
}

void MQTTPlugin::publishUploadRequeue(Event const &event) {
    if (!isAutoTuningParticipating())
        return;

    JsonDocument doc;
    doc["event_type"] = "upload_queue_maintenance";
    doc["schema_version"] = 1;
    doc["machine_id"] = machineId();
    String action = event.getString("action");
    if (action.isEmpty()) {
        action = "requeue_valid_rejected";
    }
    if (action != "requeue_valid_rejected" && action != "purge_rejected") {
        return;
    }
    doc["action"] = action;
    doc["limit"] = event.getInt("limit") > 0 ? event.getInt("limit") : 50;
    doc["source"] = event.getString("source").isEmpty() ? "gaggimate_mqtt" : event.getString("source");
    doc["timestamp"] = static_cast<long>(std::time(nullptr));
    addRecipeMetadata(doc);

    String json;
    serializeJson(doc, json);
    publish("rl/upload/requeue", json.c_str());
}

void MQTTPlugin::addRecipeMetadata(JsonDocument &doc) const {
    const float dose = doseTargetG();
    const float targetYield = targetYieldG();

    doc["bean_context_id"] = beanContextId();
    doc["bean_context_name"] = beanContextName();
    if (dose > 0.0f) {
        doc["dose_in_g"] = roundf(dose * 10.0f) / 10.0f;
    }
    if (targetYield > 0.0f) {
        doc["target_yield_g"] = roundf(targetYield * 10.0f) / 10.0f;
    }
    if (dose > 0.0f && targetYield > 0.0f) {
        doc["target_ratio"] = roundf((targetYield / dose) * 100.0f) / 100.0f;
    }
}

bool MQTTPlugin::isAutoTuningEnabled() const {
    if (!controller)
        return false;
    Settings const &settings = controller->getSettings();
    return settings.isHomeAssistant() && settings.isRLRatingEnabled();
}

bool MQTTPlugin::isAutoTuningParticipating() const { return isAutoTuningEnabled() && localOptimizationEnabled(); }

bool MQTTPlugin::canApplyGrindByWeightTarget() const {
    if (!controller)
        return false;
    Settings const &settings = controller->getSettings();
    return controller->isGrindVolumetricAvailable() && settings.isVolumetricTarget();
}

String MQTTPlugin::machineTopicId() const {
    String mac = WiFi.macAddress();
    mac.replace(":", "_");
    return mac;
}

String MQTTPlugin::machineId() const { return "gaggimate:" + machineTopicId(); }

String MQTTPlugin::beanContextId() const {
    if (!controller)
        return "";
    return controller->getSettings().getRLBeanContextId();
}

String MQTTPlugin::beanContextName() const {
    if (!controller)
        return "";
    return controller->getSettings().getRLBeanContextName();
}

bool MQTTPlugin::localOptimizationEnabled() const {
    if (!controller)
        return false;
    Settings const &settings = controller->getSettings();
    return settings.isRLLocalOptimizationEnabled() && !settings.isRLOptimizationPaused() &&
           !settings.getRLBeanContextId().isEmpty();
}

String MQTTPlugin::makeShotId() const {
    return "shot_" + machineTopicId() + "_" + String(static_cast<unsigned long>(std::time(nullptr))) + "_" +
           String(static_cast<unsigned long>(millis()));
}

float MQTTPlugin::targetYieldG() const {
    if (!controller || !controller->getProfileManager())
        return 0.0f;
    return controller->getProfileManager()->getSelectedProfile().getTotalVolume();
}

float MQTTPlugin::doseTargetG() const {
    if (!controller)
        return 0.0f;
    return static_cast<float>(controller->getSettings().getTargetGrindVolume());
}

float MQTTPlugin::currentShotWeightG() const {
    if (!controller)
        return currentBluetoothWeight;

    switch (static_cast<VolumetricMeasurementSource>(shotSource)) {
    case VolumetricMeasurementSource::HARDWARE_SCALE:
        return currentHardwareShotWeight > 0.0f ? currentHardwareShotWeight : currentHardwareWeight;
    case VolumetricMeasurementSource::FLOW_ESTIMATION:
        return currentEstimatedWeight;
    case VolumetricMeasurementSource::BLUETOOTH:
        return currentBluetoothWeight;
    case VolumetricMeasurementSource::INACTIVE:
    default:
        return currentBluetoothWeight > 0.0f ? currentBluetoothWeight : currentEstimatedWeight;
    }
}

float MQTTPlugin::currentShotFlowGPerS(float currentWeightG, uint16_t elapsedMs) const {
    if (weightSamples.empty() || timeSamples.empty()) {
        return 0.0f;
    }
    const uint16_t previousMs = timeSamples.back();
    if (elapsedMs <= previousMs) {
        return 0.0f;
    }
    const float dtS = static_cast<float>(elapsedMs - previousMs) / 1000.0f;
    if (dtS <= 0.0f || !std::isfinite(dtS) || !std::isfinite(currentWeightG)) {
        return 0.0f;
    }
    const float flow = (currentWeightG - weightSamples.back()) / dtS;
    if (!std::isfinite(flow) || flow <= 0.0f) {
        return 0.0f;
    }
    return std::min(flow, MAX_CANONICAL_FLOW_G_PER_S);
}

const char *MQTTPlugin::weightSourceName() const {
    switch (static_cast<VolumetricMeasurementSource>(shotSource)) {
    case VolumetricMeasurementSource::HARDWARE_SCALE:
        return "hardware_scale";
    case VolumetricMeasurementSource::BLUETOOTH:
        return "bluetooth_scale";
    case VolumetricMeasurementSource::FLOW_ESTIMATION:
        return "predictive_pump_flow";
    case VolumetricMeasurementSource::INACTIVE:
    default:
        return "unknown";
    }
}

const char *MQTTPlugin::flowSourceName() const {
    switch (static_cast<VolumetricMeasurementSource>(shotSource)) {
    case VolumetricMeasurementSource::HARDWARE_SCALE:
    case VolumetricMeasurementSource::BLUETOOTH:
        return "beverage_weight_derivative";
    case VolumetricMeasurementSource::FLOW_ESTIMATION:
        return "predictive_pump_model_derivative";
    case VolumetricMeasurementSource::INACTIVE:
    default:
        return "unknown";
    }
}

bool MQTTPlugin::pumpFlowCalibrationRequired() const {
    return static_cast<VolumetricMeasurementSource>(shotSource) == VolumetricMeasurementSource::FLOW_ESTIMATION;
}

void MQTTPlugin::setup(Controller *ctrl, PluginManager *pm) {
    this->controller = ctrl;
    this->pluginManager = pm;

    client.onMessage([this](String &topic, String &payload) {
        if (topic.endsWith("/rl/recommendation")) {
            handleRecommendation(payload);
        } else if (topic.endsWith("/rl/status")) {
            handleStatus(payload);
        }
    });

    pm->on("rl:rating", [this](Event const &event) {
        if (!isAutoTuningParticipating())
            return;
        String shotId = event.getString("shot_id");
        String recommendationId = event.getString("recommendation_id");
        int rating = event.getInt("rating");
        bool skipped = event.getInt("skipped") == 1 || rating < 1 || rating > 5;

        JsonDocument doc;
        doc["event_type"] = "shot_feedback";
        doc["schema_version"] = 1;
        doc["shot_id"] = shotId;
        doc["recommendation_id"] = recommendationId;
        doc["machine_id"] = machineId();
        if (!skipped) {
            doc["rating"] = rating;
        }
        doc["skipped"] = skipped;
        addTasteTags(doc, event.getString("taste_tags"));
        doc["source"] = "gaggimate_mqtt";
        doc["timestamp"] = static_cast<long>(std::time(nullptr));

        String json;
        serializeJson(doc, json);
        publish("rl/rating", json.c_str());
    });

    pm->on("rl:recommendation:apply", [this](Event const &) { applyLatestRecommendation(); });

    pm->on("rl:recommendation:ignore", [this](Event const &) { ignoreLatestRecommendation(); });

    pm->on("rl:shot:correction", [this](Event const &event) { publishShotCorrection(event); });

    pm->on("rl:upload:requeue", [this](Event const &event) { publishUploadRequeue(event); });

    pm->on("rl:settings:changed", [this](Event const &) {
        if (!isAutoTuningParticipating()) {
            clearLatestRecommendation();
            resetShotCapture();
            return;
        }
        publishMachineState("idle");
    });

    pm->on("controller:wifi:connect", [this, ctrl](const Event &) {
        if (!connect(ctrl))
            return;
        publishDiscovery(ctrl);
        publishMachineState(ctrl->getMode() == MODE_STANDBY ? "standby" : "idle");
    });

    pm->on("boiler:currentTemperature:change", [this](Event const &event) {
        if (!client.connected())
            return;
        char json[50];
        const float temp = event.getFloat("value");
        if (temp != lastTemperature) {
            snprintf(json, sizeof(json), R"***({"temperature":%02f})***", temp);
            publish("boilers/0/temperature", json);
        }
        lastTemperature = temp;
    });

    pm->on("boiler:targetTemperature:change", [this](Event const &event) {
        if (!client.connected())
            return;
        char json[50];
        const float temp = event.getFloat("value");
        snprintf(json, sizeof(json), R"***({"temperature":%02f})***", temp);
        publish("boilers/0/targetTemperature", json);
    });

    pm->on("controller:mode:change", [this](Event const &event) {
        int newMode = event.getInt("value");
        const char *modeStr;
        switch (newMode) {
        case 0:
            modeStr = "Standby";
            break;
        case 1:
            modeStr = "Brew";
            break;
        case 2:
            modeStr = "Steam";
            break;
        case 3:
            modeStr = "Water";
            break;
        case 4:
            modeStr = "Grind";
            break;
        default:
            modeStr = "Unknown";
            break;
        }
        char json[100];
        snprintf(json, sizeof(json), R"({"mode":%d,"mode_str":"%s"})", newMode, modeStr);
        publish("controller/mode", json);
        publishMachineState(newMode == MODE_STANDBY ? "standby" : "idle");
    });

    pm->on("controller:volumetric-measurement:estimation:change",
           [this](Event const &event) { currentEstimatedWeight = event.getFloat("value"); });
    pm->on("controller:volumetric-measurement:bluetooth:change",
           [this](Event const &event) { currentBluetoothWeight = event.getFloat("value"); });
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    pm->on("controller:volumetric-measurement:hardware:change",
           [this](Event const &event) { currentHardwareWeight = event.getFloat("value"); });
    pm->on("controller:volumetric-measurement:hardware-shot:change",
           [this](Event const &event) { currentHardwareShotWeight = event.getFloat("value"); });
#endif

    pm->on("controller:brew:start", [this](Event const &event) {
        // A flush (utility cycle) is not a shot — never capture it for EspressoRL.
        if (isAutoTuningParticipating() && event.getInt("utility") == 0) {
            resetShotCapture();
            isBrewing = true;
            brewStartMs = millis();
            currentShotId = makeShotId();
            shotSource = static_cast<int>(controller->getCurrentVolumetricSource());
            currentBluetoothWeight = 0.0f;
            currentHardwareWeight = 0.0f;
            currentHardwareShotWeight = 0.0f;
            currentEstimatedWeight = 0.0f;
        }
        publishBrewState("brewing");
        publishMachineState("brewing");
    });

    pm->on("controller:brew:end", [this](Event const &event) {
        // Skip flushes: no shot profile publish and no rl:shot:complete (which would
        // otherwise pop the rating prompt for a cleaning cycle).
        const bool capturedEspressoShot =
            isBrewing && isAutoTuningParticipating() && event.getInt("utility") == 0 && !pressureSamples.empty();
        if (capturedEspressoShot) {
            const String completedShotId = currentShotId;
            isBrewing = false;
            publishShotProfile();
            Event event;
            event.id = "rl:shot:complete";
            event.setString("shot_id", completedShotId);
            event.setString("recommendation_id", latestRecommendationId);
            event.setInt("grind_delta_steps", latestRecommendationGrindDeltaSteps);
            event.setFloat("next_dose_g", latestRecommendationNextDoseG);
            event.setFloat("target_yield_g", latestRecommendationTargetYieldG);
            pluginManager->trigger(event);
        }
        resetShotCapture();
        publishBrewState("not brewing");
        publishMachineState("idle");
    });
}
