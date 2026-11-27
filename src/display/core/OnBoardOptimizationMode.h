#ifndef ONBOARD_OPTIMIZATION_MODE_H
#define ONBOARD_OPTIMIZATION_MODE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace OnBoardOpt {

/// Three user-selectable on-board CPBO search spaces on ESP32-S3:
/// - RecipeOnly: 3D [grind, dose, yield] + Level 1 Assist-Only MPC (spline locked to 0,0,0)
/// - SplineOnly: 3D [early, mid, late] B-spline profile warp in g/s (recipe stays fixed)
/// - Joint6D:    6D [grind, dose, yield, early, mid, late] co-optimized
enum class ControlMode : std::uint8_t {
    RecipeOnly = 0,
    SplineOnly = 1,
    Joint6D = 2,
};

constexpr std::size_t SPLINE_KNOT_COUNT = 3;
constexpr float SPLINE_MAX_ABS_DELTA_GS = 1.0f;
constexpr float SPLINE_RESOLUTION_GS = 0.05f;
constexpr std::array<float, SPLINE_KNOT_COUNT> INITIAL_EXPLORATION_SPLINE_GS = {-0.15f, 0.15f, -0.25f};

inline const char *controlModeKey(ControlMode mode) {
    switch (mode) {
    case ControlMode::SplineOnly:
        return "spline_only";
    case ControlMode::Joint6D:
        return "joint_6d";
    case ControlMode::RecipeOnly:
    default:
        return "recipe_only";
    }
}

inline bool parseControlModeKey(const std::string &key, ControlMode &outMode) {
    if (key == "recipe_only") {
        outMode = ControlMode::RecipeOnly;
        return true;
    }
    if (key == "spline_only") {
        outMode = ControlMode::SplineOnly;
        return true;
    }
    if (key == "joint_6d") {
        outMode = ControlMode::Joint6D;
        return true;
    }
    return false;
}

inline ControlMode controlModeFromByte(std::uint8_t raw) {
    if (raw == static_cast<std::uint8_t>(ControlMode::SplineOnly)) {
        return ControlMode::SplineOnly;
    }
    if (raw == static_cast<std::uint8_t>(ControlMode::Joint6D)) {
        return ControlMode::Joint6D;
    }
    return ControlMode::RecipeOnly;
}

inline std::size_t rawDimForMode(ControlMode mode) {
    return (mode == ControlMode::Joint6D) ? 6u : 3u;
}

inline bool modeTunesRecipe(ControlMode mode) {
    return mode == ControlMode::RecipeOnly || mode == ControlMode::Joint6D;
}

inline bool modeTunesSpline(ControlMode mode) {
    return mode == ControlMode::SplineOnly || mode == ControlMode::Joint6D;
}

inline bool isValidSplineDelta(const std::array<float, SPLINE_KNOT_COUNT> &delta,
                               float maxAbs = SPLINE_MAX_ABS_DELTA_GS) {
    if (!std::isfinite(maxAbs) || maxAbs <= 0.0f) {
        return false;
    }
    for (float v : delta) {
        if (!std::isfinite(v) || v < -maxAbs - 1e-5f || v > maxAbs + 1e-5f) {
            return false;
        }
    }
    return true;
}

inline float quantizeSplineKnot(float value, float maxAbs = SPLINE_MAX_ABS_DELTA_GS,
                                float resolution = SPLINE_RESOLUTION_GS) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    const float clipped = std::clamp(value, -maxAbs, maxAbs);
    if (!std::isfinite(resolution) || resolution <= 0.0f) {
        return clipped;
    }
    const float steps = std::floor(((clipped + maxAbs) / resolution) + 0.5f + 1e-6f);
    const float q = std::clamp(-maxAbs + steps * resolution, -maxAbs, maxAbs);
    return (std::fabs(q) < 0.25f * resolution) ? 0.0f : q;
}

inline std::array<float, SPLINE_KNOT_COUNT> quantizeSplineDelta(const std::array<float, SPLINE_KNOT_COUNT> &raw,
                                                                float maxAbs = SPLINE_MAX_ABS_DELTA_GS,
                                                                float resolution = SPLINE_RESOLUTION_GS) {
    return {
        quantizeSplineKnot(raw[0], maxAbs, resolution),
        quantizeSplineKnot(raw[1], maxAbs, resolution),
        quantizeSplineKnot(raw[2], maxAbs, resolution),
    };
}

inline double splineKnotToNorm(float deltaGs, float maxAbs = SPLINE_MAX_ABS_DELTA_GS) {
    if (!std::isfinite(deltaGs) || !std::isfinite(maxAbs) || maxAbs <= 0.0f) {
        return 0.5;
    }
    const double clipped = std::clamp(static_cast<double>(deltaGs), -static_cast<double>(maxAbs),
                                      static_cast<double>(maxAbs));
    return std::clamp((clipped + static_cast<double>(maxAbs)) / (2.0 * static_cast<double>(maxAbs)), 0.0, 1.0);
}

inline float normToSplineKnot(double normVal, float maxAbs = SPLINE_MAX_ABS_DELTA_GS,
                              float resolution = SPLINE_RESOLUTION_GS) {
    if (!std::isfinite(normVal) || !std::isfinite(maxAbs) || maxAbs <= 0.0f) {
        return 0.0f;
    }
    const double u = std::clamp(normVal, 0.0, 1.0);
    const float raw = static_cast<float>(-static_cast<double>(maxAbs) + u * (2.0 * static_cast<double>(maxAbs)));
    return quantizeSplineKnot(raw, maxAbs, resolution);
}

/// Physical shape invariants of a 3-knot clamped quadratic B-spline [d0, d1, d2]:
///   0: mean flow warp      = (d0 + 2*d1 + d2) / 4   (average extraction speed shift)
///   1: ramp/decline slope  = d2 - d0                (lever decline vs. rising flow)
///   2: mid-shot curvature  = d1 - 0.5 * (d0 + d2)   (mid-shot peak vs. dip)
inline std::array<double, 3> computeSplineShapeFeatures(const std::array<float, SPLINE_KNOT_COUNT> &delta) {
    const double d0 = std::isfinite(delta[0]) ? static_cast<double>(delta[0]) : 0.0;
    const double d1 = std::isfinite(delta[1]) ? static_cast<double>(delta[1]) : 0.0;
    const double d2 = std::isfinite(delta[2]) ? static_cast<double>(delta[2]) : 0.0;
    return {
        0.25 * (d0 + 2.0 * d1 + d2),
        d2 - d0,
        d1 - 0.5 * (d0 + d2),
    };
}

/// Deterministic candidate generator for 3D (4x4x4 grid) or 6D (12 axis probes + 52 R_6 Kronecker points).
/// Candidate 0 is always the anchor point so MES evaluates all other candidates against index 0.
inline bool generateTrustRegionCandidates(ControlMode mode, const std::vector<double> &anchorNorm,
                                          const std::vector<double> &lo, const std::vector<double> &hi,
                                          std::vector<std::array<double, 6>> &outCandidates) {
    outCandidates.clear();
    const std::size_t dim = rawDimForMode(mode);
    if (anchorNorm.size() < dim || lo.size() < dim || hi.size() < dim) {
        return false;
    }
    std::array<double, 6> anc{0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    for (std::size_t d = 0; d < dim; ++d) {
        if (!std::isfinite(anchorNorm[d]) || !std::isfinite(lo[d]) || !std::isfinite(hi[d]) || hi[d] < lo[d]) {
            return false;
        }
        anc[d] = std::clamp(anchorNorm[d], 0.0, 1.0);
    }

    outCandidates.reserve(65);
    outCandidates.push_back(anc); // Index 0 = incumbent anchor

    if (dim == 3) {
        for (int i0 = 0; i0 < 4; ++i0) {
            for (int i1 = 0; i1 < 4; ++i1) {
                for (int i2 = 0; i2 < 4; ++i2) {
                    std::array<double, 6> c{0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
                    c[0] = std::clamp(lo[0] + (static_cast<double>(i0) + 0.5) * 0.25 * (hi[0] - lo[0]), 0.0, 1.0);
                    c[1] = std::clamp(lo[1] + (static_cast<double>(i1) + 0.5) * 0.25 * (hi[1] - lo[1]), 0.0, 1.0);
                    c[2] = std::clamp(lo[2] + (static_cast<double>(i2) + 0.5) * 0.25 * (hi[2] - lo[2]), 0.0, 1.0);
                    outCandidates.push_back(c);
                }
            }
        }
        return true;
    }

    // 6D: 12 axis-aligned local probes (+/- half-span along each dimension from anchor)
    for (std::size_t d = 0; d < 6; ++d) {
        const double span = std::max(hi[d] - lo[d], 0.0);
        const double step = 0.35 * span;
        for (double sgn : {-1.0, 1.0}) {
            std::array<double, 6> c = anc;
            c[d] = std::clamp(anc[d] + sgn * step, lo[d], hi[d]);
            outCandidates.push_back(c);
        }
    }

    // Plus 52 deterministic low-discrepancy generalized golden-ratio R_6 quasirandom points
    // (phi_6^7 = phi_6 + 1, phi_6 ~= 1.1127756842787055; alpha_d = 1 / phi_6^(d+1)).
    static constexpr std::array<double, 6> kAlpha6 = {
        0.8986537146418022,
        0.8075784987616143,
        0.7257334190845694,
        0.6521830240669428,
        0.5860866948019480,
        0.5266890007314518,
    };
    for (std::size_t idx = 1; idx <= 52; ++idx) {
        std::array<double, 6> c{};
        for (std::size_t d = 0; d < 6; ++d) {
            const double raw = 0.5 + static_cast<double>(idx) * kAlpha6[d];
            const double u = raw - std::floor(raw);
            c[d] = std::clamp(lo[d] + u * (hi[d] - lo[d]), 0.0, 1.0);
        }
        outCandidates.push_back(c);
    }
    return true;
}

} // namespace OnBoardOpt

#endif // ONBOARD_OPTIMIZATION_MODE_H
