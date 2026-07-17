#ifndef LOCALAUTOTUNINGSTOREPLUGIN_H
#define LOCALAUTOTUNINGSTOREPLUGIN_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdint>
#include <display/core/AutoTuningPorts.h>

#include <display/core/Event.h>
#include <display/core/Plugin.h>
#include <display/plugins/autotuning/local/LocalAutoTuningContextStore.h>
#include <display/plugins/autotuning/local/LocalAutoTuningSummaryStore.h>

class LocalAutoTuningStorePlugin : public Plugin,
                                   public AutoTuning::LocalOptimizationStorePort,
                                   public AutoTuning::AutoTuningRecordStorePort {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

    bool reset() override;
    AutoTuning::LocalStoreStats stats() const override;
    bool storeShot(AutoTuning::ShotRecord const &shot, AutoTuning::ShotCompletion const &completion,
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
    void handleShotReprocess(Event const &event);
    void handleShotDeliveryAck(Event const &event);
    void handleShotCorrection(Event const &event);
    void handleRecommendationApply(Event const &event);
    void handleRecommendationIgnore(Event const &event);
    void snapshotContexts();
    void publishStatus();
    void refreshDeliveryStatus();
    void recoverPendingDoseConfirmation();
    void dispatchPendingCommunityUploads();
    void processDueDelivery();
    bool emitShotComplete(const String &shotId, JsonDocument &envelope);

    bool ensureDirectories() const;
    bool saveReplaySnapshot(const String &shotId, JsonVariantConst payload, JsonVariantConst completion,
                            bool doseConfirmationRequired, bool localDeliveryRequired, bool communityUploadRequired);
    bool loadReplaySnapshot(const String &shotId, JsonDocument &out) const;
    bool dispatchStoredShot(const String &shotId, bool reprocess, bool automaticRetry = false);
    void prune();
    void pruneReplaySnapshots();

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    LocalAutoTuningContextStore contextStore;
    LocalAutoTuningSummaryStore summaryStore;
    unsigned long lastStatusMs = 0;
    unsigned long lastDeliverySweepMs = 0;
    bool pendingDoseRecoveryChecked = false;
    bool deliveryWorkPending = true;
    std::int64_t nextDeliveryCheckAt = 0;
    int deliveryPendingCount = 0;
    int deliveryRetryCount = 0;
    int deliveryRejectedCount = 0;
    String deliveryLastError;
};

extern LocalAutoTuningStorePlugin LocalAutoTuningStore;

#endif // LOCALAUTOTUNINGSTOREPLUGIN_H
