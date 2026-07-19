#ifndef AUTOTUNINGCAPTUREPLUGIN_H
#define AUTOTUNINGCAPTUREPLUGIN_H

#include "../core/Plugin.h"
#include <display/core/AutoTuningModels.h>
#include <display/core/Event.h>
#include <display/util/PsramStlAllocator.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class AutoTuningCapturePlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    void resetShotCapture();
    void recordShotSample();
    void publishLiveShotStarted();
    void publishLiveShotEnded(const char *endState);
    bool latchShotStop(unsigned long finishedAtMs);
    void trimShotSamplesToElapsed(uint16_t elapsedMs);
    bool trimShotSamplesToInactiveControlTail();
    void appendShotStopSample(uint16_t elapsedMs, float weightG);
    void publishShotProfile();
    void captureCompletedGrindDose();
    void handleRecommendationReceived(Event const &event);
    void clearLatestRecommendation();
    bool shouldCaptureShot() const;
    bool optimizerDeliveryRequired() const;
    String machineTopicId() const;
    String machineId() const;
    String makeShotId() const;
    float doseTargetG() const;
    float currentShotWeightG() const;
    float currentMeasuredWeightG() const;
    float shotWeightAtElapsed(uint16_t elapsedMs) const;
    float currentShotFlowGPerS(float currentWeightG, uint16_t elapsedMs) const;
    float updateMeasuredFlowGPerS(float measuredWeightG, uint16_t elapsedMs);
    const char *weightSourceName() const;
    const char *flowSourceName() const;
    bool pumpFlowCalibrationRequired() const;
    void captureCurrentBrewControl(AutoTuning::PumpTargetMode &pumpTargetMode, bool &valveOpen) const;
    void captureHistoryStartMetadata(std::uint32_t historyId);
    void captureHistoryPhaseTransition();
    std::uint16_t captureHistorySystemInfo() const;

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;

    using ShotSampleVector = std::vector<AutoTuning::ShotSample, PsramStlAllocator<AutoTuning::ShotSample>>;

    bool isBrewing = false;
    unsigned long brewStartMs = 0;
    unsigned long lastSampleMs = 0;
    AutoTuning::Timestamp shotStartedAt = 0;
    AutoTuning::Timestamp liveShotStartedAtMs = 0;
    bool liveShotActive = false;
    uint16_t shotStopElapsedMs = 0;
    float shotStopWeightG = 0.0f;
    String currentShotId;
    int shotSource = 0;
    float measuredFlowGPerS = 0.0f;
    std::atomic<float> currentBluetoothWeight{0.0f};
    std::atomic<float> currentHardwareWeight{0.0f};
    std::atomic<float> currentEstimatedWeight{0.0f};

    bool pendingMeasuredDoseAvailable = false;
    float pendingMeasuredDoseG = 0.0f;
    bool shotMeasuredDoseAvailable = false;
    float shotMeasuredDoseG = 0.0f;

    bool hasRecommendation = false;
    AutoTuning::RecommendationReference latestRecommendation;

    bool shotOptimizerDeliveryRequired = false;
    bool shotCommunityUploadRequired = false;
    bool shotHasRecommendation = false;
    AutoTuning::RecommendationReference shotRecommendation;
    AutoTuning::ShotHistoryMetadata shotHistory;
    std::uint8_t lastHistoryPhase = 0xFF;

    mutable std::mutex captureMutex;
    ShotSampleVector shotSamples;
};

extern AutoTuningCapturePlugin AutoTuningCapture;

#endif // AUTOTUNINGCAPTUREPLUGIN_H
