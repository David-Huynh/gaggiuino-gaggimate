#ifndef MQTTPLUGIN_H
#define MQTTPLUGIN_H

#include "../core/AutoTuning.h"
#include "../core/FeatureFlags.h"
#include "../core/Plugin.h"
#include "../core/PluginManager.h"
#include "GaggiMateComm.h"
#include <ArduinoJson.h>
#include <MQTT.h>
#include <WiFi.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
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
    bool publishLiveShotStarted(AutoTuning::LiveShotStarted const &event) override;
    bool publishLiveShotSample(AutoTuning::LiveShotSample const &event) override;
    bool publishLiveShotEnded(AutoTuning::LiveShotEnded const &event) override;

  private:
    struct ConnectionConfiguration {
        bool enabled = false;
        bool autoTuning = false;
        bool legacyHomeAssistant = false;
        String host;
        String user;
        String password;
        int port = 1883;
    };

    struct OutboundMessage {
        String topic;
        String message;
        bool retained = false;
        int qos = 0;
    };

    struct InboundMessage {
        String topic;
        String payload;
    };

    enum class LiveEventKind : uint8_t { Started, Sample, Ended };
    struct LiveEvent {
        LiveEventKind kind = LiveEventKind::Sample;
        AutoTuning::LiveShotStarted started;
        AutoTuning::LiveShotSample sample;
        AutoTuning::LiveShotEnded ended;
        unsigned long queuedAtMs = 0;
    };

    bool connectOnce();
    void requestReconnect();
    void serviceWorker();
    void refreshAutoTuningSubscriptions(ConnectionConfiguration const &configuration);
    void handleConnectionReady();
    void markConnected();
    void markDisconnected();
    bool publish(const std::string &topic, const std::string &message, bool retained = false, int qos = 0, bool durable = false);
    bool enqueuePublish(const String &topic, const String &message, bool retained, int qos);
    bool publishNow(const String &topic, const String &message, bool retained, int qos);
    bool enqueueDurablePublish(const String &topic, const String &message, bool retained, int qos);
    void recoverDurablePublishes();
    bool flushDurablePublishes();
    bool publishLiveEvent();
    bool publishQueuedMessage();
    void enqueueInbound(const String &topic, const String &payload);
    void drainInbound();
    static void connectionWorkerTask(void *arg);
#if GAGGIMATE_ENABLE_LEGACY_HOME_ASSISTANT_MQTT
    void publishBrewState(const char *state);
#endif
    void publishPendingControllerState();
#if GAGGIMATE_ENABLE_LEGACY_HOME_ASSISTANT_MQTT
    void publishDiscovery(Controller *controller);
#endif
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
    std::atomic_bool mqttWasConnected{false};
    std::atomic_bool connectionEnabled{false};
    bool autoTuningSubscribed = false;
    SemaphoreHandle_t outboxMutex = nullptr;
    std::atomic_bool durablePublishPending{true};
    unsigned long nextDurablePublishAttemptMs = 0;
    TaskHandle_t connectionTaskHandle = nullptr;
    unsigned long nextReconnectAttemptMs = 0;
    unsigned long reconnectDelayMs = 0;
    std::mutex connectionConfigMutex;
    ConnectionConfiguration connectionConfiguration;
    std::atomic_uint32_t connectionConfigRevision{0};
    uint32_t appliedConnectionConfigRevision = 0;
    std::atomic_uint32_t connectionGeneration{0};
    uint32_t handledConnectionGeneration = 0;
    std::mutex queueMutex;
    std::deque<OutboundMessage> outboundMessages;
    std::deque<InboundMessage> inboundMessages;
    std::deque<LiveEvent> liveEvents;
    static constexpr size_t MAX_OUTBOUND_MESSAGES = 32;
    static constexpr size_t MAX_INBOUND_MESSAGES = 16;
    static constexpr size_t MAX_LIVE_SAMPLES = 16;
    static constexpr unsigned long MAX_LIVE_EVENT_AGE_MS = 2000;
    static constexpr unsigned long MQTT_RECONNECT_INITIAL_DELAY_MS = 2000;
    static constexpr unsigned long MQTT_RECONNECT_MAX_DELAY_MS = 60000;

#if GAGGIMATE_ENABLE_LEGACY_HOME_ASSISTANT_MQTT
    float lastTemperature = 0;
    unsigned long lastLegacyTemperaturePublishMs = 0;
    std::atomic<float> pendingTemperature{0.0f};
    std::atomic_bool temperaturePublishPending{false};
    std::atomic<float> pendingTargetTemperature{0.0f};
    std::atomic_bool targetTemperaturePublishPending{false};
    std::atomic<int> pendingMode{-1};
    // -1 means no pending update; 0/1 map to not-brewing/brewing.
    std::atomic<int> pendingBrewState{-1};
#endif
    enum class PendingMachineState : uint8_t { None, Idle, Brewing, Standby };
    std::atomic<PendingMachineState> pendingMachineState{PendingMachineState::None};

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
