#include "LocalAutoTuningSummaryStore.h"

#include "LocalAutoTuningFiles.h"

#include <LittleFS.h>
#include <display/core/AutoTuningModels.h>
#include <display/core/AutoTuningPorts.h>
#include <display/core/EpochTime.h>
#include <display/util/PsramAllocator.h>
#include <limits>

namespace {

constexpr const char *STORE_DIR = "/rll";
constexpr const char *SHOT_DIR = "/rll/s";
constexpr const char *RECOMMENDATION_DIR = "/rll/r";
constexpr AutoTuning::LocalStoreRetentionPolicy RETENTION{};

using EpochSeconds = EpochTime::Seconds;

static EpochSeconds nowEpoch() { return EpochTime::now(); }

static bool jsonNumber(JsonVariantConst value) {
    return value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>();
}

static EpochSeconds jsonEpochOrZero(JsonVariantConst value) {
    if (value.is<bool>() || !value.is<std::int64_t>()) {
        return 0;
    }
    return value.as<std::int64_t>();
}

static std::optional<bool> optionalBool(JsonVariantConst value) {
    return value.is<bool>() ? std::optional<bool>(value.as<bool>()) : std::nullopt;
}

static void copyIfPresent(JsonObjectConst source, JsonObject target, const char *key) {
    JsonVariantConst value = source[key];
    if (!value.isNull()) {
        target[key].set(value);
    }
}

static void copyKeys(JsonObjectConst source, JsonObject target, const char *const *keys, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        copyIfPresent(source, target, keys[index]);
    }
}

static void addFixedCadenceMetadata(JsonObjectConst raw, JsonObject summary) {
    JsonObjectConst sequence = raw["fixed_cadence_sequence"].as<JsonObjectConst>();
    if (sequence.isNull()) {
        return;
    }
    summary["fixed_cadence_available"] = true;
    if (jsonNumber(sequence["sample_interval_ms"])) {
        summary["fixed_cadence_sample_interval_ms"] = sequence["sample_interval_ms"].as<int>();
    }
    JsonArrayConst pressure = sequence["pressure_bar"].as<JsonArrayConst>();
    if (!pressure.isNull()) {
        summary["fixed_cadence_step_count"] = static_cast<int>(pressure.size());
    }
}

static bool removeOldestAcrossSummaryStores() {
    String shotPath;
    EpochSeconds shotTime = std::numeric_limits<EpochSeconds>::max();
    const bool haveShot = LocalAutoTuningFiles::findOldestFile(SHOT_DIR, shotPath, shotTime);
    String recommendationPath;
    EpochSeconds recommendationTime = std::numeric_limits<EpochSeconds>::max();
    const bool haveRecommendation =
        LocalAutoTuningFiles::findOldestFile(RECOMMENDATION_DIR, recommendationPath, recommendationTime);
    if (!haveShot && !haveRecommendation) {
        return false;
    }
    return haveShot && (!haveRecommendation || shotTime <= recommendationTime) ? LittleFS.remove(shotPath)
                                                                               : LittleFS.remove(recommendationPath);
}

} // namespace

bool LocalAutoTuningSummaryStore::begin() const {
    const bool available = LocalAutoTuningFiles::ensureDirectory(STORE_DIR) && LocalAutoTuningFiles::ensureDirectory(SHOT_DIR) &&
                           LocalAutoTuningFiles::ensureDirectory(RECOMMENDATION_DIR);
    if (available) {
        LocalAutoTuningFiles::recoverDirectory(SHOT_DIR);
        LocalAutoTuningFiles::recoverDirectory(RECOMMENDATION_DIR);
    }
    return available;
}

bool LocalAutoTuningSummaryStore::reset() const {
    return begin() && LocalAutoTuningFiles::clearDirectory(SHOT_DIR) && LocalAutoTuningFiles::clearDirectory(RECOMMENDATION_DIR);
}

LocalAutoTuningSummaryStore::Stats LocalAutoTuningSummaryStore::stats() const {
    Stats result;
    const LocalAutoTuningFiles::DirectoryStats shots = LocalAutoTuningFiles::directoryStats(SHOT_DIR);
    const LocalAutoTuningFiles::DirectoryStats recommendations = LocalAutoTuningFiles::directoryStats(RECOMMENDATION_DIR);
    result.shotCount = shots.count;
    result.recommendationCount = recommendations.count;
    result.bytes = shots.bytes + recommendations.bytes;
    return result;
}

bool LocalAutoTuningSummaryStore::loadShot(const String &shotId, JsonDocument &document) const {
    return !shotId.isEmpty() && LocalAutoTuningFiles::readJson(LocalAutoTuningFiles::recordPath(SHOT_DIR, shotId), document);
}

bool LocalAutoTuningSummaryStore::removeShot(const String &shotId) const {
    if (shotId.isEmpty()) {
        return false;
    }
    const String path = LocalAutoTuningFiles::recordPath(SHOT_DIR, shotId);
    return LittleFS.exists(path) && LittleFS.remove(path);
}

bool LocalAutoTuningSummaryStore::upsertShot(JsonObjectConst raw) {
    if (!begin() || raw.isNull()) {
        return false;
    }
    const String shotId = raw["shot_id"].as<String>();
    if (shotId.isEmpty()) {
        return false;
    }

    JsonDocument outDoc(&psramAllocator);
    JsonObject out = outDoc.to<JsonObject>();
    out["event_type"] = "local_shot_summary";
    out["schema_version"] = 1;
    out["shot_id"] = shotId;
    out["updated_at"] = nowEpoch();

    static constexpr const char *KEYS[] = {
        "timestamp",
        "created_at",
        "install_id",
        "machine_id",
        "machine_adapter",
        "bean_context_id",
        "bean_context_name",
        "grinder_context_id",
        "grinder_context_name",
        "taste_goal",
        "profile_id",
        "profile_label",
        "profile_type",
        "raw_profile_hash",
        "shot_type",
        "shot_end_state",
        "weight_source",
        "flow_source",
        "raw_profile_available",
        "dose_in_g",
        "dose_observed",
        "dose_target_g",
        "dose_target_confirmed",
        "beverage_out_g",
        "beverage_out_observation",
        "predicted_final_beverage_out_g",
        "predictive_stop_applied",
        "predictive_stop_delay_ms",
        "predictive_stop_rate_g_per_s",
        "predictive_stop_lead_g",
        "brew_ratio",
        "target_yield_g",
        "target_ratio",
        "shot_time_s",
        "profile_temperature_c",
        "final_phase_temperature_c",
        "grinder_calibration_mode",
        "grinder_adjustment_mode",
        "microns_per_step",
        "step_direction",
        "relative_grind_steps_from_reference",
        "relative_grind_um_from_reference",
        "current_absolute_step",
        "absolute_reference_step",
        "action_observed",
        "recommendation_id",
        "recommended_grind_delta_steps_from_current",
        "recommended_grind_delta_um_from_current",
        "recommended_projected_relative_step_from_reference",
        "recommended_dose_g",
        "recommended_target_yield_g",
        "recommended_target_ratio",
        "grind_followed",
        "dose_followed",
        "yield_followed",
        "recommendation_followed",
        "exclude_from_local_optimization",
        "optimization_weight",
    };
    copyKeys(raw, out, KEYS, sizeof(KEYS) / sizeof(KEYS[0]));
    JsonObject observed = out["action_observed"].to<JsonObject>();
    observed["grind"] = raw["grind_observed"] | false;
    observed["dose"] = (raw["dose_observed"] | false) || (raw["dose_target_confirmed"] | false);
    observed["target_yield"] = jsonNumber(raw["target_yield_g"]);
    addFixedCadenceMetadata(raw, out);

    return LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(SHOT_DIR, shotId), outDoc);
}

bool LocalAutoTuningSummaryStore::upsertRecommendation(JsonObjectConst raw) {
    if (!begin() || raw.isNull()) {
        return false;
    }
    const String recommendationId = raw["recommendation_id"].as<String>();
    if (recommendationId.isEmpty()) {
        return false;
    }

    JsonDocument outDoc(&psramAllocator);
    JsonObject out = outDoc.to<JsonObject>();
    out["event_type"] = "local_recommendation_summary";
    out["schema_version"] = 1;
    out["recommendation_id"] = recommendationId;
    const EpochSeconds sourceUpdatedAt = jsonEpochOrZero(raw["updated_at"]);
    out["updated_at"] = sourceUpdatedAt > 0 ? sourceUpdatedAt : nowEpoch();

    static constexpr const char *KEYS[] = {
        "created_at",
        "expires_at",
        "install_id",
        "machine_id",
        "bean_context_id",
        "grinder_context_id",
        "profile_id",
        "raw_profile_hash",
        "source_shot_id",
        "optimization_run_id",
        "comparison_anchor_shot_id",
        "comparison_mode",
        "preference_feedback_required",
        "mode",
        "reason",
        "status",
        "shown_count",
        "accepted_at",
        "ignored_at",
        "edited_at",
        "used_at",
        "superseded_at",
        "apply_status",
        "apply_acknowledged_at",
        "applied_fields",
        "manual_fields",
        "apply_error",
        "grinder_calibration_mode",
        "grinder_adjustment_mode",
        "microns_per_step",
        "step_direction",
        "reference_label",
        "current_absolute_step",
        "absolute_reference_step",
        "projected_absolute_step",
        "grind_delta_steps_from_current",
        "grind_delta_um_from_current",
        "projected_relative_step_from_reference",
        "projected_relative_grind_um_from_reference",
        "next_dose_g",
        "target_yield_g",
        "target_ratio",
        "confidence",
    };
    copyKeys(raw, out, KEYS, sizeof(KEYS) / sizeof(KEYS[0]));

    return LocalAutoTuningFiles::writeJson(LocalAutoTuningFiles::recordPath(RECOMMENDATION_DIR, recommendationId), outDoc);
}

bool LocalAutoTuningSummaryStore::patchShotCorrection(const String &shotId, bool hasGrindFollowed, bool grindFollowed,
                                                      bool hasDoseFollowed, bool doseFollowed, bool hasYieldFollowed,
                                                      bool yieldFollowed) {
    if (!hasGrindFollowed && !hasDoseFollowed && !hasYieldFollowed) {
        return false;
    }
    JsonDocument doc(&psramAllocator);
    const String path = LocalAutoTuningFiles::recordPath(SHOT_DIR, shotId);
    if (!LocalAutoTuningFiles::readJson(path, doc)) {
        return false;
    }
    JsonObject root = doc.as<JsonObject>();
    std::optional<bool> storedGrind = optionalBool(root["grind_followed"]);
    std::optional<bool> storedDose = optionalBool(root["dose_followed"]);
    std::optional<bool> storedYield = optionalBool(root["yield_followed"]);
    if (hasGrindFollowed) {
        root["grind_followed"] = grindFollowed;
        storedGrind = grindFollowed;
    }
    if (hasDoseFollowed) {
        root["dose_followed"] = doseFollowed;
        storedDose = doseFollowed;
    }
    if (hasYieldFollowed) {
        root["yield_followed"] = yieldFollowed;
        storedYield = yieldFollowed;
    }
    const AutoTuning::FollowThroughStatus followThrough = AutoTuning::deriveFollowThrough(storedGrind, storedDose, storedYield);
    if (followThrough == AutoTuning::FollowThroughStatus::Unknown) {
        root.remove("recommendation_followed");
    } else {
        root["recommendation_followed"] = AutoTuning::followThroughStatusKey(followThrough);
    }
    root["updated_at"] = nowEpoch();
    return LocalAutoTuningFiles::writeJson(path, doc);
}

bool LocalAutoTuningSummaryStore::patchRecommendationStatus(const String &recommendationId, const String &status,
                                                            const char *timestampKey) {
    JsonDocument doc(&psramAllocator);
    const String path = LocalAutoTuningFiles::recordPath(RECOMMENDATION_DIR, recommendationId);
    if (!LocalAutoTuningFiles::readJson(path, doc)) {
        return false;
    }
    JsonObject root = doc.as<JsonObject>();
    const EpochSeconds now = nowEpoch();
    root["status"] = status;
    root[timestampKey] = now;
    root["updated_at"] = now;
    return LocalAutoTuningFiles::writeJson(path, doc);
}

void LocalAutoTuningSummaryStore::prune() const {
    if (!begin()) {
        return;
    }
    while (LocalAutoTuningFiles::directoryStats(SHOT_DIR).count > RETENTION.maxShotSummaries) {
        if (!LocalAutoTuningFiles::removeOldestFile(SHOT_DIR)) {
            break;
        }
    }
    while (LocalAutoTuningFiles::directoryStats(RECOMMENDATION_DIR).count > RETENTION.maxRecommendationSummaries) {
        if (!LocalAutoTuningFiles::removeOldestFile(RECOMMENDATION_DIR)) {
            break;
        }
    }
    while (LocalAutoTuningFiles::directoryStats(SHOT_DIR).bytes + LocalAutoTuningFiles::directoryStats(RECOMMENDATION_DIR).bytes >
           RETENTION.maxBytes) {
        if (!removeOldestAcrossSummaryStores()) {
            break;
        }
    }
}
