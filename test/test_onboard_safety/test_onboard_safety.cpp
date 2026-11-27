// Native Unity safety, deadman lease, profile horizon, and AutoTuning router tests for Phase 4.
// Host-side, zero ESP32/Arduino/FreeRTOS runtime.
//
// Groups:
//   A — LiveBrewLease zero-trust bounds, corridor, slew-rate, Shadow Mode, and 500 ms deadman expiry
//   B — ProfileHorizonSampler on Damian's LM Leva (pressure decline) and Sir Lancelot's Lever (mixed pressure->flow)
//   C — AutoTuning::Router OnBoard vs OffBoard boundary isolation

#include <unity.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

#include "display/core/AutoTuning.cpp"
#include "display/core/LiveBrewLease.h"
#include "display/core/OnBoardOptimizationMode.h"
#include "display/core/OnBoardStateCodec.h"
#include "display/core/ProfileHorizonSampler.h"
#include "EmbeddedCpbo.cpp"

namespace {

class FakeTransport final : public AutoTuning::OptimizerTransportPort {
  public:
    bool cfg = true;
    bool conn = true;
    bool configured() const override { return cfg; }
    bool connected() const override { return conn; }
    AutoTuning::ShotSubmissionResult publishShot(AutoTuning::ShotRecord const &,
                                                 AutoTuning::ShotDeliveryAttempt const &) override {
        return {AutoTuning::ShotSubmissionOutcome::Submitted, "fake"};
    }
    bool publishLiveShotStarted(AutoTuning::LiveShotStarted const &) override { return true; }
    bool publishLiveShotSample(AutoTuning::LiveShotSample const &) override { return true; }
    bool publishLiveShotEnded(AutoTuning::LiveShotEnded const &) override { return true; }
};

} // namespace

// ---------------------------------------------------------------------------
// Group A — LiveBrewLease Zero-Trust Safety & Deadman Expiry
// ---------------------------------------------------------------------------

static void test_live_lease_shadow_mode_never_actuates() {
    LiveBrewLease lease;
    LiveBrewLeaseCommand cmd;
    cmd.mode = LiveLeasePumpMode::PRESSURE;
    cmd.pressureTargetBar = 6.5f;
    cmd.leaseDurationMs = 500;
    cmd.shadowOnly = true;

    TEST_ASSERT_TRUE(lease.submit(cmd, LiveLeasePumpMode::PRESSURE, 8.0f, 0.0f, 1000u, false));
    TEST_ASSERT_FALSE(lease.isActive());
    TEST_ASSERT_EQUAL_UINT32(1u, lease.diagnostics().shadowCount);
    TEST_ASSERT_EQUAL_UINT32(0u, lease.diagnostics().appliedCount);
    TEST_ASSERT_TRUE(lease.diagnostics().lastWasShadow);

    // Resolve must return untouched base profile (8.0 bar).
    LiveLeasePumpMode outMode = LiveLeasePumpMode::SIMPLE;
    float outP = 0.0f, outQ = 0.0f;
    TEST_ASSERT_FALSE(lease.resolve(1100u, LiveLeasePumpMode::PRESSURE, 8.0f, 0.0f, outMode, outP, outQ));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 8.0f, outP);

    // Even if caller sets shadowOnly = false, forceShadowMode = true overrides and keeps actuation OFF.
    cmd.shadowOnly = false;
    TEST_ASSERT_TRUE(lease.submit(cmd, LiveLeasePumpMode::PRESSURE, 8.0f, 0.0f, 1250u, /*forceShadowMode=*/true));
    TEST_ASSERT_FALSE(lease.isActive());
    TEST_ASSERT_EQUAL_UINT32(2u, lease.diagnostics().shadowCount);
}

static void test_live_lease_corridor_slew_and_deadman_expiry() {
    LiveBrewLease lease;
    LiveBrewLeaseCommand cmd;
    cmd.mode = LiveLeasePumpMode::PRESSURE;
    cmd.pressureTargetBar = 12.0f; // Extreme request above corridor & slew
    cmd.leaseDurationMs = 500;
    cmd.shadowOnly = false;

    // Base = 8.0 bar -> corridor [5.5, 10.5], first tick slew = 3.0 * 0.25 = 0.75 -> clamped to 8.75 bar!
    TEST_ASSERT_TRUE(lease.submit(cmd, LiveLeasePumpMode::PRESSURE, 8.0f, 0.0f, 1000u));
    TEST_ASSERT_TRUE(lease.isActive());

    LiveLeasePumpMode outMode = LiveLeasePumpMode::SIMPLE;
    float outP = 0.0f, outQ = 0.0f;
    TEST_ASSERT_TRUE(lease.resolve(1200u, LiveLeasePumpMode::PRESSURE, 8.0f, 0.0f, outMode, outP, outQ));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 8.75f, outP);

    // Next tick 250 ms later: slew from 8.75 allows up to 9.50 bar.
    TEST_ASSERT_TRUE(lease.submit(cmd, LiveLeasePumpMode::PRESSURE, 8.0f, 0.0f, 1250u));
    TEST_ASSERT_TRUE(lease.resolve(1400u, LiveLeasePumpMode::PRESSURE, 8.0f, 0.0f, outMode, outP, outQ));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 9.50f, outP);

    // If base profile ramps down to 6.0 bar mid-lease, read-time corridor re-clamps to 6.0 + 2.5 = 8.50 bar.
    TEST_ASSERT_TRUE(lease.resolve(1500u, LiveLeasePumpMode::PRESSURE, 6.0f, 0.0f, outMode, outP, outQ));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 8.50f, outP);

    // Deadman expiry: 1250 + 501 = 1751 ms -> must automatically revert to untouched base profile (6.0 bar)!
    TEST_ASSERT_FALSE(lease.resolve(1751u, LiveLeasePumpMode::PRESSURE, 6.0f, 0.0f, outMode, outP, outQ));
    TEST_ASSERT_FALSE(lease.isActive());
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 6.0f, outP);
    TEST_ASSERT_EQUAL_UINT32(1u, lease.diagnostics().expiredCount);
}

static void test_live_lease_uint32_wraparound_and_invalid_inputs() {
    LiveBrewLease lease;
    LiveBrewLeaseCommand cmd;
    cmd.mode = LiveLeasePumpMode::FLOW;
    cmd.flowTargetMlS = 1.6f;
    cmd.leaseDurationMs = 500;
    cmd.shadowOnly = false;

    // Issue 100 ms before 32-bit millis() wraparound.
    const uint32_t nearWrap = 0xFFFFFF9Cu;
    TEST_ASSERT_TRUE(lease.submit(cmd, LiveLeasePumpMode::FLOW, 0.0f, 2.0f, nearWrap));
    LiveLeasePumpMode outMode = LiveLeasePumpMode::SIMPLE;
    float outP = 0.0f, outQ = 0.0f;

    // 200 ms after wraparound (total elapsed = 300 ms <= 500 ms) -> still valid.
    TEST_ASSERT_TRUE(lease.resolve(nearWrap + 300u, LiveLeasePumpMode::FLOW, 0.0f, 2.0f, outMode, outP, outQ));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.6f, outQ);

    // 550 ms elapsed -> expired cleanly across wraparound!
    TEST_ASSERT_FALSE(lease.resolve(nearWrap + 550u, LiveLeasePumpMode::FLOW, 0.0f, 2.0f, outMode, outP, outQ));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 2.0f, outQ);

    // Zero-trust rejections: NaN, mode mismatch, SIMPLE mode, zero lease duration.
    cmd.flowTargetMlS = std::numeric_limits<float>::quiet_NaN();
    TEST_ASSERT_FALSE(lease.submit(cmd, LiveLeasePumpMode::FLOW, 0.0f, 2.0f, 5000u));
    cmd.flowTargetMlS = 1.8f;
    TEST_ASSERT_FALSE(lease.submit(cmd, LiveLeasePumpMode::PRESSURE, 8.0f, 0.0f, 5000u)); // mode mismatch
    TEST_ASSERT_FALSE(lease.submit(cmd, LiveLeasePumpMode::SIMPLE, 0.0f, 0.0f, 5000u));
    cmd.leaseDurationMs = 0;
    TEST_ASSERT_FALSE(lease.submit(cmd, LiveLeasePumpMode::FLOW, 0.0f, 2.0f, 5000u));
    TEST_ASSERT_EQUAL_UINT32(4u, lease.diagnostics().rejectedCount);
}

// ---------------------------------------------------------------------------
// Group B — ProfileHorizonSampler on Multi-Phase Lever Profiles
// ---------------------------------------------------------------------------

static void test_horizon_sampler_damian_pressure_decline_and_lancelot_mixed() {
    // 1. Damian's LM Leva style: 8s preinfusion (2 bar) -> 2s ramp to 8.5 bar -> 25s linear decline to 5.0 bar.
    ProfileHorizon::PhaseSpec damian[3]{};
    damian[0] = {false, false, false, ProfileHorizon::Mode::PRESSURE, 8.0f, 2.0f, 0.0f, ProfileHorizon::Easing::INSTANT, 0.0f, false};
    damian[1] = {true, false, false, ProfileHorizon::Mode::PRESSURE, 2.0f, 8.5f, 0.0f, ProfileHorizon::Easing::LINEAR, 2.0f, true};
    damian[2] = {true, true, false, ProfileHorizon::Mode::PRESSURE, 25.0f, 5.0f, 0.0f, ProfileHorizon::Easing::LINEAR, 25.0f, true};

    // Damian phase 0 (true preinfusion before brew phases) stays inactive; phases 1 & 2 activate.
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(damian, 3, 0));
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(damian, 3, 1));
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(damian, 3, 2));

    ProfileHorizon::HorizonSample hDamian{};
    // Mid-way through phase 1 (t=1.0s into 2s ramp) -> crosses into phase 2 linear decline within 16 steps (4s).
    TEST_ASSERT_TRUE(
        ProfileHorizon::sampleHorizon(damian, 3, 1, 1.0f, 2.0f, 0.0f, 5.25f, 1.5f, 1.8f, 0.80f, 0.25f, 16, hDamian));
    TEST_ASSERT_EQUAL_UINT32(16u, static_cast<uint32_t>(hDamian.steps));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProfileHorizon::Mode::PRESSURE),
                            static_cast<uint8_t>(hDamian.modes[0]));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 5.25f, hDamian.basePressureBar[0]);
    TEST_ASSERT_TRUE(hDamian.basePressureBar[4] > 7.5f); // Reached peak near phase 1 end
    TEST_ASSERT_TRUE(hDamian.basePressureBar[15] < hDamian.basePressureBar[4]); // Declining in phase 2

    // 2. Exact Sir Lancelot's Lever JSON:
    //    Phase 0 "Pump":  phase="preinfusion", duration=30, pressure=10, flow=0, target pressure>=9 (no volumetric)
    //    Phase 1 "Bloom": phase="preinfusion", duration=40, pressure=0, flow=1.8, target volumetric>=33
    ProfileHorizon::PhaseSpec lancelot[2]{};
    lancelot[0] = {false, false, false, ProfileHorizon::Mode::PRESSURE, 30.0f, 10.0f, 0.0f, ProfileHorizon::Easing::INSTANT, 0.0f, true};
    lancelot[1] = {false, true, false, ProfileHorizon::Mode::FLOW, 40.0f, 0.0f, 1.8f, ProfileHorizon::Easing::INSTANT, 0.0f, true};

    // Phase 0 ("Pump") stays untouched during pressure ramp; Phase 1 ("Bloom") activates even though tagged preinfusion!
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(lancelot, 2, 0));
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(lancelot, 2, 1));

    ProfileHorizon::HorizonSample hLancelot{};
    TEST_ASSERT_TRUE(
        ProfileHorizon::sampleHorizon(lancelot, 2, 1, 2.0f, 9.0f, 1.8f, 7.5f, 1.8f, 1.5f, 0.80f, 0.25f, 16, hLancelot));
    for (std::size_t i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProfileHorizon::Mode::FLOW),
                                static_cast<uint8_t>(hLancelot.modes[i]));
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, hLancelot.basePressureBar[i]); // Uncapped pressure preserved
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.8f, hLancelot.baseFlowMlS[i]);
        // Beverage flow reference in FLOW mode derives from eta * baseFlowMlS = 0.80 * 1.8 = 1.44 g/s.
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.44f, hLancelot.refBeverageFlowGS[i]);
    }

    // 3. Dark & Steady 9-6 v1.5 (5 phases: simple Stabilize -> Fill -> Saturate -> Ramp -> volumetric ease-in Decline 9->6 w/ 3 ml/s cap).
    ProfileHorizon::PhaseSpec darkSteady[5]{};
    darkSteady[0] = {false, false, true, ProfileHorizon::Mode::SIMPLE, 3.0f, 0.0f, 0.0f, ProfileHorizon::Easing::INSTANT, 0.0f, true};
    darkSteady[1] = {false, false, false, ProfileHorizon::Mode::FLOW, 8.0f, 4.0f, 4.0f, ProfileHorizon::Easing::INSTANT, 0.0f, true};
    darkSteady[2] = {false, true, false, ProfileHorizon::Mode::PRESSURE, 3.0f, 3.0f, 8.0f, ProfileHorizon::Easing::LINEAR, 1.0f, true};
    darkSteady[3] = {true, true, false, ProfileHorizon::Mode::PRESSURE, 10.0f, 10.0f, 8.0f, ProfileHorizon::Easing::EASE_OUT, 7.0f, true};
    darkSteady[4] = {true, true, false, ProfileHorizon::Mode::PRESSURE, 40.0f, 6.0f, 3.0f, ProfileHorizon::Easing::EASE_IN, 0.0f, true,
                     ProfileHorizon::TransTarget::VOLUMETRIC, 31.0f, 0.0f};

    // Preinfusion phases 0..2 (including Saturate with volumetric>=3) stay inactive because brew phases 3..4 exist!
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(darkSteady, 5, 0));
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(darkSteady, 5, 1));
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(darkSteady, 5, 2));
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(darkSteady, 5, 3));
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(darkSteady, 5, 4));

    // Volumetric ease-in decline in phase 4: phaseStartWeight=4g, target=31g (span=27g), current=17.5g (50% weight -> alpha=0.25 -> 8.25 bar).
    ProfileHorizon::HorizonSample hDark{};
    TEST_ASSERT_TRUE(ProfileHorizon::sampleHorizon(darkSteady, 5, 4, 8.0f, 9.0f, 3.0f, 8.3f, 1.5f, 1.5f, 0.80f, 0.25f,
                                                   16, hDark, /*currentWeightG=*/17.5f, /*phaseStartWeightG=*/4.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 8.25f, hDark.basePressureBar[0]);
    TEST_ASSERT_TRUE(hDark.basePressureBar[15] < hDark.basePressureBar[0]); // Declines over projected weight
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3.0f, hDark.baseFlowMlS[0]);            // Preserves 3 ml/s secondary flow cap

    // 4. Extractamundo Dos! (3 preinfusion phases incl. 0.1 ml/s dynamic bloom -> 6 bar brew phase).
    ProfileHorizon::PhaseSpec extractamundo[4]{};
    extractamundo[0] = {false, false, false, ProfileHorizon::Mode::PRESSURE, 2.0f, 8.0f, 0.0f, ProfileHorizon::Easing::EASE_OUT, 2.0f, true};
    extractamundo[1] = {false, false, false, ProfileHorizon::Mode::PRESSURE, 20.0f, 8.0f, 0.0f, ProfileHorizon::Easing::EASE_OUT, 2.0f, true};
    extractamundo[2] = {false, false, false, ProfileHorizon::Mode::FLOW, 40.0f, 0.0f, 0.1f, ProfileHorizon::Easing::EASE_OUT, 2.0f, true};
    extractamundo[3] = {true, true, false, ProfileHorizon::Mode::PRESSURE, 60.0f, 6.0f, 0.0f, ProfileHorizon::Easing::EASE_OUT, 2.0f, true};
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(extractamundo, 4, 0));
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(extractamundo, 4, 1));
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(extractamundo, 4, 2)); // Dynamic bloom untouched!
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(extractamundo, 4, 3));  // 6 bar brew active!
}

static void test_universal_schema_matrix_cremina_direct_lever_lh_and_standard() {
    // 1. Cremina lever machine (5 phases: 3x 1.1 bar preinfusion -> 10s linear ramp -> 50s linear ramp-down 9->3 bar).
    ProfileHorizon::PhaseSpec cremina[5]{};
    cremina[0] = {false, false, false, ProfileHorizon::Mode::PRESSURE, 2.0f, 1.1f, 0.0f, ProfileHorizon::Easing::INSTANT, 0.0f, false};
    cremina[1] = {false, false, false, ProfileHorizon::Mode::PRESSURE, 3.0f, 1.1f, 0.0f, ProfileHorizon::Easing::INSTANT, 0.0f, false};
    cremina[1].addTarget(ProfileHorizon::ExitTargetType::PRESSURE, ProfileHorizon::ExitTargetOp::GTE, 3.0f);
    cremina[2] = {false, false, false, ProfileHorizon::Mode::PRESSURE, 10.0f, 1.1f, 0.0f, ProfileHorizon::Easing::INSTANT, 0.0f, false};
    cremina[3] = {true, false, false, ProfileHorizon::Mode::PRESSURE, 10.0f, 9.0f, 0.0f, ProfileHorizon::Easing::LINEAR, 10.0f, true};
    cremina[4] = {true, true, false, ProfileHorizon::Mode::PRESSURE, 50.0f, 3.0f, 0.0f, ProfileHorizon::Easing::LINEAR, 50.0f, true};
    cremina[4].addTarget(ProfileHorizon::ExitTargetType::VOLUMETRIC, ProfileHorizon::ExitTargetOp::GTE, 36.0f);

    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(cremina, 5, 0));
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(cremina, 5, 1));
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(cremina, 5, 2));
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(cremina, 5, 3));
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(cremina, 5, 4));
    // Hydraulic gate keeps dry start of phase 3 untouched and enables once flowing.
    TEST_ASSERT_FALSE(ProfileHorizon::isHydraulicallyReadyForMpc(true, 2.5f, 0.05f, 0.2f));
    TEST_ASSERT_TRUE(ProfileHorizon::isHydraulicallyReadyForMpc(true, 4.5f, 0.65f, 2.0f));
    // Horizon crosses from end of phase 3 (t=9.5s) into phase 4 linear 9->3 bar decline.
    ProfileHorizon::HorizonSample hCremina{};
    TEST_ASSERT_TRUE(ProfileHorizon::sampleHorizon(cremina, 5, 3, 9.5f, 1.1f, 0.0f, 8.6f, 0.8f, 0.8f, 0.80f, 0.25f, 16,
                                                   hCremina, 5.0f, 0.0f, 20.0f, 10.0f));
    TEST_ASSERT_TRUE(hCremina.basePressureBar[0] > 8.4f);
    TEST_ASSERT_TRUE(hCremina.basePressureBar[15] < hCremina.basePressureBar[2]); // Declining in phase 4!

    // 2. Direct Lever [Automatic Pro] v3.5 style: multi-phase skip (phase 2 -> skip 3,4,5 -> land on phase 6 -> 7)
    //    AND low-pressure 2.5 bar flow-mode extraction (tf=2.40 -> 2.31, tp=9 soft limit).
    ProfileHorizon::PhaseSpec directLever[8]{};
    directLever[0] = {false, false, true, ProfileHorizon::Mode::SIMPLE, 2.0f, 0.0f, 0.0f, ProfileHorizon::Easing::INSTANT, 0.0f, false};
    directLever[1] = {false, false, false, ProfileHorizon::Mode::FLOW, 10.0f, 2.0f, 6.0f, ProfileHorizon::Easing::INSTANT, 0.0f, true};
    directLever[2] = {false, true, false, ProfileHorizon::Mode::PRESSURE, 8.0f, 2.0f, 6.0f, ProfileHorizon::Easing::INSTANT, 0.0f, true};
    directLever[2].addTarget(ProfileHorizon::ExitTargetType::VOLUMETRIC, ProfileHorizon::ExitTargetOp::GTE, 3.2f);
    // Phases 3, 4, 5 have already-satisfied volumetric/pressure targets -> skipped in one step!
    for (int k = 3; k <= 5; ++k) {
        directLever[k] = {false, false, false, ProfileHorizon::Mode::PRESSURE, 10.0f, 3.0f, 0.0f, ProfileHorizon::Easing::INSTANT, 0.0f, true};
        directLever[k].addTarget(ProfileHorizon::ExitTargetType::VOLUMETRIC, ProfileHorizon::ExitTargetOp::GTE, 2.5f);
    }
    directLever[6] = {true, false, false, ProfileHorizon::Mode::FLOW, 2.0f, 9.0f, 2.4f, ProfileHorizon::Easing::LINEAR, 2.0f, true};
    directLever[7] = {true, true, false, ProfileHorizon::Mode::FLOW, 35.0f, 9.0f, 2.2f, ProfileHorizon::Easing::LINEAR, 35.0f, true};
    directLever[7].addTarget(ProfileHorizon::ExitTargetType::VOLUMETRIC, ProfileHorizon::ExitTargetOp::GTE, 40.0f);

    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(directLever, 8, 2)); // Start Saturate stays untouched
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(directLever, 8, 6));
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(directLever, 8, 7));
    // Low-pressure 2.5 bar flow-mode extraction passes hydraulic gate cleanly!
    TEST_ASSERT_TRUE(ProfileHorizon::isHydraulicallyReadyForMpc(true, 2.5f, 1.85f, 7.0f));

    // At end of phase 2 (w=3.2g), step 1 immediately skips phases 3, 4, 5 and lands in FLOW phase 6!
    ProfileHorizon::HorizonSample hSkip{};
    TEST_ASSERT_TRUE(ProfileHorizon::sampleHorizon(directLever, 8, 2, 3.0f, 2.0f, 6.0f, 1.8f, 2.02f, 1.5f, 0.80f, 0.25f,
                                                   16, hSkip, /*currentWeightG=*/3.2f, /*phaseStartWeightG=*/0.0f));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProfileHorizon::Mode::PRESSURE), static_cast<uint8_t>(hSkip.modes[0]));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProfileHorizon::Mode::FLOW), static_cast<uint8_t>(hSkip.modes[1]));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 9.0f, hSkip.basePressureBar[1]); // Phase 6 soft pressure cap = 9 bar!

    // 3. LH profile (Rapid filling flow=5/p=2 -> Puck Sat flow=1.5/p=2 -> Pump 9 bar brew volumetric>=40).
    ProfileHorizon::PhaseSpec lh[3]{};
    lh[0] = {false, false, false, ProfileHorizon::Mode::FLOW, 3.0f, 2.0f, 5.0f, ProfileHorizon::Easing::INSTANT, 0.0f, true};
    lh[1] = {false, false, false, ProfileHorizon::Mode::FLOW, 15.0f, 2.0f, 1.5f, ProfileHorizon::Easing::INSTANT, 0.0f, true};
    lh[2] = {true, true, false, ProfileHorizon::Mode::PRESSURE, 20.0f, 9.0f, 0.0f, ProfileHorizon::Easing::LINEAR, 2.0f, true};
    lh[2].addTarget(ProfileHorizon::ExitTargetType::VOLUMETRIC, ProfileHorizon::ExitTargetOp::GTE, 40.0f);
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(lh, 3, 0));
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(lh, 3, 1));
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(lh, 3, 2));

    // 4. Schema edge cases: Standard simple profile, closed valve (valve==0), -1 hold sentinel, and RATIO target.
    ProfileHorizon::PhaseSpec stdProf[1]{};
    stdProf[0] = {true, true, true, ProfileHorizon::Mode::SIMPLE, 50.0f, 0.0f, 0.0f};
    stdProf[0].isStandardProfile = true;
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(stdProf, 1, 0));

    ProfileHorizon::PhaseSpec closedValve[1]{};
    closedValve[0] = {true, false, false, ProfileHorizon::Mode::PRESSURE, 10.0f, 6.0f, 0.0f};
    closedValve[0].valveOpen = false;
    TEST_ASSERT_FALSE(ProfileHorizon::isExtractionPhaseActive(closedValve, 1, 0));
    ProfileHorizon::HorizonSample hClosed{};
    TEST_ASSERT_TRUE(ProfileHorizon::sampleHorizon(closedValve, 1, 0, 1.0f, 6.0f, 0.0f, 6.0f, 0.0f, 1.5f, 0.80f, 0.25f,
                                                   8, hClosed));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ProfileHorizon::Mode::SIMPLE), static_cast<uint8_t>(hClosed.modes[0]));

    // -1 sentinel + RATIO target (2.0 * 15.5g = 31.0g) + explicit current effective target from BrewProcess.
    ProfileHorizon::PhaseSpec ratioHold[1]{};
    ratioHold[0] = {true, false, false, ProfileHorizon::Mode::PRESSURE, 40.0f, -1.0f, 0.0f, ProfileHorizon::Easing::INSTANT, 0.0f, true};
    ratioHold[0].profileDoseG = 15.5f;
    ratioHold[0].addTarget(ProfileHorizon::ExitTargetType::RATIO, ProfileHorizon::ExitTargetOp::GTE, 2.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 31.0f, ratioHold[0].resolvedVolumetricTargetG());
    TEST_ASSERT_TRUE(ProfileHorizon::isExtractionPhaseActive(ratioHold, 1, 0, 7.2f, 0.0f));
    ProfileHorizon::HorizonSample hHold{};
    TEST_ASSERT_TRUE(ProfileHorizon::sampleHorizon(ratioHold, 1, 0, 2.0f, 7.2f, 0.0f, 7.1f, 1.4f, 1.5f, 0.80f, 0.25f, 8,
                                                   hHold, 10.0f, 2.0f, 25.0f, 15.0f, true, true, 7.2f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 7.2f, hHold.basePressureBar[0]);
}

// ---------------------------------------------------------------------------
// Group C — AutoTuning::Router On-Board vs Off-Board Boundary Isolation
// ---------------------------------------------------------------------------

static void test_router_onboard_and_offboard_isolation() {
    FakeTransport fakeTransport;
    AutoTuning::OptimizerConfiguration cfg;
    cfg.autoTuningEnabled = true;
    cfg.localOptimizationEnabled = true;
    cfg.optimizationPaused = false;
    cfg.beanContextSelected = true;

    // 1. OnBoard mode: must NEVER route off-board MQTT transport.
    cfg.providerMode = AutoTuning::ProviderMode::OnBoard;
    AutoTuning::Router onBoardRouter(cfg, &fakeTransport);
    TEST_ASSERT_FALSE(onBoardRouter.routeOffBoardTransport());
    TEST_ASSERT_FALSE(onBoardRouter.acceptOffBoardStatus());
#if defined(GAGGIMATE_ENABLE_ONBOARD_ESPRESSORL)
    TEST_ASSERT_TRUE(onBoardRouter.providerAvailable());
    TEST_ASSERT_TRUE(onBoardRouter.useOnBoardOptimizer());
    TEST_ASSERT_TRUE(onBoardRouter.acceptOptimizerStatus());
    TEST_ASSERT_TRUE(onBoardRouter.optimizationActive());
    TEST_ASSERT_EQUAL_STRING("ready", onBoardRouter.providerStatus());
#else
    TEST_ASSERT_FALSE(onBoardRouter.providerAvailable());
    TEST_ASSERT_FALSE(onBoardRouter.useOnBoardOptimizer());
    TEST_ASSERT_FALSE(onBoardRouter.acceptOptimizerStatus());
    TEST_ASSERT_FALSE(onBoardRouter.optimizationActive());
    TEST_ASSERT_EQUAL_STRING("unavailable", onBoardRouter.providerStatus());
#endif

    // 2. OffBoard mode: unaffected regardless of GAGGIMATE_ENABLE_ONBOARD_ESPRESSORL.
    cfg.providerMode = AutoTuning::ProviderMode::OffBoard;
    AutoTuning::Router offBoardRouter(cfg, &fakeTransport);
    TEST_ASSERT_TRUE(offBoardRouter.providerAvailable());
    TEST_ASSERT_TRUE(offBoardRouter.routeOffBoardTransport());
    TEST_ASSERT_FALSE(offBoardRouter.useOnBoardOptimizer());
    TEST_ASSERT_TRUE(offBoardRouter.acceptOffBoardStatus());
    TEST_ASSERT_TRUE(offBoardRouter.acceptOptimizerStatus());
    TEST_ASSERT_TRUE(offBoardRouter.optimizationActive());
}

// ---------------------------------------------------------------------------
// Group D — OnBoardStateCodec Binary Persistence & Zero-Trust Validation
// ---------------------------------------------------------------------------

static void test_onboard_state_codec_roundtrip_and_zero_trust_corruption() {
    OnBoardStateCodec::PersistedState src;
    src.activeContextKey = "bean_eth|grinder_df64|prof_cremina";
    src.optimizationRunId = "obrun_0123456789abcdef";
    src.activeBeanContextId = "bean_eth";
    src.activeGrinderContextId = "grinder_df64";
    src.activeProfileId = "prof_cremina";
    src.activeProfileLabel = "Cremina lever machine";
    src.activeRawProfileHash = "abcdef0123456789";
    src.tasteGoalMode = 1u;
    src.tasteGoalTargets[0] = 3u; // Fruity High
    src.tasteGoalTargets[8] = 1u; // Sour Low
    src.bevToPumpRatio = 0.84f;
    src.hasLastShotRecipe = true;
    src.lastShotHydraulicGrindSteps = -2.5f;
    src.lastShotDoseG = 16.5f;
    src.activeSplineDelta = {0.15f, -0.05f, -0.20f};
    src.controlMode = static_cast<std::uint8_t>(OnBoardOpt::ControlMode::Joint6D);
    src.hasPendingSplineDelta = true;
    src.pendingSplineDelta = {-0.20f, 0.15f, -0.35f};
    src.pendingSplineRecId = "obrec_002";
    src.trustDim = 6u;
    src.trustCenter = {0.42, 0.58, 0.65, 0.40, 0.55, 0.35};
    src.trustLength = 0.16;
    src.trustSuccesses = 2;
    src.trustFailures = 1;
    src.trustConverged = false;
    src.trustLastAction = 1u;
    src.hasBestPoint = true;
    src.bestPointIndex = 1u;
    src.localShotCount = 3;
    src.lastShotId = "shot_003";
    src.lastShotAt = 1794214206;
    src.lastShotTimeS = 41.5f;
    src.lastShotBeverageOutG = 35.8f;
    src.lastShotTargetYieldG = 36.0f;
    src.lastRecommendationId = "obrec_002";
    src.lastRecommendationAt = 1794214215;

    for (int i = 0; i < 60; ++i) {
        src.histZ.push_back({static_cast<float>(i) * 0.02f, 0.7f, -0.5f, -0.5f});
        src.histY.push_back(-0.35f + 0.005f * static_cast<float>(i));
    }
    for (int p = 0; p < 3; ++p) {
        OnBoardStateCodec::PersistedPoint pt;
        pt.shotId = "shot_00" + std::to_string(p + 1);
        pt.grindSteps = -1.0 * p;
        pt.doseG = 16.5;
        pt.targetYieldG = 36.0;
        pt.beverageOutG = 35.5 + 0.2 * p;
        pt.splineDelta = {-0.10f * p, 0.05f * p, -0.15f * p};
        pt.normDim = 6u;
        pt.normX = {0.4 + 0.1 * p, 0.5, 0.6, 0.5 - 0.05 * p, 0.5 + 0.025 * p, 0.5 - 0.075 * p};
        for (std::size_t k = 0; k < OnBoardStateCodec::PHI0_DIM; ++k) {
            pt.phi0[k] = 0.1 * static_cast<double>(k + p);
        }
        src.points.push_back(pt);
    }
    src.comparisons.push_back({1u, 0u, 0u});
    src.comparisons.push_back({2u, 1u, 1u});

    std::vector<std::uint8_t> bytes;
    TEST_ASSERT_TRUE(OnBoardStateCodec::encode(src, bytes));
    TEST_ASSERT_TRUE(bytes.size() > OnBoardStateCodec::HEADER_SIZE && bytes.size() < 8192u);

    OnBoardStateCodec::PersistedState dst;
    TEST_ASSERT_TRUE(OnBoardStateCodec::decode(bytes.data(), bytes.size(), dst));
    TEST_ASSERT_EQUAL_STRING(src.activeContextKey.c_str(), dst.activeContextKey.c_str());
    TEST_ASSERT_EQUAL_STRING(src.activeProfileLabel.c_str(), dst.activeProfileLabel.c_str());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(OnBoardOpt::ControlMode::Joint6D), dst.controlMode);
    TEST_ASSERT_TRUE(dst.hasPendingSplineDelta);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -0.35f, dst.pendingSplineDelta[2]);
    TEST_ASSERT_EQUAL_STRING("obrec_002", dst.pendingSplineRecId.c_str());
    TEST_ASSERT_EQUAL_UINT8(6u, dst.trustDim);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.35, dst.trustCenter[5]);
    TEST_ASSERT_EQUAL_UINT8(1u, dst.tasteGoalMode);
    TEST_ASSERT_EQUAL_UINT8(3u, dst.tasteGoalTargets[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.84f, dst.bevToPumpRatio);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -2.5f, dst.lastShotHydraulicGrindSteps);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -0.20f, dst.activeSplineDelta[2]);
    TEST_ASSERT_EQUAL_UINT32(60u, static_cast<uint32_t>(dst.histZ.size()));
    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(dst.points.size()));
    TEST_ASSERT_EQUAL_UINT8(6u, dst.points[2].normDim);
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -0.30f, dst.points[2].splineDelta[2]);
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(dst.comparisons.size()));
    TEST_ASSERT_EQUAL_UINT16(1u, dst.bestPointIndex);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.16, dst.trustLength);

    // 1. Single bit flip inside payload must fail CRC32 cleanly.
    auto badCrc = bytes;
    badCrc[OnBoardStateCodec::HEADER_SIZE + 5] ^= 0x01u;
    TEST_ASSERT_FALSE(OnBoardStateCodec::decode(badCrc.data(), badCrc.size(), dst));
    TEST_ASSERT_TRUE(dst.points.empty());

    // 2. Truncated buffer must be rejected.
    TEST_ASSERT_FALSE(OnBoardStateCodec::decode(bytes.data(), bytes.size() - 4, dst));

    // 3. Bad magic / version must be rejected.
    auto badMagic = bytes;
    badMagic[0] ^= 0xFFu;
    TEST_ASSERT_FALSE(OnBoardStateCodec::decode(badMagic.data(), badMagic.size(), dst));

    // 4. Out-of-bounds bestPointIndex / comparison index / mode / dim / NaN rejected at encode time too.
    OnBoardStateCodec::PersistedState badState = src;
    badState.bestPointIndex = 99u;
    std::vector<std::uint8_t> tmp;
    TEST_ASSERT_FALSE(OnBoardStateCodec::encode(badState, tmp));
    badState = src;
    badState.controlMode = 9u;
    TEST_ASSERT_FALSE(OnBoardStateCodec::encode(badState, tmp));
    badState = src;
    badState.trustDim = 5u;
    TEST_ASSERT_FALSE(OnBoardStateCodec::encode(badState, tmp));
    badState = src;
    badState.bevToPumpRatio = std::numeric_limits<float>::quiet_NaN();
    TEST_ASSERT_FALSE(OnBoardStateCodec::encode(badState, tmp));
    badState = src;
    badState.comparisons.push_back({0u, 0u, 0u}); // self-comparison invalid
    TEST_ASSERT_FALSE(OnBoardStateCodec::encode(badState, tmp));
}

// ---------------------------------------------------------------------------
// Group E: 3-Mode On-Board Optimization (RecipeOnly, SplineOnly, Joint6D)
// ---------------------------------------------------------------------------

void test_onboard_optimization_modes_and_spline_6d_cpbo_end_to_end(void) {
    // 1. Mode parsing & invalid-input rejection.
    OnBoardOpt::ControlMode parsed = OnBoardOpt::ControlMode::Joint6D;
    TEST_ASSERT_TRUE(OnBoardOpt::parseControlModeKey("recipe_only", parsed));
    TEST_ASSERT_EQUAL_UINT8(0u, static_cast<uint8_t>(parsed));
    TEST_ASSERT_TRUE(OnBoardOpt::parseControlModeKey("spline_only", parsed));
    TEST_ASSERT_EQUAL_UINT8(1u, static_cast<uint8_t>(parsed));
    TEST_ASSERT_TRUE(OnBoardOpt::parseControlModeKey("joint_6d", parsed));
    TEST_ASSERT_EQUAL_UINT8(2u, static_cast<uint8_t>(parsed));
    TEST_ASSERT_FALSE(OnBoardOpt::parseControlModeKey("unknown_mode", parsed));
    TEST_ASSERT_FALSE(OnBoardOpt::parseControlModeKey("", parsed));

    // 2. Spline quantization, bounds, and NaN safety.
    TEST_ASSERT_TRUE(OnBoardOpt::isValidSplineDelta({-1.0f, 0.0f, 1.0f}));
    TEST_ASSERT_FALSE(OnBoardOpt::isValidSplineDelta({-1.5f, 0.0f, 0.0f}));
    TEST_ASSERT_FALSE(OnBoardOpt::isValidSplineDelta({std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f}));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, -0.15f, OnBoardOpt::quantizeSplineKnot(-0.14f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, OnBoardOpt::quantizeSplineKnot(0.01f));
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.5, OnBoardOpt::splineKnotToNorm(0.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f, OnBoardOpt::normToSplineKnot(0.5));

    // 3. Spline shape invariants: lever decline (-0.2, 0.1, -0.6) -> negative slope, positive arch.
    const auto inv = OnBoardOpt::computeSplineShapeFeatures({-0.2f, 0.1f, -0.6f});
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -0.15, inv[0]); // mean = (-0.2 + 0.2 - 0.6)/4 = -0.15
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -0.40, inv[1]); // slope = -0.6 - (-0.2) = -0.40
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.50, inv[2]);  // arch = 0.1 - (-0.4) = +0.50

    // 3b. End-to-end 3D RecipeOnly CPBO fit + MES (rawDim=3 [grind,dose,yield], physDim=9 phi0).
    {
        std::vector<double> phiGrid;
        for (double f : {0.0, 0.5, 1.0}) {
            for (double d : {0.0, 0.5, 1.0}) {
                for (double o : {0.0, 0.5, 1.0}) {
                    const auto p0 = EspressoRL::computePhi0(f, 16.0 + 4.0 * d, 30.0 + 20.0 * o);
                    phiGrid.insert(phiGrid.end(), p0.begin(), p0.end());
                }
            }
        }
        EspressoRL::EmbeddedRobustScaler scaler;
        TEST_ASSERT_TRUE(scaler.fit(phiGrid.data(), phiGrid.size() / EspressoRL::PHYSICS_PROXY_DIM,
                                    EspressoRL::PHYSICS_PROXY_DIM));

        const std::array<std::array<double, 3>, 4> pts = {{
            {0.50, 0.50, 0.50},
            {0.58, 0.50, 0.52},
            {0.40, 0.45, 0.60},
            {0.62, 0.52, 0.48},
        }};
        constexpr std::size_t stride = 3 + EspressoRL::PHYSICS_PROXY_DIM;
        std::vector<double> trainX(4 * stride, 0.0);
        for (std::size_t i = 0; i < 4; ++i) {
            double *row = trainX.data() + i * stride;
            row[0] = pts[i][0];
            row[1] = pts[i][1];
            row[2] = pts[i][2];
            const auto p0 = EspressoRL::computePhi0(pts[i][0], 16.0 + 4.0 * pts[i][1], 30.0 + 20.0 * pts[i][2]);
            scaler.transformRow(p0.data(), row + 3);
        }
        const std::array<EspressoRL::ComparisonPair, 3> comps = {{
            {1u, 0u, EspressoRL::PreferenceLabel::NEW_BETTER},
            {2u, 1u, EspressoRL::PreferenceLabel::ANCHOR_BETTER},
            {3u, 1u, EspressoRL::PreferenceLabel::NEW_BETTER},
        }};
        EspressoRL::PreferenceGPConfig gpCfg;
        gpCfg.maxOuterIters = 25;
        EspressoRL::EmbeddedPreferenceGPModel model;
        TEST_ASSERT_TRUE(EspressoRL::fitEmbeddedPreferenceGP(trainX.data(), 4, 3, EspressoRL::PHYSICS_PROXY_DIM, 0,
                                                             comps.data(), comps.size(), gpCfg, model));
        TEST_ASSERT_TRUE(model.valid && std::isfinite(model.elbo));

        std::vector<double> anc(pts[3].begin(), pts[3].end());
        std::vector<double> lo(3, 0.15), hi(3, 0.85);
        std::vector<std::array<double, 6>> cands;
        TEST_ASSERT_TRUE(OnBoardOpt::generateTrustRegionCandidates(OnBoardOpt::ControlMode::RecipeOnly, anc, lo, hi,
                                                                   cands));
        TEST_ASSERT_EQUAL_UINT32(65u, static_cast<uint32_t>(cands.size()));
        std::vector<double> qIn(cands.size() * stride, 0.0);
        for (std::size_t i = 0; i < cands.size(); ++i) {
            double *row = qIn.data() + i * stride;
            row[0] = cands[i][0];
            row[1] = cands[i][1];
            row[2] = cands[i][2];
            const auto p0 = EspressoRL::computePhi0(cands[i][0], 16.0 + 4.0 * cands[i][1], 30.0 + 20.0 * cands[i][2]);
            scaler.transformRow(p0.data(), row + 3);
        }
        std::vector<double> pm, pc;
        TEST_ASSERT_TRUE(model.predict(qIn.data(), cands.size(), pm, pc));
        std::vector<std::size_t> candIdx(cands.size() - 1);
        for (std::size_t i = 1; i < cands.size(); ++i) candIdx[i - 1] = i;
        EspressoRL::MESConfig mesCfg;
        mesCfg.posteriorMaxFunctionSamples = 128;
        mesCfg.gumbelMaximumSamples = 500;
        EspressoRL::EmbeddedMESResult mes;
        TEST_ASSERT_TRUE(EspressoRL::evaluateEmbeddedCpboMes(pm.data(), pc.data(), cands.size(), candIdx.data(),
                                                             candIdx.size(), nullptr, 0, 0, model.gamma,
                                                             model.sigmaPref, mesCfg, 17u, mes));
        TEST_ASSERT_TRUE(mes.candidateIndex >= 1u && mes.candidateIndex < cands.size());
    }

    // 4. End-to-end 3D SplineOnly CPBO fit + MES (rawDim=3, physDim=3).
    {
        const std::array<std::array<float, 3>, 4> knots = {{
            {0.0f, 0.0f, 0.0f},
            {-0.15f, 0.15f, -0.25f},
            {0.25f, 0.20f, 0.10f},
            {-0.25f, 0.10f, -0.45f},
        }};
        std::vector<double> physGrid;
        for (float d0 : {-1.0f, 0.0f, 1.0f}) {
            for (float d1 : {-1.0f, 0.0f, 1.0f}) {
                for (float d2 : {-1.0f, 0.0f, 1.0f}) {
                    const auto sf = OnBoardOpt::computeSplineShapeFeatures({d0, d1, d2});
                    physGrid.insert(physGrid.end(), sf.begin(), sf.end());
                }
            }
        }
        EspressoRL::EmbeddedRobustScaler scaler;
        TEST_ASSERT_TRUE(scaler.fit(physGrid.data(), physGrid.size() / 3u, 3u));

        std::vector<double> trainX(4 * 6, 0.0);
        for (std::size_t i = 0; i < 4; ++i) {
            double *row = trainX.data() + i * 6;
            row[0] = OnBoardOpt::splineKnotToNorm(knots[i][0]);
            row[1] = OnBoardOpt::splineKnotToNorm(knots[i][1]);
            row[2] = OnBoardOpt::splineKnotToNorm(knots[i][2]);
            const auto sf = OnBoardOpt::computeSplineShapeFeatures(knots[i]);
            scaler.transformRow(sf.data(), row + 3);
        }
        const std::array<EspressoRL::ComparisonPair, 3> comps = {{
            {1u, 0u, EspressoRL::PreferenceLabel::NEW_BETTER},
            {2u, 1u, EspressoRL::PreferenceLabel::ANCHOR_BETTER},
            {3u, 1u, EspressoRL::PreferenceLabel::NEW_BETTER},
        }};
        EspressoRL::PreferenceGPConfig gpCfg;
        gpCfg.maxOuterIters = 25;
        gpCfg.initialRawKernelWeight = 0.75;
        gpCfg.initialPhysicsKernelWeight = 0.25;
        EspressoRL::EmbeddedPreferenceGPModel model;
        TEST_ASSERT_TRUE(EspressoRL::fitEmbeddedPreferenceGP(trainX.data(), 4, 3, 3, 0, comps.data(), comps.size(),
                                                             gpCfg, model));
        TEST_ASSERT_TRUE(model.valid && std::isfinite(model.elbo));

        std::vector<double> anchorNorm = {OnBoardOpt::splineKnotToNorm(knots[3][0]),
                                          OnBoardOpt::splineKnotToNorm(knots[3][1]),
                                          OnBoardOpt::splineKnotToNorm(knots[3][2])};
        std::vector<double> lo = {0.1, 0.1, 0.1};
        std::vector<double> hi = {0.9, 0.9, 0.9};
        std::vector<std::array<double, 6>> cands;
        TEST_ASSERT_TRUE(OnBoardOpt::generateTrustRegionCandidates(OnBoardOpt::ControlMode::SplineOnly, anchorNorm, lo,
                                                                   hi, cands));
        TEST_ASSERT_EQUAL_UINT32(65u, static_cast<uint32_t>(cands.size()));

        std::vector<double> qIn(cands.size() * 6, 0.0);
        for (std::size_t i = 0; i < cands.size(); ++i) {
            double *row = qIn.data() + i * 6;
            row[0] = cands[i][0];
            row[1] = cands[i][1];
            row[2] = cands[i][2];
            const std::array<float, 3> sp = {OnBoardOpt::normToSplineKnot(cands[i][0]),
                                             OnBoardOpt::normToSplineKnot(cands[i][1]),
                                             OnBoardOpt::normToSplineKnot(cands[i][2])};
            const auto sf = OnBoardOpt::computeSplineShapeFeatures(sp);
            scaler.transformRow(sf.data(), row + 3);
        }
        std::vector<double> pm, pc;
        TEST_ASSERT_TRUE(model.predict(qIn.data(), cands.size(), pm, pc));
        std::vector<std::size_t> candIdx(cands.size() - 1);
        for (std::size_t i = 1; i < cands.size(); ++i) candIdx[i - 1] = i;
        EspressoRL::MESConfig mesCfg;
        mesCfg.posteriorMaxFunctionSamples = 128;
        mesCfg.gumbelMaximumSamples = 500;
        EspressoRL::EmbeddedMESResult mes;
        TEST_ASSERT_TRUE(EspressoRL::evaluateEmbeddedCpboMes(pm.data(), pc.data(), cands.size(), candIdx.data(),
                                                             candIdx.size(), nullptr, 0, 0, model.gamma,
                                                             model.sigmaPref, mesCfg, 42u, mes));
        TEST_ASSERT_TRUE(mes.candidateIndex >= 1u && mes.candidateIndex < cands.size());
        const auto &win = cands[mes.candidateIndex];
        const std::array<float, 3> winSpline = {OnBoardOpt::normToSplineKnot(win[0]),
                                                OnBoardOpt::normToSplineKnot(win[1]),
                                                OnBoardOpt::normToSplineKnot(win[2])};
        TEST_ASSERT_TRUE(OnBoardOpt::isValidSplineDelta(winSpline));
    }

    // 5. End-to-end 6D Joint6D CPBO fit + MES (rawDim=6, physDim=9, 64 R6+axis candidates).
    {
        std::vector<double> phiGrid;
        for (double f : {0.0, 0.5, 1.0}) {
            for (double d : {0.0, 0.5, 1.0}) {
                for (double o : {0.0, 0.5, 1.0}) {
                    const auto p0 = EspressoRL::computePhi0(f, 16.0 + 4.0 * d, 30.0 + 20.0 * o);
                    phiGrid.insert(phiGrid.end(), p0.begin(), p0.end());
                }
            }
        }
        EspressoRL::EmbeddedRobustScaler scaler;
        TEST_ASSERT_TRUE(scaler.fit(phiGrid.data(), phiGrid.size() / EspressoRL::PHYSICS_PROXY_DIM,
                                    EspressoRL::PHYSICS_PROXY_DIM));

        const std::array<std::array<double, 6>, 4> pts = {{
            {0.50, 0.50, 0.50, 0.50, 0.50, 0.50},
            {0.55, 0.50, 0.50, 0.42, 0.57, 0.37},
            {0.40, 0.45, 0.60, 0.65, 0.55, 0.60},
            {0.58, 0.52, 0.48, 0.38, 0.55, 0.30},
        }};
        constexpr std::size_t stride = 6 + EspressoRL::PHYSICS_PROXY_DIM;
        std::vector<double> trainX(4 * stride, 0.0);
        for (std::size_t i = 0; i < 4; ++i) {
            double *row = trainX.data() + i * stride;
            for (std::size_t d = 0; d < 6; ++d) row[d] = pts[i][d];
            const auto p0 = EspressoRL::computePhi0(pts[i][0], 16.0 + 4.0 * pts[i][1], 30.0 + 20.0 * pts[i][2]);
            scaler.transformRow(p0.data(), row + 6);
        }
        const std::array<EspressoRL::ComparisonPair, 3> comps = {{
            {1u, 0u, EspressoRL::PreferenceLabel::NEW_BETTER},
            {2u, 1u, EspressoRL::PreferenceLabel::ANCHOR_BETTER},
            {3u, 1u, EspressoRL::PreferenceLabel::NEW_BETTER},
        }};
        EspressoRL::PreferenceGPConfig gpCfg;
        gpCfg.maxOuterIters = 25;
        EspressoRL::EmbeddedPreferenceGPModel model;
        TEST_ASSERT_TRUE(EspressoRL::fitEmbeddedPreferenceGP(trainX.data(), 4, 6, EspressoRL::PHYSICS_PROXY_DIM, 0,
                                                             comps.data(), comps.size(), gpCfg, model));
        TEST_ASSERT_TRUE(model.valid && std::isfinite(model.elbo));

        std::vector<double> anc(pts[3].begin(), pts[3].end());
        std::vector<double> lo(6, 0.15), hi(6, 0.85);
        std::vector<std::array<double, 6>> cands;
        TEST_ASSERT_TRUE(OnBoardOpt::generateTrustRegionCandidates(OnBoardOpt::ControlMode::Joint6D, anc, lo, hi,
                                                                   cands));
        TEST_ASSERT_EQUAL_UINT32(65u, static_cast<uint32_t>(cands.size()));
        for (std::size_t i = 1; i < cands.size(); ++i) {
            for (std::size_t d = 0; d < 6; ++d) {
                TEST_ASSERT_TRUE(cands[i][d] >= lo[d] - 1e-9 && cands[i][d] <= hi[d] + 1e-9);
            }
        }
        std::vector<double> qIn(cands.size() * stride, 0.0);
        for (std::size_t i = 0; i < cands.size(); ++i) {
            double *row = qIn.data() + i * stride;
            for (std::size_t d = 0; d < 6; ++d) row[d] = cands[i][d];
            const auto p0 = EspressoRL::computePhi0(cands[i][0], 16.0 + 4.0 * cands[i][1], 30.0 + 20.0 * cands[i][2]);
            scaler.transformRow(p0.data(), row + 6);
        }
        std::vector<double> pm, pc;
        TEST_ASSERT_TRUE(model.predict(qIn.data(), cands.size(), pm, pc));
        std::vector<std::size_t> candIdx(cands.size() - 1);
        for (std::size_t i = 1; i < cands.size(); ++i) candIdx[i - 1] = i;
        EspressoRL::MESConfig mesCfg;
        mesCfg.posteriorMaxFunctionSamples = 128;
        mesCfg.gumbelMaximumSamples = 500;
        EspressoRL::EmbeddedMESResult mes;
        TEST_ASSERT_TRUE(EspressoRL::evaluateEmbeddedCpboMes(pm.data(), pc.data(), cands.size(), candIdx.data(),
                                                             candIdx.size(), nullptr, 0, 0, model.gamma,
                                                             model.sigmaPref, mesCfg, 99u, mes));
        TEST_ASSERT_TRUE(mes.candidateIndex >= 1u && mes.candidateIndex < cands.size());
    }
}

void setUp(void) {}
void tearDown(void) {}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_live_lease_shadow_mode_never_actuates);
    RUN_TEST(test_live_lease_corridor_slew_and_deadman_expiry);
    RUN_TEST(test_live_lease_uint32_wraparound_and_invalid_inputs);
    RUN_TEST(test_horizon_sampler_damian_pressure_decline_and_lancelot_mixed);
    RUN_TEST(test_universal_schema_matrix_cremina_direct_lever_lh_and_standard);
    RUN_TEST(test_router_onboard_and_offboard_isolation);
    RUN_TEST(test_onboard_state_codec_roundtrip_and_zero_trust_corruption);
    RUN_TEST(test_onboard_optimization_modes_and_spline_6d_cpbo_end_to_end);
    return UNITY_END();
}
