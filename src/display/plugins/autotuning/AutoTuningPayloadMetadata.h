#ifndef AUTOTUNINGPAYLOADMETADATA_H
#define AUTOTUNINGPAYLOADMETADATA_H

#include <ArduinoJson.h>
#include <display/core/AutoTuningModels.h>

class Controller;

namespace AutoTuningPayloadMetadata {
void captureRecipe(Controller *controller, AutoTuning::RecipeSnapshot &recipe);
void captureProfile(Controller *controller, AutoTuning::ProfileSnapshot &profile);
void addRecipe(Controller *controller, JsonDocument &doc);
void addProfile(Controller *controller, JsonDocument &doc);
} // namespace AutoTuningPayloadMetadata

#endif // AUTOTUNINGPAYLOADMETADATA_H
