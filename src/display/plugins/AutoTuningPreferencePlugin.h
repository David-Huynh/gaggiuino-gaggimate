#ifndef AUTOTUNINGPREFERENCEPLUGIN_H
#define AUTOTUNINGPREFERENCEPLUGIN_H

#ifndef GAGGIMATE_HEADLESS

#include "../core/Event.h"
#include "../core/Plugin.h"
#include <deque>
#include <display/core/AutoTuningModels.h>
#include <lvgl.h>

enum class AutoTuningOverlayMode { NONE, DOSE_CONFIRMATION, PREFERENCE, RECOMMENDATION };

class AutoTuningPreferencePlugin : public Plugin {
  public:
    void setup(Controller *controller, PluginManager *pluginManager) override;
    void loop() override {}

    void selectPreference(const String &label);
    void selectDoseConfirmation(bool followed);
    void useRecommendation();
    void ignoreRecommendation();
    void closeOverlay();

  private:
    void storeRecommendation(Event const &event);
    void showDoseConfirmationOverlay();
    void showPreferenceOverlay();
    void showRecommendationOverlay();
    bool activateDosePrompt(Event const &event);
    bool activatePreferencePrompt(Event const &event);
    void showNextPendingOverlay();
    bool autoTuningEnabled() const;
    bool shouldPromptRecommendation() const;
    void clearOverlay(bool clearShotContext);

    Controller *controller = nullptr;
    PluginManager *pluginManager = nullptr;
    lv_obj_t *overlay = nullptr;
    AutoTuningOverlayMode overlayMode = AutoTuningOverlayMode::NONE;

    String pendingShotId;
    String pendingDoseShotId;
    float pendingDoseTargetG = 0.0f;
    String pendingShotRecommendationId;
    String pendingPreferenceInstallId;
    String pendingPreferenceRunId;
    String pendingPreferenceAnchorShotId;
    String pendingPreferenceComparisonMode;
    AutoTuning::TasteGoal pendingPreferenceTasteGoal = AutoTuning::TasteGoal::balanced();
    String pendingPreferenceTasteGoalSummary = "Balanced";

    String pendingRecommendationId;
    String pendingRecommendationStatus;
    String pendingStepDirection = "higher_is_finer";
    float pendingGrindDeltaStepsFromCurrent = 0.0f;
    bool pendingHasCurrentAbsoluteStep = false;
    float pendingCurrentAbsoluteStep = 0.0f;
    bool pendingHasProjectedAbsoluteStep = false;
    float pendingProjectedAbsoluteStep = 0.0f;
    float pendingNextDoseG = 0.0f;
    float pendingTargetYieldG = 0.0f;
    float pendingTargetRatio = 0.0f;
    bool hasPendingRecommendation = false;
    std::deque<Event> pendingDosePrompts;
    std::deque<Event> pendingPreferencePrompts;
};

#endif // GAGGIMATE_HEADLESS
#endif // AUTOTUNINGPREFERENCEPLUGIN_H
