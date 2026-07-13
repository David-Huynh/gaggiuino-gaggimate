#ifndef WEBUIPLUGIN_H
#define WEBUIPLUGIN_H

#define ELEGANTOTA_USE_ASYNC_WEBSERVER 1

#include <DNSServer.h>

#include "GitHubOTA.h"
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <cstdint>
#include <deque>
#include <display/core/AutoTuningModels.h>
#include <display/core/Event.h>
#include <display/core/Plugin.h>
#include <display/util/PsramAllocator.h>

constexpr size_t UPDATE_CHECK_INTERVAL = 30 * 60 * 1000;
constexpr size_t CLEANUP_PERIOD = 1000;
constexpr size_t STATUS_PERIOD = 500;
constexpr size_t DNS_PERIOD = 50;

const String LOCAL_URL = "http://4.4.4.1/";
const String RELEASE_URL = "https://github.com/jniebuhr/gaggimate/releases/";

class ProfileManager;

class WebUIPlugin : public Plugin {
  public:
    WebUIPlugin();
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    void setupServer();
    void start();
    void stop();

    // Websocket handlers
    void handleWebSocketData(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data,
                             size_t len);
    void handleOTASettings(uint32_t clientId, JsonDocument &request);
    void handleOTAStart(uint32_t clientId, JsonDocument &request);
    void handleAutotuneStart(uint32_t clientId, JsonDocument &request);
    void handleProfileRequest(uint32_t clientId, JsonDocument &request);
    void handleRLRequest(uint32_t clientId, JsonDocument &request);
    void handleFlushStart(uint32_t clientId, JsonDocument &request);
    // Send/re-send the pending shot comparison prompt. No-op when nothing is pending.
    void sendPreferencePrompt(AsyncWebSocketClient *client);
    void clearPendingPreferencePrompt();
    void sendDoseConfirmationPrompt(AsyncWebSocketClient *client);
    void clearPendingDoseConfirmation();
    bool activatePreferencePrompt(Event const &event);
    bool activateDoseConfirmation(Event const &event);
    void advancePreferencePrompt();
    void advanceDoseConfirmation();

    // HTTP handlers
    // Serves the web UI from the firmware-embedded, memory-mapped flash blob
    // (catch-all for any path not claimed by an explicit route). [GM-106]
    void serveWebAsset(AsyncWebServerRequest *request);
    void handleSettings(AsyncWebServerRequest *request) const;
    void handleBLEScaleList(AsyncWebServerRequest *request);
    void handleBLEScaleScan(AsyncWebServerRequest *request);
    void handleBLEScaleConnect(AsyncWebServerRequest *request);
    void handleBLEScaleInfo(AsyncWebServerRequest *request);
    void updateOTAStatus(const String &version);
    void updateOTAProgress(uint8_t phase, int progress);
    void sendAutotuneResult();
    void sendAutotuneFailed();

    void broadcastJson(JsonDocument &doc);

    // Core dump download
    void handleCoreDumpDownload(AsyncWebServerRequest *request);

    GitHubOTA *ota = nullptr;
    AsyncWebServer server;
    AsyncWebSocket ws;
    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    DNSServer *dnsServer = nullptr;
    ProfileManager *profileManager = nullptr;

    long lastUpdateCheck = 0;
    long lastStatus = 0;
    long lastCleanup = 0;
    long lastDns = 0;
    bool updating = false;
    bool apMode = false;
    bool serverRunning = false;
    String updateComponent = "";
    float currentBluetoothWeight = 0.0f;
    float currentHardwareWeight = 0.0f;
    float currentEstimatedWeight = 0.0f;
    // Reused for every 500ms status broadcast. Allocating a fresh JsonDocument
    // each tick was a major contributor to internal-heap fragmentation
    // (device reports 33%+ fragmentation, causing AsyncTCP buffer allocs to
    // stall mid-asset-serve). Keeping one doc lets its underlying pool grow
    // once and stay put.
    JsonDocument statusDoc{&psramAllocator};
    bool rlStatusSeen = false;
    bool rlAddonOnline = false;
    std::int64_t rlLastStatusAt = 0;
    String rlLastShotId = "";
    std::int64_t rlLastShotAt = 0;
    String rlLastShotType = "";
    float rlLastShotTimeS = 0.0f;
    float rlLastShotBeverageOutG = 0.0f;
    float rlLastShotTargetYieldG = 0.0f;
    String rlLastRecommendationId = "";
    std::int64_t rlLastRecommendationAt = 0;
    String rlRecommendationApplyStatus = "";
    String rlRecommendationStatus = "";
    String rlRecommendationMode = "";
    float rlRecommendationGrindDeltaStepsFromCurrent = 0.0f;
    float rlRecommendationGrindDeltaUmFromCurrent = 0.0f;
    float rlRecommendationProjectedRelativeStepFromReference = 0.0f;
    float rlRecommendationProjectedRelativeGrindUmFromReference = 0.0f;
    float rlRecommendationNextDoseG = 0.0f;
    float rlRecommendationTargetYieldG = 0.0f;
    float rlRecommendationTargetRatio = 0.0f;
    bool rlRecommendationHasCurrentAbsoluteStep = false;
    float rlRecommendationCurrentAbsoluteStep = 0.0f;
    bool rlRecommendationHasProjectedAbsoluteStep = false;
    float rlRecommendationProjectedAbsoluteStep = 0.0f;
    String rlMode = "";
    String rlOptimizerProfileId = "";
    String rlOptimizerProfileLabel = "";
    String rlOptimizerConfiguredMode = "";
    String rlOptimizerEffectiveMode = "";
    String rlOptimizerFallbackReason = "";
    int rlLocalShotCount = 0;
    int rlUploadQueueCount = 0;
    int rlUploadQueueRejectedCount = 0;
    String rlUploadQueueLastRejectedId = "";
    String rlUploadQueueLastRejectedRecordId = "";
    String rlUploadQueueLastRejectedError = "";
    bool rlCommunityUploadEnabled = false;
    String rlRuntimeHealthStatus = "";
    String rlRuntimeHealthSummary = "";
    String rlRuntimeHealthWarningsJson = "[]";
    String rlRuntimeHealthWaitingReasonsJson = "[]";
    String rlAutoTuningDiagnosticStepsJson = "[]";
    String rlRuntimeHealthStorageBackend = "";
    bool rlRuntimeHealthStorageAvailable = false;
    bool rlRuntimeHealthUploadConfigured = false;
    bool rlRuntimeHealthCommunityUploadRequested = false;
    int rlRuntimeHealthPendingUploadCount = 0;
    int rlRuntimeHealthFailedUploadCount = 0;
    int rlRuntimeHealthRejectedUploadCount = 0;
    int rlLocalDeliveryPendingCount = 0;
    int rlLocalDeliveryRetryCount = 0;
    int rlLocalDeliveryRejectedCount = 0;
    String rlLocalDeliveryLastError = "";
    String rlGrinderCatalogSearchUrl = "";
    String rlRecentShotsJson = "[]";

    // Pending RL prompts (serialized evt payloads). WebUIPlugin is the source of
    // truth so the WebUI can restore minimized prompt pills after a timeout, a
    // minimize, or a full page reload. Empty string = nothing pending.
    String _pendingPreferenceShotId = "";
    String _pendingPreferenceRecommendationId = "";
    String _pendPreferenceInstallId = "";
    String _pendPreferenceRunId = "";
    String _pendPreferenceAnchorShotId = "";
    String _pendPreferenceComparisonMode = "";
    AutoTuning::TasteGoal _pendPreferenceTasteGoal = AutoTuning::TasteGoal::balanced();
    String _pendPreferenceTasteGoalSummary = "Balanced";
    String _pendingDoseShotId = "";
    float _pendingDoseTargetG = 0.0f;
    std::deque<Event> _queuedPreferencePrompts;
    std::deque<Event> _queuedDoseConfirmations;
    String _pendRecJson = "";
};

#endif // WEBUIPLUGIN_H
