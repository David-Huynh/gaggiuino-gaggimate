// Unit & golden-parity tests for portable C++17 lib/EspressoRLCore.
// Host-side, zero ESP32/Arduino/WiFi runtime — pio test -e native -f test_espressorl_core.
//
// Groups:
//   A — HydraulicWorldModel (float32 Givens QR, VFE Nyström, Matérn-3/2 KF, compliance ODE)
//   B — Level 1 SplineTrajectoryBasis, HydraulicControlEnvelope, and CEM GP-MPC closed-loop rescue
//   C — EmbeddedCpbo (Gauss-Hermite VI, Envelope-Theorem grads, Golden Fixture v1 parity, MES, Trust Region)

#include <unity.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

// Direct-include EspressoRLCore pure-STL TUs (standard PlatformIO native Unity pattern).
#include "CemGpMpc.cpp"
#include "EmbeddedCpbo.cpp"
#include "HydraulicWorldModel.cpp"

using namespace EspressoRL;

namespace {

double pearsonCorr(const std::vector<double> &a, const std::vector<double> &b) {
    const std::size_t n = std::min(a.size(), b.size());
    if (n < 2) {
        return 0.0;
    }
    double ma = 0.0, mb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= static_cast<double>(n);
    mb /= static_cast<double>(n);
    double num = 0.0, da2 = 0.0, db2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double da = a[i] - ma;
        const double db = b[i] - mb;
        num += da * db;
        da2 += da * da;
        db2 += db * db;
    }
    if (da2 <= 1e-18 || db2 <= 1e-18) {
        return 0.0;
    }
    return num / std::sqrt(da2 * db2);
}

// Embedded numeric arrays from espressorl/tests/fixtures/cpbo_embedded_golden_v1.json
constexpr std::size_t kGoldenN = 11;
constexpr std::size_t kGoldenQ = 16;
constexpr std::size_t kGoldenRawDim = 3;
constexpr std::size_t kGoldenPhysDim = 9;
constexpr std::size_t kGoldenStride = kGoldenRawDim + kGoldenPhysDim;
constexpr std::size_t kGoldenC = 9;

constexpr double kGoldenTrainInputs[kGoldenN * kGoldenStride] = {
    0.5, 0.5, 0.5454545454545454, 0.0, 0.09090909090909091, 0.07174538479980835, 0.0, 0.0, 0.0, -0.007752830297166914,
    0.007752830461385854, -0.014003560709291275,
    0.18499999999999994, 0.8250000000000002, 0.6363636363636364, 0.5473684210526318, 0.2727272727272727,
    0.07511371272468208, -0.6300000000000001, 0.6300000012692106, 0.534751277226781, -0.5883869166797839,
    0.5883869437991015, -0.5240710694185344,
    0.44500000000000006, 0.9000000000000004, 0.9121212121212121, 0.6736842105263164, 0.8242424242424243,
    0.4481872186259625, -0.10999999999999988, 0.11000000008605856, 0.6535156219997044, -0.08816936901021269,
    0.08816937138492753, -0.018314876850460484,
    0.855, 0.9000000000000004, 0.7242424242424242, 0.6736842105263164, 0.4484848484848484, 0.17360611630570877, 0.71,
    -0.7099999978095022, 0.6535156219997044, 0.693625964426808, -0.6936258964721165, 0.6603501514373051,
    0.725, 0.9000000000000004, 0.15757575757575767, 0.6736842105263164, -0.6848484848484847, -0.6545659181118301,
    0.44999999999999996, -0.4499999993308899, 0.6535156219997044, 0.4457396527054987, -0.44573963798047145,
    0.30210027808469664,
    0.725, 0.9000000000000004, 1.0, 0.6736842105263164, 1.0, 0.5766203148725325, 0.44999999999999996,
    -0.4499999993308899, 0.6535156219997044, 0.4457396527054987, -0.44573963798047145, 0.48221787293041246,
    0.8049999999999999, 0.9000000000000004, 0.4696969696969697, 0.6736842105263164, -0.06060606060606061,
    -0.19840699006366694, 0.6099999999999999, -0.6099999985253783, 0.6535156219997044, 0.5982850780360003,
    -0.5982850371129278, 0.5213332867278021,
    0.8049999999999999, 0.9749999999999996, 0.8606060606060606, 0.7999999999999994, 0.7212121212121211,
    0.33928905718874314, 0.6099999999999999, -0.6099999985253783, 0.7706417855716182, 0.6026686372039923,
    -0.6026685952478678, 0.6020385416168017,
    0.845, 1.0, 0.4272727272727273, 0.8421052631578947, -0.14545454545454536, -0.29350384690830605, 0.69,
    -0.6899999979673777, 0.8093275574011723, 0.6803891988276538, -0.6803891352462064, 0.5864062577014829,
    0.85, 1.0, 0.49696969696969695, 0.8421052631578947, -0.0060606060606061465, -0.19349512870251287, 0.7,
    -0.6999999978894001, 0.8093275574011723, 0.6899232873183317, -0.6899232206090868, 0.6108466506471968,
    0.78, 0.9749999999999996, 0.6909090909090908, 0.7999999999999994, 0.38181818181818167, 0.09467769957600294, 0.56,
    -0.5599999988187098, 0.7706417855716182, 0.5549981925904947, -0.5549981609315081, 0.5277881961722465,
};

constexpr double kGoldenQueryInputs[kGoldenQ * kGoldenStride] = {
    0.8049999999999999, 0.9749999999999996, 0.8606060606060606, 0.7999999999999994, 0.7212121212121211,
    0.33928905718874314, 0.6099999999999999, -0.6099999985253783, 0.7706417855716182, 0.6026686372039923,
    -0.6026685952478678, 0.6020385416168017,
    0.74, 0.9500000000000002, 0.5696969696969696, 0.7578947368421055, 0.1393939393939392, -0.070867765413878, 0.48,
    -0.47999999920820746, 0.7317789615380779, 0.4772710011323203, -0.4772709823535428, 0.4327752888145058,
    0.1050000000000001, 0.07500000000000018, 0.11818181818181814, -0.7157894736842102, -0.7636363636363637,
    -0.46536487299110546, -0.7899999999999998, 0.7900000018076342, -0.7543113375446994, -0.7891767632735913,
    0.7891768027614587, -0.8343828212783949,
    0.35, 0.5999999999999996, 0.9878787878787879, 0.16842105263157836, 0.9757575757575758, 0.7141713794124701,
    -0.30000000000000004, 0.30000000039145547, 0.16815425186079067, -0.28748219306249423, 0.28748220364461585,
    -0.18741458966114785,
    1.0, 0.47499999999999964, 0.4484848484848484, -0.04210526315789533, -0.10303030303030321, -0.07126467870399691, 1.0,
    -0.9999999945112588, -0.042564060195240956, 0.944063050165746, -0.9440628443967162, 0.8314039594843443,
    0.835, 0.6500000000000004, 0.24848484848484856, 0.25263157894736904, -0.5030303030303028, -0.4460287514577069,
    0.6699999999999999, -0.6699999981176823, 0.250998930722092, 0.6404250365989667, -0.6404249849547775,
    0.5051671930924194,
    0.44000000000000006, 0.27500000000000036, 0.6909090909090908, -0.37894736842105203, 0.38181818181818167,
    0.4135689458878993, -0.11999999999999988, 0.12000000009748024, -0.3909826668761837, -0.1367948105775775,
    0.13679481464388654, -0.1021819292557319,
    0.19499999999999995, 0.7999999999999998, 0.32727272727272716, 0.5052631578947365, -0.3454545454545456,
    -0.3749711620669219, -0.6100000000000001, 0.6100000012063008, 0.4947911459320254, -0.5708142829714319,
    0.5708143090444829, -0.5772046768284491,
    0.58, 0.17499999999999982, 0.8606060606060606, -0.5473684210526318, 0.7212121212121211, 0.7533904845912122,
    0.15999999999999992, -0.15999999997577538, -0.5707039199468525, 0.12343346392126439, -0.12343346528749405,
    0.16563079307098175,
    0.52, 0.5, 0.2666666666666666, 0.0, -0.46666666666666684, -0.368292975305682, 0.040000000000000036,
    -0.040000000011798265, 0.0, 0.030383529013925884, -0.03038352959337011, -0.04546580657503348,
    0.135, 0.375, 0.7969696969696969, -0.21052631578947367, 0.5939393939393938, 0.5341045312874612, -0.73,
    0.7300000015988338, -0.2149864761212743, -0.7117874722607875, 0.7117875069040606, -0.6061331714768696,
    0.38, 0.9249999999999998, 0.16060606060606053, 0.7157894736842102, -0.6787878787878789, -0.656729259634496, -0.24,
    0.24000000027603727, 0.6927374572257371, -0.21064462307928025, 0.21064463012812631, -0.29472810659088683,
    0.775, 0.04999999999999982, 0.6545454545454545, -0.7578947368421055, 0.3090909090909092, 0.4738349040075248, 0.55,
    -0.5499999988726094, -0.8008402880956375, 0.48664987760087836, -0.48664985749517164, 0.4584889748132205,
    0.9349999999999999, 0.8250000000000002, 0.9151515151515153, 0.5473684210526318, 0.8303030303030304,
    0.48829527150917995, 0.8699999999999999, -0.8699999962447689, 0.534751277226781, 0.8417265021621049,
    -0.8417263675917804, 0.8289860838849844,
    0.2849999999999999, 0.20000000000000018, 0.3939393939393939, -0.5052631578947365, -0.21212121212121213,
    -0.04528609395165184, -0.43000000000000016, 0.43000000069273486, -0.5254157473733351, -0.4373828860050919,
    0.4373829044069355, -0.43932593411633847,
    0.04499999999999993, 0.75, 0.5242424242424242, 0.42105263157894735, 0.04848484848484831, -0.06263485974586407,
    -0.9100000000000001, 0.9100000022468118, 0.4143022757220028, -0.8598493565694393, 0.8598494005450174,
    -0.7944887647986544,
};

constexpr ComparisonPair kGoldenComparisons[kGoldenC] = {
    {1, 0, PreferenceLabel::ANCHOR_BETTER}, {2, 0, PreferenceLabel::NEW_BETTER},
    {3, 2, PreferenceLabel::NEW_BETTER},    {4, 3, PreferenceLabel::ANCHOR_BETTER},
    {5, 3, PreferenceLabel::ANCHOR_BETTER}, {6, 3, PreferenceLabel::NEW_BETTER},
    {7, 6, PreferenceLabel::NEW_BETTER},    {8, 7, PreferenceLabel::ANCHOR_BETTER},
    {9, 7, PreferenceLabel::ANCHOR_BETTER},
};

constexpr double kGoldenElbo = -5.77919201746338;
constexpr double kGoldenPosteriorMean[kGoldenQ] = {
    0.353608425120597,   0.19560476034472454, -0.4366672716596069, -0.024787457793593367,
    0.33049965547002186, 0.14445562848580065, -0.037229700625672615, -0.34202432810673344,
    0.15309393306160707, -0.10834214208862211, -0.25779926573557865, -0.2536141555405144,
    0.2544086161137465,  0.4583481651165542,  -0.2622840712095481, -0.382022977720638,
};
constexpr double kGoldenPosteriorVar[kGoldenQ] = {
    0.9456102963272661, 0.959268762230045,  0.9210792708888194, 0.9478121021327738,
    0.901576739798615,  0.9243478904101962, 0.9715078806700782, 0.9389735632358609,
    0.9691943821368697, 0.9619139139129899, 0.9214268279740219, 0.952485484902876,
    0.9538656315344299, 0.917147629485804,  0.9535681547724429, 0.9115016227438537,
};
constexpr double kGoldenMesAcq[kGoldenQ - 1] = {
    -0.029845142975076788, -0.09629784302080568,  -0.08259967362224974,  0.014342540549339922,
    -0.026797714038835796, -0.09381264065672112,  -0.10385211931192545,  -0.04718464500657171,
    -0.09720734882720072,  -0.10009970376451488,  -0.102280902619793,    -0.013655347816636576,
    -0.026160166628121373, -0.10528894384872084,  -0.09818451076862572,
};

struct CancelAfterCalls {
    int maxCalls = 0;
    int count = 0;
};

bool cancelCallback(void *ctx) {
    auto *st = static_cast<CancelAfterCalls *>(ctx);
    st->count += 1;
    return st->count <= st->maxCalls;
}

} // namespace

// ---------------------------------------------------------------------------
// Group A — HydraulicWorldModel Tests
// ---------------------------------------------------------------------------

static void test_givens_qr_stability_and_forgetting() {
    SquareRootBayesianLinearF32 qr(4, 0.95f);
    const std::array<float, 4> trueW{0.4f, -0.25f, 0.15f, -0.1f};
    std::uint32_t rng = 42u;
    auto nextF = [&]() -> float {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>((rng >> 8) & 0xffffu) / 32768.0f - 1.0f;
    };

    for (int shot = 0; shot < 6; ++shot) {
        if (shot > 0) {
            qr.applyShotForgetting(0.95f);
        }
        for (int i = 0; i < 100; ++i) {
            std::array<float, 4> phi{nextF(), nextF(), nextF(), nextF()};
            float y = 0.0f;
            for (std::size_t d = 0; d < 4; ++d) {
                y += phi[d] * trueW[d];
            }
            qr.updateRow(phi.data(), y, 4.0f);
        }
        qr.solveWeights();
    }

    const std::array<float, 4> probe{0.5f, -0.5f, 0.25f, 0.75f};
    float mean = 0.0f, var = 0.0f;
    qr.predict(probe.data(), mean, var);
    const float expected = 0.5f * 0.4f + (-0.5f) * (-0.25f) + 0.25f * 0.15f + 0.75f * (-0.1f);
    TEST_ASSERT_TRUE(std::isfinite(mean));
    TEST_ASSERT_TRUE(std::isfinite(var) && var > 0.0f && var < 0.05f);
    TEST_ASSERT_FLOAT_WITHIN(0.03f, expected, mean);
}

static void test_vfe_nystrom_ood_variance_and_duplicate_anchors() {
    ResidualGPConfig cfg;
    cfg.basisSize = 32;
    FixedBasisVfeNystromGP gp(cfg);

    // Feed heavily duplicated points near V=20, P=6 with positive residual +0.35.
    std::vector<FeatureVec4> zRows;
    std::vector<float> yVals;
    for (int i = 0; i < 80; ++i) {
        zRows.push_back(buildHydraulicFeature(20.0f + 0.05f * static_cast<float>(i % 4), 6.0f, 0.0f, 18.0f));
        yVals.push_back(0.35f);
    }
    gp.refreshFromHistory(zRows.data(), yVals.data(), zRows.size(), 0.25f);

    TEST_ASSERT_EQUAL_UINT32(0u, gp.choleskyFallbackCount());
    // Pivoted Cholesky prunes collinear scaffold/duplicate points for high conditioning.
    TEST_ASSERT_TRUE(gp.basisSize() >= 4 && gp.basisSize() <= 32);

    float inMean = 0.0f, inVar = 0.0f;
    gp.predict(buildHydraulicFeature(20.0f, 6.0f, 0.0f, 18.0f), inMean, inVar);
    TEST_ASSERT_TRUE(std::isfinite(inMean) && std::isfinite(inVar));
    TEST_ASSERT_FLOAT_WITHIN(0.10f, 0.35f, inMean);
    TEST_ASSERT_TRUE(inVar < 0.08f);

    // Far out-of-distribution grind/dose must restore near-prior variance (VFE guarantee).
    float oodMean = 0.0f, oodVar = 0.0f;
    gp.predict(buildHydraulicFeature(30.0f, 6.0f, 25.0f, 24.0f), oodMean, oodVar);
    TEST_ASSERT_TRUE(std::isfinite(oodMean) && std::isfinite(oodVar));
    TEST_ASSERT_TRUE(oodVar > 0.85f * cfg.signalVariance);
    TEST_ASSERT_TRUE(oodVar > inVar);
}

static void test_matern32_kf_psd_and_channeling_ramp() {
    Matern32FilterConfig cfg;
    Matern32StateSpaceFilter kf(cfg);

    float peakRate = 0.0f;
    for (int step = 0; step < 20; ++step) {
        kf.predict(0.25f);
        const float y = 0.04f * static_cast<float>(step);
        kf.update(y, 1e-4f); // tiny noise stress-tests Joseph PSD preservation
        TEST_ASSERT_TRUE(std::isfinite(kf.offset()) && std::isfinite(kf.rate()));
        TEST_ASSERT_TRUE(kf.cov00() > 0.0f && kf.cov11() > 0.0f);
        TEST_ASSERT_TRUE(kf.cov00() * kf.cov11() >= kf.cov01() * kf.cov01());
        peakRate = std::max(peakRate, kf.rate());
    }
    TEST_ASSERT_TRUE(kf.offset() > 0.50f);
    TEST_ASSERT_TRUE(peakRate > 0.05f);
    TEST_ASSERT_TRUE(kf.rate() > 0.0f);

    // NaN observation must be ignored cleanly.
    const float prevOffset = kf.offset();
    kf.update(std::numeric_limits<float>::quiet_NaN());
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, prevOffset, kf.offset());
}

static void test_unified_ode_pressure_and_flow_modes() {
    CombinedHydraulicPredictor pred;
    HydraulicState st;
    st.elapsedS = 8.0f;
    st.pressureBar = 6.0f;
    st.pumpFlowMlS = 2.0f;
    st.beverageFlowGS = 1.6f;
    st.waterPumpedMl = 18.0f;
    st.weightG = 4.0f;

    std::array<PumpMode, 16> pModes{};
    pModes.fill(PumpMode::PRESSURE);
    std::array<float, 16> pTargets{};
    std::array<float, 16> qTargets{};
    for (std::size_t i = 0; i < 16; ++i) {
        pTargets[i] = 8.0f - 0.12f * static_cast<float>(i);
        qTargets[i] = 1.8f;
    }

    RolloutOutput pOut{};
    pred.rollout(st, pModes.data(), pTargets.data(), qTargets.data(), 16, 0.0f, 18.0f, true, pOut);
    TEST_ASSERT_EQUAL_UINT32(16u, static_cast<std::uint32_t>(pOut.steps));
    for (std::size_t i = 0; i < 16; ++i) {
        TEST_ASSERT_TRUE(std::isfinite(pOut.pressureBar[i]) && pOut.pressureBar[i] > 0.0f);
        TEST_ASSERT_TRUE(std::isfinite(pOut.beverageFlowGS[i]) && pOut.beverageFlowGS[i] > 0.0f);
        TEST_ASSERT_TRUE(pOut.pressureStdBar[i] > 0.0f && pOut.beverageFlowStdGS[i] > 0.0f);
        if (i > 0) {
            TEST_ASSERT_TRUE(pOut.weightG[i] >= pOut.weightG[i - 1]);
        }
    }

    std::array<PumpMode, 16> qModes{};
    qModes.fill(PumpMode::FLOW);
    RolloutOutput qOut{};
    pred.rollout(st, qModes.data(), pTargets.data(), qTargets.data(), 16, 0.0f, 18.0f, true, qOut);
    TEST_ASSERT_EQUAL_UINT32(16u, static_cast<std::uint32_t>(qOut.steps));
    TEST_ASSERT_TRUE(qOut.weightG[15] > st.weightG + 2.0f);
    TEST_ASSERT_TRUE(qOut.pressureStdBar[15] > 0.0f);
}

static void test_unified_ode_secondary_flow_and_pressure_caps() {
    CombinedHydraulicPredictor pred;
    HydraulicState st;
    st.elapsedS = 10.0f;
    st.pressureBar = 4.0f;
    st.pumpFlowMlS = 2.0f;
    st.beverageFlowGS = 1.6f;
    st.waterPumpedMl = 20.0f;
    st.weightG = 5.0f;

    // 1. Pressure mode + secondary flow cap (e.g., Dark & Steady Phase 4: pressure=9, flow=3 cap)
    // vs uncapped (flow=0, e.g., Extractamundo Dos!). Use coarse puck (relGrind = -20 -> ell = +0.25).
    std::array<PumpMode, 16> pModes{};
    pModes.fill(PumpMode::PRESSURE);
    std::array<float, 16> pTargets{}, qUncapped{}, qCapped{};
    pTargets.fill(9.0f);
    qUncapped.fill(0.0f);
    qCapped.fill(3.0f);

    RolloutOutput pFree{}, pLim{};
    pred.rollout(st, pModes.data(), pTargets.data(), qUncapped.data(), 16, -20.0f, 18.0f, false, pFree);
    pred.rollout(st, pModes.data(), pTargets.data(), qCapped.data(), 16, -20.0f, 18.0f, false, pLim);
    TEST_ASSERT_TRUE(pFree.beverageFlowGS[15] > 3.2f);
    TEST_ASSERT_TRUE(pLim.beverageFlowGS[15] < pFree.beverageFlowGS[15] - 0.8f);
    TEST_ASSERT_TRUE(pLim.pressureBar[15] < pFree.pressureBar[15] - 2.0f);

    // 2. Flow mode + secondary pressure cap (e.g., Dark & Steady "Fill": flow=4, pressure=4 cap)
    // vs uncapped (pressure=0, e.g., Sir Lancelot's Lever "Bloom"). Use fine puck (relGrind = +15 -> ell = -0.975).
    std::array<PumpMode, 16> qModes{};
    qModes.fill(PumpMode::FLOW);
    std::array<float, 16> qTargets{}, pUncapped{}, pCapped{};
    qTargets.fill(4.0f);
    pUncapped.fill(0.0f);
    pCapped.fill(4.0f);

    RolloutOutput qFree{}, qLim{};
    pred.rollout(st, qModes.data(), pUncapped.data(), qTargets.data(), 16, 15.0f, 18.0f, false, qFree);
    pred.rollout(st, qModes.data(), pCapped.data(), qTargets.data(), 16, 15.0f, 18.0f, false, qLim);
    TEST_ASSERT_TRUE(qFree.pressureBar[15] > 8.0f);
    TEST_ASSERT_TRUE(qLim.pressureBar[15] <= 4.3f);
    TEST_ASSERT_TRUE(qLim.pressureBar[15] < qFree.pressureBar[15] - 3.5f);
}

// ---------------------------------------------------------------------------
// Group B — Level 1 Spline Basis, Safety Envelope & CEM GP-MPC
// ---------------------------------------------------------------------------

static void test_spline_basis_partition_of_unity_and_clamping() {
    for (std::size_t k : {std::size_t{3}, std::size_t{4}}) {
        SplineTrajectoryBasis basis{k, 1.0f};
        for (float s : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            const auto w = basis.basisWeights(s);
            float sum = 0.0f;
            for (std::size_t i = 0; i < k; ++i) {
                TEST_ASSERT_TRUE(w[i] >= 0.0f);
                sum += w[i];
            }
            TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, sum);
        }
    }

    SplineTrajectoryBasis b3{3, 0.8f};
    const float validDelta[3] = {0.5f, 0.0f, -0.5f};
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, b3.offsetAt(0.0f, validDelta, 3));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, b3.offsetAt(0.5f, validDelta, 3));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -0.5f, b3.offsetAt(1.0f, validDelta, 3));

    const float badDelta[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f};
    TEST_ASSERT_FALSE(b3.validDelta(badDelta, 3));
    TEST_ASSERT_FALSE(b3.validDelta(validDelta, 2));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, b3.offsetAt(0.5f, badDelta, 3));
}

static void test_control_envelope_corridor_slew_and_invalid_inputs() {
    HydraulicControlEnvelope env;
    // Corridor around base=8.0 is [5.5, 10.5]; slew from prev=8.0 at dt=0.25s is +/-0.75 -> [7.25, 8.75].
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 8.75f, env.clampPressureTarget(12.0f, 8.0f, 8.0f, 0.25f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 7.25f, env.clampPressureTarget(1.0f, 8.0f, 8.0f, 0.25f));
    // Corridor around base=2.0 flow is [0.8, 3.2]; slew from prev=2.0 at dt=0.25s is +/-1.0 -> [1.0, 3.0].
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 3.0f, env.clampFlowTarget(10.0f, 2.0f, 2.0f, 0.25f));
    // NaN proposed target safely falls back to clamped base target.
    TEST_ASSERT_FLOAT_WITHIN(
        1e-5f, 8.0f, env.clampPressureTarget(std::numeric_limits<float>::quiet_NaN(), 8.0f, 8.0f, 0.25f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f, env.clampFlowTarget(2.5f, 2.0f, 2.0f, 0.0f));
}

static void test_mpc_activation_guard_and_zero_trust_fallbacks() {
    CombinedHydraulicPredictor pred;
    HydraulicControlEnvelope env;
    SplineTrajectoryBasis basis{3, 1.0f};
    CemGpMpcController mpc(pred, env, basis);

    std::array<PumpMode, 16> modes{};
    modes.fill(PumpMode::PRESSURE);
    std::array<float, 16> baseP{}, baseQ{}, refQ{};
    baseP.fill(7.0f);
    baseQ.fill(2.0f);
    refQ.fill(1.8f);
    const float zeroDelta[3] = {0.0f, 0.0f, 0.0f};

    // Pre-infusion / low weight -> must wait and hold base profile.
    HydraulicState st;
    st.pressureBar = 1.0f;
    st.weightG = 0.1f;
    auto cmdWait = mpc.computeCommand(st, modes.data(), baseP.data(), baseQ.data(), refQ.data(), 16, zeroDelta, 3, 0.0f,
                                      18.0f, 36.0f, false);
    TEST_ASSERT_TRUE(cmdWait.fallbackUsed);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 7.0f, cmdWait.pressureTargetBar);

    // NaN state -> zero-trust fallback.
    st.pressureBar = std::numeric_limits<float>::quiet_NaN();
    st.weightG = 5.0f;
    auto cmdNan = mpc.computeCommand(st, modes.data(), baseP.data(), baseQ.data(), refQ.data(), 16, zeroDelta, 3, 0.0f,
                                     18.0f, 36.0f, true);
    TEST_ASSERT_TRUE(cmdNan.fallbackUsed);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 7.0f, cmdNan.pressureTargetBar);

    // Unsupported SIMPLE mode -> safe fallback.
    st.pressureBar = 6.0f;
    modes.fill(PumpMode::SIMPLE);
    auto cmdSimple = mpc.computeCommand(st, modes.data(), baseP.data(), baseQ.data(), refQ.data(), 16, zeroDelta, 3,
                                        0.0f, 18.0f, 36.0f, true);
    TEST_ASSERT_TRUE(cmdSimple.fallbackUsed);
}

static void test_mpc_closed_loop_channeling_rescue_pressure_and_flow() {
    auto runClosedLoop = [](PumpMode mode, const float *deltaTheta, double &outBaseRmse, double &outMpcRmse,
                            int &outViolations) {
        HydraulicPhysicsConfig phys;
        CombinedHydraulicPredictor pred(phys);
        HydraulicControlEnvelope env;
        SplineTrajectoryBasis basis{3, 0.8f};
        CemGpMpcConfig mpcCfg;
        mpcCfg.candidateCount = 24;
        mpcCfg.cemIterations = 2;
        CemGpMpcController mpc(pred, env, basis, mpcCfg);

        const std::size_t totalSteps = 80;
        const std::size_t horizon = mpcCfg.horizonSteps;
        std::vector<PumpMode> modes(totalSteps + horizon, mode);
        std::vector<float> baseP(totalSteps + horizon, 0.0f);
        std::vector<float> baseQ(totalSteps + horizon, 0.0f);
        std::vector<float> refQ(totalSteps + horizon, 1.8f);
        for (std::size_t t = 0; t < totalSteps + horizon; ++t) {
            baseP[t] = std::max(4.5f, 8.5f - 0.045f * static_cast<float>(t));
            baseQ[t] = 2.65f;
        }

        auto simShot = [&](bool useMpc, double &rmseOut, int &violOut) {
            pred.resetShot();
            mpc.resetShot();
            float p = 5.5f, qIn = 2.0f, vIn = 12.0f, w = 2.0f;
            float prevCmd = (mode == PumpMode::PRESSURE) ? baseP[0] : baseQ[0];
            double sqErr = 0.0;
            int nEval = 0;
            violOut = 0;

            for (std::size_t t = 0; t < totalSteps; ++t) {
                const float dt = phys.dtS;
                // Sudden mid-shot channeling surge after tick 20 (+0.55 log conductance).
                const float channelSurge =
                    (t >= 20) ? (0.55f * std::min(1.0f, static_cast<float>(t - 20) / 8.0f)) : 0.0f;
                const float trueEll = physicsPriorLogConductance(0.0f, 18.0f, phys) + channelSurge;
                const float kBev = std::exp(trueEll);
                const float qBev = kBev * std::sqrt(std::max(p, 0.05f));

                pred.observeTick(p, qBev, vIn, 0.0f, 18.0f, true);

                HydraulicState st;
                st.elapsedS = static_cast<float>(t) * dt;
                st.pressureBar = p;
                st.pumpFlowMlS = qIn;
                st.beverageFlowGS = qBev;
                st.waterPumpedMl = vIn;
                st.weightG = w;
                st.pumpMode = mode;
                st.liveOffset = pred.liveFilter().offset();
                st.liveRate = pred.liveFilter().rate();
                st.liveCov00 = pred.liveFilter().cov00();
                st.liveCov01 = pred.liveFilter().cov01();
                st.liveCov11 = pred.liveFilter().cov11();

                float pCmd = baseP[t];
                float qCmd = baseQ[t];
                if (useMpc) {
                    auto cmd = mpc.computeCommand(st, modes.data() + t, baseP.data() + t, baseQ.data() + t,
                                                  refQ.data() + t, horizon, deltaTheta, 3, 0.0f, 18.0f, 36.0f, true);
                    pCmd = cmd.pressureTargetBar;
                    qCmd = cmd.flowTargetMlS;
                    const float activeVal = (mode == PumpMode::PRESSURE) ? pCmd : qCmd;
                    const float baseVal = (mode == PumpMode::PRESSURE) ? baseP[t] : baseQ[t];
                    const float maxDelta =
                        (mode == PumpMode::PRESSURE) ? env.maxBasePressureDeltaBar : env.maxBaseFlowDeltaMlS;
                    const float maxSlew =
                        ((mode == PumpMode::PRESSURE) ? env.maxPressureSlewBarS : env.maxFlowSlewMlS2) * dt;
                    if (std::fabs(activeVal - baseVal) > maxDelta + 1e-4f ||
                        (t > 0 && std::fabs(activeVal - prevCmd) > maxSlew + 1e-4f)) {
                        ++violOut;
                    }
                    prevCmd = activeVal;
                }

                // Target beverage flow for RMSE evaluation
                const float progress = std::clamp(w / 36.0f, 0.0f, 1.0f);
                const float desiredQ = basis.warpTarget(refQ[t], progress, deltaTheta, 3, 0.2f, env.maxPredictedFlowGS);
                if (t >= 20) {
                    const double err = static_cast<double>(qBev - desiredQ);
                    sqErr += err * err;
                    ++nEval;
                }

                // Advance plant 1 tick using unified compliance ODE.
                const float dtSub = dt / static_cast<float>(phys.substeps);
                for (int sub = 0; sub < phys.substeps; ++sub) {
                    const float cEff = effectiveCompliance(vIn, phys);
                    const float qB = kBev * std::sqrt(std::max(p, 0.05f));
                    const float qPuckEq = qB / pred.bevToPumpRatio();
                    const float qMax = std::max(0.5f, phys.pumpQ0MlS - phys.pumpSlopeMlSPerBar * p);
                    if (mode == PumpMode::FLOW) {
                        qIn += (phys.flowTrackAlpha / static_cast<float>(phys.substeps)) * (qCmd - qIn);
                        qIn = std::clamp(qIn, 0.0f, qMax);
                    } else {
                        qIn = std::clamp(qPuckEq + cEff * (pCmd - p) / phys.pressureTrackTauS, 0.0f, qMax);
                    }
                    p = std::clamp(p + ((qIn - qPuckEq) / cEff) * dtSub, 0.05f, 15.0f);
                    vIn += qIn * dtSub;
                    w += kBev * std::sqrt(std::max(p, 0.05f)) * dtSub;
                }
            }
            rmseOut = std::sqrt(sqErr / static_cast<double>(std::max(nEval, 1)));
        };

        int dummyViol = 0;
        simShot(false, outBaseRmse, dummyViol);
        simShot(true, outMpcRmse, outViolations);
    };

    // 1. Pressure-lever Assist-Only (delta_theta = 0)
    const float zeroDelta[3] = {0.0f, 0.0f, 0.0f};
    double baseRmse = 0.0, mpcRmse = 0.0;
    int viol = 0;
    runClosedLoop(PumpMode::PRESSURE, zeroDelta, baseRmse, mpcRmse, viol);
    TEST_ASSERT_EQUAL_INT(0, viol);
    TEST_ASSERT_TRUE(mpcRmse < baseRmse * 0.75);

    // 2. Pressure-lever 3-knot Level 1 spline
    const float splineDelta[3] = {0.15f, -0.10f, -0.20f};
    runClosedLoop(PumpMode::PRESSURE, splineDelta, baseRmse, mpcRmse, viol);
    TEST_ASSERT_EQUAL_INT(0, viol);
    TEST_ASSERT_TRUE(mpcRmse < baseRmse * 0.80);

    // 3. Flow-lever Assist-Only
    runClosedLoop(PumpMode::FLOW, zeroDelta, baseRmse, mpcRmse, viol);
    TEST_ASSERT_EQUAL_INT(0, viol);
    TEST_ASSERT_TRUE(mpcRmse < baseRmse * 0.85);
}

static void test_nominal_reference_tracks_cremina_and_turbo_without_fighting() {
    CombinedHydraulicPredictor pred;
    HydraulicControlEnvelope env;
    SplineTrajectoryBasis basis{3, 1.0f};
    CemGpMpcConfig cfg;
    cfg.candidateCount = 24;
    cfg.cemIterations = 2;
    CemGpMpcController mpc(pred, env, basis, cfg);

    const float zeroDelta[3] = {0.0f, 0.0f, 0.0f};
    std::array<PumpMode, 16> modes{};
    modes.fill(PumpMode::PRESSURE);
    std::array<float, 16> baseP{}, baseQ{}, nomRef{};
    baseQ.fill(0.0f); // Uncapped pressure profile

    // 1. Cremina lever machine ramp-down (9 -> 3 bar over 50s -> ~0.48 bar drop over 4s), fine puck (-0.60 offset).
    for (std::size_t i = 0; i < 16; ++i) {
        baseP[i] = 7.5f - 0.03f * static_cast<float>(i);
    }
    HydraulicState stCremina;
    stCremina.elapsedS = 22.0f;
    stCremina.pressureBar = 7.5f;
    stCremina.pumpFlowMlS = 1.2f;
    stCremina.beverageFlowGS = 0.95f;
    stCremina.waterPumpedMl = 30.0f;
    stCremina.weightG = 14.0f;
    stCremina.pumpMode = PumpMode::PRESSURE;
    stCremina.liveOffset = -0.60f; // Healthy fine Cremina puck matching baseline
    stCremina.liveRate = 0.0f;

    pred.predictNominalReferenceFlow(stCremina, modes.data(), baseP.data(), baseQ.data(), 16, 0.0f, 18.0f, -0.60f,
                                     nomRef.data());
    TEST_ASSERT_TRUE(nomRef[0] > 0.55f && nomRef[0] < 1.25f);
    // When Cremina pressure declines from 8.5 -> 3.5 bar over 4s, nominal reference declines smoothly with it.
    std::array<float, 16> steepDeclineP{}, steepRef{};
    for (std::size_t i = 0; i < 16; ++i) {
        steepDeclineP[i] = 8.5f - 0.30f * static_cast<float>(i);
    }
    pred.predictNominalReferenceFlow(stCremina, modes.data(), steepDeclineP.data(), baseQ.data(), 16, 0.0f, 18.0f,
                                     0.0f, steepRef.data());
    TEST_ASSERT_TRUE(steepRef[15] < steepRef[0] - 0.25f);

    mpc.resetShot();
    auto cmdHealthy = mpc.computeCommand(stCremina, modes.data(), baseP.data(), baseQ.data(), nomRef.data(), 16,
                                         zeroDelta, 3, 0.0f, 18.0f, 36.0f, true);
    TEST_ASSERT_FALSE(cmdHealthy.fallbackUsed);
    TEST_ASSERT_FLOAT_WITHIN(0.20f, baseP[0], cmdHealthy.pressureTargetBar); // Zero fighting on healthy Cremina!

    // 2. Sudden mid-shot channeling surge on Cremina (+0.65 jump above baseline): MPC rescues by dropping pressure!
    HydraulicState stChannel = stCremina;
    stChannel.liveOffset = 0.05f;
    stChannel.liveRate = 0.12f;
    stChannel.beverageFlowGS = 1.85f;
    mpc.resetShot();
    auto cmdRescue = mpc.computeCommand(stChannel, modes.data(), baseP.data(), baseQ.data(), nomRef.data(), 16,
                                        zeroDelta, 3, 0.0f, 18.0f, 36.0f, true);
    TEST_ASSERT_FALSE(cmdRescue.fallbackUsed);
    TEST_ASSERT_TRUE(cmdRescue.pressureTargetBar < baseP[0] - 0.35f);

    // 3. Extractamundo Dos! 6 bar turbo (coarse puck, +0.45 offset): MPC does NOT throttle healthy turbo down!
    baseP.fill(6.0f);
    HydraulicState stTurbo;
    stTurbo.elapsedS = 14.0f;
    stTurbo.pressureBar = 6.0f;
    stTurbo.pumpFlowMlS = 3.1f;
    stTurbo.beverageFlowGS = 2.45f;
    stTurbo.waterPumpedMl = 35.0f;
    stTurbo.weightG = 16.0f;
    stTurbo.pumpMode = PumpMode::PRESSURE;
    stTurbo.liveOffset = 0.45f;
    stTurbo.liveRate = 0.0f;
    pred.predictNominalReferenceFlow(stTurbo, modes.data(), baseP.data(), baseQ.data(), 16, 0.0f, 18.0f, 0.45f,
                                     nomRef.data());
    TEST_ASSERT_TRUE(nomRef[0] > 2.1f);
    mpc.resetShot();
    auto cmdTurbo = mpc.computeCommand(stTurbo, modes.data(), baseP.data(), baseQ.data(), nomRef.data(), 16, zeroDelta,
                                       3, 0.0f, 18.0f, 42.0f, true);
    TEST_ASSERT_FALSE(cmdTurbo.fallbackUsed);
    TEST_ASSERT_FLOAT_WITHIN(0.30f, 6.0f, cmdTurbo.pressureTargetBar);
}

static void test_mpc_mode_switch_preserves_future_phase_secondary_caps() {
    CombinedHydraulicPredictor pred;
    HydraulicControlEnvelope env;
    SplineTrajectoryBasis basis{3, 1.0f};
    CemGpMpcConfig cfg;
    cfg.candidateCount = 24;
    cfg.cemIterations = 2;
    CemGpMpcController mpc(pred, env, basis, cfg);

    // Horizon transitions mid-window from PRESSURE (8 bar, uncapped flow=0) -> FLOW (1.8 ml/s, uncapped pressure=0).
    std::array<PumpMode, 16> modes{};
    std::array<float, 16> baseP{}, baseQ{}, nomRef{};
    for (std::size_t i = 0; i < 16; ++i) {
        if (i < 6) {
            modes[i] = PumpMode::PRESSURE;
            baseP[i] = 8.0f;
            baseQ[i] = 0.0f;
        } else {
            modes[i] = PumpMode::FLOW;
            baseP[i] = 0.0f; // Uncapped pressure in future FLOW phase
            baseQ[i] = 1.8f;
        }
    }
    HydraulicState st;
    st.elapsedS = 10.0f;
    st.pressureBar = 7.8f;
    st.pumpFlowMlS = 2.0f;
    st.beverageFlowGS = 1.6f;
    st.waterPumpedMl = 22.0f;
    st.weightG = 6.0f;
    st.pumpMode = PumpMode::PRESSURE;

    pred.predictNominalReferenceFlow(st, modes.data(), baseP.data(), baseQ.data(), 16, 0.0f, 18.0f, 0.0f,
                                     nomRef.data());
    const float zeroDelta[3] = {0.0f, 0.0f, 0.0f};
    auto cmd = mpc.computeCommand(st, modes.data(), baseP.data(), baseQ.data(), nomRef.data(), 16, zeroDelta, 3, 0.0f,
                                  18.0f, 36.0f, true);
    TEST_ASSERT_FALSE(cmd.fallbackUsed);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PumpMode::PRESSURE), static_cast<uint8_t>(cmd.pumpMode));
    TEST_ASSERT_TRUE(std::isfinite(cmd.pressureTargetBar));
    TEST_ASSERT_FLOAT_WITHIN(0.35f, 8.0f, cmd.pressureTargetBar);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, cmd.flowTargetMlS);
}

static void test_nominal_baseline_policy_prevents_inner_outer_fighting() {
    NominalBaselinePolicy policy;

    // 1. Recipe changed (e.g. CPBO requested 3 steps finer): latches early-brew conductance (~-0.45)
    //    so inner MPC does NOT fight the new grind, while still catching a later channeling surge (+0.25)!
    policy.resetShot(/*hasCrossShotHistory=*/true, /*grindConfirmedPreShot=*/true,
                     /*recipeChangedSinceLastShot=*/true, /*assistOnlyMode=*/false);
    TEST_ASSERT_TRUE(policy.useEarlyBrewLatch);
    for (int t = 0; t < 5; ++t) {
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, -0.45f, policy.updateAndResolve(true, -0.45f));
        TEST_ASSERT_FALSE(policy.latched);
    }
    // Tick 6 latches the steady baseline at -0.45.
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -0.45f, policy.updateAndResolve(true, -0.45f));
    TEST_ASSERT_TRUE(policy.latched);
    // Mid-shot channeling surge jumps liveOffset to +0.25: baseline stays anchored at -0.45 so MPC rescues!
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -0.45f, policy.updateAndResolve(true, 0.25f));

    // Verify end-to-end with CemGpMpcController: latched baseline holds base pressure when steady,
    // and drops pressure sharply when channeling hits!
    CombinedHydraulicPredictor pred;
    HydraulicControlEnvelope env;
    SplineTrajectoryBasis basis{3, 1.0f};
    CemGpMpcConfig cfg;
    cfg.candidateCount = 24;
    cfg.cemIterations = 2;
    CemGpMpcController mpc(pred, env, basis, cfg);

    std::array<PumpMode, 16> modes{};
    modes.fill(PumpMode::PRESSURE);
    std::array<float, 16> baseP{}, baseQ{}, nomRef{};
    baseP.fill(8.5f);
    baseQ.fill(0.0f);
    const float zeroDelta[3] = {0.0f, 0.0f, 0.0f};

    HydraulicState stFiner;
    stFiner.elapsedS = 12.0f;
    stFiner.pressureBar = 8.5f;
    stFiner.pumpFlowMlS = 1.4f;
    stFiner.beverageFlowGS = 1.1f;
    stFiner.waterPumpedMl = 24.0f;
    stFiner.weightG = 8.0f;
    stFiner.pumpMode = PumpMode::PRESSURE;
    stFiner.liveOffset = -0.45f; // Finer grind than previous shot
    stFiner.liveRate = 0.0f;

    const float baseOff = policy.updateAndResolve(true, stFiner.liveOffset);
    pred.predictNominalReferenceFlow(stFiner, modes.data(), baseP.data(), baseQ.data(), 16, 0.0f, 18.0f, baseOff,
                                     nomRef.data());
    auto cmdFiner = mpc.computeCommand(stFiner, modes.data(), baseP.data(), baseQ.data(), nomRef.data(), 16, zeroDelta,
                                       3, 0.0f, 18.0f, 36.0f, true);
    TEST_ASSERT_FALSE(cmdFiner.fallbackUsed);
    TEST_ASSERT_FLOAT_WITHIN(0.25f, 8.5f, cmdFiner.pressureTargetBar); // Does NOT fight CPBO's finer grind!

    HydraulicState stSurge = stFiner;
    stSurge.liveOffset = 0.25f;
    stSurge.liveRate = 0.10f;
    mpc.resetShot();
    auto cmdSurge = mpc.computeCommand(stSurge, modes.data(), baseP.data(), baseQ.data(), nomRef.data(), 16, zeroDelta,
                                       3, 0.0f, 18.0f, 36.0f, true);
    TEST_ASSERT_FALSE(cmdSurge.fallbackUsed);
    TEST_ASSERT_TRUE(cmdSurge.pressureTargetBar < 8.15f); // Still rescues channeling!

    // 2. Confirmed unchanged recipe + cross-shot VFE history + 6D Spline MPC (assistOnly == false):
    //    uses 0.0f immediately (full cross-shot nominal tracking from tick 1).
    policy.resetShot(/*hasCrossShotHistory=*/true, /*grindConfirmedPreShot=*/true,
                     /*recipeChangedSinceLastShot=*/false, /*assistOnlyMode=*/false);
    TEST_ASSERT_FALSE(policy.useEarlyBrewLatch);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, policy.updateAndResolve(true, -0.45f));

    // 3. Invalid NaN / extreme offset safely clamped.
    policy.resetShot(false, false, true, true);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, policy.updateAndResolve(true, std::numeric_limits<float>::quiet_NaN()));
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.9f, policy.updateAndResolve(true, 5.0f));
}

// ---------------------------------------------------------------------------
// Group C — EmbeddedCpbo Math, Gradients, Golden Parity & Trust Region
// ---------------------------------------------------------------------------

static void test_scalar_math_ndtr_logndtr_ndtri_tails() {
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.5, ndtr(0.0));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, std::log(0.5), logNdtr(0.0));

    // Extreme negative tail: logNdtr(-40.0) ~= -0.5*1600 - 0.5*log(2pi) - log(40) ~= -804.6085
    const double lp40 = logNdtr(-40.0);
    TEST_ASSERT_TRUE(std::isfinite(lp40));
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, -804.6085, lp40);

    // Roundtrip ndtri(ndtr(x)) == x across [-5.5, 2.5] to 1e-11 and +5.0 to 1e-9
    // (at x=+5.0, 1 - Phi(5.0) ~= 2.87e-7 loses ~22 bits of IEEE-754 mantissa in p itself).
    for (double x : {-5.5, -3.0, -1.25, 0.0, 0.75, 2.5}) {
        const double p = ndtr(x);
        TEST_ASSERT_DOUBLE_WITHIN(1e-11, x, ndtri(p));
    }
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 5.0, ndtri(ndtr(5.0)));

    // JND probabilities sum to 1 and obey exact sign symmetry.
    const auto pPos = jndProbabilities(0.35, 0.15, 0.20);
    const auto pNeg = jndProbabilities(-0.35, 0.15, 0.20);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, pPos[0] + pPos[1] + pPos[2]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, pPos[0], pNeg[2]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, pPos[1], pNeg[1]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, pPos[2], pNeg[0]);
}

static void test_phi0_and_robust_scaler() {
    PhysicsProxyConfig cfg;
    const auto f = computePhi0(0.5, 18.0, 36.0, cfg);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 18.0, f[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 36.0, f[1]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 2.0, f[2]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.5, f[3]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-8, -1.5, f[4]); // log(exp(-3 * 0.5))

    // Robust scaler IQR & MAD fallback on constant-IQR column.
    const double rows[4 * 2] = {1.0, 5.0, 2.0, 5.0, 3.0, 5.0, 4.0, 5.0};
    EmbeddedRobustScaler scaler;
    TEST_ASSERT_TRUE(scaler.fit(rows, 4, 2, 1e-6));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 2.5, scaler.median[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.5, scaler.scale[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1e-6, scaler.scale[1]);

    double out[2] = {0.0, 0.0};
    const double probe[2] = {4.0, 5.0};
    TEST_ASSERT_TRUE(scaler.transformRow(probe, out));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, out[0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, out[1]);
}

static void test_envelope_theorem_analytic_gradients_vs_fd() {
    PreferenceGPConfig cfg;
    cfg.maxInnerIters = 50;
    const std::size_t nParams = kGoldenRawDim + kGoldenPhysDim + 2 + 1;
    std::vector<double> theta(nParams, -0.3);
    theta[0] = 0.2;
    theta[1] = 0.5;
    theta[kGoldenRawDim + kGoldenPhysDim] = 0.7;
    theta[kGoldenRawDim + kGoldenPhysDim + 1] = -0.7;
    theta[nParams - 1] = -1.2;

    double f0 = 0.0;
    std::vector<double> analyticGrad;
    TEST_ASSERT_TRUE(evaluatePreferenceGpObjectiveAndGrad(kGoldenTrainInputs, kGoldenN, kGoldenRawDim, kGoldenPhysDim,
                                                          0, kGoldenComparisons, kGoldenC, cfg, theta, f0,
                                                          analyticGrad));

    const double eps = 1e-5;
    for (std::size_t k = 0; k < nParams; ++k) {
        std::vector<double> tp = theta, tm = theta;
        tp[k] += eps;
        tm[k] -= eps;
        double fp = 0.0, fm = 0.0;
        std::vector<double> gp, gm;
        TEST_ASSERT_TRUE(evaluatePreferenceGpObjectiveAndGrad(kGoldenTrainInputs, kGoldenN, kGoldenRawDim,
                                                              kGoldenPhysDim, 0, kGoldenComparisons, kGoldenC, cfg, tp,
                                                              fp, gp));
        TEST_ASSERT_TRUE(evaluatePreferenceGpObjectiveAndGrad(kGoldenTrainInputs, kGoldenN, kGoldenRawDim,
                                                              kGoldenPhysDim, 0, kGoldenComparisons, kGoldenC, cfg, tm,
                                                              fm, gm));
        const double fd = (fp - fm) / (2.0 * eps);
        TEST_ASSERT_DOUBLE_WITHIN(1e-4, fd, analyticGrad[k]);
    }

    // Also verify trace-kernel (uncertain RBF) analytic gradients when traceDim == 2.
    constexpr std::size_t tN = 5;
    constexpr std::size_t tRaw = 2, tPhys = 2, tTrace = 2;
    constexpr std::size_t tStride = tRaw + tPhys + 2 * tTrace;
    const double tData[tN * tStride] = {
        0.1, 0.2, -0.5, 0.3, 0.0, 0.2, 0.05, 0.08,
        0.8, 0.4,  0.4, -0.2, 0.6, -0.1, 0.12, 0.04,
        0.3, 0.9, -0.1, 0.7, -0.4, 0.5, 0.09, 0.11,
        0.6, 0.7,  0.2, 0.1, 0.2, 0.3, 0.03, 0.06,
        0.9, 0.1,  0.8, -0.6, 0.9, -0.5, 0.15, 0.10,
    };
    const ComparisonPair tPairs[3] = {
        {1, 0, PreferenceLabel::NEW_BETTER},
        {2, 1, PreferenceLabel::TIE},
        {4, 3, PreferenceLabel::ANCHOR_BETTER},
    };
    std::vector<double> tTheta(tRaw + tPhys + tTrace + 3 + 1, -0.15);
    tTheta[tRaw + tPhys + tTrace] = 0.4;
    tTheta[tRaw + tPhys + tTrace + 1] = 0.0;
    tTheta[tRaw + tPhys + tTrace + 2] = -0.3;
    tTheta.back() = -1.0;

    double tf0 = 0.0;
    std::vector<double> tGrad;
    TEST_ASSERT_TRUE(
        evaluatePreferenceGpObjectiveAndGrad(tData, tN, tRaw, tPhys, tTrace, tPairs, 3, cfg, tTheta, tf0, tGrad));
    for (std::size_t k = 0; k < tTheta.size(); ++k) {
        std::vector<double> tp = tTheta, tm = tTheta;
        tp[k] += eps;
        tm[k] -= eps;
        double fp = 0.0, fm = 0.0;
        std::vector<double> gp, gm;
        TEST_ASSERT_TRUE(
            evaluatePreferenceGpObjectiveAndGrad(tData, tN, tRaw, tPhys, tTrace, tPairs, 3, cfg, tp, fp, gp));
        TEST_ASSERT_TRUE(
            evaluatePreferenceGpObjectiveAndGrad(tData, tN, tRaw, tPhys, tTrace, tPairs, 3, cfg, tm, fm, gm));
        TEST_ASSERT_DOUBLE_WITHIN(1e-4, (fp - fm) / (2.0 * eps), tGrad[k]);
    }
}

static void test_cpbo_golden_fixture_v1_exact_parity() {
    PreferenceGPConfig gpCfg;
    EmbeddedPreferenceGPModel model;
    TEST_ASSERT_TRUE(fitEmbeddedPreferenceGP(kGoldenTrainInputs, kGoldenN, kGoldenRawDim, kGoldenPhysDim, 0,
                                             kGoldenComparisons, kGoldenC, gpCfg, model));
    TEST_ASSERT_TRUE(model.valid);
    TEST_ASSERT_FALSE(model.cancelled);
    TEST_ASSERT_TRUE(std::isfinite(model.elbo));
    TEST_ASSERT_TRUE(std::isfinite(model.gamma) && model.gamma >= 0.0);

    // ELBO must match SciPy L-BFGS-B golden reference tightly.
    TEST_ASSERT_DOUBLE_WITHIN(0.03, kGoldenElbo, model.elbo);

    // Predict on all 16 golden query points.
    std::vector<double> postMean, postCov;
    TEST_ASSERT_TRUE(model.predict(kGoldenQueryInputs, kGoldenQ, postMean, postCov));
    TEST_ASSERT_EQUAL_UINT32(static_cast<std::uint32_t>(kGoldenQ), static_cast<std::uint32_t>(postMean.size()));

    const std::vector<double> refMean(kGoldenPosteriorMean, kGoldenPosteriorMean + kGoldenQ);
    const double meanCorr = pearsonCorr(postMean, refMean);
    TEST_ASSERT_TRUE(meanCorr > 0.995);

    double maxMeanDiff = 0.0;
    double maxVarDiff = 0.0;
    for (std::size_t i = 0; i < kGoldenQ; ++i) {
        maxMeanDiff = std::max(maxMeanDiff, std::fabs(postMean[i] - kGoldenPosteriorMean[i]));
        const double varI = postCov[i * kGoldenQ + i];
        TEST_ASSERT_TRUE(std::isfinite(varI) && varI > 0.0);
        maxVarDiff = std::max(maxVarDiff, std::fabs(varI - kGoldenPosteriorVar[i]));
    }
    TEST_ASSERT_TRUE(maxMeanDiff < 0.05);
    TEST_ASSERT_TRUE(maxVarDiff < 0.05);

    // Evaluate MES with candidates 1..15 against anchor 0 (matching golden fixture).
    std::array<std::size_t, kGoldenQ - 1> candIdx{};
    for (std::size_t i = 0; i < kGoldenQ - 1; ++i) {
        candIdx[i] = i + 1;
    }
    MESConfig mesCfg;
    EmbeddedMESResult mes;
    TEST_ASSERT_TRUE(evaluateEmbeddedCpboMes(postMean.data(), postCov.data(), kGoldenQ, candIdx.data(), candIdx.size(),
                                             nullptr, 0, 0, model.gamma, model.sigmaPref, mesCfg, 17ULL, mes));
    TEST_ASSERT_TRUE(mes.valid);
    TEST_ASSERT_FALSE(mes.cancelled);

    // Exact same winning candidate index as Python/PyTorch golden fixture (candidate 4).
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<std::uint32_t>(mes.candidateIndex));
    TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.014342540549339922, mes.acquisitionValue);
    TEST_ASSERT_DOUBLE_WITHIN(0.03, 0.47287927408493013, mes.outcomeProbabilities[0]);
    TEST_ASSERT_DOUBLE_WITHIN(0.03, 0.0007748648508204004, mes.outcomeProbabilities[1]);
    TEST_ASSERT_DOUBLE_WITHIN(0.03, 0.5263458610642495, mes.outcomeProbabilities[2]);

    const std::vector<double> refAcq(kGoldenMesAcq, kGoldenMesAcq + (kGoldenQ - 1));
    const double acqCorr = pearsonCorr(mes.allAcquisitionValues, refAcq);
    TEST_ASSERT_TRUE(acqCorr > 0.99);
    TEST_ASSERT_TRUE(mes.quadratureEvaluations > 100u);
}

static void test_mes_quadrature_and_robert_gibbs_far_tail() {
    std::array<double, 3> probs{};
    bool usedGibbs = true;

    // Moderate correlated case -> deterministic 24x20 Gauss-Legendre quadrature.
    TEST_ASSERT_TRUE(conditionalJndProbsBivariateTruncated(0.2, -0.1, 0.9, 0.45, 0.85, 0.8, 0.8, 0.15, 0.20, 42ULL,
                                                           256, 64, 2, probs, usedGibbs));
    TEST_ASSERT_FALSE(usedGibbs);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, probs[0] + probs[1] + probs[2]);
    TEST_ASSERT_TRUE(probs[0] > probs[2]);

    // Deep lower tail (alpha = -6.0) -> Robert-Gibbs fallback.
    TEST_ASSERT_TRUE(conditionalJndProbsBivariateTruncated(0.0, 0.0, 1.0, 0.3, 1.0, -6.0, 1.0, 0.10, 0.20, 42ULL, 256,
                                                           64, 2, probs, usedGibbs));
    TEST_ASSERT_TRUE(usedGibbs);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, probs[0] + probs[1] + probs[2]);
    TEST_ASSERT_TRUE(probs[2] > 0.95); // f_x <= -6 while f_a <= 1 -> anchor strongly better

    // Near-singular correlation |rho| > 0.9995 -> Robert-Gibbs fallback.
    TEST_ASSERT_TRUE(conditionalJndProbsBivariateTruncated(0.0, 0.0, 1.0, 0.9999, 1.0, 0.5, 0.5, 0.10, 0.20, 42ULL, 256,
                                                           64, 2, probs, usedGibbs));
    TEST_ASSERT_TRUE(usedGibbs);
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, probs[0] + probs[1] + probs[2]);
}

static void test_cpbo_cooperative_yield_cancellation_and_invalid_inputs() {
    // 1. Cooperative cancellation inside fitEmbeddedPreferenceGP.
    CancelAfterCalls cancelFit{2, 0};
    PreferenceGPConfig gpCfg;
    EmbeddedPreferenceGPModel model;
    TEST_ASSERT_FALSE(fitEmbeddedPreferenceGP(kGoldenTrainInputs, kGoldenN, kGoldenRawDim, kGoldenPhysDim, 0,
                                              kGoldenComparisons, kGoldenC, gpCfg, model, cancelCallback, &cancelFit));
    TEST_ASSERT_TRUE(model.cancelled);
    TEST_ASSERT_FALSE(model.valid);

    // 2. Cooperative cancellation inside evaluateEmbeddedCpboMes.
    const double mean[3] = {0.1, 0.2, -0.1};
    const double cov[9] = {1.0, 0.2, 0.1, 0.2, 1.0, 0.2, 0.1, 0.2, 1.0};
    const std::size_t cands[2] = {1, 2};
    CancelAfterCalls cancelMes{1, 0};
    MESConfig mesCfg;
    EmbeddedMESResult mes;
    TEST_ASSERT_FALSE(evaluateEmbeddedCpboMes(mean, cov, 3, cands, 2, nullptr, 0, 0, 0.1, 0.2, mesCfg, 17ULL, mes,
                                              cancelCallback, &cancelMes));
    TEST_ASSERT_TRUE(mes.cancelled);
    TEST_ASSERT_FALSE(mes.valid);

    // 3. Zero-trust rejection of invalid inputs (out-of-range comparison index, negative sigmaPref, NaN data).
    const ComparisonPair badPair[1] = {{99, 0, PreferenceLabel::NEW_BETTER}};
    TEST_ASSERT_FALSE(fitEmbeddedPreferenceGP(kGoldenTrainInputs, kGoldenN, kGoldenRawDim, kGoldenPhysDim, 0, badPair,
                                              1, gpCfg, model));
    gpCfg.sigmaPref = -0.1;
    TEST_ASSERT_FALSE(fitEmbeddedPreferenceGP(kGoldenTrainInputs, kGoldenN, kGoldenRawDim, kGoldenPhysDim, 0,
                                              kGoldenComparisons, kGoldenC, gpCfg, model));
    gpCfg.sigmaPref = 0.20;
    std::vector<double> nanTrain(kGoldenTrainInputs, kGoldenTrainInputs + kGoldenN * kGoldenStride);
    nanTrain[3] = std::numeric_limits<double>::quiet_NaN();
    TEST_ASSERT_FALSE(fitEmbeddedPreferenceGP(nanTrain.data(), kGoldenN, kGoldenRawDim, kGoldenPhysDim, 0,
                                              kGoldenComparisons, kGoldenC, gpCfg, model));
}

static void test_level1_trust_region_6d_dynamics() {
    TrustRegionConfig cfg;
    Level1TrustRegionState st;
    st.center = {0.5, 0.5, 0.5, 0.5, 0.5, 0.5};
    st.length = 0.2;

    const std::vector<double> nextCenter = {0.55, 0.52, 0.48, 0.60, 0.50, 0.45};
    // 3 consecutive improvements -> EXPANDED (length doubles to 0.4).
    for (int i = 0; i < 3; ++i) {
        Level1TrustRegionState next;
        TEST_ASSERT_TRUE(updateLevel1TrustRegion(st, PreferenceLabel::NEW_BETTER, nextCenter, cfg, next));
        st = next;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TrustRegionAction::EXPANDED), static_cast<int>(st.lastAction));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.4, st.length);

    // 2 consecutive non-improvements -> CONTRACTED (length halves to 0.2).
    for (int i = 0; i < 2; ++i) {
        Level1TrustRegionState next;
        TEST_ASSERT_TRUE(updateLevel1TrustRegion(st, PreferenceLabel::ANCHOR_BETTER, nextCenter, cfg, next));
        st = next;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TrustRegionAction::CONTRACTED), static_cast<int>(st.lastAction));
    TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.2, st.length);

    // Anisotropic lengthscale-shaped bounds in [0, 1]^6.
    const std::vector<double> ls = {0.2, 2.0, 1.0, 0.5, 1.0, 4.0};
    std::vector<double> lo, hi;
    TEST_ASSERT_TRUE(computeLevel1TrustRegionBounds(st, ls, cfg, lo, hi));
    TEST_ASSERT_EQUAL_UINT32(6u, static_cast<std::uint32_t>(lo.size()));
    for (std::size_t d = 0; d < 6; ++d) {
        TEST_ASSERT_TRUE(lo[d] >= 0.0 && hi[d] <= 1.0 && lo[d] < hi[d]);
    }
    // Larger lengthscale -> wider trust-region dimension.
    TEST_ASSERT_TRUE((hi[5] - lo[5]) > (hi[0] - lo[0]));

    // Contraction down to minimumLength triggers CONVERGED and rejects further updates.
    st.length = cfg.minimumLength * 1.5;
    st.failureCount = 1;
    st.successCount = 0;
    Level1TrustRegionState conv;
    TEST_ASSERT_TRUE(updateLevel1TrustRegion(st, PreferenceLabel::TIE, nextCenter, cfg, conv));
    TEST_ASSERT_TRUE(conv.locallyConverged);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TrustRegionAction::CONVERGED), static_cast<int>(conv.lastAction));
    Level1TrustRegionState rejected;
    TEST_ASSERT_FALSE(updateLevel1TrustRegion(conv, PreferenceLabel::NEW_BETTER, nextCenter, cfg, rejected));
}

void setUp(void) {}
void tearDown(void) {}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    // Group A — HydraulicWorldModel
    RUN_TEST(test_givens_qr_stability_and_forgetting);
    RUN_TEST(test_vfe_nystrom_ood_variance_and_duplicate_anchors);
    RUN_TEST(test_matern32_kf_psd_and_channeling_ramp);
    RUN_TEST(test_unified_ode_pressure_and_flow_modes);
    RUN_TEST(test_unified_ode_secondary_flow_and_pressure_caps);
    // Group B — Level 1 Spline Basis, Safety Envelope & CEM GP-MPC
    RUN_TEST(test_spline_basis_partition_of_unity_and_clamping);
    RUN_TEST(test_control_envelope_corridor_slew_and_invalid_inputs);
    RUN_TEST(test_mpc_activation_guard_and_zero_trust_fallbacks);
    RUN_TEST(test_mpc_closed_loop_channeling_rescue_pressure_and_flow);
    RUN_TEST(test_nominal_reference_tracks_cremina_and_turbo_without_fighting);
    RUN_TEST(test_mpc_mode_switch_preserves_future_phase_secondary_caps);
    RUN_TEST(test_nominal_baseline_policy_prevents_inner_outer_fighting);
    // Group C — EmbeddedCpbo & Golden Fixture v1 Parity
    RUN_TEST(test_scalar_math_ndtr_logndtr_ndtri_tails);
    RUN_TEST(test_phi0_and_robust_scaler);
    RUN_TEST(test_envelope_theorem_analytic_gradients_vs_fd);
    RUN_TEST(test_cpbo_golden_fixture_v1_exact_parity);
    RUN_TEST(test_mes_quadrature_and_robert_gibbs_far_tail);
    RUN_TEST(test_cpbo_cooperative_yield_cancellation_and_invalid_inputs);
    RUN_TEST(test_level1_trust_region_6d_dynamics);
    return UNITY_END();
}
