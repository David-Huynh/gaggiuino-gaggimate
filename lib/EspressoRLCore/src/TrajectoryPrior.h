#ifndef ESPRESSORL_CORE_TRAJECTORY_PRIOR_H
#define ESPRESSORL_CORE_TRAJECTORY_PRIOR_H

#include "HydraulicControl.h"
#include "HydraulicWorldModel.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace EspressoRL {

constexpr std::uint32_t ETP1_MAGIC = 0x31505445u; // "ETP1"
constexpr std::uint16_t ETP1_VERSION = 1u;
constexpr std::size_t ETP1_HEADER_SIZE = 16;
constexpr std::size_t ETP1_MAX_BYTES = 4096;
constexpr std::size_t ETP1_MAX_GOAL_BUCKETS = 8;
constexpr std::size_t TRAJECTORY_FEATURE_DIM = 14;

constexpr float ETP1_MAX_PRIOR_SCALE = 0.60f;
constexpr float ETP1_MAX_ABS_LINEAR_WEIGHT = 5.0f;
constexpr float ETP1_MAX_ABS_QUAD_WEIGHT = 2.0f;
constexpr float ETP1_STANDARDIZED_CLIP = 4.0f;
constexpr std::uint8_t ETP1_EXTENDED_RECORDING_BIT = 1u << 4;

/// 14D profile-agnostic physical trajectory summary matching Python espresso_rl.domain.trajectory_prior:
///   0: brew_ratio             7: late_flow_gs
///   1: dose_g                 8: flow_slope_gs (late - early)
///   2: total_time_s           9: peak_pressure_bar
///   3: time_to_first_drop_s  10: mean_pressure_bar
///   4: mean_flow_gs          11: pressure_decline_bar
///   5: early_flow_gs         12: flow_roughness_gs
///   6: mid_flow_gs           13: mean_temp_c
using TrajectoryFeatureVec = std::array<float, TRAJECTORY_FEATURE_DIM>;

struct TrajectorySamplePoint {
    float elapsedS = 0.0f;
    float pressureBar = 0.0f;
    float beverageFlowGS = 0.0f;
    float weightG = 0.0f;
    float temperatureC = 93.0f;
    bool valveOpen = true;
    std::uint8_t systemInfo = 0;
};

struct GoalPriorBucket {
    std::uint64_t goalHash = 0; // 0 = Balanced fallback; FNV-1a 64-bit of TasteGoal fingerprint for custom goals
    float priorScale = 0.0f;
    float confidence = 0.0f;
    std::uint16_t comparisonCount = 0;
    std::uint16_t installCount = 0;
    TrajectoryFeatureVec linearWeights{};
    TrajectoryFeatureVec quadWeights{};
};

struct TrajectoryTastePrior {
    bool valid = false;
    bool approved = false;
    std::int64_t createdAt = 0;
    float decayComparisons = 4.0f;
    TrajectoryFeatureVec featureMean{};
    TrajectoryFeatureVec featureScale{};
    std::size_t bucketCount = 0;
    std::array<GoalPriorBucket, ETP1_MAX_GOAL_BUCKETS> buckets{};

    /// Returns 0.0 when invalid/unapproved or missing bucket; decays monotonically as local comparisons grow.
    float evaluate(const TrajectoryFeatureVec &features, std::uint64_t goalHash,
                   std::size_t localComparisonCount) const;

    float primaryConfidence() const;
};

/// Deterministic FNV-1a 64-bit hash of a TasteGoal fingerprint string (returns 0 if empty/balanced).
std::uint64_t hashTasteGoalFingerprint(const char *fingerprint, std::size_t len, bool isBalanced);

/// IEEE-802.3 CRC32 used by ETP1 binary codec.
std::uint32_t etp1Crc32(const std::uint8_t *data, std::size_t len);

/// Zero-trust decode of an ETP1 binary buffer (validates magic, version, CRC32, finiteness, and weight bounds).
bool decodeEtp1(const std::uint8_t *data, std::size_t size, TrajectoryTastePrior &outPrior);

/// Extract canonical 14D trajectory features from recorded or simulated shot samples (matches Python bit-for-bit).
bool extractTrajectoryFeatures(const TrajectorySamplePoint *samples, std::size_t count, float doseG,
                               float beverageOutG, float shotTimeS, TrajectoryFeatureVec &outFeatures);

/// Lightweight forward hydraulic rollout predicting expected 14D trajectory features for any candidate
/// (grind, dose, yield, 3-knot spline delta) under a nominal pressure/flow extraction profile.
bool predictCandidateTrajectoryFeatures(const CombinedHydraulicPredictor &predictor, float hydraulicGrindSteps,
                                        float doseG, float targetYieldG, const std::array<float, 3> &splineDelta,
                                        float basePressureBar, float tailPressureBar, float temperatureC,
                                        TrajectoryFeatureVec &outFeatures);

} // namespace EspressoRL

#endif // ESPRESSORL_CORE_TRAJECTORY_PRIOR_H
