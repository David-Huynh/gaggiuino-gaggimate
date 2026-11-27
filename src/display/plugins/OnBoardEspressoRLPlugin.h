#ifndef ONBOARDESPRESSORLPLUGIN_H
#define ONBOARDESPRESSORLPLUGIN_H

#if defined(GAGGIMATE_ENABLE_ONBOARD_ESPRESSORL) && !defined(GAGGIMATE_SIM)

#include <Arduino.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <display/core/AutoTuningPorts.h>
#include <display/core/Event.h>
#include <display/core/Plugin.h>
#include <display/util/PsramStlAllocator.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace EspressoRL {
class CombinedHydraulicPredictor;
class CemGpMpcController;
} // namespace EspressoRL

/// On-board EspressoRL adapter for ESP32-S3 N16R8 (Core-1 PSRAM worker + 4 Hz Shadow/Live CEM GP-MPC).
///
/// Architectural guarantees:
/// - Lives strictly in the firmware adapter layer; translates Gaggimate events into canonical
///   lib/EspressoRLCore types.
/// - Defaults to Shadow Mode (logs MPC proposals without actuating pump setpoints until explicitly armed).
/// - All large model workspaces reside in 8 MB PSRAM via makePsramUnique (zero internal SRAM bloat).
/// - Between-shot VFE Nyström refreshes and Gauss-Hermite VI CPBO fits run on Core 1 at low priority
///   with a 20 ms cooperative YieldCallback that feeds TWDT and aborts within <25 ms if a brew starts.
class OnBoardEspressoRLPlugin : public Plugin, public AutoTuning::OnBoardOptimizerPort {
  public:
    OnBoardEspressoRLPlugin();
    ~OnBoardEspressoRLPlugin() override;

    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

    // AutoTuning::OnBoardOptimizerPort
    bool ready() const override { return workspaceReady_.load(std::memory_order_acquire); }
    bool shadowModeOnly() const override { return shadowModeOnly_.load(std::memory_order_acquire); }
    void setShadowModeOnly(bool shadowOnly) override { shadowModeOnly_.store(shadowOnly, std::memory_order_release); }
    std::uint8_t controlMode() const override { return controlMode_.load(std::memory_order_acquire); }
    bool setControlMode(std::uint8_t mode) override;
    bool resetActiveSplineDelta() override;
    AutoTuning::OnBoardRuntimeTelemetry runtimeTelemetry() const override;
    AutoTuning::ShotSubmissionResult submitShot(AutoTuning::ShotRecord const &shot,
                                                AutoTuning::ShotDeliveryAttempt const &attempt) override;
    bool onLiveShotStarted(AutoTuning::LiveShotStarted const &event) override;
    bool onLiveShotSample(AutoTuning::LiveShotSample const &event) override;
    bool onLiveShotEnded(AutoTuning::LiveShotEnded const &event) override;

  private:
    struct Workspace;

    enum class WorkKind : uint8_t {
        LoadPersistedState,
        IngestShot,
        ProcessPreference,
        SwitchControlMode,
        ResetSpline,
        PersistState,
        ResetContext,
        RefreshStatus,
    };

    struct PendingShotWork {
        std::string shotId;
        std::string machineId;
        std::string contextKey;
        std::string beanContextId;
        std::string grinderContextId;
        std::string profileId;
        std::string profileLabel;
        std::string rawProfileHash;
        AutoTuning::TasteGoal tasteGoal = AutoTuning::TasteGoal::balanced();
        std::uint32_t recordRevision = 1;
        AutoTuning::Timestamp timestamp = 0;
        float relativeGrindSteps = 0.0f;
        float hydraulicGrindSteps = 0.0f; // Canonical sign: positive = finer / higher resistance
        float doseG = 18.0f;
        float targetYieldG = 36.0f;
        float beverageOutG = 0.0f;
        float shotTimeS = 0.0f;
        std::array<float, 3> realizedSplineDelta{0.0f, 0.0f, 0.0f};
        bool hasUsableDose = false;
        bool excludeFromOptimization = false;
        std::vector<AutoTuning::ShotSample, PsramStlAllocator<AutoTuning::ShotSample>> samples;
    };

    struct WorkItem {
        WorkKind kind = WorkKind::RefreshStatus;
        std::uint8_t targetControlMode = 0;
        std::unique_ptr<PendingShotWork, PsramDeleter<PendingShotWork>> shot;
        std::optional<AutoTuning::PreferenceFeedback> preference;
    };

    static void workerTaskEntry(void *arg);
    void workerLoop();
    void processWorkItem(WorkItem &item);
    void handleIngestShot(PendingShotWork const &shot);
    void handleProcessPreference(AutoTuning::PreferenceFeedback const &feedback);
    void handleSwitchControlMode(std::uint8_t mode);
    void handleResetSpline();
    bool applyLatestRecommendation(const std::string &recommendationId);
    bool ignoreLatestRecommendation(const std::string &recommendationId);
    void loadPersistedState();
    void loadCommunityPrior();
    bool savePersistedState();
    void clearPersistedState();
    void publishLiveTelemetryEvent(const AutoTuning::OnBoardRuntimeTelemetry &t);
    void publishStatusEvent();
    void queueMainThreadEvent(Event event);
    void drainMainThreadEvents();
    bool enqueueWork(WorkItem item);
    bool isOnBoardSelected() const;
    std::string activeContextKey() const;
    std::string machineId() const;

    Controller *controller_ = nullptr;
    PluginManager *pluginManager_ = nullptr;

    std::unique_ptr<Workspace, PsramDeleter<Workspace>> workspace_;
    mutable std::mutex workspaceMutex_;
    mutable std::mutex telemetryMutex_;
    AutoTuning::OnBoardRuntimeTelemetry cachedTelemetry_{};
    std::mutex workQueueMutex_;
    std::deque<WorkItem> workQueue_;
    std::mutex mainEventMutex_;
    std::deque<Event> mainThreadEvents_;

    TaskHandle_t workerTaskHandle_ = nullptr;
    std::atomic<bool> workspaceReady_{false};
    std::atomic<bool> brewActive_{false};
    std::atomic<bool> otaActive_{false};
    std::atomic<bool> shadowModeOnly_{true}; // Safe default: Shadow Mode ON until explicitly armed.
    std::atomic<std::uint8_t> controlMode_{0}; // 0=recipe_only, 1=spline_only, 2=joint_6d
    std::atomic<bool> persistenceAvailable_{false};
    std::atomic<bool> persistedStateLoaded_{false};
    std::atomic<bool> priorLoaded_{false};

    // Active live-shot parameters captured at brew start & realized live actuation tracking.
    float liveGrindSteps_ = 0.0f; // Canonical sign: positive = finer / higher resistance
    float liveDoseG_ = 18.0f;
    float liveTargetYieldG_ = 36.0f;
    std::uint32_t liveSampleCount_ = 0;
    float maxLiveWeightG_ = 0.0f;
    std::string currentLiveShotId_;
    std::array<float, 3> shotAppliedSplineDelta_{0.0f, 0.0f, 0.0f};
    bool shotHadLiveActuation_ = false;
    std::string lastLiveShotId_;
    std::array<float, 3> lastLiveRealizedSpline_{0.0f, 0.0f, 0.0f};
    std::uint32_t lastStatusPublishMs_ = 0;
};

extern OnBoardEspressoRLPlugin OnBoardEspressoRL;

#endif // defined(GAGGIMATE_ENABLE_ONBOARD_ESPRESSORL) && !defined(GAGGIMATE_SIM)
#endif // ONBOARDESPRESSORLPLUGIN_H
