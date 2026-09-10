#include "ShotHistoryPlugin.h"
#include "LocalAutoTuningStorePlugin.h"

#include <LittleFS.h>
#include <SD_MMC.h>
#include <cmath>
#include <display/core/Controller.h>
#include <display/core/ProfileManager.h>
#include <display/core/StorageCoordinator.h>
#include <display/core/process/BrewProcess.h>
#include <display/core/utils.h>
#include <display/models/shot_log_format.h>
#include <display/util/LittleFSUtil.h>
#include <display/util/PsramAllocator.h>

namespace {
constexpr float TEMP_SCALE = 10.0f;
constexpr float PRESSURE_SCALE = 10.0f;
constexpr float FLOW_SCALE = 100.0f;
constexpr float WEIGHT_SCALE = 10.0f;
constexpr float RESISTANCE_SCALE = 100.0f;

constexpr uint16_t TEMP_MAX_VALUE = 2000;    // 200.0 °C
constexpr uint16_t PRESSURE_MAX_VALUE = 200; // 20.0 bar
constexpr uint16_t WEIGHT_MAX_VALUE = 10000; // 1000.0 g
constexpr uint16_t RESISTANCE_MAX_VALUE = 0xFFFF;
constexpr int16_t FLOW_MIN_VALUE = -2000; // -20.00 ml/s
constexpr int16_t FLOW_MAX_VALUE = 2000;  //  20.00 ml/s

// Largest believable change in scale weight within one sample interval. A real
// espresso never adds >5 g in 250 ms (that is already 20 ml/s, the saturation
// point of the vf field); anything larger is a scale/BLE glitch and must not be
// folded into the flow EMA, where a single bad reading would otherwise pin vf at
// the ±20 floor for seconds while the EMA bleeds off. See GM-110.
constexpr float MAX_PLAUSIBLE_WEIGHT_DELTA = 5.0f; // grams per sample
constexpr float SHOT_HISTORY_TARGET_ACTIVE_EPSILON = 0.001f;
constexpr uint8_t SHOT_HISTORY_FINISH_SETTLE_SAMPLES = 2;
constexpr float SHOT_HISTORY_FINISH_SETTLE_MAX_DELTA_G = 1.0f;

uint16_t encodeUnsigned(float value, float scale, uint16_t maxValue) {
    if (!std::isfinite(value)) {
        return 0;
    }
    float scaled = value * scale;
    if (scaled < 0.0f) {
        scaled = 0.0f;
    }
    scaled += 0.5f;
    uint32_t fixed = static_cast<uint32_t>(scaled);
    if (fixed > maxValue) {
        fixed = maxValue;
    }
    return static_cast<uint16_t>(fixed);
}

int16_t encodeSigned(float value, float scale, int16_t minValue, int16_t maxValue) {
    if (!std::isfinite(value)) {
        return 0;
    }
    float scaled = value * scale;
    if (scaled >= 0.0f) {
        scaled += 0.5f;
    } else {
        scaled -= 0.5f;
    }
    int32_t fixed = static_cast<int32_t>(scaled);
    if (fixed < minValue) {
        fixed = minValue;
    }
    if (fixed > maxValue) {
        fixed = maxValue;
    }
    return static_cast<int16_t>(fixed);
}

String padId(String id, int length = 6) {
    while (id.length() < length) {
        id = "0" + id;
    }
    return id;
}

bool readOptionalCorrectionFloat(JsonObjectConst source, const char *key, std::optional<float> &out, String &reason) {
    JsonVariantConst value = source[key];
    if (value.isNull()) {
        out.reset();
        return true;
    }
    if (value.is<bool>() ||
        !(value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>())) {
        reason = String("Correction field '") + key + "' must be numeric";
        return false;
    }
    const float parsed = value.as<float>();
    if (!std::isfinite(parsed)) {
        reason = String("Correction field '") + key + "' must be finite";
        return false;
    }
    out = parsed;
    return true;
}

} // namespace

ShotHistoryPlugin ShotHistory;

void ShotHistoryPlugin::setup(Controller *c, PluginManager *pm) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    controller = c;
    pluginManager = pm;
    controller->setCompletedShotProjection(this);
    if (controller->isSDCard()) {
        fs = &SD_MMC;
        ESP_LOGI("ShotHistoryPlugin", "Logging shot history to SD card");
    }
    pm->on("controller:brew:start", [this](Event const &event) { startRecording(event); });
    pm->on("controller:brew:end", [this](Event const &) { endRecording(); });
    pm->on("controller:brew:clear", [this](Event const &) { endExtendedRecording(); });
    pm->on("rl:shot:captured", [this](Event const &event) { rememberRLShotHistoryMapping(event); });
    pm->on("controller:volumetric-measurement:estimation:change",
           [this](Event const &event) { currentEstimatedWeight = event.getFloat("value"); });
    pm->on("controller:volumetric-measurement:bluetooth:change",
           [this](Event const &event) { currentBluetoothWeight = event.getFloat("value"); });
#ifndef GAGGIMATE_DISABLE_HARDWARE_SCALE
    pm->on("controller:volumetric-measurement:hardware:change",
           [this](Event const &event) { currentHardwareWeight = event.getFloat("value"); });
#endif
    pm->on("boiler:currentTemperature:change", [this](Event const &event) { currentTemperature = event.getFloat("value"); });
    pm->on("pump:puck-resistance:change", [this](Event const &event) { currentPuckResistance = event.getFloat("value"); });
    // Initialize rebuild state
    rebuildInProgress = false;
    bufferedSamples.reserve(240);
    // Leftover from the abandoned separate recent-shots index; aggregates now live in index.bin.
    const bool recentIndexExists =
        fs == &LittleFS ? LittleFSUtil::existsQuietly("/h/recent.bin") : fs->exists("/h/recent.bin");
    if (recentIndexExists) {
        fs->remove("/h/recent.bin");
    }
    xTaskCreatePinnedToCore(loopTask, "ShotHistoryPlugin::loop", configMINIMAL_STACK_SIZE * 6, this, 1, &taskHandle, 0);
}

void ShotHistoryPlugin::loop() {}

bool ShotHistoryPlugin::ensureProjection(AutoTuning::CompletedShotArtifact const &artifact) {
    AutoTuning::ShotHistoryMetadata const &history = artifact.record.history;
    if (!history.reserved || history.id == 0 || artifact.record.shotId.empty() ||
        artifact.record.samples.empty() || history.phaseTransitionCount > history.phaseTransitions.size()) {
        return false;
    }
    const std::uint32_t durationMs =
        artifact.record.samples[artifact.record.samples.size() - 1].elapsedMs > 0
            ? artifact.record.samples[artifact.record.samples.size() - 1].elapsedMs
            : static_cast<std::uint32_t>(std::max(0.0f, artifact.record.shotTimeS) * 1000.0f + 0.5f);
    if (durationMs <= 7500) {
        return true;
    }

    const String historyId = padId(String(history.id));
    const String finalPath = "/h/" + historyId + ".slog";
    const String temporaryPath = finalPath + ".tmp";
    const size_t expectedSize =
        sizeof(ShotLogHeader) + artifact.record.samples.size() * sizeof(ShotLogSample);
    bool existingValid = false;
    {
        auto lease = StorageCoordinator::instance().acquireFlash();
        if (!fs->exists("/h") && !fs->mkdir("/h")) {
            return false;
        }
        File existing = fs->open(finalPath, FILE_READ);
        if (existing && existing.size() == expectedSize) {
            ShotLogHeader existingHeader{};
            existingValid =
                existing.read(reinterpret_cast<std::uint8_t *>(&existingHeader), sizeof(existingHeader)) ==
                    sizeof(existingHeader) &&
                existingHeader.magic == SHOT_LOG_MAGIC &&
                existingHeader.version == SHOT_LOG_VERSION &&
                existingHeader.sampleCount == artifact.record.samples.size();
        }
        if (existing) {
            existing.close();
        }
        if (fs->exists(temporaryPath)) {
            fs->remove(temporaryPath);
        }
    }

    ShotLogHeader projectedHeader{};
    projectedHeader.magic = SHOT_LOG_MAGIC;
    projectedHeader.version = SHOT_LOG_VERSION;
    projectedHeader.reserved0 = static_cast<std::uint8_t>(SHOT_LOG_SAMPLE_SIZE);
    projectedHeader.headerSize = SHOT_LOG_HEADER_SIZE;
    projectedHeader.sampleInterval = SHOT_LOG_SAMPLE_INTERVAL_MS;
    projectedHeader.fieldsMask = SHOT_LOG_FIELDS_MASK_ALL;
    projectedHeader.sampleCount = artifact.record.samples.size();
    projectedHeader.durationMs = durationMs;
    projectedHeader.startEpoch =
        artifact.record.timestamp > UINT32_MAX ? UINT32_MAX
                                               : static_cast<std::uint32_t>(std::max<AutoTuning::Timestamp>(
                                                     artifact.record.timestamp, 0));
    strncpy(projectedHeader.profileId, artifact.record.profile.id.c_str(),
            sizeof(projectedHeader.profileId) - 1);
    strncpy(projectedHeader.profileName, artifact.record.profile.label.c_str(),
            sizeof(projectedHeader.profileName) - 1);
    projectedHeader.finalWeight =
        encodeUnsigned(history.finalMeasuredWeightG, WEIGHT_SCALE, WEIGHT_MAX_VALUE);
    projectedHeader.finalExitReason = history.finalExitReason;
    projectedHeader.brewDelayMs = history.brewDelayMs;
    projectedHeader.phaseTransitionCount = static_cast<std::uint8_t>(history.phaseTransitionCount);
    for (size_t index = 0; index < history.phaseTransitionCount; ++index) {
        AutoTuning::ShotPhaseTransition const &source = history.phaseTransitions[index];
        PhaseTransition &target = projectedHeader.phaseTransitions[index];
        target.sampleIndex = source.sampleIndex;
        target.phaseNumber = source.phaseNumber;
        target.transitionReason = source.exitReason;
        strncpy(target.phaseName, source.phaseName.c_str(), sizeof(target.phaseName) - 1);
    }

    std::uint64_t temperatureTotal = 0;
    std::uint64_t positiveFlowTotal = 0;
    std::uint32_t positiveFlowCount = 0;
    std::uint16_t maximumPressure = 0;
    if (!existingValid) {
        {
            auto lease = StorageCoordinator::instance().acquireFlash();
            File file = fs->open(temporaryPath, FILE_WRITE);
            if (!file ||
                file.write(reinterpret_cast<const std::uint8_t *>(&projectedHeader),
                           sizeof(projectedHeader)) != sizeof(projectedHeader)) {
                if (file) {
                    file.close();
                }
                return false;
            }
            file.close();
        }

        constexpr size_t SAMPLES_PER_CHUNK =
            StorageCoordinator::MAX_FLASH_QUANTUM_BYTES / sizeof(ShotLogSample);
        // This 4 KB flash quantum must not live on a task's 8 KB stack.
        // Allocate only when rebuilding; release it after this projection.
        auto encodedStorage = makePsramUnique<std::array<ShotLogSample, SAMPLES_PER_CHUNK>>();
        auto &encoded = *encodedStorage;
        for (size_t offset = 0; offset < artifact.record.samples.size();
             offset += SAMPLES_PER_CHUNK) {
            const size_t count =
                std::min(SAMPLES_PER_CHUNK, artifact.record.samples.size() - offset);
            for (size_t index = 0; index < count; ++index) {
                AutoTuning::ShotSample const &source = artifact.record.samples[offset + index];
                ShotLogSample &target = encoded[index];
                target = ShotLogSample{};
                target.t = static_cast<std::uint16_t>(
                    std::min<size_t>(offset + index, UINT16_MAX));
                target.tt = encodeUnsigned(source.targetTemperature, TEMP_SCALE, TEMP_MAX_VALUE);
                target.ct = encodeUnsigned(source.temperature, TEMP_SCALE, TEMP_MAX_VALUE);
                target.tp = encodeUnsigned(source.targetPressure, PRESSURE_SCALE, PRESSURE_MAX_VALUE);
                target.cp = encodeUnsigned(source.pressure, PRESSURE_SCALE, PRESSURE_MAX_VALUE);
                target.fl = encodeSigned(source.pumpFlow, FLOW_SCALE, FLOW_MIN_VALUE, FLOW_MAX_VALUE);
                target.tf = encodeSigned(source.targetFlow, FLOW_SCALE, FLOW_MIN_VALUE, FLOW_MAX_VALUE);
                target.pf = encodeSigned(source.puckFlow, FLOW_SCALE, FLOW_MIN_VALUE, FLOW_MAX_VALUE);
                target.vf = encodeSigned(source.measuredFlow, FLOW_SCALE, FLOW_MIN_VALUE, FLOW_MAX_VALUE);
                target.v = encodeUnsigned(source.measuredWeight, WEIGHT_SCALE, WEIGHT_MAX_VALUE);
                target.ev = encodeUnsigned(source.estimatedWeight, WEIGHT_SCALE, WEIGHT_MAX_VALUE);
                target.pr = encodeUnsigned(source.puckResistance, RESISTANCE_SCALE, RESISTANCE_MAX_VALUE);
                target.si = source.systemInfo;
                temperatureTotal += target.ct;
                maximumPressure = std::max(maximumPressure, target.cp);
                if (target.fl > 0) {
                    positiveFlowTotal += static_cast<std::uint16_t>(target.fl);
                    ++positiveFlowCount;
                }
            }
            auto lease = StorageCoordinator::instance().acquireFlash();
            File file = fs->open(temporaryPath, FILE_APPEND);
            const size_t bytes = count * sizeof(ShotLogSample);
            if (!file ||
                file.write(reinterpret_cast<const std::uint8_t *>(encoded.data()), bytes) != bytes) {
                if (file) {
                    file.close();
                }
                fs->remove(temporaryPath);
                return false;
            }
            file.close();
        }

        auto lease = StorageCoordinator::instance().acquireFlash();
        File verification = fs->open(temporaryPath, FILE_READ);
        const bool valid = verification && verification.size() == expectedSize;
        if (verification) {
            verification.close();
        }
        if (!valid || (fs->exists(finalPath) && !fs->remove(finalPath)) ||
            !fs->rename(temporaryPath.c_str(), finalPath.c_str())) {
            fs->remove(temporaryPath);
            return false;
        }
    } else {
        for (AutoTuning::ShotSample const &source : artifact.record.samples) {
            const std::uint16_t temperature =
                encodeUnsigned(source.temperature, TEMP_SCALE, TEMP_MAX_VALUE);
            const std::uint16_t pressure =
                encodeUnsigned(source.pressure, PRESSURE_SCALE, PRESSURE_MAX_VALUE);
            const std::int16_t flow =
                encodeSigned(source.pumpFlow, FLOW_SCALE, FLOW_MIN_VALUE, FLOW_MAX_VALUE);
            temperatureTotal += temperature;
            maximumPressure = std::max(maximumPressure, pressure);
            if (flow > 0) {
                positiveFlowTotal += static_cast<std::uint16_t>(flow);
                ++positiveFlowCount;
            }
        }
    }

    ShotIndexEntry indexEntry{};
    indexEntry.id = history.id;
    indexEntry.timestamp = projectedHeader.startEpoch;
    indexEntry.duration = durationMs;
    indexEntry.volume = projectedHeader.finalWeight;
    indexEntry.flags = SHOT_FLAG_COMPLETED;
    strncpy(indexEntry.profileId, projectedHeader.profileId, sizeof(indexEntry.profileId) - 1);
    strncpy(indexEntry.profileName, projectedHeader.profileName, sizeof(indexEntry.profileName) - 1);
    indexEntry.avgTemp =
        artifact.record.samples.empty()
            ? 0
            : static_cast<std::uint16_t>(temperatureTotal / artifact.record.samples.size());
    indexEntry.maxPressure = maximumPressure;
    indexEntry.avgFlow =
        positiveFlowCount == 0
            ? 0
            : static_cast<std::uint16_t>(positiveFlowTotal / positiveFlowCount);
    if (!appendToIndex(indexEntry)) {
        return false;
    }

    JsonDocument notes(&psramAllocator);
    loadNotes(String(history.id), notes);
    notes["id"] = String(history.id);
    notes["rlShotId"] = artifact.record.shotId.c_str();
    if (artifact.record.recommendation.present()) {
        notes["rlRecommendationId"] = artifact.record.recommendation.recommendationId.c_str();
    }
    saveNotes(String(history.id), notes);
    return true;
}

bool ShotHistoryPlugin::removeProjection(const std::uint32_t historyId) {
    if (historyId == 0) {
        return false;
    }
    const String id = String(historyId);
    const String padded = padId(id);
    auto lease = StorageCoordinator::instance().acquireFlash();
    bool removed = false;
    const String shotPath = "/h/" + padded + ".slog";
    const String notesPath = "/h/" + padded + ".json";
    if (fs->exists(shotPath)) {
        removed = fs->remove(shotPath) || removed;
    }
    if (fs->exists(notesPath)) {
        removed = fs->remove(notesPath) || removed;
    }
    markIndexDeleted(historyId);
    return removed;
}

float ShotHistoryPlugin::sourceWeight(VolumetricMeasurementSource source) const {
    switch (source) {
    case VolumetricMeasurementSource::HARDWARE_SCALE:
        return currentHardwareWeight;
    case VolumetricMeasurementSource::FLOW_ESTIMATION:
        return currentEstimatedWeight;
    case VolumetricMeasurementSource::BLUETOOTH:
    default:
        return currentBluetoothWeight;
    }
}

void ShotHistoryPlugin::record() {
    // Track the source latched at brew start for control, history, and final-yield metadata.
    const float scaleWeight = sourceWeight(shotSource);
    const bool measuredScaleSource = shotSource == VolumetricMeasurementSource::BLUETOOTH ||
                                     shotSource == VolumetricMeasurementSource::HARDWARE_SCALE;

    bool shouldRecord = recording || extendedRecording;
    if (shouldRecord && recording && shotSource == VolumetricMeasurementSource::HARDWARE_SCALE && controller) {
        const bool targetActive = fabsf(controller->getTargetPressure()) > SHOT_HISTORY_TARGET_ACTIVE_EPSILON ||
                                  fabsf(controller->getTargetFlow()) > SHOT_HISTORY_TARGET_ACTIVE_EPSILON;
        if (targetActive) {
            sawShotHistoryControlTarget = true;
            hardwareScaleFinishSettleSamples = 0;
        } else if (sawShotHistoryControlTarget) {
            bool brewActive = false;
            {
                std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
                Process *process = controller->getProcess();
                brewActive = process != nullptr && process->getType() == MODE_BREW && process->isActive();
            }
            if (!brewActive) {
                bool includeSettleSample = false;
                if (sampleCount > 0 && hardwareScaleFinishSettleSamples < SHOT_HISTORY_FINISH_SETTLE_SAMPLES &&
                    std::isfinite(scaleWeight) && std::isfinite(lastScaleWeight)) {
                    includeSettleSample = fabsf(scaleWeight - lastScaleWeight) <= SHOT_HISTORY_FINISH_SETTLE_MAX_DELTA_G;
                }
                if (includeSettleSample) {
                    hardwareScaleFinishSettleSamples++;
                } else {
                    recording = false;
                    extendedRecording = false;
                    shouldRecord = false;
                }
            }
        }
    }

    if (shouldRecord && (controller->getMode() == MODE_BREW || extendedRecording)) {
        // v/vf are measured scale channels. Predictive output remains separate in ev.
        const float loggedScaleWeight = measuredScaleSource && scaleWeight > 0.0f ? scaleWeight : 0.0f;
        const float scaleDiff = loggedScaleWeight - lastScaleWeight;
        if (fabsf(scaleDiff) <= MAX_PLAUSIBLE_WEIGHT_DELTA) {
            const float scaleFlow = scaleDiff / (SHOT_LOG_SAMPLE_INTERVAL_MS / 1000.0f);
            currentScaleFlow = currentScaleFlow * 0.75f + scaleFlow * 0.25f;
        }
        lastScaleWeight = loggedScaleWeight;

        ShotLogSample sample{};
        uint32_t tick = sampleCount <= 0xFFFF ? sampleCount : 0xFFFF;
        sample.t = static_cast<uint16_t>(tick);
        sample.tt = encodeUnsigned(controller->getTargetTemp(), TEMP_SCALE, TEMP_MAX_VALUE);
        sample.ct = encodeUnsigned(currentTemperature, TEMP_SCALE, TEMP_MAX_VALUE);
        sample.tp = encodeUnsigned(controller->getTargetPressure(), PRESSURE_SCALE, PRESSURE_MAX_VALUE);
        sample.cp = encodeUnsigned(controller->getCurrentPressure(), PRESSURE_SCALE, PRESSURE_MAX_VALUE);
        sample.fl = encodeSigned(controller->getCurrentPumpFlow(), FLOW_SCALE, FLOW_MIN_VALUE, FLOW_MAX_VALUE);
        sample.tf = encodeSigned(controller->getTargetFlow(), FLOW_SCALE, FLOW_MIN_VALUE, FLOW_MAX_VALUE);
        sample.pf = encodeSigned(controller->getCurrentPuckFlow(), FLOW_SCALE, FLOW_MIN_VALUE, FLOW_MAX_VALUE);
        sample.vf = encodeSigned(currentScaleFlow, FLOW_SCALE, FLOW_MIN_VALUE, FLOW_MAX_VALUE);
        sample.v = encodeUnsigned(loggedScaleWeight, WEIGHT_SCALE, WEIGHT_MAX_VALUE);
        sample.ev = encodeUnsigned(currentEstimatedWeight, WEIGHT_SCALE, WEIGHT_MAX_VALUE);
        sample.pr = encodeUnsigned(currentPuckResistance, RESISTANCE_SCALE, RESISTANCE_MAX_VALUE);
        sample.si = getSystemInfo(); // Pack system state information

        // Track phase transitions
        if (controller->getMode() == MODE_BREW) {
            // Deref under the process lock — other tasks delete the process at any time (GM-147).
            std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
            Process *process = controller->getProcess();
            if (process != nullptr && process->getType() == MODE_BREW) {
                auto *brewProcess = static_cast<BrewProcess *>(process);
                uint8_t currentPhase = static_cast<uint8_t>(brewProcess->phaseIndex);

                // Check for phase transition
                if (currentPhase != lastRecordedPhase) {
                    recordPhaseTransition(currentPhase, sampleCount, static_cast<uint8_t>(brewProcess->lastExitReason));
                    lastRecordedPhase = currentPhase;
                }
            }
        }

        bufferedSamples.push_back(sample);
        sampleCount = static_cast<uint32_t>(bufferedSamples.size());

        // Track running aggregates for the rolling recent-shots buffer.
        tempSumScaled += sample.ct;
        tempSampleCount++;
        if (sample.cp > maxPressureScaled) {
            maxPressureScaled = sample.cp;
        }
        if (sample.fl > 0) {
            flowSumScaled += sample.fl;
            positiveFlowCount++;
        }
        lastLoggedElapsedMs = millis() - shotStart;

        // Check for weight stabilization during extended recording
        if (extendedRecording) {
            const unsigned long now = millis();

            bool canProcessWeight = (controller != nullptr);
            if (canProcessWeight) {
                canProcessWeight = controller->isVolumetricAvailable();
            }

            if (!canProcessWeight) {
                // If BLE connection is unstable, end extended recording early
                extendedRecording = false;
                return;
            }

            const float weightDiff = abs(scaleWeight - lastStableWeight);

            if (weightDiff < WEIGHT_STABILIZATION_THRESHOLD) {
                if (lastWeightChangeTime == 0) {
                    lastWeightChangeTime = now;
                }
                // Weight has been stable for the threshold time, stop extended recording
                if (now - lastWeightChangeTime >= WEIGHT_STABILIZATION_TIME) {
                    extendedRecording = false;
                }
            } else {
                // Weight changed, reset stabilization timer
                lastWeightChangeTime = 0;
                lastStableWeight = scaleWeight;
            }

            // Also stop extended recording after maximum duration
            if (now - extendedRecordingStart >= EXTENDED_RECORDING_DURATION) {
                extendedRecording = false;
            }
        }
    }
    if (!recording && !extendedRecording && persistencePending.exchange(false, std::memory_order_acq_rel)) {
        // Patch header with sampleCount and duration
        header.sampleCount = sampleCount;
        header.durationMs = lastLoggedElapsedMs > 0 ? lastLoggedElapsedMs : millis() - shotStart;
        header.finalExitReason = finalExitReason; // why the shot ended (last phase exit or manual abort)
        float finalWeight = sampleCount > 0 ? lastScaleWeight : scaleWeight;
        header.finalWeight = finalWeight > 0.0f ? encodeUnsigned(finalWeight, WEIGHT_SCALE, WEIGHT_MAX_VALUE) : 0;
        const String shotPath = "/h/" + currentId + ".slog";
        const size_t expectedSize = sizeof(header) + static_cast<size_t>(sampleCount) * sizeof(ShotLogSample);
        const bool shotLogValid = !shotLogWriteFailed && persistBufferedShot();
        unsigned long duration = header.durationMs;
        if (duration <= 7500) { // Exclude failed shots and flushes
            auto lease = StorageCoordinator::instance().acquireFlash();
            if (fs->exists(shotPath)) {
                fs->remove(shotPath);
            }
        } else {
            if (!shotLogValid) {
                ESP_LOGE("ShotHistoryPlugin", "Shot %s log is incomplete; expected %u bytes", currentId.c_str(),
                         static_cast<unsigned>(expectedSize));
                fs->remove(shotPath);
            }
            cleanupHistory();

            // Always create a complete index entry via upsert.
            // If an early entry exists, it gets overwritten with final data.
            // If no early entry exists, a new one is appended.
            ShotIndexEntry indexEntry{};
            indexEntry.id = currentId.toInt();
            indexEntry.timestamp = header.startEpoch;
            indexEntry.duration = header.durationMs;
            indexEntry.volume = header.finalWeight;
            indexEntry.rating = ratingFromNotes(String(currentId.toInt(), 10));
            indexEntry.flags = shotLogValid ? SHOT_FLAG_COMPLETED : 0;
            if (indexEntry.rating > 0) {
                indexEntry.flags |= SHOT_FLAG_HAS_NOTES;
            }
            strncpy(indexEntry.profileId, header.profileId, sizeof(indexEntry.profileId) - 1);
            indexEntry.profileId[sizeof(indexEntry.profileId) - 1] = '\0';
            strncpy(indexEntry.profileName, header.profileName, sizeof(indexEntry.profileName) - 1);
            indexEntry.profileName[sizeof(indexEntry.profileName) - 1] = '\0';
            indexEntry.avgTemp = tempSampleCount ? static_cast<uint16_t>(tempSumScaled / tempSampleCount) : 0;
            indexEntry.maxPressure = maxPressureScaled;
            indexEntry.avgFlow = positiveFlowCount ? static_cast<uint16_t>(flowSumScaled / positiveFlowCount) : 0;

            if (!appendToIndex(indexEntry)) {
                ESP_LOGE("ShotHistoryPlugin", "CRITICAL: Failed to add completed shot %u to index", indexEntry.id);
            }

            // Notify clients the shot is actually persisted. The brew process's
            // isActive/isFinished state (used elsewhere for UI) can go inactive
            // well before extended recording (BLE scale weight settling, see
            // endRecording()) finishes writing this entry, so the dashboard
            // listens for this event instead of inferring timing from that state.
            if (pluginManager) {
                Event savedEvent;
                savedEvent.id = "evt:history-shot-saved";
                savedEvent.setInt("id", indexEntry.id);
                pluginManager->trigger(savedEvent);
            }
        }
        bufferedSamples.clear();
    }
}

void ShotHistoryPlugin::startRecording(Event const &event) {
    {
        // Deref under the process lock — other tasks delete the process at any time (GM-147).
        std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
        Process *process = controller->getProcess();
        if (process != nullptr && process->getType() == MODE_BREW) {
            BrewProcess *brewProcess = static_cast<BrewProcess *>(process);
            if (brewProcess->isUtility()) {
                return;
            }
            // Capture initial volumetric mode state (brew by weight vs brew by time)
            shotStartedVolumetric = brewProcess->target == ProcessTarget::VOLUMETRIC;
            // Capture the brew delay the shot runs with (fixed at process construction)
            currentBrewDelay = brewProcess->brewDelay;
        }
    }
    currentId = padId(String(std::max(0, event.getInt("history_id"))));
    memset(&header, 0, sizeof(header));
    header.magic = SHOT_LOG_MAGIC;
    header.version = SHOT_LOG_VERSION;
    header.reserved0 = static_cast<uint8_t>(SHOT_LOG_SAMPLE_SIZE);
    header.headerSize = SHOT_LOG_HEADER_SIZE;
    header.sampleInterval = SHOT_LOG_SAMPLE_INTERVAL_MS;
    header.fieldsMask = SHOT_LOG_FIELDS_MASK_ALL;
    header.startEpoch = getTime();
    Profile profile = controller->getProfileManager()->getSelectedProfile();
    strncpy(header.profileId, profile.id.c_str(), sizeof(header.profileId) - 1);
    header.profileId[sizeof(header.profileId) - 1] = '\0';
    strncpy(header.profileName, profile.label.c_str(), sizeof(header.profileName) - 1);
    header.profileName[sizeof(header.profileName) - 1] = '\0';
    const double delayMs = currentBrewDelay > 0.0 ? currentBrewDelay + 0.5 : 0.0;
    header.brewDelayMs = delayMs > 65535.0 ? 65535 : static_cast<uint16_t>(delayMs);
    shotStart = millis();
    lastWeightChangeTime = 0;
    extendedRecordingStart = 0;
    currentBluetoothWeight = 0.0f;
    currentHardwareWeight = 0.0f;
    lastStableWeight = 0.0f;
    lastScaleWeight = 0.0f;
    currentEstimatedWeight = 0.0f;
    currentScaleFlow = 0.0f;
    sawShotHistoryControlTarget = false;
    hardwareScaleFinishSettleSamples = 0;
    lastLoggedElapsedMs = 0;
    currentProfileName = controller->getProfileManager()->getSelectedProfile().label;
    // Latch the source the brew controller resolved for this shot (set in
    // Controller::activate() before controller:brew:start fires).
    shotSource = controller ? controller->getCurrentVolumetricSource() : VolumetricMeasurementSource::INACTIVE;
    recording = true;
    extendedRecording = false;
    sampleCount = 0;
    bufferedSamples.clear();
    persistencePending.store(true, std::memory_order_release);
    shotLogWriteFailed = false;
    tempSumScaled = 0;
    tempSampleCount = 0;
    maxPressureScaled = 0;
    flowSumScaled = 0;
    positiveFlowCount = 0;

    // Reset phase tracking for new shot
    lastRecordedPhase = 0xFF;                                      // Invalid value to detect first phase
    finalExitReason = static_cast<uint8_t>(PhaseExitReason::NONE); // Reset shot-end reason
}

unsigned long ShotHistoryPlugin::getTime() {
    time_t now;
    time(&now);
    return now;
}

void ShotHistoryPlugin::endRecording() {
    // Capture how the shot ended: if the process ran to completion, reuse the last phase's exit reason;
    // if it was still running when stopped, the user aborted it. getLastProcess() is the just-ended brew
    // process here (deactivate() moves currentProcess -> lastProcess before firing controller:brew:end).
    if (controller != nullptr) {
        // Deref under the process lock — other tasks delete the process at any time (GM-147).
        std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
        Process *last = controller->getLastProcess();
        if (last != nullptr && last->getType() == MODE_BREW) {
            auto *brewProcess = static_cast<BrewProcess *>(last);
            PhaseExitReason reason =
                brewProcess->processPhase == ProcessPhase::FINISHED ? brewProcess->lastExitReason : PhaseExitReason::ABORTED;
            finalExitReason = static_cast<uint8_t>(reason);
        }
    }

    if (recording && shotSource != VolumetricMeasurementSource::HARDWARE_SCALE && controller && controller->isVolumetricAvailable()) {
        const float scaleWeight = sourceWeight(shotSource);
        if (scaleWeight > 0) {
            // Start extended recording for any shot with active weight data
            extendedRecording = true;
            extendedRecordingStart = millis();
            lastStableWeight = scaleWeight;
            lastWeightChangeTime = 0;
        }
    }

    // Notify clients immediately, without waiting for the history file write
    // (which can lag behind by the extended-recording window above). Pressure
    // and flow are already final at this point: the pump is off, so any
    // further samples recorded during extended recording have cp/fl at or
    // near zero and cannot change the running max/average.
    if (pluginManager) {
        Event statsEvent;
        statsEvent.id = "evt:shot-finished-stats";
        statsEvent.setFloat("maxPressure", maxPressureScaled > 0 ? maxPressureScaled / PRESSURE_SCALE : 0.0f);
        statsEvent.setFloat("avgFlow",
                            positiveFlowCount > 0 ? (flowSumScaled / static_cast<float>(positiveFlowCount)) / FLOW_SCALE : 0.0f);
        pluginManager->trigger(statsEvent);
    }

    recording = false;
}

void ShotHistoryPlugin::endExtendedRecording() {
    if (extendedRecording) {
        extendedRecording = false;
    }
}

void ShotHistoryPlugin::recordPhaseTransition(uint8_t phaseNumber, uint16_t sampleIndex, uint8_t reason) {
    // Only record if we have space and a valid header
    if (header.phaseTransitionCount >= 12 || !persistencePending.load(std::memory_order_acquire)) {
        return;
    }

    // Get current profile to extract phase name
    Profile profile = controller->getProfileManager()->getSelectedProfile();
    PhaseTransition &transition = header.phaseTransitions[header.phaseTransitionCount];

    transition.sampleIndex = sampleIndex;
    transition.phaseNumber = phaseNumber;
    transition.transitionReason = reason; // PhaseExitReason for why the previous phase ended

    // Get phase name from profile
    if (phaseNumber < profile.phases.size()) {
        strncpy(transition.phaseName, profile.phases[phaseNumber].name.c_str(), sizeof(transition.phaseName) - 1);
        transition.phaseName[sizeof(transition.phaseName) - 1] = '\0';
    } else {
        // Fallback to generic name
        snprintf(transition.phaseName, sizeof(transition.phaseName), "Phase %d", phaseNumber + 1);
    }

    header.phaseTransitionCount++;

    ESP_LOGD("ShotHistoryPlugin", "Recorded phase transition to phase %d (%s) at sample %d", phaseNumber, transition.phaseName,
             sampleIndex);
}

void ShotHistoryPlugin::rememberRLShotHistoryMapping(Event const &event) {
    const String shotId = event.getString("shot_id");
    if (shotId.isEmpty() || currentId.isEmpty())
        return;

    PendingRLShotHistoryMapping mapping;
    mapping.historyId = String(std::max(0, event.getInt("history_id")), 10);
    mapping.shotId = shotId;
    mapping.recommendationId = event.getString("recommendation_id");
    std::lock_guard<std::mutex> guard(pendingRLMappingMutex);
    pendingRLMappings.push_back(std::move(mapping));
}

void ShotHistoryPlugin::persistNextRLShotHistoryMapping() {
    // The recorder task owns the open .slog file. Wait until it has flushed and
    // closed so notes never enter LittleFS concurrently with shot finalization.
    if (persistencePending.load(std::memory_order_acquire))
        return;

    PendingRLShotHistoryMapping mapping;
    {
        std::lock_guard<std::mutex> guard(pendingRLMappingMutex);
        if (pendingRLMappings.empty())
            return;
        mapping = std::move(pendingRLMappings.front());
        pendingRLMappings.pop_front();
    }

    JsonDocument notes(&psramAllocator);
    loadNotes(mapping.historyId, notes);
    notes["id"] = mapping.historyId;
    notes["rlShotId"] = mapping.shotId;
    if (!mapping.recommendationId.isEmpty()) {
        notes["rlRecommendationId"] = mapping.recommendationId;
    }
    saveNotes(mapping.historyId, notes);
}

void ShotHistoryPlugin::attachAutoTuningSummary(JsonDocument &response, JsonObjectConst notes) const {
    const String shotId = notes["rlShotId"].as<String>();
    if (shotId.isEmpty()) {
        return;
    }
    JsonDocument summary(&psramAllocator);
    if (LocalAutoTuningStore.loadShotSummary(shotId, summary) && summary.is<JsonObject>()) {
        summary["reprocess_available"] = LocalAutoTuningStore.hasShotReplay(shotId);
        response["auto_tuning"].set(summary.as<JsonObjectConst>());
    }
}

uint8_t ShotHistoryPlugin::ratingFromNotes(const String &id) {
    JsonDocument notes(&psramAllocator);
    loadNotes(id, notes);
    uint8_t rating = notes["rating"].as<uint8_t>();
    return rating <= 5 ? rating : 0;
}

uint16_t ShotHistoryPlugin::getSystemInfo() {
    uint16_t systemInfo = 0;

    // Bit 0: Shot started in volumetric mode
    if (shotStartedVolumetric) {
        systemInfo |= SYSTEM_INFO_SHOT_STARTED_VOLUMETRIC;
    }

    // Bit 1: Currently in volumetric mode (check current process if active)
    if (controller != nullptr) {
        // Deref under the process lock — other tasks delete the process at any time (GM-147).
        std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
        Process *process = controller->getProcess();
        if (process != nullptr && process->getType() == MODE_BREW) {
            auto *brewProcess = static_cast<BrewProcess *>(process);
            bool currentlyVolumetric = brewProcess->target == ProcessTarget::VOLUMETRIC &&
                                       brewProcess->currentPhase.hasVolumetricTarget() && controller->isVolumetricAvailable();
            if (currentlyVolumetric) {
                systemInfo |= SYSTEM_INFO_CURRENTLY_VOLUMETRIC;
            }
        }
    }

    // Bit 2: Bluetooth scale connected
    if (controller != nullptr && controller->isBluetoothScaleHealthy()) {
        systemInfo |= SYSTEM_INFO_BLUETOOTH_SCALE_CONNECTED;
    }

    // Bit 3: Volumetric available
    if (controller != nullptr && controller->isVolumetricAvailable()) {
        systemInfo |= SYSTEM_INFO_VOLUMETRIC_AVAILABLE;
    }

    // Bit 4: Extended recording active
    if (extendedRecording) {
        systemInfo |= SYSTEM_INFO_EXTENDED_RECORDING;
    }

    return systemInfo;
}

void ShotHistoryPlugin::cleanupHistory() {
    size_t freeSpace = getFreeSpace();
    if (freeSpace > MIN_FREE_SPACE_BYTES) {
        return; // Enough space, nothing to do
    }

    // Collect and sort .slog files to find the oldest
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    File directory = fs->open("/h");
    std::vector<String> slogFiles;
    String filename = directory.getNextFileName();
    size_t scanned = 0;
    while (filename != "") {
        if (filename.endsWith(".slog")) {
            slogFiles.push_back(filename);
        }
        filename = directory.getNextFileName();
        if (++scanned % 8 == 0) {
            flashLease.checkpoint();
        }
    }
    directory.close();
    flashLease.reset();

    if (slogFiles.empty()) {
        return;
    }

    sort(slogFiles.begin(), slogFiles.end(), [](const String &a, const String &b) { return a < b; });

    // Remove oldest files one at a time until we have enough free space
    size_t removed = 0;
    for (size_t i = 0; i < slogFiles.size() && getFreeSpace() <= MIN_FREE_SPACE_BYTES; i++) {
        String fname = slogFiles[i];
        int start = fname.lastIndexOf('/') + 1;
        int end = fname.lastIndexOf('.');
        String rlShotId;
        if (end > start) {
            uint32_t shotId = fname.substring(start, end).toInt();
            JsonDocument notes(&psramAllocator);
            loadNotes(String(shotId), notes);
            rlShotId = notes["rlShotId"].as<String>();
            if (!rlShotId.isEmpty() && !LocalAutoTuningStore.canRemoveShotData(rlShotId)) {
                continue;
            }
            markIndexDeleted(shotId);
        }
        if (!rlShotId.isEmpty()) {
            LocalAutoTuningStore.removeShotData(rlShotId);
        }

        // Remove .slog and associated .json notes file
        {
            auto lease = StorageCoordinator::instance().acquireFlash();
            fs->remove(fname);
            String notesPath = fname.substring(0, fname.lastIndexOf('.')) + ".json";
            fs->remove(notesPath);
        }
        removed++;
    }

    if (removed > 0) {
        ESP_LOGI("ShotHistoryPlugin", "Cleaned up %u old shots (free space: %u bytes)", removed, getFreeSpace());
    }
}

size_t ShotHistoryPlugin::getFreeSpace() {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    if (controller->isSDCard()) {
        uint64_t total = SD_MMC.totalBytes();
        uint64_t used = SD_MMC.usedBytes();
        uint64_t free = total > used ? (total - used) : 0;
        // Cap to size_t max for consistency
        return free > SIZE_MAX ? SIZE_MAX : static_cast<size_t>(free);
    }
    size_t total = LittleFS.totalBytes();
    size_t used = LittleFS.usedBytes();
    return total > used ? (total - used) : 0;
}

void ShotHistoryPlugin::handleRequest(JsonDocument &request, JsonDocument &response) {
    auto flashLease = StorageCoordinator::instance().tryAcquireFlash();
    if (!flashLease) {
        response["error"] = "Storage is busy while a machine process is active";
        return;
    }
    String type = request["tp"].as<String>();
    response["tp"] = String("res:") + type.substring(4);
    response["rid"] = request["rid"].as<String>();

    if (type == "req:history:delete") {
        auto id = request["id"].as<String>();
        String paddedId = id;
        while (paddedId.length() < 6) {
            paddedId = "0" + paddedId;
        }
        JsonDocument notes(&psramAllocator);
        loadNotes(id, notes);
        const String rlShotId = notes["rlShotId"].as<String>();
        fs->remove("/h/" + paddedId + ".slog");
        fs->remove("/h/" + paddedId + ".json");
        if (id != paddedId) {
            fs->remove("/h/" + id + ".json");
        }
        markIndexDeleted(id.toInt());
        flashLease.reset();
        if (!rlShotId.isEmpty()) {
            LocalAutoTuningStore.removeShotData(rlShotId, true);
        }

        response["msg"] = "Ok";
    } else if (type == "req:history:rl:reprocess") {
        const String id = request["id"].as<String>();
        JsonDocument notes(&psramAllocator);
        loadNotes(id, notes);
        const String shotId = notes["rlShotId"].as<String>();
        flashLease.reset();
        if (shotId.isEmpty() || !LocalAutoTuningStore.hasShotReplay(shotId)) {
            response["error"] = "Shot replay data is unavailable";
        } else {
            Event replay;
            replay.id = "rl:shot:reprocess";
            replay.setString("shot_id", shotId);
            pluginManager->trigger(replay);
            if (replay.getInt("queued") != 1) {
                response["error"] = "Shot reprocessing worker is unavailable";
            } else {
                response["msg"] = "Shot queued for reprocessing";
                response["shot_id"] = shotId;
            }
        }
    } else if (type == "req:history:notes:get") {
        auto id = request["id"].as<String>();
        JsonDocument notes(&psramAllocator);
        loadNotes(id, notes);
        response["notes"] = notes;
        flashLease.reset();
        attachAutoTuningSummary(response, notes.as<JsonObjectConst>());
    } else if (type == "req:history:notes:save") {
        auto id = request["id"].as<String>();
        if (!request["notes"].is<JsonObjectConst>()) {
            response["error"] = "Shot notes must be an object";
            return;
        }
        JsonDocument existingNotes(&psramAllocator);
        loadNotes(id, existingNotes);
        const String rlShotId = existingNotes["rlShotId"].as<String>();
        const String rlRecommendationId = existingNotes["rlRecommendationId"].as<String>();
        // Auto-tuning and community stores acquire their own mutex before the
        // flash lease. Do not hold the flash lease while entering either store.
        flashLease.reset();

        JsonDocument notes(&psramAllocator); // variant->const JsonDocument& is ambiguous on clang
        notes.set(request["notes"]);
        notes.remove("rlShotId");
        notes.remove("rlRecommendationId");
        notes["id"] = id;
        if (!rlShotId.isEmpty()) {
            notes["rlShotId"] = rlShotId;
        }
        if (!rlRecommendationId.isEmpty()) {
            notes["rlRecommendationId"] = rlRecommendationId;
        }

        JsonVariantConst correctionValue = request["shot_correction"];
        if (!correctionValue.isNull()) {
            if (!correctionValue.is<JsonObjectConst>()) {
                response["error"] = "Shot correction must be an object";
            } else if (rlShotId.isEmpty()) {
                response["error"] = "This history entry is not linked to an auto-tuning shot";
            } else {
                AutoTuning::ShotCorrection correction;
                correction.shotId = rlShotId.c_str();
                correction.source = "gaggimate_shot_history";
                JsonObjectConst correctionJson = correctionValue.as<JsonObjectConst>();
                String correctionError;
                const bool valid =
                    readOptionalCorrectionFloat(correctionJson, "relative_grind_steps_from_reference",
                                                correction.relativeGrindStepsFromReference, correctionError) &&
                    readOptionalCorrectionFloat(correctionJson, "current_absolute_step", correction.currentAbsoluteStep,
                                                correctionError) &&
                    readOptionalCorrectionFloat(correctionJson, "dose_in_g", correction.doseInG, correctionError) &&
                    readOptionalCorrectionFloat(correctionJson, "target_yield_g", correction.targetYieldG,
                                                correctionError) &&
                    readOptionalCorrectionFloat(correctionJson, "beverage_out_g", correction.beverageOutG,
                                                correctionError);
                if (!valid) {
                    response["error"] = correctionError;
                } else {
                    AutoTuning::AutoTuningRecordStorePort *store = controller->getAutoTuningRecordStore();
                    AutoTuning::CorrectedShotRecord corrected;
                    std::string reason;
                    if (!store || !store->correctShot(correction, corrected, reason)) {
                        response["error"] = reason.empty() ? "Unable to correct the stored shot" : reason.c_str();
                    } else {
                        if (correction.doseInG.has_value() && corrected.record.doseFollowed.has_value()) {
                            correction.doseFollowed = corrected.record.doseFollowed;
                        }
                        Event correctionEvent;
                        correctionEvent.id = "rl:shot:correction";
                        correctionEvent.setString("shot_id", rlShotId);
                        correctionEvent.setPayload(correction);
                        pluginManager->trigger(correctionEvent);
                        response["optimizer_persisted"] = correctionEvent.getInt("optimizer_persisted") == 1;

                        // The container rebuilds its cloud record from this correction.
                        response["community_replacement_queued"] = correctionEvent.getInt("optimizer_persisted") == 1;

                    }
                }
            }
        }

        if (!response["error"].isNull()) {
            attachAutoTuningSummary(response, existingNotes.as<JsonObjectConst>());
            return;
        }

        saveNotes(id, notes);
        // Update rating and volume in index
        uint8_t rating = notes["rating"].as<uint8_t>();

        // Check if user provided a doseOut value to override volume
        uint16_t volume = 0;
        if (notes["doseOut"].is<String>() && !notes["doseOut"].as<String>().isEmpty()) {
            float doseOut = notes["doseOut"].as<String>().toFloat();
            if (doseOut > 0.0f) {
                volume = encodeUnsigned(doseOut, WEIGHT_SCALE, WEIGHT_MAX_VALUE);
            }
        }

        // Always use updateIndexMetadata - it handles both rating and optional volume
        updateIndexMetadata(id.toInt(), rating, volume);

        response["msg"] = "Ok";
        attachAutoTuningSummary(response, notes.as<JsonObjectConst>());
    } else if (type == "req:history:rebuild") {
        // Rebuild is now handled asynchronously by WebUIPlugin
        // This path shouldn't be reached, but handle it just in case
        response["msg"] = "Use async rebuild";
    }
}

void ShotHistoryPlugin::saveNotes(const String &id, const JsonDocument &notes) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    File file = fs->open("/h/" + id + ".json", FILE_WRITE);
    if (file) {
        String notesStr;
        serializeJson(notes, notesStr);
        file.print(notesStr);
        file.close();
    }
}

void ShotHistoryPlugin::loadNotes(const String &id, JsonDocument &notes) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    File file = fs->open("/h/" + id + ".json", "r");
    if (file) {
        String notesStr = file.readString();
        file.close();
        deserializeJson(notes, notesStr);
    }
}

void ShotHistoryPlugin::loopTask(void *arg) {
    auto *plugin = static_cast<ShotHistoryPlugin *>(arg);
    while (true) {
        plugin->record();
        plugin->persistNextRLShotHistoryMapping();
        // Use canonical interval from shot log format to avoid divergence.
        vTaskDelay(SHOT_LOG_SAMPLE_INTERVAL_MS / portTICK_PERIOD_MS);
    }
}

bool ShotHistoryPlugin::persistBufferedShot() {
    const String finalPath = "/h/" + currentId + ".slog";
    const String temporaryPath = finalPath + ".tmp";
    {
        auto lease = StorageCoordinator::instance().acquireFlash();
        if (!fs->exists("/h") && !fs->mkdir("/h")) {
            return false;
        }
        if (fs->exists(temporaryPath)) {
            fs->remove(temporaryPath);
        }
        File file = fs->open(temporaryPath, FILE_WRITE);
        if (!file || file.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) != sizeof(header)) {
            if (file) {
                file.close();
            }
            return false;
        }
        file.close();
    }

    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(bufferedSamples.data());
    const size_t byteCount = bufferedSamples.size() * sizeof(ShotLogSample);
    for (size_t offset = 0; offset < byteCount; offset += StorageCoordinator::MAX_FLASH_QUANTUM_BYTES) {
        const size_t chunk = std::min(StorageCoordinator::MAX_FLASH_QUANTUM_BYTES, byteCount - offset);
        auto lease = StorageCoordinator::instance().acquireFlash();
        File file = fs->open(temporaryPath, FILE_APPEND);
        if (!file || file.write(bytes + offset, chunk) != chunk) {
            if (file) {
                file.close();
            }
            fs->remove(temporaryPath);
            return false;
        }
        file.close();
    }

    auto lease = StorageCoordinator::instance().acquireFlash();
    File verification = fs->open(temporaryPath, FILE_READ);
    const size_t expectedSize = sizeof(header) + byteCount;
    const bool valid = verification && verification.size() == expectedSize;
    if (verification) {
        verification.close();
    }
    if (!valid) {
        fs->remove(temporaryPath);
        return false;
    }
    if (fs->exists(finalPath) && !fs->remove(finalPath)) {
        fs->remove(temporaryPath);
        return false;
    }
    return fs->rename(temporaryPath.c_str(), finalPath.c_str());
}

// Index management methods
bool ShotHistoryPlugin::ensureIndexExists() {
    auto lease = StorageCoordinator::instance().acquireFlash();
    if (fs->exists("/h/index.bin")) {
        // Validate existing index header
        File indexFile = fs->open("/h/index.bin", "r");
        if (indexFile) {
            ShotIndexHeader hdr{};
            bool valid =
                (indexFile.read(reinterpret_cast<uint8_t *>(&hdr), sizeof(hdr)) == sizeof(hdr) && hdr.magic == SHOT_INDEX_MAGIC);
            indexFile.close();
            if (valid) {
                return true;
            }
            ESP_LOGW("ShotHistoryPlugin", "Corrupt index file detected (bad magic), recreating");
            fs->remove("/h/index.bin");
        }
    }

    // Create new empty index
    File indexFile = fs->open("/h/index.bin", FILE_WRITE);
    if (!indexFile) {
        ESP_LOGE("ShotHistoryPlugin", "Failed to create index file");
        return false;
    }

    ShotIndexHeader header{};
    header.magic = SHOT_INDEX_MAGIC;
    header.version = SHOT_INDEX_VERSION;
    header.entrySize = SHOT_INDEX_ENTRY_SIZE;
    header.entryCount = 0;
    header.nextId = controller->getSettings().getHistoryIndex();

    indexFile.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));
    indexFile.close();

    ESP_LOGI("ShotHistoryPlugin", "Created new index file");
    return true;
}

bool ShotHistoryPlugin::appendToIndex(const ShotIndexEntry &entry) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    if (!ensureIndexExists()) {
        return false;
    }

    File indexFile = fs->open("/h/index.bin", "r+");
    if (!indexFile) {
        ESP_LOGE("ShotHistoryPlugin", "Failed to open index file for append");
        return false;
    }

    ShotIndexHeader header{};
    if (!readIndexHeader(indexFile, header)) {
        indexFile.close();
        return false;
    }

    // Check for existing entry with same ID - update in place (upsert)
    int existingPos = findEntryPosition(indexFile, header, entry.id);
    if (existingPos >= 0) {
        if (writeEntryAtPosition(indexFile, existingPos, entry)) {
            ESP_LOGD("ShotHistoryPlugin", "Updated existing index entry for shot %u", entry.id);
            indexFile.close();
            return true;
        }
        ESP_LOGE("ShotHistoryPlugin", "Failed to update existing index entry for shot %u", entry.id);
        indexFile.close();
        return false;
    }

    // Append entry
    indexFile.seek(0, SeekEnd);
    size_t written = indexFile.write(reinterpret_cast<const uint8_t *>(&entry), sizeof(entry));
    if (written != sizeof(entry)) {
        ESP_LOGE("ShotHistoryPlugin", "Failed to write index entry for shot %u", entry.id);
        indexFile.close();
        return false;
    }

    // Update header
    header.entryCount++;
    header.nextId = entry.id + 1;
    indexFile.seek(0, SeekSet);
    indexFile.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header));

    indexFile.close();
    ESP_LOGD("ShotHistoryPlugin", "Appended shot %u to index", entry.id);
    return true;
}

void ShotHistoryPlugin::updateIndexMetadata(uint32_t shotId, uint8_t rating, uint16_t volume) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    File indexFile = fs->open("/h/index.bin", "r+");
    if (!indexFile) {
        ESP_LOGE("ShotHistoryPlugin", "Failed to open index file for metadata update");
        return;
    }

    ShotIndexHeader header{};
    if (!readIndexHeader(indexFile, header)) {
        indexFile.close();
        return;
    }

    int entryPos = findEntryPosition(indexFile, header, shotId);
    if (entryPos >= 0) {
        ShotIndexEntry entry{};
        if (readEntryAtPosition(indexFile, entryPos, entry)) {
            entry.rating = rating;
            if (volume > 0) {
                entry.volume = volume;
            }
            if (rating > 0) {
                entry.flags |= SHOT_FLAG_HAS_NOTES;
            }

            if (writeEntryAtPosition(indexFile, entryPos, entry)) {
                ESP_LOGD("ShotHistoryPlugin", "Updated metadata for shot %u: rating=%u, volume=%u", shotId, rating, volume);
            }
        }
    } else {
        ESP_LOGW("ShotHistoryPlugin", "Shot %u not found in index for metadata update", shotId);
    }

    indexFile.close();
}

void ShotHistoryPlugin::markIndexDeleted(uint32_t shotId) {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    File indexFile = fs->open("/h/index.bin", "r+");
    if (!indexFile) {
        ESP_LOGE("ShotHistoryPlugin", "Failed to open index file for deletion marking");
        return;
    }

    ShotIndexHeader header{};
    if (!readIndexHeader(indexFile, header)) {
        indexFile.close();
        return;
    }

    // Find ALL entries with this shot ID and mark them as deleted
    uint32_t duplicatesFound = 0;

    for (uint32_t i = 0; i < header.entryCount; i++) {
        size_t entryPos = sizeof(ShotIndexHeader) + i * sizeof(ShotIndexEntry);
        ShotIndexEntry entry{};
        if (readEntryAtPosition(indexFile, entryPos, entry)) {
            if (entry.id == shotId) {
                duplicatesFound++;

                // Mark this entry as deleted
                entry.flags |= SHOT_FLAG_DELETED;

                if (writeEntryAtPosition(indexFile, entryPos, entry)) {
                    ESP_LOGD("ShotHistoryPlugin", "Marked shot %u as deleted in index (duplicate #%u)", shotId, duplicatesFound);
                }
            }
        }
    }

    if (duplicatesFound == 0) {
        ESP_LOGW("ShotHistoryPlugin", "Shot %u not found in index for deletion marking", shotId);
    } else if (duplicatesFound > 1) {
        ESP_LOGW("ShotHistoryPlugin", "Found and marked %u duplicate entries for shot %u as deleted", duplicatesFound, shotId);
    }

    indexFile.close();
}

size_t ShotHistoryPlugin::readRecentEntries(ShotIndexEntry *outEntries, size_t maxCount) {
    auto flashLease = StorageCoordinator::instance().tryAcquireFlash();
    if (!flashLease) {
        return 0;
    }
    File indexFile = fs->open("/h/index.bin", "r");
    if (!indexFile) {
        return 0;
    }

    ShotIndexHeader header{};
    if (!readIndexHeader(indexFile, header)) {
        indexFile.close();
        return 0;
    }

    // Entries are appended in id order, so walking backwards yields newest first.
    size_t found = 0;
    for (uint32_t i = header.entryCount; i > 0 && found < maxCount; i--) {
        size_t entryPos = sizeof(ShotIndexHeader) + (i - 1) * sizeof(ShotIndexEntry);
        ShotIndexEntry entry{};
        if (!readEntryAtPosition(indexFile, entryPos, entry)) {
            break;
        }
        if (entry.flags & SHOT_FLAG_DELETED) {
            continue;
        }
        outEntries[found++] = entry;
    }

    indexFile.close();
    return found;
}

void ShotHistoryPlugin::startAsyncRebuild() {
    if (!rebuildInProgress) {
        rebuildInProgress = true; // Set immediately to prevent multiple rebuilds
        ESP_LOGI("ShotHistoryPlugin", "Starting immediate async rebuild task");

        // Create a dedicated task for rebuild instead of using the existing loop
        xTaskCreatePinnedToCore(
            [](void *param) {
                auto *plugin = static_cast<ShotHistoryPlugin *>(param);
                ESP_LOGI("ShotHistoryPlugin", "Rebuild task started");
                while (!plugin->rebuildIndex()) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
                plugin->rebuildInProgress = false;
                ESP_LOGI("ShotHistoryPlugin", "Rebuild task completed");
                vTaskDelete(NULL); // Delete this task when done
            },
            "ShotHistoryRebuild",
            configMINIMAL_STACK_SIZE * 8, // Larger stack for file operations
            this,
            2, // Higher priority than normal
            NULL, 0);
    } else {
        ESP_LOGW("ShotHistoryPlugin", "Rebuild already in progress, ignoring request");
    }
}

bool ShotHistoryPlugin::rebuildIndex() {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    const std::uint64_t processGeneration =
        StorageCoordinator::instance().processGeneration();
    const auto yieldForProcess = [&flashLease, processGeneration]() {
        return flashLease.checkpoint() &&
               StorageCoordinator::instance().processGeneration() ==
                   processGeneration;
    };
    const auto publishPaused = [this]() {
        ESP_LOGI("ShotHistoryPlugin",
                 "Pausing index rebuild for a machine process");
        if (pluginManager) {
            Event pausedEvent;
            pausedEvent.id = "evt:history-rebuild-progress";
            pausedEvent.setInt("total", 0);
            pausedEvent.setInt("current", 0);
            pausedEvent.setString("status", "paused");
            pluginManager->trigger(pausedEvent);
        }
    };
    ESP_LOGI("ShotHistoryPlugin", "Starting index rebuild...");

    // Send scanning event
    if (pluginManager) {
        Event startEvent;
        startEvent.id = "evt:history-rebuild-progress";
        startEvent.setInt("total", 0);
        startEvent.setInt("current", 0);
        startEvent.setString("status", "scanning");
        pluginManager->trigger(startEvent);
    }

    // Delete existing index
    fs->remove("/h/index.bin");

    // Create new empty index
    if (!ensureIndexExists()) {
        ESP_LOGE("ShotHistoryPlugin", "Failed to create index during rebuild");
        // Emit error event
        if (pluginManager) {
            Event errorEvent;
            errorEvent.id = "evt:history-rebuild-progress";
            errorEvent.setInt("total", 0);
            errorEvent.setInt("current", 0);
            errorEvent.setString("status", "error");
            pluginManager->trigger(errorEvent);
        }
        return true;
    }

    File directory = fs->open("/h");
    if (!directory || !directory.isDirectory()) {
        ESP_LOGW("ShotHistoryPlugin", "No history directory found");
        if (directory)
            directory.close();
        // Emit completion event even if no directory exists
        if (pluginManager) {
            Event completedEvent;
            completedEvent.id = "evt:history-rebuild-progress";
            completedEvent.setInt("total", 0);
            completedEvent.setInt("current", 0);
            completedEvent.setString("status", "completed");
            pluginManager->trigger(completedEvent);
        }
        return true;
    }

    // Collect all .slog files
    std::vector<String> slogFiles;
    File file = directory.openNextFile();
    size_t scanned = 0;
    while (file) {
        String fname = String(file.name());
        if (fname.endsWith(".slog")) {
            slogFiles.push_back(fname);
        }
        file.close();
        file = directory.openNextFile();
        if (++scanned % 8 == 0 && !yieldForProcess()) {
            if (file) {
                file.close();
            }
            directory.close();
            publishPaused();
            return false;
        }
    }
    directory.close();
    if (!yieldForProcess()) {
        publishPaused();
        return false;
    }

    // Sort files to maintain order
    std::sort(slogFiles.begin(), slogFiles.end());

    ESP_LOGI("ShotHistoryPlugin", "Rebuilding index from %d shot files", slogFiles.size());

    // Emit start event with total file count
    if (pluginManager) {
        Event startEvent;
        startEvent.id = "evt:history-rebuild-progress";
        startEvent.setInt("total", (int)slogFiles.size());
        startEvent.setInt("current", 0);
        startEvent.setString("status", "started");
        pluginManager->trigger(startEvent);
    }

    int currentIndex = 0;
    uint32_t maxId = controller->getSettings().getHistoryIndex();
    for (const String &fileName : slogFiles) {
        currentIndex++;
        File shotFile = fs->open("/h/" + fileName, "r");
        if (!shotFile) {
            if (!yieldForProcess()) {
                publishPaused();
                return false;
            }
            continue;
        }

        // Read shot header
        ShotLogHeader shotHeader{};
        if (shotFile.read(reinterpret_cast<uint8_t *>(&shotHeader), sizeof(shotHeader)) != sizeof(shotHeader) ||
            shotHeader.magic != SHOT_LOG_MAGIC) {
            shotFile.close();
            if (!yieldForProcess()) {
                publishPaused();
                return false;
            }
            continue;
        }

        // Extract shot ID from filename
        int start = fileName.lastIndexOf('/') + 1;
        int end = fileName.lastIndexOf('.');
        uint32_t shotId = fileName.substring(start, end).toInt();
        if (shotId > maxId) {
            maxId = shotId;
        }

        // Create index entry
        ShotIndexEntry entry{};
        entry.id = shotId;
        entry.timestamp = shotHeader.startEpoch;
        entry.duration = shotHeader.durationMs;
        entry.volume = shotHeader.finalWeight;
        entry.rating = 0; // Will be updated if notes exist
        entry.flags = SHOT_FLAG_COMPLETED;
        strncpy(entry.profileId, shotHeader.profileId, sizeof(entry.profileId) - 1);
        entry.profileId[sizeof(entry.profileId) - 1] = '\0';
        strncpy(entry.profileName, shotHeader.profileName, sizeof(entry.profileName) - 1);
        entry.profileName[sizeof(entry.profileName) - 1] = '\0';

        // Check for incomplete shots
        if (shotHeader.sampleCount == 0) {
            entry.flags &= ~SHOT_FLAG_COMPLETED;
        }

        // Recompute the per-shot aggregates from the sample records (same math
        // as the running sums in record()).
        {
            uint32_t tempSum = 0, tempCount = 0, flowSum = 0, flowCount = 0;
            uint16_t maxPressure = 0;
            ShotLogSample sample{};
            shotFile.seek(shotHeader.headerSize, SeekSet);
            for (uint32_t s = 0; s < shotHeader.sampleCount; s++) {
                if (shotFile.read(reinterpret_cast<uint8_t *>(&sample), sizeof(sample)) != sizeof(sample)) {
                    break;
                }
                tempSum += sample.ct;
                tempCount++;
                if (sample.cp > maxPressure) {
                    maxPressure = sample.cp;
                }
                if (sample.fl > 0) {
                    flowSum += sample.fl;
                    flowCount++;
                }
                constexpr std::size_t SAMPLES_PER_QUANTUM =
                    StorageCoordinator::MAX_FLASH_QUANTUM_BYTES /
                    sizeof(ShotLogSample);
                if ((s + 1) % SAMPLES_PER_QUANTUM == 0 &&
                    !yieldForProcess()) {
                    shotFile.close();
                    publishPaused();
                    return false;
                }
            }
            entry.avgTemp = tempCount ? static_cast<uint16_t>(tempSum / tempCount) : 0;
            entry.maxPressure = maxPressure;
            entry.avgFlow = flowCount ? static_cast<uint16_t>(flowSum / flowCount) : 0;
        }

        // Check for notes and extract rating and volume override
        String notesPath = "/h/" + String(shotId, 10) + ".json";
        if (fs->exists(notesPath)) {
            entry.flags |= SHOT_FLAG_HAS_NOTES;

            File notesFile = fs->open(notesPath, "r");
            if (notesFile) {
                String notesStr = notesFile.readString();
                notesFile.close();

                JsonDocument notesDoc(&psramAllocator);
                if (deserializeJson(notesDoc, notesStr) == DeserializationError::Ok) {
                    entry.rating = notesDoc["rating"].as<uint8_t>();

                    // Check if user provided a doseOut value to override volume
                    if (notesDoc["doseOut"].is<String>() && !notesDoc["doseOut"].as<String>().isEmpty()) {
                        float doseOut = notesDoc["doseOut"].as<String>().toFloat();
                        if (doseOut > 0.0f) {
                            entry.volume = encodeUnsigned(doseOut, WEIGHT_SCALE, WEIGHT_MAX_VALUE);
                        }
                    }
                }
            }
        }

        shotFile.close();

        // Append to index
        appendToIndex(entry);

        // Emit progress update with adaptive frequency
        // Update every file for small rebuilds, every few files for larger ones
        int updateFrequency = slogFiles.size() <= 20 ? 1 : (slogFiles.size() <= 100 ? 3 : 5);
        if (pluginManager && (currentIndex % updateFrequency == 0 || currentIndex == slogFiles.size())) {
            Event progressEvent;
            progressEvent.id = "evt:history-rebuild-progress";
            progressEvent.setInt("total", (int)slogFiles.size());
            progressEvent.setInt("current", currentIndex);
            progressEvent.setString("status", "processing");
            pluginManager->trigger(progressEvent);
            ESP_LOGI("ShotHistoryPlugin", "Rebuild progress: %d/%d", currentIndex, (int)slogFiles.size());

            flashLease.reset();
            vTaskDelay(pdMS_TO_TICKS(10));
            flashLease =
                StorageCoordinator::instance().acquireFlash();
            if (StorageCoordinator::instance().processGeneration() !=
                processGeneration) {
                publishPaused();
                return false;
            }
        } else if (!yieldForProcess()) {
            publishPaused();
            return false;
        }
    }

    if (maxId > controller->getSettings().getHistoryIndex()) {
        controller->getSettings().setHistoryIndex(maxId);
    }

    // Emit completion event
    if (pluginManager) {
        Event completionEvent;
        completionEvent.id = "evt:history-rebuild-progress";
        completionEvent.setInt("total", (int)slogFiles.size());
        completionEvent.setInt("current", (int)slogFiles.size());
        completionEvent.setString("status", "completed");
        pluginManager->trigger(completionEvent);
    }

    ESP_LOGI("ShotHistoryPlugin", "Index rebuild completed");
    return true;
}

// Index helper functions
bool ShotHistoryPlugin::readIndexHeader(File &indexFile, ShotIndexHeader &header) {
    if (indexFile.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) != sizeof(header)) {
        ESP_LOGE("ShotHistoryPlugin", "Failed to read index header");
        return false;
    }
    if (header.magic != SHOT_INDEX_MAGIC) {
        ESP_LOGE("ShotHistoryPlugin", "Invalid index magic: 0x%08X", header.magic);
        return false;
    }
    return true;
}

int ShotHistoryPlugin::findEntryPosition(File &indexFile, const ShotIndexHeader &header, uint32_t shotId) {
    for (uint32_t i = 0; i < header.entryCount; i++) {
        size_t entryPos = sizeof(ShotIndexHeader) + i * sizeof(ShotIndexEntry);
        indexFile.seek(entryPos, SeekSet);

        ShotIndexEntry entry{};
        if (!readEntryAtPosition(indexFile, entryPos, entry)) {
            ESP_LOGW("ShotHistoryPlugin", "Failed to read entry at position %u", i);
            break;
        }

        if (entry.id == shotId) {
            return entryPos;
        }
    }
    return -1;
}

bool ShotHistoryPlugin::readEntryAtPosition(File &indexFile, size_t position, ShotIndexEntry &entry) {
    indexFile.seek(position, SeekSet);
    if (indexFile.read(reinterpret_cast<uint8_t *>(&entry), sizeof(entry)) != sizeof(entry)) {
        ESP_LOGE("ShotHistoryPlugin", "Failed to read entry at position %zu", position);
        return false;
    }
    return true;
}

bool ShotHistoryPlugin::writeEntryAtPosition(File &indexFile, size_t position, const ShotIndexEntry &entry) {
    indexFile.seek(position, SeekSet);
    if (indexFile.write(reinterpret_cast<const uint8_t *>(&entry), sizeof(entry)) != sizeof(entry)) {
        ESP_LOGE("ShotHistoryPlugin", "Failed to write entry at position %zu", position);
        return false;
    }
    return true;
}
