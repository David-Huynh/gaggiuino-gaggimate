#ifndef LOCALAUTOTUNINGSUMMARYSTORE_H
#define LOCALAUTOTUNINGSUMMARYSTORE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstddef>

namespace AutoTuning {
struct ShotCorrection;
}

class LocalAutoTuningSummaryStore {
  public:
    struct Stats {
        size_t shotCount = 0;
        size_t recommendationCount = 0;
        size_t bytes = 0;
    };

    bool begin() const;
    bool reset() const;
    Stats stats() const;
    bool loadShot(const String &shotId, JsonDocument &document) const;
    bool removeShot(const String &shotId) const;
    bool upsertShot(JsonObjectConst shot);
    bool upsertRecommendation(JsonObjectConst recommendation);
    bool patchShotCorrection(const String &shotId, AutoTuning::ShotCorrection const &correction);
    bool patchRecommendationStatus(const String &recommendationId, const String &status, const char *timestampKey);
    void prune() const;
};

#endif // LOCALAUTOTUNINGSUMMARYSTORE_H
