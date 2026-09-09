#ifndef GAGGIMATE_HEADLESS

#include "AutoTuningPreferencePlugin.h"
#include <display/core/RecipeConfirmation.h>
#include <cstdlib>
#include "../core/AutoTuning.h"
#include "../core/Controller.h"
#include "../core/PluginManager.h"
#include "autotuning/AutoTuningTasteGoalJson.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>

namespace {
constexpr lv_coord_t CARD_MAX_W = 360;
constexpr lv_coord_t PREFERENCE_CARD_H = 448;
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
        const std::uint32_t promptRevision =
            static_cast<std::uint32_t>(std::max<std::int64_t>(
                event.getInt64("prompt_revision"), 0));
        if (shotId.isEmpty() || promptRevision == 0 ||
            !std::isfinite(targetG) || targetG <= 0.0f) {
            return;
        }
        if (shotId == pendingDoseShotId) {
            if (promptRevision > pendingDosePromptRevision &&
                activateDosePrompt(event)) {
                showDoseConfirmationOverlay();
            }
            return;
        }
        for (Event &pending : pendingDosePrompts) {
            if (pending.getString("shot_id") == shotId) {
                if (promptRevision >
                    static_cast<std::uint32_t>(std::max<std::int64_t>(
                        pending.getInt64("prompt_revision"), 0))) {
                    pending = event;
                }
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
        if (event.getInt("persisted") != 1) return;
        if (event.getString("shot_id") != pendingDoseShotId ||
            event.getInt64("prompt_revision") !=
                static_cast<std::int64_t>(pendingDosePromptRevision)) {
            return;
        }
        pendingDoseShotId = "";
        pendingDosePromptRevision = 0;
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
        const std::uint32_t promptRevision =
            static_cast<std::uint32_t>(std::max<std::int64_t>(
                event.getInt64("prompt_revision"), 0));
        if (shotId.isEmpty() || promptRevision == 0) {
            return;
        }
        if (shotId == pendingShotId) {
            if (promptRevision > pendingPreferencePromptRevision &&
                activatePreferencePrompt(event)) {
                showPreferenceOverlay();
            }
            return;
        }
        for (Event &pending : pendingPreferencePrompts) {
            if (pending.getString("shot_id") == shotId) {
                if (promptRevision >
                    static_cast<std::uint32_t>(std::max<std::int64_t>(
                        pending.getInt64("prompt_revision"), 0))) {
                    pending = event;
                }
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
            event.getString("anchor_shot_id") == pendingPreferenceAnchorShotId &&
            event.getInt64("prompt_revision") ==
                static_cast<std::int64_t>(pendingPreferencePromptRevision)) {
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
            pendingDosePromptRevision = 0;
            pendingDoseTargetG = 0.0f;
            closeOverlay();
            return;
        }

        if (pendingDoseShotId == shotId) {
            pendingDoseShotId = "";
            pendingDosePromptRevision = 0;
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
            pendingDosePromptRevision = 0;
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
    const std::uint32_t promptRevision =
        static_cast<std::uint32_t>(std::max<std::int64_t>(
            event.getInt64("prompt_revision"), 0));
    if (shotId.isEmpty() || promptRevision == 0 ||
        !std::isfinite(targetG) || targetG <= 0.0f) {
        return false;
    }
    pendingDoseShotId = shotId;
    pendingDosePromptRevision = promptRevision;
    pendingDoseTargetG = targetG;
    pendingRecipeGrind = event.getInt("has_grind_setting") == 1
        ? std::optional<float>(event.getFloat("grind_setting")) : std::nullopt;
    pendingRecipeAbsolute = event.getInt("grind_is_absolute") == 1;
    editingRecipe = false;
    return true;
}

bool AutoTuningPreferencePlugin::activatePreferencePrompt(Event const &event) {
    AutoTuning::ShotCompletion const *completion = event.getPayload<AutoTuning::ShotCompletion>();
    if (!completion || !completion->preferenceRequest.has_value()) {
        return false;
    }
    AutoTuning::PreferenceRequest const &preference = *completion->preferenceRequest;
    const String shotId = completion->shotId.c_str();
    const String installId = preference.installId.c_str();
    const String runId = preference.optimizationRunId.c_str();
    const String anchorShotId = preference.anchorShotId.c_str();
    const String comparisonMode = AutoTuning::comparisonModeKey(preference.comparisonMode);
    const std::uint32_t promptRevision =
        static_cast<std::uint32_t>(std::max<std::int64_t>(
            event.getInt64("prompt_revision"), 0));
    const bool validMode = comparisonMode == "global_previous" || comparisonMode == "best_incumbent";
    if (shotId.isEmpty() || installId.isEmpty() || runId.isEmpty() || anchorShotId.isEmpty() || anchorShotId == shotId ||
        promptRevision == 0 || !validMode) {
        return false;
    }
    pendingShotId = shotId;
    pendingPreferencePromptRevision = promptRevision;
    pendingShotRecommendationId = preference.recommendationId.c_str();
    pendingPreferenceInstallId = installId;
    pendingPreferenceRunId = runId;
    pendingPreferenceAnchorShotId = anchorShotId;
    pendingPreferenceAnchor = preference.anchor;
    pendingPreferenceComparisonMode = comparisonMode;
    pendingPreferenceTasteGoal = preference.tasteGoal;
    pendingPreferenceTasteGoalSummary = AutoTuning::tasteGoalSummary(preference.tasteGoal);
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
    lv_obj_t *card = createOverlayCard(overlay, std::min<lv_coord_t>(LV_VER_RES - 24, 440));
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    const auto text = [card](const String &value) {
        lv_obj_t *label = lv_label_create(card);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(label, value.c_str());
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
        return label;
    };
    text("Confirm the recipe you used");
    text("Grind: " + (pendingRecipeGrind ? String(*pendingRecipeGrind, 1) : String("unknown")) +
         (pendingRecipeAbsolute ? "" : " steps from reference") + " / Dose: " + String(pendingDoseTargetG, 1) + " g");
    text("Not sure keeps the shot without using its recipe for optimization.");
    recipeGrindInput = recipeDoseInput = recipeKeyboard = nullptr;
    if (editingRecipe) {
        const auto input = [card, &text](const char *name, const String &value) {
            text(name);
            lv_obj_t *field = lv_textarea_create(card);
            lv_obj_set_width(field, LV_PCT(100));
            lv_textarea_set_one_line(field, true);
            lv_textarea_set_accepted_chars(field, "0123456789.-");
            lv_textarea_set_max_length(field, 12);
            lv_textarea_set_text(field, value.c_str());
            return field;
        };
        recipeGrindInput = input(pendingRecipeAbsolute ? "Actual grind setting" : "Actual grind (steps from reference)",
                                 pendingRecipeGrind ? String(*pendingRecipeGrind, 1) : String(""));
        recipeDoseInput = input("Actual dose (g)", String(pendingDoseTargetG, 1));
        recipeKeyboard = lv_keyboard_create(card);
        lv_obj_set_width(recipeKeyboard, LV_PCT(100));
        lv_obj_set_height(recipeKeyboard, 120);
        lv_keyboard_set_mode(recipeKeyboard, LV_KEYBOARD_MODE_NUMBER);
        lv_keyboard_set_textarea(recipeKeyboard, recipeGrindInput);
        for (lv_obj_t *field : {recipeGrindInput, recipeDoseInput}) {
            lv_obj_add_event_cb(field, [](lv_event_t *event) {
                auto *keyboard = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
                lv_keyboard_set_textarea(keyboard, static_cast<lv_obj_t *>(lv_event_get_target(event)));
            }, LV_EVENT_FOCUSED, recipeKeyboard);
        }
    }
    recipeError = text("");
    const char *labels[] = {editingRecipe ? "Save recipe" : "Yes", "Change values", "Not sure"};
    for (int index = 0; index < 3; ++index) {
        if (editingRecipe && index == 1) continue;
        lv_obj_t *button = lv_btn_create(card);
        lv_obj_set_size(button, LV_PCT(100), 44);
        if (index == 0 && !editingRecipe && !pendingRecipeGrind) lv_obj_add_state(button, LV_STATE_DISABLED);
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
    lv_obj_t *card = createOverlayCard(overlay, std::min<lv_coord_t>(PREFERENCE_CARD_H, LV_VER_RES - 32));
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_style_pad_row(card, 10, 0);
    const auto addText = [card](const String &text) {
        lv_obj_t *label = lv_label_create(card);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(label, text.c_str());
        lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    };
    addText("Which is closer to your goal?");
    addText("Goal: " + pendingPreferenceTasteGoalSummary);
    String reference = "Reference: " + pendingPreferenceAnchorShotId;
    if (pendingPreferenceAnchor) {
        const auto &anchor = *pendingPreferenceAnchor;
        const time_t epoch = static_cast<time_t>(anchor.timestamp);
        struct tm utc{};
        char date[40] = "Date unavailable";
        if (anchor.timestamp > 0 && gmtime_r(&epoch, &utc)) strftime(date, sizeof(date), "%Y-%m-%d %H:%M UTC", &utc);
        char recipe[120];
        snprintf(recipe, sizeof(recipe), "\nGrind %.1f%s; dose %.1fg\nTarget %.1fg",
                 anchor.absoluteStep.value_or(anchor.relativeGrindSteps), anchor.absoluteStep ? "" : " rel.",
                 anchor.doseG, anchor.targetYieldG);
        reference = String(date) + recipe;
        if (anchor.beverageOutG) reference += " / actual " + String(*anchor.beverageOutG, 1) + "g";
        if (!anchor.profileLabel.empty()) reference += "\n" + String(anchor.profileLabel.c_str());
        reference += "\nID: " + pendingPreferenceAnchorShotId;
    } else {
        reference += "\nRecipe details unavailable";
    }
    addText(reference);

    const char *labels[] = {
        "New shot is closer",
        "No noticeable difference",
        "Reference shot is closer",
        "Can't compare / don't remember",
    };
    const uint32_t colors[] = {0x2E7D32, 0x3E4A59, 0x7A3E24, 0x333333};
    for (int index = 0; index < 4; index++) {
        lv_obj_t *button = lv_btn_create(card);
        lv_obj_set_size(button, LV_PCT(100), 48);
        lv_obj_set_style_radius(button, 7, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(colors[index]), 0);
        lv_obj_set_user_data(button, reinterpret_cast<void *>(static_cast<intptr_t>(index)));
        lv_obj_add_event_cb(button, preferenceButtonCallback, LV_EVENT_CLICKED, this);
        lv_obj_t *label = lv_label_create(button);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
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
        pendingPreferencePromptRevision = 0;
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
        pendingPreferenceAnchorShotId.isEmpty() ||
        pendingPreferencePromptRevision == 0) {
        return;
    }
    const auto parsedLabel = AutoTuning::preferenceLabelFromKey(label.c_str());
    const auto parsedMode = AutoTuning::comparisonModeFromKey(pendingPreferenceComparisonMode.c_str());
    if (!parsedLabel || !parsedMode) {
        return;
    }
    const String shotId = pendingShotId;
    const std::uint32_t promptRevision = pendingPreferencePromptRevision;
    Event claim;
    claim.id = "rl:prompt:claim";
    claim.setString("shot_id", shotId);
    claim.setInt64("prompt_revision", promptRevision);
    pluginManager->trigger(claim);
    if (claim.getInt("claimed") != 1) {
        return;
    }
    AutoTuning::PreferenceFeedback feedback(*parsedLabel);
    feedback.installId = pendingPreferenceInstallId.c_str();
    feedback.optimizationRunId = pendingPreferenceRunId.c_str();
    feedback.newShotId = pendingShotId.c_str();
    feedback.anchorShotId = pendingPreferenceAnchorShotId.c_str();
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
    event.setInt64("prompt_revision", promptRevision);
    event.setInt("prompt_claimed", 1);
    event.setPayload(feedback);
    pluginManager->trigger(event);
    if (event.getInt("decision_persisted") == 1) {
        hasPendingRecommendation = false;
    } else {
        Event release;
        release.id = "rl:prompt:release";
        release.setString("shot_id", shotId);
        release.setInt64("prompt_revision", promptRevision);
        pluginManager->trigger(release);
    }
}

void AutoTuningPreferencePlugin::selectDoseConfirmation(int selection) {
    if (selection == 1) {
        editingRecipe = true;
        showDoseConfirmationOverlay();
        return;
    }
    AutoTuning::RecipeConfirmation answer;
    answer.answer = selection == 2 ? AutoTuning::RecipeAnswer::Unknown
        : editingRecipe ? AutoTuning::RecipeAnswer::Change : AutoTuning::RecipeAnswer::Confirm;
    if (answer.answer == AutoTuning::RecipeAnswer::Change) {
        const auto number = [](lv_obj_t *field) -> std::optional<float> {
            if (!field) return {};
            const char *value = lv_textarea_get_text(field);
            char *end = nullptr;
            const float result = std::strtof(value, &end);
            return end != value && *end == '\0' && std::isfinite(result) ? std::optional<float>(result) : std::nullopt;
        };
        answer.grindSetting = number(recipeGrindInput);
        answer.doseG = number(recipeDoseInput);
        if (!answer.grindSetting || !answer.doseG || std::fabs(*answer.grindSetting) > 10000 ||
            *answer.doseG < 0.1f || *answer.doseG > 100) {
            lv_label_set_text(recipeError, "Enter a valid grind and dose (0.1-100 g).");
            return;
        }
    }
    if (!pluginManager || pendingDoseShotId.isEmpty() ||
        pendingDosePromptRevision == 0) {
        return;
    }
    Event claim;
    claim.id = "rl:prompt:claim";
    claim.setString("shot_id", pendingDoseShotId);
    claim.setInt64("prompt_revision", pendingDosePromptRevision);
    pluginManager->trigger(claim);
    if (claim.getInt("claimed") != 1) {
        return;
    }
    Event event;
    event.id = "rl:dose-confirmation";
    event.setString("shot_id", pendingDoseShotId);
    event.setInt64("prompt_revision", pendingDosePromptRevision);
    event.setInt("prompt_claimed", 1);
    event.setPayload(answer);
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
    static constexpr const char *actions[] = {"new_better", "tie", "anchor_better", "abstain"};
    if (selection >= 0 && selection < 4) plugin->selectPreference(actions[selection]);
}

void doseConfirmationButtonCallback(lv_event_t *event) {
    auto *plugin = static_cast<AutoTuningPreferencePlugin *>(lv_event_get_user_data(event));
    auto *button = static_cast<lv_obj_t *>(lv_event_get_target(event));
    const int selection = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
    plugin->selectDoseConfirmation(selection);
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
