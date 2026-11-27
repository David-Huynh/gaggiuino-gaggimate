#include "HydraulicWorldModel.h"

#include <algorithm>
#include <cmath>

namespace EspressoRL {

FeatureVec4 buildHydraulicFeature(float waterPumpedMl, float pressureBar, float relativeGrindSteps, float doseInG) {
    return FeatureVec4{
        waterPumpedMl / 40.0f,
        pressureBar / 9.0f,
        relativeGrindSteps / 5.0f,
        (doseInG - 18.0f) / 3.0f,
    };
}

float physicsPriorLogConductance(float relativeGrindSteps, float doseInG, const HydraulicPhysicsConfig &cfg) {
    const float doseRatio = std::max(doseInG, 1.0f) / cfg.referenceDoseG;
    const float value = cfg.baseLogConductance - cfg.grindStepSensitivity * relativeGrindSteps - std::log(doseRatio);
    return std::clamp(value, cfg.logConductanceMin, cfg.logConductanceMax);
}

float effectiveCompliance(float waterPumpedMl, const HydraulicPhysicsConfig &cfg) {
    if (waterPumpedMl <= cfg.complianceVminMl) {
        return cfg.complianceInitMlPerBar;
    }
    const float decay = std::exp(-(waterPumpedMl - cfg.complianceVminMl) / cfg.complianceVfillMl);
    return cfg.compliancePuckMlPerBar + (cfg.complianceInitMlPerBar - cfg.compliancePuckMlPerBar) * decay;
}

// ---------------------------------------------------------------------------
// SquareRootBayesianLinearF32 (in-place Givens-rotation QR)
// ---------------------------------------------------------------------------

SquareRootBayesianLinearF32::SquareRootBayesianLinearF32(std::size_t dim, float forgetting)
    : forgetting_(forgetting) {
    reset(dim);
}

void SquareRootBayesianLinearF32::reset(std::size_t dim) {
    dim_ = std::min(dim, MAX_NYSTROM_BASIS);
    rMat_.fill(0.0f);
    rhs_.fill(0.0f);
    muW_.fill(0.0f);
    for (std::size_t i = 0; i < dim_; ++i) {
        rAt(i, i) = 1.0f;
    }
}

void SquareRootBayesianLinearF32::updateRow(const float *phi, float y, float invNoiseStd) {
    if (dim_ == 0 || phi == nullptr || !std::isfinite(y) || !std::isfinite(invNoiseStd)) {
        return;
    }
    std::array<float, MAX_NYSTROM_BASIS> row{};
    for (std::size_t j = 0; j < dim_; ++j) {
        row[j] = phi[j] * invNoiseStd;
        if (!std::isfinite(row[j])) {
            return;
        }
    }
    float rowRhs = y * invNoiseStd;

    // In-place Givens rotations zero out row[i] into upper-triangular rMat_[i, :].
    for (std::size_t i = 0; i < dim_; ++i) {
        const float b = row[i];
        if (std::fabs(b) <= 1e-12f) {
            continue;
        }
        const float a = rAt(i, i);
        const float hyp = std::hypot(a, b);
        if (hyp <= 1e-12f || !std::isfinite(hyp)) {
            continue;
        }
        const float c = a / hyp;
        const float s = b / hyp;
        rAt(i, i) = hyp;
        row[i] = 0.0f;
        for (std::size_t j = i + 1; j < dim_; ++j) {
            const float r_ij = rAt(i, j);
            const float v_j = row[j];
            rAt(i, j) = c * r_ij + s * v_j;
            row[j] = -s * r_ij + c * v_j;
        }
        const float rhs_i = rhs_[i];
        rhs_[i] = c * rhs_i + s * rowRhs;
        rowRhs = -s * rhs_i + c * rowRhs;
    }
}

void SquareRootBayesianLinearF32::applyShotForgetting(float forgettingFactor) {
    if (dim_ == 0 || forgettingFactor >= 1.0f) {
        return;
    }
    const float lam = std::clamp(forgettingFactor, 0.5f, 1.0f);
    const float lamSqrt = std::sqrt(lam);
    const float priorSqrt = std::sqrt(std::max(1.0f - lam, 0.0f));
    for (std::size_t i = 0; i < dim_; ++i) {
        rhs_[i] *= lamSqrt;
        for (std::size_t j = i; j < dim_; ++j) {
            rAt(i, j) *= lamSqrt;
        }
    }
    if (priorSqrt > 1e-6f) {
        std::array<float, MAX_NYSTROM_BASIS> unitRow{};
        for (std::size_t i = 0; i < dim_; ++i) {
            unitRow[i] = 1.0f;
            updateRow(unitRow.data(), 0.0f, priorSqrt);
            unitRow[i] = 0.0f;
        }
    }
}

void SquareRootBayesianLinearF32::solveWeights() {
    if (dim_ == 0) {
        return;
    }
    // Enforce positive diagonal floor before back-substitution.
    for (std::size_t i = 0; i < dim_; ++i) {
        if (!std::isfinite(rAt(i, i)) || rAt(i, i) < 1e-3f) {
            rAt(i, i) = 1e-3f;
        }
    }
    std::array<float, MAX_NYSTROM_BASIS> sol{};
    for (std::size_t rev = 0; rev < dim_; ++rev) {
        const std::size_t i = dim_ - 1 - rev;
        float sum = rhs_[i];
        for (std::size_t j = i + 1; j < dim_; ++j) {
            sum -= rAt(i, j) * sol[j];
        }
        sol[i] = sum / rAt(i, i);
        if (!std::isfinite(sol[i])) {
            return;
        }
    }
    muW_ = sol;
}

void SquareRootBayesianLinearF32::predict(const float *phi, float &outMean, float &outVar) const {
    if (dim_ == 0 || phi == nullptr) {
        outMean = 0.0f;
        outVar = 0.0f;
        return;
    }
    float mean = 0.0f;
    for (std::size_t i = 0; i < dim_; ++i) {
        mean += phi[i] * muW_[i];
    }
    // Forward triangular solve R^T v = phi => var = ||v||_2^2 >= 0
    std::array<float, MAX_NYSTROM_BASIS> v{};
    float var = 0.0f;
    for (std::size_t i = 0; i < dim_; ++i) {
        float sum = phi[i];
        for (std::size_t k = 0; k < i; ++k) {
            sum -= rAt(k, i) * v[k];
        }
        const float diag = std::max(rAt(i, i), 1e-3f);
        v[i] = sum / diag;
        var += v[i] * v[i];
    }
    outMean = std::isfinite(mean) ? mean : 0.0f;
    outVar = (std::isfinite(var) && var > 0.0f) ? var : 0.0f;
}

// ---------------------------------------------------------------------------
// FixedBasisVfeNystromGP
// ---------------------------------------------------------------------------

FixedBasisVfeNystromGP::FixedBasisVfeNystromGP(const ResidualGPConfig &cfg)
    : cfg_(cfg), core_(16, cfg.shotForgettingFactor) {
    reset();
}

void FixedBasisVfeNystromGP::reset() {
    choleskyFallbacks_ = 0;
    std::vector<FeatureVec4> scaffold;
    buildScaffold(0.0f, 18.0f, scaffold);
    anchorCount_ = std::min(scaffold.size(), std::min(cfg_.basisSize, MAX_NYSTROM_BASIS));
    for (std::size_t i = 0; i < anchorCount_; ++i) {
        anchors_[i] = scaffold[i];
    }
    factorAnchorsFloat64();
    core_.reset(anchorCount_);
    rebuildMeanWeights();
}

void FixedBasisVfeNystromGP::buildScaffold(float grindCenter, float doseCenter, std::vector<FeatureVec4> &out) const {
    out.clear();
    const std::size_t nv = std::max<std::size_t>(cfg_.scaffoldGridV, 1);
    const std::size_t np = std::max<std::size_t>(cfg_.scaffoldGridP, 1);
    out.reserve(nv * np);
    for (std::size_t iv = 0; iv < nv; ++iv) {
        const float v = (nv == 1) ? 28.0f : (6.0f + (46.0f * static_cast<float>(iv)) / static_cast<float>(nv - 1));
        for (std::size_t ip = 0; ip < np; ++ip) {
            const float p = (np == 1) ? 6.0f : (2.0f + (7.5f * static_cast<float>(ip)) / static_cast<float>(np - 1));
            out.push_back(buildHydraulicFeature(v, p, grindCenter, doseCenter));
        }
    }
}

float FixedBasisVfeNystromGP::rbfKernelF32(const FeatureVec4 &a, const FeatureVec4 &b) const {
    float sq = 0.0f;
    for (std::size_t d = 0; d < HYDRAULIC_FEATURE_DIM; ++d) {
        const float diff = (a[d] - b[d]) / cfg_.lengthscales[d];
        sq += diff * diff;
    }
    return cfg_.signalVariance * std::exp(-0.5f * sq);
}

double FixedBasisVfeNystromGP::rbfKernelF64(const FeatureVec4 &a, const FeatureVec4 &b) const {
    double sq = 0.0;
    for (std::size_t d = 0; d < HYDRAULIC_FEATURE_DIM; ++d) {
        const double diff = (static_cast<double>(a[d]) - static_cast<double>(b[d])) /
                            static_cast<double>(cfg_.lengthscales[d]);
        sq += diff * diff;
    }
    return static_cast<double>(cfg_.signalVariance) * std::exp(-0.5 * sq);
}

void FixedBasisVfeNystromGP::factorAnchorsFloat64() {
    const std::size_t m = anchorCount_;
    lMmInv_.fill(0.0f);
    if (m == 0) {
        return;
    }
    std::vector<double> kmm(m * m, 0.0);
    for (std::size_t i = 0; i < m; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            const double val = rbfKernelF64(anchors_[i], anchors_[j]);
            kmm[i * m + j] = val;
            kmm[j * m + i] = val;
        }
    }

    std::vector<double> lMat(m * m, 0.0);
    double jitter = cfg_.nystromJitter;
    bool factored = false;
    for (int attempt = 0; attempt < 7; ++attempt) {
        std::fill(lMat.begin(), lMat.end(), 0.0);
        bool ok = true;
        for (std::size_t i = 0; i < m && ok; ++i) {
            for (std::size_t j = 0; j <= i; ++j) {
                double sum = kmm[i * m + j] + (i == j ? jitter : 0.0);
                for (std::size_t k = 0; k < j; ++k) {
                    sum -= lMat[i * m + k] * lMat[j * m + k];
                }
                if (i == j) {
                    if (sum <= 1e-14 || !std::isfinite(sum)) {
                        ok = false;
                        break;
                    }
                    lMat[i * m + j] = std::sqrt(sum);
                } else {
                    lMat[i * m + j] = sum / lMat[j * m + j];
                }
            }
        }
        if (ok) {
            if (attempt > 0) {
                ++choleskyFallbacks_;
            }
            factored = true;
            break;
        }
        jitter *= 10.0;
    }

    if (!factored) {
        ++choleskyFallbacks_;
        const float diagInv = 1.0f / std::sqrt(std::max(cfg_.signalVariance, 1e-6f));
        for (std::size_t i = 0; i < m; ++i) {
            lMmInv_[i * MAX_NYSTROM_BASIS + i] = diagInv;
        }
        return;
    }

    // Invert lower-triangular L_MM in float64, then cast to float32.
    std::vector<double> lInv(m * m, 0.0);
    for (std::size_t col = 0; col < m; ++col) {
        lInv[col * m + col] = 1.0 / lMat[col * m + col];
        for (std::size_t row = col + 1; row < m; ++row) {
            double sum = 0.0;
            for (std::size_t k = col; k < row; ++k) {
                sum -= lMat[row * m + k] * lInv[k * m + col];
            }
            lInv[row * m + col] = sum / lMat[row * m + row];
        }
    }
    for (std::size_t row = 0; row < m; ++row) {
        for (std::size_t col = 0; col <= row; ++col) {
            lMmInv_[row * MAX_NYSTROM_BASIS + col] = static_cast<float>(lInv[row * m + col]);
        }
    }
}

void FixedBasisVfeNystromGP::rebuildMeanWeights() {
    alphaW_.fill(0.0f);
    const std::size_t m = anchorCount_;
    if (m == 0) {
        return;
    }
    const float *muW = core_.weights();
    // alphaW = L_MM^{-T} * muW so psi^T muW == kVec^T alphaW exactly.
    for (std::size_t j = 0; j < m; ++j) {
        float sum = 0.0f;
        for (std::size_t i = j; i < m; ++i) {
            sum += lMmInv_[i * MAX_NYSTROM_BASIS + j] * muW[i];
        }
        alphaW_[j] = std::isfinite(sum) ? sum : 0.0f;
    }
}

void FixedBasisVfeNystromGP::computeFeatures(const FeatureVec4 &z, float *outPsi) const {
    std::array<float, MAX_NYSTROM_BASIS> kVec{};
    for (std::size_t j = 0; j < anchorCount_; ++j) {
        kVec[j] = rbfKernelF32(z, anchors_[j]);
    }
    // psi = L_MM^{-1} * kVec (lower-triangular mat-vec)
    for (std::size_t i = 0; i < anchorCount_; ++i) {
        float sum = 0.0f;
        const std::size_t rowOff = i * MAX_NYSTROM_BASIS;
        for (std::size_t j = 0; j <= i; ++j) {
            sum += lMmInv_[rowOff + j] * kVec[j];
        }
        outPsi[i] = sum;
    }
}

float FixedBasisVfeNystromGP::predictMean(const FeatureVec4 &z) const {
    if (anchorCount_ == 0) {
        return 0.0f;
    }
    float mean = 0.0f;
    for (std::size_t j = 0; j < anchorCount_; ++j) {
        mean += alphaW_[j] * rbfKernelF32(z, anchors_[j]);
    }
    return std::isfinite(mean) ? mean : 0.0f;
}

void FixedBasisVfeNystromGP::predict(const FeatureVec4 &z, float &outMean, float &outVar) const {
    if (anchorCount_ == 0) {
        outMean = 0.0f;
        outVar = cfg_.signalVariance;
        return;
    }
    std::array<float, MAX_NYSTROM_BASIS> psi{};
    computeFeatures(z, psi.data());
    float mean = 0.0f;
    float postVar = 0.0f;
    core_.predict(psi.data(), mean, postVar);
    float psiNormSq = 0.0f;
    for (std::size_t i = 0; i < anchorCount_; ++i) {
        psiNormSq += psi[i] * psi[i];
    }
    const float priorResidual = std::max(cfg_.signalVariance - psiNormSq, 0.0f);
    outMean = mean;
    outVar = priorResidual + postVar;
}

float FixedBasisVfeNystromGP::novelty(const FeatureVec4 &z) const {
    if (anchorCount_ == 0) {
        return cfg_.signalVariance;
    }
    std::array<float, MAX_NYSTROM_BASIS> psi{};
    computeFeatures(z, psi.data());
    float psiNormSq = 0.0f;
    for (std::size_t i = 0; i < anchorCount_; ++i) {
        psiNormSq += psi[i] * psi[i];
    }
    return std::max(cfg_.signalVariance - psiNormSq, 0.0f);
}

void FixedBasisVfeNystromGP::updateFromShot(const FeatureVec4 *zRows, const float *yValues, std::size_t count,
                                            float noiseVar) {
    if (count == 0 || zRows == nullptr || yValues == nullptr || anchorCount_ == 0) {
        return;
    }
    core_.applyShotForgetting(cfg_.shotForgettingFactor);
    const float invNoiseStd = 1.0f / std::sqrt(std::max(noiseVar, 1e-6f));
    std::array<float, MAX_NYSTROM_BASIS> psi{};
    for (std::size_t i = 0; i < count; ++i) {
        computeFeatures(zRows[i], psi.data());
        core_.updateRow(psi.data(), yValues[i], invNoiseStd);
    }
    core_.solveWeights();
    rebuildMeanWeights();
}

void FixedBasisVfeNystromGP::refreshFromHistory(const FeatureVec4 *zRows, const float *yValues, std::size_t count,
                                                float noiseVar) {
    if (count == 0 || zRows == nullptr || yValues == nullptr) {
        reset();
        return;
    }
    // Median grind and dose for scaffold centering.
    std::vector<float> grinds(count), doses(count);
    for (std::size_t i = 0; i < count; ++i) {
        grinds[i] = zRows[i][2] * 5.0f;
        doses[i] = zRows[i][3] * 3.0f + 18.0f;
    }
    std::nth_element(grinds.begin(), grinds.begin() + count / 2, grinds.end());
    std::nth_element(doses.begin(), doses.begin() + count / 2, doses.end());

    std::vector<FeatureVec4> pool;
    buildScaffold(grinds[count / 2], doses[count / 2], pool);
    const std::size_t scaffoldCount = pool.size();
    pool.insert(pool.end(), zRows, zRows + count);

    const std::size_t nPool = pool.size();
    const std::size_t cap = std::min(std::min(cfg_.basisSize, MAX_NYSTROM_BASIS), nPool);

    // Greedy Pivoted Cholesky over [scaffold; history] in float32.
    std::vector<float> diag(nPool, cfg_.signalVariance);
    std::vector<float> lCols(nPool * cap, 0.0f);
    std::size_t colIdx = 0;
    anchorCount_ = 0;

    auto addPivot = [&](std::size_t pivot) -> bool {
        const float dVal = diag[pivot];
        if (dVal < cfg_.pivotedCholeskyMinDiag || colIdx >= cap) {
            return false;
        }
        const float invSqrtD = 1.0f / std::sqrt(dVal);
        for (std::size_t i = 0; i < nPool; ++i) {
            float val = rbfKernelF32(pool[i], pool[pivot]);
            for (std::size_t prev = 0; prev < colIdx; ++prev) {
                val -= lCols[i * cap + prev] * lCols[pivot * cap + prev];
            }
            const float lVal = val * invSqrtD;
            lCols[i * cap + colIdx] = lVal;
            diag[i] = std::max(diag[i] - lVal * lVal, 0.0f);
        }
        diag[pivot] = 0.0f;
        anchors_[anchorCount_++] = pool[pivot];
        ++colIdx;
        return true;
    };

    for (std::size_t s = 0; s < scaffoldCount && colIdx < cap; ++s) {
        addPivot(s);
    }
    while (colIdx < cap) {
        std::size_t best = 0;
        float bestDiag = diag[0];
        for (std::size_t i = 1; i < nPool; ++i) {
            if (diag[i] > bestDiag) {
                bestDiag = diag[i];
                best = i;
            }
        }
        if (!addPivot(best)) {
            break;
        }
    }

    factorAnchorsFloat64();
    core_.reset(anchorCount_);
    const float invNoiseStd = 1.0f / std::sqrt(std::max(noiseVar, 1e-6f));
    std::array<float, MAX_NYSTROM_BASIS> psi{};
    for (std::size_t i = 0; i < count; ++i) {
        computeFeatures(zRows[i], psi.data());
        core_.updateRow(psi.data(), yValues[i], invNoiseStd);
    }
    core_.solveWeights();
    rebuildMeanWeights();
}

// ---------------------------------------------------------------------------
// Matern32StateSpaceFilter (2-state exact SDE + Joseph form in float32)
// ---------------------------------------------------------------------------

Matern32StateSpaceFilter::Matern32StateSpaceFilter(const Matern32FilterConfig &cfg) : cfg_(cfg) {
    lam_ = std::sqrt(3.0f) / cfg_.lengthscaleS;
    const float s2 = cfg_.signalStd * cfg_.signalStd;
    pInf_ = {s2, 0.0f, 0.0f, (lam_ * lam_) * s2};
    reset();
}

void Matern32StateSpaceFilter::reset() {
    mean_ = {0.0f, 0.0f};
    cov_ = pInf_;
}

void Matern32StateSpaceFilter::transitionMatrices(float dtS, float aMat[4], float qMat[4]) const {
    const float decay = std::exp(-lam_ * dtS);
    aMat[0] = decay * (1.0f + lam_ * dtS);
    aMat[1] = decay * dtS;
    aMat[2] = decay * (-(lam_ * lam_) * dtS);
    aMat[3] = decay * (1.0f - lam_ * dtS);

    // Q = P_inf - A * P_inf * A^T (exact stationary discrete process noise)
    const float p00 = pInf_[0];
    const float p11 = pInf_[3];
    const float ap00 = aMat[0] * aMat[0] * p00 + aMat[1] * aMat[1] * p11;
    const float ap01 = aMat[0] * aMat[2] * p00 + aMat[1] * aMat[3] * p11;
    const float ap11 = aMat[2] * aMat[2] * p00 + aMat[3] * aMat[3] * p11;
    qMat[0] = std::max(p00 - ap00, 1e-7f);
    qMat[1] = -ap01;
    qMat[2] = -ap01;
    qMat[3] = std::max(p11 - ap11, 1e-7f);
}

void Matern32StateSpaceFilter::stabilizeCov(float cov[4]) {
    const float off = 0.5f * (cov[1] + cov[2]);
    cov[0] = std::max(cov[0], 1e-7f);
    cov[3] = std::max(cov[3], 1e-7f);
    const float maxOff = std::sqrt(cov[0] * cov[3]) * 0.999f;
    const float clamped = std::clamp(off, -maxOff, maxOff);
    cov[1] = clamped;
    cov[2] = clamped;
}

void Matern32StateSpaceFilter::predict(float dtS) {
    float a[4], q[4];
    transitionMatrices(dtS, a, q);
    const float m0 = a[0] * mean_[0] + a[1] * mean_[1];
    const float m1 = a[2] * mean_[0] + a[3] * mean_[1];
    mean_[0] = m0;
    mean_[1] = m1;

    // P_new = A * P * A^T + Q
    const float t00 = a[0] * cov_[0] + a[1] * cov_[2];
    const float t01 = a[0] * cov_[1] + a[1] * cov_[3];
    const float t10 = a[2] * cov_[0] + a[3] * cov_[2];
    const float t11 = a[2] * cov_[1] + a[3] * cov_[3];
    float nextCov[4] = {
        t00 * a[0] + t01 * a[1] + q[0],
        t00 * a[2] + t01 * a[3] + q[1],
        t10 * a[0] + t11 * a[1] + q[2],
        t10 * a[2] + t11 * a[3] + q[3],
    };
    stabilizeCov(nextCov);
    cov_ = {nextCov[0], nextCov[1], nextCov[2], nextCov[3]};
}

void Matern32StateSpaceFilter::update(float observation, float measurementVar) {
    if (!std::isfinite(observation)) {
        return;
    }
    const float rVar =
        (measurementVar > 0.0f) ? std::max(measurementVar, 1e-6f) : (cfg_.measurementStd * cfg_.measurementStd);
    const float sVar = cov_[0] + rVar;
    if (sVar <= 1e-9f || !std::isfinite(sVar)) {
        return;
    }
    const float k0 = cov_[0] / sVar;
    const float k1 = cov_[2] / sVar;
    const float innov = observation - mean_[0];
    mean_[0] += k0 * innov;
    mean_[1] += k1 * innov;

    // Joseph form: (I - K H) P (I - K H)^T + K rVar K^T
    const float m00 = 1.0f - k0;
    const float m10 = -k1;
    const float t00 = m00 * cov_[0];
    const float t01 = m00 * cov_[1];
    const float t10 = m10 * cov_[0] + cov_[2];
    const float t11 = m10 * cov_[1] + cov_[3];
    float nextCov[4] = {
        t00 * m00 + k0 * k0 * rVar,
        t00 * m10 + t01 + k0 * k1 * rVar,
        t10 * m00 + k1 * k0 * rVar,
        t10 * m10 + t11 + k1 * k1 * rVar,
    };
    stabilizeCov(nextCov);
    cov_ = {nextCov[0], nextCov[1], nextCov[2], nextCov[3]};
}

void Matern32StateSpaceFilter::forecast(float initOffset, float initRate, float cov00, float cov01, float cov11,
                                        std::size_t horizonSteps, float dtS, float *outOffsets,
                                        float *outVars) const {
    float a[4], q[4];
    transitionMatrices(dtS, a, q);
    float m0 = initOffset;
    float m1 = initRate;
    float p[4] = {cov00, cov01, cov01, cov11};
    stabilizeCov(p);
    for (std::size_t step = 0; step < horizonSteps; ++step) {
        const float nm0 = a[0] * m0 + a[1] * m1;
        const float nm1 = a[2] * m0 + a[3] * m1;
        m0 = nm0;
        m1 = nm1;
        const float t00 = a[0] * p[0] + a[1] * p[2];
        const float t01 = a[0] * p[1] + a[1] * p[3];
        const float t10 = a[2] * p[0] + a[3] * p[2];
        const float t11 = a[2] * p[1] + a[3] * p[3];
        p[0] = t00 * a[0] + t01 * a[1] + q[0];
        p[1] = t00 * a[2] + t01 * a[3] + q[1];
        p[2] = t10 * a[0] + t11 * a[1] + q[2];
        p[3] = t10 * a[2] + t11 * a[3] + q[3];
        stabilizeCov(p);
        outOffsets[step] = m0;
        outVars[step] = std::max(p[0], 0.0f);
    }
}

// ---------------------------------------------------------------------------
// CombinedHydraulicPredictor
// ---------------------------------------------------------------------------

CombinedHydraulicPredictor::CombinedHydraulicPredictor(const HydraulicPhysicsConfig &physCfg,
                                                       const ResidualGPConfig &gpCfg,
                                                       const Matern32FilterConfig &kfCfg)
    : physics_(physCfg), residualGP_(gpCfg), liveFilter_(kfCfg), bevToPumpRatio_(physCfg.initialBevToPumpRatio) {}

void CombinedHydraulicPredictor::resetShot() { liveFilter_.reset(); }

void CombinedHydraulicPredictor::resetAll() {
    residualGP_.reset();
    liveFilter_.reset();
    bevToPumpRatio_ = physics_.initialBevToPumpRatio;
}

void CombinedHydraulicPredictor::updateBevToPumpRatio(const float *pumpFlow, const float *bevFlow,
                                                      const uint8_t *activeMask, std::size_t count) {
    std::vector<float> ratios;
    ratios.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (activeMask[i] && pumpFlow[i] > 0.35f && bevFlow[i] > 0.15f) {
            const float r = bevFlow[i] / pumpFlow[i];
            if (std::isfinite(r)) {
                ratios.push_back(r);
            }
        }
    }
    if (ratios.size() < 4) {
        return;
    }
    std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
    const float med = std::clamp(ratios[ratios.size() / 2], 0.50f, 1.20f);
    bevToPumpRatio_ = 0.65f * bevToPumpRatio_ + 0.35f * med;
}

void CombinedHydraulicPredictor::observeTick(float pressureBar, float beverageFlowGS, float waterPumpedMl,
                                             float relativeGrindSteps, float doseInG, bool activeForEstimation) {
    liveFilter_.predict(physics_.dtS);
    if (activeForEstimation && std::isfinite(pressureBar) && std::isfinite(beverageFlowGS) && pressureBar >= 1.2f &&
        beverageFlowGS >= 0.15f) {
        const float obsEll = std::log(std::max(beverageFlowGS, 0.05f) / std::sqrt(std::max(pressureBar, 0.1f)));
        const float mPhys = physicsPriorLogConductance(relativeGrindSteps, doseInG, physics_);
        const FeatureVec4 z = buildHydraulicFeature(waterPumpedMl, pressureBar, relativeGrindSteps, doseInG);
        float gMean = 0.0f;
        float gVar = 0.0f;
        residualGP_.predict(z, gMean, gVar);
        const float residual = obsEll - mPhys - gMean;
        const float measVar = liveFilter_.config().measurementStd * liveFilter_.config().measurementStd +
                              0.25f * std::max(gVar, 0.0f);
        liveFilter_.update(residual, measVar);
    }
}

void CombinedHydraulicPredictor::rolloutSinglePath(const HydraulicState &state, const PumpMode *modes,
                                                   const float *pressureTargets, const float *flowTargets,
                                                   std::size_t horizonSteps, float relativeGrindSteps, float doseInG,
                                                   const float *liveOffsets, const float *liveStds, float sigmaShift,
                                                   float *outP, float *outQ, float *outW) const {
    float p = std::max(state.pressureBar, 0.05f);
    float qIn = std::max(state.pumpFlowMlS, 0.0f);
    float vIn = std::max(state.waterPumpedMl, 0.0f);
    float w = std::max(state.weightG, 0.0f);

    const float mPhys = physicsPriorLogConductance(relativeGrindSteps, doseInG, physics_);
    const float dtSub = physics_.dtS / static_cast<float>(physics_.substeps);
    const float eta = bevToPumpRatio_;

    for (std::size_t step = 0; step < horizonSteps; ++step) {
        const PumpMode mode = modes[step];
        const float pTgt = pressureTargets[step];
        const float qTgt = flowTargets[step];

        const FeatureVec4 z = buildHydraulicFeature(vIn, p, relativeGrindSteps, doseInG);
        float gMean = 0.0f;
        float gVar = 0.0f;
        if (sigmaShift == 0.0f) {
            gMean = residualGP_.predictMean(z);
        } else {
            residualGP_.predict(z, gMean, gVar);
        }
        const float liveVar = (liveStds != nullptr && sigmaShift != 0.0f) ? (liveStds[step] * liveStds[step]) : 0.0f;
        const float ellStd = (sigmaShift != 0.0f) ? std::sqrt(std::max(gVar + liveVar, 0.0f)) : 0.0f;
        const float ell = std::clamp(mPhys + gMean + liveOffsets[step] + sigmaShift * ellStd,
                                     physics_.logConductanceMin, physics_.logConductanceMax);
        const float kBev = std::exp(ell);

        float qBevStep = 0.0f;
        for (int sub = 0; sub < physics_.substeps; ++sub) {
            const float cEff = effectiveCompliance(vIn, physics_);
            const float qBev = kBev * std::sqrt(std::max(p, 0.05f));
            const float qPuckEq = qBev / eta;
            const float qMax = std::max(0.5f, physics_.pumpQ0MlS - physics_.pumpSlopeMlSPerBar * p);

            if (mode == PumpMode::FLOW) {
                qIn += (physics_.flowTrackAlpha / static_cast<float>(physics_.substeps)) * (qTgt - qIn);
                // Respect optional secondary pressure cap (pTgt > 0.1 bar; 0 means uncapped).
                if (pTgt > 0.1f && p >= pTgt) {
                    const float qCap = qPuckEq + cEff * (pTgt - p) / physics_.pressureTrackTauS;
                    qIn = std::min(qIn, std::max(qCap, 0.0f));
                }
                qIn = std::clamp(qIn, 0.0f, qMax);
            } else if (mode == PumpMode::PRESSURE) {
                float qReq = qPuckEq + cEff * (pTgt - p) / physics_.pressureTrackTauS;
                // Respect optional secondary flow cap (qTgt > 0.1 ml/s; 0 means uncapped).
                if (qTgt > 0.1f) {
                    qReq = std::min(qReq, qTgt);
                }
                qIn = std::clamp(qReq, 0.0f, qMax);
            } else {
                qIn = 0.0f;
            }

            const float dp = ((qIn - qPuckEq) / cEff) * dtSub;
            p = std::clamp(p + dp, 0.05f, 15.0f);
            vIn = std::max(vIn + qIn * dtSub, 0.0f);
            qBevStep = kBev * std::sqrt(std::max(p, 0.05f));
            w = std::max(w + qBevStep * dtSub, 0.0f);
        }

        outP[step] = p;
        outQ[step] = qBevStep;
        outW[step] = w;
    }
}

void CombinedHydraulicPredictor::rollout(const HydraulicState &state, const PumpMode *modes,
                                         const float *pressureTargets, const float *flowTargets,
                                         std::size_t horizonSteps, float relativeGrindSteps, float doseInG,
                                         bool propagateUncertainty, RolloutOutput &out) const {
    RolloutScratch localScratch;
    rolloutWithScratch(state, modes, pressureTargets, flowTargets, horizonSteps, relativeGrindSteps, doseInG,
                       propagateUncertainty, out, localScratch);
}

void CombinedHydraulicPredictor::rolloutWithScratch(const HydraulicState &state, const PumpMode *modes,
                                                    const float *pressureTargets, const float *flowTargets,
                                                    std::size_t horizonSteps, float relativeGrindSteps, float doseInG,
                                                    bool propagateUncertainty, RolloutOutput &out,
                                                    RolloutScratch &ws) const {
    const std::size_t h = std::min(horizonSteps, MAX_MPC_HORIZON);
    out.steps = h;
    if (h == 0) {
        return;
    }

    liveFilter_.forecast(state.liveOffset, state.liveRate, state.liveCov00, state.liveCov01, state.liveCov11, h,
                         physics_.dtS, ws.liveOffsets.data(), ws.liveStds.data());
    for (std::size_t i = 0; i < h; ++i) {
        ws.liveStds[i] = std::sqrt(std::max(ws.liveStds[i], 0.0f));
    }

    rolloutSinglePath(state, modes, pressureTargets, flowTargets, h, relativeGrindSteps, doseInG,
                      ws.liveOffsets.data(), ws.liveStds.data(), 0.0f, out.pressureBar.data(),
                      out.beverageFlowGS.data(), out.weightG.data());

    if (propagateUncertainty) {
        rolloutSinglePath(state, modes, pressureTargets, flowTargets, h, relativeGrindSteps, doseInG,
                          ws.liveOffsets.data(), ws.liveStds.data(), 1.0f, ws.pHi.data(), ws.qHi.data(),
                          ws.wHi.data());
        rolloutSinglePath(state, modes, pressureTargets, flowTargets, h, relativeGrindSteps, doseInG,
                          ws.liveOffsets.data(), ws.liveStds.data(), -1.0f, ws.pLo.data(), ws.qLo.data(),
                          ws.wLo.data());
        const float pObsVar = physics_.obsPressureNoiseStdBar * physics_.obsPressureNoiseStdBar;
        const float qObsVar = physics_.obsFlowNoiseStdGS * physics_.obsFlowNoiseStdGS;
        const float wStepVar = (physics_.obsFlowNoiseStdGS * physics_.dtS) * (physics_.obsFlowNoiseStdGS * physics_.dtS);
        for (std::size_t i = 0; i < h; ++i) {
            const float pe = 0.5f * std::fabs(ws.pHi[i] - ws.pLo[i]);
            const float qe = 0.5f * std::fabs(ws.qHi[i] - ws.qLo[i]);
            const float we = 0.5f * std::fabs(ws.wHi[i] - ws.wLo[i]);
            out.pressureStdBar[i] = std::sqrt(pe * pe + pObsVar);
            out.beverageFlowStdGS[i] = std::sqrt(qe * qe + qObsVar);
            out.weightStdG[i] = std::sqrt(we * we + static_cast<float>(i + 1) * wStepVar);
        }
    } else {
        for (std::size_t i = 0; i < h; ++i) {
            out.pressureStdBar[i] = physics_.obsPressureNoiseStdBar;
            out.beverageFlowStdGS[i] = physics_.obsFlowNoiseStdGS;
            out.weightStdG[i] = 0.0f;
        }
    }
}

void CombinedHydraulicPredictor::predictNominalReferenceFlow(const HydraulicState &state, const PumpMode *modes,
                                                             const float *basePressureTargets,
                                                             const float *baseFlowTargets, std::size_t horizonSteps,
                                                             float relativeGrindSteps, float doseInG,
                                                             float baselineLogConductanceOffset,
                                                             float *outRefFlowGS) const {
    if (outRefFlowGS == nullptr || modes == nullptr || basePressureTargets == nullptr || baseFlowTargets == nullptr) {
        return;
    }
    const std::size_t h = std::min(horizonSteps, MAX_MPC_HORIZON);
    if (h == 0) {
        return;
    }
    const float baseOff =
        std::clamp(std::isfinite(baselineLogConductanceOffset) ? baselineLogConductanceOffset : 0.0f, -1.2f, 1.2f);
    std::array<float, MAX_MPC_HORIZON> nomOffsets{};
    std::array<float, MAX_MPC_HORIZON> nomVars{};
    // Forecast baseline offset through the exact same Matérn-3/2 decay law as rollout() so that
    // a healthy shot (liveOffset == baseOff, liveRate == 0) matches its nominal reference to machine precision.
    liveFilter_.forecast(baseOff, 0.0f, 0.0f, 0.0f, 0.0f, h, physics_.dtS, nomOffsets.data(), nomVars.data());

    HydraulicState nomState = state;
    nomState.liveOffset = baseOff;
    nomState.liveRate = 0.0f;
    nomState.liveCov00 = 0.0f;
    nomState.liveCov01 = 0.0f;
    nomState.liveCov11 = 0.0f;

    std::array<float, MAX_MPC_HORIZON> tmpP{}, tmpQ{}, tmpW{};
    rolloutSinglePath(nomState, modes, basePressureTargets, baseFlowTargets, h, relativeGrindSteps, doseInG,
                      nomOffsets.data(), nullptr, 0.0f, tmpP.data(), tmpQ.data(), tmpW.data());
    for (std::size_t i = 0; i < h; ++i) {
        if (modes[i] == PumpMode::SIMPLE) {
            outRefFlowGS[i] = 0.0f;
        } else {
            outRefFlowGS[i] = std::clamp(std::isfinite(tmpQ[i]) ? tmpQ[i] : 0.0f, 0.20f, 5.5f);
        }
    }
}

} // namespace EspressoRL
