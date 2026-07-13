#ifndef AUTOTUNINGCAPTUREPLUGIN_H
#define AUTOTUNINGCAPTUREPLUGIN_H

#include "../core/Plugin.h"
#include <display/core/AutoTuningModels.h>
#include <display/core/Event.h>
#include <display/util/PsramStlAllocator.h>
#include <string>
#include <vector>

class AutoTuningCapturePlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override;

  private:
    void resetShotCapture();
    void recordShotSample();
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
    float shotWeightAtElapsed(uint16_t elapsedMs) const;
    float currentShotFlowGPerS(float currentWeightG, uint16_t elapsedMs) const;
    const char *weightSourceName() const;
    const char *flowSourceName() const;
    bool pumpFlowCalibrationRequired() const;
    void captureCurrentBrewControl(AutoTuning::PumpTargetMode &pumpTargetMode, bool &valveOpen) const;

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;

    bool isBrewing = false;
    unsigned long brewStartMs = 0;
    unsigned long lastSampleMs = 0;
    uint16_t shotStopElapsedMs = 0;
    float shotStopWeightG = 0.0f;
    String currentShotId;
    int shotSource = 0;
    float currentBluetoothWeight = 0.0f;
    float currentHardwareWeight = 0.0f;
    float currentEstimatedWeight = 0.0f;

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

    std::vector<AutoTuning::ShotSample, PsramStlAllocator<AutoTuning::ShotSample>> shotSamples;
};

extern AutoTuningCapturePlugin AutoTuningCapture;

#endif // AUTOTUNINGCAPTUREPLUGIN_H
