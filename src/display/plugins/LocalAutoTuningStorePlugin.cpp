#include "LocalAutoTuningStorePlugin.h"
#include "autotuning/AutoTuningJsonCodec.h"
#include "autotuning/local/LocalAutoTuningFiles.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <algorithm>
#include <cstdint>
#include <display/core/AutoTuning.h>
#include <display/core/Controller.h>
#include <display/core/EpochTime.h>
#include <display/core/PluginManager.h>
#include <display/core/Settings.h>
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

static EpochSeconds nowEpoch() { return EpochTime::now(); }

static bool jsonNumber(JsonVariantConst value) {
    return value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>();
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
    String state = replay["local_delivery_state"].as<String>();
    if (!state.isEmpty()) {
        const auto parsed = AutoTuning::deliveryStatusFromKey(state.c_str());
        delivery.status = parsed.value_or(AutoTuning::DeliveryStatus::Pending);
    } else {
        const String legacy = replay["dispatch_state"].as<String>();
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
    delivery.lastError = replay["local_last_error"].as<String>().c_str();
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
    File root = LittleFS.open(REPLAY_DIR);
    if (!root || !root.isDirectory()) {
        return stats;
    }
    File file = root.openNextFile();
    while (file) {
        const String path = file.name();
        const bool candidate = !file.isDirectory() && path.endsWith(".json");
        file.close();
        if (candidate) {
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
                const String error = replay["local_last_error"].as<String>();
                const EpochSeconds updatedAt = jsonEpochOrZero(replay["updated_at"]);
                if (!error.isEmpty() && updatedAt >= stats.lastErrorAt) {
                    stats.lastError = error;
                    stats.lastErrorAt = updatedAt;
                }
            }
        }
        file = root.openNextFile();
    }
    root.close();
    return stats;
}

static bool removeOldestTerminalReplay() {
    File root = LittleFS.open(REPLAY_DIR);
    if (!root || !root.isDirectory()) {
        return false;
    }
    bool found = false;
    String oldestPath;
    EpochSeconds oldestTimestamp = std::numeric_limits<EpochSeconds>::max();
    File file = root.openNextFile();
    while (file) {
        const String path = file.name();
        const bool candidate = !file.isDirectory() && path.endsWith(".json");
        file.close();
        if (candidate) {
            JsonDocument doc(&psramAllocator);
            if (LocalAutoTuningFiles::readJson(path, doc)) {
                JsonObjectConst replay = doc.as<JsonObjectConst>();
                const bool communityPending = (replay["community_required"] | false) && !(replay["community_dispatched"] | false);
                if (!deliveryTerminal(replay) || communityPending) {
                    file = root.openNextFile();
                    continue;
                }
                const EpochSeconds timestamp = LocalAutoTuningFiles::fileTimestamp(path);
                if (!found || timestamp < oldestTimestamp) {
                    found = true;
                    oldestPath = path;
                    oldestTimestamp = timestamp;
                }
            }
        }
        file = root.openNextFile();
    }
    root.close();
    return found && LittleFS.remove(oldestPath);
}

} // namespace

LocalAutoTuningStorePlugin LocalAutoTuningStore;

void LocalAutoTuningStorePlugin::setup(Controller *ctrl, PluginManager *pm) {
    controller = ctrl;
    pluginManager = pm;
    ctrl->setAutoTuningRecordStore(this);
    ensureDirectories();
    refreshDeliveryStatus();
    snapshotContexts();

    pm->on("settings:changed", [this](Event const &) { snapshotContexts(); });
    pm->on("rl:settings:changed", [this](Event const &) { snapshotContexts(); });
    pm->on("rl:status:refresh", [this](Event const &) { publishStatus(); });
    pm->on("rl:shot:dispatch", [this](Event const &event) { handleShotDispatch(event); });
    pm->on("rl:dose-confirmation", [this](Event const &event) { handleDoseConfirmation(event); });
    pm->on("rl:shot:reprocess", [this](Event const &event) { handleShotReprocess(event); });
    pm->on("rl:shot:delivery:ack", [this](Event const &event) { handleShotDeliveryAck(event); });
    pm->on("rl:local:shot:delete", [this](Event const &event) { removeShotData(event.getString("shot_id"), true); });
    pm->on("rl:shot:correction", [this](Event const &event) { handleShotCorrection(event); });
    pm->on("rl:community-upload:ready", [this](Event const &) { dispatchPendingCommunityUploads(); });
    pm->on("rl:recommendation:apply", [this](Event const &event) { handleRecommendationApply(event); });
    pm->on("rl:recommendation:ignore", [this](Event const &event) { handleRecommendationIgnore(event); });
    pm->on("rl:local:reset", [this](Event const &) {
        reset();
        snapshotContexts();
    });

    publishStatus();
}

bool LocalAutoTuningStorePlugin::storeShot(AutoTuning::ShotRecord const &shot, AutoTuning::ShotCompletion const &completion,
                                           AutoTuning::ShotCaptureDisposition const &disposition) {
    JsonDocument payload(&psramAllocator);
    JsonDocument completionPayload(&psramAllocator);
    if (!AutoTuningJsonCodec::writeShotRecord(shot, payload) ||
        !AutoTuningJsonCodec::writeShotCompletion(completion, completionPayload)) {
        return false;
    }
    const String shotId(shot.shotId.c_str());
    pendingDoseRecoveryChecked = true;
    const bool replaySaved = saveReplaySnapshot(shotId, payload.as<JsonVariantConst>(), completionPayload.as<JsonVariantConst>(),
                                                disposition.doseConfirmationRequired, disposition.optimizerDeliveryRequired,
                                                disposition.communityUploadRequired);
    if (!replaySaved) {
        ESP_LOGW(LOG_TAG, "Failed to persist replay snapshot for %s", shotId.c_str());
        refreshDeliveryStatus();
        publishStatus();
        return false;
    }
    const bool summarySaved = summaryStore.upsertShot(payload.as<JsonObjectConst>());
    if (!summarySaved) {
        ESP_LOGW(LOG_TAG, "Replay saved without compact summary for %s", shotId.c_str());
    }
    prune();
    if (!disposition.doseConfirmationRequired) {
        deliveryWorkPending = true;
        nextDeliveryCheckAt = 0;
    }
    refreshDeliveryStatus();
    publishStatus();
    return true;
}

bool LocalAutoTuningStorePlugin::storeRecommendation(AutoTuning::Recommendation const &recommendation) {
    JsonDocument payload(&psramAllocator);
    if (!AutoTuningJsonCodec::writeRecommendation(recommendation, payload) ||
        !summaryStore.upsertRecommendation(payload.as<JsonObjectConst>())) {
        return false;
    }
    prune();
    publishStatus();
    return true;
}

void LocalAutoTuningStorePlugin::loop() {
    if (!pendingDoseRecoveryChecked && millis() >= 1000) {
        pendingDoseRecoveryChecked = true;
        recoverPendingDoseConfirmation();
    }
    if (millis() - lastDeliverySweepMs >= DELIVERY_SWEEP_INTERVAL_MS) {
        lastDeliverySweepMs = millis();
        processDueDelivery();
        if (controller && controller->getSettings().isRLCommunityUploadEnabled() && nowEpoch() >= EpochTime::MIN_VALID) {
            dispatchPendingCommunityUploads();
        }
    }
    if (millis() - lastStatusMs >= STATUS_INTERVAL_MS) {
        publishStatus();
    }
}

bool LocalAutoTuningStorePlugin::ensureDirectories() const {
    const bool available = LocalAutoTuningFiles::ensureDirectory(STORE_DIR) && summaryStore.begin() && contextStore.begin() &&
                           LocalAutoTuningFiles::ensureDirectory(REPLAY_DIR);
    if (available) {
        LocalAutoTuningFiles::recoverDirectory(REPLAY_DIR);
    }
    return available;
}

bool LocalAutoTuningStorePlugin::reset() {
    if (!ensureDirectories()) {
        return false;
    }
    const bool ok = summaryStore.reset() && LocalAutoTuningFiles::clearDirectory(REPLAY_DIR) && contextStore.clear();
    deliveryWorkPending = false;
    nextDeliveryCheckAt = 0;
    refreshDeliveryStatus();
    publishStatus();
    return ok;
}

AutoTuning::LocalStoreStats LocalAutoTuningStorePlugin::stats() const {
    AutoTuning::LocalStoreStats out;
    out.available = LittleFS.exists(STORE_DIR);
    const LocalAutoTuningSummaryStore::Stats summaries = summaryStore.stats();
    const LocalAutoTuningFiles::DirectoryStats replays = LocalAutoTuningFiles::directoryStats(REPLAY_DIR);
    out.shotCount = summaries.shotCount;
    out.recommendationCount = summaries.recommendationCount;
    out.bytes = summaries.bytes + replays.bytes + contextStore.bytes();
    return out;
}

bool LocalAutoTuningStorePlugin::loadShotSummary(const String &shotId, JsonDocument &out) const {
    return summaryStore.loadShot(shotId, out);
}

bool LocalAutoTuningStorePlugin::hasShotReplay(const String &shotId) const {
    return !shotId.isEmpty() && LittleFS.exists(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId));
}

bool LocalAutoTuningStorePlugin::canRemoveShotData(const String &shotId) const {
    if (shotId.isEmpty()) {
        return false;
    }
    const String replayPath = LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId);
    if (!LittleFS.exists(replayPath)) {
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
    if (shotId.isEmpty()) {
        return false;
    }
    if (!force && !canRemoveShotData(shotId)) {
        return false;
    }
    bool removed = false;
    const String replayPath = LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId);
    removed = summaryStore.removeShot(shotId) || removed;
    if (LittleFS.exists(replayPath)) {
        removed = LittleFS.remove(replayPath) || removed;
    }
    if (removed) {
        deliveryWorkPending = true;
        nextDeliveryCheckAt = 0;
        refreshDeliveryStatus();
        publishStatus();
    }
    return removed;
}

void LocalAutoTuningStorePlugin::handleShotDispatch(Event const &event) { dispatchStoredShot(event.getString("shot_id"), false); }

void LocalAutoTuningStorePlugin::handleDoseConfirmation(Event const &event) {
    const String shotId = event.getString("shot_id");
    if (shotId.isEmpty() || event.getInt("has_followed") != 1) {
        return;
    }
    JsonDocument envelope(&psramAllocator);
    if (!loadReplaySnapshot(shotId, envelope)) {
        return;
    }
    JsonObject root = envelope.as<JsonObject>();
    JsonObject payload = root["payload"].as<JsonObject>();
    if (payload.isNull()) {
        return;
    }
    const bool followed = event.getInt("followed") == 1;
    payload["dose_target_confirmed"] = followed;
    payload["dose_followed"] = followed;
    payload["dose_observed"] = payload["dose_observed"] | false;
    if (followed && payload["dose_in_g"].isNull() && jsonNumber(payload["dose_target_g"])) {
        payload["dose_in_g"] = payload["dose_target_g"];
    }
    root["dose_confirmation_status"] = followed ? "confirmed" : "not_followed";
    root["dispatch_state"] = "ready";
    root["local_delivery_state"] = "pending";
    root["local_next_retry_at"] = 0;
    root["updated_at"] = nowEpoch();
    if (!LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
        return;
    }

    summaryStore.upsertShot(payload);
    Event resolved;
    resolved.id = "rl:dose-confirmation:resolved";
    resolved.setString("shot_id", shotId);
    resolved.setInt("followed", followed ? 1 : 0);
    pluginManager->trigger(resolved);
    deliveryWorkPending = true;
    nextDeliveryCheckAt = 0;
    dispatchStoredShot(shotId, false);
    recoverPendingDoseConfirmation();
}

void LocalAutoTuningStorePlugin::handleShotReprocess(Event const &event) {
    const String shotId = event.getString("shot_id");
    JsonDocument envelope(&psramAllocator);
    if (!loadReplaySnapshot(shotId, envelope)) {
        return;
    }
    JsonObject root = envelope.as<JsonObject>();
    if (deliveryState(root).status == AutoTuning::DeliveryStatus::AwaitingDoseConfirmation) {
        Event confirmation;
        confirmation.id = "rl:dose-confirmation:required";
        confirmation.setString("shot_id", shotId);
        JsonObjectConst payload = root["payload"].as<JsonObjectConst>();
        confirmation.setFloat("dose_target_g", payload["dose_target_g"] | 0.0f);
        pluginManager->trigger(confirmation);
        return;
    }
    const bool localDeliveryRequired = controller && AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(),
                                                                        controller->getOptimizerTransport())
                                                         .optimizationActive();
    root["local_delivery_required"] = localDeliveryRequired;
    root["local_delivery_state"] = localDeliveryRequired ? "pending" : "not_required";
    root["local_next_retry_at"] = 0;
    root["local_last_error"] = nullptr;
    root["community_dispatched"] = false;
    root["community_required"] = controller && controller->getSettings().isRLCommunityUploadEnabled();
    root["completion_emitted"] = !localDeliveryRequired;
    root["updated_at"] = nowEpoch();
    if (!LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
        return;
    }
    deliveryWorkPending = true;
    nextDeliveryCheckAt = 0;
    dispatchStoredShot(shotId, true);
}

bool LocalAutoTuningStorePlugin::saveReplaySnapshot(const String &shotId, JsonVariantConst payload, JsonVariantConst completion,
                                                    bool doseConfirmationRequired, bool localDeliveryRequired,
                                                    bool communityUploadRequired) {
    if (!ensureDirectories() || shotId.isEmpty() || !payload.is<JsonObjectConst>()) {
        return false;
    }
    JsonDocument envelope(&psramAllocator);
    JsonObject root = envelope.to<JsonObject>();
    root["event_type"] = "local_shot_replay";
    root["schema_version"] = 2;
    root["shot_id"] = shotId;
    root["captured_at"] = nowEpoch();
    root["updated_at"] = nowEpoch();
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
    root["payload"].set(payload);
    if (completion.is<JsonObjectConst>()) {
        root["completion"].set(completion);
    }
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
    JsonDocument envelope(&psramAllocator);
    if (!loadReplaySnapshot(shotId, envelope)) {
        return false;
    }
    JsonObject root = envelope.as<JsonObject>();
    if (deliveryState(root).status == AutoTuning::DeliveryStatus::AwaitingDoseConfirmation) {
        return false;
    }
    const bool localDeliveryRequired =
        root["local_delivery_required"].isNull() ? true : root["local_delivery_required"].as<bool>();
    const int replaySchemaVersion = root["schema_version"] | 1;
    if (replaySchemaVersion < 2 && root["completion_emitted"].isNull()) {
        // Version 1 emitted completion immediately after dispatch. Preserve
        // that fact when recovering an old sent snapshot so its acknowledgement
        // cannot reopen a stale preference prompt.
        root["completion_emitted"] = root["dispatch_state"].as<String>() == "dispatched";
    }
    root["schema_version"] = 2;
    JsonObject payload = root["payload"].as<JsonObject>();
    if (payload.isNull() || payload["shot_id"].as<String>() != shotId) {
        return false;
    }
    EpochSeconds payloadTimestamp = 0;
    if (!jsonEpoch(payload["timestamp"], payloadTimestamp) || payloadTimestamp < EpochTime::MIN_VALID) {
        payload["timestamp"] = now;
        root["updated_at"] = now;
        if (!LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
            return false;
        }
    }
    AutoTuningJsonCodec::DecodedShotRecord decoded;
    String decodeError;
    if (!AutoTuningJsonCodec::parseShotRecord(payload, decoded, decodeError)) {
        ESP_LOGW(LOG_TAG, "Stored shot %s is invalid: %s", shotId.c_str(), decodeError.c_str());
        return false;
    }

    root["dispatch_state"] = "dispatched";
    root["updated_at"] = now;
    if (localDeliveryRequired) {
        const int previousAttempts =
            jsonNumber(root["local_attempt_count"]) ? root["local_attempt_count"].as<int>() : (root["dispatch_count"] | 0);
        const int attemptCount = previousAttempts + 1;
        root["local_delivery_state"] = "awaiting_ack";
        root["local_attempt_count"] = attemptCount;
        root["local_last_attempt_at"] = now;
        root["local_next_retry_at"] = now + deliveryRetryDelaySeconds(attemptCount);
        root["local_last_error"] = nullptr;
        if (!LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
            return false;
        }
        deliveryWorkPending = true;
        nextDeliveryCheckAt = jsonEpochOrZero(root["local_next_retry_at"]);

        AutoTuning::OptimizerTransportPort *transport = controller ? controller->getOptimizerTransport() : nullptr;
        if (transport) {
            transport->publishShot(decoded.record, reprocess);
        }
    } else {
        root["local_delivery_state"] = "not_required";
        root["local_next_retry_at"] = 0;
        root["local_last_error"] = nullptr;
    }

    const bool communityDispatched = root["community_dispatched"] | false;
    const bool communityRequired = root["community_required"] | false;
    if (!automaticRetry && communityRequired && !communityDispatched) {
        AutoTuning::CommunityUploadPort *upload = controller ? controller->getCommunityUpload() : nullptr;
        root["community_dispatched"] = upload && upload->enqueueShot(decoded.record);
    }
    root["dispatch_count"] = (root["dispatch_count"] | 0) + 1;
    LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
    refreshDeliveryStatus();
    publishStatus();
    return true;
}

void LocalAutoTuningStorePlugin::dispatchPendingCommunityUploads() {
    if (!pluginManager) {
        return;
    }
    File directory = LittleFS.open(REPLAY_DIR);
    if (!directory || !directory.isDirectory()) {
        return;
    }
    File file = directory.openNextFile();
    while (file) {
        const String path = file.name();
        const bool candidate = !file.isDirectory() && path.endsWith(".json");
        file.close();
        if (candidate) {
            JsonDocument envelope(&psramAllocator);
            if (LocalAutoTuningFiles::readJson(path, envelope)) {
                JsonObject root = envelope.as<JsonObject>();
                JsonObjectConst payload = root["payload"].as<JsonObjectConst>();
                if ((root["community_required"] | false) && !(root["community_dispatched"] | false) &&
                    deliveryState(root).status != AutoTuning::DeliveryStatus::AwaitingDoseConfirmation && !payload.isNull()) {
                    AutoTuningJsonCodec::DecodedShotRecord decoded;
                    String decodeError;
                    AutoTuning::CommunityUploadPort *upload = controller ? controller->getCommunityUpload() : nullptr;
                    if (upload && AutoTuningJsonCodec::parseShotRecord(payload, decoded, decodeError) &&
                        upload->enqueueShot(decoded.record)) {
                        root["community_dispatched"] = true;
                        root["updated_at"] = nowEpoch();
                        LocalAutoTuningFiles::writeJson(path, envelope);
                    }
                }
            }
        }
        file = directory.openNextFile();
    }
    directory.close();
}

void LocalAutoTuningStorePlugin::handleShotDeliveryAck(Event const &event) {
    const String shotId = event.getString("shot_id");
    const String outcome = event.getString("outcome");
    const String reason = event.getString("reason");
    JsonDocument envelope(&psramAllocator);
    if (shotId.isEmpty() || !loadReplaySnapshot(shotId, envelope)) {
        return;
    }
    JsonObject root = envelope.as<JsonObject>();
    const AutoTuning::DeliveryState currentDelivery = deliveryState(root);
    const EpochSeconds acknowledgementTimestamp = event.getInt64("timestamp");
    if (currentDelivery.terminal() || acknowledgementTimestamp < EpochTime::MIN_VALID ||
        (currentDelivery.lastAttemptAt.has_value() && acknowledgementTimestamp < *currentDelivery.lastAttemptAt)) {
        return;
    }
    const EpochSeconds now = nowEpoch();
    if (outcome == "accepted" || outcome == "already_processed") {
        if (!currentDelivery.canTransitionTo(AutoTuning::DeliveryStatus::Accepted)) {
            return;
        }
        root["local_delivery_state"] = "accepted";
        root["local_delivery_outcome"] = outcome;
        root["local_acknowledged_at"] = now;
        root["local_next_retry_at"] = 0;
        root["local_last_error"] = nullptr;
        root["updated_at"] = now;
        if (LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope)) {
            emitShotComplete(shotId, envelope);
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
        LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
        deliveryWorkPending = true;
        nextDeliveryCheckAt = 0;
    } else {
        return;
    }
    refreshDeliveryStatus();
    publishStatus();
}

bool LocalAutoTuningStorePlugin::emitShotComplete(const String &shotId, JsonDocument &envelope) {
    JsonObject root = envelope.as<JsonObject>();
    if ((root["completion_emitted"] | false) || deliveryState(root).status != AutoTuning::DeliveryStatus::Accepted) {
        return false;
    }
    JsonObjectConst completion = root["completion"].as<JsonObjectConst>();
    JsonObjectConst payload = root["payload"].as<JsonObjectConst>();
    if (completion.isNull() || payload.isNull()) {
        // Legacy or damaged snapshots may not contain prompt metadata. Their
        // accepted delivery is still terminal and must not cause a hot retry
        // scan forever.
        root["completion_emitted"] = true;
        root["updated_at"] = nowEpoch();
        LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
        return false;
    }
    AutoTuning::ShotCompletion completionRecord;
    String completionError;
    if (!AutoTuningJsonCodec::parseShotCompletion(completion, completionRecord, completionError)) {
        ESP_LOGW(LOG_TAG, "Stored completion for %s is invalid: %s", shotId.c_str(), completionError.c_str());
        root["completion_emitted"] = true;
        root["updated_at"] = nowEpoch();
        LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
        return false;
    }
    const bool doseUsable = payload["dose_observed"].as<bool>() || payload["dose_target_confirmed"].as<bool>();
    completionRecord.recommendation.preferenceFeedbackRequired =
        completionRecord.recommendation.preferenceFeedbackRequired && doseUsable;
    Event completeEvent;
    completeEvent.id = "rl:shot:complete";
    completeEvent.setString("shot_id", shotId);
    completeEvent.setInt("preference_feedback_required", completionRecord.recommendation.preferenceFeedbackRequired ? 1 : 0);
    completeEvent.setPayload(completionRecord);
    pluginManager->trigger(completeEvent);
    root["completion_emitted"] = true;
    root["updated_at"] = nowEpoch();
    return LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(REPLAY_DIR, shotId), envelope);
}

void LocalAutoTuningStorePlugin::processDueDelivery() {
    const EpochSeconds now = nowEpoch();
    if (now < EpochTime::MIN_VALID || !deliveryWorkPending || (nextDeliveryCheckAt > 0 && now < nextDeliveryCheckAt) ||
        !controller || !pluginManager ||
        !AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(), controller->getOptimizerTransport())
             .routeOffBoardTransport()) {
        return;
    }
    File root = LittleFS.open(REPLAY_DIR);
    if (!root || !root.isDirectory()) {
        return;
    }
    String dueShotId;
    bool completionDue = false;
    EpochSeconds oldest = std::numeric_limits<EpochSeconds>::max();
    EpochSeconds earliestFutureRetry = std::numeric_limits<EpochSeconds>::max();
    bool activeDeliveryFound = false;
    File file = root.openNextFile();
    while (file) {
        const String path = file.name();
        const bool candidate = !file.isDirectory() && path.endsWith(".json");
        file.close();
        if (candidate) {
            JsonDocument envelope(&psramAllocator);
            if (LocalAutoTuningFiles::readJson(path, envelope)) {
                JsonObjectConst replay = envelope.as<JsonObjectConst>();
                const AutoTuning::DeliveryStatus state = deliveryState(replay).status;
                const EpochSeconds updatedAt = jsonEpochOrZero(replay["updated_at"]);
                if (state == AutoTuning::DeliveryStatus::Accepted && !(replay["completion_emitted"] | false)) {
                    dueShotId = replay["shot_id"].as<String>();
                    completionDue = true;
                    break;
                }
                const EpochSeconds nextRetryAt = jsonEpochOrZero(replay["local_next_retry_at"]);
                if (state == AutoTuning::DeliveryStatus::Pending || state == AutoTuning::DeliveryStatus::RetryWait ||
                    state == AutoTuning::DeliveryStatus::AwaitingAcknowledgement) {
                    activeDeliveryFound = true;
                    if (nextRetryAt > now) {
                        earliestFutureRetry = std::min(earliestFutureRetry, nextRetryAt);
                    }
                }
                const bool due =
                    (state == AutoTuning::DeliveryStatus::Pending || state == AutoTuning::DeliveryStatus::RetryWait ||
                     state == AutoTuning::DeliveryStatus::AwaitingAcknowledgement) &&
                    (nextRetryAt <= 0 || now >= nextRetryAt);
                if (due && updatedAt <= oldest) {
                    dueShotId = replay["shot_id"].as<String>();
                    oldest = updatedAt;
                }
            }
        }
        file = root.openNextFile();
    }
    root.close();
    deliveryWorkPending = activeDeliveryFound || completionDue;
    nextDeliveryCheckAt = earliestFutureRetry == std::numeric_limits<EpochSeconds>::max() ? 0 : earliestFutureRetry;
    if (dueShotId.isEmpty()) {
        return;
    }
    if (completionDue) {
        JsonDocument envelope(&psramAllocator);
        if (loadReplaySnapshot(dueShotId, envelope)) {
            emitShotComplete(dueShotId, envelope);
        }
        return;
    }
    dispatchStoredShot(dueShotId, true, true);
}

void LocalAutoTuningStorePlugin::handleShotCorrection(Event const &event) {
    AutoTuning::ShotCorrection const *correction = event.getPayload<AutoTuning::ShotCorrection>();
    if (!correction || correction->shotId.empty()) {
        return;
    }
    if (summaryStore.patchShotCorrection(correction->shotId.c_str(), correction->grindFollowed.has_value(),
                                         correction->grindFollowed.value_or(false), correction->doseFollowed.has_value(),
                                         correction->doseFollowed.value_or(false), correction->yieldFollowed.has_value(),
                                         correction->yieldFollowed.value_or(false))) {
        publishStatus();
    }
}

void LocalAutoTuningStorePlugin::handleRecommendationApply(Event const &event) {
    if (event.getInt("decision_persisted") != 1) {
        return;
    }
    const String recommendationId = event.getString("recommendation_id");
    if (!recommendationId.isEmpty() && summaryStore.patchRecommendationStatus(recommendationId, "accepted", "accepted_at")) {
        publishStatus();
    }
}

void LocalAutoTuningStorePlugin::handleRecommendationIgnore(Event const &event) {
    if (event.getInt("decision_persisted") != 1) {
        return;
    }
    const String recommendationId = event.getString("recommendation_id");
    if (!recommendationId.isEmpty() && summaryStore.patchRecommendationStatus(recommendationId, "ignored", "ignored_at")) {
        publishStatus();
    }
}

void LocalAutoTuningStorePlugin::snapshotContexts() {
    if (!controller || !ensureDirectories()) {
        return;
    }
    contextStore.save(controller->getSettings());
    publishStatus();
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
        if (!removeOldestTerminalReplay()) {
            break;
        }
    }
}

void LocalAutoTuningStorePlugin::publishStatus() {
    if (!pluginManager) {
        return;
    }
    lastStatusMs = millis();
    const AutoTuning::LocalStoreStats current = stats();
    Event event;
    event.id = "rl:local_store:status";
    event.setInt("available", current.available ? 1 : 0);
    event.setInt("shot_count", static_cast<int>(current.shotCount));
    event.setInt("recommendation_count", static_cast<int>(current.recommendationCount));
    event.setInt("bytes", static_cast<int>(current.bytes));
    event.setInt("delivery_pending_count", deliveryPendingCount);
    event.setInt("delivery_retry_count", deliveryRetryCount);
    event.setInt("delivery_rejected_count", deliveryRejectedCount);
    event.setString("delivery_last_error", deliveryLastError);
    event.setString("summary", current.available ? "Local Auto-Tuning store ready" : "Local Auto-Tuning store unavailable");
    pluginManager->trigger(event);
}

void LocalAutoTuningStorePlugin::refreshDeliveryStatus() {
    const DeliveryStats delivery = localDeliveryStats();
    deliveryPendingCount = delivery.pending;
    deliveryRetryCount = delivery.retrying;
    deliveryRejectedCount = delivery.rejected;
    deliveryLastError = delivery.lastError;
}

void LocalAutoTuningStorePlugin::recoverPendingDoseConfirmation() {
    if (!pluginManager) {
        return;
    }
    File root = LittleFS.open(REPLAY_DIR);
    if (!root || !root.isDirectory()) {
        return;
    }
    String newestShotId;
    float newestDoseTargetG = 0.0f;
    EpochSeconds newestTimestamp = std::numeric_limits<EpochSeconds>::min();
    File file = root.openNextFile();
    while (file) {
        const String path = file.name();
        const bool candidate = !file.isDirectory() && path.endsWith(".json");
        file.close();
        if (candidate) {
            JsonDocument envelope(&psramAllocator);
            if (LocalAutoTuningFiles::readJson(path, envelope)) {
                JsonObjectConst replay = envelope.as<JsonObjectConst>();
                const EpochSeconds updatedAt = jsonEpochOrZero(replay["updated_at"]);
                if (deliveryState(replay).status == AutoTuning::DeliveryStatus::AwaitingDoseConfirmation &&
                    updatedAt >= newestTimestamp) {
                    JsonObjectConst payload = replay["payload"].as<JsonObjectConst>();
                    newestShotId = replay["shot_id"].as<String>();
                    newestDoseTargetG = payload["dose_target_g"] | 0.0f;
                    newestTimestamp = updatedAt;
                }
            }
        }
        file = root.openNextFile();
    }
    root.close();
    if (newestShotId.isEmpty() || newestDoseTargetG <= 0.0f) {
        return;
    }
    Event confirmation;
    confirmation.id = "rl:dose-confirmation:required";
    confirmation.setString("shot_id", newestShotId);
    confirmation.setFloat("dose_target_g", newestDoseTargetG);
    pluginManager->trigger(confirmation);
}
