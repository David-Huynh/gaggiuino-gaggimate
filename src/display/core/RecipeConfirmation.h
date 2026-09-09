#pragma once
#include "AutoTuningModels.h"
#include <cmath>

namespace AutoTuning {
enum class RecipeAnswer { Confirm, Change, Unknown };
struct RecipeConfirmation {
    RecipeAnswer answer = RecipeAnswer::Unknown;
    std::optional<float> grindSetting;
    std::optional<float> doseG;
};

inline bool usesAbsoluteGrind(ShotRecord const &shot) {
    return shot.recipe.grinder.absoluteReferenceStep.has_value() &&
           shot.recipe.grinder.currentAbsoluteStep.has_value();
}
inline std::optional<float> displayedGrind(ShotRecord const &shot) {
    return usesAbsoluteGrind(shot) ? shot.recipe.grinder.currentAbsoluteStep
                                   : shot.recipe.grinder.relativeStepsFromReference;
}
inline std::optional<float> displayedDose(ShotRecord const &shot) {
    return shot.doseObserved ? shot.measuredDoseG : shot.recipe.doseTargetG;
}

// Validate every value before changing the durable record. "Observed" means
// known recipe input, including a user's explicit report, not sensor verification.
inline bool confirmRecipe(ShotRecord &shot, RecipeConfirmation const &answer) {
    if (answer.answer == RecipeAnswer::Unknown) {
        shot.recipe.grinder.observed = false;
        shot.doseTargetConfirmed = false;
        shot.grindFollowed.reset();
        shot.doseFollowed.reset();
        return true;
    }
    const auto grind = answer.answer == RecipeAnswer::Confirm ? displayedGrind(shot) : answer.grindSetting;
    const auto dose = answer.answer == RecipeAnswer::Confirm ? displayedDose(shot) : answer.doseG;
    if (!grind || !dose || !std::isfinite(*grind) || std::fabs(*grind) > 10000 ||
        !std::isfinite(*dose) || *dose < 0.1f || *dose > 100) return false;
    const float relative = usesAbsoluteGrind(shot) ? *grind - *shot.recipe.grinder.absoluteReferenceStep : *grind;
    if (!std::isfinite(relative) || std::fabs(relative) > 10000) return false;
    auto &snapshot = shot.recipe.grinder;
    snapshot.relativeStepsFromReference = relative;
    if (usesAbsoluteGrind(shot)) snapshot.currentAbsoluteStep = *grind;
    if (snapshot.micronsPerStep) {
        snapshot.relativeMicronsFromReference = relative * *snapshot.micronsPerStep *
            (snapshot.stepDirection == "higher_is_coarser" ? -1 : 1);
    }
    snapshot.observed = true;
    if (!shot.doseObserved || !shot.measuredDoseG || std::fabs(*dose - *shot.measuredDoseG) > 0.0001f) {
        shot.recipe.doseTargetG = *dose;
        shot.measuredDoseG = *dose;
        shot.doseObserved = false;
        shot.doseTargetConfirmed = true;
    }
    if (shot.recommendation.present()) {
        shot.grindFollowed = std::fabs(relative - shot.recommendation.projectedRelativeStepFromReference) <= 0.5f;
        shot.doseFollowed = std::fabs(*dose - shot.recommendation.nextDoseG) <= DOSE_FOLLOW_THROUGH_TOLERANCE_G;
    }
    if (shot.recipe.targetYieldG) shot.recipe.targetRatio = *shot.recipe.targetYieldG / *dose;
    return true;
}
} // namespace AutoTuning
