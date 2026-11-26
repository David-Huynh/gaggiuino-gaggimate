#ifndef LIVEBREWLEASE_H
#define LIVEBREWLEASE_H

#include "LiveBrewTargetUpdate.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

enum class LiveLeasePumpMode : uint8_t {
    SIMPLE = 0,
    PRESSURE = 1,
    FLOW = 2,
};

struct LiveBrewLeaseConfig {
    uint32_t maxLeaseMs = 500;
    uint32_t hardLeaseCapMs = 1500;
    float minPressureBar = LIVE_BREW_MIN_PRESSURE_BAR;
    float maxPressureBar = LIVE_BREW_MAX_PRESSURE_BAR;
    float minFlowMlS = LIVE_BREW_MIN_FLOW_ML_S;
    float maxFlowMlS = LIVE_BREW_MAX_FLOW_ML_S;
    float maxBasePressureDeltaBar = 2.5f;
    float maxBaseFlowDeltaMlS = 1.2f;
    float maxPressureSlewBarS = 3.0f;
    float maxFlowSlewMlS2 = 4.0f;
};

struct LiveBrewLeaseCommand {
    LiveLeasePumpMode mode = LiveLeasePumpMode::PRESSURE;
    float pressureTargetBar = 0.0f;
    float flowTargetMlS = 0.0f;
    uint32_t leaseDurationMs = 500;
    bool shadowOnly = true; // Safe default: Shadow Mode logs without actuating.
};

struct LiveBrewLeaseDiagnostics {
    uint32_t evaluatedCount = 0;
    uint32_t appliedCount = 0;
    uint32_t shadowCount = 0;
    uint32_t rejectedCount = 0;
    uint32_t expiredCount = 0;
    float lastBaseTarget = 0.0f;
    float lastProposedTarget = 0.0f;
    float lastClampedTarget = 0.0f;
    LiveLeasePumpMode lastMode = LiveLeasePumpMode::SIMPLE;
    bool lastWasShadow = true;
    bool currentlyActive = false;
};

/// Zero-trust firmware safety gate and deadman lease for live brew setpoint overrides.
///
/// Guarantees:
/// 1. Base profile phase definitions are never mutated.
/// 2. Proposed overrides are strictly validated for finiteness, mode match, hard bounds,
///    base-profile corridor (+/-delta), and slew-rate limit.
/// 3. Shadow Mode evaluates, clamps, and records telemetry while always keeping actuation off.
/// 4. Wrap-safe deadman lease (default 500 ms) automatically reverts to the untouched base
///    profile if updates stop arriving or when the phase transitions.
class LiveBrewLease {
  public:
    explicit LiveBrewLease(const LiveBrewLeaseConfig &cfg = LiveBrewLeaseConfig{}) : cfg_(cfg) {}

    void clear() {
        active_ = false;
        hasPrevTarget_ = false;
        diag_.currentlyActive = false;
    }

    void resetAll() {
        clear();
        diag_ = LiveBrewLeaseDiagnostics{};
    }

    bool submit(const LiveBrewLeaseCommand &cmd, LiveLeasePumpMode baseMode, float basePressureBar, float baseFlowMlS,
                uint32_t nowMs, bool forceShadowMode = false) {
        ++diag_.evaluatedCount;

        if (baseMode == LiveLeasePumpMode::SIMPLE || cmd.mode != baseMode || cmd.leaseDurationMs == 0) {
            ++diag_.rejectedCount;
            return false;
        }
        if (!std::isfinite(basePressureBar) || !std::isfinite(baseFlowMlS)) {
            ++diag_.rejectedCount;
            clear();
            return false;
        }

        const bool isPressure = (baseMode == LiveLeasePumpMode::PRESSURE);
        const float proposed = isPressure ? cmd.pressureTargetBar : cmd.flowTargetMlS;
        if (!std::isfinite(proposed)) {
            ++diag_.rejectedCount;
            return false;
        }

        const float minAbs = isPressure ? cfg_.minPressureBar : cfg_.minFlowMlS;
        const float maxAbs = isPressure ? cfg_.maxPressureBar : cfg_.maxFlowMlS;
        const float maxDelta = isPressure ? cfg_.maxBasePressureDeltaBar : cfg_.maxBaseFlowDeltaMlS;
        const float maxSlewPerS = isPressure ? cfg_.maxPressureSlewBarS : cfg_.maxFlowSlewMlS2;
        const float baseClamped = std::clamp(isPressure ? basePressureBar : baseFlowMlS, minAbs, maxAbs);

        const float lo = std::max(minAbs, baseClamped - maxDelta);
        const float hi = std::min(maxAbs, baseClamped + maxDelta);
        float target = std::clamp(proposed, lo, hi);

        // Apply slew limit relative to last issued target (or base target on first tick).
        float dtS = 0.25f;
        float prevVal = baseClamped;
        if (hasPrevTarget_ && prevMode_ == baseMode) {
            const uint32_t elapsedMs = static_cast<uint32_t>(nowMs - lastUpdateMs_);
            dtS = std::clamp(static_cast<float>(elapsedMs) / 1000.0f, 0.05f, 1.0f);
            prevVal = prevTarget_;
        }
        const float maxStep = maxSlewPerS * dtS;
        target = std::clamp(target, prevVal - maxStep, prevVal + maxStep);
        target = std::clamp(target, minAbs, maxAbs);

        diag_.lastMode = baseMode;
        diag_.lastBaseTarget = baseClamped;
        diag_.lastProposedTarget = proposed;
        diag_.lastClampedTarget = target;

        const bool shadow = forceShadowMode || cmd.shadowOnly;
        diag_.lastWasShadow = shadow;
        if (shadow) {
            active_ = false;
            diag_.currentlyActive = false;
            ++diag_.shadowCount;
            return true;
        }

        const uint32_t maxAllowedLease = std::min(cfg_.maxLeaseMs, cfg_.hardLeaseCapMs);
        activeLeaseMs_ = std::min(cmd.leaseDurationMs, std::max<uint32_t>(maxAllowedLease, 50u));
        issuedAtMs_ = nowMs;
        lastUpdateMs_ = nowMs;
        leasedMode_ = baseMode;
        leasedPressureBar_ = isPressure ? target : basePressureBar;
        leasedFlowMlS_ = isPressure ? baseFlowMlS : target;
        prevTarget_ = target;
        prevMode_ = baseMode;
        hasPrevTarget_ = true;
        active_ = true;
        diag_.currentlyActive = true;
        ++diag_.appliedCount;
        return true;
    }

    /// Resolve effective control target at nowMs. Returns true if an active lease overrode
    /// the base profile, or false if the untouched base profile is being used.
    bool resolve(uint32_t nowMs, LiveLeasePumpMode baseMode, float basePressureBar, float baseFlowMlS,
                 LiveLeasePumpMode &outMode, float &outPressureBar, float &outFlowMlS) {
        outMode = baseMode;
        outPressureBar = basePressureBar;
        outFlowMlS = baseFlowMlS;

        if (!active_) {
            diag_.currentlyActive = false;
            return false;
        }

        const uint32_t elapsedMs = static_cast<uint32_t>(nowMs - issuedAtMs_);
        if (elapsedMs > activeLeaseMs_ || baseMode != leasedMode_ || baseMode == LiveLeasePumpMode::SIMPLE) {
            active_ = false;
            diag_.currentlyActive = false;
            ++diag_.expiredCount;
            return false;
        }

        // Re-enforce corridor against the live base profile at read time so base ramps
        // can never be violated mid-lease.
        if (leasedMode_ == LiveLeasePumpMode::PRESSURE) {
            const float base = std::clamp(basePressureBar, cfg_.minPressureBar, cfg_.maxPressureBar);
            const float lo = std::max(cfg_.minPressureBar, base - cfg_.maxBasePressureDeltaBar);
            const float hi = std::min(cfg_.maxPressureBar, base + cfg_.maxBasePressureDeltaBar);
            outPressureBar = std::clamp(leasedPressureBar_, lo, hi);
        } else {
            const float base = std::clamp(baseFlowMlS, cfg_.minFlowMlS, cfg_.maxFlowMlS);
            const float lo = std::max(cfg_.minFlowMlS, base - cfg_.maxBaseFlowDeltaMlS);
            const float hi = std::min(cfg_.maxFlowMlS, base + cfg_.maxBaseFlowDeltaMlS);
            outFlowMlS = std::clamp(leasedFlowMlS_, lo, hi);
        }
        return true;
    }

    bool isActive() const { return active_; }
    const LiveBrewLeaseDiagnostics &diagnostics() const { return diag_; }
    const LiveBrewLeaseConfig &config() const { return cfg_; }

  private:
    LiveBrewLeaseConfig cfg_{};
    bool active_ = false;
    uint32_t issuedAtMs_ = 0;
    uint32_t activeLeaseMs_ = 0;
    uint32_t lastUpdateMs_ = 0;
    LiveLeasePumpMode leasedMode_ = LiveLeasePumpMode::SIMPLE;
    float leasedPressureBar_ = 0.0f;
    float leasedFlowMlS_ = 0.0f;
    bool hasPrevTarget_ = false;
    LiveLeasePumpMode prevMode_ = LiveLeasePumpMode::SIMPLE;
    float prevTarget_ = 0.0f;
    LiveBrewLeaseDiagnostics diag_{};
};

#endif // LIVEBREWLEASE_H
