#include "EmbeddedCpbo.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace EspressoRL {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSqrt2 = 1.41421356237309504880;
constexpr double kInvSqrt2 = 0.70710678118654752440;
constexpr double kSqrt3 = 1.73205080756887729353;
constexpr double kSqrt5 = 2.23606797749978969641;
constexpr double kInvSqrt2Pi = 0.39894228040143267794;
constexpr double kLogSqrt2Pi = 0.91893853320467274178;

// 20-point Gauss-Hermite nodes and weights normalized by 1/sqrt(pi) (sum == 1.0).
constexpr std::array<double, 20> kGhNodes = {
    -5.38748089001123276e+00, -4.60368244955074424e+00, -3.94476404011562520e+00, -3.34785456738321630e+00,
    -2.78880605842813045e+00, -2.25497400208927568e+00, -1.73853771211658614e+00, -1.23407621539532308e+00,
    -7.37473728545394391e-01, -2.45340708300901239e-01, 2.45340708300901239e-01,  7.37473728545394391e-01,
    1.23407621539532308e+00,  1.73853771211658614e+00,  2.25497400208927568e+00,  2.78880605842813045e+00,
    3.34785456738321630e+00,  3.94476404011562520e+00,  4.60368244955074424e+00,  5.38748089001123276e+00,
};

constexpr std::array<double, 20> kGhWeights = {
    1.25780067243792340e-13, 2.48206236231517553e-10, 6.12749025998292797e-08, 4.40212109023085101e-06,
    1.28826279961929280e-04, 1.83010313108049002e-03, 1.39978374471010220e-02, 6.15063720639768968e-02,
    1.61739333983999978e-01, 2.60793063449554885e-01, 2.60793063449554885e-01, 1.61739333983999978e-01,
    6.15063720639768968e-02, 1.39978374471010220e-02, 1.83010313108049002e-03, 1.28826279961929280e-04,
    4.40212109023085101e-06, 6.12749025998292797e-08, 2.48206236231517553e-10, 1.25780067243792340e-13,
};

// 24-point and 20-point Gauss-Legendre nodes/weights on [-1, 1].
constexpr std::array<double, 24> kGl24Nodes = {
    -9.95187219997021311e-01, -9.74728555971309474e-01, -9.38274552002732798e-01, -8.86415527004401071e-01,
    -8.20001985973902947e-01, -7.40124191578554358e-01, -6.48093651936975546e-01, -5.45421471388839563e-01,
    -4.33793507626045127e-01, -3.15042679696163397e-01, -1.91118867473616311e-01, -6.40568928626056300e-02,
    6.40568928626056300e-02,  1.91118867473616311e-01,  3.15042679696163397e-01,  4.33793507626045127e-01,
    5.45421471388839563e-01,  6.48093651936975546e-01,  7.40124191578554358e-01,  8.20001985973902947e-01,
    8.86415527004401071e-01,  9.38274552002732798e-01,  9.74728555971309474e-01,  9.95187219997021311e-01,
};

constexpr std::array<double, 24> kGl24Weights = {
    1.23412297999886903e-02, 2.85313886289335593e-02, 4.42774388174194122e-02, 5.92985849154363601e-02,
    7.33464814110801611e-02, 8.61901615319532050e-02, 9.76186521041139260e-02, 1.07444270115965565e-01,
    1.15505668053725516e-01, 1.21670472927803294e-01, 1.25837456346828247e-01, 1.27938195346752021e-01,
    1.27938195346752021e-01, 1.25837456346828247e-01, 1.21670472927803294e-01, 1.15505668053725516e-01,
    1.07444270115965565e-01, 9.76186521041139260e-02, 8.61901615319532050e-02, 7.33464814110801611e-02,
    5.92985849154363601e-02, 4.42774388174194122e-02, 2.85313886289335593e-02, 1.23412297999886903e-02,
};

constexpr std::array<double, 20> kGl20Nodes = {
    -9.93128599185094996e-01, -9.63971927277913809e-01, -9.12234428251325946e-01, -8.39116971822218782e-01,
    -7.46331906460150796e-01, -6.36053680726515025e-01, -5.10867001950827126e-01, -3.73706088715419549e-01,
    -2.27785851141645068e-01, -7.65265211334973383e-02, 7.65265211334973383e-02,  2.27785851141645068e-01,
    3.73706088715419549e-01,  5.10867001950827126e-01,  6.36053680726515025e-01,  7.46331906460150796e-01,
    8.39116971822218782e-01,  9.12234428251325946e-01,  9.63971927277913809e-01,  9.93128599185094996e-01,
};

constexpr std::array<double, 20> kGl20Weights = {
    1.76140071391508932e-02, 4.06014298003864460e-02, 6.26720483341087903e-02, 8.32767415767047131e-02,
    1.01930119817240705e-01, 1.18194531961518606e-01, 1.31688638449176887e-01, 1.42096109318382402e-01,
    1.49172986472604241e-01, 1.52753387130726281e-01, 1.52753387130726281e-01, 1.49172986472604241e-01,
    1.42096109318382402e-01, 1.31688638449176887e-01, 1.18194531961518606e-01, 1.01930119817240705e-01,
    8.32767415767047131e-02, 6.26720483341087903e-02, 4.06014298003864460e-02, 1.76140071391508932e-02,
};

/// Deterministic SplitMix64 PRNG (portable across x86/ARM/Xtensa).
class SplitMix64 {
  public:
    explicit SplitMix64(std::uint64_t seed)
        : state_(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}

    std::uint64_t nextU64() {
        state_ += 0x9e3779b97f4a7c15ULL;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    double uniform01() {
        // Strictly in (0, 1) with 53 bits of significand precision.
        return (static_cast<double>(nextU64() >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }

    double standardNormal() {
        if (hasSpare_) {
            hasSpare_ = false;
            return spare_;
        }
        const double u1 = std::max(uniform01(), 1e-16);
        const double u2 = uniform01();
        const double r = std::sqrt(-2.0 * std::log(u1));
        const double theta = 2.0 * kPi * u2;
        spare_ = r * std::sin(theta);
        hasSpare_ = true;
        return r * std::cos(theta);
    }

  private:
    std::uint64_t state_ = 0;
    bool hasSpare_ = false;
    double spare_ = 0.0;
};

double laplaceMillsRatioNegTail(double negX) {
    // negX = -x > 0. Exact Laplace continued fraction for phi(x)/Phi(x) = negX * cf.
    const double invZ2 = 1.0 / (negX * negX);
    double cf = 1.0;
    for (int k = 16; k >= 1; --k) {
        cf = 1.0 + (static_cast<double>(k) * invZ2) / cf;
    }
    return negX * cf;
}

double millsRatio(double x) {
    if (x < -10.0) {
        return laplaceMillsRatioNegTail(-x);
    }
    const double lp = logNdtr(x);
    const double expo = -0.5 * x * x - kLogSqrt2Pi - lp;
    return std::exp(expo);
}

double linearQuantileSorted(const std::vector<double> &sorted, double q) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted[0];
    }
    const double pos = std::clamp(q, 0.0, 1.0) * static_cast<double>(sorted.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(pos));
    const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

bool stableCholesky(const double *mat, std::size_t n, double jitter, std::vector<double> &chol, double &jitterUsed) {
    chol.assign(n * n, 0.0);
    double current = std::max(jitter, 1e-12);
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::fill(chol.begin(), chol.end(), 0.0);
        bool ok = true;
        for (std::size_t i = 0; i < n && ok; ++i) {
            for (std::size_t j = 0; j <= i; ++j) {
                double sum = 0.5 * (mat[i * n + j] + mat[j * n + i]) + (i == j ? current : 0.0);
                for (std::size_t k = 0; k < j; ++k) {
                    sum -= chol[i * n + k] * chol[j * n + k];
                }
                if (i == j) {
                    if (sum <= 0.0 || !std::isfinite(sum)) {
                        ok = false;
                        break;
                    }
                    chol[i * n + j] = std::sqrt(sum);
                } else {
                    chol[i * n + j] = sum / chol[j * n + j];
                }
            }
        }
        if (ok) {
            jitterUsed = current;
            return true;
        }
        current *= 10.0;
    }
    return false;
}

// Solve L * x = b where L is [n x n] lower triangular.
void solveLowerVec(const double *lMat, std::size_t n, const double *b, double *x) {
    for (std::size_t i = 0; i < n; ++i) {
        double sum = b[i];
        const double *row = lMat + i * n;
        for (std::size_t k = 0; k < i; ++k) {
            sum -= row[k] * x[k];
        }
        x[i] = sum / row[i];
    }
}

// Solve L^T * x = b where L is [n x n] lower triangular.
void solveUpperTransVec(const double *lMat, std::size_t n, const double *b, double *x) {
    for (std::size_t rev = 0; rev < n; ++rev) {
        const std::size_t i = n - 1 - rev;
        double sum = b[i];
        for (std::size_t k = i + 1; k < n; ++k) {
            sum -= lMat[k * n + i] * x[k];
        }
        x[i] = sum / lMat[i * n + i];
    }
}

// Solve L * X = B where L is [n x n] lower triangular, B and X are row-major [n x m].
void solveLowerMat(const double *lMat, std::size_t n, const double *bMat, std::size_t m, double *xMat) {
    for (std::size_t i = 0; i < n; ++i) {
        const double *lRow = lMat + i * n;
        const double invDiag = 1.0 / lRow[i];
        double *xRow = xMat + i * m;
        const double *bRow = bMat + i * m;
        for (std::size_t j = 0; j < m; ++j) {
            xRow[j] = bRow[j];
        }
        for (std::size_t k = 0; k < i; ++k) {
            const double lik = lRow[k];
            const double *xPrev = xMat + k * m;
            for (std::size_t j = 0; j < m; ++j) {
                xRow[j] -= lik * xPrev[j];
            }
        }
        for (std::size_t j = 0; j < m; ++j) {
            xRow[j] *= invDiag;
        }
    }
}

// Compute L^{-1} where L is [n x n] lower triangular (result stored row-major [n x n]).
void invertLowerTriangular(const double *lMat, std::size_t n, double *invL) {
    std::fill(invL, invL + n * n, 0.0);
    for (std::size_t col = 0; col < n; ++col) {
        invL[col * n + col] = 1.0 / lMat[col * n + col];
        for (std::size_t row = col + 1; row < n; ++row) {
            double sum = 0.0;
            const double *lRow = lMat + row * n;
            for (std::size_t k = col; k < row; ++k) {
                sum -= lRow[k] * invL[k * n + col];
            }
            invL[row * n + col] = sum / lRow[row];
        }
    }
}

std::vector<double> softmaxVec(const std::vector<double> &logits) {
    if (logits.empty()) {
        return {};
    }
    const double maxL = *std::max_element(logits.begin(), logits.end());
    std::vector<double> out(logits.size(), 0.0);
    double sum = 0.0;
    for (std::size_t i = 0; i < logits.size(); ++i) {
        out[i] = std::exp(logits[i] - maxL);
        sum += out[i];
    }
    const double invSum = 1.0 / std::max(sum, 1e-300);
    for (double &v : out) {
        v *= invSum;
    }
    return out;
}

std::size_t computeFeatureStride(std::size_t rawDim, std::size_t physicsDim, std::size_t traceDim) {
    return rawDim + physicsDim + (traceDim > 0 ? 2 * traceDim : 0);
}

// Evaluate additive ARD kernel K(X1, X2) of shape [n1 x n2] (row-major).
void evaluateAdditiveKernelMatrix(const double *x1, std::size_t n1, const double *x2, std::size_t n2,
                                  const EmbeddedKernelHyperparams &hp, std::size_t rawDim, std::size_t physicsDim,
                                  std::size_t traceDim, std::vector<double> &outK) {
    const std::size_t stride = computeFeatureStride(rawDim, physicsDim, traceDim);
    const auto weights = hp.mixtureWeights();
    const double wRaw = weights.size() > 0 ? weights[0] : 1.0;
    const double wPhys = weights.size() > 1 ? weights[1] : 0.0;
    const double wTrace = (traceDim > 0 && weights.size() > 2) ? weights[2] : 0.0;

    std::vector<double> invRawLs(rawDim, 1.0);
    for (std::size_t d = 0; d < rawDim; ++d) {
        invRawLs[d] = std::exp(-hp.rawLogLengthscales[d]);
    }
    std::vector<double> invPhysLs(physicsDim, 1.0);
    for (std::size_t d = 0; d < physicsDim; ++d) {
        invPhysLs[d] = std::exp(-hp.physicsLogLengthscales[d]);
    }
    std::vector<double> traceLs2(traceDim, 1.0);
    std::vector<double> self1, self2;
    if (traceDim > 0) {
        for (std::size_t d = 0; d < traceDim; ++d) {
            traceLs2[d] = std::exp(2.0 * hp.traceLogLengthscales[d]);
        }
        self1.assign(n1, 1.0);
        self2.assign(n2, 1.0);
        const std::size_t tvOff = rawDim + physicsDim + traceDim;
        for (std::size_t i = 0; i < n1; ++i) {
            const double *v = x1 + i * stride + tvOff;
            double logS = 0.0;
            for (std::size_t d = 0; d < traceDim; ++d) {
                const double vs = 2.0 * std::max(v[d], 0.0);
                logS += std::log1p(vs / traceLs2[d]);
            }
            self1[i] = std::exp(-0.5 * logS);
        }
        for (std::size_t j = 0; j < n2; ++j) {
            const double *v = x2 + j * stride + tvOff;
            double logS = 0.0;
            for (std::size_t d = 0; d < traceDim; ++d) {
                const double vs = 2.0 * std::max(v[d], 0.0);
                logS += std::log1p(vs / traceLs2[d]);
            }
            self2[j] = std::exp(-0.5 * logS);
        }
    }

    outK.assign(n1 * n2, 0.0);
    const std::size_t tmOff = rawDim + physicsDim;
    const std::size_t tvOff = rawDim + physicsDim + traceDim;

    for (std::size_t i = 0; i < n1; ++i) {
        const double *row1 = x1 + i * stride;
        for (std::size_t j = 0; j < n2; ++j) {
            const double *row2 = x2 + j * stride;

            // Matérn-5/2 on raw
            double r2Raw = 0.0;
            for (std::size_t d = 0; d < rawDim; ++d) {
                const double sd = (row1[d] - row2[d]) * invRawLs[d];
                r2Raw += sd * sd;
            }
            const double uRaw = kSqrt5 * std::sqrt(std::max(r2Raw, 0.0));
            const double kRaw = (1.0 + uRaw + (uRaw * uRaw) / 3.0) * std::exp(-uRaw);

            // Matérn-3/2 on physics
            double r2Phys = 0.0;
            for (std::size_t d = 0; d < physicsDim; ++d) {
                const double sd = (row1[rawDim + d] - row2[rawDim + d]) * invPhysLs[d];
                r2Phys += sd * sd;
            }
            const double uPhys = kSqrt3 * std::sqrt(std::max(r2Phys, 0.0));
            const double kPhys = (1.0 + uPhys) * std::exp(-uPhys);

            double kTot = wRaw * kRaw + wPhys * kPhys;
            if (traceDim > 0) {
                double logDet = 0.0;
                double quad = 0.0;
                for (std::size_t d = 0; d < traceDim; ++d) {
                    const double vSum = std::max(row1[tvOff + d] + row2[tvOff + d], 0.0);
                    const double denom = traceLs2[d] + vSum;
                    const double diff = row1[tmOff + d] - row2[tmOff + d];
                    logDet += -0.5 * std::log1p(vSum / traceLs2[d]);
                    quad += -0.5 * (diff * diff) / denom;
                }
                const double kRawTr = std::exp(logDet + quad);
                const double normDenom = std::max(std::sqrt(self1[i] * self2[j]), 1e-12);
                const double kTr = kRawTr / normDenom;
                kTot += wTrace * kTr;
            }
            outK[i * n2 + j] = kTot;
        }
    }
}

// Contract G_K [n x n] with analytic kernel derivatives in O(1) extra memory.
void accumulateKernelHyperparamGrads(const double *x, std::size_t n, const EmbeddedKernelHyperparams &hp,
                                     std::size_t rawDim, std::size_t physicsDim, std::size_t traceDim,
                                     const double *gK, double *gradRaw, double *gradPhys, double *gradTrace,
                                     double *gradLogits) {
    const std::size_t stride = computeFeatureStride(rawDim, physicsDim, traceDim);
    const auto weights = hp.mixtureWeights();
    const double wRaw = weights.size() > 0 ? weights[0] : 1.0;
    const double wPhys = weights.size() > 1 ? weights[1] : 0.0;
    const double wTrace = (traceDim > 0 && weights.size() > 2) ? weights[2] : 0.0;

    std::fill(gradRaw, gradRaw + rawDim, 0.0);
    std::fill(gradPhys, gradPhys + physicsDim, 0.0);
    if (traceDim > 0) {
        std::fill(gradTrace, gradTrace + traceDim, 0.0);
    }
    const std::size_t nWeights = traceDim > 0 ? 3 : 2;
    std::fill(gradLogits, gradLogits + nWeights, 0.0);

    std::vector<double> invRawLs(rawDim, 1.0);
    for (std::size_t d = 0; d < rawDim; ++d) {
        invRawLs[d] = std::exp(-hp.rawLogLengthscales[d]);
    }
    std::vector<double> invPhysLs(physicsDim, 1.0);
    for (std::size_t d = 0; d < physicsDim; ++d) {
        invPhysLs[d] = std::exp(-hp.physicsLogLengthscales[d]);
    }

    std::vector<double> traceLs2(traceDim, 1.0);
    std::vector<double> selfVal;
    std::vector<double> dlogSelf;
    const std::size_t tmOff = rawDim + physicsDim;
    const std::size_t tvOff = rawDim + physicsDim + traceDim;
    if (traceDim > 0) {
        for (std::size_t d = 0; d < traceDim; ++d) {
            traceLs2[d] = std::exp(2.0 * hp.traceLogLengthscales[d]);
        }
        selfVal.assign(n, 1.0);
        dlogSelf.assign(n * traceDim, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            const double *v = x + i * stride + tvOff;
            double logS = 0.0;
            for (std::size_t d = 0; d < traceDim; ++d) {
                const double vs = 2.0 * std::max(v[d], 0.0);
                logS += std::log1p(vs / traceLs2[d]);
                dlogSelf[i * traceDim + d] = vs / (traceLs2[d] + vs);
            }
            selfVal[i] = std::exp(-0.5 * logS);
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        const double *row1 = x + i * stride;
        for (std::size_t j = 0; j < n; ++j) {
            const double gij = gK[i * n + j];
            const double *row2 = x + j * stride;

            // Raw Matérn-5/2
            double r2Raw = 0.0;
            for (std::size_t d = 0; d < rawDim; ++d) {
                const double sd = (row1[d] - row2[d]) * invRawLs[d];
                r2Raw += sd * sd;
            }
            const double uRaw = kSqrt5 * std::sqrt(std::max(r2Raw, 0.0));
            const double expURaw = std::exp(-uRaw);
            const double kRaw = (1.0 + uRaw + (uRaw * uRaw) / 3.0) * expURaw;
            const double coeffRaw = gij * wRaw * (5.0 / 3.0) * (1.0 + uRaw) * expURaw;
            for (std::size_t d = 0; d < rawDim; ++d) {
                const double sd = (row1[d] - row2[d]) * invRawLs[d];
                gradRaw[d] += coeffRaw * (sd * sd);
            }

            // Physics Matérn-3/2
            double r2Phys = 0.0;
            for (std::size_t d = 0; d < physicsDim; ++d) {
                const double sd = (row1[rawDim + d] - row2[rawDim + d]) * invPhysLs[d];
                r2Phys += sd * sd;
            }
            const double uPhys = kSqrt3 * std::sqrt(std::max(r2Phys, 0.0));
            const double expUPhys = std::exp(-uPhys);
            const double kPhys = (1.0 + uPhys) * expUPhys;
            const double coeffPhys = gij * wPhys * 3.0 * expUPhys;
            for (std::size_t d = 0; d < physicsDim; ++d) {
                const double sd = (row1[rawDim + d] - row2[rawDim + d]) * invPhysLs[d];
                gradPhys[d] += coeffPhys * (sd * sd);
            }

            double kTr = 0.0;
            if (traceDim > 0) {
                double logDet = 0.0;
                double quad = 0.0;
                for (std::size_t d = 0; d < traceDim; ++d) {
                    const double vSum = std::max(row1[tvOff + d] + row2[tvOff + d], 0.0);
                    const double denom = traceLs2[d] + vSum;
                    const double diff = row1[tmOff + d] - row2[tmOff + d];
                    logDet += -0.5 * std::log1p(vSum / traceLs2[d]);
                    quad += -0.5 * (diff * diff) / denom;
                }
                const double kRawTr = std::exp(logDet + quad);
                const double normDenom = std::max(std::sqrt(selfVal[i] * selfVal[j]), 1e-12);
                kTr = kRawTr / normDenom;
                const double coeffTr = gij * wTrace * kTr;
                for (std::size_t d = 0; d < traceDim; ++d) {
                    const double vSum = std::max(row1[tvOff + d] + row2[tvOff + d], 0.0);
                    const double denom = traceLs2[d] + vSum;
                    const double diff = row1[tmOff + d] - row2[tmOff + d];
                    const double dlogIj = (vSum / denom) + (traceLs2[d] * diff * diff) / (denom * denom);
                    const double dlogNorm =
                        dlogIj - 0.5 * dlogSelf[i * traceDim + d] - 0.5 * dlogSelf[j * traceDim + d];
                    gradTrace[d] += coeffTr * dlogNorm;
                }
            }

            const double kTot = wRaw * kRaw + wPhys * kPhys + (traceDim > 0 ? wTrace * kTr : 0.0);
            gradLogits[0] += gij * wRaw * (kRaw - kTot);
            gradLogits[1] += gij * wPhys * (kPhys - kTot);
            if (traceDim > 0) {
                gradLogits[2] += gij * wTrace * (kTr - kTot);
            }
        }
    }
}

void gaussHermiteJndElboTerms(const std::vector<double> &muD, const std::vector<double> &varD,
                              const ComparisonPair *comparisons, std::size_t compCount, double gamma, double sigmaPref,
                              double epsilon, double &outEll, std::vector<double> &outGMu, std::vector<double> &outGVar,
                              double &outGGamma) {
    outEll = 0.0;
    outGMu.assign(compCount, 0.0);
    outGVar.assign(compCount, 0.0);
    outGGamma = 0.0;

    const double invS = 1.0 / (kSqrt2 * sigmaPref);
    for (std::size_t c = 0; c < compCount; ++c) {
        const double safeVar = std::max(varD[c], 1e-12);
        const double sqrt2v = std::sqrt(2.0 * safeVar);
        const PreferenceLabel lbl = comparisons[c].label;
        double sumLogP = 0.0;
        double sumDD = 0.0;
        double sumDDx = 0.0;
        double sumDG = 0.0;

        for (std::size_t q = 0; q < kGhNodes.size(); ++q) {
            const double xq = kGhNodes[q];
            const double wq = kGhWeights[q];
            const double d = muD[c] + sqrt2v * xq;

            double lp = 0.0;
            double ddd = 0.0;
            double ddg = 0.0;
            if (lbl == PreferenceLabel::NEW_BETTER) {
                const double z = (d - gamma) * invS;
                lp = logNdtr(z);
                const double m = millsRatio(z);
                ddd = m * invS;
                ddg = -m * invS;
            } else if (lbl == PreferenceLabel::ANCHOR_BETTER) {
                const double z = (-d - gamma) * invS;
                lp = logNdtr(z);
                const double m = millsRatio(z);
                ddd = -m * invS;
                ddg = -m * invS;
            } else {
                const double zHi = (gamma - d) * invS;
                const double zLo = (-gamma - d) * invS;
                const double pRaw = (zLo > 0.0) ? (ndtr(-zLo) - ndtr(-zHi)) : (ndtr(zHi) - ndtr(zLo));
                const bool clamped = pRaw < epsilon;
                const double pSafe = std::max(pRaw, epsilon);
                lp = std::log(pSafe);
                if (!clamped) {
                    const double phiHi = kInvSqrt2Pi * std::exp(-0.5 * zHi * zHi);
                    const double phiLo = kInvSqrt2Pi * std::exp(-0.5 * zLo * zLo);
                    ddd = (-phiHi + phiLo) * invS / pSafe;
                    ddg = (phiHi + phiLo) * invS / pSafe;
                }
            }
            sumLogP += wq * lp;
            sumDD += wq * ddd;
            sumDDx += wq * ddd * xq;
            sumDG += wq * ddg;
        }
        outEll += sumLogP;
        outGMu[c] = sumDD;
        outGVar[c] = sumDDx / sqrt2v;
        outGGamma += sumDG;
    }
}

struct InnerViResult {
    bool ok = false;
    bool cancelled = false;
    std::vector<double> mBar;
    std::vector<double> lam;
    std::vector<double> cholB;
    double elbo = 0.0;
    std::vector<double> gMu;
    double gGamma = 0.0;
};

InnerViResult solveInnerOpperArchambeau(const std::vector<double> &cholK, std::size_t n,
                                        const ComparisonPair *comparisons, std::size_t compCount, double gamma,
                                        double sigmaPref, double epsilon, const std::vector<double> *initMBar,
                                        const std::vector<double> *initLam, int maxIters, double tol,
                                        YieldCallback onYield, void *yieldCtx,
                                        const double *trainPriorMean = nullptr) {
    InnerViResult res;
    if (compCount == 0) {
        res.ok = true;
        res.mBar.assign(n, 0.0);
        res.cholB.assign(n * n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            res.cholB[i * n + i] = 1.0;
        }
        res.elbo = 0.0;
        return res;
    }

    // Build W = E * L_K [C x N]: row c is cholK[newIdx, :] - cholK[anchorIdx, :].
    std::vector<double> wMat(compCount * n, 0.0);
    for (std::size_t c = 0; c < compCount; ++c) {
        const std::size_t iNew = comparisons[c].newIdx;
        const std::size_t iAnc = comparisons[c].anchorIdx;
        double *wRow = wMat.data() + c * n;
        const double *rNew = cholK.data() + iNew * n;
        const double *rAnc = cholK.data() + iAnc * n;
        for (std::size_t j = 0; j < n; ++j) {
            wRow[j] = rNew[j] - rAnc[j];
        }
    }

    // Transpose W^T [N x C] once for fast triangular solves.
    std::vector<double> wT(n * compCount, 0.0);
    for (std::size_t c = 0; c < compCount; ++c) {
        for (std::size_t i = 0; i < n; ++i) {
            wT[i * compCount + c] = wMat[c * n + i];
        }
    }

    std::vector<double> mBar =
        (initMBar != nullptr && initMBar->size() == n) ? *initMBar : std::vector<double>(n, 0.0);
    std::vector<double> lam(compCount, 0.25);
    if (initLam != nullptr && initLam->size() == compCount) {
        for (std::size_t c = 0; c < compCount; ++c) {
            lam[c] = std::max((*initLam)[c], 1e-8);
        }
    }

    std::vector<double> bMat(n * n, 0.0);
    std::vector<double> uMat(n * compCount, 0.0);
    std::vector<double> invCholB(n * n, 0.0);
    std::vector<double> muD(compCount, 0.0);
    std::vector<double> varD(compCount, 0.0);

    auto evalElbo = [&](const std::vector<double> &mVec, const std::vector<double> &lamVec, double &outElbo,
                        std::vector<double> &outCholB, std::vector<double> &outGMu, std::vector<double> &outGVar,
                        double &outGGamma) -> bool {
        // B = I_N + W^T diag(lam) W
        std::fill(bMat.begin(), bMat.end(), 0.0);
        for (std::size_t i = 0; i < n; ++i) {
            bMat[i * n + i] = 1.0;
        }
        for (std::size_t c = 0; c < compCount; ++c) {
            const double lc = lamVec[c];
            const double *wRow = wMat.data() + c * n;
            for (std::size_t i = 0; i < n; ++i) {
                const double lwi = lc * wRow[i];
                for (std::size_t j = 0; j <= i; ++j) {
                    bMat[i * n + j] += lwi * wRow[j];
                }
            }
        }
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                bMat[j * n + i] = bMat[i * n + j];
            }
        }

        double jitterUsed = 0.0;
        if (!stableCholesky(bMat.data(), n, 1e-12, outCholB, jitterUsed)) {
            return false;
        }

        // U = L_B^{-1} W^T [N x C]
        solveLowerMat(outCholB.data(), n, wT.data(), compCount, uMat.data());

        for (std::size_t c = 0; c < compCount; ++c) {
            const double *wRow = wMat.data() + c * n;
            double m = (trainPriorMean != nullptr)
                           ? (trainPriorMean[comparisons[c].newIdx] - trainPriorMean[comparisons[c].anchorIdx])
                           : 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                m += wRow[i] * mVec[i];
            }
            muD[c] = m;
            double v = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const double u = uMat[i * compCount + c];
                v += u * u;
            }
            varD[c] = v;
        }

        double ell = 0.0;
        gaussHermiteJndElboTerms(muD, varD, comparisons, compCount, gamma, sigmaPref, epsilon, ell, outGMu, outGVar,
                                 outGGamma);

        invertLowerTriangular(outCholB.data(), n, invCholB.data());
        double trBInv = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j <= i; ++j) {
                const double v = invCholB[i * n + j];
                trBInv += v * v;
            }
        }
        double logdetB = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            logdetB += 2.0 * std::log(outCholB[i * n + i]);
        }
        double mNorm2 = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            mNorm2 += mVec[i] * mVec[i];
        }
        const double kl = 0.5 * (mNorm2 + trBInv - static_cast<double>(n) + logdetB);
        outElbo = ell - kl;
        return std::isfinite(outElbo);
    };

    double elbo = 0.0;
    std::vector<double> cholB;
    std::vector<double> gMu;
    std::vector<double> gVar;
    double gGamma = 0.0;
    if (!evalElbo(mBar, lam, elbo, cholB, gMu, gVar, gGamma)) {
        return res;
    }

    std::vector<double> targetLam(compCount, 0.0);
    std::vector<double> candLam(compCount, 0.0);
    std::vector<double> gradM(n, 0.0);
    std::vector<double> rhs(n, 0.0);
    std::vector<double> deltaM(n, 0.0);
    std::vector<double> candM(n, 0.0);
    std::vector<double> candCholB, candGMu, candGVar;

    for (int iter = 0; iter < maxIters; ++iter) {
        if (onYield != nullptr && !onYield(yieldCtx)) {
            res.cancelled = true;
            return res;
        }

        double maxLamDiff = 0.0;
        for (std::size_t c = 0; c < compCount; ++c) {
            targetLam[c] = std::clamp(-2.0 * gVar[c], 1e-8, 1e4);
            maxLamDiff = std::max(maxLamDiff, std::fabs(targetLam[c] - lam[c]));
        }

        double maxGradM = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            double s = -mBar[i];
            for (std::size_t c = 0; c < compCount; ++c) {
                s += wMat[c * n + i] * gMu[c];
            }
            gradM[i] = s;
            maxGradM = std::max(maxGradM, std::fabs(s));
        }

        if (maxGradM < tol && maxLamDiff < tol * 10.0) {
            break;
        }

        // Damped fixed-point update on lambda.
        double stepL = 1.0;
        for (int ls = 0; ls < 8; ++ls) {
            for (std::size_t c = 0; c < compCount; ++c) {
                candLam[c] = (1.0 - stepL) * lam[c] + stepL * targetLam[c];
            }
            double candElbo = 0.0;
            double candGG = 0.0;
            if (evalElbo(mBar, candLam, candElbo, candCholB, candGMu, candGVar, candGG) && candElbo >= elbo - 1e-12) {
                lam.swap(candLam);
                elbo = candElbo;
                cholB.swap(candCholB);
                gMu.swap(candGMu);
                gVar.swap(candGVar);
                gGamma = candGG;
                break;
            }
            stepL *= 0.5;
        }

        // Exact Newton step on m_bar using B = L_B L_B^T.
        for (std::size_t i = 0; i < n; ++i) {
            double s = -mBar[i];
            for (std::size_t c = 0; c < compCount; ++c) {
                s += wMat[c * n + i] * gMu[c];
            }
            gradM[i] = s;
        }
        solveLowerVec(cholB.data(), n, gradM.data(), rhs.data());
        solveUpperTransVec(cholB.data(), n, rhs.data(), deltaM.data());

        double stepM = 1.0;
        for (int ls = 0; ls < 10; ++ls) {
            for (std::size_t i = 0; i < n; ++i) {
                candM[i] = mBar[i] + stepM * deltaM[i];
            }
            double candElbo = 0.0;
            double candGG = 0.0;
            if (evalElbo(candM, lam, candElbo, candCholB, candGMu, candGVar, candGG) && candElbo >= elbo - 1e-12) {
                mBar.swap(candM);
                elbo = candElbo;
                cholB.swap(candCholB);
                gMu.swap(candGMu);
                gVar.swap(candGVar);
                gGamma = candGG;
                break;
            }
            stepM *= 0.5;
        }
    }

    res.ok = true;
    res.mBar = std::move(mBar);
    res.lam = std::move(lam);
    res.cholB = std::move(cholB);
    res.elbo = elbo;
    res.gMu = std::move(gMu);
    res.gGamma = gGamma;
    return res;
}

void unpackHyperparams(const std::vector<double> &vec, std::size_t rawDim, std::size_t physicsDim, std::size_t traceDim,
                       bool learnGamma, double defaultRawGamma, EmbeddedKernelHyperparams &outHp, double &outRawGamma) {
    std::size_t pos = 0;
    outHp.rawLogLengthscales.assign(vec.begin() + pos, vec.begin() + pos + rawDim);
    pos += rawDim;
    outHp.physicsLogLengthscales.assign(vec.begin() + pos, vec.begin() + pos + physicsDim);
    pos += physicsDim;
    outHp.traceLogLengthscales.assign(vec.begin() + pos, vec.begin() + pos + traceDim);
    pos += traceDim;
    const std::size_t nWeights = traceDim > 0 ? 3 : 2;
    outHp.weightLogits.assign(vec.begin() + pos, vec.begin() + pos + nWeights);
    pos += nWeights;
    outRawGamma = learnGamma ? vec[pos] : defaultRawGamma;
}

bool evalEnvelopeObjective(const double *trainInputs, std::size_t trainCount, std::size_t rawDim,
                           std::size_t physicsDim, std::size_t traceDim, const ComparisonPair *comparisons,
                           std::size_t comparisonCount, const PreferenceGPConfig &cfg, double initRawGamma,
                           const std::vector<double> &vec, std::vector<double> &cacheMBar, std::vector<double> &cacheLam,
                           int maxInnerIters, YieldCallback onYield, void *yieldCtx, double &outNegElbo,
                           std::vector<double> &outGrad, bool &outCancelled, const double *trainPriorMean = nullptr) {
    outCancelled = false;
    EmbeddedKernelHyperparams hp;
    double rawG = initRawGamma;
    unpackHyperparams(vec, rawDim, physicsDim, traceDim, cfg.learnGamma, initRawGamma, hp, rawG);
    const double gamma = cfg.learnGamma ? softplus(rawG) : cfg.initialGamma;

    std::vector<double> kMat;
    evaluateAdditiveKernelMatrix(trainInputs, trainCount, trainInputs, trainCount, hp, rawDim, physicsDim, traceDim,
                                 kMat);
    std::vector<double> cholK;
    double jitterUsed = 0.0;
    if (!stableCholesky(kMat.data(), trainCount, cfg.covarianceJitter, cholK, jitterUsed)) {
        return false;
    }

    const std::vector<double> *mInit = (cacheMBar.size() == trainCount) ? &cacheMBar : nullptr;
    const std::vector<double> *lInit = (cacheLam.size() == comparisonCount) ? &cacheLam : nullptr;
    InnerViResult inner =
        solveInnerOpperArchambeau(cholK, trainCount, comparisons, comparisonCount, gamma, cfg.sigmaPref,
                                  cfg.probabilityEpsilon, mInit, lInit, maxInnerIters, 1e-9, onYield, yieldCtx,
                                  trainPriorMean);
    if (inner.cancelled) {
        outCancelled = true;
        return false;
    }
    if (!inner.ok) {
        return false;
    }
    cacheMBar = inner.mBar;
    cacheLam = inner.lam;

    // Envelope Theorem exact gradient w.r.t. K:
    //   d(ELBO*)/dK = 0.5 * (alpha alpha^T - L_K^{-T} L_K^{-1} + (L_B^{-1} L_K^{-1})^T (L_B^{-1} L_K^{-1}))
    std::vector<double> alpha(trainCount, 0.0);
    for (std::size_t c = 0; c < comparisonCount; ++c) {
        alpha[comparisons[c].newIdx] += inner.gMu[c];
        alpha[comparisons[c].anchorIdx] -= inner.gMu[c];
    }

    std::vector<double> invLk(trainCount * trainCount, 0.0);
    invertLowerTriangular(cholK.data(), trainCount, invLk.data());

    std::vector<double> invLbInvLk(trainCount * trainCount, 0.0);
    solveLowerMat(inner.cholB.data(), trainCount, invLk.data(), trainCount, invLbInvLk.data());

    std::vector<double> gK(trainCount * trainCount, 0.0);
    for (std::size_t i = 0; i < trainCount; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            double kInvIJ = 0.0;
            double bInvIJ = 0.0;
            const std::size_t startRow = std::max(i, j);
            for (std::size_t r = startRow; r < trainCount; ++r) {
                kInvIJ += invLk[r * trainCount + i] * invLk[r * trainCount + j];
            }
            for (std::size_t r = 0; r < trainCount; ++r) {
                bInvIJ += invLbInvLk[r * trainCount + i] * invLbInvLk[r * trainCount + j];
            }
            const double val = 0.5 * (alpha[i] * alpha[j] - kInvIJ + bInvIJ);
            gK[i * trainCount + j] = val;
            gK[j * trainCount + i] = val;
        }
    }

    const std::size_t nWeights = traceDim > 0 ? 3 : 2;
    const std::size_t totalDim = rawDim + physicsDim + traceDim + nWeights + (cfg.learnGamma ? 1 : 0);
    outGrad.assign(totalDim, 0.0);

    double *gRaw = outGrad.data();
    double *gPhys = gRaw + rawDim;
    double *gTrace = gPhys + physicsDim;
    double *gLogits = gTrace + traceDim;

    accumulateKernelHyperparamGrads(trainInputs, trainCount, hp, rawDim, physicsDim, traceDim, gK.data(), gRaw, gPhys,
                                    gTrace, gLogits);

    if (cfg.learnGamma) {
        outGrad[totalDim - 1] = inner.gGamma * sigmoid(rawG);
    }

    // Negate for minimization of -ELBO.
    for (double &v : outGrad) {
        v = -v;
    }
    outNegElbo = -inner.elbo;
    return std::isfinite(outNegElbo);
}

double categoricalEntropy(const std::array<double, 3> &probs, double eps) {
    double h = 0.0;
    for (double p : probs) {
        h -= p * std::log(std::max(p, eps));
    }
    return h;
}

double sampleUpperTruncatedNormal(double mean, double stdDev, double upper, SplitMix64 &rng) {
    if (!std::isfinite(upper) && upper > 0.0) {
        return mean + stdDev * rng.standardNormal();
    }
    const double alpha = (upper - mean) / stdDev;
    if (alpha < -5.0) {
        // Robert (1995) exponential rejection sampler for extreme lower tail.
        const double lower = -alpha;
        const double rate = 0.5 * (lower + std::sqrt(lower * lower + 4.0));
        for (int attempt = 0; attempt < 10000; ++attempt) {
            const double u1 = std::max(rng.uniform01(), 1e-15);
            const double proposal = lower - std::log(u1) / rate;
            const double u2 = std::max(rng.uniform01(), 1e-15);
            const double diff = proposal - rate;
            if (std::log(u2) <= -0.5 * diff * diff) {
                return mean - stdDev * proposal;
            }
        }
        return upper;
    }
    const double pHi = std::max(ndtr(alpha), 1e-15);
    const double rank = std::clamp(rng.uniform01() * pHi, 1e-15, 1.0 - 1e-15);
    return std::min(mean + stdDev * ndtri(rank), upper);
}

std::array<double, 3> conditionalProbsGibbsFallback(double meanX, double meanA, double covXX, double covXA,
                                                    double covAA, double upperX, double upperA, double gamma,
                                                    double sigmaPref, std::size_t sampleCount, std::size_t burnIn,
                                                    std::size_t thinning, SplitMix64 &rng) {
    const double v0 = std::max(covXX, 1e-14);
    const double v1 = std::max(covAA, 1e-14);
    const double c01 = covXA;
    const double s0Cond = std::sqrt(std::max(v0 - (c01 * c01) / v1, 1e-14));
    const double s1Cond = std::sqrt(std::max(v1 - (c01 * c01) / v0, 1e-14));

    double x0 = std::isfinite(upperX) ? std::min(meanX, upperX - 0.1 * std::sqrt(v0)) : meanX;
    double x1 = std::isfinite(upperA) ? std::min(meanA, upperA - 0.1 * std::sqrt(v1)) : meanA;

    std::array<double, 3> sumProbs{0.0, 0.0, 0.0};
    const std::size_t totalSteps = burnIn + sampleCount * thinning;
    std::size_t collected = 0;
    for (std::size_t step = 0; step < totalSteps; ++step) {
        const double m0 = meanX + (c01 / v1) * (x1 - meanA);
        x0 = sampleUpperTruncatedNormal(m0, s0Cond, upperX, rng);
        const double m1 = meanA + (c01 / v0) * (x0 - meanX);
        x1 = sampleUpperTruncatedNormal(m1, s1Cond, upperA, rng);
        if (step >= burnIn && ((step - burnIn) % thinning == 0)) {
            const auto p = jndProbabilities(x0 - x1, gamma, sigmaPref);
            sumProbs[0] += p[0];
            sumProbs[1] += p[1];
            sumProbs[2] += p[2];
            ++collected;
        }
    }
    const double denom = std::max(sumProbs[0] + sumProbs[1] + sumProbs[2], 1e-12);
    if (collected == 0) {
        return {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    }
    return {sumProbs[0] / denom, sumProbs[1] / denom, sumProbs[2] / denom};
}

} // namespace

// ---------------------------------------------------------------------------
// Scalar Math Implementations
// ---------------------------------------------------------------------------

double ndtr(double x) { return 0.5 * std::erfc(-x * kInvSqrt2); }

double logNdtr(double x) {
    if (x >= 0.0) {
        return std::log1p(-0.5 * std::erfc(x * kInvSqrt2));
    }
    if (x >= -10.0) {
        return std::log(0.5 * std::erfc(-x * kInvSqrt2));
    }
    // Deep negative tail: Phi(x) = phi(x) / millsRatio(x) => log Phi(x) = -0.5 x^2 - log(sqrt(2pi)) - log(mills)
    const double mills = laplaceMillsRatioNegTail(-x);
    return -0.5 * x * x - kLogSqrt2Pi - std::log(mills);
}

double ndtri(double p) {
    if (p <= 0.0) {
        return -std::numeric_limits<double>::infinity();
    }
    if (p >= 1.0) {
        return std::numeric_limits<double>::infinity();
    }
    // Peter J. Acklam's rational approximation (< 1.15e-9 relative error) + 1 Halley refinement (< 1e-15).
    static constexpr double a1 = -3.969683028665376e+01;
    static constexpr double a2 = 2.209460984245205e+02;
    static constexpr double a3 = -2.759285104469687e+02;
    static constexpr double a4 = 1.383577518672690e+02;
    static constexpr double a5 = -3.066479806614716e+01;
    static constexpr double a6 = 2.506628277459239e+00;

    static constexpr double b1 = -5.447609879822406e+01;
    static constexpr double b2 = 1.615858368580409e+02;
    static constexpr double b3 = -1.556989798598866e+02;
    static constexpr double b4 = 6.680131188771972e+01;
    static constexpr double b5 = -1.328068155288572e+01;

    static constexpr double c1 = -7.784894002430293e-03;
    static constexpr double c2 = -3.223964580411365e-01;
    static constexpr double c3 = -2.400758277161838e+00;
    static constexpr double c4 = -2.549732539343734e+00;
    static constexpr double c5 = 4.374664141464968e+00;
    static constexpr double c6 = 2.938163982698783e+00;

    static constexpr double d1 = 7.784695709041462e-03;
    static constexpr double d2 = 3.224671290700398e-01;
    static constexpr double d3 = 2.445134137142996e+00;
    static constexpr double d4 = 3.754408661907416e+00;

    static constexpr double pLow = 0.02425;
    static constexpr double pHigh = 1.0 - pLow;

    double x = 0.0;
    if (p < pLow) {
        const double q = std::sqrt(-2.0 * std::log(p));
        x = (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
            ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
    } else if (p <= pHigh) {
        const double q = p - 0.5;
        const double r = q * q;
        x = (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q /
            (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
    } else {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        x = -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
            ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
    }

    // One Halley step refines to full float64 precision.
    const double err = ndtr(x) - p;
    const double pdf = kInvSqrt2Pi * std::exp(-0.5 * x * x);
    if (pdf > 1e-300) {
        const double u = err / pdf;
        x -= u / (1.0 + 0.5 * x * u);
    }
    return x;
}

double softplus(double x) {
    if (x > 25.0) {
        return x;
    }
    return std::log1p(std::exp(std::clamp(x, -50.0, 25.0)));
}

double inverseSoftplus(double value) {
    if (value > 20.0) {
        return value;
    }
    return std::log(std::expm1(std::max(value, 1e-12)));
}

double sigmoid(double x) {
    const double c = std::clamp(x, -50.0, 50.0);
    if (c >= 0.0) {
        return 1.0 / (1.0 + std::exp(-c));
    }
    const double e = std::exp(c);
    return e / (1.0 + e);
}

std::array<double, 3> jndProbabilities(double difference, double gamma, double sigmaPref) {
    const double scale = kSqrt2 * sigmaPref;
    const double pNew = ndtr((difference - gamma) / scale);
    const double pAnc = ndtr((-difference - gamma) / scale);
    const double zHi = (gamma - difference) / scale;
    const double zLo = (-gamma - difference) / scale;
    const double pTie = std::max((zLo > 0.0) ? (ndtr(-zLo) - ndtr(-zHi)) : (ndtr(zHi) - ndtr(zLo)), 0.0);
    return {pNew, pTie, pAnc};
}

// ---------------------------------------------------------------------------
// Physics Proxy (phi0) & Robust Scaler
// ---------------------------------------------------------------------------

std::array<double, PHYSICS_PROXY_DIM> computePhi0(double finenessNormalized, double doseG, double targetOutputG,
                                                  const PhysicsProxyConfig &cfg) {
    const double fineness = finenessNormalized;
    double basketAreaCm2 = 1.0;
    double bedDepth = doseG;
    if (cfg.hasBasketDiameter && cfg.basketDiameterMm > 0.0) {
        const double dCm = cfg.basketDiameterMm / 10.0;
        basketAreaCm2 = kPi * (0.5 * dCm) * (0.5 * dCm);
        bedDepth = doseG / (basketAreaCm2 * cfg.bulkDensityGCm3 * cfg.packingFraction);
    }

    double particleSize = 0.0;
    if (cfg.hasCalibratedParticleSize && cfg.calibratedCoarseParticleSizeUm > cfg.calibratedFineParticleSizeUm &&
        cfg.calibratedFineParticleSizeUm > 0.0) {
        particleSize = std::exp((1.0 - fineness) * std::log(cfg.calibratedCoarseParticleSizeUm) +
                                fineness * std::log(cfg.calibratedFineParticleSizeUm));
    } else {
        particleSize = std::exp(-cfg.particleSizeBeta * fineness);
    }

    const double permeability = particleSize * particleSize;
    const double resistance = bedDepth / (permeability + cfg.epsilon);
    const double flow = cfg.nominalPressureBar / (resistance + cfg.epsilon);
    const double expectedDuration =
        targetOutputG / (flow * basketAreaCm2 * cfg.nominalWaterDensityGCm3 + cfg.epsilon);
    const double brewRatio = (doseG > 0.0) ? (targetOutputG / doseG) : 0.0;

    return {
        doseG,
        targetOutputG,
        brewRatio,
        fineness,
        std::log(particleSize + cfg.epsilon),
        std::log(bedDepth + cfg.epsilon),
        std::log(resistance + cfg.epsilon),
        std::log(flow + cfg.epsilon),
        std::log(expectedDuration + cfg.epsilon),
    };
}

bool EmbeddedRobustScaler::fit(const double *rows, std::size_t nRows, std::size_t nCols, double scaleFloor) {
    if (rows == nullptr || nRows == 0 || nCols == 0 || !std::isfinite(scaleFloor) || scaleFloor <= 0.0) {
        return false;
    }
    median.assign(nCols, 0.0);
    scale.assign(nCols, 1.0);
    std::vector<double> col(nRows, 0.0);
    std::vector<double> absDev(nRows, 0.0);

    for (std::size_t j = 0; j < nCols; ++j) {
        for (std::size_t i = 0; i < nRows; ++i) {
            const double v = rows[i * nCols + j];
            if (!std::isfinite(v)) {
                return false;
            }
            col[i] = v;
        }
        std::sort(col.begin(), col.end());
        const double med = linearQuantileSorted(col, 0.50);
        const double q1 = linearQuantileSorted(col, 0.25);
        const double q3 = linearQuantileSorted(col, 0.75);
        const double iqr = q3 - q1;

        for (std::size_t i = 0; i < nRows; ++i) {
            absDev[i] = std::fabs(col[i] - med);
        }
        std::sort(absDev.begin(), absDev.end());
        const double mad = linearQuantileSorted(absDev, 0.50) * 1.4826;

        median[j] = med;
        scale[j] = std::max(iqr > scaleFloor ? iqr : mad, scaleFloor);
    }
    return true;
}

bool EmbeddedRobustScaler::transformRow(const double *inRow, double *outRow) const {
    if (inRow == nullptr || outRow == nullptr || median.empty() || median.size() != scale.size()) {
        return false;
    }
    for (std::size_t j = 0; j < median.size(); ++j) {
        if (!std::isfinite(inRow[j])) {
            return false;
        }
        outRow[j] = (inRow[j] - median[j]) / scale[j];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Level 1 TuRPBO Trust Region
// ---------------------------------------------------------------------------

bool updateLevel1TrustRegion(const Level1TrustRegionState &state, PreferenceLabel label,
                             const std::vector<double> &candidateCenter, const TrustRegionConfig &cfg,
                             Level1TrustRegionState &outState) {
    if (state.center.empty() || state.locallyConverged || !std::isfinite(state.length) || state.length <= 0.0) {
        return false;
    }
    const std::size_t dim = state.center.size();
    std::vector<double> center = state.center;
    int successes = state.successCount;
    int failures = state.failureCount;
    TrustRegionAction action = TrustRegionAction::NONE;

    if (label == PreferenceLabel::NEW_BETTER) {
        if (candidateCenter.size() != dim) {
            return false;
        }
        for (std::size_t i = 0; i < dim; ++i) {
            if (!std::isfinite(candidateCenter[i]) || candidateCenter[i] < -1e-9 || candidateCenter[i] > 1.0 + 1e-9) {
                return false;
            }
            center[i] = std::clamp(candidateCenter[i], 0.0, 1.0);
        }
        successes += 1;
        failures = 0;
        action = TrustRegionAction::IMPROVED;
    } else {
        successes = 0;
        failures += 1;
        action = TrustRegionAction::NON_IMPROVEMENT;
    }

    double length = state.length;
    bool converged = false;
    if (successes >= cfg.successTolerance) {
        length = std::min(2.0 * length, cfg.maximumLength);
        successes = 0;
        failures = 0;
        action = TrustRegionAction::EXPANDED;
    } else if (failures >= cfg.failureTolerance) {
        length = std::max(cfg.minimumLength, 0.5 * length);
        successes = 0;
        failures = 0;
        converged = (length <= cfg.minimumLength);
        action = converged ? TrustRegionAction::CONVERGED : TrustRegionAction::CONTRACTED;
    }

    outState.center = std::move(center);
    outState.length = length;
    outState.successCount = successes;
    outState.failureCount = failures;
    outState.locallyConverged = converged;
    outState.lastAction = action;
    return true;
}

bool computeLevel1TrustRegionBounds(const Level1TrustRegionState &state, const std::vector<double> &lengthscales,
                                    const TrustRegionConfig &cfg, std::vector<double> &outLower,
                                    std::vector<double> &outUpper) {
    const std::size_t dim = state.center.size();
    if (dim == 0 || lengthscales.size() != dim || !std::isfinite(state.length) || state.length <= 0.0) {
        return false;
    }
    double sumLog = 0.0;
    for (double ls : lengthscales) {
        if (!std::isfinite(ls) || ls <= 0.0) {
            return false;
        }
        sumLog += std::log(ls);
    }
    const double geom = std::exp(sumLog / static_cast<double>(dim));

    std::vector<double> shape(dim, 1.0);
    double sumLogShape = 0.0;
    for (std::size_t i = 0; i < dim; ++i) {
        shape[i] = std::clamp(lengthscales[i] / geom, cfg.shapeFactorMin, cfg.shapeFactorMax);
        sumLogShape += std::log(shape[i]);
    }
    const double shapeGeom = std::exp(sumLogShape / static_cast<double>(dim));

    outLower.resize(dim);
    outUpper.resize(dim);
    for (std::size_t i = 0; i < dim; ++i) {
        const double s = std::clamp(shape[i] / shapeGeom, cfg.shapeFactorMin, cfg.shapeFactorMax);
        const double half = 0.5 * state.length * s;
        outLower[i] = std::clamp(state.center[i] - half, 0.0, 1.0);
        outUpper[i] = std::clamp(state.center[i] + half, 0.0, 1.0);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Kernel Hyperparams & Preference GP Prediction / Fitting
// ---------------------------------------------------------------------------

std::vector<double> EmbeddedKernelHyperparams::rawLengthscales() const {
    std::vector<double> out(rawLogLengthscales.size());
    for (std::size_t i = 0; i < rawLogLengthscales.size(); ++i) {
        out[i] = std::exp(rawLogLengthscales[i]);
    }
    return out;
}

std::vector<double> EmbeddedKernelHyperparams::physicsLengthscales() const {
    std::vector<double> out(physicsLogLengthscales.size());
    for (std::size_t i = 0; i < physicsLogLengthscales.size(); ++i) {
        out[i] = std::exp(physicsLogLengthscales[i]);
    }
    return out;
}

std::vector<double> EmbeddedKernelHyperparams::traceLengthscales() const {
    std::vector<double> out(traceLogLengthscales.size());
    for (std::size_t i = 0; i < traceLogLengthscales.size(); ++i) {
        out[i] = std::exp(traceLogLengthscales[i]);
    }
    return out;
}

std::vector<double> EmbeddedKernelHyperparams::mixtureWeights() const { return softmaxVec(weightLogits); }

bool EmbeddedPreferenceGPModel::predict(const double *queryInputs, std::size_t queryCount, std::vector<double> &outMean,
                                        std::vector<double> &outCov, const double *queryPriorMean) const {
    if (!valid || queryInputs == nullptr || queryCount == 0 || trainCount == 0) {
        return false;
    }
    for (std::size_t i = 0; i < queryCount * featureStride; ++i) {
        if (!std::isfinite(queryInputs[i])) {
            return false;
        }
    }
    if (queryPriorMean != nullptr) {
        for (std::size_t j = 0; j < queryCount; ++j) {
            if (!std::isfinite(queryPriorMean[j])) {
                return false;
            }
        }
    }

    std::vector<double> kXxs, kSs;
    evaluateAdditiveKernelMatrix(trainInputs.data(), trainCount, queryInputs, queryCount, hyperparams, rawDim,
                                 physicsDim, traceDim, kXxs);
    evaluateAdditiveKernelMatrix(queryInputs, queryCount, queryInputs, queryCount, hyperparams, rawDim, physicsDim,
                                 traceDim, kSs);

    // V_* = L_K^{-1} K_{X, X_*} [N x M], U_* = L_B^{-1} V_* [N x M]
    std::vector<double> vStar(trainCount * queryCount, 0.0);
    std::vector<double> uStar(trainCount * queryCount, 0.0);
    solveLowerMat(cholK.data(), trainCount, kXxs.data(), queryCount, vStar.data());
    solveLowerMat(cholB.data(), trainCount, vStar.data(), queryCount, uStar.data());

    outMean.assign(queryCount, 0.0);
    for (std::size_t j = 0; j < queryCount; ++j) {
        double m = (queryPriorMean != nullptr) ? queryPriorMean[j] : 0.0;
        for (std::size_t i = 0; i < trainCount; ++i) {
            m += vStar[i * queryCount + j] * whitenedMean[i];
        }
        outMean[j] = m;
    }

    outCov.assign(queryCount * queryCount, 0.0);
    for (std::size_t a = 0; a < queryCount; ++a) {
        for (std::size_t b = 0; b <= a; ++b) {
            double vDot = 0.0;
            double uDot = 0.0;
            for (std::size_t i = 0; i < trainCount; ++i) {
                vDot += vStar[i * queryCount + a] * vStar[i * queryCount + b];
                uDot += uStar[i * queryCount + a] * uStar[i * queryCount + b];
            }
            const double val = kSs[a * queryCount + b] - vDot + uDot;
            outCov[a * queryCount + b] = val;
            outCov[b * queryCount + a] = val;
        }
    }
    return true;
}

bool evaluatePreferenceGpObjectiveAndGrad(const double *trainInputs, std::size_t trainCount, std::size_t rawDim,
                                          std::size_t physicsDim, std::size_t traceDim,
                                          const ComparisonPair *comparisons, std::size_t comparisonCount,
                                          const PreferenceGPConfig &cfg, const std::vector<double> &packedTheta,
                                          double &outNegElbo, std::vector<double> &outGrad) {
    if (trainInputs == nullptr || trainCount == 0 || rawDim == 0 || physicsDim == 0 || cfg.sigmaPref <= 0.0 ||
        !std::isfinite(cfg.sigmaPref)) {
        return false;
    }
    const std::size_t nWeights = traceDim > 0 ? 3 : 2;
    const std::size_t expectedDim = rawDim + physicsDim + traceDim + nWeights + (cfg.learnGamma ? 1 : 0);
    if (packedTheta.size() != expectedDim) {
        return false;
    }
    for (std::size_t c = 0; c < comparisonCount; ++c) {
        if (comparisons == nullptr || comparisons[c].newIdx >= trainCount || comparisons[c].anchorIdx >= trainCount) {
            return false;
        }
    }
    std::vector<double> cacheM, cacheL;
    bool cancelled = false;
    const double initRawGamma = inverseSoftplus(std::max(cfg.initialGamma, 1e-12));
    return evalEnvelopeObjective(trainInputs, trainCount, rawDim, physicsDim, traceDim, comparisons, comparisonCount,
                                 cfg, initRawGamma, packedTheta, cacheM, cacheL, cfg.maxInnerIters, nullptr, nullptr,
                                 outNegElbo, outGrad, cancelled);
}

bool fitEmbeddedPreferenceGP(const double *trainInputs, std::size_t trainCount, std::size_t rawDim,
                             std::size_t physicsDim, std::size_t traceDim, const ComparisonPair *comparisons,
                             std::size_t comparisonCount, const PreferenceGPConfig &cfg,
                             EmbeddedPreferenceGPModel &outModel, YieldCallback onYield, void *yieldCtx,
                             const double *trainPriorMean) {
    outModel = EmbeddedPreferenceGPModel{};

    if (trainInputs == nullptr || trainCount == 0 || rawDim == 0 || physicsDim == 0 || !std::isfinite(cfg.sigmaPref) ||
        cfg.sigmaPref <= 0.0 || !std::isfinite(cfg.initialGamma) || cfg.initialGamma < 0.0 ||
        !std::isfinite(cfg.covarianceJitter) || cfg.covarianceJitter <= 0.0) {
        return false;
    }
    if (trainPriorMean != nullptr) {
        for (std::size_t i = 0; i < trainCount; ++i) {
            if (!std::isfinite(trainPriorMean[i])) {
                return false;
            }
        }
    }
    const std::size_t stride = computeFeatureStride(rawDim, physicsDim, traceDim);
    for (std::size_t i = 0; i < trainCount * stride; ++i) {
        if (!std::isfinite(trainInputs[i])) {
            return false;
        }
    }
    for (std::size_t c = 0; c < comparisonCount; ++c) {
        if (comparisons == nullptr || comparisons[c].newIdx >= trainCount || comparisons[c].anchorIdx >= trainCount) {
            return false;
        }
    }

    const double initLs = std::log(std::log(2.0));
    const std::size_t nWeights = traceDim > 0 ? 3 : 2;
    std::vector<double> wInit(nWeights, 0.0);
    wInit[0] = std::max(cfg.initialRawKernelWeight, 1e-12);
    wInit[1] = std::max(cfg.initialPhysicsKernelWeight, 1e-12);
    if (traceDim > 0) {
        wInit[2] = std::max(cfg.initialTraceKernelWeight, 1e-6);
    }
    const double wSum = std::accumulate(wInit.begin(), wInit.end(), 0.0);
    std::vector<double> initLogits(nWeights, 0.0);
    for (std::size_t i = 0; i < nWeights; ++i) {
        initLogits[i] = std::log(wInit[i] / std::max(wSum, 1e-12));
    }
    const double initRawGamma = inverseSoftplus(std::max(cfg.initialGamma, 1e-12));

    EmbeddedKernelHyperparams initHp;
    initHp.rawLogLengthscales.assign(rawDim, initLs);
    initHp.physicsLogLengthscales.assign(physicsDim, initLs);
    initHp.traceLogLengthscales.assign(traceDim, initLs);
    initHp.weightLogits = initLogits;

    if (comparisonCount == 0) {
        std::vector<double> kMat, cholK;
        double jitterUsed = 0.0;
        evaluateAdditiveKernelMatrix(trainInputs, trainCount, trainInputs, trainCount, initHp, rawDim, physicsDim,
                                     traceDim, kMat);
        if (!stableCholesky(kMat.data(), trainCount, cfg.covarianceJitter, cholK, jitterUsed)) {
            return false;
        }
        outModel.valid = true;
        outModel.converged = true;
        outModel.trainCount = trainCount;
        outModel.featureStride = stride;
        outModel.rawDim = rawDim;
        outModel.physicsDim = physicsDim;
        outModel.traceDim = traceDim;
        outModel.trainInputs.assign(trainInputs, trainInputs + trainCount * stride);
        outModel.hyperparams = std::move(initHp);
        outModel.gamma = cfg.initialGamma;
        outModel.sigmaPref = cfg.sigmaPref;
        outModel.jitterUsed = jitterUsed;
        outModel.cholK = std::move(cholK);
        outModel.cholB.assign(trainCount * trainCount, 0.0);
        for (std::size_t i = 0; i < trainCount; ++i) {
            outModel.cholB[i * trainCount + i] = 1.0;
        }
        outModel.whitenedMean.assign(trainCount, 0.0);
        outModel.elbo = 0.0;
        return true;
    }

    // Pack initial outer parameter vector and box bounds.
    const std::size_t totalDim = rawDim + physicsDim + traceDim + nWeights + (cfg.learnGamma ? 1 : 0);
    std::vector<double> x(totalDim, 0.0);
    std::vector<double> lo(totalDim, 0.0);
    std::vector<double> hi(totalDim, 0.0);
    {
        std::size_t pos = 0;
        const double logLsMin = std::log(0.05);
        const double logLsMax = std::log(20.0);
        for (std::size_t i = 0; i < rawDim + physicsDim + traceDim; ++i) {
            x[pos] = initLs;
            lo[pos] = logLsMin;
            hi[pos] = logLsMax;
            ++pos;
        }
        for (std::size_t i = 0; i < nWeights; ++i) {
            x[pos] = initLogits[i];
            lo[pos] = -5.0;
            hi[pos] = 5.0;
            ++pos;
        }
        if (cfg.learnGamma) {
            x[pos] = std::clamp(initRawGamma, -8.0, 3.0);
            lo[pos] = -8.0;
            hi[pos] = 3.0;
        }
    }

    std::vector<double> cacheMBar, cacheLam;
    double fVal = 0.0;
    std::vector<double> gVal(totalDim, 0.0);
    bool cancelled = false;
    if (!evalEnvelopeObjective(trainInputs, trainCount, rawDim, physicsDim, traceDim, comparisons, comparisonCount, cfg,
                               initRawGamma, x, cacheMBar, cacheLam, cfg.maxInnerIters, onYield, yieldCtx, fVal, gVal,
                               cancelled, trainPriorMean)) {
        outModel.cancelled = cancelled;
        return false;
    }

    // Projected Limited-Memory BFGS (box-constrained, history m = 10).
    constexpr std::size_t kHistCap = 10;
    std::vector<std::vector<double>> sHist, yHist;
    std::vector<double> rhoHist;
    sHist.reserve(kHistCap);
    yHist.reserve(kHistCap);
    rhoHist.reserve(kHistCap);

    std::vector<uint8_t> isActive(totalDim, 0);
    std::vector<double> dir(totalDim, 0.0);
    std::vector<double> qVec(totalDim, 0.0);
    std::vector<double> xTrial(totalDim, 0.0);
    std::vector<double> gTrial(totalDim, 0.0);
    std::vector<double> sVec(totalDim, 0.0);
    std::vector<double> yVec(totalDim, 0.0);

    bool converged = false;
    int outerIter = 0;

    for (; outerIter < cfg.maxOuterIters; ++outerIter) {
        if (onYield != nullptr && !onYield(yieldCtx)) {
            outModel.cancelled = true;
            return false;
        }

        // Exact box-constrained KKT projected gradient infinity-norm.
        double projGradInf = 0.0;
        double freeGradNorm2 = 0.0;
        for (std::size_t i = 0; i < totalDim; ++i) {
            const double pg = x[i] - std::clamp(x[i] - gVal[i], lo[i], hi[i]);
            projGradInf = std::max(projGradInf, std::fabs(pg));
            const bool bindLo = (x[i] <= lo[i] + 1e-11) && (gVal[i] > 0.0);
            const bool bindHi = (x[i] >= hi[i] - 1e-11) && (gVal[i] < 0.0);
            isActive[i] = (bindLo || bindHi) ? 1u : 0u;
            if (!isActive[i]) {
                freeGradNorm2 += gVal[i] * gVal[i];
            }
        }
        if (projGradInf <= 1e-7) {
            converged = true;
            break;
        }

        // Compute projected L-BFGS direction on free variables.
        auto setSteepestDescent = [&]() {
            double maxAbs = 0.0;
            for (std::size_t i = 0; i < totalDim; ++i) {
                dir[i] = isActive[i] ? 0.0 : -gVal[i];
                maxAbs = std::max(maxAbs, std::fabs(dir[i]));
            }
            if (maxAbs > 1.0) {
                const double scale = 1.0 / maxAbs;
                for (double &d : dir) {
                    d *= scale;
                }
            }
        };

        auto computeDirection = [&](bool useHistory) {
            if (!useHistory || sHist.empty()) {
                setSteepestDescent();
                return;
            }
            for (std::size_t i = 0; i < totalDim; ++i) {
                qVec[i] = isActive[i] ? 0.0 : gVal[i];
            }

            const std::size_t m = sHist.size();
            std::vector<double> alphaH(m, 0.0);
            for (std::size_t rev = 0; rev < m; ++rev) {
                const std::size_t idx = m - 1 - rev;
                double dot = 0.0;
                for (std::size_t i = 0; i < totalDim; ++i) {
                    if (!isActive[i]) {
                        dot += sHist[idx][i] * qVec[i];
                    }
                }
                alphaH[idx] = rhoHist[idx] * dot;
                for (std::size_t i = 0; i < totalDim; ++i) {
                    if (!isActive[i]) {
                        qVec[i] -= alphaH[idx] * yHist[idx][i];
                    }
                }
            }

            double syLast = 0.0;
            double yyLast = 0.0;
            for (std::size_t i = 0; i < totalDim; ++i) {
                if (!isActive[i]) {
                    syLast += sHist.back()[i] * yHist.back()[i];
                    yyLast += yHist.back()[i] * yHist.back()[i];
                }
            }
            const double gammaH = (yyLast > 1e-24 && syLast > 0.0) ? std::clamp(syLast / yyLast, 1e-5, 1e5) : 1.0;
            for (std::size_t i = 0; i < totalDim; ++i) {
                dir[i] = isActive[i] ? 0.0 : (gammaH * qVec[i]);
            }

            for (std::size_t idx = 0; idx < m; ++idx) {
                double dot = 0.0;
                for (std::size_t i = 0; i < totalDim; ++i) {
                    if (!isActive[i]) {
                        dot += yHist[idx][i] * dir[i];
                    }
                }
                const double betaH = rhoHist[idx] * dot;
                for (std::size_t i = 0; i < totalDim; ++i) {
                    if (!isActive[i]) {
                        dir[i] += sHist[idx][i] * (alphaH[idx] - betaH);
                    }
                }
            }

            double gDotD = 0.0;
            double dNorm2 = 0.0;
            for (std::size_t i = 0; i < totalDim; ++i) {
                dir[i] = isActive[i] ? 0.0 : -dir[i];
                gDotD += gVal[i] * dir[i];
                dNorm2 += dir[i] * dir[i];
            }
            if (!std::isfinite(gDotD) || gDotD >= -1e-12 * std::sqrt(freeGradNorm2 * dNorm2)) {
                sHist.clear();
                yHist.clear();
                rhoHist.clear();
                setSteepestDescent();
            }
        };

        computeDirection(true);

        // Projected Armijo backtracking line search.
        auto lineSearch = [&](double &outFNew) -> bool {
            double step = 1.0;
            for (int ls = 0; ls < 18; ++ls) {
                double slope = 0.0;
                double maxDelta = 0.0;
                for (std::size_t i = 0; i < totalDim; ++i) {
                    xTrial[i] = std::clamp(x[i] + step * dir[i], lo[i], hi[i]);
                    const double dx = xTrial[i] - x[i];
                    slope += gVal[i] * dx;
                    maxDelta = std::max(maxDelta, std::fabs(dx));
                }
                if (maxDelta <= 1e-15) {
                    return false;
                }
                if (slope < 0.0) {
                    double fCand = 0.0;
                    bool lsCancelled = false;
                    if (evalEnvelopeObjective(trainInputs, trainCount, rawDim, physicsDim, traceDim, comparisons,
                                              comparisonCount, cfg, initRawGamma, xTrial, cacheMBar, cacheLam,
                                              cfg.maxInnerIters, onYield, yieldCtx, fCand, gTrial, lsCancelled,
                                              trainPriorMean)) {
                        if (fCand <= fVal + 1e-4 * slope) {
                            outFNew = fCand;
                            return true;
                        }
                    } else if (lsCancelled) {
                        outModel.cancelled = true;
                        return false;
                    }
                }
                step *= 0.5;
            }
            return false;
        };

        double fNew = fVal;
        bool accepted = lineSearch(fNew);
        if (outModel.cancelled) {
            return false;
        }
        if (!accepted && !sHist.empty()) {
            // Reset L-BFGS history and retry projected steepest descent.
            sHist.clear();
            yHist.clear();
            rhoHist.clear();
            computeDirection(false);
            accepted = lineSearch(fNew);
            if (outModel.cancelled) {
                return false;
            }
        }
        if (!accepted) {
            converged = (projGradInf <= 1e-4);
            break;
        }

        double sy = 0.0;
        double ss = 0.0;
        double yy = 0.0;
        for (std::size_t i = 0; i < totalDim; ++i) {
            sVec[i] = xTrial[i] - x[i];
            yVec[i] = gTrial[i] - gVal[i];
            sy += sVec[i] * yVec[i];
            ss += sVec[i] * sVec[i];
            yy += yVec[i] * yVec[i];
        }

        const double fChange = std::fabs(fVal - fNew);
        const double fScale = std::max({1.0, std::fabs(fVal), std::fabs(fNew)});
        x.swap(xTrial);
        gVal.swap(gTrial);
        fVal = fNew;

        if (fChange <= 1e-10 * fScale && std::sqrt(ss) <= 1e-6) {
            converged = true;
            ++outerIter;
            break;
        }

        if (sy > 1e-12 * std::sqrt(ss * yy) && yy > 1e-24) {
            if (sHist.size() == kHistCap) {
                sHist.erase(sHist.begin());
                yHist.erase(yHist.begin());
                rhoHist.erase(rhoHist.begin());
            }
            sHist.push_back(sVec);
            yHist.push_back(yVec);
            rhoHist.push_back(1.0 / sy);
        }
    }

    // Final high-accuracy inner VI solve at optimized hyperparameters.
    EmbeddedKernelHyperparams optHp;
    double optRawG = initRawGamma;
    unpackHyperparams(x, rawDim, physicsDim, traceDim, cfg.learnGamma, initRawGamma, optHp, optRawG);
    const double optGamma = cfg.learnGamma ? softplus(optRawG) : cfg.initialGamma;

    std::vector<double> kOpt, cholK;
    double jitterUsed = 0.0;
    evaluateAdditiveKernelMatrix(trainInputs, trainCount, trainInputs, trainCount, optHp, rawDim, physicsDim, traceDim,
                                 kOpt);
    if (!stableCholesky(kOpt.data(), trainCount, cfg.covarianceJitter, cholK, jitterUsed)) {
        return false;
    }

    const std::vector<double> *mInit = (cacheMBar.size() == trainCount) ? &cacheMBar : nullptr;
    const std::vector<double> *lInit = (cacheLam.size() == comparisonCount) ? &cacheLam : nullptr;
    InnerViResult finalInner =
        solveInnerOpperArchambeau(cholK, trainCount, comparisons, comparisonCount, optGamma, cfg.sigmaPref,
                                  cfg.probabilityEpsilon, mInit, lInit, cfg.finalInnerIters, 1e-10, onYield, yieldCtx,
                                  trainPriorMean);
    if (finalInner.cancelled) {
        outModel.cancelled = true;
        return false;
    }
    if (!finalInner.ok) {
        return false;
    }

    outModel.valid = true;
    outModel.converged = converged;
    outModel.outerIterations = outerIter;
    outModel.trainCount = trainCount;
    outModel.featureStride = stride;
    outModel.rawDim = rawDim;
    outModel.physicsDim = physicsDim;
    outModel.traceDim = traceDim;
    outModel.trainInputs.assign(trainInputs, trainInputs + trainCount * stride);
    outModel.hyperparams = std::move(optHp);
    outModel.gamma = optGamma;
    outModel.sigmaPref = cfg.sigmaPref;
    outModel.jitterUsed = jitterUsed;
    outModel.cholK = std::move(cholK);
    outModel.cholB = std::move(finalInner.cholB);
    outModel.whitenedMean = std::move(finalInner.mBar);
    outModel.dualLambda = std::move(finalInner.lam);
    outModel.elbo = finalInner.elbo;
    return true;
}

// ---------------------------------------------------------------------------
// Conditional-Coordinate Gauss-Legendre MES + Robert-Gibbs Far-Tail Fallback
// ---------------------------------------------------------------------------

bool conditionalJndProbsBivariateTruncated(double meanX, double meanA, double covXX, double covXA, double covAA,
                                           double upperX, double upperA, double gamma, double sigmaPref,
                                           std::uint64_t seed, std::size_t gibbsSamples, std::size_t gibbsBurnIn,
                                           std::size_t gibbsThinning, std::array<double, 3> &outProbs,
                                           bool &outUsedGibbs) {
    if (!std::isfinite(meanX) || !std::isfinite(meanA) || !std::isfinite(covXX) || !std::isfinite(covXA) ||
        !std::isfinite(covAA) || !std::isfinite(gamma) || !std::isfinite(sigmaPref) || sigmaPref <= 0.0) {
        return false;
    }

    const double vX = std::max(covXX, 1e-12);
    const double vA = std::max(covAA, 1e-12);
    const double sX = std::sqrt(vX);
    const double sA = std::sqrt(vA);
    const double rho = std::clamp(covXA / (sX * sA), -0.999999, 0.999999);

    const double alphaX = std::isfinite(upperX) ? ((upperX - meanX) / sX) : 8.0;
    const double alphaA = std::isfinite(upperA) ? ((upperA - meanA) / sA) : 8.0;

    if (alphaX < -4.5 || alphaA < -4.5 || std::fabs(rho) > 0.9995) {
        SplitMix64 rng(seed);
        outProbs = conditionalProbsGibbsFallback(meanX, meanA, covXX, covXA, covAA, upperX, upperA, gamma, sigmaPref,
                                                 gibbsSamples, gibbsBurnIn, gibbsThinning, rng);
        outUsedGibbs = true;
        return true;
    }

    // Active Gaussian support for z_a (24 nodes).
    const double aHi = std::min(alphaA, 5.5);
    const double aLo = (alphaA >= 0.0) ? -5.5 : (aHi - 5.0);
    const double halfA = 0.5 * (aHi - aLo);
    const double midA = 0.5 * (aHi + aLo);

    const double sXCond = sX * std::sqrt(std::max(1.0 - rho * rho, 1e-12));

    std::array<double, 24> fAArr{};
    std::array<double, 24> logWaArr{};
    std::array<double, 24> mXCondArr{};
    std::array<double, 24> alphaXCondArr{};

    bool deepTail = false;
    for (std::size_t ia = 0; ia < 24; ++ia) {
        const double zA = midA + halfA * kGl24Nodes[ia];
        fAArr[ia] = meanA + sA * zA;
        logWaArr[ia] = std::log(halfA * kGl24Weights[ia]) - 0.5 * zA * zA;
        mXCondArr[ia] = meanX + rho * sX * zA;
        alphaXCondArr[ia] = std::isfinite(upperX) ? ((upperX - mXCondArr[ia]) / sXCond) : 5.5;
        if (alphaXCondArr[ia] < -6.0) {
            deepTail = true;
        }
    }

    if (deepTail) {
        SplitMix64 rng(seed);
        outProbs = conditionalProbsGibbsFallback(meanX, meanA, covXX, covXA, covAA, upperX, upperA, gamma, sigmaPref,
                                                 gibbsSamples, gibbsBurnIn, gibbsThinning, rng);
        outUsedGibbs = true;
        return true;
    }

    // Streaming two-pass log-sum-exp over 24 x 20 = 480 nodes (zero heap, minimal stack).
    std::array<double, 20> logGl20Weights{};
    for (std::size_t ix = 0; ix < 20; ++ix) {
        logGl20Weights[ix] = std::log(kGl20Weights[ix]);
    }

    double maxLog = -std::numeric_limits<double>::infinity();
    for (std::size_t ia = 0; ia < 24; ++ia) {
        const double aXc = alphaXCondArr[ia];
        const double xHi = std::min(aXc, 5.5);
        const double xLo = (aXc >= 0.0) ? -5.5 : (xHi - 5.0);
        const double halfX = 0.5 * (xHi - xLo);
        const double midX = 0.5 * (xHi + xLo);
        const double baseLog = logWaArr[ia] + std::log(halfX);
        for (std::size_t ix = 0; ix < 20; ++ix) {
            const double zX = midX + halfX * kGl20Nodes[ix];
            const double lj = baseLog + logGl20Weights[ix] - 0.5 * zX * zX;
            if (lj > maxLog) {
                maxLog = lj;
            }
        }
    }

    double wSum = 0.0;
    std::array<double, 3> probs{0.0, 0.0, 0.0};
    for (std::size_t ia = 0; ia < 24; ++ia) {
        const double aXc = alphaXCondArr[ia];
        const double xHi = std::min(aXc, 5.5);
        const double xLo = (aXc >= 0.0) ? -5.5 : (xHi - 5.0);
        const double halfX = 0.5 * (xHi - xLo);
        const double midX = 0.5 * (xHi + xLo);
        const double baseLog = logWaArr[ia] + std::log(halfX);
        for (std::size_t ix = 0; ix < 20; ++ix) {
            const double zX = midX + halfX * kGl20Nodes[ix];
            const double lj = baseLog + logGl20Weights[ix] - 0.5 * zX * zX;
            const double w = std::exp(lj - maxLog);
            wSum += w;
            const double fX = mXCondArr[ia] + sXCond * zX;
            const auto p = jndProbabilities(fX - fAArr[ia], gamma, sigmaPref);
            probs[0] += w * p[0];
            probs[1] += w * p[1];
            probs[2] += w * p[2];
        }
    }

    const double norm = std::max(wSum, 1e-300);
    probs[0] = std::max(probs[0] / norm, 0.0);
    probs[1] = std::max(probs[1] / norm, 0.0);
    probs[2] = std::max(probs[2] / norm, 0.0);
    const double pSum = std::max(probs[0] + probs[1] + probs[2], 1e-12);
    outProbs = {probs[0] / pSum, probs[1] / pSum, probs[2] / pSum};
    outUsedGibbs = false;
    return true;
}

bool evaluateEmbeddedCpboMes(const double *posteriorMean, const double *posteriorCov, std::size_t pointCount,
                             const std::size_t *candidateIndices, std::size_t candidateCount,
                             const std::size_t *maximumIndices, std::size_t maximumCount, std::size_t anchorIndex,
                             double gamma, double sigmaPref, const MESConfig &cfg, std::uint64_t seed,
                             EmbeddedMESResult &outResult, YieldCallback onYield, void *yieldCtx) {
    outResult = EmbeddedMESResult{};

    if (posteriorMean == nullptr || posteriorCov == nullptr || pointCount == 0 || candidateIndices == nullptr ||
        candidateCount == 0 || anchorIndex >= pointCount || !std::isfinite(gamma) || gamma < 0.0 ||
        !std::isfinite(sigmaPref) || sigmaPref <= 0.0) {
        return false;
    }
    for (std::size_t i = 0; i < candidateCount; ++i) {
        if (candidateIndices[i] >= pointCount) {
            return false;
        }
    }

    std::vector<std::size_t> maxIdx;
    if (maximumIndices == nullptr || maximumCount == 0) {
        maxIdx.resize(pointCount);
        std::iota(maxIdx.begin(), maxIdx.end(), 0);
    } else {
        maxIdx.assign(maximumIndices, maximumIndices + maximumCount);
        for (std::size_t idx : maxIdx) {
            if (idx >= pointCount) {
                return false;
            }
        }
    }
    const bool anchorIsFeasible = std::find(maxIdx.begin(), maxIdx.end(), anchorIndex) != maxIdx.end();

    // O(K^2) centering of posterior mean and covariance.
    double meanAvg = 0.0;
    for (std::size_t i = 0; i < pointCount; ++i) {
        if (!std::isfinite(posteriorMean[i])) {
            return false;
        }
        meanAvg += posteriorMean[i];
    }
    meanAvg /= static_cast<double>(pointCount);

    std::vector<double> centeredMean(pointCount, 0.0);
    for (std::size_t i = 0; i < pointCount; ++i) {
        centeredMean[i] = posteriorMean[i] - meanAvg;
    }

    std::vector<double> rowMean(pointCount, 0.0);
    std::vector<double> colMean(pointCount, 0.0);
    double grandMean = 0.0;
    for (std::size_t i = 0; i < pointCount; ++i) {
        for (std::size_t j = 0; j < pointCount; ++j) {
            const double v = posteriorCov[i * pointCount + j];
            if (!std::isfinite(v)) {
                return false;
            }
            rowMean[i] += v;
            colMean[j] += v;
            grandMean += v;
        }
    }
    const double invK = 1.0 / static_cast<double>(pointCount);
    for (std::size_t i = 0; i < pointCount; ++i) {
        rowMean[i] *= invK;
        colMean[i] *= invK;
    }
    grandMean *= invK * invK;

    std::vector<double> centeredCov(pointCount * pointCount, 0.0);
    for (std::size_t i = 0; i < pointCount; ++i) {
        for (std::size_t j = 0; j <= i; ++j) {
            const double cij = posteriorCov[i * pointCount + j] - rowMean[i] - colMean[j] + grandMean;
            const double cji = posteriorCov[j * pointCount + i] - rowMean[j] - colMean[i] + grandMean;
            const double sym = 0.5 * (cij + cji);
            centeredCov[i * pointCount + j] = sym;
            centeredCov[j * pointCount + i] = sym;
        }
    }

    // Build sub-covariance over feasible maximum indices & factor Cholesky.
    const std::size_t nMax = maxIdx.size();
    std::vector<double> subCov(nMax * nMax, 0.0);
    std::vector<double> subMean(nMax, 0.0);
    for (std::size_t a = 0; a < nMax; ++a) {
        subMean[a] = centeredMean[maxIdx[a]];
        for (std::size_t b = 0; b <= a; ++b) {
            const double v = centeredCov[maxIdx[a] * pointCount + maxIdx[b]];
            subCov[a * nMax + b] = v;
            subCov[b * nMax + a] = v;
        }
    }

    std::vector<double> chol;
    double jitterUsed = 0.0;
    if (!stableCholesky(subCov.data(), nMax, cfg.covarianceJitter, chol, jitterUsed)) {
        return false;
    }
    // Free subCov early to reduce peak RAM on MCU.
    std::vector<double>().swap(subCov);

    // Draw posterior_max_function_samples joint samples over maxIdx.
    SplitMix64 rng(seed + 17ULL);
    std::vector<double> directMaxima(cfg.posteriorMaxFunctionSamples, 0.0);
    std::vector<double> zNoise(nMax, 0.0);
    std::vector<double> fSample(nMax, 0.0);

    for (std::size_t s = 0; s < cfg.posteriorMaxFunctionSamples; ++s) {
        if ((s & 63u) == 0 && onYield != nullptr && !onYield(yieldCtx)) {
            outResult.cancelled = true;
            return false;
        }
        for (std::size_t j = 0; j < nMax; ++j) {
            zNoise[j] = rng.standardNormal();
        }
        double sMean = 0.0;
        for (std::size_t i = 0; i < nMax; ++i) {
            double val = subMean[i];
            const double *cRow = chol.data() + i * nMax;
            for (std::size_t j = 0; j <= i; ++j) {
                val += cRow[j] * zNoise[j];
            }
            fSample[i] = val;
            sMean += val;
        }
        sMean /= static_cast<double>(nMax);
        double maxV = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < nMax; ++i) {
            maxV = std::max(maxV, fSample[i] - sMean);
        }
        directMaxima[s] = maxV;
    }
    std::vector<double>().swap(chol);

    std::vector<double> maxSamples;
    if (!cfg.usePaperGumbel) {
        maxSamples = std::move(directMaxima);
    } else {
        std::vector<double> sortedMax = directMaxima;
        std::sort(sortedMax.begin(), sortedMax.end());
        const double q25 = linearQuantileSorted(sortedMax, 0.25);
        const double q50 = linearQuantileSorted(sortedMax, 0.50);
        const double q75 = linearQuantileSorted(sortedMax, 0.75);
        const std::array<double, 3> qVals = {q25, q50, q75};
        const std::array<double, 3> gCoords = {
            -std::log(-std::log(0.25)),
            -std::log(-std::log(0.50)),
            -std::log(-std::log(0.75)),
        };

        // Closed-form 2-parameter OLS: q = loc + scale * g
        const double gMean = (gCoords[0] + gCoords[1] + gCoords[2]) / 3.0;
        const double qMean = (qVals[0] + qVals[1] + qVals[2]) / 3.0;
        double num = 0.0;
        double den = 0.0;
        for (std::size_t i = 0; i < 3; ++i) {
            const double dg = gCoords[i] - gMean;
            num += dg * (qVals[i] - qMean);
            den += dg * dg;
        }
        double scale = (den > 1e-18) ? (num / den) : 0.0;
        double loc = qMean - scale * gMean;
        if (!std::isfinite(scale) || scale <= 1e-9) {
            double avg = std::accumulate(directMaxima.begin(), directMaxima.end(), 0.0) /
                         static_cast<double>(directMaxima.size());
            double var = 0.0;
            for (double v : directMaxima) {
                var += (v - avg) * (v - avg);
            }
            const double sd = std::sqrt(var / static_cast<double>(directMaxima.size()));
            scale = std::max(sd, 1e-6) * std::sqrt(6.0) / kPi;
            loc = q50 + scale * std::log(std::log(2.0));
        }

        SplitMix64 rngG(seed + 29ULL);
        maxSamples.resize(cfg.gumbelMaximumSamples);
        for (std::size_t s = 0; s < cfg.gumbelMaximumSamples; ++s) {
            const double rank = cfg.rankEpsilon + rngG.uniform01() * (1.0 - 2.0 * cfg.rankEpsilon);
            maxSamples[s] = loc - scale * std::log(-std::log(rank));
        }
    }

    // Weighted histogram bins over maxSamples.
    const auto [minIt, maxIt] = std::minmax_element(maxSamples.begin(), maxSamples.end());
    const double loM = *minIt;
    const double hiM = *maxIt;
    std::vector<double> repVals;
    std::vector<double> repWeights;
    if (hiM - loM <= 1e-12 || cfg.maximumValueBins <= 1) {
        repVals.push_back(loM);
        repWeights.push_back(1.0);
    } else {
        const std::size_t nBins = cfg.maximumValueBins;
        std::vector<double> binSums(nBins, 0.0);
        std::vector<std::size_t> binCounts(nBins, 0);
        const double invWidth = static_cast<double>(nBins) / (hiM - loM);
        for (double v : maxSamples) {
            std::size_t b = static_cast<std::size_t>(std::floor((v - loM) * invWidth));
            if (b >= nBins) {
                b = nBins - 1;
            }
            binSums[b] += v;
            binCounts[b] += 1;
        }
        const double invTotal = 1.0 / static_cast<double>(maxSamples.size());
        for (std::size_t b = 0; b < nBins; ++b) {
            if (binCounts[b] > 0) {
                repVals.push_back(binSums[b] / static_cast<double>(binCounts[b]));
                repWeights.push_back(static_cast<double>(binCounts[b]) * invTotal);
            }
        }
    }

    // Evaluate each candidate's MES information gain.
    outResult.allAcquisitionValues.resize(candidateCount, 0.0);
    std::vector<std::array<double, 3>> uncondProbsList(candidateCount);
    std::uint32_t quadCount = 0;
    std::uint32_t gibbsCount = 0;

    std::size_t bestLocal = 0;
    double bestAcq = -std::numeric_limits<double>::infinity();

    for (std::size_t localI = 0; localI < candidateCount; ++localI) {
        if (onYield != nullptr && !onYield(yieldCtx)) {
            outResult.cancelled = true;
            return false;
        }
        const std::size_t cIdx = candidateIndices[localI];
        const double meanDiff = posteriorMean[cIdx] - posteriorMean[anchorIndex];
        const double diffVar = std::max(posteriorCov[cIdx * pointCount + cIdx] +
                                            posteriorCov[anchorIndex * pointCount + anchorIndex] -
                                            2.0 * posteriorCov[cIdx * pointCount + anchorIndex],
                                        0.0);
        const double totalSd = std::sqrt(diffVar + 2.0 * sigmaPref * sigmaPref);
        const double pNew = ndtr((meanDiff - gamma) / totalSd);
        const double pAnc = ndtr((-meanDiff - gamma) / totalSd);
        const double zHi = (gamma - meanDiff) / totalSd;
        const double zLo = (-gamma - meanDiff) / totalSd;
        const double pTie = std::max((zLo > 0.0) ? (ndtr(-zLo) - ndtr(-zHi)) : (ndtr(zHi) - ndtr(zLo)), 0.0);
        const std::array<double, 3> uncondProbs = {pNew, pTie, pAnc};
        uncondProbsList[localI] = uncondProbs;
        const double uncondH = categoricalEntropy(uncondProbs, cfg.entropyEpsilon);

        const double pairMeanX = centeredMean[cIdx];
        const double pairMeanA = centeredMean[anchorIndex];
        const double pairCovXX = centeredCov[cIdx * pointCount + cIdx];
        const double pairCovXA = centeredCov[cIdx * pointCount + anchorIndex];
        const double pairCovAA = centeredCov[anchorIndex * pointCount + anchorIndex];

        double condH = 0.0;
        for (std::size_t mI = 0; mI < repVals.size(); ++mI) {
            const double upperX = repVals[mI];
            const double upperA = anchorIsFeasible ? repVals[mI] : std::numeric_limits<double>::infinity();
            const std::uint64_t pairSeed =
                seed + static_cast<std::uint64_t>(cIdx) * 100003ULL + static_cast<std::uint64_t>(mI) * 997ULL;
            std::array<double, 3> condProbs{};
            bool usedGibbs = false;
            if (!conditionalJndProbsBivariateTruncated(pairMeanX, pairMeanA, pairCovXX, pairCovXA, pairCovAA, upperX,
                                                       upperA, gamma, sigmaPref, pairSeed, cfg.truncatedSamplesPerBin,
                                                       cfg.gibbsBurnIn, cfg.gibbsThinning, condProbs, usedGibbs)) {
                return false;
            }
            if (usedGibbs) {
                ++gibbsCount;
            } else {
                ++quadCount;
            }
            condH += repWeights[mI] * categoricalEntropy(condProbs, cfg.entropyEpsilon);
        }

        double rawIg = uncondH - condH;
        if (rawIg < 0.0 && rawIg >= -1e-6) {
            rawIg = 0.0;
        }
        outResult.allAcquisitionValues[localI] = rawIg;
        if (rawIg > bestAcq) {
            bestAcq = rawIg;
            bestLocal = localI;
        }
    }

    outResult.valid = true;
    outResult.candidateIndex = candidateIndices[bestLocal];
    outResult.acquisitionValue = std::max(0.0, bestAcq);
    outResult.outcomeProbabilities = uncondProbsList[bestLocal];
    outResult.quadratureEvaluations = quadCount;
    outResult.gibbsFallbackCount = gibbsCount;
    return true;
}

} // namespace EspressoRL
