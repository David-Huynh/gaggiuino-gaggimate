#ifndef MQTTPLUGIN_H
#define MQTTPLUGIN_H

#include "../core/Plugin.h"
#include "../core/PluginManager.h"
#include <ArduinoJson.h>
#include <MQTT.h>
#include <WiFi.h>
#include <string>
#include <vector>

constexpr int MQTT_CONNECTION_RETRIES = 5;
constexpr int MQTT_CONNECTION_DELAY = 1000;
constexpr int MQTT_BUFFER_SIZE = 8192;
constexpr int SHOT_SAMPLE_INTERVAL_MS = 250;
constexpr int SHOT_MAX_SAMPLES = 240; // 60s at 4Hz

class MQTTPlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    bool connect(Controller *controller);
    void loop() override;

  private:
    void publish(const std::string &topic, const std::string &message);
    void publishBrewState(const char *state);
    void publishDiscovery(Controller *controller);
    void recordShotSample();
    void publishShotProfile();
    void publishMachineState(const char *state);
    void handleRecommendation(const String &payload);
    void handleStatus(const String &payload);
    void applyLatestRecommendation();
    void ignoreLatestRecommendation();
    void clearLatestRecommendation();
    void publishRecommendationDecision(const char *decision, bool includeEditedFields);
    void publishRecommendationApply(bool doseApplied, bool yieldApplied, bool yieldFailed);
    void publishShotCorrection(Event const &event);
    void publishUploadRequeue(Event const &event);
    void addRecipeMetadata(JsonDocument &doc) const;
    bool isAutoTuningEnabled() const;
    bool isAutoTuningParticipating() const;
    bool canApplyGrindByWeightTarget() const;
    String machineTopicId() const;
    String machineId() const;
    String beanContextId() const;
    String beanContextName() const;
    bool localOptimizationEnabled() const;
    String makeShotId() const;
    float targetYieldG() const;
    float doseTargetG() const;
    float currentShotWeightG() const;
    float currentShotFlowGPerS(float currentWeightG, uint16_t elapsedMs) const;
    const char *weightSourceName() const;
    const char *flowSourceName() const;
    bool pumpFlowCalibrationRequired() const;

    MQTTClient client{MQTT_BUFFER_SIZE};
    WiFiClient net;
    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;

    float lastTemperature = 0;

    bool isBrewing = false;
    unsigned long brewStartMs = 0;
    unsigned long lastSampleMs = 0;
    String currentShotId;
    int shotSource = 0;

    float currentBluetoothWeight = 0.0f;
    float currentHardwareWeight = 0.0f;
    float currentHardwareShotWeight = 0.0f;
    float currentEstimatedWeight = 0.0f;

    bool hasRecommendation = false;
    String latestRecommendationId;
    String latestRecommendationSourceShotId;
    int latestRecommendationGrindDeltaSteps = 0;
    float latestRecommendationGrindDeltaUm = 0.0f;
    float latestRecommendationNextGrindSteps = 0.0f;
    float latestRecommendationNextGrindUm = 0.0f;
    float latestRecommendationNextDoseG = 0.0f;
    float latestRecommendationTargetYieldG = 0.0f;
    float latestRecommendationTargetRatio = 0.0f;
    String latestRecommendationStatus;

    bool latestStatusSeen = false;
    bool latestAddonOnline = false;
    long latestStatusTimestamp = 0;
    String latestStatusLastShotId;
    long latestStatusLastShotAt = 0;
    String latestStatusLastRecommendationId;
    long latestStatusLastRecommendationAt = 0;
    String latestStatusRecommendationApplyStatus;
    String latestStatusMode;
    int latestStatusLocalShotCount = 0;
    int latestStatusRatedShotCount = 0;
    int latestStatusUploadQueueCount = 0;
    int latestStatusUploadQueueRejectedCount = 0;
    String latestStatusUploadQueueLastRejectedId;
    String latestStatusUploadQueueLastRejectedRecordId;
    String latestStatusUploadQueueLastRejectedError;
    bool latestStatusCommunityUploadEnabled = false;
    String latestStatusBestKnownRecipe;

    std::vector<float> pressureSamples;
    std::vector<float> targetPressureSamples;
    std::vector<float> flowSamples;
    std::vector<float> pumpFlowSamples;
    std::vector<float> targetFlowSamples;
    std::vector<float> weightSamples;
    std::vector<uint16_t> timeSamples;
};

#endif // MQTTPLUGIN_H
