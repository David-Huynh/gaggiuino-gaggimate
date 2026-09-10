#include <display/core/RecipeConfirmation.h>
#include "LocalAutoTuningStorePlugin.h"
#include "autotuning/AutoTuningJsonCodec.h"
#include "autotuning/AutoTuningTasteGoalJson.h"
#include "autotuning/local/LocalAutoTuningFiles.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <display/core/AutoTuning.h>
#include <display/core/Controller.h>
#include <display/core/EpochTime.h>
#include <display/core/PluginManager.h>
#include <display/core/Settings.h>
#include <display/core/StorageCoordinator.h>
#include <display/util/LittleFSUtil.h>
#include <display/util/PsramAllocator.h>
#include <esp_log.h>
#include <limits>

namespace {
constexpr const char *LOG_TAG = "LocalAutoTuningStore";
constexpr const char *STORE_DIR = "/rll";
constexpr const char *REPLAY_DIR = "/rll/p";
constexpr unsigned long STATUS_INTERVAL_MS = 10000;
constexpr unsigned long DELIVERY_SWEEP_INTERVAL_MS = 5000;
constexpr long DELIVERY_RETRY_BASE_SECONDS = 90;
constexpr long DELIVERY_RETRY_MAX_SECONDS = 15 * 60;
constexpr size_t MAX_REPLAY_SNAPSHOTS = 4;
constexpr size_t MAX_REPLAY_BYTES = 256 * 1024;

using EpochSeconds = EpochTime::Seconds;

using LocalStoreLock = std::lock_guard<std::recursive_mutex>;

static EpochSeconds nowEpoch() { return EpochTime::now(); }

static bool jsonNumber(JsonVariantConst value) {
    return value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>();
}

static String jsonStringOrEmpty(JsonVariantConst value) {
    return value.is<const char *>() ? value.as<String>() : String("");
}

static bool jsonEpoch(JsonVariantConst value, EpochSeconds &out) {
    if (value.is<bool>() || !value.is<std::int64_t>()) {
        return false;
    }
    out = value.as<std::int64_t>();
    return true;
}

static EpochSeconds jsonEpochOrZero(JsonVariantConst value) {
    EpochSeconds parsed = 0;
    return jsonEpoch(value, parsed) ? parsed : 0;
}

static AutoTuning::DeliveryState deliveryState(JsonObjectConst replay) {
    AutoTuning::DeliveryState delivery;
    String state = jsonStringOrEmpty(replay["local_delivery_state"]);
    if (!state.isEmpty()) {
        const auto parsed = AutoTuning::deliveryStatusFromKey(state.c_str());
        delivery.status = parsed.value_or(AutoTuning::DeliveryStatus::Pending);
    } else {
        const String legacy = jsonStringOrEmpty(replay["dispatch_state"]);
        if (legacy == "awaiting_dose_confirmation") {
            delivery.status = AutoTuning::DeliveryStatus::AwaitingDoseConfirmation;
        } else if (legacy == "dispatched") {
            delivery.status = AutoTuning::DeliveryStatus::RetryWait;
        } else {
            delivery.status = AutoTuning::DeliveryStatus::Pending;
        }
    }
    delivery.attemptCount = replay["local_attempt_count"] | 0;
    delivery.nextRetryAt = jsonEpochOrZero(replay["local_next_retry_at"]);
    EpochSeconds lastAttemptAt = 0;
    if (jsonEpoch(replay["local_last_attempt_at"], lastAttemptAt)) {
        delivery.lastAttemptAt = lastAttemptAt;
    }
    EpochSeconds acknowledgedAt = 0;
    if (jsonEpoch(replay["local_acknowledged_at"], acknowledgedAt)) {
        delivery.acknowledgedAt = acknowledgedAt;
    }
    delivery.lastError = jsonStringOrEmpty(replay["local_last_error"]).c_str();
    return delivery;
}

static bool deliveryTerminal(JsonObjectConst replay) {
    const AutoTuning::DeliveryState delivery = deliveryState(replay);
    return delivery.terminal() || delivery.status == AutoTuning::DeliveryStatus::NotRequired;
}

static long deliveryRetryDelaySeconds(int attemptCount) {
    const int exponent = std::clamp(attemptCount - 1, 0, 4);
    return std::min(DELIVERY_RETRY_BASE_SECONDS << exponent, DELIVERY_RETRY_MAX_SECONDS);
}

struct DeliveryStats {
    int pending = 0;
    int retrying = 0;
    int rejected = 0;
    String lastError;
    EpochSeconds lastErrorAt = std::numeric_limits<EpochSeconds>::min();
};

static DeliveryStats localDeliveryStats() {
    DeliveryStats stats;
    std::vector<String> paths;
    if (!LocalAutoTuningFiles::listRecordPaths(REPLAY_DIR, paths)) {
        return stats;
    }
    for (const String &path : paths) {
        JsonDocument doc(&psramAllocator);
        if (LocalAutoTuningFiles::readJson(path, doc)) {
            JsonObjectConst replay = doc.as<JsonObjectConst>();
            const AutoTuning::DeliveryStatus state = deliveryState(replay).status;
            const int attempts = jsonNumber(replay["local_attempt_count"]) ? replay["local_attempt_count"].as<int>()
                                                                           : (replay["dispatch_count"] | 0);
            if (state == AutoTuning::DeliveryStatus::PermanentRejection) {
                stats.rejected += 1;
            } else if (state == AutoTuning::DeliveryStatus::RetryWait ||
                       (state == AutoTuning::DeliveryStatus::AwaitingAcknowledgement && attempts > 1)) {
                stats.retrying += 1;
            } else if (state == AutoTuning::DeliveryStatus::Pending ||
                       state == AutoTuning::DeliveryStatus::AwaitingAcknowledgement) {
                stats.pending += 1;
            }
            String error = jsonStringOrEmpty(replay["local_last_error"]);
            if (state == AutoTuning::DeliveryStatus::PermanentRejection && error.isEmpty()) {
                error = "permanent_rejection";
            }
            const EpochSeconds updatedAt = jsonEpochOrZero(replay["updated_at"]);
            const bool deliveryNeedsAttention = state == AutoTuning::DeliveryStatus::PermanentRejection ||
                                                state == AutoTuning::DeliveryStatus::RetryWait;
            if (deliveryNeedsAttention && !error.isEmpty() && updatedAt >= stats.lastErrorAt) {
                stats.lastError = error;
                stats.lastErrorAt = updatedAt;
            }
        }
    }
    return stats;
}

static bool findOldestTerminalReplay(String &oldestPath, String &oldestShotId) {
    std::vector<String> paths;
    if (!LocalAutoTuningFiles::listRecordPaths(REPLAY_DIR, paths)) {
        return false;
    }
    bool found = false;
    EpochSeconds oldestTimestamp = std::numeric_limits<EpochSeconds>::max();
    for (const String &path : paths) {
        JsonDocument doc(&psramAllocator);
        if (LocalAutoTuningFiles::readJson(path, doc)) {
            JsonObjectConst replay = doc.as<JsonObjectConst>();
            const bool communityPending = (replay["community_required"] | false) && !(replay["community_dispatched"] | false);
            if (!deliveryTerminal(replay) || communityPending) {
                continue;
            }
            const EpochSeconds timestamp = LocalAutoTuningFiles::fileTimestamp(path);
            if (!found || timestamp < oldestTimestamp) {
                found = true;
                oldestPath = path;
                oldestShotId = replay["shot_id"].as<String>();
                oldestTimestamp = timestamp;
            }
        }
    }
    return found && !oldestPath.isEmpty() && !oldestShotId.isEmpty();
}

static AutoTuning::PromptState promptState(JsonObjectConst replay) {
    AutoTuning::PromptState prompt;
    const String state = jsonStringOrEmpty(replay["prompt_state"]);
    if (!state.isEmpty()) {
        prompt.status = AutoTuning::promptStatusFromKey(state.c_str())
                            .value_or(AutoTuning::PromptStatus::Processing);
    }
    prompt.revision = std::max(1, replay["prompt_revision"] | 1);
    prompt.updatedAt = jsonEpochOrZero(replay["prompt_updated_at"]);
    return prompt;
}

} // namespace

LocalAutoTuningStorePlugin LocalAutoTuningStore;

void LocalAutoTuningStorePlugin::setup(Controller *ctrl, PluginManager *pm) {
    controller = ctrl;
    pluginManager = pm;
    ctrl->setAutoTuningRecordStore(this);
    {
        LocalStoreLock lock(storeMutex);
        ensureDirectories();
    }
    recoverCommittedArtifacts();
    refreshCachedStatus();
    snapshotContexts();
#if !defined(GAGGIMATE_SIM)
    if (xTaskCreatePinnedToCore(workerTask, "AutoTune replay", 8192, this, 1, &workerTaskHandle, 0) != pdPASS) {
        workerTaskHandle = nullptr;
        ESP_LOGE(LOG_TAG, "Unable to start replay worker; background persistence is disabled");
    } else {
        xTaskNotifyGive(workerTaskHandle);
    }
    workerStartAttempted.store(true, std::memory_order_release);
#endif

    pm->on("settings:changed", [this](Event const &) { snapshotContexts(); });
    pm->on("rl:settings:changed", [this](Event const &) { snapshotContexts(); });
    pm->on("rl:status:refresh", [this](Event const &) { requestStatusRefresh(); });
    pm->on("rl:shot:dispatch", [this](Event const &event) { handleShotDispatch(event); });
    pm->on("rl:dose-confirmation", [this](Event const &event) { handleDoseConfirmation(event); });
    pm->on("rl:shot:reprocess", [this](Event &event) { handleShotReprocess(event); });
    pm->on("rl:shot:delivery:ack", [this](Event const &event) { handleShotDeliveryAck(event); });
    pm->on("rl:local:shot:delete", [this](Event const &event) {
        const String shotId = event.getString("shot_id");
        if (!removeShotData(shotId, true)) {
            return;
        }
        Event invalidated;
        invalidated.id = "rl:prompts:invalidated";
        invalidated.setString("shot_id", shotId);
        pluginManager->trigger(invalidated);
    });
    pm->on("rl:shot:correction", [this](Event const &event) { handleShotCorrection(event); });
    pm->on("rl:community-upload:ready", [this](Event const &) {
        WorkItem work;
        work.kind = WorkKind::CommunitySweep;
        enqueueWork(std::move(work));
    });
    pm->on("rl:recommendation:apply", [this](Event const &event) { handleRecommendationApply(event); });
    pm->on("rl:recommendation:ignore", [this](Event const &event) { handleRecommendationIgnore(event); });
    pm->on("rl:prompt:claim", [this](Event &event) { handlePromptClaim(event); });
    pm->on("rl:prompt:release", [this](Event &event) {
        event.setInt("release", 1);
        handlePromptClaim(event);
    });
    pm->on("rl:preference", [this](Event const &event) { handlePreferencePersisted(event); });
    pm->on("rl:local:reset", [this](Event const &) {
        const bool resetComplete = reset();
        snapshotContexts();
        if (resetComplete) {
            Event invalidated;
            invalidated.id = "rl:prompts:invalidated";
            pluginManager->trigger(invalidated);
        }
    });

    requestStatusRefresh();
}

bool LocalAutoTuningStorePlugin::enqueueShot(AutoTuning::ShotRecord const &shot,
                                             AutoTuning::ShotCompletion const &completion,
                                             AutoTuning::ShotCaptureDisposition const &disposition) {
    if (shot.shotId.empty() || shot.samples.empty()) {
        return false;
    }
    auto queued = std::make_unique<QueuedShot>();
    queued->samples.assign(shot.samples.begin(), shot.samples.end());
    queued->shot = shot;
    queued->shot.samples =
        AutoTuning::ArrayView<const AutoTuning::ShotSample>(queued->samples.data(), queued->samples.size());
    queued->completion = completion;
    queued->disposition = disposition;
    queued->committedAt = nowEpoch();

    WorkItem work;
    work.kind = WorkKind::StoreShot;
    work.shotId = shot.shotId.c_str();
    work.queuedShot = std::move(queued);
    pendingDoseRecoveryChecked.store(true, std::memory_order_release);
    return enqueueWork(std::move(work));
}

bool LocalAutoTuningStorePlugin::persistShot(AutoTuning::ShotRecord const &shot,
                                             AutoTuning::ShotCompletion const &completion,
                                             AutoTuning::ShotCaptureDisposition const &disposition,
                                             const AutoTuning::Timestamp committedAt) {
    LocalStoreLock lock(storeMutex);
    if (!ensureDirectories()) {
        return false;
    }

    auto artifactStorage = makePsramUnique<AutoTuning::CompletedShotArtifact>();
    auto &artifact = *artifactStorage;
    artifact.record = shot;
    artifact.completion = completion;
    artifact.disposition = disposition;
    artifact.samples.assign(shot.samples.begin(), shot.samples.end());
    artifact.revision = 1;
    artifact.committedAt = committedAt;
    artifact.bindSamples();
    if (!artifactStore.write(artifact)) {
        ESP_LOGW(LOG_TAG, "Failed to commit canonical shot artifact for %s", shot.shotId.c_str());
        requestStatusRefresh();
        return false;
    }

    const String shotId(shot.shotId.c_str());
    const bool replaySaved = saveReplaySnapshot(artifact);
    if (!replaySaved) {
        ESP_LOGW(LOG_TAG, "Canonical shot %s committed; replay projection will recover on boot", shotId.c_str());
        requestStatusRefresh();
        return false;
    }
    const bool summarySaved = upsertArtifactSummary(artifact);
    if (!summarySaved) {
        ESP_LOGW(LOG_TAG, "Canonical shot %s committed without compact summary", shotId.c_str());
    }
    prune();
    if (!disposition.doseConfirmationRequired) {
        deliveryWorkPending = true;
        nextDeliveryCheckAt = 0;
    }
    requestStatusRefresh();
    return true;
}

bool LocalAutoTuningStorePlugin::correctShot(AutoTuning::ShotCorrection const &correction,
                                             AutoTuning::CorrectedShotRecord &corrected, std::string &reason) {
    LocalStoreLock lock(storeMutex);
    reason.clear();
    if (!controller || correction.shotId.empty()) {
        reason = "Shot correction is missing its shot ID";
        return false;
    }
    if (!correction.relativeGrindStepsFromReference.has_value() && !correction.currentAbsoluteStep.has_value() &&
        !correction.doseInG.has_value() && !correction.targetYieldG.has_value() && !correction.beverageOutG.has_value()) {
        reason = "Shot correction contains no editable parameters";
        return false;
    }

    auto finite = [](std::optional<float> const &value) { return !value.has_value() || std::isfinite(*value); };
    if (!finite(correction.relativeGrindStepsFromReference) || !finite(correction.currentAbsoluteStep) ||
        !finite(correction.doseInG) || !finite(correction.targetYieldG) || !finite(correction.beverageOutG)) {
        reason = "Shot correction values must be finite";
        return false;
    }

    if (correction.relativeGrindStepsFromReference.has_value() &&
        std::fabs(*correction.relativeGrindStepsFromReference) > AutoTuning::RECIPE_DOMAIN_GRIND_RADIUS_MAX_STEPS) {
        reason = "Corrected grind setting is outside the integrity envelope";
        return false;
    }
    if (correction.doseInG.has_value() &&
        (*correction.doseInG < AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G ||
         *correction.doseInG > AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G)) {
        reason = "Corrected dose is outside the integrity envelope";
        return false;
    }
    if (correction.targetYieldG.has_value() &&
        (*correction.targetYieldG < AutoTuning::RECIPE_DOMAIN_OUTPUT_MIN_G ||
         *correction.targetYieldG > AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G)) {
        reason = "Corrected target yield is outside the integrity envelope";
        return false;
    }
    if (correction.beverageOutG.has_value() &&
        (*correction.beverageOutG < AutoTuning::RECIPE_DOMAIN_OUTPUT_MIN_G ||
         *correction.beverageOutG > AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G)) {
        reason = "Corrected beverage output is outside the integrity envelope";
        return false;
    }

    const String shotId(correction.shotId.c_str());
    JsonDocument envelope(&psramAllocator);
    if (!loadReplaySnapshot(shotId, envelope)) {
        summaryStore.patchShotCorrection(correction.shotId.c_str(), correction);
        requestStatusRefresh();
        return true;
    }

    auto artifactStorage = makePsramUnique<AutoTuning::CompletedShotArtifact>();
    auto &artifact = *artifactStorage;
    if (!loadCommittedShot(shotId, artifact)) {
        reason = "Stored shot artifact is unavailable";
        return false;
    }
    const bool canonicalArtifact = artifactStore.exists(shotId);
    if (artifact.record.shotId != correction.shotId) {
        reason = "Shot correction does not match the stored replay";
        return false;
    }

    AutoTuning::ShotRecord &record = artifact.record;
    if (correction.excludeFromLocalOptimization.has_value()) {
        record.excludeFromLocalOptimization = *correction.excludeFromLocalOptimization;
    }
    if (correction.grindFollowed.has_value()) {
        record.grindFollowed = correction.grindFollowed;
    }
    if (correction.doseFollowed.has_value()) {
        record.doseFollowed = correction.doseFollowed;
    }
    if (correction.yieldFollowed.has_value()) {
        record.yieldFollowed = correction.yieldFollowed;
    }
    AutoTuning::GrinderSnapshot &grinder = record.recipe.grinder;
    std::optional<float> relativeGrind = correction.relativeGrindStepsFromReference;
    if (correction.currentAbsoluteStep.has_value()) {
        if (!grinder.absoluteReferenceStep.has_value()) {
            reason = "This shot has no absolute grinder reference";
            return false;
        }
        const float derivedRelative = *correction.currentAbsoluteStep - *grinder.absoluteReferenceStep;
        if (relativeGrind.has_value() && std::fabs(*relativeGrind - derivedRelative) > 0.01f) {
            reason = "Absolute and relative grind corrections disagree";
            return false;
        }
        relativeGrind = derivedRelative;
    }

    if (relativeGrind.has_value() &&
        std::fabs(*relativeGrind) > AutoTuning::RECIPE_DOMAIN_GRIND_RADIUS_MAX_STEPS) {
        reason = "Corrected grind setting is outside the integrity envelope";
        return false;
    }
    if (relativeGrind.has_value()) {
        grinder.relativeStepsFromReference = *relativeGrind;
        grinder.observed = true;
        if (grinder.absoluteReferenceStep.has_value()) {
            grinder.currentAbsoluteStep = *grinder.absoluteReferenceStep + *relativeGrind;
        }
        if (grinder.micronsPerStep.has_value()) {
            const float directionSign = grinder.stepDirection == "higher_is_finer" ? 1.0f : -1.0f;
            grinder.relativeMicronsFromReference = *relativeGrind * *grinder.micronsPerStep * directionSign;
        }
    }
    if (correction.doseInG.has_value()) {
        record.measuredDoseG = *correction.doseInG;
        record.doseObserved = true;
        record.doseTargetConfirmed = false;
        record.recipe.doseTargetG = *correction.doseInG;
        if (record.recommendation.present()) {
            const float doseErrorG = std::fabs(*correction.doseInG - record.recommendation.nextDoseG);
            record.doseFollowed = doseErrorG <= AutoTuning::DOSE_FOLLOW_THROUGH_TOLERANCE_G;
        }
    }
    if (correction.targetYieldG.has_value()) {
        record.recipe.targetYieldG = *correction.targetYieldG;
    }
    if (correction.beverageOutG.has_value()) {
        record.beverageOutG = *correction.beverageOutG;
        record.beverageOutObservation = "user_corrected";
    }
    if (record.recipe.doseTargetG.has_value() && record.recipe.targetYieldG.has_value()) {
        record.recipe.targetRatio = *record.recipe.targetYieldG / *record.recipe.doseTargetG;
    }

    if (!record.recipe.doseTargetG.has_value() || !record.recipe.targetYieldG.has_value() ||
        !record.recipe.targetRatio.has_value()) {
        reason = "Corrected shot is missing a complete optimizer recipe";
        return false;
    }
    if (*record.recipe.doseTargetG < AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G ||
        *record.recipe.doseTargetG > AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G ||
        *record.recipe.targetYieldG < AutoTuning::RECIPE_DOMAIN_OUTPUT_MIN_G ||
        *record.recipe.targetYieldG > AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G ||
        !std::isfinite(*record.recipe.targetRatio) || *record.recipe.targetRatio <= 0.0f) {
        reason = "Corrected recipe is outside the integrity envelope";
        return false;
    }

    corrected.record = record;
    corrected.samples.assign(artifact.samples.begin(), artifact.samples.end());
    corrected.bindSamples();

    JsonDocument correctedPayload(&psramAllocator);
    if (!AutoTuningJsonCodec::writeShotRecord(corrected.record, correctedPayload)) {
        reason = "Unable to serialize the corrected shot";
        return false;
    }
    JsonObject replay = envelope.as<JsonObject>();
    const EpochSeconds now = nowEpoch();
    if (canonicalArtifact) {
        artifact.record = corrected.record;
        artifact.samples.assign(corrected.samples.begin(), corrected.samples.end());
        artifact.revision += 1;
        artifact.bindSamples();
        if (!artifactStore.write(artifact)) {
            reason = "Unable to commit the corrected shot artifact";
            return false;
        }
        replay.remove("payload");
        replay.remove("completion");
        replay["schema_version"] = 3;
        updateReplayArtifactIdentity(replay, artifact);
    } else {
        replay["payload"].set(correctedPayload.as<JsonObjectConst>());
        replay["payload_revision"] = (replay["payload_revision"] | 0) + 1;
    }
    const AutoTuning::DeliveryStatus currentDelivery = deliveryState(replay).status;
    if (currentDelivery == AutoTuning::DeliveryStatus::Pending ||
        currentDelivery == AutoTuning::DeliveryStatus::RetryWait ||
        currentDelivery == AutoTuning::DeliveryStatus::AwaitingAcknowledgement) {
        replay["local_delivery_state"] = "pending";
        replay["local_next_retry_at"] = 0;
        replay["local_last_error"] = nullptr;
        replay.remove("active_attempt_id");
        transitionPrompt(replay, AutoTuning::PromptStatus::Processing, now);
        deliveryWorkPending = true;
        nextDeliveryCheckAt = 0;
    }
    replay["corrected_at"] = now;
    replay["updated_at"] = now;
    if (replay["community_required"] | false) {
        replay["community_dispatched"] = false;
    }
    if (!LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
        reason = "Unable to persist the corrected shot replay";
        return false;
    }
    if (!summaryStore.upsertShot(correctedPayload.as<JsonObjectConst>())) {
        reason = "Unable to persist the corrected shot summary";
        return false;
    }
    requestStatusRefresh();
    return true;
}

bool LocalAutoTuningStorePlugin::storeRecommendation(AutoTuning::Recommendation const &recommendation) {
    WorkItem work;
    work.kind = WorkKind::StoreRecommendation;
    work.recommendation =
        std::make_unique<AutoTuning::Recommendation>(recommendation);
    return enqueueWork(std::move(work));
}

bool LocalAutoTuningStorePlugin::persistRecommendation(
    AutoTuning::Recommendation const &recommendation) {
    LocalStoreLock lock(storeMutex);
    JsonDocument payload(&psramAllocator);
    if (!AutoTuningJsonCodec::writeRecommendation(recommendation, payload) ||
        !summaryStore.upsertRecommendation(payload.as<JsonObjectConst>())) {
        return false;
    }
    prune();
    requestStatusRefresh();
    return true;
}

void LocalAutoTuningStorePlugin::loop() {
    drainPromptEvents();
    drainStoredShots();
    drainShotCompletions();
    if (!pendingDoseRecoveryChecked.load(std::memory_order_acquire) && millis() >= 1000) {
        pendingDoseRecoveryChecked.store(true, std::memory_order_release);
        WorkItem recovery;
        recovery.kind = WorkKind::DoseConfirmationRecovery;
        enqueueWork(std::move(recovery));
    }
    if (millis() - lastDeliverySweepMs >= DELIVERY_SWEEP_INTERVAL_MS) {
        lastDeliverySweepMs = millis();
        WorkItem delivery;
        delivery.kind = WorkKind::DeliverySweep;
        enqueueWork(std::move(delivery));
        if (controller && controller->getSettings().isRLCommunityUploadEnabled() && nowEpoch() >= EpochTime::MIN_VALID) {
            WorkItem community;
            community.kind = WorkKind::CommunitySweep;
            enqueueWork(std::move(community));
        }
    }
    if (millis() - lastStatusMs >= STATUS_INTERVAL_MS) {
        requestStatusRefresh();
    }
#if defined(GAGGIMATE_SIM)
    if (workerTaskHandle == nullptr) {
        processOneWorkItem();
    }
#endif
    if (statusPublishRequested.exchange(false, std::memory_order_acq_rel)) {
        publishStatus();
    }
}

bool LocalAutoTuningStorePlugin::ensureDirectories() {
    const bool available =
        LocalAutoTuningFiles::ensureDirectory(STORE_DIR) && summaryStore.begin() &&
        contextStore.begin() && LocalAutoTuningFiles::ensureDirectory(REPLAY_DIR);
    if (available) {
        LocalAutoTuningFiles::recoverDirectory(REPLAY_DIR);
    }
    if (!available) {
        return false;
    }
    if (!artifactStoreReady) {
        artifactStoreReady = artifactStore.begin();
    }
    return artifactStoreReady;
}

bool LocalAutoTuningStorePlugin::reset() {
    LocalStoreLock lock(storeMutex);
    if (!ensureDirectories()) {
        return false;
    }
    // The artifact is the canonical record. Clear derived state first so an
    // interrupted reset can be reconstructed from any artifact that remains.
    const bool projectionsCleared =
        summaryStore.reset() && LocalAutoTuningFiles::clearDirectory(REPLAY_DIR) &&
        contextStore.clear();
    if (!projectionsCleared) {
        requestStatusRefresh();
        return false;
    }
    std::vector<String> artifactShotIds;
    const bool listed = artifactStore.listShotIds(artifactShotIds);
    bool artifactsCleared = listed;
    for (const String &shotId : artifactShotIds) {
        artifactsCleared = artifactStore.remove(shotId) && artifactsCleared;
    }
    const bool ok = projectionsCleared && artifactsCleared;
    {
        std::lock_guard<std::mutex> guard(workMutex);
        storedShotNotices.clear();
        promptEvents.clear();
        completionNotices.clear();
        queuedCompletionShotIds.clear();
        claimablePrompts.clear();
    }
    deliveryWorkPending = false;
    nextDeliveryCheckAt = 0;
    requestStatusRefresh();
    return ok;
}

AutoTuning::LocalStoreStats LocalAutoTuningStorePlugin::stats() const {
    std::lock_guard<std::mutex> guard(statusMutex);
    return cachedStats;
}

bool LocalAutoTuningStorePlugin::loadShotSummary(const String &shotId, JsonDocument &out) const {
    LocalStoreLock lock(storeMutex);
    return summaryStore.loadShot(shotId, out);
}

bool LocalAutoTuningStorePlugin::hasShotReplay(const String &shotId) const {
    LocalStoreLock lock(storeMutex);
    return !shotId.isEmpty() && LittleFSUtil::existsQuietly(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId));
}

bool LocalAutoTuningStorePlugin::canRemoveShotData(const String &shotId) const {
    LocalStoreLock lock(storeMutex);
    if (shotId.isEmpty()) {
        return false;
    }
    const String replayPath = LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId);
    if (!LittleFSUtil::existsQuietly(replayPath)) {
        return true;
    }
    JsonDocument envelope(&psramAllocator);
    if (!LocalAutoTuningFiles::readJson(replayPath, envelope)) {
        return false;
    }
    JsonObjectConst replay = envelope.as<JsonObjectConst>();
    const bool communityPending = (replay["community_required"] | false) && !(replay["community_dispatched"] | false);
    return deliveryTerminal(replay) && !communityPending;
}

bool LocalAutoTuningStorePlugin::removeShotData(const String &shotId, const bool force) {
    LocalStoreLock lock(storeMutex);
    if (shotId.isEmpty()) {
        return false;
    }
    if (!force && !canRemoveShotData(shotId)) {
        return false;
    }
    bool removed = false;
    const String replayPath = LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId);
    removed = summaryStore.removeShot(shotId) || removed;
    if (LittleFSUtil::existsQuietly(replayPath)) {
        auto lease = StorageCoordinator::instance().acquireFlash();
        removed = LittleFS.remove(replayPath) || removed;
    }
    removed = artifactStore.remove(shotId) || removed;
    if (removed) {
        {
            std::lock_guard<std::mutex> guard(workMutex);
            promptEvents.erase(
                std::remove_if(promptEvents.begin(), promptEvents.end(),
                               [&shotId](Event const &event) {
                                   return event.getString("shot_id") == shotId;
                               }),
                promptEvents.end());
            storedShotNotices.erase(
                std::remove_if(storedShotNotices.begin(), storedShotNotices.end(),
                               [&shotId](StoredShotNotice const &notice) {
                                   return notice.shotId == shotId;
                               }),
                storedShotNotices.end());
            completionNotices.erase(
                std::remove_if(completionNotices.begin(), completionNotices.end(),
                               [&shotId](CompletionNotice const &notice) {
                                   return notice.shotId == shotId;
                               }),
                completionNotices.end());
            queuedCompletionShotIds.erase(
                std::remove(queuedCompletionShotIds.begin(),
                            queuedCompletionShotIds.end(), shotId),
                queuedCompletionShotIds.end());
            claimablePrompts.erase(
                std::remove_if(claimablePrompts.begin(), claimablePrompts.end(),
                               [&shotId](ClaimablePrompt const &prompt) {
                                   return prompt.shotId == shotId;
                               }),
                claimablePrompts.end());
        }
        deliveryWorkPending = true;
        nextDeliveryCheckAt = 0;
        requestStatusRefresh();
    }
    return removed;
}

void LocalAutoTuningStorePlugin::handleShotDispatch(Event const &event) {
    WorkItem work;
    work.kind = WorkKind::Dispatch;
    work.shotId = event.getString("shot_id");
    enqueueWork(std::move(work));
}

void LocalAutoTuningStorePlugin::handleDoseConfirmation(Event const &event) {
    const String shotId = event.getString("shot_id");
    const std::uint32_t promptRevision =
        static_cast<std::uint32_t>(std::max<std::int64_t>(
            event.getInt64("prompt_revision"), 0));
    if (shotId.isEmpty() || promptRevision == 0 ||
        event.getInt("prompt_claimed") != 1 ||
        !event.getPayload<AutoTuning::RecipeConfirmation>()) {
        return;
    }
    WorkItem work;
    work.kind = WorkKind::DoseConfirmation;
    work.shotId = shotId;
    work.promptRevision = promptRevision;
    work.recipeConfirmation = *event.getPayload<AutoTuning::RecipeConfirmation>();
    if (!enqueueWork(std::move(work))) {
        Event release;
        release.id = "rl:prompt:release";
        release.setString("shot_id", shotId);
        release.setInt64("prompt_revision", promptRevision);
        pluginManager->trigger(release);
    }
}

void LocalAutoTuningStorePlugin::processDoseConfirmation(
    const String &shotId, const std::uint32_t promptRevision,
    AutoTuning::RecipeConfirmation const &answer) {
    LocalStoreLock lock(storeMutex);
    const auto releaseClaim = [this, &shotId, promptRevision]() {
        Event release;
        release.id = "rl:prompt:release";
        release.setString("shot_id", shotId);
        release.setInt64("prompt_revision", promptRevision);
        queuePromptEvent(std::move(release));
    };
    JsonDocument envelope(&psramAllocator);
    if (!loadReplaySnapshot(shotId, envelope)) {
        releaseClaim();
        if (hasShotReplay(shotId)) {
            ESP_LOGE(LOG_TAG, "Failed to load dose confirmation replay for shot %s", shotId.c_str());
            return;
        }
        ESP_LOGW(LOG_TAG, "Discarding stale dose confirmation for unavailable shot %s", shotId.c_str());
        Event resolved;
        resolved.id = "rl:dose-confirmation:resolved";
        resolved.setString("shot_id", shotId);
        resolved.setInt64("prompt_revision", promptRevision);
        resolved.setInt("followed", answer.answer != AutoTuning::RecipeAnswer::Unknown ? 1 : 0);
        resolved.setInt("persisted", 0);
        queuePromptEvent(std::move(resolved));
        return;
    }
    JsonObject root = envelope.as<JsonObject>();
    const AutoTuning::PromptState currentPrompt = promptState(root);
    if (currentPrompt.revision != promptRevision ||
        deliveryState(root).status !=
            AutoTuning::DeliveryStatus::AwaitingDoseConfirmation) {
        releaseClaim();
        return;
    }
    auto artifactStorage = makePsramUnique<AutoTuning::CompletedShotArtifact>();
    auto &artifact = *artifactStorage;
    if (!loadCommittedShot(shotId, artifact)) {
        releaseClaim();
        return;
    }
    if (!AutoTuning::confirmRecipe(artifact.record, answer)) {
        releaseClaim();
        return;
    }
    const bool followed = answer.answer != AutoTuning::RecipeAnswer::Unknown;
    artifact.disposition.doseConfirmationRequired = false;
    const bool canonicalArtifact = artifactStore.exists(shotId);
    JsonDocument payload(&psramAllocator);
    if (canonicalArtifact) {
        artifact.revision += 1;
        artifact.bindSamples();
        if (!artifactStore.write(artifact)) {
            ESP_LOGE(LOG_TAG, "Failed to commit dose confirmation for shot %s", shotId.c_str());
            releaseClaim();
            return;
        }
        root.remove("payload");
        root.remove("completion");
        root["schema_version"] = 3;
        updateReplayArtifactIdentity(root, artifact);
    } else if (!AutoTuningJsonCodec::writeShotRecord(artifact.record, payload)) {
        releaseClaim();
        return;
    } else {
        root["payload"].set(payload.as<JsonObjectConst>());
    }
    const EpochSeconds now = nowEpoch();
    root["dose_confirmation_status"] = followed ? "confirmed" : "unknown";
    root["dispatch_state"] = "ready";
    root["local_delivery_state"] = "pending";
    root["local_next_retry_at"] = 0;
    root["updated_at"] = now;
    transitionPrompt(root, AutoTuning::PromptStatus::Processing, now);
    if (!LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
        ESP_LOGE(LOG_TAG, "Failed to persist dose confirmation for shot %s", shotId.c_str());
        releaseClaim();
        return;
    }

    if (canonicalArtifact) {
        upsertArtifactSummary(artifact);
    } else {
        summaryStore.upsertShot(payload.as<JsonObjectConst>());
    }
    Event resolved;
    resolved.id = "rl:dose-confirmation:resolved";
    resolved.setString("shot_id", shotId);
    resolved.setInt64("prompt_revision", promptRevision);
    resolved.setInt("followed", followed ? 1 : 0);
    resolved.setInt("persisted", 1);
    {
        std::lock_guard<std::mutex> guard(workMutex);
        // A recovery notice may have been queued before this answer was saved.
        // Do not reopen that obsolete prompt after emitting the saved result.
        storedShotNotices.erase(
            std::remove_if(storedShotNotices.begin(), storedShotNotices.end(),
                           [&shotId, promptRevision](StoredShotNotice const &notice) {
                               return notice.shotId == shotId &&
                                      notice.promptRevision == promptRevision;
                           }),
            storedShotNotices.end());
        claimablePrompts.erase(
            std::remove_if(
                claimablePrompts.begin(), claimablePrompts.end(),
                [&shotId, promptRevision](ClaimablePrompt const &prompt) {
                    return prompt.shotId == shotId &&
                           prompt.revision == promptRevision;
                }),
            claimablePrompts.end());
    }
    queuePromptEvent(std::move(resolved));
    deliveryWorkPending = true;
    nextDeliveryCheckAt = 0;
    WorkItem work;
    work.kind = WorkKind::Dispatch;
    work.shotId = shotId;
    enqueueWork(std::move(work));
    WorkItem recovery;
    recovery.kind = WorkKind::DoseConfirmationRecovery;
    enqueueWork(std::move(recovery));
}

void LocalAutoTuningStorePlugin::handleShotReprocess(Event &event) {
    WorkItem work;
    work.kind = WorkKind::Reprocess;
    work.shotId = event.getString("shot_id");
    event.setInt("queued", enqueueWork(std::move(work)) ? 1 : 0);
}

bool LocalAutoTuningStorePlugin::prepareShotReprocess(const String &shotId) {
    LocalStoreLock lock(storeMutex);
    JsonDocument envelope(&psramAllocator);
    if (!controller || !loadReplaySnapshot(shotId, envelope)) {
        return false;
    }
    JsonObject root = envelope.as<JsonObject>();
    if (deliveryState(root).status == AutoTuning::DeliveryStatus::AwaitingDoseConfirmation) {
        StoredShotNotice notice;
        notice.shotId = shotId;
        notice.doseConfirmationRequired = true;
        notice.promptRevision = promptState(root).revision;
        auto artifact = makePsramUnique<AutoTuning::CompletedShotArtifact>();
        if (!loadCommittedShot(shotId, *artifact)) return false;
        notice.recipe = RecipePrompt::fromShot(artifact->record, artifact->completion.doseTargetG);
        std::lock_guard<std::mutex> guard(workMutex);
        storedShotNotices.push_back(std::move(notice));
        return true;
    }
    auto artifactStorage = makePsramUnique<AutoTuning::CompletedShotArtifact>();
    auto &artifact = *artifactStorage;
    if (!loadCommittedShot(shotId, artifact)) {
        return false;
    }
    if (!artifact.record.recipe.tasteGoal.valid()) {
        Settings const &settings = controller->getSettings();
        const bool activeContextMatches =
            artifact.record.recipe.beanContextId == settings.getRLBeanContextId().c_str() &&
            artifact.record.recipe.grinder.contextId == settings.getRLGrinderContextId().c_str();
        AutoTuning::TasteGoal activeGoal;
        String goalError;
        if (!activeContextMatches || !AutoTuning::activeTasteGoal(settings, activeGoal, &goalError)) {
            ESP_LOGW(LOG_TAG, "Cannot restore the missing taste goal for shot %s", shotId.c_str());
            return false;
        }
        artifact.record.recipe.tasteGoal = activeGoal;
        if (artifactStore.exists(shotId)) {
            artifact.revision += 1;
            artifact.bindSamples();
            if (!artifactStore.write(artifact)) {
                return false;
            }
            updateReplayArtifactIdentity(root, artifact);
            upsertArtifactSummary(artifact);
        } else {
            JsonDocument payload(&psramAllocator);
            if (!AutoTuningJsonCodec::writeShotRecord(artifact.record, payload)) {
                return false;
            }
            root["payload"].set(payload.as<JsonObjectConst>());
            summaryStore.upsertShot(payload.as<JsonObjectConst>());
        }
        ESP_LOGI(LOG_TAG, "Restored the active taste goal while reprocessing shot %s", shotId.c_str());
    }
    const bool localDeliveryRequired = controller &&
        (controller->getSettings().isRLCommunityUploadEnabled() ||
         AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(),
                           controller->getOptimizerTransport()).optimizationActive());
    root["local_delivery_required"] = localDeliveryRequired;
    root["local_delivery_state"] = localDeliveryRequired ? "pending" : "not_required";
    root["local_next_retry_at"] = 0;
    root["local_last_error"] = nullptr;
    root["community_dispatched"] = false;
    root["community_required"] = false;
    root["completion_emitted"] = !localDeliveryRequired;
    const EpochSeconds now = nowEpoch();
    root["updated_at"] = now;
    transitionPrompt(root, AutoTuning::PromptStatus::Processing, now);
    if (!LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
        return false;
    }
    deliveryWorkPending = true;
    nextDeliveryCheckAt = 0;
    WorkItem work;
    work.kind = WorkKind::Dispatch;
    work.shotId = shotId;
    work.reprocess = true;
    return enqueueWork(std::move(work));
}

bool LocalAutoTuningStorePlugin::transitionPrompt(JsonObject root, const AutoTuning::PromptStatus status,
                                                  const std::int64_t timestamp) const {
    if (root.isNull()) {
        return false;
    }
    const String current = jsonStringOrEmpty(root["prompt_state"]);
    const char *next = AutoTuning::promptStatusKey(status);
    if (current == next) {
        return false;
    }
    root["prompt_state"] = next;
    root["prompt_revision"] = std::max(0, root["prompt_revision"] | 0) + 1;
    root["prompt_updated_at"] = timestamp;
    return true;
}

bool LocalAutoTuningStorePlugin::updateReplayArtifactIdentity(
    JsonObject root, AutoTuning::CompletedShotArtifact const &artifact) const {
    if (root.isNull() || artifact.record.shotId.empty() || artifact.revision == 0 ||
        artifact.payloadHash.size() != 64) {
        return false;
    }
    root["artifact_revision"] = artifact.revision;
    root["artifact_payload_hash"] = artifact.payloadHash.c_str();
    root["artifact_encoding_version"] = 1;
    return true;
}

bool LocalAutoTuningStorePlugin::upsertArtifactSummary(
    AutoTuning::CompletedShotArtifact const &artifact) {
    JsonDocument payload(&psramAllocator);
    return AutoTuningJsonCodec::writeShotRecord(artifact.record, payload) &&
           summaryStore.upsertShot(payload.as<JsonObjectConst>());
}

bool LocalAutoTuningStorePlugin::saveReplaySnapshot(
    AutoTuning::CompletedShotArtifact const &artifact) {
    const String shotId(artifact.record.shotId.c_str());
    if (!ensureDirectories() || shotId.isEmpty() || artifact.payloadHash.size() != 64) {
        return false;
    }
    const bool doseConfirmationRequired = artifact.disposition.doseConfirmationRequired;
    const bool localDeliveryRequired = artifact.disposition.optimizerDeliveryRequired;
    const bool communityUploadRequired = artifact.disposition.communityUploadRequired;
    const EpochSeconds now = nowEpoch();
    JsonDocument envelope(&psramAllocator);
    JsonObject root = envelope.to<JsonObject>();
    root["event_type"] = "local_shot_replay";
    root["schema_version"] = 3;
    root["shot_id"] = shotId;
    root["captured_at"] = artifact.committedAt;
    root["updated_at"] = now;
    root["dispatch_state"] = doseConfirmationRequired ? "awaiting_dose_confirmation" : "ready";
    root["local_delivery_state"] =
        doseConfirmationRequired ? "awaiting_dose_confirmation" : (localDeliveryRequired ? "pending" : "not_required");
    root["local_delivery_required"] = localDeliveryRequired;
    root["dose_confirmation_status"] =
        doseConfirmationRequired ? "pending" : (localDeliveryRequired ? "measured" : "not_required");
    root["dispatch_count"] = 0;
    root["local_attempt_count"] = 0;
    root["local_next_retry_at"] = 0;
    root["community_dispatched"] = false;
    root["community_required"] = communityUploadRequired;
    root["completion_emitted"] = !localDeliveryRequired;
    root["dose_target_g"] = artifact.completion.doseTargetG;
    root["prompt_state"] = AutoTuning::promptStatusKey(AutoTuning::PromptStatus::Processing);
    root["prompt_revision"] = 1;
    root["prompt_updated_at"] = now;
    updateReplayArtifactIdentity(root, artifact);
    return LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
}

bool LocalAutoTuningStorePlugin::loadReplaySnapshot(const String &shotId, JsonDocument &out) const {
    return !shotId.isEmpty() && LocalAutoTuningFiles::readJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), out);
}

bool LocalAutoTuningStorePlugin::dispatchStoredShot(const String &shotId, bool reprocess, bool automaticRetry) {
    if (!pluginManager || shotId.isEmpty()) {
        return false;
    }
    const EpochSeconds now = nowEpoch();
    if (now < EpochTime::MIN_VALID) {
        deliveryWorkPending = true;
        nextDeliveryCheckAt = 0;
        return false;
    }

    auto artifactStorage = makePsramUnique<AutoTuning::CompletedShotArtifact>();
    auto &artifact = *artifactStorage;
    AutoTuning::ShotDeliveryAttempt attempt;
    bool localDeliveryRequired = false;
    JsonDocument envelope(&psramAllocator);
    {
        LocalStoreLock lock(storeMutex);
        if (!loadReplaySnapshot(shotId, envelope) || !loadCommittedShot(shotId, artifact)) {
            return false;
        }
        JsonObject root = envelope.as<JsonObject>();
        if (deliveryState(root).status == AutoTuning::DeliveryStatus::AwaitingDoseConfirmation) {
            return false;
        }
        localDeliveryRequired =
            root["local_delivery_required"].isNull() ? true
                                                     : root["local_delivery_required"].as<bool>();
        // Migrate old device-owned delivery onto the acknowledged container path.
        if ((root["community_required"] | false) && !(root["community_dispatched"] | false)) {
            localDeliveryRequired = true;
            root["local_delivery_required"] = true;
            root["community_required"] = false;
            root["community_dispatched"] = true;
        }

        if (artifact.record.timestamp < EpochTime::MIN_VALID) {
            artifact.record.timestamp = now;
            if (artifactStore.exists(shotId)) {
                artifact.revision += 1;
                artifact.bindSamples();
                if (!artifactStore.write(artifact)) {
                    return false;
                }
                updateReplayArtifactIdentity(root, artifact);
                upsertArtifactSummary(artifact);
            } else {
                JsonDocument legacyPayload(&psramAllocator);
                if (!AutoTuningJsonCodec::writeShotRecord(artifact.record, legacyPayload)) {
                    return false;
                }
                root["payload"].set(legacyPayload.as<JsonObjectConst>());
            }
        }

        const int previousAttempts =
            jsonNumber(root["local_attempt_count"])
                ? root["local_attempt_count"].as<int>()
                : (root["dispatch_count"] | 0);
        const int attemptCount = previousAttempts + 1;
        attempt.shotId = artifact.record.shotId;
        attempt.recordRevision = artifact.revision;
        attempt.reprocess = reprocess;
        if (!attempt.valid()) {
            return false;
        }

        root["schema_version"] = artifact.payloadHash.empty() ? (root["schema_version"] | 2) : 3;
        root["dispatch_state"] = "dispatching";
        root["updated_at"] = now;
        root["local_attempt_count"] = attemptCount;
        root["local_last_attempt_at"] = now;
        root["local_next_retry_at"] = 0;
        root["local_last_error"] = nullptr;
        root["active_record_revision"] = attempt.recordRevision;
        root.remove("active_attempt_id");
        root.remove("active_payload_hash");
        root.remove("active_artifact_revision");
        root.remove("active_encoding_version");
        root["local_delivery_state"] =
            localDeliveryRequired ? "pending" : "not_required";
        if (!LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
            return false;
        }
    }

    AutoTuning::ShotSubmissionResult submission{
        AutoTuning::ShotSubmissionOutcome::Submitted, "not_required"};
    if (localDeliveryRequired) {
        AutoTuning::OptimizerTransportPort *transport =
            controller ? controller->getOptimizerTransport() : nullptr;
        submission =
            transport
                ? transport->publishShot(artifact.record, attempt)
                : AutoTuning::ShotSubmissionResult{
                      AutoTuning::ShotSubmissionOutcome::NotConnected,
                      "optimizer_transport_unavailable"};
    }


    {
        LocalStoreLock lock(storeMutex);
        JsonDocument latest(&psramAllocator);
        if (!loadReplaySnapshot(shotId, latest)) {
            return false;
        }
        JsonObject root = latest.as<JsonObject>();
        if ((root["active_record_revision"] | 0U) != attempt.recordRevision ||
            (root["artifact_revision"] | (root["payload_revision"] | 1U)) != attempt.recordRevision) {
            return false;
        }
        const int attemptCount = root["local_attempt_count"] | 1;
        root["dispatch_count"] = (root["dispatch_count"] | 0) + 1;
        root["updated_at"] = nowEpoch();
        if (!localDeliveryRequired) {
            root["dispatch_state"] = "not_required";
            root["local_delivery_state"] = "not_required";
            root["local_next_retry_at"] = 0;
            transitionPrompt(root, AutoTuning::PromptStatus::Resolved, nowEpoch());
        } else if (submission.submitted()) {
            root["dispatch_state"] = "dispatched";
            root["local_delivery_state"] = "awaiting_ack";
            root["local_next_retry_at"] =
                nowEpoch() + deliveryRetryDelaySeconds(attemptCount);
            transitionPrompt(root, AutoTuning::PromptStatus::AwaitingAcknowledgement,
                             nowEpoch());
        } else if (submission.retryable()) {
            root["dispatch_state"] = "retry_wait";
            root["local_delivery_state"] = "retry_wait";
            root["local_next_retry_at"] =
                nowEpoch() + deliveryRetryDelaySeconds(attemptCount);
            root["local_last_error"] = submission.reason.c_str();
            transitionPrompt(root, AutoTuning::PromptStatus::DeliveryRetrying, nowEpoch());
        } else {
            root["dispatch_state"] = "delivery_error";
            root["local_delivery_state"] = "permanent_rejection";
            root["local_next_retry_at"] = 0;
            root["local_last_error"] = submission.reason.c_str();
            transitionPrompt(root, AutoTuning::PromptStatus::DeliveryError, nowEpoch());
        }
        if (!LocalAutoTuningFiles::writeJson(
                LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), latest)) {
            return false;
        }
        deliveryWorkPending = localDeliveryRequired && !deliveryState(root).terminal();
        nextDeliveryCheckAt = jsonEpochOrZero(root["local_next_retry_at"]);
    }
    refreshCachedStatus();
    return !localDeliveryRequired || submission.submitted();
}

bool LocalAutoTuningStorePlugin::loadCommittedShot(
    const String &shotId, AutoTuning::CompletedShotArtifact &artifact) const {
    if (shotId.isEmpty()) {
        return false;
    }
    if (artifactStore.load(shotId, artifact)) {
        return true;
    }

    // Read-only migration fallback for snapshots created before the canonical
    // artifact format. New writes never duplicate the payload in replay JSON.
    JsonDocument replay(&psramAllocator);
    if (!loadReplaySnapshot(shotId, replay)) {
        return false;
    }
    JsonObjectConst root = replay.as<JsonObjectConst>();
    auto decoded = makePsramUnique<AutoTuningJsonCodec::DecodedShotRecord>();
    auto migrated = makePsramUnique<AutoTuning::CompletedShotArtifact>();
    String error;
    if (!AutoTuningJsonCodec::parseShotRecord(root["payload"], *decoded, error) ||
        !AutoTuningJsonCodec::parseShotCompletion(root["completion"], migrated->completion, error)) {
        return false;
    }
    artifact = std::move(*migrated);
    artifact.record = std::move(decoded->record);
    artifact.disposition.doseConfirmationRequired =
        deliveryState(root).status == AutoTuning::DeliveryStatus::AwaitingDoseConfirmation;
    artifact.disposition.optimizerDeliveryRequired =
        root["local_delivery_required"].isNull() || root["local_delivery_required"].as<bool>();
    artifact.disposition.communityUploadRequired = root["community_required"] | false;
    artifact.samples.assign(decoded->samples.begin(), decoded->samples.end());
    artifact.revision = std::max(1, root["payload_revision"] | 1);
    artifact.committedAt = jsonEpochOrZero(root["captured_at"]);
    artifact.bindSamples();
    return true;
}

bool LocalAutoTuningStorePlugin::recoverCommittedArtifacts() {
    LocalStoreLock lock(storeMutex);
    if (!ensureDirectories()) {
        return false;
    }
    std::vector<String> shotIds;
    if (!artifactStore.listShotIds(shotIds)) {
        return false;
    }
    bool recovered = true;
    for (const String &shotId : shotIds) {
        auto artifactStorage = makePsramUnique<AutoTuning::CompletedShotArtifact>();
        auto &artifact = *artifactStorage;
        if (!artifactStore.load(shotId, artifact)) {
            recovered = false;
            continue;
        }
        JsonDocument replay(&psramAllocator);
        if (!loadReplaySnapshot(shotId, replay)) {
            recovered = saveReplaySnapshot(artifact) && recovered;
        } else {
            JsonObject root = replay.as<JsonObject>();
            const bool identityChanged =
                jsonStringOrEmpty(root["artifact_payload_hash"]) != artifact.payloadHash.c_str() ||
                (root["artifact_revision"] | 0U) != artifact.revision;
            root["schema_version"] = 3;
            root.remove("payload");
            root.remove("completion");
            updateReplayArtifactIdentity(root, artifact);
            if (root["dose_target_g"].isNull()) {
                root["dose_target_g"] = artifact.completion.doseTargetG;
            }
            if (root["prompt_state"].isNull()) {
                root["prompt_state"] =
                    AutoTuning::promptStatusKey(AutoTuning::PromptStatus::Processing);
                root["prompt_revision"] = 1;
                root["prompt_updated_at"] = nowEpoch();
            }
            if (identityChanged) {
                root["updated_at"] = nowEpoch();
            }
            recovered =
                LocalAutoTuningFiles::writeJson(
                    LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), replay) &&
                recovered;
        }
        recovered = upsertArtifactSummary(artifact) && recovered;
        AutoTuning::CompletedShotProjectionPort *projection =
            controller ? controller->getCompletedShotProjection() : nullptr;
        if (projection) {
            recovered = projection->ensureProjection(artifact) && recovered;
        }
    }
    requestStatusRefresh();
    return recovered;
}

void LocalAutoTuningStorePlugin::dispatchPendingCommunityUploads() {
    if (!pluginManager) {
        return;
    }
    std::vector<String> paths;
    if (!LocalAutoTuningFiles::listRecordPaths(REPLAY_DIR, paths)) {
        return;
    }
    for (const String &path : paths) {
        String shotId;
        auto artifactStorage = makePsramUnique<AutoTuning::CompletedShotArtifact>();
        auto &artifact = *artifactStorage;
        bool due = false;
        JsonDocument envelope(&psramAllocator);
        {
            LocalStoreLock lock(storeMutex);
            if (LocalAutoTuningFiles::readJson(path, envelope)) {
                JsonObjectConst root = envelope.as<JsonObjectConst>();
                shotId = root["shot_id"].as<String>();
                due = (root["community_required"] | false) &&
                      !(root["community_dispatched"] | false) &&
                      deliveryState(root).status !=
                          AutoTuning::DeliveryStatus::AwaitingDoseConfirmation &&
                      loadCommittedShot(shotId, artifact);
            }
        }
        if (due) dispatchStoredShot(shotId, false);

    }
}

void LocalAutoTuningStorePlugin::handleShotDeliveryAck(Event const &event) {
    AutoTuning::ShotDeliveryAcknowledgement const *acknowledgement =
        event.getPayload<AutoTuning::ShotDeliveryAcknowledgement>();
    if (!acknowledgement) {
        return;
    }
    WorkItem work;
    work.kind = WorkKind::DeliveryAcknowledgement;
    work.shotId = acknowledgement->shotId.c_str();
    work.outcome = acknowledgement->outcome.c_str();
    work.reason = acknowledgement->reason.c_str();
    work.timestamp = acknowledgement->timestamp;
    work.recordRevision = acknowledgement->recordRevision;
    work.preferenceRequest = acknowledgement->preferenceRequest;
    enqueueWork(std::move(work));
}

void LocalAutoTuningStorePlugin::processShotDeliveryAck(const String &shotId, const String &outcome, const String &reason,
                                                        const std::int64_t acknowledgementTimestamp,
                                                        const std::uint32_t recordRevision,
                                                         std::optional<AutoTuning::PreferenceRequest> const &preferenceRequest) {
    LocalStoreLock lock(storeMutex);
    JsonDocument envelope(&psramAllocator);
    if (shotId.isEmpty() || !loadReplaySnapshot(shotId, envelope)) {
        return;
    }
    JsonObject root = envelope.as<JsonObject>();
    const AutoTuning::DeliveryState currentDelivery = deliveryState(root);
    const std::uint32_t currentRevision = root["artifact_revision"] | (root["payload_revision"] | 1U);
    const std::uint32_t submittedRevision = root["active_record_revision"] | 0U;
    if (currentDelivery.terminal() || acknowledgementTimestamp < EpochTime::MIN_VALID ||
        recordRevision == 0 || recordRevision != currentRevision || recordRevision != submittedRevision) {
        return;
    }
    const EpochSeconds now = nowEpoch();
    if (outcome == "accepted" || outcome == "already_processed") {
        if (!currentDelivery.canTransitionTo(AutoTuning::DeliveryStatus::Accepted)) {
            return;
        }
        if (preferenceRequest.has_value()) {
            auto artifactStorage = makePsramUnique<AutoTuning::CompletedShotArtifact>();
            auto &artifact = *artifactStorage;
            if (!loadCommittedShot(shotId, artifact)) {
                ESP_LOGW(LOG_TAG, "Cannot attach comparison request to unavailable shot %s",
                         shotId.c_str());
                return;
            }
            artifact.completion.preferenceRequest = preferenceRequest;
            if (artifactStore.exists(shotId)) {
                artifact.revision += 1;
                artifact.bindSamples();
                if (!artifactStore.write(artifact)) {
                    ESP_LOGW(LOG_TAG, "Cannot commit comparison request for shot %s",
                             shotId.c_str());
                    return;
                }
                updateReplayArtifactIdentity(root, artifact);
            } else {
                JsonDocument updatedCompletion(&psramAllocator);
                if (!AutoTuningJsonCodec::writeShotCompletion(artifact.completion,
                                                               updatedCompletion)) {
                    return;
                }
                root["completion"].set(updatedCompletion.as<JsonObjectConst>());
            }
        }
        root["local_delivery_state"] = "accepted";
        root["local_delivery_outcome"] = outcome;
        root["local_acknowledged_at"] = now;
        root["local_next_retry_at"] = 0;
        root["local_last_error"] = nullptr;
        root["updated_at"] = now;
        transitionPrompt(
            root,
            preferenceRequest.has_value()
                ? AutoTuning::PromptStatus::ComparisonAvailable
                : AutoTuning::PromptStatus::Resolved,
            now);
        if (LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
            prepareShotComplete(shotId, envelope);
        }
        deliveryWorkPending = true;
        nextDeliveryCheckAt = 0;
    } else if (outcome == "transient_failure") {
        if (!currentDelivery.canTransitionTo(AutoTuning::DeliveryStatus::RetryWait)) {
            return;
        }
        const int attempts = jsonNumber(root["local_attempt_count"]) ? root["local_attempt_count"].as<int>()
                                                                     : std::max(root["dispatch_count"] | 0, 1);
        root["local_delivery_state"] = "retry_wait";
        root["local_next_retry_at"] = now + deliveryRetryDelaySeconds(attempts);
        root["local_last_error"] = reason.isEmpty() ? String("ingest_unavailable") : reason;
        root["updated_at"] = now;
        transitionPrompt(root, AutoTuning::PromptStatus::DeliveryRetrying, now);
        LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
        deliveryWorkPending = true;
        nextDeliveryCheckAt = jsonEpochOrZero(root["local_next_retry_at"]);
    } else if (outcome == "permanent_rejection") {
        if (!currentDelivery.canTransitionTo(AutoTuning::DeliveryStatus::PermanentRejection)) {
            return;
        }
        root["local_delivery_state"] = "permanent_rejection";
        root["local_next_retry_at"] = 0;
        root["local_last_error"] = reason.isEmpty() ? String("permanent_rejection") : reason;
        root["updated_at"] = now;
        transitionPrompt(root, AutoTuning::PromptStatus::DeliveryError, now);
        LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
        ESP_LOGW(LOG_TAG, "Shot %s was permanently rejected: %s", shotId.c_str(),
                 (reason.isEmpty() ? String("permanent_rejection") : reason).c_str());
        deliveryWorkPending = true;
        nextDeliveryCheckAt = 0;
    } else {
        return;
    }
    refreshCachedStatus();
}

bool LocalAutoTuningStorePlugin::prepareShotComplete(const String &shotId, JsonDocument &envelope) {
    JsonObject root = envelope.as<JsonObject>();
    const int replaySchemaVersion = root["schema_version"] | 1;
    const AutoTuning::PromptState prompt = promptState(root);
    const bool durablePromptAvailable =
        replaySchemaVersion >= 3 &&
        prompt.status == AutoTuning::PromptStatus::ComparisonAvailable;
    if (deliveryState(root).status != AutoTuning::DeliveryStatus::Accepted ||
        (!durablePromptAvailable && (root["completion_emitted"] | false))) {
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(workMutex);
        if (std::find(queuedCompletionShotIds.begin(), queuedCompletionShotIds.end(), shotId) !=
                queuedCompletionShotIds.end() ||
            std::any_of(
                claimablePrompts.begin(), claimablePrompts.end(),
                [&shotId, &prompt](ClaimablePrompt const &candidate) {
                    return candidate.shotId == shotId &&
                           candidate.revision == prompt.revision;
                })) {
            return false;
        }
    }
    auto artifactStorage = makePsramUnique<AutoTuning::CompletedShotArtifact>();
    auto &artifact = *artifactStorage;
    if (!loadCommittedShot(shotId, artifact)) {
        root["completion_emitted"] = true;
        root["updated_at"] = nowEpoch();
        LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
        return false;
    }
    AutoTuning::ShotCompletion completionRecord = artifact.completion;
    const bool doseUsable = artifact.record.hasUsableDose();
    if (!doseUsable) {
        completionRecord.preferenceRequest.reset();
    }
    if (durablePromptAvailable && !completionRecord.preferenceRequest.has_value()) {
        const EpochSeconds now = nowEpoch();
        transitionPrompt(root, AutoTuning::PromptStatus::Resolved, now);
        root["updated_at"] = now;
        LocalAutoTuningFiles::writeJson(
            LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(workMutex);
        queuedCompletionShotIds.push_back(shotId);
        completionNotices.push_back(
            CompletionNotice{shotId, std::move(completionRecord),
                             durablePromptAvailable ? prompt.revision : 0});
    }
    return true;
}

void LocalAutoTuningStorePlugin::queuePromptEvent(Event event) {
    std::lock_guard<std::mutex> guard(workMutex);
    promptEvents.push_back(std::move(event));
}

void LocalAutoTuningStorePlugin::drainPromptEvents() {
    for (;;) {
        Event event;
        {
            std::lock_guard<std::mutex> guard(workMutex);
            if (promptEvents.empty()) return;
            event = std::move(promptEvents.front());
            promptEvents.pop_front();
        }
        pluginManager->trigger(event);
    }
}

void LocalAutoTuningStorePlugin::drainStoredShots() {
    for (;;) {
        StoredShotNotice notice;
        {
            std::lock_guard<std::mutex> guard(workMutex);
            if (storedShotNotices.empty()) {
                return;
            }
            notice = std::move(storedShotNotices.front());
            storedShotNotices.pop_front();
        }

        if (notice.doseConfirmationRequired) {
            if (notice.promptRevision > 0) {
                std::lock_guard<std::mutex> guard(workMutex);
                const bool alreadyClaimable = std::any_of(
                    claimablePrompts.begin(), claimablePrompts.end(),
                    [&notice](ClaimablePrompt const &prompt) {
                        return prompt.shotId == notice.shotId &&
                               prompt.revision == notice.promptRevision;
                    });
                if (!alreadyClaimable) {
                    claimablePrompts.push_back(
                        ClaimablePrompt{notice.shotId, notice.promptRevision, false});
                }
            }
            Event confirmationEvent;
            confirmationEvent.id = "rl:dose-confirmation:required";
            confirmationEvent.setString("shot_id", notice.shotId);
            notice.recipe.writeTo(confirmationEvent);
            confirmationEvent.setInt64("prompt_revision", notice.promptRevision);
            pluginManager->trigger(confirmationEvent);
        } else {
            Event dispatchEvent;
            dispatchEvent.id = "rl:shot:dispatch";
            dispatchEvent.setString("shot_id", notice.shotId);
            pluginManager->trigger(dispatchEvent);
        }
    }
}

void LocalAutoTuningStorePlugin::drainShotCompletions() {
    for (;;) {
        CompletionNotice notice;
        {
            std::lock_guard<std::mutex> guard(workMutex);
            if (completionNotices.empty()) {
                return;
            }
            notice = std::move(completionNotices.front());
            completionNotices.pop_front();
            queuedCompletionShotIds.erase(
                std::remove(queuedCompletionShotIds.begin(),
                            queuedCompletionShotIds.end(), notice.shotId),
                queuedCompletionShotIds.end());
            if (notice.promptRevision > 0) {
                claimablePrompts.push_back(
                    ClaimablePrompt{notice.shotId, notice.promptRevision, false});
            }
        }

        Event completeEvent;
        completeEvent.id = "rl:shot:complete";
        completeEvent.setString("shot_id", notice.shotId);
        completeEvent.setInt("preference_feedback_required",
                             notice.completion.preferenceRequest.has_value() ? 1 : 0);
        completeEvent.setInt64("prompt_revision", notice.promptRevision);
        completeEvent.setPayload(std::move(notice.completion));
        pluginManager->trigger(completeEvent);

        if (notice.promptRevision == 0) {
            WorkItem work;
            work.kind = WorkKind::MarkCompletionEmitted;
            work.shotId = notice.shotId;
            enqueueWork(std::move(work));
        }
    }
}

bool LocalAutoTuningStorePlugin::markShotCompletionEmitted(const String &shotId) {
    LocalStoreLock lock(storeMutex);
    bool persisted = false;
    JsonDocument envelope(&psramAllocator);
    if (loadReplaySnapshot(shotId, envelope)) {
        JsonObject root = envelope.as<JsonObject>();
        if ((root["completion_emitted"] | false) ||
            deliveryState(root).status != AutoTuning::DeliveryStatus::Accepted) {
            persisted = true;
        } else {
            root["completion_emitted"] = true;
            root["updated_at"] = nowEpoch();
            persisted = LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
        }
    }
    {
        std::lock_guard<std::mutex> guard(workMutex);
        queuedCompletionShotIds.erase(
            std::remove(queuedCompletionShotIds.begin(), queuedCompletionShotIds.end(), shotId),
            queuedCompletionShotIds.end());
    }
    if (!persisted) {
        deliveryWorkPending.store(true, std::memory_order_release);
        nextDeliveryCheckAt.store(0, std::memory_order_release);
    }
    refreshCachedStatus();
    return persisted;
}

bool LocalAutoTuningStorePlugin::enqueueWork(WorkItem work) {
#if !defined(GAGGIMATE_SIM)
    if (workerStartAttempted.load(std::memory_order_acquire) && workerTaskHandle == nullptr) {
        return false;
    }
#endif
    if (work.kind == WorkKind::StoreShot && (!work.queuedShot || work.shotId.isEmpty())) {
        return false;
    }
    if (work.kind == WorkKind::StoreRecommendation && !work.recommendation) {
        return false;
    }
    if (work.kind == WorkKind::PatchShotCorrection &&
        (!work.correction || work.correction->shotId.empty())) {
        return false;
    }
    if (work.kind == WorkKind::PatchRecommendationStatus &&
        (work.recommendationId.isEmpty() || work.recommendationStatus.isEmpty() ||
         work.recommendationTimestampField.isEmpty())) {
        return false;
    }
    if ((work.kind == WorkKind::Reprocess || work.kind == WorkKind::Dispatch ||
         work.kind == WorkKind::DeliveryAcknowledgement ||
         work.kind == WorkKind::MarkCompletionEmitted) &&
        work.shotId.isEmpty()) {
        return false;
    }
    if (work.kind == WorkKind::DoseConfirmation && !work.recipeConfirmation) {
        return false;
    }
    if ((work.kind == WorkKind::ResolvePrompt || work.kind == WorkKind::DoseConfirmation) &&
        (work.shotId.isEmpty() || work.promptRevision == 0)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(workMutex);
        if (work.kind == WorkKind::StoreShot) {
            const std::size_t queuedShots =
                std::count_if(workItems.begin(), workItems.end(),
                              [](WorkItem const &queued) { return queued.kind == WorkKind::StoreShot; });
            if (queuedShots >= MAX_PENDING_SHOT_WRITES) {
                return false;
            }
        }
        const bool coalescible =
            work.kind == WorkKind::DeliverySweep || work.kind == WorkKind::CommunitySweep ||
            work.kind == WorkKind::DoseConfirmationRecovery ||
            work.kind == WorkKind::StatusRefresh ||
            work.kind == WorkKind::SnapshotContexts ||
            work.kind == WorkKind::RecoverCommittedArtifacts;
        if (coalescible &&
            std::any_of(workItems.begin(), workItems.end(),
                        [&work](WorkItem const &queued) { return queued.kind == work.kind; })) {
            return true;
        }
        workItems.push_back(std::move(work));
    }
    if (workerTaskHandle) {
#if !defined(GAGGIMATE_SIM)
        xTaskNotifyGive(workerTaskHandle);
#endif
    }
    return true;
}

bool LocalAutoTuningStorePlugin::processOneWorkItem() {
    WorkItem work;
    {
        std::lock_guard<std::mutex> guard(workMutex);
        if (workItems.empty()) {
            return false;
        }
        work = std::move(workItems.front());
        workItems.pop_front();
    }
    switch (work.kind) {
    case WorkKind::StoreShot:
        if (!work.queuedShot) {
            break;
        }
        if (!persistShot(work.queuedShot->shot, work.queuedShot->completion,
                         work.queuedShot->disposition,
                         work.queuedShot->committedAt)) {
            ESP_LOGE(LOG_TAG, "Failed to persist shot %s; retrying", work.shotId.c_str());
            vTaskDelay(pdMS_TO_TICKS(1000));
            std::lock_guard<std::mutex> guard(workMutex);
            workItems.push_back(std::move(work));
            break;
        }
        {
            StoredShotNotice notice;
            notice.shotId = work.shotId;
            notice.doseConfirmationRequired = work.queuedShot->disposition.doseConfirmationRequired;
            notice.recipe = RecipePrompt::fromShot(work.queuedShot->shot,
                                                   work.queuedShot->completion.doseTargetG);
            notice.promptRevision = notice.doseConfirmationRequired ? 1 : 0;
            std::lock_guard<std::mutex> guard(workMutex);
            storedShotNotices.push_back(std::move(notice));
        }
        break;
    case WorkKind::Reprocess:
        if (!prepareShotReprocess(work.shotId)) {
            ESP_LOGW(LOG_TAG, "Unable to prepare shot %s for reprocessing", work.shotId.c_str());
        }
        break;
    case WorkKind::Dispatch:
        dispatchStoredShot(work.shotId, work.reprocess, work.automaticRetry);
        break;
    case WorkKind::DeliveryAcknowledgement:
        processShotDeliveryAck(work.shotId, work.outcome, work.reason, work.timestamp,
                               work.recordRevision, work.preferenceRequest);
        break;
    case WorkKind::DeliverySweep:
        processDueDelivery();
        break;
    case WorkKind::CommunitySweep:
        dispatchPendingCommunityUploads();
        break;
    case WorkKind::DoseConfirmation:
        if (work.recipeConfirmation) {
            processDoseConfirmation(work.shotId, work.promptRevision, *work.recipeConfirmation);
        }
        break;
    case WorkKind::DoseConfirmationRecovery:
        recoverPendingDoseConfirmation();
        break;
    case WorkKind::StatusRefresh:
        refreshCachedStatus();
        break;
    case WorkKind::MarkCompletionEmitted:
        markShotCompletionEmitted(work.shotId);
        break;
    case WorkKind::RecoverCommittedArtifacts:
        recoverCommittedArtifacts();
        break;
    case WorkKind::StoreRecommendation:
        if (work.recommendation) {
            persistRecommendation(*work.recommendation);
        }
        break;
    case WorkKind::SnapshotContexts:
        persistContexts();
        break;
    case WorkKind::PatchShotCorrection:
        if (work.correction) {
            LocalStoreLock lock(storeMutex);
            if (summaryStore.patchShotCorrection(work.correction->shotId.c_str(),
                                                 *work.correction)) {
                requestStatusRefresh();
            }
        }
        break;
    case WorkKind::PatchRecommendationStatus: {
        LocalStoreLock lock(storeMutex);
        if (summaryStore.patchRecommendationStatus(
                work.recommendationId, work.recommendationStatus.c_str(),
                work.recommendationTimestampField.c_str())) {
            requestStatusRefresh();
        }
        break;
    }
    case WorkKind::ResolvePrompt:
        if (!resolvePrompt(work.shotId, work.promptRevision)) {
            Event release;
            release.id = "rl:prompt:release";
            release.setString("shot_id", work.shotId);
            release.setInt64("prompt_revision", work.promptRevision);
            pluginManager->trigger(release);
        }
        break;
    }
    return true;
}

void LocalAutoTuningStorePlugin::workerTask(void *arg) {
#if defined(GAGGIMATE_SIM)
    (void)arg;
#else
    auto *plugin = static_cast<LocalAutoTuningStorePlugin *>(arg);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (plugin->processOneWorkItem()) {
        }
    }
#endif
}

void LocalAutoTuningStorePlugin::processDueDelivery() {
    const EpochSeconds now = nowEpoch();
    const EpochSeconds scheduledRetry = nextDeliveryCheckAt.load(std::memory_order_acquire);
    if (now < EpochTime::MIN_VALID || !deliveryWorkPending.load(std::memory_order_acquire) ||
        (scheduledRetry > 0 && now < scheduledRetry) || !controller || !pluginManager) {
        return;
    }
    String dueShotId;
    bool completionDue = false;
    {
        LocalStoreLock lock(storeMutex);
        std::vector<String> paths;
        if (!LocalAutoTuningFiles::listRecordPaths(REPLAY_DIR, paths)) {
            return;
        }
        EpochSeconds oldest = std::numeric_limits<EpochSeconds>::max();
        EpochSeconds earliestFutureRetry = std::numeric_limits<EpochSeconds>::max();
        bool activeDeliveryFound = false;
        for (const String &path : paths) {
            JsonDocument envelope(&psramAllocator);
            if (LocalAutoTuningFiles::readJson(path, envelope)) {
                JsonObjectConst replay = envelope.as<JsonObjectConst>();
                const AutoTuning::DeliveryStatus state = deliveryState(replay).status;
                const EpochSeconds updatedAt = jsonEpochOrZero(replay["updated_at"]);
                const bool durablePromptAvailable =
                    (replay["schema_version"] | 1) >= 3 &&
                    promptState(replay).status ==
                        AutoTuning::PromptStatus::ComparisonAvailable;
                bool promptAlreadyAnnounced = false;
                if (durablePromptAvailable) {
                    const String promptShotId = replay["shot_id"].as<String>();
                    const std::uint32_t revision = promptState(replay).revision;
                    std::lock_guard<std::mutex> guard(workMutex);
                    promptAlreadyAnnounced =
                        std::find(queuedCompletionShotIds.begin(),
                                  queuedCompletionShotIds.end(),
                                  promptShotId) != queuedCompletionShotIds.end() ||
                        std::any_of(
                            claimablePrompts.begin(), claimablePrompts.end(),
                            [&promptShotId, revision](
                                ClaimablePrompt const &candidate) {
                                return candidate.shotId == promptShotId &&
                                       candidate.revision == revision;
                            });
                }
                const bool legacyCompletionAvailable =
                    (replay["schema_version"] | 1) < 3 &&
                    !(replay["completion_emitted"] | false);
                if (state == AutoTuning::DeliveryStatus::Accepted &&
                    ((durablePromptAvailable && !promptAlreadyAnnounced) ||
                     legacyCompletionAvailable)) {
                    dueShotId = replay["shot_id"].as<String>();
                    completionDue = true;
                    break;
                }
                const EpochSeconds nextRetryAt =
                    jsonEpochOrZero(replay["local_next_retry_at"]);
                if (state == AutoTuning::DeliveryStatus::Pending ||
                    state == AutoTuning::DeliveryStatus::RetryWait ||
                    state == AutoTuning::DeliveryStatus::AwaitingAcknowledgement) {
                    activeDeliveryFound = true;
                    if (nextRetryAt > now) {
                        earliestFutureRetry =
                            std::min(earliestFutureRetry, nextRetryAt);
                    }
                }
                const bool due =
                    (state == AutoTuning::DeliveryStatus::Pending ||
                     state == AutoTuning::DeliveryStatus::RetryWait ||
                     state == AutoTuning::DeliveryStatus::AwaitingAcknowledgement) &&
                    (nextRetryAt <= 0 || now >= nextRetryAt);
                if (due && updatedAt <= oldest) {
                    dueShotId = replay["shot_id"].as<String>();
                    oldest = updatedAt;
                }
            }
        }
        deliveryWorkPending = activeDeliveryFound || completionDue;
        nextDeliveryCheckAt =
            earliestFutureRetry == std::numeric_limits<EpochSeconds>::max()
                ? 0
                : earliestFutureRetry;
    }
    if (dueShotId.isEmpty()) {
        return;
    }
    if (completionDue) {
        JsonDocument envelope(&psramAllocator);
        if (loadReplaySnapshot(dueShotId, envelope)) {
            prepareShotComplete(dueShotId, envelope);
        }
        return;
    }
    if (!AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(),
                            controller->getOptimizerTransport())
             .routeOffBoardTransport()) {
        return;
    }
    dispatchStoredShot(dueShotId, true, true);
}

void LocalAutoTuningStorePlugin::handleShotCorrection(Event const &event) {
    AutoTuning::ShotCorrection const *correction = event.getPayload<AutoTuning::ShotCorrection>();
    if (!correction || correction->shotId.empty()) {
        return;
    }
    WorkItem work;
    work.kind = WorkKind::PatchShotCorrection;
    work.correction = std::make_unique<AutoTuning::ShotCorrection>(*correction);
    enqueueWork(std::move(work));
}

void LocalAutoTuningStorePlugin::handleRecommendationApply(Event const &event) {
    if (event.getInt("decision_persisted") != 1) {
        return;
    }
    const String recommendationId = event.getString("recommendation_id");
    if (!recommendationId.isEmpty()) {
        WorkItem work;
        work.kind = WorkKind::PatchRecommendationStatus;
        work.recommendationId = recommendationId;
        work.recommendationStatus = "accepted";
        work.recommendationTimestampField = "accepted_at";
        enqueueWork(std::move(work));
    }
}

void LocalAutoTuningStorePlugin::handleRecommendationIgnore(Event const &event) {
    if (event.getInt("decision_persisted") != 1) {
        return;
    }
    const String recommendationId = event.getString("recommendation_id");
    if (!recommendationId.isEmpty()) {
        WorkItem work;
        work.kind = WorkKind::PatchRecommendationStatus;
        work.recommendationId = recommendationId;
        work.recommendationStatus = "ignored";
        work.recommendationTimestampField = "ignored_at";
        enqueueWork(std::move(work));
    }
}

void LocalAutoTuningStorePlugin::handlePromptClaim(Event &event) {
    const String shotId = event.getString("shot_id");
    const std::uint32_t revision =
        static_cast<std::uint32_t>(std::max<std::int64_t>(
            event.getInt64("prompt_revision"), 0));
    if (shotId.isEmpty() || revision == 0) {
        return;
    }
    std::lock_guard<std::mutex> guard(workMutex);
    auto prompt = std::find_if(
        claimablePrompts.begin(), claimablePrompts.end(),
        [&shotId, revision](ClaimablePrompt const &candidate) {
            return candidate.shotId == shotId && candidate.revision == revision;
        });
    if (prompt == claimablePrompts.end()) {
        return;
    }
    if (event.getInt("release") == 1) {
        prompt->claimed = false;
        event.setInt("released", 1);
    } else if (!prompt->claimed) {
        prompt->claimed = true;
        event.setInt("claimed", 1);
    }
}

void LocalAutoTuningStorePlugin::handlePreferencePersisted(Event const &event) {
    if (event.getInt("decision_persisted") != 1 ||
        event.getInt("prompt_claimed") != 1) {
        return;
    }
    WorkItem work;
    work.kind = WorkKind::ResolvePrompt;
    work.shotId = event.getString("new_shot_id");
    work.promptRevision = static_cast<std::uint32_t>(
        std::max<std::int64_t>(event.getInt64("prompt_revision"), 0));
    enqueueWork(std::move(work));
}

bool LocalAutoTuningStorePlugin::resolvePrompt(const String &shotId,
                                               const std::uint32_t revision) {
    LocalStoreLock lock(storeMutex);
    JsonDocument replay(&psramAllocator);
    if (shotId.isEmpty() || revision == 0 || !loadReplaySnapshot(shotId, replay)) {
        return false;
    }
    JsonObject root = replay.as<JsonObject>();
    const AutoTuning::PromptState prompt = promptState(root);
    if (prompt.revision != revision ||
        (prompt.status != AutoTuning::PromptStatus::ComparisonAvailable &&
         prompt.status != AutoTuning::PromptStatus::AwaitingComparison)) {
        return false;
    }
    const EpochSeconds now = nowEpoch();
    transitionPrompt(root, AutoTuning::PromptStatus::Resolved, now);
    root["updated_at"] = now;
    if (!LocalAutoTuningFiles::writeJson(
            LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), replay)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> guard(workMutex);
        claimablePrompts.erase(
            std::remove_if(
                claimablePrompts.begin(), claimablePrompts.end(),
                [&shotId](ClaimablePrompt const &prompt) {
                    return prompt.shotId == shotId;
                }),
            claimablePrompts.end());
    }
    requestStatusRefresh();
    return true;
}

void LocalAutoTuningStorePlugin::snapshotContexts() {
    WorkItem work;
    work.kind = WorkKind::SnapshotContexts;
    enqueueWork(std::move(work));
}

void LocalAutoTuningStorePlugin::persistContexts() {
    LocalStoreLock lock(storeMutex);
    if (!controller || !ensureDirectories()) {
        return;
    }
    contextStore.save(controller->getSettings());
    requestStatusRefresh();
}

void LocalAutoTuningStorePlugin::prune() {
    if (!ensureDirectories()) {
        return;
    }
    summaryStore.prune();
    pruneReplaySnapshots();
}

void LocalAutoTuningStorePlugin::pruneReplaySnapshots() {
    while (true) {
        const LocalAutoTuningFiles::DirectoryStats stats = LocalAutoTuningFiles::directoryStats(REPLAY_DIR);
        if (stats.count <= MAX_REPLAY_SNAPSHOTS && stats.bytes <= MAX_REPLAY_BYTES) {
            break;
        }
        // Active deliveries are durable. Retention removes only terminal
        // snapshots; an outage is surfaced instead of silently losing a shot.
        String replayPath;
        String shotId;
        if (!findOldestTerminalReplay(replayPath, shotId)) {
            break;
        }
        auto lease = StorageCoordinator::instance().acquireFlash();
        if (!LittleFS.remove(replayPath)) {
            break;
        }
        lease.reset();
        artifactStore.remove(shotId);
    }
}

void LocalAutoTuningStorePlugin::publishStatus() {
    if (!pluginManager) {
        return;
    }
    lastStatusMs = millis();
    AutoTuning::LocalStoreStats current;
    int pending = 0;
    int retrying = 0;
    int rejected = 0;
    String lastError;
    {
        std::lock_guard<std::mutex> guard(statusMutex);
        current = cachedStats;
        pending = deliveryPendingCount;
        retrying = deliveryRetryCount;
        rejected = deliveryRejectedCount;
        lastError = deliveryLastError;
    }
    Event event;
    event.id = "rl:local_store:status";
    event.setInt("available", current.available ? 1 : 0);
    event.setInt("shot_count", static_cast<int>(current.shotCount));
    event.setInt("recommendation_count", static_cast<int>(current.recommendationCount));
    event.setInt("bytes", static_cast<int>(current.bytes));
    event.setInt("delivery_pending_count", pending);
    event.setInt("delivery_retry_count", retrying);
    event.setInt("delivery_rejected_count", rejected);
    event.setString("delivery_last_error", lastError);
    event.setString("summary", current.available ? "Local Auto-Tuning store ready" : "Local Auto-Tuning store unavailable");
    pluginManager->trigger(event);
}

void LocalAutoTuningStorePlugin::requestStatusRefresh() {
    WorkItem work;
    work.kind = WorkKind::StatusRefresh;
    enqueueWork(std::move(work));
}

void LocalAutoTuningStorePlugin::refreshCachedStatus() {
    LocalStoreLock lock(storeMutex);
    AutoTuning::LocalStoreStats current;
    current.available = LittleFSUtil::existsQuietly(STORE_DIR);
    const LocalAutoTuningSummaryStore::Stats summaries = summaryStore.stats();
    const LocalAutoTuningFiles::DirectoryStats replays = LocalAutoTuningFiles::directoryStats(REPLAY_DIR);
    current.shotCount = summaries.shotCount;
    current.recommendationCount = summaries.recommendationCount;
    current.bytes = summaries.bytes + replays.bytes + contextStore.bytes() + artifactStore.bytes();
    const DeliveryStats delivery = localDeliveryStats();
    {
        std::lock_guard<std::mutex> guard(statusMutex);
        cachedStats = current;
        deliveryPendingCount = delivery.pending;
        deliveryRetryCount = delivery.retrying;
        deliveryRejectedCount = delivery.rejected;
        deliveryLastError = delivery.lastError;
    }
    statusPublishRequested.store(true, std::memory_order_release);
}

void LocalAutoTuningStorePlugin::refreshDeliveryStatus() {
    refreshCachedStatus();
}

void LocalAutoTuningStorePlugin::recoverPendingDoseConfirmation() {
    LocalStoreLock lock(storeMutex);
    std::vector<String> paths;
    if (!LocalAutoTuningFiles::listRecordPaths(REPLAY_DIR, paths)) {
        return;
    }
    String newestShotId;
    std::uint32_t newestPromptRevision = 0;
    EpochSeconds newestTimestamp = std::numeric_limits<EpochSeconds>::min();
    for (const String &path : paths) {
        JsonDocument envelope(&psramAllocator);
        if (LocalAutoTuningFiles::readJson(path, envelope)) {
            JsonObjectConst replay = envelope.as<JsonObjectConst>();
            const EpochSeconds updatedAt = jsonEpochOrZero(replay["updated_at"]);
            if (deliveryState(replay).status == AutoTuning::DeliveryStatus::AwaitingDoseConfirmation &&
                updatedAt >= newestTimestamp) {
                newestShotId = replay["shot_id"].as<String>();
                newestPromptRevision = promptState(replay).revision;
                newestTimestamp = updatedAt;
            }
        }
    }
    if (newestShotId.isEmpty()) {
        return;
    }
    StoredShotNotice notice;
    notice.shotId = newestShotId;
    notice.doseConfirmationRequired = true;
    auto artifact = makePsramUnique<AutoTuning::CompletedShotArtifact>();
    if (!loadCommittedShot(newestShotId, *artifact)) return;
    notice.recipe = RecipePrompt::fromShot(artifact->record, artifact->completion.doseTargetG);
    notice.promptRevision = newestPromptRevision;
    std::lock_guard<std::mutex> guard(workMutex);
    storedShotNotices.push_back(std::move(notice));
}
