#ifndef ESPRESSORL_CORE_CEM_GP_MPC_H
#define ESPRESSORL_CORE_CEM_GP_MPC_H

#include "HydraulicControl.h"
#include "HydraulicWorldModel.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace EspressoRL {

constexpr std::size_t MAX_CEM_KNOTS = 6;
constexpr std::size_t MAX_CEM_CANDIDATES = 48;

struct CemGpMpcConfig {
    std::size_t horizonSteps = 16;
    std::size_t controlKnots = 4;
    std::size_t cemIterations = 3;
    std::size_t candidateCount = 32;
    std::size_t eliteCount = 6;
    float initialPressureStdBar = 1.2f;
    float initialFlowStdMlS = 0.5f;
    float minStd = 0.08f;
    float trackingWeight = 4.0f;
    float smoothnessWeight = 0.35f;
    float baseDeviationWeight = 0.08f;
    float uncertaintyWeight = 0.50f;
    float safetyUcbBeta = 2.0f;
    float safetyBarrierWeight = 250.0f;
    std::uint32_t randomSeed = 17u;
};

/// Zero-heap-allocation Level 1 CEM GP-MPC controller for 4 Hz in-shot ESP32-S3 execution.
class CemGpMpcController {
  public:
    CemGpMpcController(const CombinedHydraulicPredictor &predictor,
                       const HydraulicControlEnvelope &envelope = HydraulicControlEnvelope{},
                       const SplineTrajectoryBasis &splineBasis = SplineTrajectoryBasis{},
                       const CemGpMpcConfig &config = CemGpMpcConfig{});

    void resetShot();

    ControlCommand computeCommand(const HydraulicState &state, const PumpMode *baseModes,
                                  const float *basePressureTargets, const float *baseFlowTargets,
                                  const float *referenceBeverageFlow, std::size_t horizonAvailable,
                                  const float *deltaTheta, std::size_t deltaCount, float relativeGrindSteps,
                                  float doseInG, float targetYieldG, bool inBrewPhase);

  private:
    const CombinedHydraulicPredictor &predictor_;
    HydraulicControlEnvelope envelope_{};
    SplineTrajectoryBasis splineBasis_{};
    CemGpMpcConfig cfg_{};

    std::uint64_t rngState_ = 17u;
    bool hasLastPressure_ = false;
    bool hasLastFlow_ = false;
    bool hasLastPressureKnots_ = false;
    bool hasLastFlowKnots_ = false;
    float lastCommandedPressure_ = 0.0f;
    float lastCommandedFlow_ = 0.0f;
    std::array<float, MAX_CEM_KNOTS> lastPressureKnots_{};
    std::array<float, MAX_CEM_KNOTS> lastFlowKnots_{};

    float nextNormalF32();
    void expandKnots(const float *knots, float *outPlan) const;
    void sampleKnotsFromPlan(const float *plan, float *outKnots) const;
    void applyCorridorAndSlew(const float *rawPlan, const float *basePlan, float prevVal, float minVal, float maxVal,
                              float maxBaseDelta, float maxStepSlew, float *outPlan) const;
    ControlCommand baseFallback(const HydraulicState &state, const float *baseP, const float *baseQ,
                                std::size_t avail, float yieldStop, const char *reason) const;
};

} // namespace EspressoRL

#endif // ESPRESSORL_CORE_CEM_GP_MPC_H
