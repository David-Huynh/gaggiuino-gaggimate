#ifndef ESPRESSORL_CORE_HYDRAULIC_WORLD_MODEL_H
#define ESPRESSORL_CORE_HYDRAULIC_WORLD_MODEL_H

#include "HydraulicControl.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace EspressoRL {

constexpr std::size_t HYDRAULIC_FEATURE_DIM = 4;
constexpr std::size_t MAX_NYSTROM_BASIS = 64;
constexpr std::size_t MAX_MPC_HORIZON = 24;

struct HydraulicPhysicsConfig {
    float dtS = 0.25f;
    int substeps = 2;
    float baseLogConductance = -0.45f;
    float grindStepSensitivity = 0.035f;
    float referenceDoseG = 18.0f;
    float compliancePuckMlPerBar = 1.2f;
    float complianceInitMlPerBar = 5.0f;
    float complianceVminMl = 6.0f;
    float complianceVfillMl = 4.0f;
    float pumpQ0MlS = 11.5f;
    float pumpSlopeMlSPerBar = 0.55f;
    float pressureTrackTauS = 0.35f;
    float flowTrackAlpha = 0.75f;
    float initialBevToPumpRatio = 0.80f;
    float logConductanceMin = -3.2f;
    float logConductanceMax = 1.6f;
    float obsFlowNoiseStdGS = 0.38f;
    float obsPressureNoiseStdBar = 0.28f;
    float obsLogConductanceNoiseStd = 0.75f;
};

struct ResidualGPConfig {
    std::size_t basisSize = 64;
    std::size_t scaffoldGridV = 4;
    std::size_t scaffoldGridP = 4;
    float signalVariance = 0.20f;
    std::array<float, HYDRAULIC_FEATURE_DIM> lengthscales{0.60f, 1.60f, 0.90f, 1.20f};
    float shotForgettingFactor = 0.95f;
    double nystromJitter = 1e-5;
    float pivotedCholeskyMinDiag = 1e-3f;
};

struct Matern32FilterConfig {
    float signalStd = 0.30f;
    float lengthscaleS = 4.5f;
    float measurementStd = 0.24f;
};

using FeatureVec4 = std::array<float, HYDRAULIC_FEATURE_DIM>;

FeatureVec4 buildHydraulicFeature(float waterPumpedMl, float pressureBar, float relativeGrindSteps, float doseInG);
float physicsPriorLogConductance(float relativeGrindSteps, float doseInG, const HydraulicPhysicsConfig &cfg);
float effectiveCompliance(float waterPumpedMl, const HydraulicPhysicsConfig &cfg);

/// In-place float32 Givens-rotation QR square-root Bayesian linear regression.
/// Zero dynamic allocations during live ticks; fits completely in ~16 KB SRAM at M=64.
class SquareRootBayesianLinearF32 {
  public:
    explicit SquareRootBayesianLinearF32(std::size_t dim = 16, float forgetting = 0.95f);

    void reset(std::size_t dim);
    std::size_t dim() const { return dim_; }

    /// Apply mild cross-shot forgetting while preserving prior precision I_M.
    void applyShotForgetting(float forgettingFactor);

    /// In-place Givens-rotation rank-1 QR update for one feature vector phi and scalar target y.
    void updateRow(const float *phi, float y, float invNoiseStd);

    /// Re-solve R * mu_w = rhs after a batch of updateRow calls.
    void solveWeights();

    /// Read-only access to the solved posterior mean weights mu_w (length MAX_NYSTROM_BASIS).
    const float *weights() const { return muW_.data(); }

    /// Predict (mean, phi^T Sigma_w phi) in float32 via forward triangular solve on R^T.
    void predict(const float *phi, float &outMean, float &outVar) const;

  private:
    std::size_t dim_ = 0;
    float forgetting_ = 0.95f;
    std::array<float, MAX_NYSTROM_BASIS * MAX_NYSTROM_BASIS> rMat_{};
    std::array<float, MAX_NYSTROM_BASIS> rhs_{};
    std::array<float, MAX_NYSTROM_BASIS> muW_{};

    float &rAt(std::size_t row, std::size_t col) { return rMat_[row * MAX_NYSTROM_BASIS + col]; }
    float rAt(std::size_t row, std::size_t col) const { return rMat_[row * MAX_NYSTROM_BASIS + col]; }
};

/// Cross-shot Fixed-Basis VFE Nyström GP with coarse (V, P) scaffold + Pivoted Cholesky.
/// Between shots: selects anchors and factors L_MM^-1 in float64, then casts to float32.
/// In-shot: runs strictly in float32 with exact VFE out-of-distribution prior variance restoration.
class FixedBasisVfeNystromGP {
  public:
    explicit FixedBasisVfeNystromGP(const ResidualGPConfig &cfg = ResidualGPConfig{});

    void reset();
    std::size_t basisSize() const { return anchorCount_; }
    std::uint32_t choleskyFallbackCount() const { return choleskyFallbacks_; }

    void predict(const FeatureVec4 &z, float &outMean, float &outVar) const;
    /// Fast exact O(M) posterior mean prediction using precomputed L_MM^{-T} * mu_w (zero stack buffers).
    float predictMean(const FeatureVec4 &z) const;
    float novelty(const FeatureVec4 &z) const;

    /// Fast float32 QR update from one completed shot (keeps existing anchors).
    void updateFromShot(const FeatureVec4 *zRows, const float *yValues, std::size_t count, float noiseVar);

    /// Between-shot Pivoted Cholesky anchor selection + float64 L_MM^-1 factorization.
    void refreshFromHistory(const FeatureVec4 *zRows, const float *yValues, std::size_t count, float noiseVar);

  private:
    ResidualGPConfig cfg_{};
    std::size_t anchorCount_ = 0;
    std::uint32_t choleskyFallbacks_ = 0;
    std::array<FeatureVec4, MAX_NYSTROM_BASIS> anchors_{};
    std::array<float, MAX_NYSTROM_BASIS * MAX_NYSTROM_BASIS> lMmInv_{};
    std::array<float, MAX_NYSTROM_BASIS> alphaW_{};
    SquareRootBayesianLinearF32 core_{};

    void buildScaffold(float grindCenter, float doseCenter, std::vector<FeatureVec4> &out) const;
    void factorAnchorsFloat64();
    void rebuildMeanWeights();
    void computeFeatures(const FeatureVec4 &z, float *outPsi) const;
    float rbfKernelF32(const FeatureVec4 &a, const FeatureVec4 &b) const;
    double rbfKernelF64(const FeatureVec4 &a, const FeatureVec4 &b) const;
};

/// Exact 2-state Matérn-3/2 State-Space GP Kalman filter in float32 (Joseph form).
class Matern32StateSpaceFilter {
  public:
    explicit Matern32StateSpaceFilter(const Matern32FilterConfig &cfg = Matern32FilterConfig{});

    void reset();
    void predict(float dtS);
    void update(float observation, float measurementVar = -1.0f);

    void forecast(float initOffset, float initRate, float cov00, float cov01, float cov11, std::size_t horizonSteps,
                  float dtS, float *outOffsets, float *outVars) const;

    float offset() const { return mean_[0]; }
    float rate() const { return mean_[1]; }
    float cov00() const { return cov_[0]; }
    float cov01() const { return cov_[1]; }
    float cov11() const { return cov_[3]; }
    const Matern32FilterConfig &config() const { return cfg_; }

  private:
    Matern32FilterConfig cfg_{};
    float lam_ = 0.0f;
    std::array<float, 4> pInf_{};
    std::array<float, 2> mean_{0.0f, 0.0f};
    std::array<float, 4> cov_{};

    void transitionMatrices(float dtS, float aMat[4], float qMat[4]) const;
    static void stabilizeCov(float cov[4]);
};

struct RolloutOutput {
    std::size_t steps = 0;
    std::array<float, MAX_MPC_HORIZON> pressureBar{};
    std::array<float, MAX_MPC_HORIZON> pressureStdBar{};
    std::array<float, MAX_MPC_HORIZON> beverageFlowGS{};
    std::array<float, MAX_MPC_HORIZON> beverageFlowStdGS{};
    std::array<float, MAX_MPC_HORIZON> weightG{};
    std::array<float, MAX_MPC_HORIZON> weightStdG{};
};

/// Optional caller-owned scratch workspace for CombinedHydraulicPredictor::rollout
/// so real-time embedded callers keep large trajectory arrays off task stacks.
struct RolloutScratch {
    std::array<float, MAX_MPC_HORIZON> liveOffsets{};
    std::array<float, MAX_MPC_HORIZON> liveStds{};
    std::array<float, MAX_MPC_HORIZON> pHi{};
    std::array<float, MAX_MPC_HORIZON> qHi{};
    std::array<float, MAX_MPC_HORIZON> wHi{};
    std::array<float, MAX_MPC_HORIZON> pLo{};
    std::array<float, MAX_MPC_HORIZON> qLo{};
    std::array<float, MAX_MPC_HORIZON> wLo{};
};

/// Unified gray-box predictor combining f_phys + VFE Nyström g_past + Matérn-3/2 delta_live.
class CombinedHydraulicPredictor {
  public:
    explicit CombinedHydraulicPredictor(const HydraulicPhysicsConfig &physCfg = HydraulicPhysicsConfig{},
                                        const ResidualGPConfig &gpCfg = ResidualGPConfig{},
                                        const Matern32FilterConfig &kfCfg = Matern32FilterConfig{});

    void resetShot();
    void resetAll();

    void observeTick(float pressureBar, float beverageFlowGS, float waterPumpedMl, float relativeGrindSteps,
                     float doseInG, bool activeForEstimation);

    void updateBevToPumpRatio(const float *pumpFlow, const float *bevFlow, const uint8_t *activeMask,
                              std::size_t count);

    void rollout(const HydraulicState &state, const PumpMode *modes, const float *pressureTargets,
                 const float *flowTargets, std::size_t horizonSteps, float relativeGrindSteps, float doseInG,
                 bool propagateUncertainty, RolloutOutput &out) const;

    /// Stack-free rollout variant using caller-owned scratch workspace (zero large stack frames).
    void rolloutWithScratch(const HydraulicState &state, const PumpMode *modes, const float *pressureTargets,
                            const float *flowTargets, std::size_t horizonSteps, float relativeGrindSteps,
                            float doseInG, bool propagateUncertainty, RolloutOutput &out,
                            RolloutScratch &scratch) const;

    /// Compute the profile's own physics-induced nominal beverage-flow trajectory by rolling out
    /// the unmodified base profile with zero dynamic disturbance (liveRate = 0, liveOffset = baselineOffset).
    /// Ensures Assist-Only MPC tracks any arbitrary profile (Cremina decline, Extractamundo turbo,
    /// flat 9-bar LH, Direct Lever flow) without fighting it, intervening only on puck disturbances.
    void predictNominalReferenceFlow(const HydraulicState &state, const PumpMode *modes,
                                     const float *basePressureTargets, const float *baseFlowTargets,
                                     std::size_t horizonSteps, float relativeGrindSteps, float doseInG,
                                     float baselineLogConductanceOffset, float *outRefFlowGS) const;

    FixedBasisVfeNystromGP &residualGP() { return residualGP_; }
    const FixedBasisVfeNystromGP &residualGP() const { return residualGP_; }
    Matern32StateSpaceFilter &liveFilter() { return liveFilter_; }
    const Matern32StateSpaceFilter &liveFilter() const { return liveFilter_; }
    const HydraulicPhysicsConfig &physics() const { return physics_; }
    float bevToPumpRatio() const { return bevToPumpRatio_; }
    void setBevToPumpRatio(float ratio) {
        if (std::isfinite(ratio)) {
            bevToPumpRatio_ = std::clamp(ratio, 0.50f, 1.20f);
        }
    }

  private:
    HydraulicPhysicsConfig physics_{};
    FixedBasisVfeNystromGP residualGP_{};
    Matern32StateSpaceFilter liveFilter_{};
    float bevToPumpRatio_ = 0.80f;

    void rolloutSinglePath(const HydraulicState &state, const PumpMode *modes, const float *pressureTargets,
                           const float *flowTargets, std::size_t horizonSteps, float relativeGrindSteps, float doseInG,
                           const float *liveOffsets, const float *liveStds, float sigmaShift, float *outP, float *outQ,
                           float *outW) const;
};

} // namespace EspressoRL

#endif // ESPRESSORL_CORE_HYDRAULIC_WORLD_MODEL_H
