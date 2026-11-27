#ifndef ESPRESSORL_CORE_EMBEDDED_CPBO_H
#define ESPRESSORL_CORE_EMBEDDED_CPBO_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace EspressoRL {

enum class PreferenceLabel : std::uint8_t {
    NEW_BETTER = 0,
    TIE = 1,
    ANCHOR_BETTER = 2,
};

enum class TrustRegionAction : std::uint8_t {
    NONE = 0,
    IMPROVED = 1,
    NON_IMPROVEMENT = 2,
    EXPANDED = 3,
    CONTRACTED = 4,
    CONVERGED = 5,
};

/// Cooperative yield / cancellation callback invoked periodically during CPBO fitting and MES.
/// Returning false aborts the optimization cleanly (e.g., if a brew starts on Core 0).
using YieldCallback = bool (*)(void *userCtx);

// ---------------------------------------------------------------------------
// High-Precision Scalar Math Utilities (float64)
// ---------------------------------------------------------------------------

double ndtr(double x);
double logNdtr(double x);
double ndtri(double p);
double softplus(double x);
double inverseSoftplus(double value);
double sigmoid(double x);

/// Exact JND trichotomous probabilities [P(NEW_BETTER), P(TIE), P(ANCHOR_BETTER)].
std::array<double, 3> jndProbabilities(double difference, double gamma, double sigmaPref);

// ---------------------------------------------------------------------------
// Physics Proxy Features (phi0) & Robust Scaler
// ---------------------------------------------------------------------------

struct PhysicsProxyConfig {
    bool hasBasketDiameter = false;
    double basketDiameterMm = 58.0;
    bool hasCalibratedParticleSize = false;
    double calibratedCoarseParticleSizeUm = 600.0;
    double calibratedFineParticleSizeUm = 200.0;
    double bulkDensityGCm3 = 0.35;
    double packingFraction = 0.60;
    double particleSizeBeta = 3.0;
    double nominalPressureBar = 9.0;
    double nominalWaterDensityGCm3 = 1.0;
    double epsilon = 1e-9;
    double robustScaleFloor = 1e-6;
};

constexpr std::size_t PHYSICS_PROXY_DIM = 9;

/// Exact C++17 port of cpbo_physics.phi0 / phi0_numpy (9 physics proxy coordinates).
std::array<double, PHYSICS_PROXY_DIM> computePhi0(double finenessNormalized, double doseG, double targetOutputG,
                                                  const PhysicsProxyConfig &cfg = PhysicsProxyConfig{});

struct EmbeddedRobustScaler {
    std::vector<double> median;
    std::vector<double> scale;

    bool fit(const double *rows, std::size_t nRows, std::size_t nCols, double scaleFloor = 1e-6);
    bool transformRow(const double *inRow, double *outRow) const;
};

// ---------------------------------------------------------------------------
// Dimension-Generic Level 1 TuRPBO Trust Region (3D or 3+K spaces)
// ---------------------------------------------------------------------------

struct TrustRegionConfig {
    double initialLength = 0.8;
    double minimumLength = 0.0078125; // 0.5^7
    double maximumLength = 1.6;
    int successTolerance = 3;
    int failureTolerance = 2;
    double shapeFactorMin = 0.20;
    double shapeFactorMax = 5.0;
};

struct Level1TrustRegionState {
    std::vector<double> center;
    double length = 0.1;
    int successCount = 0;
    int failureCount = 0;
    bool locallyConverged = false;
    TrustRegionAction lastAction = TrustRegionAction::NONE;
};

bool updateLevel1TrustRegion(const Level1TrustRegionState &state, PreferenceLabel label,
                             const std::vector<double> &candidateCenter, const TrustRegionConfig &cfg,
                             Level1TrustRegionState &outState);

bool computeLevel1TrustRegionBounds(const Level1TrustRegionState &state, const std::vector<double> &lengthscales,
                                    const TrustRegionConfig &cfg, std::vector<double> &outLower,
                                    std::vector<double> &outUpper);

// ---------------------------------------------------------------------------
// Full-Fidelity 20-pt Gauss-Hermite VI Preference GP + Envelope-Theorem L-BFGS
// ---------------------------------------------------------------------------

struct PreferenceGPConfig {
    double sigmaPref = 0.20;
    double initialGamma = 0.20;
    bool learnGamma = true;
    double covarianceJitter = 1e-6;
    double probabilityEpsilon = 1e-12;
    double initialRawKernelWeight = 0.80;
    double initialPhysicsKernelWeight = 0.20;
    double initialTraceKernelWeight = 0.0;
    int maxOuterIters = 45;
    int maxInnerIters = 35;
    int finalInnerIters = 50;
};

struct EmbeddedKernelHyperparams {
    std::vector<double> rawLogLengthscales;
    std::vector<double> physicsLogLengthscales;
    std::vector<double> traceLogLengthscales;
    std::vector<double> weightLogits;

    std::vector<double> rawLengthscales() const;
    std::vector<double> physicsLengthscales() const;
    std::vector<double> traceLengthscales() const;
    std::vector<double> mixtureWeights() const;
};

struct ComparisonPair {
    std::size_t newIdx = 0;
    std::size_t anchorIdx = 0;
    PreferenceLabel label = PreferenceLabel::TIE;
};

struct EmbeddedPreferenceGPModel {
    bool valid = false;
    bool converged = false;
    bool cancelled = false;
    int outerIterations = 0;
    std::size_t trainCount = 0;
    std::size_t featureStride = 0;
    std::size_t rawDim = 0;
    std::size_t physicsDim = 0;
    std::size_t traceDim = 0;
    std::vector<double> trainInputs; // row-major [trainCount * featureStride]
    EmbeddedKernelHyperparams hyperparams;
    double gamma = 0.20;
    double sigmaPref = 0.20;
    double jitterUsed = 1e-6;
    double elbo = 0.0;
    std::vector<double> cholK;        // row-major [N * N]
    std::vector<double> cholB;        // row-major [N * N]
    std::vector<double> whitenedMean; // [N]
    std::vector<double> dualLambda;   // [C]

    /// Exact GP posterior prediction using triangular solves on L_K and L_B (never forms K^{-1}).
    /// Optional queryPriorMean [queryCount] adds a fixed non-zero prior mean m_0(x_*).
    bool predict(const double *queryInputs, std::size_t queryCount, std::vector<double> &outMean,
                 std::vector<double> &outCov, const double *queryPriorMean = nullptr) const;
};

/// Evaluate exact inner-converged ELBO and Envelope-Theorem gradient w.r.t. packed theta vector:
/// [rawLogLs(rawDim), physLogLs(physDim), traceLogLs(traceDim), weightLogits(2 or 3), (rawGamma if learnGamma)].
bool evaluatePreferenceGpObjectiveAndGrad(const double *trainInputs, std::size_t trainCount, std::size_t rawDim,
                                          std::size_t physicsDim, std::size_t traceDim,
                                          const ComparisonPair *comparisons, std::size_t comparisonCount,
                                          const PreferenceGPConfig &cfg, const std::vector<double> &packedTheta,
                                          double &outNegElbo, std::vector<double> &outGrad);

/// Fit full-fidelity 20-pt Gauss-Hermite VI Preference GP in pure float64 STL C++17.
/// Optional trainPriorMean [trainCount] specifies a fixed prior mean m_0(x_i) so f(x) = m_0(x) + g(x).
bool fitEmbeddedPreferenceGP(const double *trainInputs, std::size_t trainCount, std::size_t rawDim,
                             std::size_t physicsDim, std::size_t traceDim, const ComparisonPair *comparisons,
                             std::size_t comparisonCount, const PreferenceGPConfig &cfg,
                             EmbeddedPreferenceGPModel &outModel, YieldCallback onYield = nullptr,
                             void *yieldCtx = nullptr, const double *trainPriorMean = nullptr);

// ---------------------------------------------------------------------------
// Conditional-Coordinate Gauss-Legendre MES + Robert-Gibbs Far-Tail Fallback
// ---------------------------------------------------------------------------

struct MESConfig {
    bool usePaperGumbel = true;
    std::size_t posteriorMaxFunctionSamples = 512;
    std::size_t gumbelMaximumSamples = 2000;
    std::size_t maximumValueBins = 10;
    std::size_t truncatedSamplesPerBin = 256;
    double rankEpsilon = 1e-4;
    std::size_t gibbsBurnIn = 96;
    std::size_t gibbsThinning = 2;
    double varianceRoundoffFloor = 1e-12;
    double entropyEpsilon = 1e-12;
    double covarianceJitter = 1e-6;
};

struct EmbeddedMESResult {
    bool valid = false;
    bool cancelled = false;
    std::size_t candidateIndex = 0;
    double acquisitionValue = 0.0;
    std::array<double, 3> outcomeProbabilities{0.0, 0.0, 0.0};
    std::vector<double> allAcquisitionValues;
    std::uint32_t quadratureEvaluations = 0;
    std::uint32_t gibbsFallbackCount = 0;
};

/// Compute E[P(R | f_x - f_a, gamma) | f_x <= u_x, f_a <= u_a] via 24x20 Gauss-Legendre quadrature
/// with Robert-Gibbs fallback for extreme lower tails (alpha < -4.5) or |rho| > 0.9995.
bool conditionalJndProbsBivariateTruncated(double meanX, double meanA, double covXX, double covXA, double covAA,
                                           double upperX, double upperA, double gamma, double sigmaPref,
                                           std::uint64_t seed, std::size_t gibbsSamples, std::size_t gibbsBurnIn,
                                           std::size_t gibbsThinning, std::array<double, 3> &outProbs,
                                           bool &outUsedGibbs);

/// Pure float64 CPBO-MES using O(K^2) centering + Gumbel max bins + conditional Gauss-Legendre quadrature.
bool evaluateEmbeddedCpboMes(const double *posteriorMean, const double *posteriorCov, std::size_t pointCount,
                             const std::size_t *candidateIndices, std::size_t candidateCount,
                             const std::size_t *maximumIndices, std::size_t maximumCount, std::size_t anchorIndex,
                             double gamma, double sigmaPref, const MESConfig &cfg, std::uint64_t seed,
                             EmbeddedMESResult &outResult, YieldCallback onYield = nullptr, void *yieldCtx = nullptr);

} // namespace EspressoRL

#endif // ESPRESSORL_CORE_EMBEDDED_CPBO_H
