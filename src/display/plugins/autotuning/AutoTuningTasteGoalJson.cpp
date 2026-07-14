#include "AutoTuningTasteGoalJson.h"

#include <display/core/Settings.h>

namespace AutoTuning {
namespace {

constexpr size_t MAX_STORED_GOALS_JSON_BYTES = 3500;

constexpr const char *ATTRIBUTE_LABELS[] = {
    "Fruity",         "Citrus",
    "Floral",         "Sweet",
    "Nutty / cocoa",  "Roasted",
    "Spice",          "Fermented",
    "Sour",           "Green / vegetative",
    "Bitter",         "Astringent / harsh",
    "Papery / stale", "Salty",
};
static_assert(sizeof(ATTRIBUTE_LABELS) / sizeof(ATTRIBUTE_LABELS[0]) == TASTE_GOAL_ATTRIBUTE_COUNT);

JsonObject findStoredGoal(JsonArray entries, const String &beanContextId, const String &grinderContextId) {
    for (JsonObject entry : entries) {
        if (entry["b"].as<String>() == beanContextId && entry["g"].as<String>() == grinderContextId) {
            return entry;
        }
    }
    return JsonObject();
}

void removeStoredGoal(JsonArray entries, const String &beanContextId, const String &grinderContextId) {
    for (size_t index = entries.size(); index > 0; --index) {
        JsonObject entry = entries[index - 1];
        if (entry["b"].as<String>() == beanContextId && entry["g"].as<String>() == grinderContextId) {
            entries.remove(index - 1);
        }
    }
}

void removeStoredGoals(Settings &settings, const String &contextId, bool matchBean) {
    if (contextId.isEmpty()) {
        return;
    }
    JsonDocument entriesDoc;
    if (deserializeJson(entriesDoc, settings.getRLTasteGoalsJson()) || !entriesDoc.is<JsonArray>()) {
        settings.setRLTasteGoalsJson("[]");
        return;
    }
    JsonArray entries = entriesDoc.as<JsonArray>();
    bool changed = false;
    for (size_t index = entries.size(); index > 0; --index) {
        JsonObject entry = entries[index - 1];
        const String storedId = entry[matchBean ? "b" : "g"].as<String>();
        if (storedId == contextId) {
            entries.remove(index - 1);
            changed = true;
        }
    }
    if (changed) {
        String encoded;
        serializeJson(entriesDoc, encoded);
        settings.setRLTasteGoalsJson(encoded);
    }
}

} // namespace

bool parseTasteGoal(JsonVariantConst source, TasteGoal &goal, String &error) {
    error = "";
    if (!source.is<JsonObjectConst>()) {
        error = "Taste goal must be an object";
        return false;
    }
    JsonObjectConst input = source.as<JsonObjectConst>();
    if (input.size() != 3) {
        error = "Taste goal fields are invalid";
        return false;
    }
    if (!input["schema_version"].is<int>() || input["schema_version"].as<int>() != TASTE_GOAL_SCHEMA_VERSION) {
        error = "Taste goal version is unsupported";
        return false;
    }

    const String mode = input["mode"].as<String>();
    TasteGoal parsed;
    if (mode == "balanced") {
        parsed.mode = TasteGoalMode::Balanced;
    } else if (mode == "custom") {
        parsed.mode = TasteGoalMode::Custom;
    } else {
        error = "Taste goal mode is invalid";
        return false;
    }

    if (!input["targets"].is<JsonObjectConst>()) {
        error = "Taste goal targets must be an object";
        return false;
    }
    for (JsonPairConst pair : input["targets"].as<JsonObjectConst>()) {
        const auto attribute = tasteAttributeFromKey(pair.key().c_str());
        if (!attribute || !pair.value().is<const char *>()) {
            error = "Taste goal targets contain invalid values";
            return false;
        }
        const auto level = tasteLevelFromKey(pair.value().as<const char *>());
        if (!level) {
            error = "Taste goal targets contain invalid values";
            return false;
        }
        parsed.targets[static_cast<size_t>(*attribute)] = *level;
    }
    if (!parsed.valid()) {
        error = parsed.mode == TasteGoalMode::Balanced ? "Balanced taste goal cannot contain targets"
                                                       : "Custom taste goal requires a target";
        return false;
    }
    goal = parsed;
    return true;
}

void writeTasteGoal(TasteGoal const &goal, JsonObject destination) {
    destination.clear();
    destination["schema_version"] = TASTE_GOAL_SCHEMA_VERSION;
    destination["mode"] = goal.mode == TasteGoalMode::Custom ? "custom" : "balanced";
    JsonObject targets = destination["targets"].to<JsonObject>();
    if (goal.mode != TasteGoalMode::Custom) {
        return;
    }
    for (size_t index = 0; index < goal.targets.size(); ++index) {
        const TasteLevel level = goal.targets[index];
        if (level != TasteLevel::Unspecified) {
            targets[tasteAttributeKey(static_cast<TasteAttribute>(index))] = tasteLevelKey(level);
        }
    }
}

bool normalizeTasteGoal(JsonVariantConst source, JsonDocument &normalized, String &error) {
    TasteGoal goal;
    if (!parseTasteGoal(source, goal, error)) {
        return false;
    }
    normalized.clear();
    writeTasteGoal(goal, normalized.to<JsonObject>());
    return true;
}

void setBalancedTasteGoal(JsonDocument &goal) {
    goal.clear();
    writeTasteGoal(TasteGoal::balanced(), goal.to<JsonObject>());
}

bool activeTasteGoal(const Settings &settings, TasteGoal &goal, String *error) {
    const String beanContextId = settings.getRLBeanContextId();
    const String grinderContextId = settings.getRLGrinderContextId();
    if (beanContextId.isEmpty() || grinderContextId.isEmpty()) {
        goal = TasteGoal::balanced();
        return true;
    }

    JsonDocument entriesDoc;
    if (deserializeJson(entriesDoc, settings.getRLTasteGoalsJson()) || !entriesDoc.is<JsonArray>()) {
        goal = TasteGoal::balanced();
        if (error) {
            *error = "Stored taste goals are invalid";
        }
        return false;
    }
    JsonObject entry = findStoredGoal(entriesDoc.as<JsonArray>(), beanContextId, grinderContextId);
    if (entry.isNull()) {
        goal = TasteGoal::balanced();
        return true;
    }
    String validationError;
    if (!parseTasteGoal(entry["v"], goal, validationError)) {
        goal = TasteGoal::balanced();
        if (error) {
            *error = validationError;
        }
        return false;
    }
    return true;
}

bool activeTasteGoal(const Settings &settings, JsonDocument &goal, String *error) {
    TasteGoal typedGoal;
    const bool valid = activeTasteGoal(settings, typedGoal, error);
    goal.clear();
    writeTasteGoal(typedGoal, goal.to<JsonObject>());
    return valid;
}

String activeTasteGoalJson(const Settings &settings) {
    JsonDocument goal;
    activeTasteGoal(settings, goal);
    String encoded;
    serializeJson(goal, encoded);
    return encoded;
}

String tasteGoalSummary(TasteGoal const &goal) {
    if (!goal.valid() || goal.mode == TasteGoalMode::Balanced) {
        return "Balanced";
    }
    String summary;
    for (size_t index = 0; index < goal.targets.size(); ++index) {
        const TasteLevel level = goal.targets[index];
        if (level == TasteLevel::Unspecified) {
            continue;
        }
        if (!summary.isEmpty()) {
            summary += ", ";
        }
        summary += ATTRIBUTE_LABELS[index];
        summary += " ";
        summary += tasteLevelKey(level);
    }
    return summary;
}

String tasteGoalSummary(JsonVariantConst source) {
    TasteGoal goal;
    String error;
    return parseTasteGoal(source, goal, error) ? tasteGoalSummary(goal) : String("Balanced");
}

String activeTasteGoalSummary(const Settings &settings) {
    TasteGoal goal;
    activeTasteGoal(settings, goal);
    return tasteGoalSummary(goal);
}

const char *tasteGoalAttributeKey(const size_t index) {
    return index < TASTE_GOAL_ATTRIBUTE_COUNT ? tasteAttributeKey(static_cast<TasteAttribute>(index)) : "";
}

const char *tasteGoalAttributeLabel(const size_t index) {
    return index < TASTE_GOAL_ATTRIBUTE_COUNT ? ATTRIBUTE_LABELS[index] : "Taste";
}

bool setTypedTasteGoalForContext(Settings &settings, const String &beanContextId, const String &grinderContextId,
                                 TasteGoal const &goal, String &error) {
    if (beanContextId.isEmpty() || grinderContextId.isEmpty() || beanContextId.length() > 160 ||
        grinderContextId.length() > 160) {
        error = "Select a bean and grinder first";
        return false;
    }
    if (!goal.valid()) {
        error = "Taste goal is invalid";
        return false;
    }

    JsonDocument entriesDoc;
    if (deserializeJson(entriesDoc, settings.getRLTasteGoalsJson()) || !entriesDoc.is<JsonArray>()) {
        entriesDoc.clear();
        entriesDoc.to<JsonArray>();
    }
    JsonArray entries = entriesDoc.as<JsonArray>();
    removeStoredGoal(entries, beanContextId, grinderContextId);
    if (goal.mode == TasteGoalMode::Custom) {
        JsonObject entry = entries.add<JsonObject>();
        entry["b"] = beanContextId;
        entry["g"] = grinderContextId;
        writeTasteGoal(goal, entry["v"].to<JsonObject>());
    }

    String encoded;
    serializeJson(entriesDoc, encoded);
    if (encoded.length() > MAX_STORED_GOALS_JSON_BYTES) {
        error = "Too many saved custom taste goals";
        return false;
    }
    settings.setRLTasteGoalsJson(encoded);
    return true;
}

bool setTasteGoalForContext(Settings &settings, const String &beanContextId, const String &grinderContextId,
                            JsonVariantConst source, String &error) {
    TasteGoal goal;
    return parseTasteGoal(source, goal, error) &&
           setTypedTasteGoalForContext(settings, beanContextId, grinderContextId, goal, error);
}

void clearTasteGoals(Settings &settings) { settings.setRLTasteGoalsJson("[]"); }

void removeTasteGoalsForBeanContext(Settings &settings, const String &beanContextId) {
    removeStoredGoals(settings, beanContextId, true);
}

void removeTasteGoalsForGrinderContext(Settings &settings, const String &grinderContextId) {
    removeStoredGoals(settings, grinderContextId, false);
}

} // namespace AutoTuning
