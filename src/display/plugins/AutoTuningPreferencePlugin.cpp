#ifndef GAGGIMATE_HEADLESS

#include "AutoTuningPreferencePlugin.h"
#include "../core/AutoTuning.h"
#include "../core/Controller.h"
#include "../core/PluginManager.h"
#include "autotuning/AutoTuningTasteGoalJson.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {
constexpr lv_coord_t CARD_MAX_W = 360;
constexpr lv_coord_t PREFERENCE_CARD_H = 330;
constexpr lv_coord_t RECOMMENDATION_CARD_H = 250;
constexpr lv_coord_t DOSE_CONFIRMATION_CARD_H = 220;

void preferenceButtonCallback(lv_event_t *event);
void doseConfirmationButtonCallback(lv_event_t *event);
void useButtonCallback(lv_event_t *event);
void ignoreButtonCallback(lv_event_t *event);
void laterButtonCallback(lv_event_t *event);

lv_coord_t overlayCardWidth() {
    const lv_coord_t available = LV_HOR_RES - 96;
    return available < CARD_MAX_W ? available : CARD_MAX_W;
}

lv_obj_t *createOverlayCard(lv_obj_t *&overlay, lv_coord_t height) {
    overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(overlay);
    lv_obj_set_size(card, overlayCardWidth(), height);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}
} // namespace

void AutoTuningPreferencePlugin::setup(Controller *ctrl, PluginManager *pm) {
    controller = ctrl;
    pluginManager = pm;

    pm->on("rl:recommendation:received", [this](Event const &event) {
        if (!autoTuningEnabled()) {
            return;
        }
        storeRecommendation(event);
        if (overlayMode != AutoTuningOverlayMode::PREFERENCE && overlayMode != AutoTuningOverlayMode::DOSE_CONFIRMATION &&
            shouldPromptRecommendation()) {
            showRecommendationOverlay();
        }
    });

    pm->on("rl:recommendation:cleared", [this](Event const &) {
        hasPendingRecommendation = false;
        pendingRecommendationId = "";
        pendingRecommendationStatus = "";
        if (overlayMode == AutoTuningOverlayMode::RECOMMENDATION) {
            closeOverlay();
        }
    });

    pm->on("rl:dose-confirmation:required", [this](Event const &event) {
        if (!autoTuningEnabled()) {
            return;
        }
        const String shotId = event.getString("shot_id");
        const float targetG = event.getFloat("dose_target_g");
        if (shotId.isEmpty() || !std::isfinite(targetG) || targetG <= 0.0f) {
            return;
        }
        if (shotId == pendingDoseShotId) {
            return;
        }
        for (Event const &pending : pendingDosePrompts) {
            if (pending.getString("shot_id") == shotId) {
                return;
            }
        }
        if (pendingDoseShotId.isEmpty()) {
            activateDosePrompt(event);
            showDoseConfirmationOverlay();
        } else {
            pendingDosePrompts.push_back(event);
        }
    });

    pm->on("rl:dose-confirmation:resolved", [this](Event const &event) {
        if (event.getString("shot_id") != pendingDoseShotId) {
            return;
        }
        pendingDoseShotId = "";
        pendingDoseTargetG = 0.0f;
        if (overlayMode == AutoTuningOverlayMode::DOSE_CONFIRMATION) {
            clearOverlay(false);
        }
        showNextPendingOverlay();
    });

    pm->on("rl:shot:complete", [this](Event const &event) {
        if (!autoTuningEnabled() || event.getInt("preference_feedback_required") <= 0) {
            return;
        }
        const String shotId = event.getString("shot_id");
        if (shotId == pendingShotId) {
            return;
        }
        for (Event const &pending : pendingPreferencePrompts) {
            if (pending.getString("shot_id") == shotId) {
                return;
            }
        }
        if (!pendingShotId.isEmpty()) {
            pendingPreferencePrompts.push_back(event);
        } else if (activatePreferencePrompt(event)) {
            showPreferenceOverlay();
        }
    });

    pm->on("rl:preference", [this](Event const &event) {
        if (event.getInt("decision_persisted") == 1 && event.getString("install_id") == pendingPreferenceInstallId &&
            event.getString("optimization_run_id") == pendingPreferenceRunId && event.getString("new_shot_id") == pendingShotId &&
            event.getString("anchor_shot_id") == pendingPreferenceAnchorShotId) {
            closeOverlay();
            showNextPendingOverlay();
        }
    });

    pm->on("rl:prompts:invalidated", [this](Event const &event) {
        const String shotId = event.getString("shot_id");
        if (shotId.isEmpty()) {
            hasPendingRecommendation = false;
            pendingRecommendationId = "";
            pendingRecommendationStatus = "";
            pendingDosePrompts.clear();
            pendingPreferencePrompts.clear();
            pendingDoseShotId = "";
            pendingDoseTargetG = 0.0f;
            closeOverlay();
            return;
        }

        if (pendingDoseShotId == shotId) {
            pendingDoseShotId = "";
            pendingDoseTargetG = 0.0f;
            if (overlayMode == AutoTuningOverlayMode::DOSE_CONFIRMATION) {
                clearOverlay(false);
            }
        }
        if (pendingShotId == shotId) {
            clearOverlay(true);
        }
        pendingDosePrompts.erase(
            std::remove_if(pendingDosePrompts.begin(), pendingDosePrompts.end(),
                           [&shotId](Event const &pending) { return pending.getString("shot_id") == shotId; }),
            pendingDosePrompts.end());
        pendingPreferencePrompts.erase(
            std::remove_if(pendingPreferencePrompts.begin(), pendingPreferencePrompts.end(),
                           [&shotId](Event const &pending) { return pending.getString("shot_id") == shotId; }),
            pendingPreferencePrompts.end());
        showNextPendingOverlay();
    });

    pm->on("controller:brew:start", [this](Event const &) {
        if (!pendingPreferenceRunId.isEmpty()) {
            clearOverlay(false);
        } else {
            closeOverlay();
        }
    });
    pm->on("controller:brew:end", [this](Event const &) { showNextPendingOverlay(); });

    auto closeWhenDisabled = [this](Event const &) {
        if (!autoTuningEnabled()) {
            hasPendingRecommendation = false;
            pendingDosePrompts.clear();
            pendingPreferencePrompts.clear();
            pendingDoseShotId = "";
            pendingDoseTargetG = 0.0f;
            closeOverlay();
        }
    };
    pm->on("settings:changed", closeWhenDisabled);
    pm->on("rl:settings:changed", closeWhenDisabled);
}

bool AutoTuningPreferencePlugin::activateDosePrompt(Event const &event) {
    const String shotId = event.getString("shot_id");
    const float targetG = event.getFloat("dose_target_g");
    if (shotId.isEmpty() || !std::isfinite(targetG) || targetG <= 0.0f) {
        return false;
    }
    pendingDoseShotId = shotId;
    pendingDoseTargetG = targetG;
    return true;
}

bool AutoTuningPreferencePlugin::activatePreferencePrompt(Event const &event) {
    AutoTuning::ShotCompletion const *completion = event.getPayload<AutoTuning::ShotCompletion>();
    if (!completion || !completion->recommendation.preferenceFeedbackRequired) {
        return false;
    }
    AutoTuning::RecommendationReference const &recommendation = completion->recommendation;
    const String shotId = completion->shotId.c_str();
    const String installId = recommendation.installId.c_str();
    const String runId = recommendation.optimizationRunId.c_str();
    const String anchorShotId = recommendation.anchorShotId.c_str();
    const String comparisonMode = AutoTuning::comparisonModeKey(recommendation.comparisonMode);
    const bool validMode = comparisonMode == "global_previous" || comparisonMode == "best_incumbent";
    if (shotId.isEmpty() || installId.isEmpty() || runId.isEmpty() || anchorShotId.isEmpty() || anchorShotId == shotId ||
        !validMode) {
        return false;
    }
    pendingShotId = shotId;
    pendingShotRecommendationId = recommendation.recommendationId.c_str();
    pendingPreferenceInstallId = installId;
    pendingPreferenceRunId = runId;
    pendingPreferenceAnchorShotId = anchorShotId;
    pendingPreferenceComparisonMode = comparisonMode;
    pendingPreferenceTasteGoal = recommendation.tasteGoal;
    pendingPreferenceTasteGoalSummary = AutoTuning::tasteGoalSummary(recommendation.tasteGoal);
    if (pendingPreferenceTasteGoalSummary.isEmpty()) {
        pendingPreferenceTasteGoalSummary = "Balanced";
    }
    return true;
}

void AutoTuningPreferencePlugin::showNextPendingOverlay() {
    if (!autoTuningEnabled() || overlayMode != AutoTuningOverlayMode::NONE) {
        return;
    }
    if (!pendingDoseShotId.isEmpty()) {
        showDoseConfirmationOverlay();
        return;
    }
    while (!pendingDosePrompts.empty()) {
        Event event = pendingDosePrompts.front();
        pendingDosePrompts.pop_front();
        if (activateDosePrompt(event)) {
            showDoseConfirmationOverlay();
            return;
        }
    }
    if (!pendingShotId.isEmpty()) {
        showPreferenceOverlay();
        return;
    }
    while (!pendingPreferencePrompts.empty()) {
        Event event = pendingPreferencePrompts.front();
        pendingPreferencePrompts.pop_front();
        if (activatePreferencePrompt(event)) {
            showPreferenceOverlay();
            return;
        }
    }
    if (shouldPromptRecommendation()) {
        showRecommendationOverlay();
    }
}

void AutoTuningPreferencePlugin::storeRecommendation(Event const &event) {
    AutoTuning::Recommendation const *recommendation = event.getPayload<AutoTuning::Recommendation>();
    if (!recommendation) {
        return;
    }
    pendingRecommendationId = recommendation->recommendationId.c_str();
    if (pendingRecommendationId.isEmpty()) {
        return;
    }
    pendingRecommendationStatus = AutoTuning::recommendationStatusKey(recommendation->status);
    pendingProjectedRelativeStepFromReference = recommendation->projectedRelativeStepFromReference;
    pendingCurrentRelativeStepFromReference =
        pendingProjectedRelativeStepFromReference - recommendation->grindDeltaStepsFromCurrent;
    pendingHasCurrentAbsoluteStep = recommendation->currentAbsoluteStep.has_value();
    pendingCurrentAbsoluteStep = recommendation->currentAbsoluteStep.value_or(0.0f);
    pendingHasProjectedAbsoluteStep = recommendation->projectedAbsoluteStep.has_value();
    pendingProjectedAbsoluteStep = recommendation->projectedAbsoluteStep.value_or(0.0f);
    pendingNextDoseG = recommendation->nextDoseG;
    pendingTargetYieldG = recommendation->targetYieldG;
    pendingTargetRatio = recommendation->targetRatio;
    hasPendingRecommendation = true;
}

bool AutoTuningPreferencePlugin::autoTuningEnabled() const {
    return controller &&
           AutoTuning::Router(controller->getSettings().getRLOptimizerConfiguration(), controller->getOptimizerTransport())
               .acceptActionableRecommendations();
}

bool AutoTuningPreferencePlugin::shouldPromptRecommendation() const {
    if (!autoTuningEnabled() || !hasPendingRecommendation || pendingRecommendationId.isEmpty()) {
        return false;
    }
    return pendingRecommendationStatus.isEmpty() || pendingRecommendationStatus == "pending" ||
           pendingRecommendationStatus == "shown";
}

void AutoTuningPreferencePlugin::showDoseConfirmationOverlay() {
    clearOverlay(false);
    overlayMode = AutoTuningOverlayMode::DOSE_CONFIRMATION;
    lv_obj_t *card = createOverlayCard(overlay, DOSE_CONFIRMATION_CARD_H);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text_fmt(title, "Did you use %.1f g?", pendingDoseTargetG);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    lv_obj_t *subtitle = lv_label_create(card);
    lv_label_set_text(subtitle, "Dose was not measured by grind by weight");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xB0B0B0), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 58);

    const char *labels[] = {"Yes", "No"};
    const uint32_t colors[] = {0x2E7D32, 0x3E4A59};
    for (int index = 0; index < 2; index++) {
        lv_obj_t *button = lv_btn_create(card);
        lv_obj_set_size(button, 150, 48);
        lv_obj_align(button, index == 0 ? LV_ALIGN_BOTTOM_LEFT : LV_ALIGN_BOTTOM_RIGHT, index == 0 ? 30 : -30, -30);
        lv_obj_set_style_radius(button, 7, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(colors[index]), 0);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
        lv_obj_add_event_cb(button, doseConfirmationButtonCallback, LV_EVENT_CLICKED, this);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, labels[index]);
        lv_obj_center(label);
    }
}

void AutoTuningPreferencePlugin::showPreferenceOverlay() {
    clearOverlay(false);
    overlayMode = AutoTuningOverlayMode::PREFERENCE;
    lv_obj_t *card = createOverlayCard(overlay, PREFERENCE_CARD_H);
    const lv_coord_t cardWidth = overlayCardWidth();

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Which is closer to your goal?");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *goal = lv_label_create(card);
    String goalText = "Goal: " + pendingPreferenceTasteGoalSummary;
    lv_label_set_text(goal, goalText.c_str());
    lv_label_set_long_mode(goal, LV_LABEL_LONG_DOT);
    lv_obj_set_width(goal, cardWidth - 56);
    lv_obj_set_style_text_align(goal, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(goal, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(goal, LV_ALIGN_TOP_MID, 0, 43);

    lv_obj_t *subtitle = lv_label_create(card);
    lv_label_set_text(subtitle, pendingPreferenceComparisonMode == "best_incumbent" ? "Compare with the current best"
                                                                                    : "Compare with the previous shot");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 70);

    const char *labels[] = {
        "New shot is better",
        "No noticeable difference",
        pendingPreferenceComparisonMode == "best_incumbent" ? "Current best is better" : "Previous shot is better",
    };
    const uint32_t colors[] = {0x2E7D32, 0x3E4A59, 0x7A3E24};
    for (int index = 0; index < 3; index++) {
        lv_obj_t *button = lv_btn_create(card);
        lv_obj_set_size(button, cardWidth - 64, 48);
        lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 102 + index * 58);
        lv_obj_set_style_radius(button, 7, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(colors[index]), 0);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
        lv_obj_add_event_cb(button, preferenceButtonCallback, LV_EVENT_CLICKED, this);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, labels[index]);
        lv_obj_center(label);
    }
}

void AutoTuningPreferencePlugin::showRecommendationOverlay() {
    closeOverlay();
    overlayMode = AutoTuningOverlayMode::RECOMMENDATION;
    lv_obj_t *card = createOverlayCard(overlay, RECOMMENDATION_CARD_H);
    const lv_coord_t cardWidth = overlayCardWidth();

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Next recipe");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    char grindBuffer[64];
    if (pendingHasCurrentAbsoluteStep && pendingHasProjectedAbsoluteStep) {
        snprintf(grindBuffer, sizeof(grindBuffer), "Grind %.1f -> %.1f", pendingCurrentAbsoluteStep,
                 pendingProjectedAbsoluteStep);
    } else {
        snprintf(grindBuffer, sizeof(grindBuffer), "Grind %.1f -> %.1f rel.", pendingCurrentRelativeStepFromReference,
                 pendingProjectedRelativeStepFromReference);
    }

    char recipe[160];
    snprintf(recipe, sizeof(recipe), "%s\nDose %.1fg  Yield %.1fg\nRatio %.2f", grindBuffer, pendingNextDoseG, pendingTargetYieldG,
             pendingTargetRatio);
    lv_obj_t *details = lv_label_create(card);
    lv_label_set_text(details, recipe);
    lv_label_set_long_mode(details, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(details, cardWidth - 56);
    lv_obj_set_style_text_color(details, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_style_text_align(details, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(details, LV_ALIGN_TOP_MID, 0, 52);

    struct ActionButton {
        const char *label;
        lv_align_t alignment;
        lv_coord_t x;
        uint32_t color;
        lv_event_cb_t callback;
    };
    const ActionButton actions[] = {
        {"Use", LV_ALIGN_BOTTOM_LEFT, 24, 0x2E7D32, useButtonCallback},
        {"Later", LV_ALIGN_BOTTOM_MID, 0, 0x333333, laterButtonCallback},
        {"Ignore", LV_ALIGN_BOTTOM_RIGHT, -24, 0x7A2424, ignoreButtonCallback},
    };
    for (const auto &action : actions) {
        lv_obj_t *button = lv_btn_create(card);
        lv_obj_set_size(button, 104, 42);
        lv_obj_align(button, action.alignment, action.x, -22);
        lv_obj_set_style_bg_color(button, lv_color_hex(action.color), 0);
        lv_obj_set_style_radius(button, 8, 0);
        lv_obj_add_event_cb(button, action.callback, LV_EVENT_CLICKED, this);
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, action.label);
        lv_obj_center(label);
    }
}

void AutoTuningPreferencePlugin::clearOverlay(bool clearShotContext) {
    if (overlay) {
        lv_obj_del(overlay);
        overlay = nullptr;
    }
    overlayMode = AutoTuningOverlayMode::NONE;
    if (clearShotContext) {
        pendingShotId = "";
        pendingShotRecommendationId = "";
        pendingPreferenceInstallId = "";
        pendingPreferenceRunId = "";
        pendingPreferenceAnchorShotId = "";
        pendingPreferenceComparisonMode = "";
        pendingPreferenceTasteGoal = AutoTuning::TasteGoal::balanced();
        pendingPreferenceTasteGoalSummary = "Balanced";
    }
}

void AutoTuningPreferencePlugin::closeOverlay() { clearOverlay(true); }

void AutoTuningPreferencePlugin::selectPreference(const String &label) {
    if (pendingShotId.isEmpty() || pendingPreferenceInstallId.isEmpty() || pendingPreferenceRunId.isEmpty() ||
        pendingPreferenceAnchorShotId.isEmpty()) {
        return;
    }
    if (label != "new_better" && label != "anchor_better" && label != "tie") {
        return;
    }

    const auto parsedLabel = AutoTuning::preferenceLabelFromKey(label.c_str());
    const auto parsedMode = AutoTuning::comparisonModeFromKey(pendingPreferenceComparisonMode.c_str());
    if (!parsedLabel || !parsedMode) {
        return;
    }
    AutoTuning::PreferenceFeedback feedback;
    feedback.installId = pendingPreferenceInstallId.c_str();
    feedback.optimizationRunId = pendingPreferenceRunId.c_str();
    feedback.newShotId = pendingShotId.c_str();
    feedback.anchorShotId = pendingPreferenceAnchorShotId.c_str();
    feedback.label = *parsedLabel;
    feedback.comparisonMode = *parsedMode;
    feedback.tasteGoal = pendingPreferenceTasteGoal;
    feedback.recommendationId = pendingShotRecommendationId.c_str();

    Event event;
    event.id = "rl:preference";
    event.setString("install_id", pendingPreferenceInstallId);
    event.setString("optimization_run_id", pendingPreferenceRunId);
    event.setString("new_shot_id", pendingShotId);
    event.setString("anchor_shot_id", pendingPreferenceAnchorShotId);
    event.setString("label", label);
    event.setString("comparison_mode", pendingPreferenceComparisonMode);
    event.setString("recommendation_id", pendingShotRecommendationId);
    event.setPayload(feedback);
    pluginManager->trigger(event);
    if (event.getInt("decision_persisted") == 1) {
        hasPendingRecommendation = false;
    }
}

void AutoTuningPreferencePlugin::selectDoseConfirmation(bool followed) {
    if (!pluginManager || pendingDoseShotId.isEmpty()) {
        return;
    }
    Event event;
    event.id = "rl:dose-confirmation";
    event.setString("shot_id", pendingDoseShotId);
    event.setInt("has_followed", 1);
    event.setInt("followed", followed ? 1 : 0);
    pluginManager->trigger(event);
}

void AutoTuningPreferencePlugin::useRecommendation() {
    if (pendingRecommendationId.isEmpty() || !autoTuningEnabled()) {
        closeOverlay();
        return;
    }
    Event event;
    event.id = "rl:recommendation:apply";
    event.setString("recommendation_id", pendingRecommendationId);
    pluginManager->trigger(event);
    if (event.getInt("decision_persisted") != 1) {
        return;
    }
    hasPendingRecommendation = false;
    closeOverlay();
    showNextPendingOverlay();
}

void AutoTuningPreferencePlugin::ignoreRecommendation() {
    if (pendingRecommendationId.isEmpty() || !autoTuningEnabled()) {
        closeOverlay();
        return;
    }
    Event event;
    event.id = "rl:recommendation:ignore";
    event.setString("recommendation_id", pendingRecommendationId);
    pluginManager->trigger(event);
    if (event.getInt("decision_persisted") != 1) {
        return;
    }
    hasPendingRecommendation = false;
    closeOverlay();
    showNextPendingOverlay();
}

namespace {
void preferenceButtonCallback(lv_event_t *event) {
    auto *plugin = static_cast<AutoTuningPreferencePlugin *>(lv_event_get_user_data(event));
    auto *button = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const int selection = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
    plugin->selectPreference(selection == 0 ? "new_better" : (selection == 1 ? "tie" : "anchor_better"));
}

void doseConfirmationButtonCallback(lv_event_t *event) {
    auto *plugin = static_cast<AutoTuningPreferencePlugin *>(lv_event_get_user_data(event));
    auto *button = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const int selection = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
    plugin->selectDoseConfirmation(selection == 0);
}

void useButtonCallback(lv_event_t *event) {
    static_cast<AutoTuningPreferencePlugin *>(lv_event_get_user_data(event))->useRecommendation();
}

void ignoreButtonCallback(lv_event_t *event) {
    static_cast<AutoTuningPreferencePlugin *>(lv_event_get_user_data(event))->ignoreRecommendation();
}

void laterButtonCallback(lv_event_t *event) {
    static_cast<AutoTuningPreferencePlugin *>(lv_event_get_user_data(event))->closeOverlay();
}
} // namespace

#endif // GAGGIMATE_HEADLESS
