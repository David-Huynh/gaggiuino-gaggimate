#include "MQTTPlugin.h"
#include "../core/AutoTuning.h"
#include "../core/Controller.h"
#include "autotuning/AutoTuningJsonCodec.h"
#include "autotuning/AutoTuningPayloadMetadata.h"
#include "autotuning/AutoTuningTasteGoalJson.h"
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <display/core/EpochTime.h>
#include <display/core/ProfileManager.h>
#include <display/models/profile.h>
#include <display/util/AtomicFile.h>
#include <display/util/LittleFSUtil.h>
#include <display/util/PsramAllocator.h>
#include <esp_log.h>

const String LOG_TAG = F("MQTTPlugin");

namespace {
struct MqttOutboxLock {
    SemaphoreHandle_t mutex;
    bool locked;
    explicit MqttOutboxLock(SemaphoreHandle_t value)
        : mutex(value), locked(value && xSemaphoreTakeRecursive(value, portMAX_DELAY) == pdTRUE) {}
    ~MqttOutboxLock() {
        if (locked) {
            xSemaphoreGiveRecursive(mutex);
        }
    }
};

constexpr const char *MQTT_OUTBOX_DIR = "/rlm";
constexpr size_t MAX_MQTT_OUTBOX_ITEMS = 64;
constexpr size_t MAX_MQTT_OUTBOX_BYTES = 128 * 1024;
} // namespace

static bool mqttJsonNumber(JsonVariantConst value) {
    return !value.is<bool>() &&
           (value.is<int>() || value.is<long>() || value.is<long long>() || value.is<float>() || value.is<double>());
}

static String mqttJsonString(JsonVariantConst value) {
    return value.is<const char *>() ? value.as<String>() : String("");
}

struct ActiveGrinderPosition {
    bool relativeAvailable = false;
    float relativeStep = 0.0f;
    std::optional<float> absoluteStep;
    std::optional<float> micronsPerStep;
    float directionSign = 1.0f;
};

static bool closeEnough(float left, float right) {
    const float scale = std::max({1.0f, std::fabs(left), std::fabs(right)});
    return std::fabs(left - right) <= 0.001f * scale;
}

static bool loadActiveGrinderPosition(Settings const &settings, ActiveGrinderPosition &position) {
    const String activeContextId = settings.getRLGrinderContextId();
    if (activeContextId.isEmpty()) {
        return false;
    }
    JsonDocument contexts(&psramAllocator);
    if (deserializeJson(contexts, settings.getRLGrinderContextsJson()) || !contexts.is<JsonArrayConst>()) {
        return false;
    }
    JsonObjectConst active;
    for (JsonObjectConst context : contexts.as<JsonArrayConst>()) {
        if (context["id"].as<String>() == activeContextId) {
            active = context;
            break;
        }
    }
    if (active.isNull()) {
        return false;
    }
    if (mqttJsonNumber(active["current_absolute_step"]) && std::isfinite(active["current_absolute_step"].as<float>())) {
        position.absoluteStep = active["current_absolute_step"].as<float>();
    }
    if (position.absoluteStep.has_value() && mqttJsonNumber(active["absolute_reference_step"]) &&
        std::isfinite(active["absolute_reference_step"].as<float>())) {
        position.relativeStep = *position.absoluteStep - active["absolute_reference_step"].as<float>();
        position.relativeAvailable = true;
    } else if (mqttJsonNumber(active["current_relative_step"]) && std::isfinite(active["current_relative_step"].as<float>())) {
        position.relativeStep = active["current_relative_step"].as<float>();
        position.relativeAvailable = true;
    }
    if (mqttJsonNumber(active["microns_per_step"]) && std::isfinite(active["microns_per_step"].as<float>()) &&
        active["microns_per_step"].as<float>() > 0.0f) {
        position.micronsPerStep = active["microns_per_step"].as<float>();
    }
    position.directionSign = active["step_direction"].as<String>() == "higher_is_coarser" ? -1.0f : 1.0f;
    return true;
}

static bool validateRecommendationGrindProjection(AutoTuning::Recommendation const &recommendation, Settings const &settings,
                                                  String &reason) {
    if (std::fabs(recommendation.grindDeltaStepsFromCurrent) < 0.001f) {
        return true;
    }
    ActiveGrinderPosition position;
    if (!loadActiveGrinderPosition(settings, position) || !position.relativeAvailable) {
        reason = "active grinder position is unavailable";
        return false;
    }
    if (!closeEnough(position.relativeStep + recommendation.grindDeltaStepsFromCurrent,
                     recommendation.projectedRelativeStepFromReference)) {
        reason = "recommended relative grinder position is inconsistent";
        return false;
    }
    if (recommendation.currentAbsoluteStep.has_value() &&
        (!position.absoluteStep.has_value() || !closeEnough(*position.absoluteStep, *recommendation.currentAbsoluteStep))) {
        reason = "recommendation uses a stale absolute grinder position";
        return false;
    }
    if (recommendation.projectedAbsoluteStep.has_value() &&
        (!position.absoluteStep.has_value() || !closeEnough(*position.absoluteStep + recommendation.grindDeltaStepsFromCurrent,
                                                            *recommendation.projectedAbsoluteStep))) {
        reason = "recommended absolute grinder position is inconsistent";
        return false;
    }
    if (position.micronsPerStep.has_value()) {
        const float micronScale = *position.micronsPerStep * position.directionSign;
        if (!closeEnough(recommendation.grindDeltaStepsFromCurrent * micronScale, recommendation.grindDeltaMicronsFromCurrent) ||
            !closeEnough(recommendation.projectedRelativeStepFromReference * micronScale,
                         recommendation.projectedRelativeMicronsFromReference)) {
            reason = "recommended grinder micron projection is inconsistent";
            return false;
        }
    }
    reason = "";
    return true;
}

static bool mqttJsonEpoch(JsonVariantConst value, EpochTime::Seconds &out) {
    if (value.is<bool>() || !value.is<std::int64_t>()) {
        return false;
    }
    out = value.as<std::int64_t>();
    return true;
}

static EpochTime::Seconds mqttJsonEpochOrZero(JsonVariantConst value) {
    EpochTime::Seconds parsed = 0;
    return mqttJsonEpoch(value, parsed) ? parsed : 0;
}

void MQTTPlugin::markConnected() {
    mqttWasConnected = true;
    nextReconnectAttemptMs = 0;
    reconnectDelayMs = MQTT_RECONNECT_INITIAL_DELAY_MS;
}

void MQTTPlugin::markDisconnected() {
    if (mqttWasConnected) {
        ESP_LOGW(LOG_TAG.c_str(), "MQTT disconnected; reconnect will retry in background");
    }
    mqttWasConnected = false;
    autoTuningSubscribed = false;
    if (reconnectDelayMs == 0) {
        reconnectDelayMs = MQTT_RECONNECT_INITIAL_DELAY_MS;
    }
}

bool MQTTPlugin::connectOnce() {
    const String ip = pendingConnectionHost;
    const int haPort = pendingConnectionPort;
    const String clientId = "GaggiMate";
    const String haUser = pendingConnectionUser;
    const String haPassword = pendingConnectionPassword;

    if (ip.isEmpty()) {
        ESP_LOGW(LOG_TAG.c_str(), "MQTT host is empty; skipping connection attempt");
        return false;
    }

    client.begin(ip.c_str(), haPort, net);
    client.setKeepAlive(10);
    ESP_LOGI(LOG_TAG.c_str(), "Connecting to %s:%d", ip.c_str(), haPort);
    if (!client.connect(clientId.c_str(), haUser.c_str(), haPassword.c_str())) {
        return false;
    }

    ESP_LOGI(LOG_TAG.c_str(), "Successfully connected");
    return true;
}

bool MQTTPlugin::connect(Controller *controller) {
    if (!controller) {
        return false;
    }
    requestReconnect();
    return true;
}

void MQTTPlugin::requestReconnect() {
    if (!controller || connectionTaskHandle == nullptr) {
        return;
    }
    bool expected = false;
    if (!reconnectInProgress.compare_exchange_strong(expected, true)) {
        return;
    }
    Settings const &settings = controller->getSettings();
    pendingConnectionEnabled = settings.isHomeAssistant();
    pendingConnectionHost = settings.getHomeAssistantIP();
    pendingConnectionPort = settings.getHomeAssistantPort();
    pendingConnectionUser = settings.getHomeAssistantUser();
    pendingConnectionPassword = settings.getHomeAssistantPassword();
    xTaskNotifyGive(connectionTaskHandle);
}

void MQTTPlugin::connectionWorkerTask(void *arg) {
    auto *plugin = static_cast<MQTTPlugin *>(arg);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool connected = false;
        if (plugin->pendingConnectionEnabled && WiFi.status() == WL_CONNECTED && plugin->clientMutex &&
            xSemaphoreTake(plugin->clientMutex, portMAX_DELAY) == pdTRUE) {
            connected = plugin->connectOnce();
            xSemaphoreGive(plugin->clientMutex);
        }
        plugin->connectionAttemptSucceeded.store(connected, std::memory_order_relaxed);
        plugin->connectionAttemptComplete.store(true, std::memory_order_release);
    }
}

void MQTTPlugin::refreshAutoTuningSubscriptions() {
    if (!clientMutex || xSemaphoreTake(clientMutex, 0) != pdTRUE) {
        return;
    }
    if (!client.connected()) {
        autoTuningSubscribed = false;
        xSemaphoreGive(clientMutex);
        return;
    }

    const bool shouldSubscribe =
        AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(), this).routeOffBoardTransport();
    const String prefix = "gaggimate/" + machineTopicId();
    if (shouldSubscribe && !autoTuningSubscribed) {
        const bool recommendation = client.subscribe(prefix + "/rl/recommendation", 1);
        const bool status = client.subscribe(prefix + "/rl/status", 1);
        const bool acknowledgement = client.subscribe(prefix + "/rl/shot/ack", 1);
        autoTuningSubscribed = recommendation && status && acknowledgement;
    } else if (!shouldSubscribe && autoTuningSubscribed) {
        client.unsubscribe(prefix + "/rl/recommendation");
        client.unsubscribe(prefix + "/rl/status");
        client.unsubscribe(prefix + "/rl/shot/ack");
        autoTuningSubscribed = false;
    }
    xSemaphoreGive(clientMutex);
}

void MQTTPlugin::handleConnectionReady() {
    if (!connectionAttemptComplete.exchange(false, std::memory_order_acquire)) {
        return;
    }
    const bool connected = connectionAttemptSucceeded.load(std::memory_order_relaxed);
    reconnectInProgress.store(false, std::memory_order_release);
    if (!connected || !configured()) {
        if (connected && clientMutex && xSemaphoreTake(clientMutex, 0) == pdTRUE) {
            client.disconnect();
            xSemaphoreGive(clientMutex);
        }
        markDisconnected();
        return;
    }
    markConnected();
    refreshAutoTuningSubscriptions();
    publishDiscovery(controller);
    publishOptimizerSettings();
    publishMachineState(controller->getMode() == MODE_STANDBY ? "standby" : "idle", true);
}

void MQTTPlugin::maintainConnection() {
    if (!controller) {
        return;
    }
    if (!configured()) {
        if (clientMutex && xSemaphoreTake(clientMutex, 0) == pdTRUE) {
            if (client.connected()) {
                client.disconnect();
            }
            xSemaphoreGive(clientMutex);
        }
        markDisconnected();
        nextReconnectAttemptMs = 0;
        return;
    }
    if (mqttWasConnected) {
        if (clientMutex && xSemaphoreTake(clientMutex, 0) == pdTRUE) {
            const bool stillConnected = client.connected();
            xSemaphoreGive(clientMutex);
            if (!stillConnected) {
                markDisconnected();
            }
        }
        if (mqttWasConnected) {
            return;
        }
    }

    markDisconnected();
    if (WiFi.status() != WL_CONNECTED) {
        nextReconnectAttemptMs = 0;
        return;
    }

    const unsigned long now = millis();
    if (nextReconnectAttemptMs != 0 && static_cast<long>(now - nextReconnectAttemptMs) < 0) {
        return;
    }

    if (reconnectInProgress) {
        return;
    }

    const unsigned long delayMs = reconnectDelayMs == 0 ? MQTT_RECONNECT_INITIAL_DELAY_MS : reconnectDelayMs;
    nextReconnectAttemptMs = now + delayMs;
    reconnectDelayMs = std::min(delayMs * 2, MQTT_RECONNECT_MAX_DELAY_MS);
    requestReconnect();
}

void MQTTPlugin::publishDiscovery(Controller *controller) {
    if (!mqttWasConnected)
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
    boilerTemperature["unit_of_measurement"] = "Â°C";
    boilerTemperature["value_template"] = "{{ value_json.temperature | round(2) }}";
    boilerTemperature["unique_id"] = "boiler0Tmp";
    boilerTemperature["state_topic"] = "gaggimate/" + String(cmac) + "/boilers/0/temperature";

    boilerTargetTemperature["name"] = "Boiler Target Temperature";
    boilerTargetTemperature["p"] = "sensor";
    boilerTargetTemperature["device_class"] = "temperature";
    boilerTargetTemperature["unit_of_measurement"] = "Â°C";
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
    publishNow(publishTopic, payloadStr, false, 0);
}

bool MQTTPlugin::publish(const std::string &topic, const std::string &message, const bool retained, const int qos,
                         const bool durable) {
    String mac = machineTopicId();
    const char *cmac = mac.c_str();
    char publishTopic[80];
    snprintf(publishTopic, sizeof(publishTopic), "gaggimate/%s/%s", cmac, topic.c_str());

    if (durable) {
        if (!enqueueDurablePublish(publishTopic, message.c_str(), retained, qos)) {
            ESP_LOGE(LOG_TAG.c_str(), "Unable to persist MQTT lifecycle event for %s", publishTopic);
            return false;
        }
        return true;
    }
    return publishNow(publishTopic, message.c_str(), retained, qos);
}

bool MQTTPlugin::publishNow(const String &topic, const String &message, const bool retained, const int qos) {
    if (!clientMutex || xSemaphoreTake(clientMutex, 0) != pdTRUE) {
        return false;
    }
    bool published = false;
    if (client.connected()) {
        ESP_LOGD(LOG_TAG.c_str(), "Publishing %s: %s", topic.c_str(), message.c_str());
        published = client.publish(topic, message, retained, std::clamp(qos, 0, 2));
    }
    xSemaphoreGive(clientMutex);
    return published;
}

bool MQTTPlugin::enqueueDurablePublish(const String &topic, const String &message, const bool retained, const int qos) {
    MqttOutboxLock lock(outboxMutex);
    if (!lock.locked) {
        return false;
    }
    if (!LittleFSUtil::existsQuietly(MQTT_OUTBOX_DIR) && !LittleFS.mkdir(MQTT_OUTBOX_DIR)) {
        return false;
    }

    JsonDocument doc;
    doc["topic"] = topic;
    doc["message"] = message;
    doc["retained"] = retained;
    doc["qos"] = std::clamp(qos, 0, 2);
    doc["created_at"] = EpochTime::now();
    const size_t expected = measureJson(doc);

    unsigned long long nextSequence = 1;
    size_t queuedCount = 0;
    size_t queuedBytes = 0;
    File directory = LittleFS.open(MQTT_OUTBOX_DIR);
    if (directory && directory.isDirectory()) {
        File queued = directory.openNextFile();
        while (queued) {
            String name = queued.name();
            const bool regularFile = !queued.isDirectory();
            const size_t bytes = queued.size();
            queued.close();
            if (regularFile && name.endsWith(".json")) {
                ++queuedCount;
                queuedBytes += bytes;
            }
            const int slash = name.lastIndexOf('/');
            if (slash >= 0) {
                name = name.substring(slash + 1);
            }
            const unsigned long long sequence = std::strtoull(name.c_str(), nullptr, 10);
            nextSequence = std::max(nextSequence, sequence + 1);
            queued = directory.openNextFile();
        }
        directory.close();
    }
    if (queuedCount >= MAX_MQTT_OUTBOX_ITEMS || queuedBytes + expected > MAX_MQTT_OUTBOX_BYTES) {
        ESP_LOGE(LOG_TAG.c_str(), "MQTT lifecycle outbox is full");
        return false;
    }
    char path[48];
    snprintf(path, sizeof(path), "%s/%020llu.json", MQTT_OUTBOX_DIR, nextSequence);
    const String tempPath = AtomicFile::temporaryPath(path);
    if (!LittleFSUtil::removeIfExists(tempPath)) {
        return false;
    }
    File file = LittleFS.open(tempPath, FILE_WRITE);
    if (!file) {
        return false;
    }
    const size_t written = serializeJson(doc, file);
    file.flush();
    file.close();
    JsonDocument verification;
    File verificationFile = LittleFS.open(tempPath, FILE_READ);
    const bool valid = verificationFile && !deserializeJson(verification, verificationFile) && verification.is<JsonObject>();
    verificationFile.close();
    if (written != expected || !valid || !AtomicFile::commit(path)) {
        LittleFSUtil::removeIfExists(tempPath);
        return false;
    }
    return true;
}

void MQTTPlugin::flushDurablePublishes() {
    MqttOutboxLock lock(outboxMutex);
    if (!lock.locked) {
        return;
    }
    if (!mqttWasConnected) {
        return;
    }
    File recoveryDirectory = LittleFS.open(MQTT_OUTBOX_DIR);
    if (recoveryDirectory && recoveryDirectory.isDirectory()) {
        File pending = recoveryDirectory.openNextFile();
        while (pending) {
            const String tempPath = LittleFSUtil::pathFromEntry(MQTT_OUTBOX_DIR, pending.name());
            pending.close();
            if (tempPath.endsWith(".json.tmp")) {
                File tempFile = LittleFS.open(tempPath, FILE_READ);
                JsonDocument pendingDoc;
                const bool valid = tempFile && !deserializeJson(pendingDoc, tempFile) && pendingDoc.is<JsonObject>();
                tempFile.close();
                const String finalPath = tempPath.substring(0, tempPath.length() - 4);
                AtomicFile::recoverPending(finalPath, valid);
            } else if (tempPath.endsWith(".json.bak")) {
                const String finalPath = tempPath.substring(0, tempPath.length() - 4);
                if (LittleFSUtil::existsQuietly(finalPath)) {
                    AtomicFile::discardBackup(finalPath);
                } else {
                    AtomicFile::restoreBackup(finalPath);
                }
            }
            pending = recoveryDirectory.openNextFile();
        }
        recoveryDirectory.close();
    }
    File directory = LittleFS.open(MQTT_OUTBOX_DIR);
    if (!directory || !directory.isDirectory()) {
        return;
    }

    String selectedPath;
    File candidate = directory.openNextFile();
    while (candidate) {
        const String path = LittleFSUtil::pathFromEntry(MQTT_OUTBOX_DIR, candidate.name());
        candidate.close();
        if (path.endsWith(".json") && (selectedPath.isEmpty() || path.compareTo(selectedPath) < 0)) {
            selectedPath = path;
        }
        candidate = directory.openNextFile();
    }
    directory.close();
    if (selectedPath.isEmpty()) {
        return;
    }

    File file = LittleFS.open(selectedPath, FILE_READ);
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, file);
    file.close();
    const String topic = doc["topic"].as<String>();
    String message = doc["message"].as<String>();
    const bool retained = doc["retained"].is<bool>() && doc["retained"].as<bool>();
    const int qos = doc["qos"] | 0;
    const String expectedPrefix = "gaggimate/" + machineTopicId() + "/";
    if (error || !topic.startsWith(expectedPrefix) || message.isEmpty() || qos < 0 || qos > 2) {
        ESP_LOGE(LOG_TAG.c_str(), "Removing invalid MQTT outbox record %s", selectedPath.c_str());
        LittleFS.remove(selectedPath);
        return;
    }
    JsonDocument messageDoc;
    EpochTime::Seconds queuedTimestamp = 0;
    const EpochTime::Seconds now = EpochTime::now();
    if (!deserializeJson(messageDoc, message) && messageDoc.is<JsonObject>() &&
        (!mqttJsonEpoch(messageDoc["timestamp"], queuedTimestamp) || queuedTimestamp < EpochTime::MIN_VALID) &&
        now >= EpochTime::MIN_VALID) {
        messageDoc["timestamp"] = now;
        message = "";
        serializeJson(messageDoc, message);
    }
    if (publishNow(topic, message, retained, qos)) {
        LittleFS.remove(selectedPath);
    }
}

void MQTTPlugin::publishBrewState(const char *state) {
    JsonDocument doc;
    doc["state"] = state;
    doc["timestamp"] = EpochTime::now();
    String json;
    serializeJson(doc, json);
    publish("controller/brew/state", json.c_str());
}

void MQTTPlugin::publishMachineState(const char *state, const bool force) {
    if (!force && !isAutoTuningParticipating())
        return;

    Settings const &settings = controller->getSettings();
    JsonDocument doc;
    doc["event_type"] = "machine_state";
    doc["schema_version"] = 1;
    doc["machine_id"] = machineId();
    doc["machine_adapter"] = "gaggimate";
    doc["timestamp"] = EpochTime::now();
    doc["state"] = state;
    doc["local_optimization_enabled"] = localOptimizationEnabled();
    doc["community_upload_enabled"] = false;
    doc["community_upload_owner"] = "gaggimate";
    AutoTuningPayloadMetadata::addRecipe(controller, doc);
    AutoTuningPayloadMetadata::addProfile(controller, doc);

    String json;
    serializeJson(doc, json);
    publish("machine/state", json.c_str());
}

void MQTTPlugin::publishOptimizerSettings() {
    if (!mqttWasConnected || !controller || !isAutoTuningEnabled())
        return;

    Settings const &settings = controller->getSettings();
    JsonDocument doc;
    doc["event_type"] = "optimizer_settings";
    doc["schema_version"] = 1;
    doc["machine_id"] = machineId();
    doc["timestamp"] = EpochTime::now();
    doc["optimizer_mode"] = settings.getRLOptimizerMode();
    doc["cpbo_profile_name"] = settings.getRLCPBOProfileName();
    doc["cpbo_comparison_mode"] = settings.getRLCPBOComparisonMode();
    AutoTuning::RecipeDomain const recipeDomain = settings.getRLRecipeDomain();
    JsonObject recipeDomainJson = doc["recipe_domain"].to<JsonObject>();
    recipeDomainJson["grind_radius_steps"] = recipeDomain.grindRadiusSteps;
    recipeDomainJson["dose_min_g"] = recipeDomain.doseMinG;
    recipeDomainJson["dose_max_g"] = recipeDomain.doseMaxG;
    recipeDomainJson["target_output_min_g"] = recipeDomain.targetOutputMinG;
    recipeDomainJson["target_output_max_g"] = recipeDomain.targetOutputMaxG;
    doc["bean_context_id"] = beanContextId();
    doc["grinder_context_id"] = grinderContextId();
    JsonDocument tasteGoal;
    AutoTuning::activeTasteGoal(settings, tasteGoal);
    doc["taste_goal"].set(tasteGoal.as<JsonVariantConst>());
    if (controller->getProfileManager()) {
        Profile &profile = controller->getProfileManager()->getSelectedProfile();
        doc["profile_id"] = profile.id;
        doc["profile_label"] = profile.label;
    }
    doc["source"] = "gaggimate_mqtt";

    String json;
    serializeJson(doc, json);
    publish("rl/settings", json.c_str(), true);
}

void MQTTPlugin::loop() {
    if (clientMutex && xSemaphoreTake(clientMutex, 0) == pdTRUE) {
        client.loop();
        xSemaphoreGive(clientMutex);
    }
    maintainConnection();
    handleConnectionReady();
    refreshAutoTuningSubscriptions();
    flushDurablePublishes();
}

void MQTTPlugin::handleRecommendation(const String &payload) {
    if (!pluginManager || !isAutoTuningParticipating()) {
        clearLatestRecommendation();
        return;
    }
    if (payload.isEmpty()) {
        clearLatestRecommendationAndNotify();
        return;
    }
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error || !doc.is<JsonObject>()) {
        clearLatestRecommendationAndNotify();
        return;
    }
    const EpochTime::Seconds receivedAt = EpochTime::now();
    if (receivedAt < EpochTime::MIN_VALID) {
        clearLatestRecommendationAndNotify();
        return;
    }

    AutoTuning::Recommendation recommendation;
    String recommendationError;
    if (!AutoTuningJsonCodec::parseRecommendation(doc.as<JsonVariantConst>(), recommendation, recommendationError)) {
        ESP_LOGW(LOG_TAG.c_str(), "Rejected malformed recommendation: %s", recommendationError.c_str());
        clearLatestRecommendationAndNotify();
        return;
    }
    const bool actionableStatus = recommendation.status == AutoTuning::RecommendationStatus::Pending ||
                                  recommendation.status == AutoTuning::RecommendationStatus::Shown;
    const bool matchingMode = (recommendation.mode == AutoTuning::RecommendationMode::CpboGlobalPrevious &&
                               recommendation.comparisonMode == AutoTuning::ComparisonMode::GlobalPrevious) ||
                              (recommendation.mode == AutoTuning::RecommendationMode::CpboBestIncumbent &&
                               recommendation.comparisonMode == AutoTuning::ComparisonMode::BestIncumbent);
    const bool timestampsPlausible =
        (recommendation.createdAt <= 0 || EpochTime::plausible(recommendation.createdAt, receivedAt)) &&
        (recommendation.updatedAt <= 0 || EpochTime::plausible(recommendation.updatedAt, receivedAt)) &&
        (!recommendation.expiresAt.has_value() ||
         (EpochTime::plausible(*recommendation.expiresAt, receivedAt) && *recommendation.expiresAt > receivedAt));
    if (recommendation.machineId != machineId().c_str() || !actionableStatus || !matchingMode || !timestampsPlausible ||
        (recommendation.preferenceFeedbackRequired && recommendation.sourceShotId != recommendation.comparisonAnchorShotId)) {
        clearLatestRecommendationAndNotify();
        return;
    }

    AutoTuning::TasteGoal activeTasteGoal;
    String tasteGoalError;
    if (!AutoTuning::activeTasteGoal(controller->getSettings(), activeTasteGoal, &tasteGoalError)) {
        clearLatestRecommendationAndNotify();
        return;
    }
    Settings const &settings = controller->getSettings();
    if (recommendation.tasteGoal != activeTasteGoal || recommendation.beanContextId != settings.getRLBeanContextId().c_str() ||
        recommendation.grinderContextId != settings.getRLGrinderContextId().c_str() ||
        (!recommendation.profileId.empty() && controller->getProfileManager() &&
         recommendation.profileId != controller->getProfileManager()->getSelectedProfile().id.c_str())) {
        clearLatestRecommendationAndNotify();
        return;
    }

    const AutoTuning::RecommendationTargets targets{recommendation.nextDoseG, recommendation.targetYieldG,
                                                    recommendation.targetRatio, recommendation.grindDeltaStepsFromCurrent};
    std::string validationReason;
    if (!AutoTuning::validateRecommendationTargets(targets, settings.getRLRecipeDomain(), validationReason)) {
        ESP_LOGW(LOG_TAG.c_str(), "Rejected invalid recommendation: %s", validationReason.c_str());
        clearLatestRecommendationAndNotify();
        return;
    }
    String grindProjectionReason;
    if (!validateRecommendationGrindProjection(recommendation, settings, grindProjectionReason)) {
        ESP_LOGW(LOG_TAG.c_str(), "Rejected invalid recommendation: %s", grindProjectionReason.c_str());
        clearLatestRecommendationAndNotify();
        return;
    }
    if (recommendation.createdAt <= 0) {
        recommendation.createdAt = receivedAt;
    }
    if (recommendation.updatedAt <= 0) {
        recommendation.updatedAt = recommendation.createdAt;
    }

    hasRecommendation = true;
    latestRecommendation = recommendation;

    Event event;
    event.id = "rl:recommendation:received";
    event.setPayload(recommendation);
    pluginManager->trigger(event);

    AutoTuning::AutoTuningRecordStorePort *store = controller->getAutoTuningRecordStore();
    if (store) {
        store->storeRecommendation(recommendation);
    }
    AutoTuning::CommunityUploadPort *upload = controller->getCommunityUpload();
    if (upload) {
        upload->enqueueRecommendation(recommendation);
    }
}

void MQTTPlugin::handleShotDeliveryAck(const String &payload) {
    if (!pluginManager || !controller ||
        !AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(), this).routeOffBoardTransport() ||
        payload.isEmpty()) {
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, payload) || !doc.is<JsonObject>() || doc.as<JsonObjectConst>().size() != 8) {
        ESP_LOGW(LOG_TAG.c_str(), "Rejected malformed shot delivery acknowledgement");
        return;
    }
    const String eventType = doc["event_type"].as<String>();
    const String shotId = doc["shot_id"].as<String>();
    const String acknowledgedMachineId = doc["machine_id"].as<String>();
    const String outcome = doc["outcome"].as<String>();
    const String reason = doc["reason"].as<String>();
    const bool retryableType = doc["retryable"].is<bool>();
    const bool retryable = retryableType && doc["retryable"].as<bool>();
    const bool validOutcome = outcome == "accepted" || outcome == "already_processed" || outcome == "transient_failure" ||
                              outcome == "permanent_rejection";
    const bool validReason = reason == "stored" || reason == "already_processed" || reason == "invalid_shot" ||
                             reason == "ingest_unavailable" || reason == "local_optimization_disabled" ||
                             reason == "not_optimizable";
    EpochTime::Seconds acknowledgementTimestamp = 0;
    const bool integerTimestamp = mqttJsonEpoch(doc["timestamp"], acknowledgementTimestamp);
    if (eventType != "shot_delivery_ack" || !doc["schema_version"].is<int>() || doc["schema_version"].as<int>() != 1 ||
        shotId.isEmpty() || shotId.length() > 256 || acknowledgedMachineId != machineId() || !validOutcome || !validReason ||
        !retryableType || retryable != (outcome == "transient_failure") || reason.isEmpty() || reason.length() > 80 ||
        !integerTimestamp || !EpochTime::plausible(acknowledgementTimestamp)) {
        ESP_LOGW(LOG_TAG.c_str(), "Rejected invalid shot delivery acknowledgement");
        return;
    }

    Event event;
    event.id = "rl:shot:delivery:ack";
    event.setString("shot_id", shotId);
    event.setString("outcome", outcome);
    event.setString("reason", reason);
    event.setInt("retryable", retryable ? 1 : 0);
    event.setInt64("timestamp", acknowledgementTimestamp);
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
    latestStatusTimestamp = mqttJsonEpochOrZero(doc["timestamp"]);
    latestStatusLastShotId = mqttJsonString(doc["last_shot_id"]);
    latestStatusLastShotAt = mqttJsonEpochOrZero(doc["last_shot_at"]);
    latestStatusLastShotType = mqttJsonString(doc["last_shot_type"]);
    latestStatusLastShotTimeS = doc["last_shot_time_s"] | 0.0f;
    latestStatusLastShotBeverageOutG = doc["last_shot_beverage_out_g"] | 0.0f;
    latestStatusLastShotTargetYieldG = doc["last_shot_target_yield_g"] | 0.0f;
    latestStatusLastRecommendationId = mqttJsonString(doc["last_recommendation_id"]);
    latestStatusLastRecommendationAt = mqttJsonEpochOrZero(doc["last_recommendation_at"]);
    latestStatusRecommendationApplyStatus = mqttJsonString(doc["recommendation_apply_status"]);
    latestStatusMode = mqttJsonString(doc["mode"]);
    latestStatusOptimizerProfileId = mqttJsonString(doc["optimizer_profile_id"]);
    latestStatusOptimizerProfileLabel = mqttJsonString(doc["optimizer_profile_label"]);
    latestStatusOptimizerConfiguredMode = mqttJsonString(doc["optimizer_configured_mode"]);
    latestStatusOptimizerEffectiveMode = mqttJsonString(doc["optimizer_effective_mode"]);
    latestStatusOptimizerFallbackReason = mqttJsonString(doc["optimizer_fallback_reason"]);
    latestStatusCPBOProfileName = mqttJsonString(doc["cpbo_profile_name"]);
    latestStatusCPBOComparisonMode = mqttJsonString(doc["cpbo_comparison_mode"]);
    latestStatusLocalShotCount = doc["local_shot_count"] | 0;
    latestStatusUploadQueueCount = doc["upload_queue_count"] | 0;
    latestStatusUploadQueueRejectedCount = doc["upload_queue_rejected_count"] | 0;
    latestStatusUploadQueueLastRejectedId = mqttJsonString(doc["upload_queue_last_rejected_id"]);
    latestStatusUploadQueueLastRejectedRecordId = mqttJsonString(doc["upload_queue_last_rejected_record_id"]);
    latestStatusUploadQueueLastRejectedError = mqttJsonString(doc["upload_queue_last_rejected_error"]);
    latestStatusCommunityUploadEnabled = doc["community_upload_enabled"] | false;
    latestStatusRuntimeHealthStatus = mqttJsonString(doc["runtime_health_status"]);
    latestStatusRuntimeHealthSummary = mqttJsonString(doc["runtime_health_summary"]);
    latestStatusRuntimeHealthWarningsJson = "[]";
    latestStatusRuntimeHealthWaitingReasonsJson = "[]";
    latestStatusAutoTuningDiagnosticStepsJson = "[]";
    latestStatusRuntimeHealthStorageBackend = mqttJsonString(doc["runtime_health_storage_backend"]);
    latestStatusRuntimeHealthStorageAvailable = doc["runtime_health_storage_available"] | false;
    latestStatusRuntimeHealthUploadConfigured = doc["runtime_health_upload_configured"] | false;
    latestStatusRuntimeHealthCommunityUploadRequested = doc["runtime_health_community_upload_requested"] | false;
    latestStatusRuntimeHealthPendingUploadCount = doc["runtime_health_pending_upload_count"] | 0;
    latestStatusRuntimeHealthFailedUploadCount = doc["runtime_health_failed_upload_count"] | 0;
    latestStatusRuntimeHealthRejectedUploadCount = doc["runtime_health_rejected_upload_count"] | 0;
    latestStatusGrinderCatalogSearchUrl = mqttJsonString(doc["grinder_catalog_search_url"]);
    latestStatusRecentShotsJson = "[]";
    if (doc["runtime_health_warnings"].is<JsonArray>()) {
        String warnings;
        serializeJson(doc["runtime_health_warnings"], warnings);
        if (warnings.length() <= 768) {
            latestStatusRuntimeHealthWarningsJson = warnings;
        }
    }
    if (doc["runtime_health_waiting_reasons"].is<JsonArray>()) {
        String reasons;
        serializeJson(doc["runtime_health_waiting_reasons"], reasons);
        if (reasons.length() <= 768) {
            latestStatusRuntimeHealthWaitingReasonsJson = reasons;
        }
    }
    if (doc["auto_tuning_diagnostic_steps"].is<JsonArray>()) {
        String steps;
        serializeJson(doc["auto_tuning_diagnostic_steps"], steps);
        if (steps.length() <= 2048) {
            latestStatusAutoTuningDiagnosticStepsJson = steps;
        }
    }
    if (latestStatusLastRecommendationId.isEmpty()) {
        clearLatestRecommendation();
        Event cleared;
        cleared.id = "rl:recommendation:cleared";
        pluginManager->trigger(cleared);
    }
    if (doc["recent_shots"].is<JsonArray>()) {
        String recent;
        serializeJson(doc["recent_shots"], recent);
        if (recent.length() <= 4096) {
            latestStatusRecentShotsJson = recent;
        }
    }
    Event event;
    event.id = "rl:status:received";
    event.setInt("seen", latestStatusSeen ? 1 : 0);
    event.setInt("addon_online", latestAddonOnline ? 1 : 0);
    event.setInt64("timestamp", latestStatusTimestamp);
    event.setString("last_shot_id", latestStatusLastShotId);
    event.setInt64("last_shot_at", latestStatusLastShotAt);
    event.setString("last_shot_type", latestStatusLastShotType);
    event.setInt("last_shot_time_s_x10", static_cast<int>(latestStatusLastShotTimeS * 10.0f));
    event.setInt("last_shot_beverage_out_g_x10", static_cast<int>(latestStatusLastShotBeverageOutG * 10.0f));
    event.setInt("last_shot_target_yield_g_x10", static_cast<int>(latestStatusLastShotTargetYieldG * 10.0f));
    event.setString("last_recommendation_id", latestStatusLastRecommendationId);
    event.setInt64("last_recommendation_at", latestStatusLastRecommendationAt);
    event.setString("recommendation_apply_status", latestStatusRecommendationApplyStatus);
    event.setString("mode", latestStatusMode);
    event.setString("optimizer_profile_id", latestStatusOptimizerProfileId);
    event.setString("optimizer_profile_label", latestStatusOptimizerProfileLabel);
    event.setString("optimizer_configured_mode", latestStatusOptimizerConfiguredMode);
    event.setString("optimizer_effective_mode", latestStatusOptimizerEffectiveMode);
    event.setString("optimizer_fallback_reason", latestStatusOptimizerFallbackReason);
    event.setString("cpbo_profile_name", latestStatusCPBOProfileName);
    event.setString("cpbo_comparison_mode", latestStatusCPBOComparisonMode);
    event.setInt("local_shot_count", latestStatusLocalShotCount);
    event.setInt("upload_queue_count", latestStatusUploadQueueCount);
    event.setInt("upload_queue_rejected_count", latestStatusUploadQueueRejectedCount);
    event.setString("upload_queue_last_rejected_id", latestStatusUploadQueueLastRejectedId);
    event.setString("upload_queue_last_rejected_record_id", latestStatusUploadQueueLastRejectedRecordId);
    event.setString("upload_queue_last_rejected_error", latestStatusUploadQueueLastRejectedError);
    event.setInt("community_upload_enabled", latestStatusCommunityUploadEnabled ? 1 : 0);
    event.setString("runtime_health_status", latestStatusRuntimeHealthStatus);
    event.setString("runtime_health_summary", latestStatusRuntimeHealthSummary);
    event.setString("runtime_health_warnings_json", latestStatusRuntimeHealthWarningsJson);
    event.setString("runtime_health_waiting_reasons_json", latestStatusRuntimeHealthWaitingReasonsJson);
    event.setString("auto_tuning_diagnostic_steps_json", latestStatusAutoTuningDiagnosticStepsJson);
    event.setString("runtime_health_storage_backend", latestStatusRuntimeHealthStorageBackend);
    event.setInt("runtime_health_storage_available", latestStatusRuntimeHealthStorageAvailable ? 1 : 0);
    event.setInt("runtime_health_upload_configured", latestStatusRuntimeHealthUploadConfigured ? 1 : 0);
    event.setInt("runtime_health_community_upload_requested", latestStatusRuntimeHealthCommunityUploadRequested ? 1 : 0);
    event.setInt("runtime_health_pending_upload_count", latestStatusRuntimeHealthPendingUploadCount);
    event.setInt("runtime_health_failed_upload_count", latestStatusRuntimeHealthFailedUploadCount);
    event.setInt("runtime_health_rejected_upload_count", latestStatusRuntimeHealthRejectedUploadCount);
    event.setString("grinder_catalog_search_url", latestStatusGrinderCatalogSearchUrl);
    event.setString("recent_shots_json", latestStatusRecentShotsJson);
    pluginManager->trigger(event);
}

bool MQTTPlugin::applyProjectedGrinderPosition() {
    if (!controller || fabsf(latestRecommendation.grindDeltaStepsFromCurrent) < 0.001f) {
        return false;
    }

    Settings &settings = controller->getSettings();
    const String activeContextId = settings.getRLGrinderContextId();
    if (activeContextId.isEmpty()) {
        return false;
    }

    JsonDocument contextsDoc(&psramAllocator);
    const DeserializationError error = deserializeJson(contextsDoc, settings.getRLGrinderContextsJson());
    if (error || !contextsDoc.is<JsonArray>()) {
        return false;
    }

    JsonObject activeContext;
    for (JsonObject context : contextsDoc.as<JsonArray>()) {
        if (context["id"].as<String>() == activeContextId) {
            activeContext = context;
            break;
        }
    }
    if (activeContext.isNull()) {
        return false;
    }

    const float projectedRelativeStep = std::clamp(latestRecommendation.projectedRelativeStepFromReference, -10000.0f, 10000.0f);
    activeContext["current_relative_step"] = projectedRelativeStep;
    if (latestRecommendation.projectedAbsoluteStep.has_value()) {
        const float projectedAbsoluteStep = std::clamp(*latestRecommendation.projectedAbsoluteStep, -10000.0f, 10000.0f);
        activeContext["current_absolute_step"] = projectedAbsoluteStep;
        if (!mqttJsonNumber(activeContext["absolute_reference_step"])) {
            activeContext["absolute_reference_step"] = projectedAbsoluteStep - projectedRelativeStep;
        }
        activeContext["grinder_calibration_mode"] = "absolute_display_calibrated";
    } else if (mqttJsonNumber(activeContext["microns_per_step"])) {
        activeContext["grinder_calibration_mode"] = "relative_calibrated";
    }

    String contextsJson;
    serializeJson(contextsDoc, contextsJson);
    settings.setRLGrinderContextsJson(contextsJson);
    settings.save(true);
    return true;
}

bool MQTTPlugin::applyLatestRecommendation() {
    if (!hasRecommendation || !controller || !isAutoTuningParticipating())
        return false;

    String reason;
    if (!validateLatestRecommendation(reason)) {
        ESP_LOGW(LOG_TAG.c_str(), "Refused recommendation apply: %s", reason.c_str());
        clearLatestRecommendationAndNotify();
        return false;
    }

    // Persist the acceptance intent before changing the recipe. The outbox is
    // flushed only after this event handler returns, so the broker never sees
    // an accepted decision before the local mutation has completed.
    if (!publishRecommendationDecision("accepted", true)) {
        ESP_LOGE(LOG_TAG.c_str(), "Refused recommendation apply because acceptance could not be persisted");
        return false;
    }

    applyProjectedGrinderPosition();

    bool doseApplied = false;
    bool yieldApplied = false;
    bool yieldFailed = false;

    if (latestRecommendation.nextDoseG > 0.0f) {
        controller->setTargetGrindVolume(latestRecommendation.nextDoseG);
        doseApplied = canApplyGrindByWeightTarget();
    }

    if (latestRecommendation.targetYieldG > 0.0f && controller->getProfileManager()) {
        Profile &profile = controller->getProfileManager()->getSelectedProfile();
        if (profile.setFinalVolumetricTarget(latestRecommendation.targetYieldG)) {
            yieldApplied = controller->getProfileManager()->saveProfile(profile);
            yieldFailed = !yieldApplied;
        } else {
            yieldFailed = true;
        }
    } else if (latestRecommendation.targetYieldG > 0.0f) {
        yieldFailed = true;
    }

    publishRecommendationApply(doseApplied, yieldApplied, yieldFailed);
    publishMachineState("idle");
    return true;
}

bool MQTTPlugin::ignoreLatestRecommendation() {
    if (!hasRecommendation || !isAutoTuningParticipating())
        return false;
    if (!publishRecommendationDecision("ignored", false)) {
        ESP_LOGE(LOG_TAG.c_str(), "Refused recommendation ignore because the decision could not be persisted");
        return false;
    }
    clearLatestRecommendation();
    publishMachineState("idle");
    return true;
}

bool MQTTPlugin::validateLatestRecommendation(String &reason) const {
    if (!hasRecommendation || !controller) {
        reason = "no active recommendation";
        return false;
    }
    Settings const &settings = controller->getSettings();
    if (latestRecommendation.beanContextId != settings.getRLBeanContextId().c_str() ||
        latestRecommendation.grinderContextId != settings.getRLGrinderContextId().c_str()) {
        reason = "recommendation context is no longer active";
        return false;
    }
    if (latestRecommendation.status != AutoTuning::RecommendationStatus::Pending &&
        latestRecommendation.status != AutoTuning::RecommendationStatus::Shown) {
        reason = "recommendation is not actionable";
        return false;
    }
    const EpochTime::Seconds now = EpochTime::now();
    if (latestRecommendation.expiresAt.has_value() && (now < EpochTime::MIN_VALID || *latestRecommendation.expiresAt <= now)) {
        reason = "recommendation has expired";
        return false;
    }
    AutoTuning::RecommendationTargets targets{latestRecommendation.nextDoseG, latestRecommendation.targetYieldG,
                                              latestRecommendation.targetRatio, latestRecommendation.grindDeltaStepsFromCurrent};
    std::string validationReason;
    const bool valid =
        AutoTuning::validateRecommendationTargets(targets, controller->getSettings().getRLRecipeDomain(), validationReason);
    reason = validationReason.c_str();
    return valid && validateRecommendationGrindProjection(latestRecommendation, settings, reason);
}

void MQTTPlugin::clearLatestRecommendationAndNotify() {
    clearLatestRecommendation();
    if (!pluginManager) {
        return;
    }
    Event event;
    event.id = "rl:recommendation:cleared";
    pluginManager->trigger(event);
}

void MQTTPlugin::clearLatestRecommendation() {
    hasRecommendation = false;
    latestRecommendation = AutoTuning::Recommendation{};
}

bool MQTTPlugin::publishRecommendationDecision(const char *decision, bool includeEditedFields) {
    if (!isAutoTuningParticipating() || latestRecommendation.recommendationId.empty())
        return false;

    JsonDocument doc;
    doc["event_type"] = "recommendation_decision";
    doc["schema_version"] = 1;
    doc["recommendation_id"] = latestRecommendation.recommendationId.c_str();
    doc["decision"] = decision;
    doc["source"] = "gaggimate_mqtt";
    doc["timestamp"] = EpochTime::now();

    JsonObject edited = doc["edited_fields"].to<JsonObject>();
    if (includeEditedFields) {
        edited["next_dose_g"] = latestRecommendation.nextDoseG;
        edited["target_yield_g"] = latestRecommendation.targetYieldG;
    }

    String json;
    serializeJson(doc, json);
    return publish("rl/recommendation/decision", json.c_str(), false, 1, true);
}

void MQTTPlugin::publishRecommendationApply(bool doseApplied, bool yieldApplied, bool yieldFailed) {
    if (!isAutoTuningParticipating() || latestRecommendation.recommendationId.empty())
        return;

    const bool grindManual = fabsf(latestRecommendation.grindDeltaStepsFromCurrent) >= 0.001f;
    const bool doseManual = latestRecommendation.nextDoseG > 0.0f && !doseApplied;
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
    doc["recommendation_id"] = latestRecommendation.recommendationId.c_str();
    doc["machine_id"] = machineId();
    doc["status"] = status;
    doc["source"] = "gaggimate_mqtt";
    doc["timestamp"] = EpochTime::now();

    JsonObject applied = doc["applied_fields"].to<JsonObject>();
    if (doseApplied) {
        applied["next_dose_g"] = latestRecommendation.nextDoseG;
    }
    if (yieldApplied) {
        applied["target_yield_g"] = latestRecommendation.targetYieldG;
        applied["target_ratio"] = latestRecommendation.targetRatio;
    }

    JsonArray manual = doc["manual_fields"].to<JsonArray>();
    if (grindManual) {
        manual.add("projected_relative_step_from_reference");
    }
    if (doseManual) {
        manual.add("next_dose_g");
    }

    JsonObject failed = doc["failed_fields"].to<JsonObject>();
    if (yieldFailed) {
        failed["target_yield_g"] = latestRecommendation.targetYieldG;
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
    publish("rl/recommendation/apply", json.c_str(), false, 1, true);
}

bool MQTTPlugin::publishShotCorrection(Event const &event) {
    if (!isAutoTuningParticipating())
        return false;

    AutoTuning::ShotCorrection const *correction = event.getPayload<AutoTuning::ShotCorrection>();
    if (!correction || correction->shotId.empty()) {
        return false;
    }

    JsonDocument doc;
    doc["event_type"] = "shot_correction";
    doc["schema_version"] = 1;
    doc["shot_id"] = correction->shotId.c_str();
    doc["machine_id"] = machineId();
    doc["source"] = correction->source.empty() ? "gaggimate_mqtt" : correction->source.c_str();
    doc["timestamp"] = EpochTime::now();

    if (correction->excludeFromLocalOptimization.has_value()) {
        doc["exclude_from_local_optimization"] = *correction->excludeFromLocalOptimization;
    }
    if (!correction->shotType.empty()) {
        doc["shot_type"] = correction->shotType.c_str();
    }
    if (correction->grindFollowed.has_value()) {
        doc["grind_followed"] = *correction->grindFollowed;
    }
    if (correction->doseFollowed.has_value()) {
        doc["dose_followed"] = *correction->doseFollowed;
    }
    if (correction->yieldFollowed.has_value()) {
        doc["yield_followed"] = *correction->yieldFollowed;
    }
    JsonArray tags = doc["correction_tags"].to<JsonArray>();
    for (std::string const &tag : correction->tags) {
        tags.add(tag.c_str());
    }

    String json;
    serializeJson(doc, json);
    return publish("rl/shot/correction", json.c_str(), false, 1, true);
}

void MQTTPlugin::publishLocalReset(Event const &event) {
    if (!isAutoTuningEnabled())
        return;

    JsonDocument doc;
    doc["event_type"] = "local_reset";
    doc["schema_version"] = 1;
    doc["machine_id"] = machineId();
    doc["scope"] = "all";
    doc["dry_run"] = event.getInt("dry_run") == 1;
    doc["source"] = "gaggimate_mqtt";
    doc["timestamp"] = EpochTime::now();

    String json;
    serializeJson(doc, json);
    publish("rl/local/reset", json.c_str(), false, 1, true);
    clearLatestRecommendationAndNotify();
}

bool MQTTPlugin::isAutoTuningEnabled() const {
    if (!controller)
        return false;
    Settings const &settings = controller->getSettings();
    return AutoTuning::Router(settings.getRLOptimizerConfiguration(), this).routeOffBoardTransport();
}

bool MQTTPlugin::isAutoTuningParticipating() const {
    if (!controller)
        return false;
    return AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(), this).acceptActionableRecommendations();
}

bool MQTTPlugin::configured() const { return controller && controller->getSettings().isHomeAssistant(); }

bool MQTTPlugin::connected() const { return mqttWasConnected; }

bool MQTTPlugin::publishShot(AutoTuning::ShotRecord const &shot, bool) {
    if (!controller ||
        !AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(), this).routeOffBoardTransport()) {
        return false;
    }
    String payload;
    return AutoTuningJsonCodec::serializeShotRecord(shot, payload) && publish("shot/profile", payload.c_str(), false, 1);
}

bool MQTTPlugin::canApplyGrindByWeightTarget() const {
    if (!controller)
        return false;
    Settings const &settings = controller->getSettings();
    return controller->isVolumetricAvailable() && settings.isVolumetricTarget();
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

String MQTTPlugin::grinderContextId() const {
    if (!controller)
        return "";
    return controller->getSettings().getRLGrinderContextId();
}

bool MQTTPlugin::localOptimizationEnabled() const {
    if (!controller)
        return false;
    Settings const &settings = controller->getSettings();
    return settings.isRLLocalOptimizationEnabled() && !settings.isRLOptimizationPaused() &&
           !settings.getRLBeanContextId().isEmpty();
}

void MQTTPlugin::setup(Controller *ctrl, PluginManager *pm) {
    this->controller = ctrl;
    this->pluginManager = pm;
    ctrl->setOptimizerTransport(this);
    clientMutex = xSemaphoreCreateMutex();
    outboxMutex = xSemaphoreCreateRecursiveMutex();
    xTaskCreate(connectionWorkerTask, "MQTT connect", 6144, this, 1, &connectionTaskHandle);

    client.onMessage([this](String &topic, String &payload) {
        if (topic.endsWith("/rl/recommendation")) {
            handleRecommendation(payload);
        } else if (topic.endsWith("/rl/status")) {
            handleStatus(payload);
        } else if (topic.endsWith("/rl/shot/ack")) {
            handleShotDeliveryAck(payload);
        }
    });

    pm->on("rl:preference", [this](Event &event) {
        if (!isAutoTuningParticipating()) {
            event.setInt("decision_persisted", 0);
            return;
        }
        AutoTuning::PreferenceFeedback const *preference = event.getPayload<AutoTuning::PreferenceFeedback>();
        if (!preference || preference->installId.empty() || preference->optimizationRunId.empty() ||
            preference->newShotId.empty() || preference->anchorShotId.empty() ||
            preference->newShotId == preference->anchorShotId || preference->comparisonMode == AutoTuning::ComparisonMode::None ||
            !preference->tasteGoal.valid() || preference->installId.size() > 160 || preference->optimizationRunId.size() > 256 ||
            preference->newShotId.size() > 256 || preference->anchorShotId.size() > 256) {
            ESP_LOGW("MQTTPlugin", "Rejected invalid CPBO preference event");
            event.setInt("decision_persisted", 0);
            return;
        }

        JsonDocument doc;
        doc["event_type"] = "preference_feedback";
        doc["schema_version"] = 1;
        doc["optimization_run_id"] = preference->optimizationRunId.c_str();
        doc["new_shot_id"] = preference->newShotId.c_str();
        doc["anchor_shot_id"] = preference->anchorShotId.c_str();
        doc["label"] = AutoTuning::preferenceLabelKey(preference->label);
        doc["comparison_mode"] = AutoTuning::comparisonModeKey(preference->comparisonMode);
        AutoTuning::writeTasteGoal(preference->tasteGoal, doc["taste_goal"].to<JsonObject>());
        doc["install_id"] = preference->installId.c_str();
        doc["machine_id"] = machineId();
        doc["timestamp"] = EpochTime::now();
        doc["source"] = "gaggimate_mqtt";

        String json;
        serializeJson(doc, json);
        event.setInt("decision_persisted", publish("rl/preference", json.c_str(), false, 1, true) ? 1 : 0);
    });

    pm->on("rl:recommendation:apply", [this](Event &event) {
        if (event.getString("recommendation_id") == latestRecommendation.recommendationId.c_str()) {
            event.setInt("decision_persisted", applyLatestRecommendation() ? 1 : 0);
        }
    });

    pm->on("rl:recommendation:ignore", [this](Event &event) {
        if (event.getString("recommendation_id") == latestRecommendation.recommendationId.c_str()) {
            event.setInt("decision_persisted", ignoreLatestRecommendation() ? 1 : 0);
        }
    });

    pm->on("rl:shot:correction",
           [this](Event &event) { event.setInt("optimizer_persisted", publishShotCorrection(event) ? 1 : 0); });

    pm->on("rl:local:reset", [this](Event const &event) { publishLocalReset(event); });

    pm->on("rl:settings:changed", [this](Event const &event) {
        String reason;
        if (event.getInt("optimizer_configuration_changed") > 0 || !isAutoTuningParticipating() ||
            (hasRecommendation && !validateLatestRecommendation(reason))) {
            clearLatestRecommendationAndNotify();
        }
        refreshAutoTuningSubscriptions();
        publishOptimizerSettings();
        publishMachineState("idle", true);
    });

    pm->on("rl:taste-goal:changed", [this](Event const &) {
        clearLatestRecommendation();
        Event cleared;
        cleared.id = "rl:recommendation:cleared";
        pluginManager->trigger(cleared);
        publishOptimizerSettings();
        publishMachineState("idle", true);
    });

    pm->on("rl:status:refresh", [this](Event const &) {
        publishOptimizerSettings();
        publishMachineState("idle", true);
    });

    pm->on("controller:wifi:connect", [this, ctrl](const Event &) { connect(ctrl); });

    pm->on("boiler:currentTemperature:change", [this](Event const &event) {
        if (!mqttWasConnected)
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
        if (!mqttWasConnected)
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

    pm->on("controller:brew:start", [this](Event const &) {
        publishBrewState("brewing");
        publishMachineState("brewing");
    });

    pm->on("controller:brew:end", [this](Event const &) {
        publishBrewState("not brewing");
        publishMachineState("idle");
    });
}
