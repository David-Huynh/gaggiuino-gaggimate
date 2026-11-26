#ifndef PROFILEHORIZONSAMPLER_H
#define PROFILEHORIZONSAMPLER_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ProfileHorizon {

constexpr std::size_t MAX_HORIZON_STEPS = 24;
constexpr std::size_t MAX_PHASE_TARGETS = 6;

enum class Mode : uint8_t {
    SIMPLE = 0,
    PRESSURE = 1,
    FLOW = 2,
};

enum class Easing : uint8_t {
    INSTANT = 0,
    LINEAR = 1,
    EASE_IN = 2,
    EASE_OUT = 3,
    EASE_IN_OUT = 4,
};

enum class TransTarget : uint8_t {
    TIME = 0,
    VOLUMETRIC = 1,
    PUMPED = 2,
};

enum class ExitTargetType : uint8_t {
    VOLUMETRIC = 0,
    RATIO = 1,
    PRESSURE = 2,
    FLOW = 3,
    PUMPED = 4,
};

enum class ExitTargetOp : uint8_t {
    LTE = 0,
    GTE = 1,
};

struct ExitTarget {
    ExitTargetType type = ExitTargetType::VOLUMETRIC;
    ExitTargetOp op = ExitTargetOp::GTE;
    float value = 0.0f;

    bool isReached(float input) const {
        if (!std::isfinite(input) || !std::isfinite(value)) {
            return false;
        }
        return (op == ExitTargetOp::GTE) ? (input >= value) : (input <= value);
    }
};

struct PhaseSpec {
    bool isBrewPhase = true;
    bool hasVolumetricTarget = false;
    bool pumpIsSimple = false;
    Mode mode = Mode::PRESSURE;
    float durationS = 30.0f;
    float targetPressureBar = 9.0f; // -1.0f means hold phase-entry pressure; 0.0f in FLOW mode = uncapped
    float targetFlowMlS = 2.0f;     // -1.0f means hold phase-entry flow; 0.0f in PRESSURE mode = uncapped
    Easing easing = Easing::INSTANT;
    float transitionDurationS = 0.0f;
    bool transitionAdaptive = false;
    TransTarget transitionTarget = TransTarget::TIME;
    float volumetricTargetG = 0.0f; // >0 when phase owns a volumetric/ratio stop target
    float pumpedTargetMl = 0.0f;    // >0 when phase owns a pumped-water exit target
    bool valveOpen = true;
    bool isStandardProfile = false;
    float profileDoseG = 18.0f;
    std::array<ExitTarget, MAX_PHASE_TARGETS> targets{};
    std::size_t targetCount = 0;

    bool addTarget(ExitTargetType t, ExitTargetOp op, float val) {
        if (targetCount >= MAX_PHASE_TARGETS || !std::isfinite(val)) {
            return false;
        }
        targets[targetCount++] = ExitTarget{t, op, val};
        if ((t == ExitTargetType::VOLUMETRIC || t == ExitTargetType::RATIO) && val > 0.0f) {
            hasVolumetricTarget = true;
            volumetricTargetG = (t == ExitTargetType::RATIO && profileDoseG > 0.0f) ? (val * profileDoseG) : val;
        } else if (t == ExitTargetType::PUMPED && val > 0.0f) {
            pumpedTargetMl = val;
        }
        return true;
    }

    float resolvedVolumetricTargetG() const {
        for (std::size_t i = 0; i < std::min(targetCount, MAX_PHASE_TARGETS); ++i) {
            const ExitTarget &t = targets[i];
            if (t.type == ExitTargetType::VOLUMETRIC && t.value > 0.0f) {
                return t.value;
            }
            if (t.type == ExitTargetType::RATIO && t.value > 0.0f && profileDoseG > 0.0f) {
                return t.value * profileDoseG;
            }
        }
        return volumetricTargetG;
    }

    float resolvedPumpedTargetMl() const {
        for (std::size_t i = 0; i < std::min(targetCount, MAX_PHASE_TARGETS); ++i) {
            if (targets[i].type == ExitTargetType::PUMPED && targets[i].value > 0.0f) {
                return targets[i].value;
            }
        }
        return pumpedTargetMl;
    }

    bool anyVolumetricTarget() const {
        if (hasVolumetricTarget) {
            return true;
        }
        return resolvedVolumetricTargetG() > 0.0f;
    }

    /// Mirrors Phase::isFinished(...) in src/display/models/profile.h.
    /// Returns: 0 = still running, 1 = target reached, 2 = duration elapsed.
    uint8_t exitStatus(bool volumetricEnabled, float weightG, float timeInPhaseS, float currentFlowMlS,
                       float currentPressureBar, float phaseWaterPumpedMl) const {
        bool volumetricTested = false;
        const std::size_t n = std::min(targetCount, MAX_PHASE_TARGETS);
        for (std::size_t i = 0; i < n; ++i) {
            const ExitTarget &t = targets[i];
            switch (t.type) {
            case ExitTargetType::VOLUMETRIC:
                if (t.value <= 0.0f) {
                    break;
                }
                volumetricTested = volumetricEnabled;
                if (volumetricEnabled && t.isReached(weightG)) {
                    return 1u;
                }
                break;
            case ExitTargetType::RATIO:
                if (t.value <= 0.0f || profileDoseG <= 0.0f) {
                    break;
                }
                volumetricTested = volumetricEnabled;
                if (volumetricEnabled && t.isReached(weightG / profileDoseG)) {
                    return 1u;
                }
                break;
            case ExitTargetType::PRESSURE:
                if (t.isReached(currentPressureBar)) {
                    return 1u;
                }
                break;
            case ExitTargetType::FLOW:
                if (t.isReached(currentFlowMlS)) {
                    return 1u;
                }
                break;
            case ExitTargetType::PUMPED:
                if (t.isReached(phaseWaterPumpedMl)) {
                    return 1u;
                }
                break;
            }
        }
        if (isStandardProfile && volumetricTested) {
            return 0u;
        }
        return (durationS > 0.0f && timeInPhaseS > durationS) ? 2u : 0u;
    }
};

struct HorizonSample {
    std::size_t steps = 0;
    std::array<Mode, MAX_HORIZON_STEPS> modes{};
    std::array<float, MAX_HORIZON_STEPS> basePressureBar{};
    std::array<float, MAX_HORIZON_STEPS> baseFlowMlS{};
    std::array<float, MAX_HORIZON_STEPS> refBeverageFlowGS{};
};

/// Phase-level eligibility for GP-MPC across any Gaggimate profile schema:
/// - Rejects closed solenoid (valve == 0), simple open-loop duty phases (standard profiles, pump=0 stabilizes),
///   and low-pressure / low-flow soak & dynamic bloom phases.
/// - Respects explicit "preinfusion" phases whenever later "brew" phases exist (Damian, Dark & Steady,
///   Extractamundo Dos!, Cremina, Direct Lever, LH, Slayer).
/// - For all-preinfusion Pro profiles (e.g., Sir Lancelot's Lever), activates on the volumetric/final extraction phase.
inline bool isExtractionPhaseActive(const PhaseSpec *phases, std::size_t phaseCount, std::size_t currentPhaseIdx,
                                    float resolvedEffectiveP = -2.0f, float resolvedEffectiveQ = -2.0f,
                                    bool hasLaterBrewBeyondWindow = false) {
    if (phases == nullptr || phaseCount == 0 || currentPhaseIdx >= phaseCount) {
        return false;
    }
    const PhaseSpec &cur = phases[currentPhaseIdx];
    if (!cur.valveOpen || cur.pumpIsSimple || cur.mode == Mode::SIMPLE) {
        return false;
    }

    // Reject explicit soak / trickle-bloom setpoints (e.g., Cremina 1.1 bar soak, Extractamundo 0.1 ml/s bloom).
    // Note: -1.0f sentinel ("hold phase-entry value") is deferred to the live hydraulic readiness gate.
    const float effP = (resolvedEffectiveP >= -1.5f) ? resolvedEffectiveP : cur.targetPressureBar;
    const float effQ = (resolvedEffectiveQ >= -1.5f) ? resolvedEffectiveQ : cur.targetFlowMlS;
    if (cur.mode == Mode::PRESSURE && effP >= 0.0f && effP < 2.0f) {
        return false;
    }
    if (cur.mode == Mode::FLOW && effQ >= 0.0f && effQ < 0.4f) {
        return false;
    }

    if (cur.isBrewPhase) {
        return true;
    }
    if (hasLaterBrewBeyondWindow) {
        return false;
    }
    for (std::size_t i = currentPhaseIdx + 1; i < phaseCount; ++i) {
        if (phases[i].isBrewPhase) {
            return false;
        }
    }
    return cur.anyVolumetricTarget() || (currentPhaseIdx + 1 == phaseCount);
}

/// Physical extraction readiness gate (independent of profile author phase labels).
/// Prevents MPC from overriding dry fills, preinfusion ramps, or closed-valve pauses.
inline bool isHydraulicallyReadyForMpc(bool valveOpen, float pressureBar, float beverageFlowGS, float weightG,
                                       float minPressureBar = 1.5f, float minFlowGS = 0.25f, float minWeightG = 1.5f) {
    return valveOpen && std::isfinite(pressureBar) && std::isfinite(beverageFlowGS) && std::isfinite(weightG) &&
           pressureBar >= minPressureBar && beverageFlowGS >= minFlowGS && weightG >= minWeightG;
}

inline float applyEasing(float t, Easing easing) {
    if (t <= 0.0f) {
        return 0.0f;
    }
    if (t >= 1.0f) {
        return 1.0f;
    }
    switch (easing) {
    case Easing::LINEAR:
        return t;
    case Easing::EASE_IN:
        return t * t;
    case Easing::EASE_OUT:
        return 1.0f - (1.0f - t) * (1.0f - t);
    case Easing::EASE_IN_OUT:
        return (t < 0.5f) ? (2.0f * t * t) : (1.0f - 2.0f * (1.0f - t) * (1.0f - t));
    case Easing::INSTANT:
    default:
        return 1.0f;
    }
}

/// Sample a deterministic lookahead horizon mirroring BrewProcess::progress() and transitionAlpha()
/// across any arbitrary multi-phase Gaggimate profile (standard or pro, arbitrary phase counts via sliding window,
/// TIME/VOLUMETRIC/PUMPED transitions, all 5 exit target types including instant phase skips, -1 hold sentinels,
/// 0 uncapped soft limits, and valve-closed pauses) without mutating underlying profile state.
inline bool sampleHorizon(const PhaseSpec *phases, std::size_t phaseCount, std::size_t currentPhaseIdx,
                          float currentPhaseElapsedS, float currentPhaseStartP, float currentPhaseStartQ,
                          float currentMeasuredP, float currentMeasuredQ, float nominalRefFlowGS, float bevToPumpRatio,
                          float dtS, std::size_t horizonSteps, HorizonSample &out,
                          float currentWeightG = 0.0f, float phaseStartWeightG = 0.0f,
                          float currentWaterPumpedMl = 0.0f, float phaseStartPumpedMl = 0.0f,
                          bool volumetricEnabled = true, bool hasExplicitCurrentEffective = false,
                          float currentEffectiveP = 0.0f, float currentEffectiveQ = 0.0f) {
    out = HorizonSample{};
    if (phases == nullptr || phaseCount == 0 || currentPhaseIdx >= phaseCount || !std::isfinite(dtS) || dtS <= 0.0f ||
        horizonSteps == 0) {
        return false;
    }

    const std::size_t h = std::min(horizonSteps, MAX_HORIZON_STEPS);
    const float eta = std::clamp(std::isfinite(bevToPumpRatio) ? bevToPumpRatio : 0.80f, 0.50f, 1.20f);
    const float fallbackRefQ = std::clamp(std::isfinite(nominalRefFlowGS) ? nominalRefFlowGS : 1.8f, 0.3f, 5.0f);

    std::size_t pIdx = currentPhaseIdx;
    float elapsedInPhase = std::max(std::isfinite(currentPhaseElapsedS) ? currentPhaseElapsedS : 0.0f, 0.0f);
    float startP = std::isfinite(currentPhaseStartP) ? currentPhaseStartP : std::max(currentMeasuredP, 0.0f);
    float startQ = std::isfinite(currentPhaseStartQ) ? currentPhaseStartQ : std::max(currentMeasuredQ, 0.0f);
    float estP = std::max(std::isfinite(currentMeasuredP) ? currentMeasuredP : startP, 0.0f);
    float estPumpQ = std::max(std::isfinite(currentMeasuredQ) ? currentMeasuredQ : startQ, 0.0f);
    float lastBaseP = estP;
    float lastBaseQ = estPumpQ;
    float estWeightG = std::max(std::isfinite(currentWeightG) ? currentWeightG : 0.0f, 0.0f);
    float pStartWeightG = std::max(std::isfinite(phaseStartWeightG) ? phaseStartWeightG : 0.0f, 0.0f);
    float estPumpedMl = std::max(std::isfinite(currentWaterPumpedMl) ? currentWaterPumpedMl : 0.0f, 0.0f);
    float pStartPumpedMl = std::max(std::isfinite(phaseStartPumpedMl) ? phaseStartPumpedMl : 0.0f, 0.0f);
    bool onInitialPhase = true;

    for (std::size_t step = 0; step < h; ++step) {
        // Step 0 trusts BrewProcess's current phase; future steps mirror BrewProcess::progress() exit loop
        // (including instant multi-phase skips when exit targets are already satisfied).
        if (step > 0) {
            for (std::size_t skip = 0; skip < phaseCount && (pIdx + 1 < phaseCount); ++skip) {
                const uint8_t exitCode = phases[pIdx].exitStatus(
                    volumetricEnabled, estWeightG, elapsedInPhase, estPumpQ, estP,
                    std::max(0.0f, estPumpedMl - pStartPumpedMl));
                if (exitCode == 0u) {
                    break;
                }
                const float prevDur = phases[pIdx].durationS;
                ++pIdx;
                onInitialPhase = false;
                elapsedInPhase = (exitCode == 2u && prevDur > 0.0f) ? std::max(0.0f, elapsedInPhase - prevDur) : 0.0f;
                pStartWeightG = estWeightG;
                pStartPumpedMl = estPumpedMl;
                const PhaseSpec &nextPh = phases[pIdx];
                startP = nextPh.transitionAdaptive ? estP : lastBaseP;
                startQ = nextPh.transitionAdaptive ? estPumpQ : lastBaseQ;
            }
        }

        const PhaseSpec &ph = phases[pIdx];
        const bool nonControllable = !ph.valveOpen || ph.pumpIsSimple || ph.mode == Mode::SIMPLE;
        const Mode m = nonControllable ? Mode::SIMPLE : ph.mode;

        float effP = 0.0f;
        float effQ = 0.0f;
        if (!nonControllable) {
            if (onInitialPhase && hasExplicitCurrentEffective) {
                effP = std::max(std::isfinite(currentEffectiveP) ? currentEffectiveP : 0.0f, 0.0f);
                effQ = std::max(std::isfinite(currentEffectiveQ) ? currentEffectiveQ : 0.0f, 0.0f);
            } else {
                effP = (ph.targetPressureBar == -1.0f) ? startP : std::max(ph.targetPressureBar, 0.0f);
                effQ = (ph.targetFlowMlS == -1.0f) ? startQ : std::max(ph.targetFlowMlS, 0.0f);
            }
        }

        // Mirror BrewProcess::computeEffectiveTargetsForCurrentPhase(): only the active target ramps.
        const float phaseStartP = (m == Mode::FLOW) ? effP : startP;
        const float phaseStartQ = (m == Mode::FLOW) ? startQ : effQ;

        // Match BrewProcess::transitionAlpha() across TIME, VOLUMETRIC, and PUMPED transitions.
        float alpha = 1.0f;
        if (!nonControllable && ph.easing != Easing::INSTANT) {
            float endValue = 0.0f;
            float startValue = 0.0f;
            if (ph.transitionTarget == TransTarget::VOLUMETRIC && volumetricEnabled) {
                endValue = ph.transitionDurationS;
                const float volTarget = ph.resolvedVolumetricTargetG();
                if (endValue <= 0.0f && volTarget > 0.0f) {
                    endValue = volTarget - pStartWeightG;
                }
                startValue = std::max(0.0f, estWeightG - pStartWeightG);
            } else if (ph.transitionTarget == TransTarget::PUMPED) {
                endValue = ph.transitionDurationS;
                const float pumpedTarget = ph.resolvedPumpedTargetMl();
                if (endValue <= 0.0f && pumpedTarget > 0.0f) {
                    endValue = pumpedTarget;
                }
                startValue = std::max(0.0f, estPumpedMl - pStartPumpedMl);
            }
            if (endValue > 0.0f) {
                alpha = applyEasing(startValue / endValue, ph.easing);
            } else {
                const float transDur = (ph.transitionDurationS > 0.0f) ? ph.transitionDurationS : ph.durationS;
                if (transDur > 0.0f) {
                    alpha = applyEasing(elapsedInPhase / transDur, ph.easing);
                }
            }
        }

        const float pVal = nonControllable ? 0.0f : (phaseStartP + (effP - phaseStartP) * alpha);
        const float qVal = nonControllable ? 0.0f : (phaseStartQ + (effQ - phaseStartQ) * alpha);

        out.modes[step] = m;
        out.basePressureBar[step] = std::clamp(std::isfinite(pVal) ? pVal : 0.0f, 0.0f, 12.0f);
        out.baseFlowMlS[step] = std::clamp(std::isfinite(qVal) ? qVal : 0.0f, 0.0f, 20.0f);

        if (nonControllable) {
            out.refBeverageFlowGS[step] = 0.0f;
        } else if (m == Mode::FLOW && out.baseFlowMlS[step] > 0.1f) {
            out.refBeverageFlowGS[step] = std::clamp(eta * out.baseFlowMlS[step], 0.2f, 5.5f);
        } else {
            float refQ = fallbackRefQ;
            if (m == Mode::PRESSURE && out.baseFlowMlS[step] > 0.1f) {
                refQ = std::min(refQ, std::max(0.2f, eta * out.baseFlowMlS[step]));
            }
            out.refBeverageFlowGS[step] = refQ;
        }

        lastBaseP = out.basePressureBar[step];
        lastBaseQ = out.baseFlowMlS[step];

        // Advance dry-run hydraulic estimates for future phase target evaluation.
        if (m == Mode::PRESSURE) {
            estP = estP + 0.55f * (out.basePressureBar[step] - estP);
            const float targetPumpQ = out.refBeverageFlowGS[step] / eta;
            estPumpQ = (out.baseFlowMlS[step] > 0.1f) ? std::min(targetPumpQ, out.baseFlowMlS[step]) : targetPumpQ;
        } else if (m == Mode::FLOW) {
            estPumpQ = estPumpQ + 0.65f * (out.baseFlowMlS[step] - estPumpQ);
            if (out.basePressureBar[step] > 0.1f) {
                estP = std::min(estP, out.basePressureBar[step]);
            }
        } else {
            estPumpQ = 0.0f;
        }

        elapsedInPhase += dtS;
        estWeightG += out.refBeverageFlowGS[step] * dtS;
        estPumpedMl += std::max(estPumpQ, 0.0f) * dtS;
    }

    out.steps = h;
    return true;
}

} // namespace ProfileHorizon

#endif // PROFILEHORIZONSAMPLER_H
