#include "CemGpMpc.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace EspressoRL {

CemGpMpcController::CemGpMpcController(const CombinedHydraulicPredictor &predictor,
                                       const HydraulicControlEnvelope &envelope,
                                       const SplineTrajectoryBasis &splineBasis, const CemGpMpcConfig &config)
    : predictor_(predictor), envelope_(envelope), splineBasis_(splineBasis), cfg_(config) {
    cfg_.horizonSteps = std::clamp<std::size_t>(cfg_.horizonSteps, 2, MAX_MPC_HORIZON);
    cfg_.controlKnots = std::clamp<std::size_t>(cfg_.controlKnots, 2, MAX_CEM_KNOTS);
    cfg_.candidateCount = std::clamp<std::size_t>(cfg_.candidateCount, 4, MAX_CEM_CANDIDATES);
    cfg_.eliteCount = std::clamp<std::size_t>(cfg_.eliteCount, 1, cfg_.candidateCount - 1);
    resetShot();
}

void CemGpMpcController::resetShot() {
    hasLastPressure_ = false;
    hasLastFlow_ = false;
    hasLastPressureKnots_ = false;
    hasLastFlowKnots_ = false;
    lastCommandedPressure_ = 0.0f;
    lastCommandedFlow_ = 0.0f;
    lastPressureKnots_.fill(0.0f);
    lastFlowKnots_.fill(0.0f);
    rngState_ = static_cast<std::uint64_t>(cfg_.randomSeed) * 6364136223846793005ULL + 1442695040888963407ULL;
}

float CemGpMpcController::nextNormalF32() {
    auto nextUniform = [&]() -> float {
        // SplitMix64 deterministic PRNG (fast, zero-state overhead, portable).
        rngState_ += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = rngState_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z = z ^ (z >> 31);
        return static_cast<float>((z >> 40) + 1ULL) / 16777217.0f;
    };
    const float u1 = std::max(nextUniform(), 1e-7f);
    const float u2 = nextUniform();
    return std::sqrt(-2.0f * std::log(u1)) * std::cos(6.283185307179586f * u2);
}

void CemGpMpcController::expandKnots(const float *knots, float *outPlan) const {
    const std::size_t h = cfg_.horizonSteps;
    const std::size_t k = cfg_.controlKnots;
    const float scale = static_cast<float>(k - 1) / static_cast<float>(h - 1);
    for (std::size_t step = 0; step < h; ++step) {
        const float pos = static_cast<float>(step) * scale;
        const std::size_t lo = std::min(static_cast<std::size_t>(pos), k - 1);
        const std::size_t hi = std::min(lo + 1, k - 1);
        const float alpha = pos - static_cast<float>(lo);
        outPlan[step] = knots[lo] + alpha * (knots[hi] - knots[lo]);
    }
}

void CemGpMpcController::sampleKnotsFromPlan(const float *plan, float *outKnots) const {
    const std::size_t h = cfg_.horizonSteps;
    const std::size_t k = cfg_.controlKnots;
    const float scale = static_cast<float>(h - 1) / static_cast<float>(k - 1);
    for (std::size_t j = 0; j < k; ++j) {
        const float pos = static_cast<float>(j) * scale;
        const std::size_t lo = std::min(static_cast<std::size_t>(pos), h - 1);
        const std::size_t hi = std::min(lo + 1, h - 1);
        const float alpha = pos - static_cast<float>(lo);
        outKnots[j] = plan[lo] + alpha * (plan[hi] - plan[lo]);
    }
}

void CemGpMpcController::applyCorridorAndSlew(const float *rawPlan, const float *basePlan, float prevVal, float minVal,
                                              float maxVal, float maxBaseDelta, float maxStepSlew,
                                              float *outPlan) const {
    float prev = prevVal;
    for (std::size_t i = 0; i < cfg_.horizonSteps; ++i) {
        const float base = basePlan[i];
        const float lo = std::max(minVal, base - maxBaseDelta);
        const float hi = std::min(maxVal, base + maxBaseDelta);
        float val = std::clamp(rawPlan[i], lo, hi);
        val = std::clamp(val, prev - maxStepSlew, prev + maxStepSlew);
        val = std::clamp(val, minVal, maxVal);
        outPlan[i] = val;
        prev = val;
    }
}

ControlCommand CemGpMpcController::baseFallback(const HydraulicState &state, const float *baseP, const float *baseQ,
                                                std::size_t avail, float yieldStop, const char *reason) const {
    const float pVal = (avail > 0 && baseP != nullptr && std::isfinite(baseP[0])) ? baseP[0] : state.pressureBar;
    const float qVal = (avail > 0 && baseQ != nullptr && std::isfinite(baseQ[0])) ? baseQ[0] : state.pumpFlowMlS;
    return ControlCommand{
        state.pumpMode,
        std::clamp(std::isfinite(pVal) ? pVal : 0.0f, envelope_.minPressureBar, envelope_.maxPressureBar),
        std::clamp(std::isfinite(qVal) ? qVal : 0.0f, envelope_.minFlowMlS, envelope_.maxFlowMlS),
        yieldStop,
        true,
        reason,
    };
}

ControlCommand CemGpMpcController::computeCommand(const HydraulicState &state, const PumpMode *baseModes,
                                                  const float *basePressureTargets, const float *baseFlowTargets,
                                                  const float *referenceBeverageFlow, std::size_t horizonAvailable,
                                                  const float *deltaTheta, std::size_t deltaCount,
                                                  float relativeGrindSteps, float doseInG, float targetYieldG,
                                                  bool inBrewPhase) {
    const std::size_t h = cfg_.horizonSteps;
    const float dtS = predictor_.physics().dtS;
    const float yieldStop =
        std::clamp(std::isfinite(targetYieldG) ? targetYieldG : 36.0f, envelope_.minYieldStopG, envelope_.maxYieldStopG);

    if (!state.isFinite() || baseModes == nullptr || basePressureTargets == nullptr || baseFlowTargets == nullptr ||
        referenceBeverageFlow == nullptr || horizonAvailable < h || !std::isfinite(relativeGrindSteps) ||
        !std::isfinite(doseInG) || doseInG <= 0.0f) {
        return baseFallback(state, basePressureTargets, baseFlowTargets, horizonAvailable, yieldStop,
                            "invalid_horizon_inputs");
    }
    for (std::size_t i = 0; i < h; ++i) {
        if (!std::isfinite(basePressureTargets[i]) || !std::isfinite(baseFlowTargets[i]) ||
            !std::isfinite(referenceBeverageFlow[i])) {
            return baseFallback(state, basePressureTargets, baseFlowTargets, horizonAvailable, yieldStop,
                                "invalid_horizon_inputs");
        }
    }

    const PumpMode activeMode = baseModes[0];
    if (activeMode != PumpMode::PRESSURE && activeMode != PumpMode::FLOW) {
        return baseFallback(state, basePressureTargets, baseFlowTargets, horizonAvailable, yieldStop,
                            "unsupported_mode_fallback");
    }

    if (!inBrewPhase || state.pressureBar < envelope_.minActivationPressureBar ||
        state.weightG < envelope_.minActivationWeightG) {
        lastCommandedPressure_ = basePressureTargets[0];
        lastCommandedFlow_ = baseFlowTargets[0];
        hasLastPressure_ = true;
        hasLastFlow_ = true;
        return ControlCommand{
            activeMode,
            std::clamp(basePressureTargets[0], envelope_.minPressureBar, envelope_.maxPressureBar),
            std::clamp(baseFlowTargets[0], envelope_.minFlowMlS, envelope_.maxFlowMlS),
            yieldStop,
            true,
            "waiting_for_brew_activation",
        };
    }

    if (!splineBasis_.validDelta(deltaTheta, deltaCount)) {
        return baseFallback(state, basePressureTargets, baseFlowTargets, horizonAvailable, yieldStop,
                            "invalid_spline_delta");
    }

    // Build Level 1 spline-warped beverage-flow reference over shot progress s in [0, 1].
    std::array<float, MAX_MPC_HORIZON> targetQCurve{};
    float estWeight = state.weightG;
    for (std::size_t i = 0; i < h; ++i) {
        estWeight = std::clamp(estWeight + std::max(referenceBeverageFlow[i], 0.0f) * dtS, 0.0f, yieldStop);
        const float progress = estWeight / std::max(yieldStop, 1.0f);
        targetQCurve[i] =
            splineBasis_.warpTarget(referenceBeverageFlow[i], progress, deltaTheta, deltaCount, 0.2f,
                                    envelope_.maxPredictedFlowGS);
    }

    const bool isPressure = (activeMode == PumpMode::PRESSURE);
    const float *activeBase = isPressure ? basePressureTargets : baseFlowTargets;
    const float prevVal = isPressure ? (hasLastPressure_ ? lastCommandedPressure_ : state.pressureBar)
                                     : (hasLastFlow_ ? lastCommandedFlow_ : state.pumpFlowMlS);
    const float minVal = isPressure ? envelope_.minPressureBar : envelope_.minFlowMlS;
    const float maxVal = isPressure ? envelope_.maxPressureBar : envelope_.maxFlowMlS;
    const float maxBaseDelta = isPressure ? envelope_.maxBasePressureDeltaBar : envelope_.maxBaseFlowDeltaMlS;
    const float maxStepSlew = (isPressure ? envelope_.maxPressureSlewBarS : envelope_.maxFlowSlewMlS2) * dtS;

    std::array<float, MAX_CEM_KNOTS> baseKnots{};
    sampleKnotsFromPlan(activeBase, baseKnots.data());

    std::array<float, MAX_CEM_KNOTS> meanKnots = baseKnots;
    if (isPressure && hasLastPressureKnots_) {
        for (std::size_t j = 0; j < cfg_.controlKnots; ++j) {
            meanKnots[j] = std::clamp(lastPressureKnots_[j], baseKnots[j] - maxBaseDelta, baseKnots[j] + maxBaseDelta);
        }
    } else if (!isPressure && hasLastFlowKnots_) {
        for (std::size_t j = 0; j < cfg_.controlKnots; ++j) {
            meanKnots[j] = std::clamp(lastFlowKnots_[j], baseKnots[j] - maxBaseDelta, baseKnots[j] + maxBaseDelta);
        }
    }

    std::array<float, MAX_CEM_KNOTS> stdKnots{};
    stdKnots.fill(isPressure ? cfg_.initialPressureStdBar : cfg_.initialFlowStdMlS);

    double bestCost = std::numeric_limits<double>::infinity();
    std::array<float, MAX_MPC_HORIZON> bestPlan{};
    for (std::size_t i = 0; i < h; ++i) {
        bestPlan[i] = activeBase[i];
    }
    std::array<float, MAX_CEM_KNOTS> bestKnots = baseKnots;

    std::array<std::array<float, MAX_MPC_HORIZON>, MAX_CEM_CANDIDATES> clampedPlans{};
    std::array<double, MAX_CEM_CANDIDATES> costs{};
    std::array<std::size_t, MAX_CEM_CANDIDATES> order{};
    std::array<float, MAX_CEM_KNOTS> candKnots{};
    std::array<float, MAX_MPC_HORIZON> rawPlan{};
    RolloutOutput pred{};

    for (std::size_t iter = 0; iter < cfg_.cemIterations; ++iter) {
        bool anyFinite = false;
        for (std::size_t c = 0; c < cfg_.candidateCount; ++c) {
            if (c == 0) {
                // Candidate #0 is always the unmodified base profile.
                candKnots = baseKnots;
            } else if (iter == 0 && c == 1) {
                // Structured down-shift guarantees immediate first-tick channeling rescue at zero extra rollout cost.
                for (std::size_t j = 0; j < cfg_.controlKnots; ++j) {
                    candKnots[j] = meanKnots[j] - stdKnots[j];
                }
            } else if (iter == 0 && c == 2) {
                // Structured up-shift guarantees immediate first-tick stall/clog rescue at zero extra rollout cost.
                for (std::size_t j = 0; j < cfg_.controlKnots; ++j) {
                    candKnots[j] = meanKnots[j] + stdKnots[j];
                }
            } else {
                for (std::size_t j = 0; j < cfg_.controlKnots; ++j) {
                    candKnots[j] = meanKnots[j] + stdKnots[j] * nextNormalF32();
                }
            }
            expandKnots(candKnots.data(), rawPlan.data());
            applyCorridorAndSlew(rawPlan.data(), activeBase, prevVal, minVal, maxVal, maxBaseDelta, maxStepSlew,
                                 clampedPlans[c].data());

            // Mode-scoped horizon rollout: only mutate steps matching activeMode; preserve exact base setpoints
            // and secondary soft caps on any future phase that switches mode or uses SIMPLE / valve-closed.
            std::array<float, MAX_MPC_HORIZON> pPlanBuf{};
            std::array<float, MAX_MPC_HORIZON> qPlanBuf{};
            for (std::size_t i = 0; i < h; ++i) {
                if (baseModes[i] == activeMode) {
                    pPlanBuf[i] = isPressure ? clampedPlans[c][i] : basePressureTargets[i];
                    qPlanBuf[i] = isPressure ? baseFlowTargets[i] : clampedPlans[c][i];
                } else {
                    pPlanBuf[i] = basePressureTargets[i];
                    qPlanBuf[i] = baseFlowTargets[i];
                }
            }
            predictor_.rollout(state, baseModes, pPlanBuf.data(), qPlanBuf.data(), h, relativeGrindSteps, doseInG, true,
                               pred);

            bool ok = true;
            double trackSum = 0.0;
            double smoothSum = 0.0;
            double devSum = 0.0;
            double uncertSum = 0.0;
            double barrierSum = 0.0;
            float prevU = prevVal;

            for (std::size_t i = 0; i < h; ++i) {
                const float pPred = pred.pressureBar[i];
                const float pStd = pred.pressureStdBar[i];
                const float qPred = pred.beverageFlowGS[i];
                const float qStd = pred.beverageFlowStdGS[i];
                if (!std::isfinite(pPred) || !std::isfinite(qPred) || !std::isfinite(pStd) || !std::isfinite(qStd)) {
                    ok = false;
                    break;
                }
                if (baseModes[i] != PumpMode::SIMPLE) {
                    const float qErr = qPred - targetQCurve[i];
                    trackSum += static_cast<double>(qErr * qErr);
                    uncertSum += static_cast<double>(qStd * qStd);
                }
                if (baseModes[i] == activeMode) {
                    const float du = clampedPlans[c][i] - prevU;
                    smoothSum += static_cast<double>(du * du);
                    prevU = clampedPlans[c][i];
                    const float dev = clampedPlans[c][i] - activeBase[i];
                    devSum += static_cast<double>(dev * dev);
                }

                const float pViol = std::max((pPred + cfg_.safetyUcbBeta * pStd) - envelope_.maxPredictedPressureBar, 0.0f);
                const float qViol = std::max((qPred + cfg_.safetyUcbBeta * qStd) - envelope_.maxPredictedFlowGS, 0.0f);
                barrierSum += static_cast<double>(pViol * pViol + qViol * qViol);
            }

            if (!ok) {
                costs[c] = std::numeric_limits<double>::infinity();
                continue;
            }
            const double invH = 1.0 / static_cast<double>(h);
            const double cost = (trackSum * cfg_.trackingWeight + smoothSum * cfg_.smoothnessWeight +
                                 devSum * cfg_.baseDeviationWeight + uncertSum * cfg_.uncertaintyWeight +
                                 barrierSum * cfg_.safetyBarrierWeight) *
                                invH;
            costs[c] = cost;
            anyFinite = true;
            if (cost < bestCost) {
                bestCost = cost;
                bestPlan = clampedPlans[c];
                sampleKnotsFromPlan(bestPlan.data(), bestKnots.data());
            }
        }

        if (!anyFinite) {
            return baseFallback(state, basePressureTargets, baseFlowTargets, horizonAvailable, yieldStop,
                                "non_finite_cem_rollout");
        }

        std::iota(order.begin(), order.begin() + cfg_.candidateCount, 0);
        std::partial_sort(order.begin(), order.begin() + cfg_.eliteCount, order.begin() + cfg_.candidateCount,
                          [&](std::size_t a, std::size_t b) { return costs[a] < costs[b]; });

        std::array<float, MAX_CEM_KNOTS> sumKnots{};
        std::array<float, MAX_CEM_KNOTS> sumSqKnots{};
        std::array<float, MAX_CEM_KNOTS> eliteK{};
        for (std::size_t e = 0; e < cfg_.eliteCount; ++e) {
            sampleKnotsFromPlan(clampedPlans[order[e]].data(), eliteK.data());
            for (std::size_t j = 0; j < cfg_.controlKnots; ++j) {
                sumKnots[j] += eliteK[j];
                sumSqKnots[j] += eliteK[j] * eliteK[j];
            }
        }
        const float invE = 1.0f / static_cast<float>(cfg_.eliteCount);
        for (std::size_t j = 0; j < cfg_.controlKnots; ++j) {
            const float mu = sumKnots[j] * invE;
            const float var = std::max(sumSqKnots[j] * invE - mu * mu, 0.0f);
            meanKnots[j] = mu;
            stdKnots[j] = std::max(std::sqrt(var), cfg_.minStd);
        }
    }

    float pCmd = std::clamp(basePressureTargets[0], envelope_.minPressureBar, envelope_.maxPressureBar);
    float qCmd = std::clamp(baseFlowTargets[0], envelope_.minFlowMlS, envelope_.maxFlowMlS);
    if (isPressure) {
        lastPressureKnots_ = bestKnots;
        hasLastPressureKnots_ = true;
        pCmd = envelope_.clampPressureTarget(bestPlan[0], basePressureTargets[0], prevVal, dtS);
        lastCommandedPressure_ = pCmd;
        hasLastPressure_ = true;
    } else {
        lastFlowKnots_ = bestKnots;
        hasLastFlowKnots_ = true;
        qCmd = envelope_.clampFlowTarget(bestPlan[0], baseFlowTargets[0], prevVal, dtS);
        lastCommandedFlow_ = qCmd;
        hasLastFlow_ = true;
    }

    return ControlCommand{activeMode, pCmd, qCmd, yieldStop, false, "mpc_optimal"};
}

} // namespace EspressoRL
