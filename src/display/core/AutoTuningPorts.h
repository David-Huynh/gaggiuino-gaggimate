#ifndef AUTOTUNINGPORTS_H
#define AUTOTUNINGPORTS_H

#include "AutoTuningModels.h"
#include <array>
#include <cstddef>
#include <cstdint>
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

    virtual bool enqueueCommunityHandoff(const std::string &payload) { return false; }
    virtual bool configured() const = 0;
    virtual bool connected() const = 0;
    virtual ShotSubmissionResult publishShot(ShotRecord const &shot, ShotDeliveryAttempt const &attempt) = 0;
    virtual bool publishLiveShotStarted(LiveShotStarted const &event) = 0;
    virtual bool publishLiveShotSample(LiveShotSample const &event) = 0;
    virtual bool publishLiveShotEnded(LiveShotEnded const &event) = 0;
};

class CompletedShotProjectionPort {
  public:
    virtual ~CompletedShotProjectionPort() = default;

    virtual bool ensureProjection(CompletedShotArtifact const &artifact) = 0;
    virtual bool removeProjection(std::uint32_t historyId) = 0;
};

class AutoTuningRecordStorePort {
  public:
    virtual ~AutoTuningRecordStorePort() = default;

    virtual bool enqueueShot(ShotRecord const &shot, ShotCompletion const &completion,
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

struct OnBoardRuntimeTelemetry {
    bool ready = false;
    bool shadowModeOnly = true;
    std::uint8_t controlMode = 0; // 0=recipe_only, 1=spline_only, 2=joint_6d
    bool persistedStateLoaded = false;
    bool persistenceAvailable = false;
    std::array<float, 3> activeSplineDelta{0.0f, 0.0f, 0.0f};
    bool hasPendingSplineDelta = false;
    std::array<float, 3> pendingSplineDelta{0.0f, 0.0f, 0.0f};
    std::size_t vfeSampleCount = 0;
    std::size_t cpboPointCount = 0;
    std::size_t cpboComparisonCount = 0;
    std::uint32_t lastMpcFitUs = 0;
    std::uint32_t lastCpboFitMs = 0;
    bool lastLiveActive = false;
    std::uint8_t lastLiveMode = 0;
    float lastBasePressureBar = 0.0f;
    float lastMpcPressureBar = 0.0f;
    float lastBaseFlowMlS = 0.0f;
    float lastMpcFlowMlS = 0.0f;
    float lastKfOffset = 0.0f;
    std::uint32_t leaseEvaluatedCount = 0;
    std::uint32_t leaseShadowCount = 0;
    std::uint32_t leaseAppliedCount = 0;
    std::uint32_t leaseRejectedCount = 0;
    std::uint32_t leaseExpiredCount = 0;
};

class OnBoardOptimizerPort {
  public:
    virtual ~OnBoardOptimizerPort() = default;

    virtual bool ready() const = 0;
    virtual bool shadowModeOnly() const = 0;
    virtual void setShadowModeOnly(bool shadowOnly) = 0;
    virtual std::uint8_t controlMode() const { return 0; }
    virtual bool setControlMode(std::uint8_t) { return false; }
    virtual bool resetActiveSplineDelta() { return false; }
    virtual OnBoardRuntimeTelemetry runtimeTelemetry() const { return OnBoardRuntimeTelemetry{}; }
    virtual ShotSubmissionResult submitShot(ShotRecord const &shot, ShotDeliveryAttempt const &attempt) = 0;
    virtual bool onLiveShotStarted(LiveShotStarted const &event) = 0;
    virtual bool onLiveShotSample(LiveShotSample const &event) = 0;
    virtual bool onLiveShotEnded(LiveShotEnded const &event) = 0;
};

} // namespace AutoTuning

#endif // AUTOTUNINGPORTS_H
