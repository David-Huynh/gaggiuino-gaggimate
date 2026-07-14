#ifndef AUTOTUNINGTASTEGOALJSON_H
#define AUTOTUNINGTASTEGOALJSON_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <display/core/AutoTuningModels.h>

class Settings;

namespace AutoTuning {

bool parseTasteGoal(JsonVariantConst source, TasteGoal &goal, String &error);
void writeTasteGoal(TasteGoal const &goal, JsonVariant destination);
bool normalizeTasteGoal(JsonVariantConst source, JsonDocument &normalized, String &error);
void setBalancedTasteGoal(JsonDocument &goal);

bool activeTasteGoal(const Settings &settings, TasteGoal &goal, String *error = nullptr);
bool activeTasteGoal(const Settings &settings, JsonDocument &goal, String *error = nullptr);
String activeTasteGoalJson(const Settings &settings);
String tasteGoalSummary(TasteGoal const &goal);
String tasteGoalSummary(JsonVariantConst goal);
String activeTasteGoalSummary(const Settings &settings);

const char *tasteGoalAttributeKey(size_t index);
const char *tasteGoalAttributeLabel(size_t index);
bool setTypedTasteGoalForContext(Settings &settings, const String &beanContextId, const String &grinderContextId,
                                 TasteGoal const &goal, String &error);
bool setTasteGoalForContext(Settings &settings, const String &beanContextId, const String &grinderContextId,
                            JsonVariantConst goal, String &error);
void removeTasteGoalsForBeanContext(Settings &settings, const String &beanContextId);
void removeTasteGoalsForGrinderContext(Settings &settings, const String &grinderContextId);
void clearTasteGoals(Settings &settings);

} // namespace AutoTuning

#endif // AUTOTUNINGTASTEGOALJSON_H
