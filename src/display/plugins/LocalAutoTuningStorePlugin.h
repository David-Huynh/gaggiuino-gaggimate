#ifndef LOCALAUTOTUNINGSTOREPLUGIN_H
#define LOCALAUTOTUNINGSTOREPLUGIN_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <display/core/AutoTuningPorts.h>
#include <display/util/PsramStlAllocator.h>
#include <memory>
#include <mutex>
#include <vector>

#include <display/core/Event.h>
#include <display/core/Plugin.h>
#include <display/plugins/autotuning/local/LocalAutoTuningContextStore.h>
#include <display/plugins/autotuning/local/CompletedShotArtifactStore.h>
#include <display/plugins/autotuning/local/LocalAutoTuningSummaryStore.h>

class LocalAutoTuningStorePlugin : public Plugin,
                                   public AutoTuning::LocalOptimizationStorePort,
                                   public AutoTuning::AutoTuningRecordStorePort {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

    bool reset() override;
    AutoTuning::LocalStoreStats stats() const override;
    bool enqueueShot(AutoTuning::ShotRecord const &shot, AutoTuning::ShotCompletion const &completion,
                     AutoTuning::ShotCaptureDisposition const &disposition) override;
    bool storeRecommendation(AutoTuning::Recommendation const &recommendation) override;
    bool correctShot(AutoTuning::ShotCorrection const &correction, AutoTuning::CorrectedShotRecord &corrected,
                     std::string &reason) override;
    bool loadShotSummary(const String &shotId, JsonDocument &out) const;
    bool hasShotReplay(const String &shotId) const;
    bool canRemoveShotData(const String &shotId) const;
    bool removeShotData(const String &shotId, bool force = false);

  private:
    void handleShotDispatch(Event const &event);
    void handleDoseConfirmation(Event const &event);
    void handleShotReprocess(Event &event);
    void handleShotDeliveryAck(Event const &event);
    void processShotDeliveryAck(const String &shotId, const String &outcome, const String &reason,
                                std::int64_t acknowledgementTimestamp,
                                std::uint32_t recordRevision,
                                std::optional<AutoTuning::PreferenceRequest> const &preferenceRequest);
    void handleShotCorrection(Event const &event);
    void handleRecommendationApply(Event const &event);
    void handleRecommendationIgnore(Event const &event);
    void handlePromptClaim(Event &event);
    void handlePreferencePersisted(Event const &event);
    void snapshotContexts();
    void publishStatus();
    void requestStatusRefresh();
    void refreshCachedStatus();
    void refreshDeliveryStatus();
    void recoverPendingDoseConfirmation();
    void dispatchPendingCommunityUploads();
    void processDueDelivery();
    bool prepareShotComplete(const String &shotId, JsonDocument &envelope);
    void drainStoredShots();
    void drainShotCompletions();
    bool markShotCompletionEmitted(const String &shotId);
    bool resolvePrompt(const String &shotId, std::uint32_t revision);

    using ShotSampleVector = std::vector<AutoTuning::ShotSample, PsramStlAllocator<AutoTuning::ShotSample>>;
    struct QueuedShot {
        AutoTuning::ShotRecord shot;
        AutoTuning::ShotCompletion completion;
        AutoTuning::ShotCaptureDisposition disposition;
        ShotSampleVector samples;
        AutoTuning::Timestamp committedAt = 0;
    };
    struct StoredShotNotice {
        String shotId;
        bool doseConfirmationRequired = false;
        float doseTargetG = 0.0f;
        std::uint32_t promptRevision = 0;
    };
    enum class WorkKind : std::uint8_t {
        StoreShot,
        Reprocess,
        Dispatch,
        DeliveryAcknowledgement,
        DeliverySweep,
        CommunitySweep,
        DoseConfirmationRecovery,
        StatusRefresh,
        MarkCompletionEmitted,
        RecoverCommittedArtifacts,
        StoreRecommendation,
        SnapshotContexts,
        PatchShotCorrection,
        PatchRecommendationStatus,
        ResolvePrompt,
    };
    struct WorkItem {
        WorkKind kind = WorkKind::DeliverySweep;
        String shotId;
        String outcome;
        String reason;
        std::int64_t timestamp = 0;
        std::uint32_t recordRevision = 0;
        std::uint32_t promptRevision = 0;
        std::optional<AutoTuning::PreferenceRequest> preferenceRequest;
        bool reprocess = false;
        bool automaticRetry = false;
        std::unique_ptr<QueuedShot> queuedShot;
        std::unique_ptr<AutoTuning::Recommendation> recommendation;
        std::unique_ptr<AutoTuning::ShotCorrection> correction;
        String recommendationId;
        String recommendationStatus;
        String recommendationTimestampField;
    };
    struct CompletionNotice {
        String shotId;
        AutoTuning::ShotCompletion completion;
        std::uint32_t promptRevision = 0;
    };
    struct ClaimablePrompt {
        String shotId;
        std::uint32_t revision = 0;
        bool claimed = false;
    };
    bool enqueueWork(WorkItem work);
    bool processOneWorkItem();
    static void workerTask(void *arg);

    bool persistShot(AutoTuning::ShotRecord const &shot, AutoTuning::ShotCompletion const &completion,
                     AutoTuning::ShotCaptureDisposition const &disposition,
                     AutoTuning::Timestamp committedAt);
    bool persistRecommendation(AutoTuning::Recommendation const &recommendation);
    void persistContexts();
    bool prepareShotReprocess(const String &shotId);
    bool ensureDirectories();
    bool saveReplaySnapshot(AutoTuning::CompletedShotArtifact const &artifact);
    bool loadReplaySnapshot(const String &shotId, JsonDocument &out) const;
    bool loadCommittedShot(const String &shotId, AutoTuning::CompletedShotArtifact &artifact) const;
    bool recoverCommittedArtifacts();
    bool updateReplayArtifactIdentity(JsonObject root,
                                      AutoTuning::CompletedShotArtifact const &artifact) const;
    bool upsertArtifactSummary(AutoTuning::CompletedShotArtifact const &artifact);
    bool transitionPrompt(JsonObject root, AutoTuning::PromptStatus status,
                          std::int64_t timestamp) const;
    bool dispatchStoredShot(const String &shotId, bool reprocess, bool automaticRetry = false);
    void prune();
    void pruneReplaySnapshots();

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    LocalAutoTuningContextStore contextStore;
    CompletedShotArtifactStore artifactStore;
    LocalAutoTuningSummaryStore summaryStore;
    bool artifactStoreReady = false;
    mutable std::recursive_mutex storeMutex;
    TaskHandle_t workerTaskHandle = nullptr;
    std::mutex workMutex;
    std::deque<WorkItem> workItems;
    std::deque<StoredShotNotice> storedShotNotices;
    std::deque<CompletionNotice> completionNotices;
    std::vector<String> queuedCompletionShotIds;
    std::vector<ClaimablePrompt> claimablePrompts;
    std::atomic_bool workerStartAttempted{false};
    mutable std::mutex statusMutex;
    AutoTuning::LocalStoreStats cachedStats;
    std::atomic_bool statusPublishRequested{false};
    unsigned long lastStatusMs = 0;
    unsigned long lastDeliverySweepMs = 0;
    std::atomic_bool pendingDoseRecoveryChecked{false};
    std::atomic_bool deliveryWorkPending{true};
    std::atomic<std::int64_t> nextDeliveryCheckAt{0};
    int deliveryPendingCount = 0;
    int deliveryRetryCount = 0;
    int deliveryRejectedCount = 0;
    String deliveryLastError;
    static constexpr std::size_t MAX_PENDING_SHOT_WRITES = 4;
};

extern LocalAutoTuningStorePlugin LocalAutoTuningStore;

#endif // LOCALAUTOTUNINGSTOREPLUGIN_H
