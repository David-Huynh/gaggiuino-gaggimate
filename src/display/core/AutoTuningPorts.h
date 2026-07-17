#ifndef AUTOTUNINGPORTS_H
#define AUTOTUNINGPORTS_H

#include "AutoTuningModels.h"
#include <cstddef>
#include <string>

namespace AutoTuning {

struct LocalStoreStats {
    bool available = false;
    std::size_t shotCount = 0;
    std::size_t recommendationCount = 0;
    std::size_t bytes = 0;
};

struct LocalStoreRetentionPolicy {
    std::size_t maxShotSummaries = 96;
    std::size_t maxRecommendationSummaries = 64;
    std::size_t maxBytes = 192 * 1024;
};

class OptimizerTransportPort {
  public:
    virtual ~OptimizerTransportPort() = default;

    virtual bool configured() const = 0;
    virtual bool connected() const = 0;
    virtual bool publishShot(ShotRecord const &shot, bool reprocess) = 0;
    virtual bool publishLiveShotStarted(LiveShotStarted const &event) = 0;
    virtual bool publishLiveShotSample(LiveShotSample const &event) = 0;
    virtual bool publishLiveShotEnded(LiveShotEnded const &event) = 0;
};

class AutoTuningRecordStorePort {
  public:
    virtual ~AutoTuningRecordStorePort() = default;

    virtual bool storeShot(ShotRecord const &shot, ShotCompletion const &completion,
                           ShotCaptureDisposition const &disposition) = 0;
    virtual bool storeRecommendation(Recommendation const &recommendation) = 0;
    virtual bool correctShot(ShotCorrection const &correction, CorrectedShotRecord &corrected, std::string &reason) = 0;
};

class LocalOptimizationStorePort {
  public:
    virtual ~LocalOptimizationStorePort() = default;

    virtual bool reset() = 0;
    virtual LocalStoreStats stats() const = 0;
};

class CommunityUploadPort {
  public:
    virtual ~CommunityUploadPort() = default;

    virtual bool enqueueShot(ShotRecord const &shot) = 0;
    virtual bool enqueueRecommendation(Recommendation const &recommendation) = 0;
    virtual bool enqueuePreference(PreferenceFeedback const &preference) = 0;
    virtual bool applyCorrection(ShotCorrection const &correction) = 0;
};

} // namespace AutoTuning

#endif // AUTOTUNINGPORTS_H
