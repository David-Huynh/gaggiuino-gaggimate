#pragma once

#include <display/core/Event.h>
#include <display/core/RecipeConfirmation.h>

// Copy only the values needed by the UI across the storage-worker boundary.
// Keeping a ShotRecord here would put its large context on loopTask's stack.
struct RecipePrompt {
    float doseG = 0.0f;
    std::optional<float> grindSetting;
    bool grindIsAbsolute = false;
    bool doseMeasured = false;

    static RecipePrompt fromShot(AutoTuning::ShotRecord const &shot, float fallbackDoseG) {
        return {AutoTuning::displayedDose(shot).value_or(fallbackDoseG),
                AutoTuning::displayedGrind(shot), AutoTuning::usesAbsoluteGrind(shot),
                shot.doseObserved};
    }

    void writeTo(Event &event) const {
        event.setFloat("dose_target_g", doseG);
        event.setInt("has_grind_setting", grindSetting.has_value());
        if (grindSetting) event.setFloat("grind_setting", *grindSetting);
        event.setInt("grind_is_absolute", grindIsAbsolute);
        event.setInt("dose_measured", doseMeasured);
    }
};

static_assert(sizeof(RecipePrompt) <= 24, "Recipe prompts must stay small enough for loopTask");
