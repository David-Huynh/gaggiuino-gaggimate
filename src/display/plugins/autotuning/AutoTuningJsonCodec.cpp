#include "AutoTuningJsonCodec.h"

#include "AutoTuningTasteGoalJson.h"

#include <cmath>
#include <display/util/PsramAllocator.h>

namespace AutoTuningJsonCodec {
namespace {

constexpr size_t MAX_SHOT_SAMPLES = 240;
constexpr size_t MAX_ID_LENGTH = 256;
constexpr size_t MAX_CONTEXT_ID_LENGTH = 160;

bool jsonNumber(JsonVariantConst value) {
    return !value.is<bool>() &&
           (value.is<int>() || value.is<long>() || value.is<long long>() || value.is<float>() || value.is<double>());
}

bool finiteNumber(JsonVariantConst value) { return jsonNumber(value) && std::isfinite(value.as<double>()); }

bool exactUint16(JsonVariantConst value, std::uint16_t &output) {
    if (!finiteNumber(value)) {
        return false;
    }
    const double numeric = value.as<double>();
    if (numeric < 0.0 || numeric > UINT16_MAX || std::trunc(numeric) != numeric) {
        return false;
    }
    output = static_cast<std::uint16_t>(numeric);
    return true;
}

std::string text(JsonVariantConst value) {
    const char *encoded = value.as<const char *>();
    return encoded ? encoded : "";
}

void optionalString(JsonObject output, const char *key, std::string const &value) {
    if (!value.empty()) {
        output[key] = value.c_str();
    }
}

void optionalFloat(JsonObject output, const char *key, std::optional<float> value) {
    if (value.has_value()) {
        output[key] = *value;
    }
}

void optionalTimestamp(JsonObject output, const char *key, std::optional<AutoTuning::Timestamp> value) {
    if (value.has_value()) {
        output[key] = *value;
    }
}

std::optional<float> readOptionalFloat(JsonObjectConst input, const char *key, bool &valid) {
    JsonVariantConst value = input[key];
    if (value.isNull()) {
        return std::nullopt;
    }
    if (!finiteNumber(value)) {
        valid = false;
        return std::nullopt;
    }
    return value.as<float>();
}

std::optional<bool> readOptionalBool(JsonObjectConst input, const char *key, bool &valid) {
    JsonVariantConst value = input[key];
    if (value.isNull()) {
        return std::nullopt;
    }
    if (!value.is<bool>()) {
        valid = false;
        return std::nullopt;
    }
    return value.as<bool>();
}

bool readBoolOrDefault(JsonObjectConst input, const char *key, bool fallback, bool &valid) {
    JsonVariantConst value = input[key];
    if (value.isNull()) {
        return fallback;
    }
    if (!value.is<bool>()) {
        valid = false;
        return fallback;
    }
    return value.as<bool>();
}

std::optional<AutoTuning::Timestamp> readOptionalTimestamp(JsonObjectConst input, const char *key, bool &valid) {
    JsonVariantConst value = input[key];
    if (value.isNull()) {
        return std::nullopt;
    }
    if (value.is<bool>() || !value.is<AutoTuning::Timestamp>()) {
        valid = false;
        return std::nullopt;
    }
    const auto timestamp = value.as<AutoTuning::Timestamp>();
    if (timestamp < 0) {
        valid = false;
        return std::nullopt;
    }
    return timestamp;
}

bool requiredTimestamp(JsonObjectConst input, const char *key, AutoTuning::Timestamp &timestamp) {
    bool valid = true;
    const std::optional<AutoTuning::Timestamp> parsed = readOptionalTimestamp(input, key, valid);
    if (!valid || !parsed.has_value()) {
        return false;
    }
    timestamp = *parsed;
    return true;
}

bool requiredFinite(JsonObjectConst input, const char *key, float &output) {
    if (!finiteNumber(input[key])) {
        return false;
    }
    output = input[key].as<float>();
    return true;
}

void writeRecommendationReference(AutoTuning::RecommendationReference const &recommendation, JsonObject output) {
    if (!recommendation.present()) {
        return;
    }
    output["recommendation_id"] = recommendation.recommendationId.c_str();
    optionalString(output, "optimization_run_id", recommendation.optimizationRunId);
    optionalString(output, "comparison_anchor_shot_id", recommendation.anchorShotId);
    if (recommendation.comparisonMode != AutoTuning::ComparisonMode::None) {
        output["comparison_mode"] = AutoTuning::comparisonModeKey(recommendation.comparisonMode);
    }
    output["preference_feedback_required"] = recommendation.preferenceFeedbackRequired;
    output["recommended_grind_delta_steps_from_current"] = recommendation.grindDeltaStepsFromCurrent;
    output["recommended_grind_delta_um_from_current"] = recommendation.grindDeltaMicronsFromCurrent;
    output["recommended_projected_relative_step_from_reference"] = recommendation.projectedRelativeStepFromReference;
    output["recommended_projected_relative_grind_um_from_reference"] = recommendation.projectedRelativeMicronsFromReference;
    output["recommended_dose_g"] = recommendation.nextDoseG;
    output["recommended_target_yield_g"] = recommendation.targetYieldG;
    output["recommended_target_ratio"] = recommendation.targetRatio;
}

void readRecommendationReference(JsonObjectConst input, AutoTuning::RecommendationReference &recommendation, bool &valid) {
    recommendation.recommendationId = text(input["recommendation_id"]);
    if (recommendation.recommendationId.empty()) {
        return;
    }
    recommendation.optimizationRunId = text(input["optimization_run_id"]);
    recommendation.anchorShotId = text(input["comparison_anchor_shot_id"]);
    const std::string comparison = text(input["comparison_mode"]);
    if (!comparison.empty()) {
        auto parsed = AutoTuning::comparisonModeFromKey(comparison);
        if (!parsed) {
            valid = false;
            return;
        }
        recommendation.comparisonMode = *parsed;
    }
    if (!input["preference_feedback_required"].isNull()) {
        if (!input["preference_feedback_required"].is<bool>()) {
            valid = false;
            return;
        }
        recommendation.preferenceFeedbackRequired = input["preference_feedback_required"].as<bool>();
    }
    valid = requiredFinite(input, "recommended_grind_delta_steps_from_current", recommendation.grindDeltaStepsFromCurrent) &&
            requiredFinite(input, "recommended_grind_delta_um_from_current", recommendation.grindDeltaMicronsFromCurrent) &&
            requiredFinite(input, "recommended_projected_relative_step_from_reference",
                           recommendation.projectedRelativeStepFromReference) &&
            requiredFinite(input, "recommended_projected_relative_grind_um_from_reference",
                           recommendation.projectedRelativeMicronsFromReference) &&
            requiredFinite(input, "recommended_dose_g", recommendation.nextDoseG) &&
            requiredFinite(input, "recommended_target_yield_g", recommendation.targetYieldG) &&
            requiredFinite(input, "recommended_target_ratio", recommendation.targetRatio);
}

} // namespace

void DecodedShotRecord::bindSamples() {
    record.samples = AutoTuning::ArrayView<const AutoTuning::ShotSample>(samples.data(), samples.size());
}

bool writeShotRecord(AutoTuning::ShotRecord const &record, JsonDocument &document) {
    if (record.shotId.empty() || record.machineId.empty() || record.samples.empty()) {
        return false;
    }
    document.clear();
    JsonObject output = document.to<JsonObject>();
    output["event_type"] = "shot_profile";
    output["schema_version"] = 1;
    output["shot_id"] = record.shotId.c_str();
    output["machine_id"] = record.machineId.c_str();
    output["machine_adapter"] = record.machineAdapter.c_str();
    output["timestamp"] = record.timestamp;
    output["n_samples"] = record.samples.size();
    output["shot_type"] = "espresso";
    output["utility"] = record.utility;
    output["exclude_from_local_optimization"] = record.excludeFromLocalOptimization;
    output["local_optimization_enabled"] = record.localOptimizationEnabled;
    output["community_upload_enabled"] = record.communityUploadEnabled;
    output["community_upload_owner"] = record.communityUploadOwner.c_str();
    output["optimization_weight"] = record.optimizationWeight;
    output["weight_source"] = record.weightSource.c_str();
    output["flow_source"] = record.flowSource.c_str();
    output["flow_units"] = record.flowUnits.c_str();
    output["pump_flow_source"] = record.pumpFlowSource.c_str();
    output["pump_flow_units"] = record.pumpFlowUnits.c_str();
    output["pump_flow_interpretation"] = record.pumpFlowInterpretation.c_str();
    output["pump_flow_calibration_required"] = record.pumpFlowCalibrationRequired;
    optionalString(output, "predictive_weight_interpretation", record.predictiveWeightInterpretation);
    output["beverage_out_g"] = record.beverageOutG;
    output["beverage_out_observation"] = record.beverageOutObservation.c_str();
    output["shot_time_s"] = record.shotTimeS;
    output["predictive_stop_applied"] = record.predictiveStopApplied;
    optionalFloat(output, "predictive_stop_delay_ms", record.predictiveStopDelayMs);
    optionalFloat(output, "predictive_stop_rate_g_per_s", record.predictiveStopRateGPerS);
    optionalFloat(output, "predictive_stop_lead_g", record.predictiveStopLeadG);
    optionalFloat(output, "predicted_final_beverage_out_g", record.predictedFinalBeverageOutG);

    output["bean_context_id"] = record.recipe.beanContextId.c_str();
    output["bean_context_name"] = record.recipe.beanContextName.c_str();
    AutoTuning::writeTasteGoal(record.recipe.tasteGoal, output["taste_goal"]);
    optionalFloat(output, "dose_target_g", record.recipe.doseTargetG);
    optionalFloat(output, "target_yield_g", record.recipe.targetYieldG);
    optionalFloat(output, "target_ratio", record.recipe.targetRatio);
    optionalFloat(output, "dose_in_g", record.measuredDoseG);
    output["dose_observed"] = record.doseObserved;
    if (record.doseTargetConfirmed) {
        output["dose_target_confirmed"] = true;
    }
    if (record.grindFollowed.has_value()) {
        output["grind_followed"] = *record.grindFollowed;
    }
    if (record.doseFollowed.has_value()) {
        output["dose_followed"] = *record.doseFollowed;
    }
    if (record.yieldFollowed.has_value()) {
        output["yield_followed"] = *record.yieldFollowed;
    }
    const AutoTuning::FollowThroughStatus followThrough =
        AutoTuning::deriveFollowThrough(record.grindFollowed, record.doseFollowed, record.yieldFollowed);
    if (followThrough != AutoTuning::FollowThroughStatus::Unknown) {
        output["recommendation_followed"] = AutoTuning::followThroughStatusKey(followThrough);
    }

    AutoTuning::GrinderSnapshot const &grinder = record.recipe.grinder;
    optionalString(output, "grinder_context_id", grinder.contextId);
    optionalString(output, "grinder_context_name", grinder.contextName);
    optionalString(output, "grinder_calibration_mode", grinder.calibrationMode);
    optionalString(output, "grinder_adjustment_mode", grinder.adjustmentMode);
    optionalString(output, "step_direction", grinder.stepDirection);
    optionalString(output, "reference_label", grinder.referenceLabel);
    optionalFloat(output, "microns_per_step", grinder.micronsPerStep);
    optionalFloat(output, "current_absolute_step", grinder.currentAbsoluteStep);
    optionalFloat(output, "absolute_reference_step", grinder.absoluteReferenceStep);
    optionalFloat(output, "relative_grind_steps_from_reference", grinder.relativeStepsFromReference);
    optionalFloat(output, "relative_grind_um_from_reference", grinder.relativeMicronsFromReference);
    output["grind_observed"] = grinder.observed;

    if (record.profile.available) {
        output["profile_id"] = record.profile.id.c_str();
        output["profile_label"] = record.profile.label.c_str();
        output["profile_type"] = record.profile.type.c_str();
        output["profile_phase_count"] = record.profile.phaseCount;
        output["profile_temperature_c"] = record.profile.temperatureC;
        optionalString(output, "raw_profile_hash", record.profile.rawProfileHash);
        output["raw_profile_available"] = record.profile.rawProfileAvailable;
        if (record.profile.flowValid.has_value()) {
            output["profile_flow_valid"] = *record.profile.flowValid;
        }
        if (record.profile.flowMasked.has_value()) {
            output["profile_flow_masked"] = *record.profile.flowMasked;
        }
    }

    if (record.finalPhase.available) {
        output["final_phase_index"] = record.finalPhase.index;
        output["final_phase_name"] = record.finalPhase.name.c_str();
        output["final_phase_type"] = record.finalPhase.type.c_str();
        output["final_phase_elapsed_s"] = record.finalPhase.elapsedS;
        output["final_pump_target"] = record.finalPhase.pumpTarget.c_str();
        optionalFloat(output, "final_target_pressure", record.finalPhase.targetPressure);
        optionalFloat(output, "final_target_flow", record.finalPhase.targetFlow);
        output["final_valve_open"] = record.finalPhase.valveOpen;
        output["final_phase_temperature_c"] = record.finalPhase.temperatureC;
        output["shot_end_state"] = record.finalPhase.shotEndState.c_str();
    }
    writeRecommendationReference(record.recommendation, output);

    JsonArray pressure = output["pressure"].to<JsonArray>();
    JsonArray targetPressure = output["target_pressure"].to<JsonArray>();
    JsonArray flow = output["flow"].to<JsonArray>();
    JsonArray pumpFlow = output["pump_flow"].to<JsonArray>();
    JsonArray targetFlow = output["target_flow"].to<JsonArray>();
    JsonArray temperature = output["temperature"].to<JsonArray>();
    JsonArray targetTemperature = output["target_temperature"].to<JsonArray>();
    JsonArray weight = output["weight"].to<JsonArray>();
    JsonArray pumpTargetMode = output["pump_target_mode"].to<JsonArray>();
    JsonArray valveOpen = output["valve_open"].to<JsonArray>();
    JsonArray timeMs = output["time_ms"].to<JsonArray>();
    for (AutoTuning::ShotSample const &sample : record.samples) {
        pressure.add(sample.pressure);
        targetPressure.add(sample.targetPressure);
        flow.add(sample.flow);
        pumpFlow.add(sample.pumpFlow);
        targetFlow.add(sample.targetFlow);
        temperature.add(sample.temperature);
        targetTemperature.add(sample.targetTemperature);
        weight.add(sample.weight);
        pumpTargetMode.add(static_cast<std::uint8_t>(sample.pumpTargetMode));
        valveOpen.add(sample.valveOpen);
        timeMs.add(sample.elapsedMs);
    }
    return true;
}

bool serializeShotRecord(AutoTuning::ShotRecord const &record, String &json) {
    JsonDocument document(&psramAllocator);
    if (!writeShotRecord(record, document)) {
        return false;
    }
    json = "";
    return serializeJson(document, json) > 0;
}

bool parseShotRecord(JsonVariantConst source, DecodedShotRecord &decoded, String &error) {
    error = "";
    if (!source.is<JsonObjectConst>()) {
        error = "Shot record must be an object";
        return false;
    }
    JsonObjectConst input = source.as<JsonObjectConst>();
    AutoTuning::ShotRecord record;
    record.shotId = text(input["shot_id"]);
    record.machineId = text(input["machine_id"]);
    record.machineAdapter = text(input["machine_adapter"]);
    if (text(input["event_type"]) != "shot_profile" || !input["schema_version"].is<int>() ||
        input["schema_version"].as<int>() != 1 || record.shotId.empty() || record.shotId.size() > MAX_ID_LENGTH ||
        record.machineId.empty() || record.machineId.size() > MAX_ID_LENGTH ||
        !requiredTimestamp(input, "timestamp", record.timestamp)) {
        error = "Shot record envelope is invalid";
        return false;
    }
    bool valid = true;
    record.utility = readBoolOrDefault(input, "utility", false, valid);
    record.excludeFromLocalOptimization = readBoolOrDefault(input, "exclude_from_local_optimization", false, valid);
    record.localOptimizationEnabled = readBoolOrDefault(input, "local_optimization_enabled", false, valid);
    record.communityUploadEnabled = readBoolOrDefault(input, "community_upload_enabled", false, valid);
    record.communityUploadOwner = text(input["community_upload_owner"]);
    record.optimizationWeight =
        input["optimization_weight"].isNull()
            ? 1.0f
            : (finiteNumber(input["optimization_weight"]) ? input["optimization_weight"].as<float>() : 0.0f);
    valid = valid && record.optimizationWeight >= 0.0f && record.optimizationWeight <= 1.0f;
    record.weightSource = text(input["weight_source"]);
    record.flowSource = text(input["flow_source"]);
    record.flowUnits = text(input["flow_units"]);
    record.pumpFlowSource = text(input["pump_flow_source"]);
    record.pumpFlowUnits = text(input["pump_flow_units"]);
    record.pumpFlowInterpretation = text(input["pump_flow_interpretation"]);
    record.pumpFlowCalibrationRequired = readBoolOrDefault(input, "pump_flow_calibration_required", false, valid);
    record.predictiveWeightInterpretation = text(input["predictive_weight_interpretation"]);
    if (!requiredFinite(input, "beverage_out_g", record.beverageOutG) ||
        !requiredFinite(input, "shot_time_s", record.shotTimeS)) {
        error = "Shot output is invalid";
        return false;
    }
    record.beverageOutObservation = text(input["beverage_out_observation"]);
    record.predictiveStopApplied = readBoolOrDefault(input, "predictive_stop_applied", false, valid);
    record.predictiveStopDelayMs = readOptionalFloat(input, "predictive_stop_delay_ms", valid);
    record.predictiveStopRateGPerS = readOptionalFloat(input, "predictive_stop_rate_g_per_s", valid);
    record.predictiveStopLeadG = readOptionalFloat(input, "predictive_stop_lead_g", valid);
    record.predictedFinalBeverageOutG = readOptionalFloat(input, "predicted_final_beverage_out_g", valid);
    record.recipe.beanContextId = text(input["bean_context_id"]);
    record.recipe.beanContextName = text(input["bean_context_name"]);
    if (record.recipe.beanContextId.size() > MAX_CONTEXT_ID_LENGTH ||
        !AutoTuning::parseTasteGoal(input["taste_goal"], record.recipe.tasteGoal, error)) {
        if (error.isEmpty()) {
            error = "Shot recipe context is invalid";
        }
        return false;
    }
    record.recipe.doseTargetG = readOptionalFloat(input, "dose_target_g", valid);
    record.recipe.targetYieldG = readOptionalFloat(input, "target_yield_g", valid);
    record.recipe.targetRatio = readOptionalFloat(input, "target_ratio", valid);
    record.measuredDoseG = readOptionalFloat(input, "dose_in_g", valid);
    record.doseObserved = readBoolOrDefault(input, "dose_observed", false, valid);
    record.doseTargetConfirmed = readBoolOrDefault(input, "dose_target_confirmed", false, valid);
    record.grindFollowed = readOptionalBool(input, "grind_followed", valid);
    record.doseFollowed = readOptionalBool(input, "dose_followed", valid);
    record.yieldFollowed = readOptionalBool(input, "yield_followed", valid);

    AutoTuning::GrinderSnapshot &grinder = record.recipe.grinder;
    grinder.contextId = text(input["grinder_context_id"]);
    grinder.contextName = text(input["grinder_context_name"]);
    grinder.calibrationMode = text(input["grinder_calibration_mode"]);
    grinder.adjustmentMode = text(input["grinder_adjustment_mode"]);
    grinder.stepDirection = text(input["step_direction"]);
    grinder.referenceLabel = text(input["reference_label"]);
    grinder.micronsPerStep = readOptionalFloat(input, "microns_per_step", valid);
    grinder.currentAbsoluteStep = readOptionalFloat(input, "current_absolute_step", valid);
    grinder.absoluteReferenceStep = readOptionalFloat(input, "absolute_reference_step", valid);
    grinder.relativeStepsFromReference = readOptionalFloat(input, "relative_grind_steps_from_reference", valid);
    grinder.relativeMicronsFromReference = readOptionalFloat(input, "relative_grind_um_from_reference", valid);
    grinder.observed = readBoolOrDefault(input, "grind_observed", false, valid);
    if (grinder.contextId.size() > MAX_CONTEXT_ID_LENGTH) {
        valid = false;
    }

    record.profile.id = text(input["profile_id"]);
    record.profile.label = text(input["profile_label"]);
    record.profile.type = text(input["profile_type"]);
    record.profile.rawProfileHash = text(input["raw_profile_hash"]);
    record.profile.available = !record.profile.id.empty();
    record.profile.phaseCount = input["profile_phase_count"] | 0;
    record.profile.temperatureC =
        finiteNumber(input["profile_temperature_c"]) ? input["profile_temperature_c"].as<float>() : 0.0f;
    record.profile.rawProfileAvailable = readBoolOrDefault(input, "raw_profile_available", false, valid);
    record.profile.flowValid = readOptionalBool(input, "profile_flow_valid", valid);
    record.profile.flowMasked = readOptionalBool(input, "profile_flow_masked", valid);

    record.finalPhase.available = !input["final_phase_index"].isNull();
    if (record.finalPhase.available) {
        record.finalPhase.index = input["final_phase_index"] | 0;
        record.finalPhase.name = text(input["final_phase_name"]);
        record.finalPhase.type = text(input["final_phase_type"]);
        record.finalPhase.pumpTarget = text(input["final_pump_target"]);
        record.finalPhase.shotEndState = text(input["shot_end_state"]);
        valid = valid && requiredFinite(input, "final_phase_elapsed_s", record.finalPhase.elapsedS) &&
                requiredFinite(input, "final_phase_temperature_c", record.finalPhase.temperatureC);
        record.finalPhase.targetPressure = readOptionalFloat(input, "final_target_pressure", valid);
        record.finalPhase.targetFlow = readOptionalFloat(input, "final_target_flow", valid);
        if (!input["final_valve_open"].is<bool>()) {
            valid = false;
        } else {
            record.finalPhase.valveOpen = input["final_valve_open"].as<bool>();
        }
    }
    readRecommendationReference(input, record.recommendation, valid);
    if (!valid) {
        error = "Shot metadata contains invalid values";
        return false;
    }

    constexpr const char *ARRAY_KEYS[] = {"pressure",    "target_pressure",    "flow",   "pump_flow",        "target_flow",
                                          "temperature", "target_temperature", "weight", "pump_target_mode", "valve_open",
                                          "time_ms"};
    for (const char *key : ARRAY_KEYS) {
        if (!input[key].is<JsonArrayConst>()) {
            error = "Shot samples are missing";
            return false;
        }
    }
    const size_t sampleCount = input["pressure"].as<JsonArrayConst>().size();
    if (sampleCount == 0 || sampleCount > MAX_SHOT_SAMPLES || !input["n_samples"].is<size_t>() ||
        input["n_samples"].as<size_t>() != sampleCount) {
        error = "Shot sample count is invalid";
        return false;
    }
    for (const char *key : ARRAY_KEYS) {
        if (input[key].as<JsonArrayConst>().size() != sampleCount) {
            error = "Shot sample channels have different lengths";
            return false;
        }
    }

    decoded.samples.clear();
    decoded.samples.reserve(sampleCount);
    std::uint16_t previousTime = 0;
    for (size_t index = 0; index < sampleCount; ++index) {
        AutoTuning::ShotSample sample;
        JsonVariantConst pressure = input["pressure"][index];
        JsonVariantConst targetPressure = input["target_pressure"][index];
        JsonVariantConst flow = input["flow"][index];
        JsonVariantConst pumpFlow = input["pump_flow"][index];
        JsonVariantConst targetFlow = input["target_flow"][index];
        JsonVariantConst temperature = input["temperature"][index];
        JsonVariantConst targetTemperature = input["target_temperature"][index];
        JsonVariantConst weight = input["weight"][index];
        JsonVariantConst mode = input["pump_target_mode"][index];
        JsonVariantConst valve = input["valve_open"][index];
        JsonVariantConst elapsed = input["time_ms"][index];
        std::uint16_t elapsedMs = 0;
        if (!finiteNumber(pressure) || !finiteNumber(targetPressure) || !finiteNumber(flow) || !finiteNumber(pumpFlow) ||
            !finiteNumber(targetFlow) || !finiteNumber(temperature) || !finiteNumber(targetTemperature) ||
            !finiteNumber(weight) || !mode.is<int>() || mode.as<int>() < 0 || mode.as<int>() > 2 || !valve.is<bool>() ||
            !exactUint16(elapsed, elapsedMs)) {
            error = "Shot sample contains invalid values";
            return false;
        }
        sample.pressure = pressure.as<float>();
        sample.targetPressure = targetPressure.as<float>();
        sample.flow = flow.as<float>();
        sample.pumpFlow = pumpFlow.as<float>();
        sample.targetFlow = targetFlow.as<float>();
        sample.temperature = temperature.as<float>();
        sample.targetTemperature = targetTemperature.as<float>();
        sample.weight = weight.as<float>();
        sample.pumpTargetMode = static_cast<AutoTuning::PumpTargetMode>(mode.as<int>());
        sample.valveOpen = valve.as<bool>();
        sample.elapsedMs = elapsedMs;
        if (index > 0 && sample.elapsedMs < previousTime) {
            error = "Shot sample timestamps are not monotonic";
            return false;
        }
        previousTime = sample.elapsedMs;
        decoded.samples.push_back(sample);
    }
    decoded.record = std::move(record);
    decoded.bindSamples();
    return true;
}

bool deserializeShotRecord(String const &json, DecodedShotRecord &decoded, String &error) {
    JsonDocument document(&psramAllocator);
    if (deserializeJson(document, json) || !document.is<JsonObjectConst>()) {
        error = "Shot record JSON is invalid";
        return false;
    }
    return parseShotRecord(document.as<JsonVariantConst>(), decoded, error);
}

bool writeShotCompletion(AutoTuning::ShotCompletion const &completion, JsonDocument &document) {
    if (completion.shotId.empty()) {
        return false;
    }
    document.clear();
    JsonObject output = document.to<JsonObject>();
    output["shot_id"] = completion.shotId.c_str();
    AutoTuning::RecommendationReference const &recommendation = completion.recommendation;
    output["recommendation_id"] = recommendation.recommendationId.c_str();
    output["preference_feedback_required"] = recommendation.preferenceFeedbackRequired;
    output["install_id"] = recommendation.installId.c_str();
    output["optimization_run_id"] = recommendation.optimizationRunId.c_str();
    output["anchor_shot_id"] = recommendation.anchorShotId.c_str();
    output["comparison_mode"] = AutoTuning::comparisonModeKey(recommendation.comparisonMode);
    AutoTuning::writeTasteGoal(recommendation.tasteGoal, output["taste_goal"]);
    output["grind_delta_steps_from_current"] = recommendation.grindDeltaStepsFromCurrent;
    output["next_dose_g"] = recommendation.nextDoseG;
    output["target_yield_g"] = recommendation.targetYieldG;
    output["dose_target_g"] = completion.doseTargetG;
    return true;
}

bool serializeShotCompletion(AutoTuning::ShotCompletion const &completion, String &json) {
    JsonDocument document(&psramAllocator);
    if (!writeShotCompletion(completion, document)) {
        return false;
    }
    json = "";
    return serializeJson(document, json) > 0;
}

bool parseShotCompletion(JsonVariantConst source, AutoTuning::ShotCompletion &completion, String &error) {
    if (!source.is<JsonObjectConst>()) {
        error = "Shot completion must be an object";
        return false;
    }
    JsonObjectConst input = source.as<JsonObjectConst>();
    AutoTuning::ShotCompletion parsed;
    parsed.shotId = text(input["shot_id"]);
    if (parsed.shotId.empty() || parsed.shotId.size() > MAX_ID_LENGTH) {
        error = "Shot completion ID is invalid";
        return false;
    }
    AutoTuning::RecommendationReference &recommendation = parsed.recommendation;
    recommendation.recommendationId = text(input["recommendation_id"]);
    recommendation.installId = text(input["install_id"]);
    recommendation.optimizationRunId = text(input["optimization_run_id"]);
    recommendation.anchorShotId = text(input["anchor_shot_id"]);
    if (recommendation.recommendationId.size() > MAX_ID_LENGTH || recommendation.installId.size() > MAX_CONTEXT_ID_LENGTH ||
        recommendation.optimizationRunId.size() > MAX_ID_LENGTH || recommendation.anchorShotId.size() > MAX_ID_LENGTH) {
        error = "Shot completion recommendation context is invalid";
        return false;
    }
    const std::string mode = text(input["comparison_mode"]);
    if (!mode.empty()) {
        auto parsedMode = AutoTuning::comparisonModeFromKey(mode);
        if (!parsedMode) {
            error = "Shot completion comparison mode is invalid";
            return false;
        }
        recommendation.comparisonMode = *parsedMode;
    }
    bool valid = true;
    recommendation.preferenceFeedbackRequired = readBoolOrDefault(input, "preference_feedback_required", false, valid);
    if (!input["taste_goal"].isNull()) {
        if (!AutoTuning::parseTasteGoal(input["taste_goal"], recommendation.tasteGoal, error)) {
            return false;
        }
    } else {
        JsonDocument legacy;
        const String legacyJson = input["taste_goal_json"].as<String>();
        if (!legacyJson.isEmpty() &&
            (deserializeJson(legacy, legacyJson) ||
             !AutoTuning::parseTasteGoal(legacy.as<JsonVariantConst>(), recommendation.tasteGoal, error))) {
            return false;
        }
    }
    if (!valid || !requiredFinite(input, "grind_delta_steps_from_current", recommendation.grindDeltaStepsFromCurrent) ||
        !requiredFinite(input, "next_dose_g", recommendation.nextDoseG) ||
        !requiredFinite(input, "target_yield_g", recommendation.targetYieldG) ||
        !requiredFinite(input, "dose_target_g", parsed.doseTargetG)) {
        error = "Shot completion targets are invalid";
        return false;
    }
    if (recommendation.preferenceFeedbackRequired &&
        (recommendation.recommendationId.empty() || recommendation.installId.empty() ||
         recommendation.optimizationRunId.empty() || recommendation.anchorShotId.empty() ||
         recommendation.anchorShotId == parsed.shotId || recommendation.comparisonMode == AutoTuning::ComparisonMode::None)) {
        error = "Shot completion preference context is incomplete";
        return false;
    }
    completion = std::move(parsed);
    return true;
}

bool writeRecommendation(AutoTuning::Recommendation const &recommendation, JsonDocument &document) {
    if (recommendation.recommendationId.empty() || recommendation.sourceShotId.empty()) {
        return false;
    }
    document.clear();
    JsonObject output = document.to<JsonObject>();
    output["event_type"] = "recommendation";
    output["schema_version"] = 1;
    output["recommendation_id"] = recommendation.recommendationId.c_str();
    output["shot_id"] = recommendation.sourceShotId.c_str();
    output["source_shot_id"] = recommendation.sourceShotId.c_str();
    output["install_id"] = recommendation.installId.c_str();
    output["machine_id"] = recommendation.machineId.c_str();
    output["bean_context_id"] = recommendation.beanContextId.c_str();
    output["grinder_context_id"] = recommendation.grinderContextId.c_str();
    optionalString(output, "profile_id", recommendation.profileId);
    optionalString(output, "raw_profile_hash", recommendation.rawProfileHash);
    AutoTuning::writeTasteGoal(recommendation.tasteGoal, output["taste_goal"]);
    output["created_at"] = recommendation.createdAt;
    output["updated_at"] = recommendation.updatedAt;
    optionalTimestamp(output, "expires_at", recommendation.expiresAt);
    output["mode"] = AutoTuning::recommendationModeKey(recommendation.mode);
    output["status"] = AutoTuning::recommendationStatusKey(recommendation.status);
    optionalString(output, "reason", recommendation.reason);
    output["optimization_run_id"] = recommendation.optimizationRunId.c_str();
    output["comparison_anchor_shot_id"] = recommendation.comparisonAnchorShotId.c_str();
    output["comparison_mode"] = AutoTuning::comparisonModeKey(recommendation.comparisonMode);
    output["preference_feedback_required"] = recommendation.preferenceFeedbackRequired;
    optionalString(output, "grinder_calibration_mode", recommendation.grinderCalibrationMode);
    optionalString(output, "grinder_adjustment_mode", recommendation.grinderAdjustmentMode);
    optionalString(output, "step_direction", recommendation.stepDirection);
    optionalString(output, "reference_label", recommendation.referenceLabel);
    optionalFloat(output, "microns_per_step", recommendation.micronsPerStep);
    optionalFloat(output, "current_absolute_step", recommendation.currentAbsoluteStep);
    optionalFloat(output, "absolute_reference_step", recommendation.absoluteReferenceStep);
    optionalFloat(output, "projected_absolute_step", recommendation.projectedAbsoluteStep);
    output["grind_delta_steps_from_current"] = recommendation.grindDeltaStepsFromCurrent;
    output["grind_delta_um_from_current"] = recommendation.grindDeltaMicronsFromCurrent;
    output["projected_relative_step_from_reference"] = recommendation.projectedRelativeStepFromReference;
    output["projected_relative_grind_um_from_reference"] = recommendation.projectedRelativeMicronsFromReference;
    output["next_dose_g"] = recommendation.nextDoseG;
    output["target_yield_g"] = recommendation.targetYieldG;
    output["target_ratio"] = recommendation.targetRatio;
    optionalFloat(output, "confidence", recommendation.confidence);
    optionalTimestamp(output, "accepted_at", recommendation.acceptedAt);
    optionalTimestamp(output, "ignored_at", recommendation.ignoredAt);
    optionalTimestamp(output, "edited_at", recommendation.editedAt);
    optionalTimestamp(output, "used_at", recommendation.usedAt);
    optionalTimestamp(output, "superseded_at", recommendation.supersededAt);
    optionalTimestamp(output, "apply_acknowledged_at", recommendation.applyAcknowledgedAt);
    output["shown_count"] = recommendation.shownCount;
    optionalString(output, "apply_status", recommendation.applyStatus);
    optionalString(output, "apply_error", recommendation.applyError);
    return true;
}

bool serializeRecommendation(AutoTuning::Recommendation const &recommendation, String &json) {
    JsonDocument document(&psramAllocator);
    if (!writeRecommendation(recommendation, document)) {
        return false;
    }
    json = "";
    return serializeJson(document, json) > 0;
}

bool parseRecommendation(JsonVariantConst source, AutoTuning::Recommendation &recommendation, String &error) {
    error = "";
    if (!source.is<JsonObjectConst>()) {
        error = "Recommendation must be an object";
        return false;
    }
    JsonObjectConst input = source.as<JsonObjectConst>();
    AutoTuning::Recommendation parsed;
    parsed.recommendationId = text(input["recommendation_id"]);
    parsed.sourceShotId = text(input["source_shot_id"]);
    if (parsed.sourceShotId.empty()) {
        parsed.sourceShotId = text(input["shot_id"]);
    }
    parsed.installId = text(input["install_id"]);
    parsed.machineId = text(input["machine_id"]);
    parsed.beanContextId = text(input["bean_context_id"]);
    parsed.grinderContextId = text(input["grinder_context_id"]);
    parsed.profileId = text(input["profile_id"]);
    parsed.rawProfileHash = text(input["raw_profile_hash"]);
    if (text(input["event_type"]) != "recommendation" || !input["schema_version"].is<int>() ||
        input["schema_version"].as<int>() != 1 || parsed.recommendationId.empty() ||
        parsed.recommendationId.size() > MAX_ID_LENGTH || parsed.sourceShotId.empty() ||
        parsed.sourceShotId.size() > MAX_ID_LENGTH || parsed.installId.empty() ||
        parsed.installId.size() > MAX_CONTEXT_ID_LENGTH || parsed.machineId.empty() || parsed.machineId.size() > MAX_ID_LENGTH ||
        parsed.beanContextId.size() > MAX_CONTEXT_ID_LENGTH || parsed.grinderContextId.size() > MAX_CONTEXT_ID_LENGTH ||
        parsed.profileId.size() > MAX_ID_LENGTH || parsed.rawProfileHash.size() > MAX_ID_LENGTH) {
        error = "Recommendation envelope is invalid";
        return false;
    }
    auto mode = AutoTuning::recommendationModeFromKey(text(input["mode"]));
    auto status = AutoTuning::recommendationStatusFromKey(text(input["status"]));
    auto comparison = AutoTuning::comparisonModeFromKey(text(input["comparison_mode"]));
    if (!mode || !status || !comparison || !input["preference_feedback_required"].is<bool>() ||
        !AutoTuning::parseTasteGoal(input["taste_goal"], parsed.tasteGoal, error)) {
        if (error.isEmpty()) {
            error = "Recommendation mode, status, comparison, or taste goal is invalid";
        }
        return false;
    }
    parsed.mode = *mode;
    parsed.status = *status;
    parsed.comparisonMode = *comparison;
    parsed.preferenceFeedbackRequired = input["preference_feedback_required"].as<bool>();
    parsed.reason = text(input["reason"]);
    parsed.optimizationRunId = text(input["optimization_run_id"]);
    parsed.comparisonAnchorShotId = text(input["comparison_anchor_shot_id"]);
    if (parsed.optimizationRunId.empty() || parsed.optimizationRunId.size() > MAX_ID_LENGTH ||
        parsed.comparisonAnchorShotId.empty() || parsed.comparisonAnchorShotId.size() > MAX_ID_LENGTH) {
        error = "Recommendation context is invalid";
        return false;
    }
    bool valid = true;
    const std::optional<AutoTuning::Timestamp> createdAt = readOptionalTimestamp(input, "created_at", valid);
    const std::optional<AutoTuning::Timestamp> updatedAt = readOptionalTimestamp(input, "updated_at", valid);
    parsed.createdAt = createdAt.value_or(0);
    parsed.updatedAt = updatedAt.value_or(parsed.createdAt);
    valid = valid && parsed.updatedAt >= parsed.createdAt;
    parsed.expiresAt = readOptionalTimestamp(input, "expires_at", valid);
    parsed.grinderCalibrationMode = text(input["grinder_calibration_mode"]);
    parsed.grinderAdjustmentMode = text(input["grinder_adjustment_mode"]);
    parsed.stepDirection = text(input["step_direction"]);
    parsed.referenceLabel = text(input["reference_label"]);
    parsed.micronsPerStep = readOptionalFloat(input, "microns_per_step", valid);
    parsed.currentAbsoluteStep = readOptionalFloat(input, "current_absolute_step", valid);
    parsed.absoluteReferenceStep = readOptionalFloat(input, "absolute_reference_step", valid);
    parsed.projectedAbsoluteStep = readOptionalFloat(input, "projected_absolute_step", valid);
    valid = valid && requiredFinite(input, "grind_delta_steps_from_current", parsed.grindDeltaStepsFromCurrent) &&
            requiredFinite(input, "grind_delta_um_from_current", parsed.grindDeltaMicronsFromCurrent) &&
            requiredFinite(input, "projected_relative_step_from_reference", parsed.projectedRelativeStepFromReference) &&
            requiredFinite(input, "projected_relative_grind_um_from_reference", parsed.projectedRelativeMicronsFromReference) &&
            requiredFinite(input, "next_dose_g", parsed.nextDoseG) &&
            requiredFinite(input, "target_yield_g", parsed.targetYieldG) &&
            requiredFinite(input, "target_ratio", parsed.targetRatio);
    parsed.confidence = readOptionalFloat(input, "confidence", valid);
    parsed.acceptedAt = readOptionalTimestamp(input, "accepted_at", valid);
    parsed.ignoredAt = readOptionalTimestamp(input, "ignored_at", valid);
    parsed.editedAt = readOptionalTimestamp(input, "edited_at", valid);
    parsed.usedAt = readOptionalTimestamp(input, "used_at", valid);
    parsed.supersededAt = readOptionalTimestamp(input, "superseded_at", valid);
    parsed.applyAcknowledgedAt = readOptionalTimestamp(input, "apply_acknowledged_at", valid);
    if (input["shown_count"].isNull()) {
        parsed.shownCount = 0;
    } else if (input["shown_count"].is<int>() && input["shown_count"].as<int>() >= 0) {
        parsed.shownCount = input["shown_count"].as<int>();
    } else {
        valid = false;
    }
    parsed.applyStatus = text(input["apply_status"]);
    parsed.applyError = text(input["apply_error"]);
    const auto inLifecycle = [&parsed](std::optional<AutoTuning::Timestamp> timestamp) {
        return !timestamp.has_value() || ((parsed.createdAt <= 0 || *timestamp >= parsed.createdAt) &&
                                          (parsed.updatedAt <= 0 || *timestamp <= parsed.updatedAt));
    };
    valid = valid && (!parsed.expiresAt.has_value() || parsed.createdAt <= 0 || *parsed.expiresAt >= parsed.createdAt) &&
            inLifecycle(parsed.acceptedAt) && inLifecycle(parsed.ignoredAt) && inLifecycle(parsed.editedAt) &&
            inLifecycle(parsed.usedAt) && inLifecycle(parsed.supersededAt) && inLifecycle(parsed.applyAcknowledgedAt);
    if (!valid) {
        error = "Recommendation contains invalid numeric values";
        return false;
    }
    recommendation = std::move(parsed);
    return true;
}

bool deserializeRecommendation(String const &json, AutoTuning::Recommendation &recommendation, String &error) {
    JsonDocument document(&psramAllocator);
    if (deserializeJson(document, json) || !document.is<JsonObjectConst>()) {
        error = "Recommendation JSON is invalid";
        return false;
    }
    return parseRecommendation(document.as<JsonVariantConst>(), recommendation, error);
}

} // namespace AutoTuningJsonCodec
