#include "AutoTuningCapturePlugin.h"
#include "autotuning/AutoTuningPayloadMetadata.h"

#include <WiFi.h>
#include <algorithm>
#include <cmath>
#include <display/core/AutoTuning.h>
#include <display/core/Controller.h>
#include <display/core/EpochTime.h>
#include <display/core/PluginManager.h>
#include <display/core/process/BrewProcess.h>
#include <display/core/process/GrindProcess.h>
#include <esp_system.h>
#include <mutex>

namespace {
constexpr float MAX_CANONICAL_FLOW_G_PER_S = 20.0f;
constexpr int SHOT_SAMPLE_INTERVAL_MS = 250;
constexpr int SHOT_MAX_SAMPLES = 240; // 60s at 4Hz
constexpr float SHOT_TARGET_ACTIVE_EPSILON = 0.001f;
constexpr size_t SHOT_FINISH_SETTLE_SAMPLES = 2;
constexpr float SHOT_FINISH_SETTLE_MAX_DELTA_G = 1.0f;

static const char *phaseTypeName(PhaseType phase) {
    switch (phase) {
    case PhaseType::PHASE_TYPE_PREINFUSION:
        return "preinfusion";
    case PhaseType::PHASE_TYPE_BREW:
        return "brew";
    default:
        return "brew";
    }
}

static const char *pumpTargetName(bool pumpIsSimple, PumpTarget pumpTarget) {
    if (pumpIsSimple) {
        return "simple";
    }
    switch (pumpTarget) {
    case PumpTarget::PUMP_TARGET_PRESSURE:
        return "pressure";
    case PumpTarget::PUMP_TARGET_FLOW:
        return "flow";
    default:
        return "pressure";
    }
}

static const char *shotEndStateName(bool finished) { return finished ? "finished" : "manual_or_interrupted"; }

struct BrewProcessSnapshot {
    bool available = false;
    double stopMeasuredVolume = 0.0;
    bool stopPredictionApplied = false;
    double brewDelay = 0.0;
    double stopVolumetricRate = 0.0;
    double stopPredictedAddedVolume = 0.0;
    double stopPredictedVolume = 0.0;
    unsigned int phaseIndex = 0;
    String phaseName;
    PhaseType phaseType = PhaseType::PHASE_TYPE_BREW;
    float phaseElapsedS = 0.0f;
    bool pumpIsSimple = true;
    PumpTarget pumpTarget = PumpTarget::PUMP_TARGET_PRESSURE;
    float targetPressure = 0.0f;
    float targetFlow = 0.0f;
    bool valveOpen = false;
    float temperature = 0.0f;
    bool finished = false;
};

} // namespace

AutoTuningCapturePlugin AutoTuningCapture;

void AutoTuningCapturePlugin::setup(Controller *ctrl, PluginManager *pm) {
    this->controller = ctrl;
    this->pluginManager = pm;
    shotSamples.reserve(SHOT_MAX_SAMPLES);

    pm->on("controller:volumetric-measurement:estimation:change",
           [this](Event const &event) { currentEstimatedWeight = event.getFloat("value"); });
    pm->on("controller:volumetric-measurement:bluetooth:change",
           [this](Event const &event) { currentBluetoothWeight = event.getFloat("value"); });
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    pm->on("controller:volumetric-measurement:hardware:change",
           [this](Event const &event) { currentHardwareWeight = event.getFloat("value"); });
#endif

    pm->on("controller:grind:start", [this](Event const &) {
        pendingMeasuredDoseAvailable = false;
        pendingMeasuredDoseG = 0.0f;
    });
    pm->on("controller:grind:end", [this](Event const &) { captureCompletedGrindDose(); });

    pm->on("rl:recommendation:received", [this](Event const &event) { handleRecommendationReceived(event); });
    pm->on("rl:recommendation:cleared", [this](Event const &) { clearLatestRecommendation(); });

    pm->on("controller:brew:start", [this](Event const &event) {
        if (shouldCaptureShot() && event.getInt("utility") == 0) {
            resetShotCapture();
            shotOptimizerDeliveryRequired = optimizerDeliveryRequired();
            shotCommunityUploadRequired = controller->getSettings().isRLCommunityUploadEnabled();
            if (shotOptimizerDeliveryRequired && hasRecommendation) {
                shotHasRecommendation = true;
                shotRecommendation = latestRecommendation;
            }
            shotMeasuredDoseAvailable = pendingMeasuredDoseAvailable;
            shotMeasuredDoseG = pendingMeasuredDoseG;
            pendingMeasuredDoseAvailable = false;
            pendingMeasuredDoseG = 0.0f;
            isBrewing = true;
            brewStartMs = millis();
            currentShotId = makeShotId();
            shotSource = static_cast<int>(controller->getCurrentVolumetricSource());
            currentBluetoothWeight = 0.0f;
            currentHardwareWeight = 0.0f;
            currentEstimatedWeight = 0.0f;
        }
    });

    pm->on("controller:brew:end", [this](Event const &) {
        const bool capturedEspressoShot = isBrewing && !shotSamples.empty();
        if (capturedEspressoShot) {
            unsigned long finishedAtMs = 0;
            if (controller) {
                std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
                Process *lastProcess = controller->getLastProcess();
                if (lastProcess && lastProcess->getType() == MODE_BREW) {
                    finishedAtMs = static_cast<BrewProcess *>(lastProcess)->finished;
                }
            }
            latchShotStop(finishedAtMs);
            if (shotStopElapsedMs == 0) {
                const unsigned long stopElapsedMs = millis() - brewStartMs;
                shotStopElapsedMs = static_cast<uint16_t>(std::min<unsigned long>(stopElapsedMs, 0xFFFFUL));
            }
            trimShotSamplesToElapsed(shotStopElapsedMs);
            isBrewing = false;
            publishShotProfile();
        }
        resetShotCapture();
    });
}

void AutoTuningCapturePlugin::loop() {
    if (!isBrewing)
        return;
    unsigned long finishedAtMs = 0;
    if (controller) {
        std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
        Process *process = controller->getProcess();
        if (process && process->getType() == MODE_BREW) {
            finishedAtMs = static_cast<BrewProcess *>(process)->finished;
        }
    }
    if (latchShotStop(finishedAtMs) || shotStopElapsedMs > 0) {
        return;
    }
    unsigned long elapsed = millis() - brewStartMs;
    if (elapsed - lastSampleMs >= SHOT_SAMPLE_INTERVAL_MS && shotSamples.size() < SHOT_MAX_SAMPLES) {
        recordShotSample();
        lastSampleMs = elapsed;
    }
}

void AutoTuningCapturePlugin::resetShotCapture() {
    isBrewing = false;
    brewStartMs = 0;
    lastSampleMs = 0;
    shotStopElapsedMs = 0;
    shotStopWeightG = 0.0f;
    currentShotId = "";
    shotMeasuredDoseAvailable = false;
    shotMeasuredDoseG = 0.0f;
    shotOptimizerDeliveryRequired = false;
    shotCommunityUploadRequired = false;
    shotHasRecommendation = false;
    shotRecommendation = AutoTuning::RecommendationReference{};
    shotSamples.clear();
}

void AutoTuningCapturePlugin::recordShotSample() {
    const uint16_t elapsedMs = static_cast<uint16_t>(millis() - brewStartMs);
    const float shotWeightG = currentShotWeightG();
    const float beverageFlowGPerS = currentShotFlowGPerS(shotWeightG, elapsedMs);
    AutoTuning::PumpTargetMode pumpTargetMode = AutoTuning::PumpTargetMode::Simple;
    bool valveOpen = false;
    captureCurrentBrewControl(pumpTargetMode, valveOpen);

    AutoTuning::ShotSample sample;
    sample.pressure = controller->getCurrentPressure();
    sample.targetPressure = controller->getTargetPressure();
    sample.flow = beverageFlowGPerS;
    sample.pumpFlow = controller->getCurrentPumpFlow();
    sample.targetFlow = controller->getTargetFlow();
    sample.temperature = controller->getCurrentTemp();
    sample.targetTemperature = controller->getTargetTemp();
    sample.weight = shotWeightG;
    sample.pumpTargetMode = pumpTargetMode;
    sample.valveOpen = valveOpen;
    sample.elapsedMs = elapsedMs;
    shotSamples.push_back(sample);
}

bool AutoTuningCapturePlugin::latchShotStop(unsigned long finishedAtMs) {
    if (shotStopElapsedMs > 0 || finishedAtMs == 0)
        return shotStopElapsedMs > 0;

    unsigned long stopElapsedMs = millis() - brewStartMs;
    if (finishedAtMs >= brewStartMs) {
        stopElapsedMs = finishedAtMs - brewStartMs;
    }
    shotStopElapsedMs = static_cast<uint16_t>(std::min<unsigned long>(stopElapsedMs, 0xFFFFUL));
    shotStopWeightG = shotWeightAtElapsed(shotStopElapsedMs);
    trimShotSamplesToElapsed(shotStopElapsedMs);
    appendShotStopSample(shotStopElapsedMs, shotStopWeightG);
    return true;
}

void AutoTuningCapturePlugin::trimShotSamplesToElapsed(uint16_t elapsedMs) {
    if (elapsedMs == 0 || shotSamples.empty())
        return;

    size_t keep = shotSamples.size();
    while (keep > 1 && shotSamples[keep - 1].elapsedMs > elapsedMs) {
        keep--;
    }
    if (keep >= shotSamples.size())
        return;
    shotSamples.resize(keep);
}

bool AutoTuningCapturePlugin::trimShotSamplesToInactiveControlTail() {
    const size_t sampleCount = shotSamples.size();
    if (sampleCount < 3) {
        return false;
    }

    bool sawActiveTarget = false;
    for (size_t i = 0; i < sampleCount; i++) {
        const AutoTuning::ShotSample &sample = shotSamples[i];
        const bool targetActive = std::fabs(sample.targetPressure) > SHOT_TARGET_ACTIVE_EPSILON ||
                                  std::fabs(sample.targetFlow) > SHOT_TARGET_ACTIVE_EPSILON;
        if (targetActive) {
            sawActiveTarget = true;
            continue;
        }
        if (!sawActiveTarget)
            continue;

        const bool controlsInactive = sample.pumpTargetMode == AutoTuning::PumpTargetMode::Simple && !sample.valveOpen;
        if (!controlsInactive)
            continue;

        size_t keep = std::max<size_t>(2, i);
        if (i > 0) {
            size_t settledKeep = keep;
            float previousWeight = shotSamples[i - 1].weight;
            const size_t settleEnd = std::min(sampleCount, i + SHOT_FINISH_SETTLE_SAMPLES);
            for (size_t j = i; j < settleEnd; j++) {
                const float currentWeight = shotSamples[j].weight;
                if (!std::isfinite(previousWeight) || !std::isfinite(currentWeight))
                    break;
                if (std::fabs(currentWeight - previousWeight) > SHOT_FINISH_SETTLE_MAX_DELTA_G)
                    break;
                settledKeep = j + 1;
                previousWeight = currentWeight;
            }
            keep = std::max(keep, settledKeep);
        }
        if (keep >= sampleCount)
            return false;

        shotSamples.resize(keep);
        shotStopElapsedMs = shotSamples.back().elapsedMs;
        shotStopWeightG = shotSamples.back().weight;
        return true;
    }
    return false;
}

void AutoTuningCapturePlugin::appendShotStopSample(uint16_t elapsedMs, float weightG) {
    if (elapsedMs == 0 || shotSamples.empty())
        return;

    if (shotSamples.back().elapsedMs == elapsedMs) {
        shotSamples.back().weight = weightG;
        return;
    }

    AutoTuning::ShotSample sample = shotSamples.back();
    const float stopFlow = currentShotFlowGPerS(weightG, elapsedMs);
    sample.flow = stopFlow;
    sample.weight = weightG;
    sample.elapsedMs = elapsedMs;
    if (shotSamples.size() >= SHOT_MAX_SAMPLES) {
        shotSamples.back() = sample;
    } else {
        shotSamples.push_back(sample);
    }
}

void AutoTuningCapturePlugin::publishShotProfile() {
    if (shotSamples.empty())
        return;

    if (shotStopElapsedMs > 0) {
        trimShotSamplesToElapsed(shotStopElapsedMs);
    }
    trimShotSamplesToInactiveControlTail();
    const uint16_t shotElapsedMs =
        shotStopElapsedMs > 0 ? shotStopElapsedMs : (!shotSamples.empty() ? shotSamples.back().elapsedMs : 0);
    float beverageOutG =
        shotStopWeightG > 0.0f ? shotStopWeightG : (!shotSamples.empty() ? shotSamples.back().weight : currentShotWeightG());
    BrewProcessSnapshot brew;
    if (controller) {
        std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
        Process *lastProcess = controller->getLastProcess();
        if (lastProcess && lastProcess->getType() == MODE_BREW) {
            auto *process = static_cast<BrewProcess *>(lastProcess);
            const Phase &phase = process->currentPhase;
            const unsigned long phaseEndMs = process->finished > 0 ? process->finished : millis();

            brew.available = true;
            brew.stopMeasuredVolume = process->stopMeasuredVolume;
            brew.stopPredictionApplied = process->stopPredictionApplied;
            brew.brewDelay = process->brewDelay;
            brew.stopVolumetricRate = process->stopVolumetricRate;
            brew.stopPredictedAddedVolume = process->stopPredictedAddedVolume;
            brew.stopPredictedVolume = process->stopPredictedVolume;
            brew.phaseIndex = process->phaseIndex;
            brew.phaseName = phase.name;
            brew.phaseType = phase.phase;
            brew.phaseElapsedS =
                phaseEndMs >= process->currentPhaseStarted ? (phaseEndMs - process->currentPhaseStarted) / 1000.0f : 0.0f;
            brew.pumpIsSimple = phase.pumpIsSimple;
            if (!phase.pumpIsSimple) {
                brew.pumpTarget = phase.pumpAdvanced.target;
                brew.targetPressure = phase.pumpAdvanced.pressure;
                brew.targetFlow = phase.pumpAdvanced.flow;
            }
            brew.valveOpen = phase.valve > 0;
            brew.temperature = process->getTemperature();
            brew.finished = process->processPhase == ProcessPhase::FINISHED;

            if (std::isfinite(brew.stopMeasuredVolume) && brew.stopMeasuredVolume > 0.0) {
                beverageOutG = static_cast<float>(brew.stopMeasuredVolume);
            }
        }
    }

    AutoTuning::ShotRecord shot;
    const String shotId = currentShotId.isEmpty() ? makeShotId() : currentShotId;
    shot.shotId = shotId.c_str();
    shot.machineId = machineId().c_str();
    shot.timestamp = EpochTime::now();
    shot.localOptimizationEnabled = shotOptimizerDeliveryRequired;
    // Enabling mid-shot cannot opt an earlier sample stream in, while disabling
    // remains an immediate consent veto before anything is queued.
    shot.communityUploadEnabled =
        shotCommunityUploadRequired && controller && controller->getSettings().isRLCommunityUploadEnabled();
    shot.weightSource = weightSourceName();
    shot.flowSource = flowSourceName();
    shot.pumpFlowInterpretation = "available pump flow at current pressure scaled by pump duty";
    shot.pumpFlowCalibrationRequired = pumpFlowCalibrationRequired();
    if (shot.pumpFlowCalibrationRequired) {
        shot.predictiveWeightInterpretation = "estimated beverage output from pump/puck model";
    }
    shot.beverageOutG = roundf(beverageOutG * 10.0f) / 10.0f;
    shot.shotTimeS = roundf((shotElapsedMs / 1000.0f) * 10.0f) / 10.0f;
    AutoTuningPayloadMetadata::captureRecipe(controller, shot.recipe);
    AutoTuningPayloadMetadata::captureProfile(controller, shot.profile);
    const float configuredDoseG = doseTargetG();
    if (configuredDoseG > 0.0f) {
        shot.recipe.doseTargetG = roundf(configuredDoseG * 10.0f) / 10.0f;
    }
    if (shotMeasuredDoseAvailable) {
        shot.measuredDoseG = roundf(shotMeasuredDoseG * 10.0f) / 10.0f;
        shot.doseObserved = true;
    }
    if (brew.available) {
        shot.predictiveStopApplied = brew.stopPredictionApplied;
        shot.predictiveStopDelayMs = static_cast<float>(brew.brewDelay);
        shot.predictiveStopRateGPerS = static_cast<float>(brew.stopVolumetricRate * 1000.0);
        shot.predictiveStopLeadG = static_cast<float>(brew.stopPredictedAddedVolume);
        if (brew.stopPredictionApplied && std::isfinite(brew.stopPredictedVolume) && brew.stopPredictedVolume > 0.0) {
            shot.predictedFinalBeverageOutG = static_cast<float>(brew.stopPredictedVolume);
        }
        shot.finalPhase.available = true;
        shot.finalPhase.index = brew.phaseIndex;
        shot.finalPhase.name = brew.phaseName.c_str();
        shot.finalPhase.type = phaseTypeName(brew.phaseType);
        shot.finalPhase.elapsedS = roundf(brew.phaseElapsedS * 10.0f) / 10.0f;
        shot.finalPhase.pumpTarget = pumpTargetName(brew.pumpIsSimple, brew.pumpTarget);
        if (!brew.pumpIsSimple && brew.targetPressure >= 0.0f) {
            shot.finalPhase.targetPressure = brew.targetPressure;
        }
        if (!brew.pumpIsSimple && brew.targetFlow >= 0.0f) {
            shot.finalPhase.targetFlow = brew.targetFlow;
        }
        shot.finalPhase.valveOpen = brew.valveOpen;
        shot.finalPhase.temperatureC = brew.temperature;
        shot.finalPhase.shotEndState = shotEndStateName(brew.finished);
    }
    if (shotHasRecommendation) {
        shot.recommendation = shotRecommendation;
    }
    shot.samples = AutoTuning::ArrayView<const AutoTuning::ShotSample>(shotSamples.data(), shotSamples.size());

    const bool localDeliveryRequired = shotOptimizerDeliveryRequired;
    const bool doseConfirmationRequired = localDeliveryRequired && !shotMeasuredDoseAvailable;
    AutoTuning::ShotCompletion completion;
    completion.shotId = shot.shotId;
    completion.recommendation = shotRecommendation;
    completion.doseTargetG = configuredDoseG;
    AutoTuning::ShotCaptureDisposition disposition;
    disposition.doseConfirmationRequired = doseConfirmationRequired;
    disposition.optimizerDeliveryRequired = localDeliveryRequired;
    disposition.communityUploadRequired = shot.communityUploadEnabled;

    AutoTuning::AutoTuningRecordStorePort *store = controller ? controller->getAutoTuningRecordStore() : nullptr;
    if (!store || !store->storeShot(shot, completion, disposition) || !pluginManager) {
        ESP_LOGE("AutoTuningCapture", "Failed to persist shot %s", shotId.c_str());
        return;
    }

    Event capturedEvent;
    capturedEvent.id = "rl:shot:captured";
    capturedEvent.setString("shot_id", shotId);
    capturedEvent.setString("recommendation_id", shotRecommendation.recommendationId.c_str());
    pluginManager->trigger(capturedEvent);

    if (doseConfirmationRequired) {
        Event confirmationEvent;
        confirmationEvent.id = "rl:dose-confirmation:required";
        confirmationEvent.setString("shot_id", shotId);
        confirmationEvent.setFloat("dose_target_g", configuredDoseG);
        pluginManager->trigger(confirmationEvent);
    } else {
        Event dispatchEvent;
        dispatchEvent.id = "rl:shot:dispatch";
        dispatchEvent.setString("shot_id", shotId);
        pluginManager->trigger(dispatchEvent);
    }
}

void AutoTuningCapturePlugin::captureCompletedGrindDose() {
    pendingMeasuredDoseAvailable = false;
    pendingMeasuredDoseG = 0.0f;
    if (!controller) {
        return;
    }
    double measuredDoseG = 0.0;
    {
        std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
        Process *lastProcess = controller->getLastProcess();
        if (!lastProcess || lastProcess->getType() != MODE_GRIND) {
            return;
        }
        auto *grind = static_cast<GrindProcess *>(lastProcess);
        if (grind->target != ProcessTarget::VOLUMETRIC || !std::isfinite(grind->currentVolume) ||
            grind->currentVolume < AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G ||
            grind->currentVolume > AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G) {
            return;
        }
        measuredDoseG = grind->currentVolume;
    }
    pendingMeasuredDoseG = static_cast<float>(measuredDoseG);
    pendingMeasuredDoseAvailable = true;
}

void AutoTuningCapturePlugin::handleRecommendationReceived(Event const &event) {
    AutoTuning::Recommendation const *recommendation = event.getPayload<AutoTuning::Recommendation>();
    if (!recommendation) {
        clearLatestRecommendation();
        return;
    }
    hasRecommendation = true;
    latestRecommendation.recommendationId = recommendation->recommendationId;
    latestRecommendation.installId = recommendation->installId;
    latestRecommendation.optimizationRunId = recommendation->optimizationRunId;
    latestRecommendation.anchorShotId = recommendation->comparisonAnchorShotId;
    latestRecommendation.comparisonMode = recommendation->comparisonMode;
    latestRecommendation.tasteGoal = recommendation->tasteGoal;
    latestRecommendation.preferenceFeedbackRequired = recommendation->preferenceFeedbackRequired;
    latestRecommendation.grindDeltaStepsFromCurrent = recommendation->grindDeltaStepsFromCurrent;
    latestRecommendation.grindDeltaMicronsFromCurrent = recommendation->grindDeltaMicronsFromCurrent;
    latestRecommendation.projectedRelativeStepFromReference = recommendation->projectedRelativeStepFromReference;
    latestRecommendation.projectedRelativeMicronsFromReference = recommendation->projectedRelativeMicronsFromReference;
    latestRecommendation.nextDoseG = recommendation->nextDoseG;
    latestRecommendation.targetYieldG = recommendation->targetYieldG;
    latestRecommendation.targetRatio = recommendation->targetRatio;
}

void AutoTuningCapturePlugin::clearLatestRecommendation() {
    hasRecommendation = false;
    latestRecommendation = AutoTuning::RecommendationReference{};
}

bool AutoTuningCapturePlugin::shouldCaptureShot() const {
    return controller && (optimizerDeliveryRequired() || controller->getSettings().isRLCommunityUploadEnabled());
}

bool AutoTuningCapturePlugin::optimizerDeliveryRequired() const {
    if (!controller)
        return false;
    Settings const &settings = controller->getSettings();
    return AutoTuning::Router(settings.getRLOptimizerConfiguration(), controller->getOptimizerTransport()).optimizationActive();
}

String AutoTuningCapturePlugin::machineTopicId() const {
    String mac = WiFi.macAddress();
    mac.replace(":", "_");
    return mac;
}

String AutoTuningCapturePlugin::machineId() const { return "gaggimate:" + machineTopicId(); }

String AutoTuningCapturePlugin::makeShotId() const {
    char randomId[34];
    snprintf(randomId, sizeof(randomId), "%08lx%08lx%08lx%08lx", static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
    return "shot_" + machineTopicId() + "_" + String(randomId);
}

float AutoTuningCapturePlugin::doseTargetG() const {
    if (!controller)
        return 0.0f;
    return static_cast<float>(controller->getSettings().getTargetGrindVolume());
}

float AutoTuningCapturePlugin::currentShotWeightG() const {
    if (!controller)
        return currentBluetoothWeight;

    switch (static_cast<VolumetricMeasurementSource>(shotSource)) {
    case VolumetricMeasurementSource::HARDWARE_SCALE:
        return currentHardwareWeight;
    case VolumetricMeasurementSource::FLOW_ESTIMATION:
        return currentEstimatedWeight;
    case VolumetricMeasurementSource::BLUETOOTH:
        return currentBluetoothWeight;
    case VolumetricMeasurementSource::INACTIVE:
    default:
        return currentBluetoothWeight > 0.0f ? currentBluetoothWeight : currentEstimatedWeight;
    }
}

float AutoTuningCapturePlugin::shotWeightAtElapsed(uint16_t elapsedMs) const {
    const size_t sampleCount = shotSamples.size();
    if (sampleCount == 0) {
        return currentShotWeightG();
    }
    if (elapsedMs <= shotSamples.front().elapsedMs) {
        return shotSamples.front().weight;
    }
    for (size_t i = 1; i < sampleCount; i++) {
        const uint16_t previousMs = shotSamples[i - 1].elapsedMs;
        const uint16_t currentMs = shotSamples[i].elapsedMs;
        if (elapsedMs > currentMs)
            continue;
        const float previousWeight = shotSamples[i - 1].weight;
        const float currentWeight = shotSamples[i].weight;
        if (currentMs <= previousMs)
            return previousWeight;
        const float alpha = static_cast<float>(elapsedMs - previousMs) / static_cast<float>(currentMs - previousMs);
        return previousWeight + (currentWeight - previousWeight) * alpha;
    }
    return shotSamples.back().weight;
}

float AutoTuningCapturePlugin::currentShotFlowGPerS(float currentWeightG, uint16_t elapsedMs) const {
    if (shotSamples.empty()) {
        return 0.0f;
    }
    const uint16_t previousMs = shotSamples.back().elapsedMs;
    if (elapsedMs <= previousMs) {
        return 0.0f;
    }
    const float dtS = static_cast<float>(elapsedMs - previousMs) / 1000.0f;
    if (dtS <= 0.0f || !std::isfinite(dtS) || !std::isfinite(currentWeightG)) {
        return 0.0f;
    }
    const float previousWeightG = shotSamples.back().weight;
    if (!std::isfinite(previousWeightG) || previousWeightG < 0.0f || currentWeightG < 0.0f) {
        return 0.0f;
    }
    const float flow = (currentWeightG - previousWeightG) / dtS;
    if (!std::isfinite(flow) || flow <= 0.0f) {
        return 0.0f;
    }
    return std::min(flow, MAX_CANONICAL_FLOW_G_PER_S);
}

const char *AutoTuningCapturePlugin::weightSourceName() const {
    switch (static_cast<VolumetricMeasurementSource>(shotSource)) {
    case VolumetricMeasurementSource::HARDWARE_SCALE:
        return "hardware_scale";
    case VolumetricMeasurementSource::BLUETOOTH:
        return "bluetooth_scale";
    case VolumetricMeasurementSource::FLOW_ESTIMATION:
        return "predictive_pump_flow";
    case VolumetricMeasurementSource::INACTIVE:
    default:
        return "unknown";
    }
}

const char *AutoTuningCapturePlugin::flowSourceName() const {
    switch (static_cast<VolumetricMeasurementSource>(shotSource)) {
    case VolumetricMeasurementSource::HARDWARE_SCALE:
    case VolumetricMeasurementSource::BLUETOOTH:
        return "beverage_weight_derivative";
    case VolumetricMeasurementSource::FLOW_ESTIMATION:
        return "predictive_pump_model_derivative";
    case VolumetricMeasurementSource::INACTIVE:
    default:
        return "unknown";
    }
}

bool AutoTuningCapturePlugin::pumpFlowCalibrationRequired() const {
    return static_cast<VolumetricMeasurementSource>(shotSource) == VolumetricMeasurementSource::FLOW_ESTIMATION;
}

void AutoTuningCapturePlugin::captureCurrentBrewControl(AutoTuning::PumpTargetMode &pumpTargetMode, bool &valveOpen) const {
    pumpTargetMode = AutoTuning::PumpTargetMode::Simple;
    valveOpen = false;
    if (!controller)
        return;

    std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
    Process *process = controller->getProcess();
    if (!process || process->getType() != MODE_BREW)
        return;

    auto *brew = static_cast<BrewProcess *>(process);
    if (brew->isAdvancedPump()) {
        switch (brew->getPumpTarget()) {
        case PumpTarget::PUMP_TARGET_PRESSURE:
            pumpTargetMode = AutoTuning::PumpTargetMode::Pressure;
            break;
        case PumpTarget::PUMP_TARGET_FLOW:
            pumpTargetMode = AutoTuning::PumpTargetMode::Flow;
            break;
        default:
            break;
        }
    }
    valveOpen = brew->isActive() && brew->currentPhase.valve > 0;
}
