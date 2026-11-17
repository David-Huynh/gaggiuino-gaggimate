#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <display/core/Event.h>
#include <display/core/RecipeConfirmation.h>
#include <display/plugins/autotuning/AutoTuningJsonCodec.h>
#include <display/plugins/autotuning/AutoTuningTasteGoalJson.h>
#include <display/util/PsramAllocator.h>

class Settings {
  public:
    String contexts = R"([{"id":"grinder","absolute_reference_step":10,"step_direction":"higher_is_finer","current_relative_step":2,"current_absolute_step":12}])";
    String savedRecommendation;
    long changedAt = 0;
    float dose = 20, persistedDose = 20;
    unsigned saves = 0;
    bool failSave = false;
    bool isRLRecipeStatePersisted() const { return !failSave; }
    String getRLBeanContextId() const { return "bean"; }
    String getRLGrinderContextId() const { return "grinder"; }
    String getRLGrinderContextsJson() const { return contexts; }
    void setRLGrinderContextsJson(String const &value) { contexts = value; }
    long getRLRecipeChangedAt() const { return changedAt; }
    void setRLRecipeChangedAt(long value) { changedAt = value; }
    String getRLSavedRecommendationJson() const { return savedRecommendation; }
    void setRLSavedRecommendationJson(String const &value) { savedRecommendation = value; }
    void save(bool) { persistedDose = dose; ++saves; }
};
namespace AutoTuning {
inline bool activeTasteGoal(Settings const &, TasteGoal &goal, String *) { goal = TasteGoal::balanced(); return true; }
}
class RecipeStateController {
  public:
    Settings settings;
    struct Profiles {
        struct Profile { String id = "profile"; } selected;
        Profile &getSelectedProfile() { return selected; }
    } profiles;
    bool active = false;
    bool isActive() const { return active; }
    Settings &getSettings() { return settings; }
    Profiles *getProfileManager() { return &profiles; }
    void setTargetGrindVolume(float value) { settings.dose = value; }
};
class MQTTPlugin {
  public:
    RecipeStateController *controller;
    struct Plugins { unsigned changes = 0; void trigger(char const *) { ++changes; } } plugins;
    Plugins *pluginManager = &plugins;
    bool hasRecommendation = true;
    AutoTuning::Recommendation latestRecommendation;
    float publishedDose = 0;
    String machineId() const { return "machine"; }
    void publishMachineState(char const *, bool) { publishedDose = controller->settings.dose; }
    void rememberConfirmedRecipe(Event &event);
    void persistRecommendationView();
};
void testRememberedRecipe();
