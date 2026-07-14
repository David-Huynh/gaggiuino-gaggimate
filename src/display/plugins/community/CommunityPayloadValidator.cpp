#include "CommunityPayloadValidator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <display/core/AutoTuning.h>
#include <display/core/EpochTime.h>
#include <display/plugins/autotuning/AutoTuningTasteGoalJson.h>

namespace CommunityPayloadValidator {
namespace {

constexpr double RATIO_RELATIVE_TOLERANCE = 1e-3;
constexpr double RATIO_ABSOLUTE_TOLERANCE = 1e-3;

static bool jsonNumber(JsonVariantConst value) {
    return !value.is<bool>() && (value.is<int>() || value.is<long>() || value.is<float>() || value.is<double>());
}

using EpochSeconds = EpochTime::Seconds;

static bool jsonEpoch(JsonVariantConst value, EpochSeconds &out) {
    if (value.is<bool>() || !value.is<std::int64_t>()) {
        return false;
    }
    out = value.as<std::int64_t>();
    return true;
}

} // namespace

static bool failPreflight(String &reason, const String &message) {
    reason = message;
    return false;
}

static bool requireString(JsonObjectConst payload, const char *key, String &reason) {
    const String value = payload[key].as<String>();
    if (value.isEmpty()) {
        return failPreflight(reason, String(key) + " is required");
    }
    return true;
}

static bool requireStrings(JsonObjectConst payload, const char *const *keys, size_t count, String &reason) {
    for (size_t i = 0; i < count; i++) {
        if (!requireString(payload, keys[i], reason)) {
            return false;
        }
    }
    return true;
}

enum NumberRuleFlag : uint8_t {
    NUMBER_OPTIONAL = 0,
    NUMBER_REQUIRED = 1 << 0,
    NUMBER_POSITIVE = 1 << 1,
    NUMBER_INTEGER = 1 << 2,
};

struct NumberRule {
    const char *key;
    double minValue;
    double maxValue;
    uint8_t flags;
};

static bool validateNumberRules(JsonObjectConst payload, const NumberRule *rules, size_t count, String &reason) {
    for (size_t i = 0; i < count; i++) {
        const NumberRule &rule = rules[i];
        JsonVariantConst value = payload[rule.key];
        if (value.isNull()) {
            if ((rule.flags & NUMBER_REQUIRED) == 0) {
                continue;
            }
            const String suffix = (rule.flags & NUMBER_POSITIVE) ? " must be positive and finite" : " out of range";
            return failPreflight(reason, String(rule.key) + suffix);
        }
        const double numeric = value.as<double>();
        const bool validValue =
            jsonNumber(value) && std::isfinite(numeric) &&
            ((rule.flags & NUMBER_POSITIVE) ? numeric > 0.0 : numeric >= rule.minValue && numeric <= rule.maxValue) &&
            ((rule.flags & NUMBER_INTEGER) == 0 || std::floor(numeric) == numeric);
        if (!validValue) {
            const String suffix = (rule.flags & NUMBER_POSITIVE) ? " must be positive and finite" : " out of range";
            return failPreflight(reason, String(rule.key) + suffix);
        }
    }
    return true;
}

static bool positiveNumber(JsonVariantConst value, double &parsed) {
    if (!jsonNumber(value) || value.is<bool>()) {
        return false;
    }
    parsed = value.as<double>();
    return std::isfinite(parsed) && parsed > 0.0;
}

static bool derivedRatioMatches(JsonObjectConst payload, const char *ratioKey, const char *numeratorKey,
                                const char *denominatorKey, String &reason) {
    if (payload[ratioKey].isNull()) {
        return true;
    }
    double ratio = 0.0;
    double numerator = 0.0;
    double denominator = 0.0;
    if (!positiveNumber(payload[ratioKey], ratio)) {
        return failPreflight(reason, String(ratioKey) + " must be positive and finite");
    }
    if (!positiveNumber(payload[numeratorKey], numerator) || !positiveNumber(payload[denominatorKey], denominator)) {
        return failPreflight(reason, String(ratioKey) + " requires positive " + numeratorKey + " and " + denominatorKey);
    }
    const double expected = numerator / denominator;
    const double tolerance = std::max(RATIO_ABSOLUTE_TOLERANCE, std::fabs(expected) * RATIO_RELATIVE_TOLERANCE);
    if (std::fabs(ratio - expected) > tolerance) {
        return failPreflight(reason, String(ratioKey) + " must be derived from " + numeratorKey + " / " + denominatorKey);
    }
    return true;
}

static bool validateOptionalBools(JsonObjectConst payload, const char *const *keys, size_t count, String &reason) {
    for (size_t i = 0; i < count; i++) {
        JsonVariantConst value = payload[keys[i]];
        if (!value.isNull() && !value.is<bool>()) {
            return failPreflight(reason, String(keys[i]) + " must be boolean");
        }
    }
    return true;
}

static bool valueAllowed(const String &value, const char *const *allowed, size_t allowedCount) {
    for (size_t i = 0; i < allowedCount; i++) {
        if (value == allowed[i]) {
            return true;
        }
    }
    return false;
}

static bool optionalEnum(JsonObjectConst payload, const char *key, const char *const *allowed, size_t allowedCount,
                         String &reason) {
    JsonVariantConst raw = payload[key];
    if (raw.isNull()) {
        return true;
    }
    const String value = raw.as<String>();
    if (value.isEmpty() || !valueAllowed(value, allowed, allowedCount)) {
        return failPreflight(reason, String(key) + " is invalid");
    }
    return true;
}

struct EnumRule {
    const char *key;
    const char *const *allowed;
    size_t allowedCount;
};

static bool validateEnumRules(JsonObjectConst payload, const EnumRule *rules, size_t count, String &reason) {
    for (size_t i = 0; i < count; i++) {
        if (!optionalEnum(payload, rules[i].key, rules[i].allowed, rules[i].allowedCount, reason)) {
            return false;
        }
    }
    return true;
}

static bool fixedCadenceLengthMatches(int length, int &expectedLength, String &reason) {
    if (expectedLength < 0) {
        expectedLength = length;
        return true;
    }
    if (expectedLength != length) {
        return failPreflight(reason, "fixed_cadence_sequence channels must have matching lengths");
    }
    return true;
}

static bool fixedCadenceNumericArray(JsonObjectConst sequence, const char *key, double minValue, double maxValue,
                                     int &expectedLength, String &reason) {
    JsonArrayConst values = sequence[key].as<JsonArrayConst>();
    if (values.isNull()) {
        return failPreflight(reason, String("fixed_cadence_sequence.") + key + " must be a list");
    }
    if (!fixedCadenceLengthMatches(static_cast<int>(values.size()), expectedLength, reason)) {
        return false;
    }
    for (JsonVariantConst value : values) {
        if (!jsonNumber(value)) {
            return failPreflight(reason, String("fixed_cadence_sequence.") + key + " contains invalid values");
        }
        const double numeric = value.as<double>();
        if (!std::isfinite(numeric) || numeric < minValue || numeric > maxValue) {
            return failPreflight(reason, String("fixed_cadence_sequence.") + key + " out of range");
        }
    }
    return true;
}

static bool fixedCadencePumpModes(JsonObjectConst sequence, int &expectedLength, String &reason) {
    JsonArrayConst values = sequence["pump_target_mode"].as<JsonArrayConst>();
    if (values.isNull()) {
        return failPreflight(reason, "fixed_cadence_sequence.pump_target_mode must be a list");
    }
    if (!fixedCadenceLengthMatches(static_cast<int>(values.size()), expectedLength, reason)) {
        return false;
    }
    for (JsonVariantConst value : values) {
        if (!jsonNumber(value) || value.is<bool>()) {
            return failPreflight(reason, "fixed_cadence_sequence.pump_target_mode contains invalid values");
        }
        const int mode = value.as<int>();
        if (mode < 0 || mode > 2) {
            return failPreflight(reason, "fixed_cadence_sequence.pump_target_mode contains invalid values");
        }
    }
    return true;
}

static bool fixedCadenceValveOpen(JsonObjectConst sequence, int &expectedLength, String &reason) {
    JsonArrayConst values = sequence["valve_open"].as<JsonArrayConst>();
    if (values.isNull()) {
        return failPreflight(reason, "fixed_cadence_sequence.valve_open must be a list");
    }
    if (!fixedCadenceLengthMatches(static_cast<int>(values.size()), expectedLength, reason)) {
        return false;
    }
    for (JsonVariantConst value : values) {
        if (!value.is<bool>()) {
            return failPreflight(reason, "fixed_cadence_sequence.valve_open contains invalid values");
        }
    }
    return true;
}

static bool optionalFixedCadenceSequence(JsonObjectConst payload, String &reason) {
    JsonVariantConst raw = payload["fixed_cadence_sequence"];
    if (raw.isNull()) {
        return true;
    }
    JsonObjectConst sequence = raw.as<JsonObjectConst>();
    if (sequence.isNull()) {
        return failPreflight(reason, "fixed_cadence_sequence must be an object");
    }
    if (!jsonNumber(sequence["sample_interval_ms"]) || sequence["sample_interval_ms"].as<int>() != 250) {
        return failPreflight(reason, "fixed_cadence_sequence.sample_interval_ms must be 250");
    }

    static constexpr NumberRule CHANNEL_RULES[] = {
        {"pressure_bar", 0.0, 15.0, NUMBER_REQUIRED},
        {"pressure_target_bar", 0.0, 15.0, NUMBER_REQUIRED},
        {"pump_flow_ml_s", 0.0, 20.0, NUMBER_REQUIRED},
        {"pump_flow_target_ml_s", 0.0, 20.0, NUMBER_REQUIRED},
        {"beverage_flow_g_s", 0.0, 20.0, NUMBER_REQUIRED},
        {"weight_g", -1.0, AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G, NUMBER_REQUIRED},
        {"temperature_c", 0.0, 160.0, NUMBER_REQUIRED},
        {"temperature_target_c", 0.0, 160.0, NUMBER_REQUIRED},
    };
    int expectedLength = -1;
    for (const NumberRule &rule : CHANNEL_RULES) {
        if (!fixedCadenceNumericArray(sequence, rule.key, rule.minValue, rule.maxValue, expectedLength, reason)) {
            return false;
        }
    }
    if (!fixedCadencePumpModes(sequence, expectedLength, reason) || !fixedCadenceValveOpen(sequence, expectedLength, reason)) {
        return false;
    }
    if (expectedLength < 2 || expectedLength > 500) {
        return failPreflight(reason, "fixed_cadence_sequence must contain 2..500 steps");
    }
    return true;
}

static bool optionalActionObserved(JsonObjectConst payload, String &reason) {
    JsonVariantConst raw = payload["action_observed"];
    if (raw.isNull()) {
        return true;
    }
    JsonObjectConst observed = raw.as<JsonObjectConst>();
    if (observed.isNull()) {
        return failPreflight(reason, "action_observed must be an object");
    }
    static constexpr const char *FIELDS[] = {"grind", "dose", "target_yield"};
    for (const char *field : FIELDS) {
        if (!observed[field].is<bool>()) {
            return failPreflight(reason, String("action_observed.") + field + " must be boolean");
        }
    }
    if (observed["grind"].as<bool>()) {
        const bool hasRelativeGrind = jsonNumber(payload["relative_grind_steps_from_reference"]);
        const bool hasAbsolutePair =
            jsonNumber(payload["current_absolute_step"]) && jsonNumber(payload["absolute_reference_step"]);
        if (!hasRelativeGrind && !hasAbsolutePair) {
            return failPreflight(reason, "action_observed.grind cannot be true without a grind measurement");
        }
    }
    if (observed["dose"].as<bool>()) {
        const bool measured = (payload["dose_observed"] | false) && jsonNumber(payload["dose_in_g"]);
        const bool confirmed = payload["dose_target_confirmed"] | false;
        if (!measured && !confirmed) {
            return failPreflight(reason, "action_observed.dose cannot be true without a measured or confirmed dose");
        }
    }
    return true;
}

static bool requireTimestamp(JsonObjectConst payload, const char *key, String &reason);
static bool optionalTimestamp(JsonObjectConst payload, const char *key, String &reason);
static bool recommendationTimestampOrder(JsonObjectConst payload, String &reason);

static bool validateShotRatios(JsonObjectConst payload, String &reason) {
    if (!derivedRatioMatches(payload, "target_ratio", "target_yield_g", "dose_target_g", reason) ||
        !derivedRatioMatches(payload, "recommended_target_ratio", "recommended_target_yield_g", "recommended_dose_g", reason)) {
        return false;
    }
    if (payload["brew_ratio"].isNull()) {
        return true;
    }
    double dose = 0.0;
    const bool doseObserved = payload["dose_observed"] | false;
    const bool doseConfirmed = payload["dose_target_confirmed"] | false;
    const bool observationFlagsMissing = payload["dose_observed"].isNull() && payload["dose_target_confirmed"].isNull();
    const char *denominatorKey = nullptr;
    if (doseObserved && positiveNumber(payload["dose_in_g"], dose)) {
        denominatorKey = "dose_in_g";
    } else if (doseConfirmed) {
        denominatorKey = "dose_target_g";
    } else if (observationFlagsMissing && positiveNumber(payload["dose_in_g"], dose)) {
        denominatorKey = "dose_in_g";
    }
    if (!denominatorKey) {
        return failPreflight(reason, "brew_ratio requires an observed or confirmed dose");
    }
    return derivedRatioMatches(payload, "brew_ratio", "beverage_out_g", denominatorKey, reason);
}

static bool preflightShot(JsonObjectConst payload, const String &recordId, String &reason) {
    if (payload["event_type"].as<String>() != "shot_record") {
        return failPreflight(reason, "event_type must be shot_record");
    }
    if (!jsonNumber(payload["schema_version"]) || payload["schema_version"].as<int>() != 1) {
        return failPreflight(reason, "schema_version is unsupported");
    }
    static constexpr const char *REQUIRED_STRINGS[] = {"shot_id", "install_id", "machine_id"};
    if (!requireStrings(payload, REQUIRED_STRINGS, sizeof(REQUIRED_STRINGS) / sizeof(*REQUIRED_STRINGS), reason)) {
        return false;
    }
    if (payload["shot_id"].as<String>() != recordId) {
        return failPreflight(reason, "shot_id does not match queue record");
    }
    JsonDocument normalizedTasteGoal;
    if (!AutoTuning::normalizeTasteGoal(payload["taste_goal"], normalizedTasteGoal, reason)) {
        return false;
    }
    if (!requireTimestamp(payload, "timestamp", reason) || !optionalTimestamp(payload, "created_at", reason) ||
        !optionalTimestamp(payload, "updated_at", reason)) {
        return false;
    }

    static constexpr const char *CALIBRATION_MODES[] = {"uncalibrated", "relative_calibrated", "absolute_display_calibrated"};
    static constexpr const char *ADJUSTMENT_MODES[] = {"stepped", "stepless"};
    static constexpr const char *STEP_DIRECTIONS[] = {"higher_is_finer", "higher_is_coarser"};
    static constexpr const char *RECOMMENDATION_DECISIONS[] = {"accepted", "edited", "ignored", "dismissed", "unknown"};
    static constexpr const char *RECOMMENDATION_FOLLOWED[] = {"followed", "partially_followed", "not_followed", "unknown"};
    static constexpr const char *SHOT_TYPES[] = {"espresso", "utility_flush", "cleaning", "calibration", "unknown"};
    static constexpr const char *PHASE_TYPES[] = {"preinfusion", "brew"};
    static constexpr const char *PUMP_TARGETS[] = {"simple", "pressure", "flow"};

    static constexpr NumberRule NUMBER_RULES[] = {
        {"dose_in_g", AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G, AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G, NUMBER_OPTIONAL},
        {"dose_target_g", AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G, AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G, NUMBER_REQUIRED},
        {"target_yield_g", AutoTuning::RECIPE_DOMAIN_OUTPUT_MIN_G, AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G, NUMBER_REQUIRED},
        {"profile_temperature_c", 0.0, 160.0, NUMBER_REQUIRED},
        {"final_phase_temperature_c", 0.0, 160.0, NUMBER_REQUIRED},
        {"beverage_out_g", 0.0, AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G, NUMBER_OPTIONAL},
        {"predicted_final_beverage_out_g", 0.0, AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G, NUMBER_OPTIONAL},
        {"predictive_stop_lead_g", 0.0, 20.0, NUMBER_OPTIONAL},
        {"predictive_stop_delay_ms", 0.0, 10000.0, NUMBER_OPTIONAL},
        {"predictive_stop_rate_g_per_s", 0.0, 25.0, NUMBER_OPTIONAL},
        {"brew_ratio", 0.0, 0.0, NUMBER_POSITIVE},
        {"target_ratio", 0.0, 0.0, NUMBER_POSITIVE},
        {"shot_time_s", 0.0, 180.0, NUMBER_OPTIONAL},
        {"microns_per_step", 0.1, 100.0, NUMBER_OPTIONAL},
        {"relative_grind_steps_from_reference", -10000.0, 10000.0, NUMBER_OPTIONAL},
        {"relative_grind_um_from_reference", -1000000.0, 1000000.0, NUMBER_OPTIONAL},
        {"current_absolute_step", -10000.0, 10000.0, NUMBER_OPTIONAL},
        {"absolute_reference_step", -10000.0, 10000.0, NUMBER_OPTIONAL},
        {"recommended_grind_delta_steps_from_current", -1000.0, 1000.0, NUMBER_OPTIONAL},
        {"recommended_grind_delta_um_from_current", -100000.0, 100000.0, NUMBER_OPTIONAL},
        {"recommended_projected_relative_step_from_reference", -10000.0, 10000.0, NUMBER_OPTIONAL},
        {"recommended_dose_g", AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G, AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G, NUMBER_OPTIONAL},
        {"recommended_target_yield_g", AutoTuning::RECIPE_DOMAIN_OUTPUT_MIN_G, AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G,
         NUMBER_OPTIONAL},
        {"recommended_target_ratio", 0.0, 0.0, NUMBER_POSITIVE},
        {"profile_phase_count", 0.0, 100.0, NUMBER_INTEGER},
        {"final_phase_index", 0.0, 100.0, NUMBER_INTEGER},
        {"final_phase_elapsed_s", 0.0, 600.0, NUMBER_OPTIONAL},
        {"final_target_pressure", 0.0, 15.0, NUMBER_OPTIONAL},
        {"final_target_flow", 0.0, 25.0, NUMBER_OPTIONAL},
    };
    static constexpr EnumRule ENUM_RULES[] = {
        {"grinder_calibration_mode", CALIBRATION_MODES, sizeof(CALIBRATION_MODES) / sizeof(*CALIBRATION_MODES)},
        {"grinder_adjustment_mode", ADJUSTMENT_MODES, sizeof(ADJUSTMENT_MODES) / sizeof(*ADJUSTMENT_MODES)},
        {"step_direction", STEP_DIRECTIONS, sizeof(STEP_DIRECTIONS) / sizeof(*STEP_DIRECTIONS)},
        {"recommendation_decision", RECOMMENDATION_DECISIONS,
         sizeof(RECOMMENDATION_DECISIONS) / sizeof(*RECOMMENDATION_DECISIONS)},
        {"recommendation_followed", RECOMMENDATION_FOLLOWED, sizeof(RECOMMENDATION_FOLLOWED) / sizeof(*RECOMMENDATION_FOLLOWED)},
        {"final_phase_type", PHASE_TYPES, sizeof(PHASE_TYPES) / sizeof(*PHASE_TYPES)},
        {"final_pump_target", PUMP_TARGETS, sizeof(PUMP_TARGETS) / sizeof(*PUMP_TARGETS)},
        {"shot_type", SHOT_TYPES, sizeof(SHOT_TYPES) / sizeof(*SHOT_TYPES)},
    };
    static constexpr const char *BOOL_KEYS[] = {
        "exclude_from_local_optimization",
        "dose_observed",
        "dose_target_confirmed",
        "predictive_stop_applied",
        "grind_followed",
        "dose_followed",
        "yield_followed",
        "pump_flow_calibration_required",
        "profile_flow_valid",
        "profile_flow_masked",
        "final_valve_open",
    };

    if (!validateNumberRules(payload, NUMBER_RULES, sizeof(NUMBER_RULES) / sizeof(*NUMBER_RULES), reason) ||
        !validateEnumRules(payload, ENUM_RULES, sizeof(ENUM_RULES) / sizeof(*ENUM_RULES), reason) ||
        !optionalActionObserved(payload, reason) ||
        !validateOptionalBools(payload, BOOL_KEYS, sizeof(BOOL_KEYS) / sizeof(*BOOL_KEYS), reason) ||
        !optionalFixedCadenceSequence(payload, reason) || !validateShotRatios(payload, reason)) {
        return false;
    }
    const String shotType = payload["shot_type"].as<String>();
    if (!shotType.isEmpty() && shotType != "espresso") {
        return failPreflight(reason, "shot_type must be espresso");
    }
    return true;
}

static bool preflightRecommendation(JsonObjectConst payload, const String &recordId, String &reason) {
    if (payload["event_type"].as<String>() != "recommendation_record") {
        return failPreflight(reason, "event_type must be recommendation_record");
    }
    if (!jsonNumber(payload["schema_version"]) || payload["schema_version"].as<int>() != 1) {
        return failPreflight(reason, "schema_version is unsupported");
    }
    static constexpr const char *REQUIRED_STRINGS[] = {"recommendation_id", "install_id", "machine_id"};
    if (!requireStrings(payload, REQUIRED_STRINGS, sizeof(REQUIRED_STRINGS) / sizeof(*REQUIRED_STRINGS), reason)) {
        return false;
    }
    if (payload["recommendation_id"].as<String>() != recordId) {
        return failPreflight(reason, "recommendation_id does not match queue record");
    }
    JsonDocument normalizedTasteGoal;
    if (!AutoTuning::normalizeTasteGoal(payload["taste_goal"], normalizedTasteGoal, reason)) {
        return false;
    }

    static constexpr const char *CALIBRATION_MODES[] = {"uncalibrated", "relative_calibrated", "absolute_display_calibrated"};
    static constexpr const char *ADJUSTMENT_MODES[] = {"stepped", "stepless"};
    static constexpr const char *STEP_DIRECTIONS[] = {"higher_is_finer", "higher_is_coarser"};
    static constexpr const char *MODES[] = {"cpbo_global_previous", "cpbo_best_incumbent"};
    static constexpr const char *COMPARISON_MODES[] = {"global_previous", "best_incumbent"};
    static constexpr const char *STATUSES[] = {"pending", "shown", "accepted", "edited",
                                               "ignored", "used",  "expired",  "superseded"};
    static constexpr const char *APPLY_STATUSES[] = {"unknown", "applied", "partially_applied", "manual_required", "failed"};

    static constexpr NumberRule NUMBER_RULES[] = {
        {"grind_delta_steps_from_current", -1000.0, 1000.0, NUMBER_OPTIONAL},
        {"grind_delta_um_from_current", -100000.0, 100000.0, NUMBER_OPTIONAL},
        {"projected_relative_step_from_reference", -10000.0, 10000.0, NUMBER_OPTIONAL},
        {"projected_relative_grind_um_from_reference", -1000000.0, 1000000.0, NUMBER_OPTIONAL},
        {"microns_per_step", 0.1, 100.0, NUMBER_OPTIONAL},
        {"current_absolute_step", -10000.0, 10000.0, NUMBER_OPTIONAL},
        {"absolute_reference_step", -10000.0, 10000.0, NUMBER_OPTIONAL},
        {"projected_absolute_step", -10000.0, 10000.0, NUMBER_OPTIONAL},
        {"next_dose_g", AutoTuning::RECIPE_DOMAIN_DOSE_MIN_G, AutoTuning::RECIPE_DOMAIN_DOSE_MAX_G, NUMBER_REQUIRED},
        {"target_yield_g", AutoTuning::RECIPE_DOMAIN_OUTPUT_MIN_G, AutoTuning::RECIPE_DOMAIN_OUTPUT_MAX_G, NUMBER_REQUIRED},
        {"target_ratio", 0.0, 0.0, static_cast<uint8_t>(NUMBER_REQUIRED | NUMBER_POSITIVE)},
        {"confidence", 0.0, 1.0, NUMBER_OPTIONAL},
        {"shown_count", 0.0, 1000000.0, NUMBER_INTEGER},
    };
    static constexpr EnumRule ENUM_RULES[] = {
        {"grinder_calibration_mode", CALIBRATION_MODES, sizeof(CALIBRATION_MODES) / sizeof(*CALIBRATION_MODES)},
        {"grinder_adjustment_mode", ADJUSTMENT_MODES, sizeof(ADJUSTMENT_MODES) / sizeof(*ADJUSTMENT_MODES)},
        {"step_direction", STEP_DIRECTIONS, sizeof(STEP_DIRECTIONS) / sizeof(*STEP_DIRECTIONS)},
        {"mode", MODES, sizeof(MODES) / sizeof(*MODES)},
        {"status", STATUSES, sizeof(STATUSES) / sizeof(*STATUSES)},
        {"comparison_mode", COMPARISON_MODES, sizeof(COMPARISON_MODES) / sizeof(*COMPARISON_MODES)},
        {"apply_status", APPLY_STATUSES, sizeof(APPLY_STATUSES) / sizeof(*APPLY_STATUSES)},
    };
    static constexpr const char *BOOL_KEYS[] = {"preference_feedback_required"};

    const bool fieldsValid = requireTimestamp(payload, "created_at", reason) && requireTimestamp(payload, "updated_at", reason) &&
                             optionalTimestamp(payload, "expires_at", reason) &&
                             validateNumberRules(payload, NUMBER_RULES, sizeof(NUMBER_RULES) / sizeof(*NUMBER_RULES), reason) &&
                             validateEnumRules(payload, ENUM_RULES, sizeof(ENUM_RULES) / sizeof(*ENUM_RULES), reason) &&
                             optionalTimestamp(payload, "accepted_at", reason) &&
                             optionalTimestamp(payload, "ignored_at", reason) &&
                             optionalTimestamp(payload, "edited_at", reason) && optionalTimestamp(payload, "used_at", reason) &&
                             optionalTimestamp(payload, "superseded_at", reason) &&
                             validateOptionalBools(payload, BOOL_KEYS, sizeof(BOOL_KEYS) / sizeof(*BOOL_KEYS), reason) &&
                             optionalTimestamp(payload, "apply_acknowledged_at", reason) &&
                             derivedRatioMatches(payload, "target_ratio", "target_yield_g", "next_dose_g", reason);
    if (!fieldsValid || !recommendationTimestampOrder(payload, reason)) {
        return false;
    }

    const String mode = payload["mode"].as<String>();
    const String runId = payload["optimization_run_id"].as<String>();
    const String anchorShotId = payload["comparison_anchor_shot_id"].as<String>();
    const String comparisonMode = payload["comparison_mode"].as<String>();
    const String expectedMode = mode == "cpbo_global_previous" ? "global_previous" : "best_incumbent";
    if (runId.isEmpty() || anchorShotId.isEmpty()) {
        return failPreflight(reason, "CPBO recommendation is missing comparison identifiers");
    }
    if (!payload["preference_feedback_required"].is<bool>() || !payload["preference_feedback_required"].as<bool>()) {
        return failPreflight(reason, "CPBO recommendation must require preference feedback");
    }
    if (comparisonMode != expectedMode) {
        return failPreflight(reason, "CPBO recommendation comparison mode mismatch");
    }
    return true;
}

static bool preflightComparison(JsonObjectConst payload, const String &recordId, String &reason) {
    if (payload["event_type"].as<String>() != "comparison_record") {
        return failPreflight(reason, "event_type must be comparison_record");
    }
    if (!jsonNumber(payload["schema_version"]) || payload["schema_version"].as<int>() != 1) {
        return failPreflight(reason, "schema_version is unsupported");
    }
    static constexpr const char *REQUIRED_STRINGS[] = {"comparison_id",  "optimization_run_id", "new_shot_id",
                                                       "anchor_shot_id", "install_id",          "machine_id"};
    if (!requireStrings(payload, REQUIRED_STRINGS, sizeof(REQUIRED_STRINGS) / sizeof(*REQUIRED_STRINGS), reason)) {
        return false;
    }
    if (payload["comparison_id"].as<String>() != recordId) {
        return failPreflight(reason, "comparison_id does not match queue record");
    }
    if (payload["new_shot_id"].as<String>() == payload["anchor_shot_id"].as<String>()) {
        return failPreflight(reason, "comparison requires distinct physical shots");
    }
    JsonDocument normalizedTasteGoal;
    if (!AutoTuning::normalizeTasteGoal(payload["taste_goal"], normalizedTasteGoal, reason)) {
        return false;
    }
    static constexpr const char *LABELS[] = {"new_better", "anchor_better", "tie"};
    static constexpr const char *MODES[] = {"global_previous", "best_incumbent"};
    static constexpr EnumRule ENUM_RULES[] = {
        {"label", LABELS, sizeof(LABELS) / sizeof(*LABELS)},
        {"comparison_mode", MODES, sizeof(MODES) / sizeof(*MODES)},
    };
    if (!validateEnumRules(payload, ENUM_RULES, sizeof(ENUM_RULES) / sizeof(*ENUM_RULES), reason) ||
        !requireTimestamp(payload, "created_at", reason)) {
        return false;
    }
    if (payload["label"].as<String>().isEmpty() || payload["comparison_mode"].as<String>().isEmpty()) {
        return failPreflight(reason, "comparison label and mode are required");
    }
    return true;
}

bool validateRecord(JsonObjectConst payload, const String &recordType, const String &recordId, String &reason) {
    if (recordType == "shot") {
        return preflightShot(payload, recordId, reason);
    }
    if (recordType == "recommendation") {
        return preflightRecommendation(payload, recordId, reason);
    }
    if (recordType == "comparison") {
        return preflightComparison(payload, recordId, reason);
    }
    return failPreflight(reason, "unknown record type");
}

bool serializeValidated(JsonObjectConst payload, const String &recordType, const String &recordId, String &payloadJson,
                        String &reason) {
    if (!validateRecord(payload, recordType, recordId, reason)) {
        return false;
    }
    payloadJson = "";
    serializeJson(payload, payloadJson);
    if (payloadJson.isEmpty() || payloadJson.length() > MAX_PAYLOAD_BYTES) {
        return failPreflight(reason, "payload exceeds size limit");
    }
    return true;
}

static bool requireTimestamp(JsonObjectConst payload, const char *key, String &reason) {
    JsonVariantConst value = payload[key];
    EpochSeconds timestamp = 0;
    if (!jsonEpoch(value, timestamp)) {
        return failPreflight(reason, String(key) + " must be a Unix timestamp");
    }
    if (!EpochTime::plausible(timestamp)) {
        return failPreflight(reason, String(key) + " must be a plausible Unix timestamp");
    }
    return true;
}

static bool optionalTimestamp(JsonObjectConst payload, const char *key, String &reason) {
    return payload[key].isNull() || requireTimestamp(payload, key, reason);
}

static bool recommendationTimestampOrder(JsonObjectConst payload, String &reason) {
    const EpochSeconds createdAt = payload["created_at"].as<std::int64_t>();
    const EpochSeconds updatedAt = payload["updated_at"].as<std::int64_t>();
    if (updatedAt < createdAt) {
        return failPreflight(reason, "updated_at cannot precede created_at");
    }
    if (!payload["expires_at"].isNull() && payload["expires_at"].as<std::int64_t>() < createdAt) {
        return failPreflight(reason, "expires_at cannot precede created_at");
    }
    static constexpr const char *LIFECYCLE_TIMESTAMPS[] = {"accepted_at", "ignored_at",    "edited_at",
                                                           "used_at",     "superseded_at", "apply_acknowledged_at"};
    for (const char *key : LIFECYCLE_TIMESTAMPS) {
        if (!payload[key].isNull()) {
            const EpochSeconds timestamp = payload[key].as<std::int64_t>();
            if (timestamp < createdAt || timestamp > updatedAt) {
                return failPreflight(reason, String(key) + " is outside the recommendation lifecycle");
            }
        }
    }
    return true;
}

} // namespace CommunityPayloadValidator
