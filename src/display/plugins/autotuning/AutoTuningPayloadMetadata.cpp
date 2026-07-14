#include "AutoTuningPayloadMetadata.h"

#include "AutoTuningTasteGoalJson.h"

#include <cmath>
#include <display/core/Controller.h>
#include <display/core/ProfileManager.h>
#include <display/core/Settings.h>
#include <display/core/process/BrewProcess.h>
#include <display/models/profile.h>
#include <display/util/PsramAllocator.h>
#include <mutex>

namespace {

bool jsonNumber(JsonVariantConst value) {
    return !value.is<bool>() &&
           (value.is<int>() || value.is<long>() || value.is<long long>() || value.is<float>() || value.is<double>());
}

bool finiteNumber(JsonVariantConst value) { return jsonNumber(value) && std::isfinite(value.as<double>()); }

bool positiveFiniteNumber(JsonVariantConst value) { return finiteNumber(value) && value.as<double>() > 0.0; }

String normalizedStepDirection(JsonVariantConst value) {
    String direction = value.as<String>();
    direction.trim();
    return direction == "higher_is_coarser" ? direction : "higher_is_finer";
}

String normalizedGrinderAdjustmentMode(JsonVariantConst value) {
    String mode = value.as<String>();
    mode.trim();
    return mode == "stepless" ? mode : "stepped";
}

JsonObjectConst findContext(JsonArrayConst contexts, const String &contextId) {
    for (JsonObjectConst context : contexts) {
        if (context["id"].as<String>() == contextId) {
            return context;
        }
    }
    return JsonObjectConst();
}

String grinderCalibrationMode(JsonObjectConst context) {
    String mode = context["grinder_calibration_mode"].as<String>();
    if (mode == "absolute_display_calibrated" || mode == "relative_calibrated" || mode == "uncalibrated") {
        return mode;
    }
    if (positiveFiniteNumber(context["microns_per_step"])) {
        return finiteNumber(context["current_absolute_step"]) && finiteNumber(context["absolute_reference_step"])
                   ? "absolute_display_calibrated"
                   : "relative_calibrated";
    }
    return "uncalibrated";
}

void optionalFloat(JsonDocument &document, const char *key, std::optional<float> value) {
    if (value.has_value()) {
        document[key] = *value;
    }
}

} // namespace

namespace AutoTuningPayloadMetadata {

void captureRecipe(Controller *controller, AutoTuning::RecipeSnapshot &recipe) {
    recipe = AutoTuning::RecipeSnapshot{};
    if (!controller) {
        return;
    }

    Settings const &settings = controller->getSettings();
    const float dose = static_cast<float>(settings.getTargetGrindVolume());
    const float targetYield =
        controller->getProfileManager() ? controller->getProfileManager()->getSelectedProfile().getTotalVolume() : 0.0f;
    const String grinderId = settings.getRLGrinderContextId();
    const String grinderName = settings.getRLGrinderContextName();

    recipe.beanContextId = settings.getRLBeanContextId().c_str();
    recipe.beanContextName = settings.getRLBeanContextName().c_str();
    AutoTuning::activeTasteGoal(settings, recipe.tasteGoal);
    recipe.grinder.contextId = grinderId.c_str();
    recipe.grinder.contextName = grinderName.c_str();
    if (std::isfinite(dose) && dose > 0.0f) {
        recipe.doseTargetG = dose;
    }
    if (std::isfinite(targetYield) && targetYield > 0.0f) {
        recipe.targetYieldG = targetYield;
    }
    if (recipe.doseTargetG.has_value() && recipe.targetYieldG.has_value()) {
        recipe.targetRatio = targetYield / dose;
    }
    if (grinderId.isEmpty()) {
        return;
    }

    JsonDocument contextsDoc(&psramAllocator);
    if (deserializeJson(contextsDoc, settings.getRLGrinderContextsJson()) || !contextsDoc.is<JsonArray>()) {
        return;
    }
    JsonObjectConst context = findContext(contextsDoc.as<JsonArrayConst>(), grinderId);
    if (context.isNull()) {
        return;
    }

    AutoTuning::GrinderSnapshot &grinder = recipe.grinder;
    grinder.calibrationMode = grinderCalibrationMode(context).c_str();
    grinder.stepDirection = normalizedStepDirection(context["step_direction"]).c_str();
    grinder.adjustmentMode = normalizedGrinderAdjustmentMode(context["grinder_adjustment_mode"]).c_str();
    grinder.referenceLabel = context["reference_label"].as<String>().c_str();
    if (!positiveFiniteNumber(context["microns_per_step"])) {
        return;
    }

    const float micronsPerStep = context["microns_per_step"].as<float>();
    const float directionSign = grinder.stepDirection == "higher_is_finer" ? 1.0f : -1.0f;
    std::optional<float> relativeSteps;
    if (finiteNumber(context["current_relative_step"])) {
        relativeSteps = context["current_relative_step"].as<float>();
    }
    if (finiteNumber(context["current_absolute_step"]) && finiteNumber(context["absolute_reference_step"])) {
        grinder.currentAbsoluteStep = context["current_absolute_step"].as<float>();
        grinder.absoluteReferenceStep = context["absolute_reference_step"].as<float>();
        relativeSteps = *grinder.currentAbsoluteStep - *grinder.absoluteReferenceStep;
    }
    grinder.micronsPerStep = micronsPerStep;
    if (relativeSteps.has_value()) {
        grinder.relativeStepsFromReference = *relativeSteps;
        grinder.relativeMicronsFromReference = *relativeSteps * micronsPerStep * directionSign;
    }
    grinder.observed = grinder.relativeStepsFromReference.has_value() ||
                       (grinder.currentAbsoluteStep.has_value() && grinder.absoluteReferenceStep.has_value());
}

void captureProfile(Controller *controller, AutoTuning::ProfileSnapshot &snapshot) {
    snapshot = AutoTuning::ProfileSnapshot{};
    if (!controller) {
        return;
    }

    std::lock_guard<std::recursive_mutex> guard(controller->getProcessLock());
    const Profile *profile = nullptr;
    Process *lastProcess = controller->getLastProcess();
    if (lastProcess && lastProcess->getType() == MODE_BREW) {
        profile = &static_cast<BrewProcess *>(lastProcess)->profile;
    } else if (controller->getProfileManager()) {
        profile = &controller->getProfileManager()->getSelectedProfile();
    }
    if (!profile) {
        return;
    }
    snapshot.id = profile->id.c_str();
    snapshot.label = profile->label.c_str();
    snapshot.type = profile->type.c_str();
    snapshot.phaseCount = profile->phases.size();
    snapshot.temperatureC = profile->temperature;
    snapshot.available = true;
}

void addRecipe(Controller *controller, JsonDocument &document) {
    AutoTuning::RecipeSnapshot recipe;
    captureRecipe(controller, recipe);
    document["bean_context_id"] = recipe.beanContextId.c_str();
    document["bean_context_name"] = recipe.beanContextName.c_str();
    AutoTuning::writeTasteGoal(recipe.tasteGoal, document["taste_goal"].to<JsonObject>());
    if (!recipe.grinder.contextId.empty()) {
        document["grinder_context_id"] = recipe.grinder.contextId.c_str();
    }
    if (!recipe.grinder.contextName.empty()) {
        document["grinder_context_name"] = recipe.grinder.contextName.c_str();
    }
    optionalFloat(document, "dose_in_g", recipe.doseTargetG);
    optionalFloat(document, "target_yield_g", recipe.targetYieldG);
    optionalFloat(document, "target_ratio", recipe.targetRatio);

    AutoTuning::GrinderSnapshot const &grinder = recipe.grinder;
    if (grinder.contextId.empty()) {
        return;
    }
    document["grinder_calibration_mode"] = grinder.calibrationMode.c_str();
    document["step_direction"] = grinder.stepDirection.c_str();
    document["grinder_adjustment_mode"] = grinder.adjustmentMode.c_str();
    document["reference_label"] = grinder.referenceLabel.c_str();
    optionalFloat(document, "microns_per_step", grinder.micronsPerStep);
    optionalFloat(document, "current_absolute_step", grinder.currentAbsoluteStep);
    optionalFloat(document, "absolute_reference_step", grinder.absoluteReferenceStep);
    optionalFloat(document, "relative_grind_steps_from_reference", grinder.relativeStepsFromReference);
    optionalFloat(document, "relative_grind_um_from_reference", grinder.relativeMicronsFromReference);
}

void addProfile(Controller *controller, JsonDocument &document) {
    AutoTuning::ProfileSnapshot profile;
    captureProfile(controller, profile);
    if (!profile.available) {
        return;
    }
    document["profile_id"] = profile.id.c_str();
    document["profile_label"] = profile.label.c_str();
    document["profile_type"] = profile.type.c_str();
    document["profile_phase_count"] = profile.phaseCount;
    document["profile_temperature_c"] = profile.temperatureC;
}

} // namespace AutoTuningPayloadMetadata
