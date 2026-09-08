#ifndef AUTOTUNINGJSONCODEC_H
#define AUTOTUNINGJSONCODEC_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <display/core/AutoTuningModels.h>
#include <display/util/PsramStlAllocator.h>
#include <vector>

namespace AutoTuningJsonCodec {

struct DecodedShotRecord {
    AutoTuning::ShotRecord record;
    std::vector<AutoTuning::ShotSample, PsramStlAllocator<AutoTuning::ShotSample>> samples;

    void bindSamples();
};

bool writeShotRecord(AutoTuning::ShotRecord const &record, JsonDocument &document);
bool serializeShotRecord(AutoTuning::ShotRecord const &record, String &json);
bool parseShotRecord(JsonVariantConst source, DecodedShotRecord &decoded, String &error);
bool deserializeShotRecord(String const &json, DecodedShotRecord &decoded, String &error);

void writePreferenceAnchor(AutoTuning::PreferenceAnchorSummary const &summary, JsonObject output);
bool writePreferenceRequest(AutoTuning::PreferenceRequest const &request, JsonObject output);
bool writeShotDelivery(AutoTuning::ShotDeliveryAttempt const &attempt, JsonObject output);
bool parseShotAcknowledgement(JsonVariantConst source, AutoTuning::ShotDeliveryAcknowledgement &ack, String &error);
bool parsePreferenceRequest(JsonVariantConst source, AutoTuning::PreferenceRequest &request, String &error);
bool writeShotCompletion(AutoTuning::ShotCompletion const &completion, JsonDocument &document);
bool serializeShotCompletion(AutoTuning::ShotCompletion const &completion, String &json);
bool parseShotCompletion(JsonVariantConst source, AutoTuning::ShotCompletion &completion, String &error);

bool writeRecommendation(AutoTuning::Recommendation const &recommendation, JsonDocument &document);
bool serializeRecommendation(AutoTuning::Recommendation const &recommendation, String &json);
bool parseRecommendation(JsonVariantConst source, AutoTuning::Recommendation &recommendation, String &error);
bool deserializeRecommendation(String const &json, AutoTuning::Recommendation &recommendation, String &error);

} // namespace AutoTuningJsonCodec

#endif // AUTOTUNINGJSONCODEC_H
