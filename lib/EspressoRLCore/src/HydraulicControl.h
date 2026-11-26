#ifndef ESPRESSORL_CORE_HYDRAULIC_CONTROL_H
#define ESPRESSORL_CORE_HYDRAULIC_CONTROL_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace EspressoRL {

enum class PumpMode : uint8_t {
    SIMPLE = 0,
    PRESSURE = 1,
    FLOW = 2,
};

struct HydraulicControlEnvelope {
    float minPressureBar = 0.0f;
    float maxPressureBar = 12.0f;
    float minFlowMlS = 0.0f;
    float maxFlowMlS = 20.0f;
    float maxPressureSlewBarS = 3.0f;
    float maxFlowSlewMlS2 = 4.0f;
    float maxBasePressureDeltaBar = 2.5f;
    float maxBaseFlowDeltaMlS = 1.2f;
    float minYieldStopG = 5.0f;
    float maxYieldStopG = 90.0f;
    float minActivationPressureBar = 1.5f;
    float minActivationWeightG = 0.5f;
    float maxPredictedPressureBar = 11.5f;
    float maxPredictedFlowGS = 6.0f;

    float clampPressureTarget(float proposedBar, float baseBar, float previousBar, float dtS) const {
        if (!std::isfinite(proposedBar) || !std::isfinite(baseBar) || !std::isfinite(previousBar) ||
            !std::isfinite(dtS) || dtS <= 0.0f) {
            return std::clamp(std::isfinite(baseBar) ? baseBar : 0.0f, minPressureBar, maxPressureBar);
        }
        const float base = std::clamp(baseBar, minPressureBar, maxPressureBar);
        const float prev = std::clamp(previousBar, minPressureBar, maxPressureBar);
        const float lo = std::max(minPressureBar, base - maxBasePressureDeltaBar);
        const float hi = std::min(maxPressureBar, base + maxBasePressureDeltaBar);
        const float corridor = std::clamp(proposedBar, lo, hi);
        const float maxStep = maxPressureSlewBarS * dtS;
        const float slewed = std::clamp(corridor, prev - maxStep, prev + maxStep);
        return std::clamp(slewed, minPressureBar, maxPressureBar);
    }

    float clampFlowTarget(float proposedMlS, float baseMlS, float previousMlS, float dtS) const {
        if (!std::isfinite(proposedMlS) || !std::isfinite(baseMlS) || !std::isfinite(previousMlS) ||
            !std::isfinite(dtS) || dtS <= 0.0f) {
            return std::clamp(std::isfinite(baseMlS) ? baseMlS : 0.0f, minFlowMlS, maxFlowMlS);
        }
        const float base = std::clamp(baseMlS, minFlowMlS, maxFlowMlS);
        const float prev = std::clamp(previousMlS, minFlowMlS, maxFlowMlS);
        const float lo = std::max(minFlowMlS, base - maxBaseFlowDeltaMlS);
        const float hi = std::min(maxFlowMlS, base + maxBaseFlowDeltaMlS);
        const float corridor = std::clamp(proposedMlS, lo, hi);
        const float maxStep = maxFlowSlewMlS2 * dtS;
        const float slewed = std::clamp(corridor, prev - maxStep, prev + maxStep);
        return std::clamp(slewed, minFlowMlS, maxFlowMlS);
    }
};

struct SplineTrajectoryBasis {
    static constexpr std::size_t MAX_KNOTS = 4;

    std::size_t knotCount = 3; // 3 (early/mid/late) or 4
    float maxAbsDelta = 1.5f;

    bool validDelta(const float *deltas, std::size_t count) const {
        if (deltas == nullptr || count != knotCount || (knotCount != 3 && knotCount != 4)) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (!std::isfinite(deltas[i])) {
                return false;
            }
        }
        return true;
    }

    std::array<float, MAX_KNOTS> basisWeights(float progress) const {
        std::array<float, MAX_KNOTS> w{0.0f, 0.0f, 0.0f, 0.0f};
        const float s = std::clamp(std::isfinite(progress) ? progress : 0.0f, 0.0f, 1.0f);
        const float om = 1.0f - s;
        if (knotCount == 4) {
            w[0] = om * om * om;
            w[1] = 3.0f * s * om * om;
            w[2] = 3.0f * s * s * om;
            w[3] = s * s * s;
        } else {
            w[0] = om * om;
            w[1] = 2.0f * s * om;
            w[2] = s * s;
        }
        return w;
    }

    float offsetAt(float progress, const float *deltas, std::size_t count) const {
        if (!validDelta(deltas, count)) {
            return 0.0f;
        }
        const auto w = basisWeights(progress);
        float sum = 0.0f;
        for (std::size_t i = 0; i < knotCount; ++i) {
            const float d = std::clamp(deltas[i], -maxAbsDelta, maxAbsDelta);
            sum += w[i] * d;
        }
        return std::clamp(sum, -maxAbsDelta, maxAbsDelta);
    }

    float warpTarget(float baseValue, float progress, const float *deltas, std::size_t count, float minimum,
                     float maximum) const {
        if (!std::isfinite(baseValue)) {
            return std::clamp(0.0f, minimum, maximum);
        }
        return std::clamp(baseValue + offsetAt(progress, deltas, count), minimum, maximum);
    }
};

struct HydraulicState {
    float elapsedS = 0.0f;
    float pressureBar = 0.0f;
    float pumpFlowMlS = 0.0f;
    float beverageFlowGS = 0.0f;
    float waterPumpedMl = 0.0f;
    float weightG = 0.0f;
    PumpMode pumpMode = PumpMode::PRESSURE;
    float liveOffset = 0.0f;
    float liveRate = 0.0f;
    float liveCov00 = 0.09f;
    float liveCov01 = 0.0f;
    float liveCov11 = 0.04f;

    bool isFinite() const {
        return std::isfinite(elapsedS) && std::isfinite(pressureBar) && std::isfinite(pumpFlowMlS) &&
               std::isfinite(beverageFlowGS) && std::isfinite(waterPumpedMl) && std::isfinite(weightG) &&
               std::isfinite(liveOffset) && std::isfinite(liveRate) && std::isfinite(liveCov00) &&
               std::isfinite(liveCov01) && std::isfinite(liveCov11) && liveCov00 >= 0.0f && liveCov11 >= 0.0f;
    }
};

struct ControlCommand {
    PumpMode pumpMode = PumpMode::PRESSURE;
    float pressureTargetBar = 0.0f;
    float flowTargetMlS = 0.0f;
    float yieldStopG = 36.0f;
    bool fallbackUsed = false;
    const char *reason = "mpc_optimal";
};

/// Two-timescale anti-conflict baseline policy for predictNominalReferenceFlow(...).
/// Prevents the inner-loop MPC from fighting an intentional outer-loop CPBO grind/dose
/// change, an unconfirmed manual grinder step, or bean-aging drift while still rescuing
/// dynamic mid-shot channeling surges and flow runaways.
struct NominalBaselinePolicy {
    std::uint16_t latchTicks = 6; // ~1.5 s at 4 Hz for early-brew KF settling
    float maxAbsBaselineOffset = 0.9f;

    bool useEarlyBrewLatch = true;
    std::uint16_t activeTicks = 0;
    bool latched = false;
    float latchedOffset = 0.0f;

    void resetShot(bool hasCrossShotHistory, bool grindConfirmedPreShot, bool recipeChangedSinceLastShot,
                   bool assistOnlyMode) {
        activeTicks = 0;
        latched = false;
        latchedOffset = 0.0f;
        useEarlyBrewLatch =
            (!hasCrossShotHistory) || (!grindConfirmedPreShot) || recipeChangedSinceLastShot || assistOnlyMode;
    }

    float updateAndResolve(bool readyForMpc, float currentLiveOffset) {
        const float safeOff = std::clamp(std::isfinite(currentLiveOffset) ? currentLiveOffset : 0.0f,
                                         -maxAbsBaselineOffset, maxAbsBaselineOffset);
        if (!useEarlyBrewLatch) {
            return 0.0f;
        }
        if (readyForMpc) {
            ++activeTicks;
            if (!latched && activeTicks >= latchTicks) {
                latchedOffset = safeOff;
                latched = true;
            }
        }
        return latched ? latchedOffset : safeOff;
    }
};

} // namespace EspressoRL

#endif // ESPRESSORL_CORE_HYDRAULIC_CONTROL_H
