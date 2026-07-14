#ifndef MQTTPLUGIN_H
#define MQTTPLUGIN_H

#include "../core/AutoTuning.h"
#include "../core/Plugin.h"
#include "../core/PluginManager.h"
#include "GaggiMateComm.h"
#include <ArduinoJson.h>
#include <MQTT.h>
#include <WiFi.h>
#include <atomic>
#include <cstdint>
#include <string>

constexpr int MQTT_READ_BUFFER_SIZE = 8192;
constexpr int MQTT_WRITE_BUFFER_SIZE = 24576;

class MQTTPlugin : public Plugin, public AutoTuning::OptimizerTransportPort {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    bool connect(Controller *controller);
    void loop() override;
    bool configured() const override;
    bool connected() const override;
    bool publishShot(AutoTuning::ShotRecord const &shot, bool reprocess) override;

  private:
    bool connectOnce();
    void maintainConnection();
    void requestReconnect();
    void refreshAutoTuningSubscriptions();
    void handleConnectionReady();
    void markConnected();
    void markDisconnected();
    bool publish(const std::string &topic, const std::string &message, bool retained = false, int qos = 0, bool durable = false);
    bool publishNow(const String &topic, const String &message, bool retained, int qos);
    bool enqueueDurablePublish(const String &topic, const String &message, bool retained, int qos);
    void flushDurablePublishes();
    static void connectionWorkerTask(void *arg);
    void publishBrewState(const char *state);
    void publishDiscovery(Controller *controller);
    void publishMachineState(const char *state, bool force = false);
    void publishOptimizerSettings();
    void handleRecommendation(const String &payload);
    void handleStatus(const String &payload);
    void handleShotDeliveryAck(const String &payload);
    bool applyProjectedGrinderPosition();
    bool applyLatestRecommendation();
    bool ignoreLatestRecommendation();
    void clearLatestRecommendation();
    bool validateLatestRecommendation(String &reason) const;
    void clearLatestRecommendationAndNotify();
    bool publishRecommendationDecision(const char *decision, bool includeEditedFields);
    void publishRecommendationApply(bool doseApplied, bool yieldApplied, bool yieldFailed);
    bool publishShotCorrection(Event const &event);
    void publishLocalReset(Event const &event);
    void addUartDiagnostics(JsonDocument &doc) const;
    bool isAutoTuningEnabled() const;
    bool isAutoTuningParticipating() const;
    bool canApplyGrindByWeightTarget() const;
    String machineTopicId() const;
    String machineId() const;
    String beanContextId() const;
    String grinderContextId() const;
    bool localOptimizationEnabled() const;

    MQTTClient client{MQTT_READ_BUFFER_SIZE, MQTT_WRITE_BUFFER_SIZE};
    WiFiClient net;
    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    bool mqttWasConnected = false;
    bool autoTuningSubscribed = false;
    std::atomic_bool reconnectInProgress{false};
    std::atomic_bool connectionAttemptComplete{false};
    std::atomic_bool connectionAttemptSucceeded{false};
    SemaphoreHandle_t clientMutex = nullptr;
    SemaphoreHandle_t outboxMutex = nullptr;
    TaskHandle_t connectionTaskHandle = nullptr;
    unsigned long nextReconnectAttemptMs = 0;
    unsigned long reconnectDelayMs = 0;
    bool pendingConnectionEnabled = false;
    String pendingConnectionHost;
    String pendingConnectionUser;
    String pendingConnectionPassword;
    int pendingConnectionPort = 1883;
    static constexpr unsigned long MQTT_RECONNECT_INITIAL_DELAY_MS = 2000;
    static constexpr unsigned long MQTT_RECONNECT_MAX_DELAY_MS = 60000;

    float lastTemperature = 0;

    bool hasRecommendation = false;
    AutoTuning::Recommendation latestRecommendation;

    bool latestStatusSeen = false;
    bool latestAddonOnline = false;
    std::int64_t latestStatusTimestamp = 0;
    String latestStatusLastShotId;
    std::int64_t latestStatusLastShotAt = 0;
    String latestStatusLastShotType;
    float latestStatusLastShotTimeS = 0.0f;
    float latestStatusLastShotBeverageOutG = 0.0f;
    float latestStatusLastShotTargetYieldG = 0.0f;
    String latestStatusLastRecommendationId;
    std::int64_t latestStatusLastRecommendationAt = 0;
    String latestStatusRecommendationApplyStatus;
    String latestStatusMode;
    String latestStatusOptimizerProfileId;
    String latestStatusOptimizerProfileLabel;
    String latestStatusOptimizerConfiguredMode;
    String latestStatusOptimizerEffectiveMode;
    String latestStatusOptimizerFallbackReason;
    String latestStatusCPBOProfileName;
    String latestStatusCPBOComparisonMode;
    int latestStatusLocalShotCount = 0;
    int latestStatusUploadQueueCount = 0;
    int latestStatusUploadQueueRejectedCount = 0;
    String latestStatusUploadQueueLastRejectedId;
    String latestStatusUploadQueueLastRejectedRecordId;
    String latestStatusUploadQueueLastRejectedError;
    bool latestStatusCommunityUploadEnabled = false;
    String latestStatusRuntimeHealthStatus;
    String latestStatusRuntimeHealthSummary;
    String latestStatusRuntimeHealthWarningsJson = "[]";
    String latestStatusRuntimeHealthWaitingReasonsJson = "[]";
    String latestStatusAutoTuningDiagnosticStepsJson = "[]";
    String latestStatusRuntimeHealthStorageBackend;
    bool latestStatusRuntimeHealthStorageAvailable = false;
    bool latestStatusRuntimeHealthUploadConfigured = false;
    bool latestStatusRuntimeHealthCommunityUploadRequested = false;
    int latestStatusRuntimeHealthPendingUploadCount = 0;
    int latestStatusRuntimeHealthFailedUploadCount = 0;
    int latestStatusRuntimeHealthRejectedUploadCount = 0;
    String latestStatusGrinderCatalogSearchUrl;
    String latestStatusRecentShotsJson = "[]";
};

#endif // MQTTPLUGIN_H
