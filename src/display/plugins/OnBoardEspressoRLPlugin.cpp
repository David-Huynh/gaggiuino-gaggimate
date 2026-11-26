#include "OnBoardEspressoRLPlugin.h"

#if defined(GAGGIMATE_ENABLE_ONBOARD_ESPRESSORL) && !defined(GAGGIMATE_SIM)

#include <EspressoRLCore.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <display/core/AutoTuning.h>
#include <display/core/Controller.h>
#include <display/core/EpochTime.h>
#include <display/core/OnBoardStateCodec.h>
#include <display/core/PluginManager.h>
#include <display/core/ProfileHorizonSampler.h>
#include <display/core/StorageCoordinator.h>
#include <display/core/process/BrewProcess.h>
#include <display/models/shot_log_format.h>
#include <display/plugins/autotuning/AutoTuningPayloadMetadata.h>
#include <display/plugins/autotuning/AutoTuningTasteGoalJson.h>
#include <display/plugins/autotuning/local/LocalAutoTuningFiles.h>
#include <display/util/AtomicFile.h>
#include <display/util/LittleFSUtil.h>
#include <esp_log.h>
#include <esp_system.h>

namespace {

constexpr const char *LOG_TAG = "OnBoardEspressoRL";
constexpr const char *PERSIST_DIR = "/rll";
constexpr const char *PERSIST_STATE_PATH = "/rll/ob_state.bin";
constexpr std::size_t MAX_HYDRAULIC_HISTORY = 480; // ~6-8 shots at 4 Hz in PSRAM
constexpr std::size_t MAX_CPBO_POINTS = 40;
constexpr std::size_t MAX_CPBO_COMPARISONS = 48;
constexpr std::size_t MAX_PHASE_WINDOW = 16; // Sliding lookahead window from currentPhaseIdx (handles any profile length)
constexpr uint32_t LIVE_LEASE_DURATION_MS = 500;
constexpr uint32_t CPBO_MAX_RUNTIME_MS = 12000;
constexpr uint32_t CPBO_YIELD_INTERVAL_MS = 20;

struct YieldContext {
    std::atomic<bool> *brewActive = nullptr;
    std::atomic<bool> *otaActive = nullptr;
    uint32_t startedAtMs = 0;
    uint32_t lastYieldMs = 0;
    uint32_t maxRuntimeMs = CPBO_MAX_RUNTIME_MS;
};

bool cooperativeYieldCallback(void *raw) {
    auto *ctx = static_cast<YieldContext *>(raw);
    if (ctx == nullptr) {
        return true;
    }
    if ((ctx->brewActive != nullptr && ctx->brewActive->load(std::memory_order_acquire)) ||
        (ctx->otaActive != nullptr && ctx->otaActive->load(std::memory_order_acquire))) {
        return false; // Immediate cooperative abort on brew start or OTA update
    }
    const uint32_t now = millis();
    if (static_cast<uint32_t>(now - ctx->startedAtMs) > ctx->maxRuntimeMs) {
        return false;
    }
    if (static_cast<uint32_t>(now - ctx->lastYieldMs) >= CPBO_YIELD_INTERVAL_MS) {
        vTaskDelay(1);
        ctx->lastYieldMs = millis();
    }
    return true;
}

ProfileHorizon::Easing mapEasing(TransitionType t) {
    switch (t) {
    case TransitionType::LINEAR:
        return ProfileHorizon::Easing::LINEAR;
    case TransitionType::EASE_IN:
        return ProfileHorizon::Easing::EASE_IN;
    case TransitionType::EASE_OUT:
        return ProfileHorizon::Easing::EASE_OUT;
    case TransitionType::EASE_IN_OUT:
        return ProfileHorizon::Easing::EASE_IN_OUT;
    case TransitionType::INSTANT:
    default:
        return ProfileHorizon::Easing::INSTANT;
    }
}

ProfileHorizon::TransTarget mapTransTarget(TransitionTarget t) {
    switch (t) {
    case TransitionTarget::VOLUMETRIC:
        return ProfileHorizon::TransTarget::VOLUMETRIC;
    case TransitionTarget::PUMPED:
        return ProfileHorizon::TransTarget::PUMPED;
    case TransitionTarget::TIME:
    default:
        return ProfileHorizon::TransTarget::TIME;
    }
}

ProfileHorizon::ExitTargetType mapExitTargetType(TargetType t) {
    switch (t) {
    case TargetType::TARGET_TYPE_RATIO:
        return ProfileHorizon::ExitTargetType::RATIO;
    case TargetType::TARGET_TYPE_PRESSURE:
        return ProfileHorizon::ExitTargetType::PRESSURE;
    case TargetType::TARGET_TYPE_FLOW:
        return ProfileHorizon::ExitTargetType::FLOW;
    case TargetType::TARGET_TYPE_PUMPED:
        return ProfileHorizon::ExitTargetType::PUMPED;
    case TargetType::TARGET_TYPE_VOLUMETRIC:
    default:
        return ProfileHorizon::ExitTargetType::VOLUMETRIC;
    }
}

ProfileHorizon::ExitTargetOp mapExitTargetOp(TargetOperator op) {
    return (op == TargetOperator::LTE) ? ProfileHorizon::ExitTargetOp::LTE : ProfileHorizon::ExitTargetOp::GTE;
}

float selectBeverageFlowGS(const AutoTuning::ShotSample &smp) {
    // Prefer EMA-smoothed measuredFlow (vf) so 1-2 Hz Bluetooth scales don't drop alternating 250 ms ticks to zero.
    return (std::isfinite(smp.measuredFlow) && smp.measuredFlow > 0.05f) ? smp.measuredFlow : smp.flow;
}

float canonicalFinerGrindSteps(const AutoTuning::GrinderSnapshot &grinder) {
    const float raw = grinder.relativeStepsFromReference.value_or(0.0f);
    if (!std::isfinite(raw)) {
        return 0.0f;
    }
    const float sign = (grinder.stepDirection == "higher_is_coarser") ? -1.0f : 1.0f;
    return raw * sign;
}

EspressoRL::PumpMode mapHorizonMode(ProfileHorizon::Mode m) {
    switch (m) {
    case ProfileHorizon::Mode::PRESSURE:
        return EspressoRL::PumpMode::PRESSURE;
    case ProfileHorizon::Mode::FLOW:
        return EspressoRL::PumpMode::FLOW;
    case ProfileHorizon::Mode::SIMPLE:
    default:
        return EspressoRL::PumpMode::SIMPLE;
    }
}

LiveLeasePumpMode mapCoreToLeaseMode(EspressoRL::PumpMode m) {
    switch (m) {
    case EspressoRL::PumpMode::PRESSURE:
        return LiveLeasePumpMode::PRESSURE;
    case EspressoRL::PumpMode::FLOW:
        return LiveLeasePumpMode::FLOW;
    case EspressoRL::PumpMode::SIMPLE:
    default:
        return LiveLeasePumpMode::SIMPLE;
    }
}

std::string makeRecommendationId() {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "obrec_%08lx%08lx", static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));
    return std::string(buf);
}

std::string makeRunId(const std::string &contextKey) {
    // FNV-1a 64-bit deterministic run identifier from active bean/grinder/profile context.
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : contextKey) {
        hash ^= static_cast<uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    char buf[36];
    std::snprintf(buf, sizeof(buf), "obrun_%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

EspressoRL::PreferenceLabel mapPreferenceLabel(AutoTuning::PreferenceLabel label) {
    switch (label) {
    case AutoTuning::PreferenceLabel::NewBetter:
        return EspressoRL::PreferenceLabel::NEW_BETTER;
    case AutoTuning::PreferenceLabel::AnchorBetter:
        return EspressoRL::PreferenceLabel::ANCHOR_BETTER;
    case AutoTuning::PreferenceLabel::Tie:
    default:
        return EspressoRL::PreferenceLabel::TIE;
    }
}

const char *trustActionName(EspressoRL::TrustRegionAction action) {
    switch (action) {
    case EspressoRL::TrustRegionAction::IMPROVED:
        return "improved";
    case EspressoRL::TrustRegionAction::NON_IMPROVEMENT:
        return "non_improvement";
    case EspressoRL::TrustRegionAction::EXPANDED:
        return "expanded";
    case EspressoRL::TrustRegionAction::CONTRACTED:
        return "contracted";
    case EspressoRL::TrustRegionAction::CONVERGED:
        return "converged";
    case EspressoRL::TrustRegionAction::NONE:
    default:
        return "none";
    }
}

double quantizeStep(double val, double minVal, double maxVal, double resolution) {
    const double clipped = std::clamp(val, minVal, maxVal);
    if (resolution <= 0.0) {
        return clipped;
    }
    const double steps = std::floor(((clipped - minVal) / resolution) + 0.5 + 1e-12);
    return std::clamp(minVal + steps * resolution, minVal, maxVal);
}

} // namespace

struct OnBoardEspressoRLPlugin::Workspace {
    EspressoRL::CombinedHydraulicPredictor predictor;
    EspressoRL::CemGpMpcController mpc;

    std::string activeContextKey;
    std::string optimizationRunId;
    std::string activeBeanContextId;
    std::string activeGrinderContextId;
    std::string activeProfileId;
    std::string activeProfileLabel;
    std::string activeRawProfileHash;
    AutoTuning::TasteGoal activeTasteGoal = AutoTuning::TasteGoal::balanced();

    // Rolling cross-shot hydraulic history (PSRAM-backed).
    std::vector<EspressoRL::FeatureVec4, PsramStlAllocator<EspressoRL::FeatureVec4>> histZ;
    std::vector<float, PsramStlAllocator<float>> histY;

    // Between-shot CPBO point & comparison store for the active run.
    struct CpboPoint {
        std::string shotId;
        double grindSteps = 0.0;
        double doseG = 18.0;
        double targetYieldG = 36.0;
        double beverageOutG = 0.0;
        std::array<double, 3> normX{0.5, 0.5, 0.5};
        std::array<double, EspressoRL::PHYSICS_PROXY_DIM> phi0{};
    };

    std::vector<CpboPoint> points;
    std::vector<EspressoRL::ComparisonPair> comparisons;
    EspressoRL::Level1TrustRegionState trustRegion;
    EspressoRL::EmbeddedPreferenceGPModel lastModel;
    std::array<float, 3> activeSplineDelta{0.0f, 0.0f, 0.0f}; // Level 1 Assist default (0, 0, 0)
    EspressoRL::NominalBaselinePolicy baselinePolicy;
    bool hasLastShotRecipe = false;
    float lastShotHydraulicGrindSteps = 0.0f;
    float lastShotDoseG = 18.0f;

    std::size_t bestPointIndex = 0;
    bool hasBestPoint = false;
    std::string lastShotId;
    AutoTuning::Timestamp lastShotAt = 0;
    float lastShotTimeS = 0.0f;
    float lastShotBeverageOutG = 0.0f;
    float lastShotTargetYieldG = 0.0f;
    std::string lastRecommendationId;
    AutoTuning::Timestamp lastRecommendationAt = 0;
    int localShotCount = 0;
    uint32_t lastMpcFitUs = 0;
    uint32_t lastCpboFitMs = 0;
    bool lastCpboCancelled = false;

    Workspace()
        : predictor(),
          mpc(predictor, EspressoRL::HydraulicControlEnvelope{}, EspressoRL::SplineTrajectoryBasis{3, 1.0f},
              makeMpcConfig()) {
        histZ.reserve(MAX_HYDRAULIC_HISTORY);
        histY.reserve(MAX_HYDRAULIC_HISTORY);
        resetContext("", "", "", "", "", "", AutoTuning::TasteGoal::balanced());
    }

    static EspressoRL::CemGpMpcConfig makeMpcConfig() {
        EspressoRL::CemGpMpcConfig cfg;
        cfg.horizonSteps = 16;
        cfg.controlKnots = 4;
        cfg.cemIterations = 2;
        cfg.candidateCount = 24;
        cfg.eliteCount = 5;
        return cfg;
    }

    void resetContext(const std::string &ctxKey, const std::string &beanId, const std::string &grinderId,
                      const std::string &profileId, const std::string &profileLabel, const std::string &rawHash,
                      const AutoTuning::TasteGoal &goal) {
        activeContextKey = ctxKey;
        optimizationRunId = ctxKey.empty() ? "" : makeRunId(ctxKey);
        activeBeanContextId = beanId;
        activeGrinderContextId = grinderId;
        activeProfileId = profileId;
        activeProfileLabel = profileLabel;
        activeRawProfileHash = rawHash;
        activeTasteGoal = goal;
        predictor.resetAll();
        histZ.clear();
        histY.clear();
        points.clear();
        comparisons.clear();
        trustRegion = EspressoRL::Level1TrustRegionState{};
        trustRegion.center = {0.5, 0.5, 0.5};
        trustRegion.length = 0.20;
        lastModel = EspressoRL::EmbeddedPreferenceGPModel{};
        activeSplineDelta = {0.0f, 0.0f, 0.0f};
        baselinePolicy.resetShot(false, false, true, true);
        hasLastShotRecipe = false;
        lastShotHydraulicGrindSteps = 0.0f;
        lastShotDoseG = 18.0f;
        bestPointIndex = 0;
        hasBestPoint = false;
        localShotCount = 0;
    }

    int findPointByShotId(const std::string &shotId) const {
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (points[i].shotId == shotId) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

OnBoardEspressoRLPlugin OnBoardEspressoRL;

OnBoardEspressoRLPlugin::OnBoardEspressoRLPlugin() = default;
OnBoardEspressoRLPlugin::~OnBoardEspressoRLPlugin() = default;

void OnBoardEspressoRLPlugin::setup(Controller *ctrl, PluginManager *pm) {
    controller_ = ctrl;
    pluginManager_ = pm;

    if (controller_ != nullptr) {
        shadowModeOnly_.store(controller_->getSettings().isRLOnBoardShadowOnly(), std::memory_order_release);
    }

    workspace_ = makePsramUnique<Workspace>();
    workspaceReady_.store(workspace_ != nullptr, std::memory_order_release);

    if (controller_ != nullptr) {
        controller_->setOnBoardOptimizer(this);
    }

    // Spawn dedicated Core-1 background worker for between-shot GP/CPBO optimization & flash persistence.
    if (xTaskCreatePinnedToCore(workerTaskEntry, "OnBoardEspressoRL", 12288, this, 1, &workerTaskHandle_, 1) !=
        pdPASS) {
        workerTaskHandle_ = nullptr;
        workspaceReady_.store(false, std::memory_order_release);
        ESP_LOGE(LOG_TAG, "Failed to create Core-1 OnBoardEspressoRL worker task");
        return;
    }

    {
        WorkItem loadWork;
        loadWork.kind = WorkKind::LoadPersistedState;
        enqueueWork(std::move(loadWork));
    }

    pm->on("controller:brew:start", [this](Event const &) { brewActive_.store(true, std::memory_order_release); });
    pm->on("controller:brew:end", [this](Event const &) { brewActive_.store(false, std::memory_order_release); });
    pm->on("ota:update:start", [this](Event const &) { otaActive_.store(true, std::memory_order_release); });
    pm->on("ota:update:phase", [this](Event const &ev) {
        const int ph = ev.getInt("phase");
        if (ph == 4 || ph == 5) { // PHASE_FINISHED or PHASE_FAILED
            otaActive_.store(false, std::memory_order_release);
        }
    });

    pm->on("rl:preference", [this](Event &event) {
        if (!isOnBoardSelected()) {
            return;
        }
        AutoTuning::PreferenceFeedback const *pref = event.getPayload<AutoTuning::PreferenceFeedback>();
        if (pref == nullptr || pref->newShotId.empty() || pref->anchorShotId.empty() ||
            pref->newShotId == pref->anchorShotId) {
            event.setInt("decision_persisted", 0);
            return;
        }
        if (pref->label == AutoTuning::PreferenceLabel::Abstain) {
            event.setInt("decision_persisted", 1);
            return;
        }
        WorkItem work;
        work.kind = WorkKind::ProcessPreference;
        work.preference = *pref;
        event.setInt("decision_persisted", enqueueWork(std::move(work)) ? 1 : 0);
    });

    pm->on("rl:local:reset", [this](Event const &) {
        if (!isOnBoardSelected()) {
            return;
        }
        WorkItem work;
        work.kind = WorkKind::ResetContext;
        enqueueWork(std::move(work));
    });

    pm->on("rl:status:refresh", [this](Event const &) {
        if (!isOnBoardSelected()) {
            return;
        }
        WorkItem work;
        work.kind = WorkKind::RefreshStatus;
        enqueueWork(std::move(work));
    });

    pm->on("rl:settings:changed", [this](Event const &) {
        if (!isOnBoardSelected()) {
            return;
        }
        WorkItem work;
        work.kind = WorkKind::RefreshStatus;
        enqueueWork(std::move(work));
    });

    ESP_LOGI(LOG_TAG, "OnBoardEspressoRL ready on Core 1 (PSRAM workspace, shadowModeOnly=%d)",
             shadowModeOnly_.load() ? 1 : 0);
}

void OnBoardEspressoRLPlugin::loop() {
    drainMainThreadEvents();
    if (isOnBoardSelected()) {
        const uint32_t now = millis();
        if (lastStatusPublishMs_ == 0 || static_cast<uint32_t>(now - lastStatusPublishMs_) >= 15000u) {
            lastStatusPublishMs_ = now;
            WorkItem work;
            work.kind = WorkKind::RefreshStatus;
            enqueueWork(std::move(work));
        }
    }
}

bool OnBoardEspressoRLPlugin::isOnBoardSelected() const {
    if (controller_ == nullptr || !ready()) {
        return false;
    }
    return AutoTuning::Router(controller_->getSettings().getRLOptimizerConfiguration(),
                              controller_->getOptimizerTransport())
        .useOnBoardOptimizer();
}

std::string OnBoardEspressoRLPlugin::machineId() const {
    String mac = WiFi.macAddress();
    mac.replace(":", "_");
    return std::string("gaggimate:") + mac.c_str();
}

std::string OnBoardEspressoRLPlugin::activeContextKey() const {
    if (controller_ == nullptr) {
        return "";
    }
    const Settings &s = controller_->getSettings();
    std::string profileId;
    if (auto *pm = controller_->getProfileManager()) {
        profileId = pm->getSelectedProfile().id.c_str();
    }
    return std::string(s.getRLBeanContextId().c_str()) + "|" + std::string(s.getRLGrinderContextId().c_str()) + "|" +
           profileId;
}

bool OnBoardEspressoRLPlugin::enqueueWork(WorkItem item) {
    if (workerTaskHandle_ == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(workQueueMutex_);
        if (workQueue_.size() >= 16) {
            return false;
        }
        workQueue_.push_back(std::move(item));
    }
    xTaskNotifyGive(workerTaskHandle_);
    return true;
}

void OnBoardEspressoRLPlugin::queueMainThreadEvent(Event event) {
    std::lock_guard<std::mutex> lock(mainEventMutex_);
    if (mainThreadEvents_.size() < 24) {
        mainThreadEvents_.push_back(std::move(event));
    }
}

void OnBoardEspressoRLPlugin::drainMainThreadEvents() {
    if (pluginManager_ == nullptr) {
        return;
    }
    for (;;) {
        Event ev;
        {
            std::lock_guard<std::mutex> lock(mainEventMutex_);
            if (mainThreadEvents_.empty()) {
                return;
            }
            ev = std::move(mainThreadEvents_.front());
            mainThreadEvents_.pop_front();
        }
        pluginManager_->trigger(ev);
    }
}

bool OnBoardEspressoRLPlugin::onLiveShotStarted(AutoTuning::LiveShotStarted const &) {
    if (!isOnBoardSelected() || !workspace_) {
        return false;
    }
    brewActive_.store(true, std::memory_order_release);
    liveSampleCount_ = 0;
    maxLiveWeightG_ = 0.0f;

    bool grindConfirmedPreShot = false;
    if (controller_ != nullptr) {
        AutoTuning::RecipeSnapshot recipe;
        AutoTuningPayloadMetadata::captureRecipe(controller_, recipe);
        const Settings &s = controller_->getSettings();
        float dose = 0.0f;
        float yield = 0.0f;
        if (auto *pm = controller_->getProfileManager()) {
            const Profile &sel = pm->getSelectedProfile();
            if (std::isfinite(sel.dose) && sel.dose > 1.0f) {
                dose = sel.dose;
            }
            const float vol = sel.getTotalVolume();
            if (std::isfinite(vol) && vol >= 5.0f) {
                yield = vol;
            }
        }
        if (dose <= 1.0f) {
            const float grinderDose = recipe.doseTargetG.value_or(static_cast<float>(s.getTargetGrindVolume()));
            dose = (std::isfinite(grinderDose) && grinderDose > 1.0f) ? grinderDose : 18.0f;
        }
        liveDoseG_ = dose;
        liveTargetYieldG_ = (yield >= 5.0f) ? yield : std::clamp(2.0f * liveDoseG_, 10.0f, 90.0f);
        liveGrindSteps_ = canonicalFinerGrindSteps(recipe.grinder);
        grindConfirmedPreShot = recipe.grinder.observed && recipe.grinder.relativeStepsFromReference.has_value();
    }

    std::unique_lock<std::mutex> lock(workspaceMutex_, std::try_to_lock);
    if (lock.owns_lock()) {
        workspace_->predictor.resetShot();
        workspace_->mpc.resetShot();
        const bool hasCrossShotHistory = workspace_->histZ.size() >= 12;
        const bool recipeChanged =
            !workspace_->hasLastShotRecipe ||
            std::fabs(liveGrindSteps_ - workspace_->lastShotHydraulicGrindSteps) > 0.05f ||
            std::fabs(liveDoseG_ - workspace_->lastShotDoseG) > 0.15f;
        const bool assistOnly = (std::fabs(workspace_->activeSplineDelta[0]) +
                                 std::fabs(workspace_->activeSplineDelta[1]) +
                                 std::fabs(workspace_->activeSplineDelta[2])) < 1e-4f;
        workspace_->baselinePolicy.resetShot(hasCrossShotHistory, grindConfirmedPreShot, recipeChanged, assistOnly);
    }
    return true;
}

bool OnBoardEspressoRLPlugin::onLiveShotSample(AutoTuning::LiveShotSample const &event) {
    if (!isOnBoardSelected() || !workspace_ || controller_ == nullptr) {
        return false;
    }
    // Never block the 4 Hz brew loop if the Core-1 worker is finishing a brief cleanup.
    std::unique_lock<std::mutex> lock(workspaceMutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }

    const AutoTuning::ShotSample &smp = event.sample;
    // Ignore post-stop extended recording or cup-removal weight drops cleanly.
    if ((smp.systemInfo & SYSTEM_INFO_EXTENDED_RECORDING) != 0) {
        return true;
    }
    if (std::isfinite(smp.weight)) {
        if (smp.weight + 1.0f < maxLiveWeightG_) {
            return true;
        }
        if (smp.weight > maxLiveWeightG_) {
            maxLiveWeightG_ = smp.weight;
        }
    }

    const float waterPumped = smp.waterPumped.value_or(0.0f);
    const float bevFlow = selectBeverageFlowGS(smp);
    const bool pumpActive = smp.targetPressure > 0.1f || smp.targetFlow > 0.1f || smp.pumpFlow > 0.15f;
    const bool activeEst = smp.valveOpen && pumpActive && smp.pressure >= 1.2f && bevFlow >= 0.15f;

    workspace_->predictor.observeTick(smp.pressure, bevFlow, waterPumped, liveGrindSteps_, liveDoseG_, activeEst);

    // Snapshot a sliding phase window starting at currentPhaseIdx without holding processMutex during CEM rollouts.
    std::array<ProfileHorizon::PhaseSpec, MAX_PHASE_WINDOW> phaseSpecs{};
    std::size_t windowCount = 0;
    float currentPhaseElapsedS = 0.0f;
    float phaseStartP = smp.pressure;
    float phaseStartQ = smp.pumpFlow;
    float curEffP = smp.targetPressure;
    float curEffQ = smp.targetFlow;
    float phaseStartWeightG = 0.0f;
    float phaseStartPumpedMl = 0.0f;
    bool volumetricEnabled = true;
    bool inExtractionPhase = false;
    bool activeAdvancedBrew = false;

    {
        std::lock_guard<std::recursive_mutex> pLock(controller_->getProcessLock());
        Process *proc = controller_->getProcess();
        if (proc != nullptr && proc->getType() == MODE_BREW && proc->isActive()) {
            auto *bp = static_cast<BrewProcess *>(proc);
            if (bp->isUtility()) {
                return true;
            }
            activeAdvancedBrew = bp->isAdvancedPump();
            const std::size_t totalPhases = bp->profile.phases.size();
            const std::size_t startIdx = bp->phaseIndex;
            if (startIdx < totalPhases) {
                const unsigned long nowMs = millis();
                currentPhaseElapsedS = (nowMs >= bp->currentPhaseStarted)
                                           ? static_cast<float>(nowMs - bp->currentPhaseStarted) / 1000.0f
                                           : 0.0f;
                phaseStartP = bp->getPhaseStartPressure();
                phaseStartQ = bp->getPhaseStartFlow();
                curEffP = bp->getEffectivePressure();
                curEffQ = bp->getEffectiveFlow();
                phaseStartWeightG = static_cast<float>(bp->phaseStartVolume);
                phaseStartPumpedMl = bp->phaseStartedPumped;
                volumetricEnabled = (bp->target == ProcessTarget::VOLUMETRIC);
                const bool isStandard = (bp->profile.type == "standard");
                const float profDose = (std::isfinite(bp->profile.dose) && bp->profile.dose > 0.0f)
                                           ? bp->profile.dose
                                           : liveDoseG_;

                windowCount = std::min(totalPhases - startIdx, MAX_PHASE_WINDOW);
                for (std::size_t wi = 0; wi < windowCount; ++wi) {
                    const Phase &ph = bp->profile.phases[startIdx + wi];
                    ProfileHorizon::PhaseSpec &sp = phaseSpecs[wi];
                    sp.isBrewPhase = (ph.phase == PhaseType::PHASE_TYPE_BREW);
                    sp.valveOpen = (ph.valve > 0);
                    sp.isStandardProfile = isStandard;
                    sp.profileDoseG = profDose;
                    sp.pumpIsSimple = ph.pumpIsSimple;
                    sp.mode = ph.pumpIsSimple
                                  ? ProfileHorizon::Mode::SIMPLE
                                  : (ph.pumpAdvanced.target == PumpTarget::PUMP_TARGET_FLOW
                                         ? ProfileHorizon::Mode::FLOW
                                         : ProfileHorizon::Mode::PRESSURE);
                    sp.durationS = ph.duration;
                    sp.targetPressureBar = ph.pumpAdvanced.pressure;
                    sp.targetFlowMlS = ph.pumpAdvanced.flow;
                    sp.easing = mapEasing(ph.transition.type);
                    sp.transitionDurationS = ph.transition.duration;
                    sp.transitionAdaptive = ph.transition.adaptive;
                    sp.transitionTarget = mapTransTarget(ph.transition.target);
                    for (const Target &t : ph.targets) {
                        sp.addTarget(mapExitTargetType(t.type), mapExitTargetOp(t.operator_), t.value);
                    }
                }

                bool hasLaterBrewBeyondWindow = false;
                for (std::size_t idx = startIdx + windowCount; idx < totalPhases; ++idx) {
                    if (bp->profile.phases[idx].phase == PhaseType::PHASE_TYPE_BREW) {
                        hasLaterBrewBeyondWindow = true;
                        break;
                    }
                }
                inExtractionPhase = ProfileHorizon::isExtractionPhaseActive(
                    phaseSpecs.data(), windowCount, 0, curEffP, curEffQ, hasLaterBrewBeyondWindow);
            }
        }
    }

    if (!activeAdvancedBrew || windowCount == 0) {
        return true;
    }

    const float fallbackRefFlow = std::clamp(liveTargetYieldG_ / 22.0f, 0.8f, 3.5f);
    ProfileHorizon::HorizonSample horizon{};
    if (!ProfileHorizon::sampleHorizon(phaseSpecs.data(), windowCount, 0, currentPhaseElapsedS, phaseStartP,
                                       phaseStartQ, smp.pressure, smp.pumpFlow, fallbackRefFlow,
                                       workspace_->predictor.bevToPumpRatio(), workspace_->predictor.physics().dtS, 16,
                                       horizon, smp.weight, phaseStartWeightG, waterPumped, phaseStartPumpedMl,
                                       volumetricEnabled, /*hasExplicitCurrentEffective=*/true, curEffP, curEffQ)) {
        return true;
    }

    std::array<EspressoRL::PumpMode, EspressoRL::MAX_MPC_HORIZON> modes{};
    for (std::size_t i = 0; i < horizon.steps; ++i) {
        modes[i] = mapHorizonMode(horizon.modes[i]);
    }

    EspressoRL::HydraulicState st;
    st.elapsedS = static_cast<float>(smp.elapsedMs) / 1000.0f;
    st.pressureBar = smp.pressure;
    st.pumpFlowMlS = smp.pumpFlow;
    st.beverageFlowGS = bevFlow;
    st.waterPumpedMl = waterPumped;
    st.weightG = smp.weight;
    st.pumpMode = modes[0];
    st.liveOffset = workspace_->predictor.liveFilter().offset();
    st.liveRate = workspace_->predictor.liveFilter().rate();
    st.liveCov00 = workspace_->predictor.liveFilter().cov00();
    st.liveCov01 = workspace_->predictor.liveFilter().cov01();
    st.liveCov11 = workspace_->predictor.liveFilter().cov11();

    const bool readyForMpc =
        inExtractionPhase && ProfileHorizon::isHydraulicallyReadyForMpc(smp.valveOpen, smp.pressure, bevFlow, smp.weight);

    // Physics-induced nominal reference flow with two-timescale anti-conflict baseline latching:
    // whenever CPBO changed grind/dose, grinder wasn't confirmed pre-shot, cold-start, or Assist-Only,
    // latch steady early-brew conductance so MPC never cancels out intentional grind changes while still
    // rescuing mid-shot channeling surges & flow runaways.
    const float baselineOffset = workspace_->baselinePolicy.updateAndResolve(readyForMpc, st.liveOffset);

    std::array<float, EspressoRL::MAX_MPC_HORIZON> nominalRefFlow{};
    workspace_->predictor.predictNominalReferenceFlow(st, modes.data(), horizon.basePressureBar.data(),
                                                      horizon.baseFlowMlS.data(), horizon.steps, liveGrindSteps_,
                                                      liveDoseG_, baselineOffset, nominalRefFlow.data());

    const uint32_t t0Us = micros();
    const EspressoRL::ControlCommand cmd = workspace_->mpc.computeCommand(
        st, modes.data(), horizon.basePressureBar.data(), horizon.baseFlowMlS.data(), nominalRefFlow.data(),
        horizon.steps, workspace_->activeSplineDelta.data(), workspace_->activeSplineDelta.size(), liveGrindSteps_,
        liveDoseG_, liveTargetYieldG_, readyForMpc);
    workspace_->lastMpcFitUs = static_cast<uint32_t>(micros() - t0Us);

    ++liveSampleCount_;
    if (!cmd.fallbackUsed) {
        const bool shadow = shadowModeOnly_.load(std::memory_order_acquire);
        LiveBrewLeaseCommand leaseCmd;
        leaseCmd.mode = mapCoreToLeaseMode(cmd.pumpMode);
        leaseCmd.pressureTargetBar = cmd.pressureTargetBar;
        leaseCmd.flowTargetMlS = cmd.flowTargetMlS;
        leaseCmd.leaseDurationMs = LIVE_LEASE_DURATION_MS;
        leaseCmd.shadowOnly = shadow;
        controller_->applyLeasedLiveBrewTarget(leaseCmd, shadow);

        if ((liveSampleCount_ % 4u) == 0u) {
            ESP_LOGI(LOG_TAG, "[%s] t=%.1fs mode=%u baseP=%.2f mpcP=%.2f baseQ=%.2f mpcQ=%.2f kf=%.2f dt=%luus",
                     shadow ? "SHADOW" : "LIVE", st.elapsedS, static_cast<unsigned>(cmd.pumpMode),
                     horizon.basePressureBar[0], cmd.pressureTargetBar, horizon.baseFlowMlS[0], cmd.flowTargetMlS,
                     st.liveOffset, static_cast<unsigned long>(workspace_->lastMpcFitUs));

            LiveBrewLeaseDiagnostics leaseDiag{};
            {
                std::lock_guard<std::recursive_mutex> pLock(controller_->getProcessLock());
                Process *proc = controller_->getProcess();
                if (proc != nullptr && proc->getType() == MODE_BREW) {
                    leaseDiag = static_cast<BrewProcess *>(proc)->getLiveLeaseDiagnostics();
                }
            }
            AutoTuning::OnBoardRuntimeTelemetry tele;
            tele.ready = workspaceReady_.load(std::memory_order_acquire);
            tele.shadowModeOnly = shadow;
            tele.persistedStateLoaded = persistedStateLoaded_.load(std::memory_order_acquire);
            tele.persistenceAvailable = persistenceAvailable_.load(std::memory_order_acquire);
            tele.vfeSampleCount = workspace_->histZ.size();
            tele.cpboPointCount = workspace_->points.size();
            tele.cpboComparisonCount = workspace_->comparisons.size();
            tele.lastMpcFitUs = workspace_->lastMpcFitUs;
            tele.lastCpboFitMs = workspace_->lastCpboFitMs;
            tele.lastLiveActive = true;
            tele.lastLiveMode = static_cast<std::uint8_t>(cmd.pumpMode);
            tele.lastBasePressureBar = horizon.basePressureBar[0];
            tele.lastMpcPressureBar = cmd.pressureTargetBar;
            tele.lastBaseFlowMlS = horizon.baseFlowMlS[0];
            tele.lastMpcFlowMlS = cmd.flowTargetMlS;
            tele.lastKfOffset = st.liveOffset;
            tele.leaseEvaluatedCount = leaseDiag.evaluatedCount;
            tele.leaseShadowCount = leaseDiag.shadowCount;
            tele.leaseAppliedCount = leaseDiag.appliedCount;
            tele.leaseRejectedCount = leaseDiag.rejectedCount;
            tele.leaseExpiredCount = leaseDiag.expiredCount;
            {
                std::lock_guard<std::mutex> tLock(telemetryMutex_);
                cachedTelemetry_ = tele;
            }
            publishLiveTelemetryEvent(tele);
        }
    }
    return true;
}

bool OnBoardEspressoRLPlugin::onLiveShotEnded(AutoTuning::LiveShotEnded const &) {
    brewActive_.store(false, std::memory_order_release);
    LiveBrewLeaseDiagnostics leaseDiag{};
    if (controller_ != nullptr) {
        std::lock_guard<std::recursive_mutex> pLock(controller_->getProcessLock());
        Process *last = controller_->getLastProcess();
        if (last != nullptr && last->getType() == MODE_BREW) {
            auto *bp = static_cast<BrewProcess *>(last);
            bp->clearLiveControlLease();
            leaseDiag = bp->getLiveLeaseDiagnostics();
            if (leaseDiag.evaluatedCount > 0) {
                ESP_LOGI(LOG_TAG, "Shot lease summary: eval=%lu shadow=%lu applied=%lu rej=%lu exp=%lu",
                         static_cast<unsigned long>(leaseDiag.evaluatedCount),
                         static_cast<unsigned long>(leaseDiag.shadowCount),
                         static_cast<unsigned long>(leaseDiag.appliedCount),
                         static_cast<unsigned long>(leaseDiag.rejectedCount),
                         static_cast<unsigned long>(leaseDiag.expiredCount));
            }
        }
    }
    AutoTuning::OnBoardRuntimeTelemetry tele = runtimeTelemetry();
    tele.lastLiveActive = false;
    tele.leaseEvaluatedCount = leaseDiag.evaluatedCount;
    tele.leaseShadowCount = leaseDiag.shadowCount;
    tele.leaseAppliedCount = leaseDiag.appliedCount;
    tele.leaseRejectedCount = leaseDiag.rejectedCount;
    tele.leaseExpiredCount = leaseDiag.expiredCount;
    {
        std::lock_guard<std::mutex> tLock(telemetryMutex_);
        cachedTelemetry_ = tele;
    }
    publishLiveTelemetryEvent(tele);
    return true;
}

AutoTuning::ShotSubmissionResult OnBoardEspressoRLPlugin::submitShot(AutoTuning::ShotRecord const &shot,
                                                                     AutoTuning::ShotDeliveryAttempt const &attempt) {
    if (!ready() || !isOnBoardSelected()) {
        return {AutoTuning::ShotSubmissionOutcome::NotConnected, "on_board_optimizer_unavailable"};
    }
    auto workShot = makePsramUnique<PendingShotWork>();
    workShot->shotId = shot.shotId;
    workShot->machineId = shot.machineId;
    workShot->beanContextId = shot.recipe.beanContextId;
    workShot->grinderContextId = shot.recipe.grinder.contextId;
    workShot->profileId = shot.profile.id;
    workShot->profileLabel = shot.profile.label;
    workShot->rawProfileHash = shot.profile.rawProfileHash;
    workShot->tasteGoal = shot.recipe.tasteGoal;
    workShot->contextKey =
        workShot->beanContextId + "|" + workShot->grinderContextId + "|" + workShot->profileId;
    workShot->recordRevision = attempt.recordRevision;
    workShot->timestamp = shot.timestamp > 0 ? shot.timestamp : EpochTime::now();
    workShot->relativeGrindSteps = shot.recipe.grinder.relativeStepsFromReference.value_or(0.0f);
    workShot->hydraulicGrindSteps = canonicalFinerGrindSteps(shot.recipe.grinder);
    workShot->doseG = shot.measuredDoseG.value_or(shot.recipe.doseTargetG.value_or(18.0f));
    workShot->targetYieldG = shot.recipe.targetYieldG.value_or(36.0f);
    workShot->beverageOutG = shot.beverageOutG;
    workShot->shotTimeS = shot.shotTimeS;
    workShot->hasUsableDose = shot.hasUsableDose();
    workShot->excludeFromOptimization = shot.excludeFromLocalOptimization || shot.utility;
    workShot->samples.assign(shot.samples.begin(), shot.samples.end());

    WorkItem item;
    item.kind = WorkKind::IngestShot;
    item.shot = std::move(workShot);
    if (!enqueueWork(std::move(item))) {
        return {AutoTuning::ShotSubmissionOutcome::RetryableFailure, "on_board_queue_full"};
    }
    return {AutoTuning::ShotSubmissionOutcome::Submitted, "queued_on_board"};
}

void OnBoardEspressoRLPlugin::workerTaskEntry(void *arg) {
    static_cast<OnBoardEspressoRLPlugin *>(arg)->workerLoop();
}

void OnBoardEspressoRLPlugin::workerLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            WorkItem item;
            {
                std::lock_guard<std::mutex> lock(workQueueMutex_);
                if (workQueue_.empty()) {
                    break;
                }
                item = std::move(workQueue_.front());
                workQueue_.pop_front();
            }
            processWorkItem(item);
        }
    }
}

void OnBoardEspressoRLPlugin::processWorkItem(WorkItem &item) {
    switch (item.kind) {
    case WorkKind::LoadPersistedState:
        loadPersistedState();
        publishStatusEvent();
        break;
    case WorkKind::IngestShot:
        if (item.shot) {
            handleIngestShot(*item.shot);
        }
        break;
    case WorkKind::ProcessPreference:
        if (item.preference) {
            handleProcessPreference(*item.preference);
        }
        break;
    case WorkKind::ResetContext: {
        {
            std::lock_guard<std::mutex> lock(workspaceMutex_);
            if (workspace_) {
                workspace_->resetContext(activeContextKey(), "", "", "", "", "", AutoTuning::TasteGoal::balanced());
            }
        }
        clearPersistedState();
        publishStatusEvent();
        break;
    }
    case WorkKind::RefreshStatus:
        publishStatusEvent();
        break;
    }
}

AutoTuning::OnBoardRuntimeTelemetry OnBoardEspressoRLPlugin::runtimeTelemetry() const {
    AutoTuning::OnBoardRuntimeTelemetry tele;
    {
        std::lock_guard<std::mutex> tLock(telemetryMutex_);
        tele = cachedTelemetry_;
    }
    tele.ready = workspaceReady_.load(std::memory_order_acquire);
    tele.shadowModeOnly = shadowModeOnly_.load(std::memory_order_acquire);
    tele.persistedStateLoaded = persistedStateLoaded_.load(std::memory_order_acquire);
    tele.persistenceAvailable = persistenceAvailable_.load(std::memory_order_acquire);
    std::unique_lock<std::mutex> wLock(workspaceMutex_, std::try_to_lock);
    if (wLock.owns_lock() && workspace_) {
        tele.vfeSampleCount = workspace_->histZ.size();
        tele.cpboPointCount = workspace_->points.size();
        tele.cpboComparisonCount = workspace_->comparisons.size();
        tele.lastMpcFitUs = workspace_->lastMpcFitUs;
        tele.lastCpboFitMs = workspace_->lastCpboFitMs;
    }
    return tele;
}

void OnBoardEspressoRLPlugin::loadPersistedState() {
    if (!workspace_) {
        return;
    }
    std::vector<std::uint8_t> raw;
    bool filePresent = false;
    {
        auto flashLease = StorageCoordinator::instance().acquireFlash();
        const bool dirReady = LocalAutoTuningFiles::ensureDirectory(PERSIST_DIR);
        persistenceAvailable_.store(dirReady, std::memory_order_release);
        if (!dirReady) {
            return;
        }
        const String tmpPath = AtomicFile::temporaryPath(PERSIST_STATE_PATH);
        if (LittleFSUtil::existsQuietly(tmpPath)) {
            bool tmpValid = false;
            File tf = LittleFS.open(tmpPath, FILE_READ);
            if (tf) {
                const size_t sz = tf.size();
                if (sz >= OnBoardStateCodec::HEADER_SIZE && sz <= OnBoardStateCodec::MAX_ENCODED_BYTES) {
                    std::vector<std::uint8_t> tmpBuf(sz);
                    if (tf.read(tmpBuf.data(), sz) == sz) {
                        OnBoardStateCodec::PersistedState check;
                        tmpValid = OnBoardStateCodec::decode(tmpBuf.data(), tmpBuf.size(), check);
                    }
                }
                tf.close();
            }
            AtomicFile::recoverPending(PERSIST_STATE_PATH, tmpValid);
        }
        if (LittleFSUtil::existsQuietly(PERSIST_STATE_PATH)) {
            File f = LittleFS.open(PERSIST_STATE_PATH, FILE_READ);
            if (f) {
                const size_t sz = f.size();
                if (sz >= OnBoardStateCodec::HEADER_SIZE && sz <= OnBoardStateCodec::MAX_ENCODED_BYTES) {
                    raw.resize(sz);
                    filePresent = (f.read(raw.data(), sz) == sz);
                }
                f.close();
            }
        }
    }

    if (!filePresent || raw.empty()) {
        return;
    }
    OnBoardStateCodec::PersistedState st;
    if (!OnBoardStateCodec::decode(raw.data(), raw.size(), st)) {
        ESP_LOGW(LOG_TAG, "Persisted on-board state failed CRC/schema validation; starting fresh");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(workspaceMutex_);
        workspace_->activeContextKey = st.activeContextKey;
        workspace_->optimizationRunId = st.optimizationRunId;
        workspace_->activeBeanContextId = st.activeBeanContextId;
        workspace_->activeGrinderContextId = st.activeGrinderContextId;
        workspace_->activeProfileId = st.activeProfileId;
        workspace_->activeProfileLabel = st.activeProfileLabel;
        workspace_->activeRawProfileHash = st.activeRawProfileHash;

        AutoTuning::TasteGoal goal;
        goal.mode = (st.tasteGoalMode == 1u) ? AutoTuning::TasteGoalMode::Custom : AutoTuning::TasteGoalMode::Balanced;
        for (std::size_t i = 0; i < AutoTuning::TASTE_GOAL_ATTRIBUTE_COUNT; ++i) {
            goal.targets[i] = static_cast<AutoTuning::TasteLevel>(st.tasteGoalTargets[i]);
        }
        workspace_->activeTasteGoal = goal.valid() ? goal : AutoTuning::TasteGoal::balanced();

        workspace_->predictor.resetAll();
        workspace_->predictor.setBevToPumpRatio(st.bevToPumpRatio);
        workspace_->hasLastShotRecipe = st.hasLastShotRecipe;
        workspace_->lastShotHydraulicGrindSteps = st.lastShotHydraulicGrindSteps;
        workspace_->lastShotDoseG = st.lastShotDoseG;
        workspace_->activeSplineDelta = st.activeSplineDelta;

        workspace_->trustRegion.center = {st.trustCenter[0], st.trustCenter[1], st.trustCenter[2]};
        workspace_->trustRegion.length = st.trustLength;
        workspace_->trustRegion.successCount = st.trustSuccesses;
        workspace_->trustRegion.failureCount = st.trustFailures;
        workspace_->trustRegion.locallyConverged = st.trustConverged;
        workspace_->trustRegion.lastAction = static_cast<EspressoRL::TrustRegionAction>(st.trustLastAction);

        workspace_->hasBestPoint = st.hasBestPoint;
        workspace_->bestPointIndex = st.bestPointIndex;
        workspace_->localShotCount = st.localShotCount;
        workspace_->lastShotId = st.lastShotId;
        workspace_->lastShotAt = st.lastShotAt;
        workspace_->lastShotTimeS = st.lastShotTimeS;
        workspace_->lastShotBeverageOutG = st.lastShotBeverageOutG;
        workspace_->lastShotTargetYieldG = st.lastShotTargetYieldG;
        workspace_->lastRecommendationId = st.lastRecommendationId;
        workspace_->lastRecommendationAt = st.lastRecommendationAt;

        workspace_->histZ.assign(st.histZ.begin(), st.histZ.end());
        workspace_->histY.assign(st.histY.begin(), st.histY.end());
        workspace_->points.clear();
        workspace_->points.reserve(st.points.size());
        for (const auto &sp : st.points) {
            Workspace::CpboPoint pt;
            pt.shotId = sp.shotId;
            pt.grindSteps = sp.grindSteps;
            pt.doseG = sp.doseG;
            pt.targetYieldG = sp.targetYieldG;
            pt.beverageOutG = sp.beverageOutG;
            pt.normX = sp.normX;
            pt.phi0 = sp.phi0;
            workspace_->points.push_back(std::move(pt));
        }
        workspace_->comparisons.clear();
        workspace_->comparisons.reserve(st.comparisons.size());
        for (const auto &sc : st.comparisons) {
            workspace_->comparisons.push_back(EspressoRL::ComparisonPair{
                sc.newIndex, sc.anchorIndex, static_cast<EspressoRL::PreferenceLabel>(sc.label)});
        }

        if (!workspace_->histZ.empty()) {
            const float noiseStd = workspace_->predictor.physics().obsLogConductanceNoiseStd;
            workspace_->predictor.residualGP().refreshFromHistory(
                workspace_->histZ.data(), workspace_->histY.data(), workspace_->histZ.size(), noiseStd * noiseStd);
        }
    }

    persistedStateLoaded_.store(true, std::memory_order_release);
    ESP_LOGI(LOG_TAG, "Restored persisted on-board state (%u VFE samples, %u CPBO points, %u comparisons)",
             static_cast<unsigned>(st.histZ.size()), static_cast<unsigned>(st.points.size()),
             static_cast<unsigned>(st.comparisons.size()));
}

bool OnBoardEspressoRLPlugin::savePersistedState() {
    if (!workspace_ || brewActive_.load(std::memory_order_acquire) || otaActive_.load(std::memory_order_acquire)) {
        return false;
    }

    OnBoardStateCodec::PersistedState st;
    {
        std::lock_guard<std::mutex> lock(workspaceMutex_);
        st.activeContextKey = workspace_->activeContextKey;
        st.optimizationRunId = workspace_->optimizationRunId;
        st.activeBeanContextId = workspace_->activeBeanContextId;
        st.activeGrinderContextId = workspace_->activeGrinderContextId;
        st.activeProfileId = workspace_->activeProfileId;
        st.activeProfileLabel = workspace_->activeProfileLabel;
        st.activeRawProfileHash = workspace_->activeRawProfileHash;

        st.tasteGoalMode = (workspace_->activeTasteGoal.mode == AutoTuning::TasteGoalMode::Custom) ? 1u : 0u;
        for (std::size_t i = 0; i < AutoTuning::TASTE_GOAL_ATTRIBUTE_COUNT; ++i) {
            st.tasteGoalTargets[i] = static_cast<std::uint8_t>(workspace_->activeTasteGoal.targets[i]);
        }

        st.bevToPumpRatio = workspace_->predictor.bevToPumpRatio();
        st.hasLastShotRecipe = workspace_->hasLastShotRecipe;
        st.lastShotHydraulicGrindSteps = workspace_->lastShotHydraulicGrindSteps;
        st.lastShotDoseG = workspace_->lastShotDoseG;
        st.activeSplineDelta = workspace_->activeSplineDelta;

        if (workspace_->trustRegion.center.size() >= 3) {
            st.trustCenter = {workspace_->trustRegion.center[0], workspace_->trustRegion.center[1],
                              workspace_->trustRegion.center[2]};
        }
        st.trustLength = workspace_->trustRegion.length;
        st.trustSuccesses = workspace_->trustRegion.successCount;
        st.trustFailures = workspace_->trustRegion.failureCount;
        st.trustConverged = workspace_->trustRegion.locallyConverged;
        st.trustLastAction = static_cast<std::uint8_t>(workspace_->trustRegion.lastAction);

        st.hasBestPoint = workspace_->hasBestPoint && workspace_->bestPointIndex < workspace_->points.size();
        st.bestPointIndex = st.hasBestPoint ? static_cast<std::uint16_t>(workspace_->bestPointIndex) : 0u;
        st.localShotCount = workspace_->localShotCount;
        st.lastShotId = workspace_->lastShotId;
        st.lastShotAt = workspace_->lastShotAt;
        st.lastShotTimeS = workspace_->lastShotTimeS;
        st.lastShotBeverageOutG = workspace_->lastShotBeverageOutG;
        st.lastShotTargetYieldG = workspace_->lastShotTargetYieldG;
        st.lastRecommendationId = workspace_->lastRecommendationId;
        st.lastRecommendationAt = workspace_->lastRecommendationAt;

        st.histZ.assign(workspace_->histZ.begin(), workspace_->histZ.end());
        st.histY.assign(workspace_->histY.begin(), workspace_->histY.end());
        st.points.reserve(workspace_->points.size());
        for (const auto &pt : workspace_->points) {
            OnBoardStateCodec::PersistedPoint sp;
            sp.shotId = pt.shotId;
            sp.grindSteps = pt.grindSteps;
            sp.doseG = pt.doseG;
            sp.targetYieldG = pt.targetYieldG;
            sp.beverageOutG = pt.beverageOutG;
            sp.normX = pt.normX;
            sp.phi0 = pt.phi0;
            st.points.push_back(std::move(sp));
        }
        st.comparisons.reserve(workspace_->comparisons.size());
        for (const auto &cmp : workspace_->comparisons) {
            st.comparisons.push_back(OnBoardStateCodec::PersistedComparison{
                static_cast<std::uint16_t>(cmp.newIdx), static_cast<std::uint16_t>(cmp.anchorIdx),
                static_cast<std::uint8_t>(cmp.label)});
        }
    }

    std::vector<std::uint8_t> bytes;
    if (!OnBoardStateCodec::encode(st, bytes)) {
        ESP_LOGW(LOG_TAG, "Failed to encode on-board state for persistence");
        return false;
    }

    bool ok = false;
    {
        auto flashLease = StorageCoordinator::instance().acquireFlash();
        if (LocalAutoTuningFiles::ensureDirectory(PERSIST_DIR)) {
            const String tmpPath = AtomicFile::temporaryPath(PERSIST_STATE_PATH);
            File f = LittleFS.open(tmpPath, FILE_WRITE);
            if (f) {
                const size_t written = f.write(bytes.data(), bytes.size());
                f.flush();
                f.close();
                if (written == bytes.size()) {
                    ok = AtomicFile::commit(PERSIST_STATE_PATH);
                } else {
                    LittleFS.remove(tmpPath);
                }
            }
        }
    }
    persistenceAvailable_.store(ok, std::memory_order_release);
    if (ok) {
        persistedStateLoaded_.store(true, std::memory_order_release);
    }
    return ok;
}

void OnBoardEspressoRLPlugin::clearPersistedState() {
    auto flashLease = StorageCoordinator::instance().acquireFlash();
    const String tmpPath = AtomicFile::temporaryPath(PERSIST_STATE_PATH);
    const String bakPath = AtomicFile::backupPath(PERSIST_STATE_PATH);
    if (LittleFSUtil::existsQuietly(PERSIST_STATE_PATH)) {
        LittleFS.remove(PERSIST_STATE_PATH);
    }
    if (LittleFSUtil::existsQuietly(tmpPath)) {
        LittleFS.remove(tmpPath);
    }
    if (LittleFSUtil::existsQuietly(bakPath)) {
        LittleFS.remove(bakPath);
    }
    persistedStateLoaded_.store(false, std::memory_order_release);
}

void OnBoardEspressoRLPlugin::publishLiveTelemetryEvent(const AutoTuning::OnBoardRuntimeTelemetry &t) {
    if (!isOnBoardSelected()) {
        return;
    }
    Event ev;
    ev.id = "rl:onboard:live";
    ev.setInt("shadow_only", t.shadowModeOnly ? 1 : 0);
    ev.setInt("active", t.lastLiveActive ? 1 : 0);
    ev.setInt("mode", static_cast<int>(t.lastLiveMode));
    ev.setFloat("base_p", t.lastBasePressureBar);
    ev.setFloat("mpc_p", t.lastMpcPressureBar);
    ev.setFloat("base_q", t.lastBaseFlowMlS);
    ev.setFloat("mpc_q", t.lastMpcFlowMlS);
    ev.setFloat("kf_offset", t.lastKfOffset);
    ev.setInt("mpc_us", static_cast<int>(t.lastMpcFitUs));
    ev.setInt("eval_count", static_cast<int>(t.leaseEvaluatedCount));
    ev.setInt("shadow_count", static_cast<int>(t.leaseShadowCount));
    ev.setInt("applied_count", static_cast<int>(t.leaseAppliedCount));
    ev.setInt("rejected_count", static_cast<int>(t.leaseRejectedCount));
    ev.setInt("expired_count", static_cast<int>(t.leaseExpiredCount));
    queueMainThreadEvent(std::move(ev));
}

void OnBoardEspressoRLPlugin::handleIngestShot(PendingShotWork const &shot) {
    if (!workspace_ || controller_ == nullptr) {
        return;
    }
    std::optional<AutoTuning::PreferenceRequest> prefReq;

    {
        std::lock_guard<std::mutex> lock(workspaceMutex_);
        if (workspace_->activeContextKey != shot.contextKey) {
            workspace_->resetContext(shot.contextKey, shot.beanContextId, shot.grinderContextId, shot.profileId,
                                     shot.profileLabel, shot.rawProfileHash, shot.tasteGoal);
        }

        workspace_->lastShotId = shot.shotId;
        workspace_->lastShotAt = shot.timestamp;
        workspace_->lastShotTimeS = shot.shotTimeS;
        workspace_->lastShotBeverageOutG = shot.beverageOutG;
        workspace_->lastShotTargetYieldG = shot.targetYieldG;
        workspace_->hasLastShotRecipe = true;
        workspace_->lastShotHydraulicGrindSteps = shot.hydraulicGrindSteps;
        workspace_->lastShotDoseG = shot.doseG;

        // 1. Update hydraulic VFE Nyström GP + bev/pump ratio from shot telemetry.
        if (!shot.samples.empty() && !shot.excludeFromOptimization) {
            const std::size_t n = shot.samples.size();
            std::vector<float, PsramStlAllocator<float>> qPump(n), qBev(n);
            std::vector<uint8_t, PsramStlAllocator<uint8_t>> mask(n, 0);
            const float mPhys = EspressoRL::physicsPriorLogConductance(shot.hydraulicGrindSteps, shot.doseG,
                                                                       workspace_->predictor.physics());
            float maxWeightSeen = 0.0f;
            for (std::size_t i = 0; i < n; ++i) {
                const auto &s = shot.samples[i];
                const float bevFlow = selectBeverageFlowGS(s);
                qPump[i] = s.pumpFlow;
                qBev[i] = bevFlow;
                const bool isExtendedRec = (s.systemInfo & SYSTEM_INFO_EXTENDED_RECORDING) != 0;
                const bool cupRemoved = std::isfinite(s.weight) && (s.weight + 1.0f < maxWeightSeen);
                if (std::isfinite(s.weight) && s.weight > maxWeightSeen) {
                    maxWeightSeen = s.weight;
                }
                const bool pumpActive = s.targetPressure > 0.1f || s.targetFlow > 0.1f || s.pumpFlow > 0.15f;
                const bool active = !isExtendedRec && !cupRemoved && s.valveOpen && pumpActive && s.pressure >= 1.5f &&
                                    bevFlow >= 0.20f && s.weight >= 0.5f;
                mask[i] = active ? 1u : 0u;
                if (active) {
                    const float obsEll = std::log(std::max(bevFlow, 0.05f) / std::sqrt(std::max(s.pressure, 0.1f)));
                    const float resid = std::clamp(obsEll - mPhys, -2.0f, 2.0f);
                    const float waterMl = s.waterPumped.value_or(0.0f);
                    if (workspace_->histZ.size() >= MAX_HYDRAULIC_HISTORY) {
                        workspace_->histZ.erase(workspace_->histZ.begin());
                        workspace_->histY.erase(workspace_->histY.begin());
                    }
                    workspace_->histZ.push_back(
                        EspressoRL::buildHydraulicFeature(waterMl, s.pressure, shot.hydraulicGrindSteps, shot.doseG));
                    workspace_->histY.push_back(resid);
                }
            }
            workspace_->predictor.updateBevToPumpRatio(qPump.data(), qBev.data(), mask.data(), n);
            if (!workspace_->histZ.empty()) {
                const float noiseStd = workspace_->predictor.physics().obsLogConductanceNoiseStd;
                workspace_->predictor.residualGP().refreshFromHistory(
                    workspace_->histZ.data(), workspace_->histY.data(), workspace_->histZ.size(), noiseStd * noiseStd);
            }
        }

        // 2. Register recipe point for CPBO and construct a pairwise preference prompt against incumbent anchor.
        if (!shot.excludeFromOptimization && shot.hasUsableDose && workspace_->findPointByShotId(shot.shotId) < 0) {
            const AutoTuning::RecipeDomain domain = controller_->getSettings().getRLRecipeDomain();
            const double fineness = std::clamp(
                (static_cast<double>(shot.relativeGrindSteps) + domain.grindRadiusSteps) /
                    std::max(2.0 * domain.grindRadiusSteps, 1e-6),
                0.0, 1.0);
            const double doseNorm = std::clamp(
                (static_cast<double>(shot.doseG) - static_cast<double>(domain.doseMinG)) /
                    std::max<double>(static_cast<double>(domain.doseMaxG - domain.doseMinG), 1e-6),
                0.0, 1.0);
            const double yieldNorm = std::clamp(
                (static_cast<double>(shot.targetYieldG) - static_cast<double>(domain.targetOutputMinG)) /
                    std::max<double>(static_cast<double>(domain.targetOutputMaxG - domain.targetOutputMinG), 1e-6),
                0.0, 1.0);

            Workspace::CpboPoint pt;
            pt.shotId = shot.shotId;
            pt.grindSteps = shot.relativeGrindSteps;
            pt.doseG = shot.doseG;
            pt.targetYieldG = shot.targetYieldG;
            pt.beverageOutG = shot.beverageOutG;
            pt.normX = {fineness, doseNorm, yieldNorm};
            pt.phi0 = EspressoRL::computePhi0(fineness, shot.doseG, shot.targetYieldG);

            if (workspace_->points.size() >= MAX_CPBO_POINTS) {
                workspace_->points.erase(workspace_->points.begin());
                if (workspace_->bestPointIndex > 0) {
                    --workspace_->bestPointIndex;
                }
            }
            const std::size_t newIdx = workspace_->points.size();
            workspace_->points.push_back(pt);
            workspace_->localShotCount += 1;

            if (!workspace_->hasBestPoint) {
                workspace_->bestPointIndex = newIdx;
                workspace_->hasBestPoint = true;
                workspace_->trustRegion.center = {fineness, doseNorm, yieldNorm};
            } else if (workspace_->bestPointIndex < workspace_->points.size() && workspace_->bestPointIndex != newIdx) {
                const auto &anchor = workspace_->points[workspace_->bestPointIndex];
                AutoTuning::PreferenceRequest req;
                req.recommendationId = workspace_->lastRecommendationId;
                req.installId = "on_board_esp32s3";
                req.optimizationRunId = workspace_->optimizationRunId;
                req.newShotId = shot.shotId;
                req.anchorShotId = anchor.shotId;
                req.comparisonMode = AutoTuning::ComparisonMode::BestIncumbent;
                req.tasteGoal = workspace_->activeTasteGoal;
                AutoTuning::PreferenceAnchorSummary ancSum;
                ancSum.timestamp = workspace_->lastShotAt;
                ancSum.relativeGrindSteps = static_cast<float>(anchor.grindSteps);
                ancSum.doseG = static_cast<float>(anchor.doseG);
                ancSum.targetYieldG = static_cast<float>(anchor.targetYieldG);
                ancSum.beverageOutG = static_cast<float>(anchor.beverageOutG);
                ancSum.profileLabel = workspace_->activeProfileLabel;
                req.anchor = ancSum;
                if (req.valid()) {
                    prefReq = std::move(req);
                }
            }
        }
    }

    // Emit canonical shot delivery acknowledgement so LocalAutoTuningStore transitions cleanly.
    AutoTuning::ShotDeliveryAcknowledgement ack;
    ack.shotId = shot.shotId;
    ack.machineId = shot.machineId.empty() ? machineId() : shot.machineId;
    ack.recordRevision = shot.recordRevision;
    ack.outcome = "accepted";
    ack.reason = "ingested_on_board";
    ack.timestamp = std::max<AutoTuning::Timestamp>(EpochTime::now(), EpochTime::MIN_VALID);
    ack.preferenceRequest = prefReq;

    Event ackEv;
    ackEv.id = "rl:shot:delivery:ack";
    ackEv.setString("shot_id", ack.shotId.c_str());
    ackEv.setString("outcome", ack.outcome.c_str());
    ackEv.setString("reason", ack.reason.c_str());
    ackEv.setInt("retryable", 0);
    ackEv.setInt64("timestamp", ack.timestamp);
    ackEv.setPayload(std::move(ack));
    queueMainThreadEvent(std::move(ackEv));
    savePersistedState();
    publishStatusEvent();
}

void OnBoardEspressoRLPlugin::handleProcessPreference(AutoTuning::PreferenceFeedback const &feedback) {
    if (!workspace_ || controller_ == nullptr) {
        return;
    }

    // Snapshot domain & copy CPBO data under lock, then run heavy float64 math cooperatively on Core 1.
    const AutoTuning::RecipeDomain domain = controller_->getSettings().getRLRecipeDomain();
    std::vector<Workspace::CpboPoint> pointsCopy;
    std::vector<EspressoRL::ComparisonPair> compsCopy;
    EspressoRL::Level1TrustRegionState trState;
    std::size_t anchorIdx = 0;
    std::string runId, beanId, grinderId, profileId, rawHash, sourceShotId;
    AutoTuning::TasteGoal goal;

    {
        std::lock_guard<std::mutex> lock(workspaceMutex_);
        const int newIdx = workspace_->findPointByShotId(feedback.newShotId);
        const int ancIdx = workspace_->findPointByShotId(feedback.anchorShotId);
        if (newIdx < 0 || ancIdx < 0 || newIdx == ancIdx) {
            ESP_LOGW(LOG_TAG, "Skipping preference comparison: shot not found in active on-board run");
            return;
        }
        const EspressoRL::PreferenceLabel lbl = mapPreferenceLabel(feedback.label);
        if (workspace_->comparisons.size() >= MAX_CPBO_COMPARISONS) {
            workspace_->comparisons.erase(workspace_->comparisons.begin());
        }
        workspace_->comparisons.push_back(
            EspressoRL::ComparisonPair{static_cast<std::size_t>(newIdx), static_cast<std::size_t>(ancIdx), lbl});

        if (lbl == EspressoRL::PreferenceLabel::NEW_BETTER) {
            workspace_->bestPointIndex = static_cast<std::size_t>(newIdx);
            workspace_->hasBestPoint = true;
        }

        const EspressoRL::TrustRegionConfig trCfg;
        EspressoRL::Level1TrustRegionState nextTr;
        const auto &newPt = workspace_->points[static_cast<std::size_t>(newIdx)];
        if (EspressoRL::updateLevel1TrustRegion(workspace_->trustRegion, lbl,
                                                {newPt.normX[0], newPt.normX[1], newPt.normX[2]}, trCfg, nextTr)) {
            workspace_->trustRegion = nextTr;
        }

        pointsCopy = workspace_->points;
        compsCopy = workspace_->comparisons;
        trState = workspace_->trustRegion;
        anchorIdx = workspace_->bestPointIndex;
        runId = workspace_->optimizationRunId;
        beanId = workspace_->activeBeanContextId;
        grinderId = workspace_->activeGrinderContextId;
        profileId = workspace_->activeProfileId;
        rawHash = workspace_->activeRawProfileHash;
        sourceShotId = feedback.newShotId;
        goal = workspace_->activeTasteGoal;
    }

    if (pointsCopy.size() < 2 || compsCopy.empty()) {
        return;
    }

    // Build robust physics scaler over evaluated points + deterministic 3x3x3 domain grid.
    std::vector<double> phiGrid;
    phiGrid.reserve((pointsCopy.size() + 27) * EspressoRL::PHYSICS_PROXY_DIM);
    for (const auto &pt : pointsCopy) {
        phiGrid.insert(phiGrid.end(), pt.phi0.begin(), pt.phi0.end());
    }
    for (double f : {0.0, 0.5, 1.0}) {
        for (double d : {0.0, 0.5, 1.0}) {
            for (double o : {0.0, 0.5, 1.0}) {
                const double dose = domain.doseMinG + d * (domain.doseMaxG - domain.doseMinG);
                const double yield = domain.targetOutputMinG + o * (domain.targetOutputMaxG - domain.targetOutputMinG);
                const auto p0 = EspressoRL::computePhi0(f, dose, yield);
                phiGrid.insert(phiGrid.end(), p0.begin(), p0.end());
            }
        }
    }
    EspressoRL::EmbeddedRobustScaler scaler;
    if (!scaler.fit(phiGrid.data(), phiGrid.size() / EspressoRL::PHYSICS_PROXY_DIM, EspressoRL::PHYSICS_PROXY_DIM)) {
        return;
    }

    constexpr std::size_t rawDim = 3;
    constexpr std::size_t physDim = EspressoRL::PHYSICS_PROXY_DIM;
    constexpr std::size_t stride = rawDim + physDim;

    std::vector<double> trainInputs(pointsCopy.size() * stride, 0.0);
    for (std::size_t i = 0; i < pointsCopy.size(); ++i) {
        double *row = trainInputs.data() + i * stride;
        row[0] = pointsCopy[i].normX[0];
        row[1] = pointsCopy[i].normX[1];
        row[2] = pointsCopy[i].normX[2];
        scaler.transformRow(pointsCopy[i].phi0.data(), row + rawDim);
    }

    YieldContext yCtx;
    yCtx.brewActive = &brewActive_;
    yCtx.otaActive = &otaActive_;
    yCtx.startedAtMs = millis();
    yCtx.lastYieldMs = yCtx.startedAtMs;

    EspressoRL::PreferenceGPConfig gpCfg;
    gpCfg.maxOuterIters = 35;
    EspressoRL::EmbeddedPreferenceGPModel model;
    const bool fitOk = EspressoRL::fitEmbeddedPreferenceGP(trainInputs.data(), pointsCopy.size(), rawDim, physDim, 0,
                                                           compsCopy.data(), compsCopy.size(), gpCfg, model,
                                                           cooperativeYieldCallback, &yCtx);
    if (!fitOk || model.cancelled) {
        std::lock_guard<std::mutex> lock(workspaceMutex_);
        if (workspace_) {
            workspace_->lastCpboCancelled = model.cancelled;
        }
        ESP_LOGW(LOG_TAG, "On-board CPBO fit %s", model.cancelled ? "cooperatively aborted" : "failed");
        publishStatusEvent();
        return;
    }

    // Compute lengthscale-shaped trust-region bounds & deterministic candidate discretization (64 points + anchor).
    std::vector<double> lo(3, 0.0), hi(3, 1.0);
    if (!trState.locallyConverged) {
        const EspressoRL::TrustRegionConfig trCfg;
        EspressoRL::computeLevel1TrustRegionBounds(trState, model.hyperparams.rawLengthscales(), trCfg, lo, hi);
    }

    std::vector<std::array<double, 3>> cands;
    cands.reserve(65);
    cands.push_back(pointsCopy[anchorIdx].normX); // index 0 = anchor
    for (int i0 = 0; i0 < 4; ++i0) {
        for (int i1 = 0; i1 < 4; ++i1) {
            for (int i2 = 0; i2 < 4; ++i2) {
                const double u0 = lo[0] + (static_cast<double>(i0) + 0.5) * 0.25 * (hi[0] - lo[0]);
                const double u1 = lo[1] + (static_cast<double>(i1) + 0.5) * 0.25 * (hi[1] - lo[1]);
                const double u2 = lo[2] + (static_cast<double>(i2) + 0.5) * 0.25 * (hi[2] - lo[2]);
                cands.push_back({u0, u1, u2});
            }
        }
    }

    std::vector<double> queryInputs(cands.size() * stride, 0.0);
    for (std::size_t i = 0; i < cands.size(); ++i) {
        double *row = queryInputs.data() + i * stride;
        const double dose = quantizeStep(domain.doseMinG + cands[i][1] * (domain.doseMaxG - domain.doseMinG),
                                         domain.doseMinG, domain.doseMaxG, 0.1);
        const double yield =
            quantizeStep(domain.targetOutputMinG + cands[i][2] * (domain.targetOutputMaxG - domain.targetOutputMinG),
                         domain.targetOutputMinG, domain.targetOutputMaxG, 0.5);
        const auto p0 = EspressoRL::computePhi0(cands[i][0], dose, yield);
        row[0] = cands[i][0];
        row[1] = cands[i][1];
        row[2] = cands[i][2];
        scaler.transformRow(p0.data(), row + rawDim);
    }

    std::vector<double> postMean, postCov;
    if (!model.predict(queryInputs.data(), cands.size(), postMean, postCov)) {
        return;
    }

    std::vector<std::size_t> candIndices;
    candIndices.reserve(cands.size() - 1);
    for (std::size_t i = 1; i < cands.size(); ++i) {
        candIndices.push_back(i);
    }

    EspressoRL::MESConfig mesCfg;
    mesCfg.posteriorMaxFunctionSamples = 256;
    mesCfg.gumbelMaximumSamples = 1000;
    mesCfg.maximumValueBins = 8;
    EspressoRL::EmbeddedMESResult mes;
    if (!EspressoRL::evaluateEmbeddedCpboMes(postMean.data(), postCov.data(), cands.size(), candIndices.data(),
                                             candIndices.size(), nullptr, 0, 0, model.gamma, model.sigmaPref, mesCfg,
                                             static_cast<uint64_t>(compsCopy.size() + 17), mes,
                                             cooperativeYieldCallback, &yCtx) ||
        mes.cancelled) {
        std::lock_guard<std::mutex> lock(workspaceMutex_);
        if (workspace_) {
            workspace_->lastCpboCancelled = mes.cancelled;
        }
        publishStatusEvent();
        return;
    }

    const uint32_t fitMs = static_cast<uint32_t>(millis() - yCtx.startedAtMs);
    const auto &chosen = cands[mes.candidateIndex];
    const auto &anchorPt = pointsCopy[anchorIdx];

    const double nextGrindRel =
        quantizeStep(-domain.grindRadiusSteps + chosen[0] * (2.0 * domain.grindRadiusSteps), -domain.grindRadiusSteps,
                     domain.grindRadiusSteps, 0.5);
    const double nextDoseG = quantizeStep(domain.doseMinG + chosen[1] * (domain.doseMaxG - domain.doseMinG),
                                          domain.doseMinG, domain.doseMaxG, 0.1);
    const double nextYieldG =
        quantizeStep(domain.targetOutputMinG + chosen[2] * (domain.targetOutputMaxG - domain.targetOutputMinG),
                     domain.targetOutputMinG, domain.targetOutputMaxG, 0.5);

    AutoTuning::RecommendationTargets targetCheck;
    targetCheck.nextDoseG = static_cast<float>(nextDoseG);
    targetCheck.targetYieldG = static_cast<float>(nextYieldG);
    targetCheck.targetRatio = static_cast<float>(nextYieldG / std::max(nextDoseG, 0.1));
    targetCheck.grindDeltaStepsFromCurrent = static_cast<float>(nextGrindRel - anchorPt.grindSteps);
    std::string rejectReason;
    if (!AutoTuning::validateRecommendationTargets(targetCheck, domain, rejectReason)) {
        ESP_LOGW(LOG_TAG, "On-board CPBO recommendation rejected by safety validator: %s", rejectReason.c_str());
        return;
    }

    const AutoTuning::Timestamp nowSec = std::max<AutoTuning::Timestamp>(EpochTime::now(), EpochTime::MIN_VALID);
    AutoTuning::Recommendation rec;
    rec.recommendationId = makeRecommendationId();
    rec.sourceShotId = sourceShotId;
    rec.installId = "on_board_esp32s3";
    rec.machineId = machineId();
    rec.beanContextId = beanId;
    rec.grinderContextId = grinderId;
    rec.profileId = profileId;
    rec.rawProfileHash = rawHash;
    rec.tasteGoal = goal;
    rec.createdAt = nowSec;
    rec.updatedAt = nowSec;
    rec.mode = AutoTuning::RecommendationMode::CpboBestIncumbent;
    rec.status = AutoTuning::RecommendationStatus::Pending;
    rec.reason = shadowModeOnly_.load() ? "on_board_cpbo_ghvi_shadow" : "on_board_cpbo_ghvi_live";
    rec.optimizationRunId = runId;
    rec.comparisonAnchorShotId = anchorPt.shotId;
    rec.comparisonMode = AutoTuning::ComparisonMode::BestIncumbent;
    rec.preferenceFeedbackRequired = true;
    rec.grindDeltaStepsFromCurrent = targetCheck.grindDeltaStepsFromCurrent;
    rec.projectedRelativeStepFromReference = static_cast<float>(nextGrindRel);
    rec.nextDoseG = targetCheck.nextDoseG;
    rec.targetYieldG = targetCheck.targetYieldG;
    rec.targetRatio = targetCheck.targetRatio;
    rec.confidence = static_cast<float>(std::clamp(mes.outcomeProbabilities[0], 0.0, 1.0));

    {
        std::lock_guard<std::mutex> lock(workspaceMutex_);
        if (workspace_) {
            workspace_->lastModel = std::move(model);
            workspace_->lastRecommendationId = rec.recommendationId;
            workspace_->lastRecommendationAt = nowSec;
            workspace_->lastCpboFitMs = fitMs;
            workspace_->lastCpboCancelled = false;
        }
    }

    ESP_LOGI(LOG_TAG, "On-board CPBO fit+MES completed in %lu ms (ELBO=%.3f, IG=%.4f, dGrind=%.2f, dose=%.1f, yield=%.1f)",
             static_cast<unsigned long>(fitMs), workspace_->lastModel.elbo, mes.acquisitionValue,
             rec.grindDeltaStepsFromCurrent, rec.nextDoseG, rec.targetYieldG);

    if (auto *store = controller_->getAutoTuningRecordStore()) {
        store->storeRecommendation(rec);
    }

    Event recEv;
    recEv.id = "rl:recommendation:received";
    recEv.setPayload(std::move(rec));
    queueMainThreadEvent(std::move(recEv));
    savePersistedState();
    publishStatusEvent();
}

void OnBoardEspressoRLPlugin::publishStatusEvent() {
    if (!workspace_ || !isOnBoardSelected()) {
        return;
    }
    const bool shadow = shadowModeOnly_.load(std::memory_order_acquire);
    const bool persisted = persistenceAvailable_.load(std::memory_order_acquire);
    Event ev;
    ev.id = "rl:status:received";
    ev.setInt("seen", 1);
    ev.setInt("addon_online", 1);
    ev.setInt64("timestamp", EpochTime::now());
    {
        std::lock_guard<std::mutex> lock(workspaceMutex_);
        ev.setString("last_shot_id", workspace_->lastShotId.c_str());
        ev.setInt64("last_shot_at", workspace_->lastShotAt);
        ev.setString("last_shot_type", "espresso");
        ev.setInt("last_shot_time_s_x10", static_cast<int>(std::lround(workspace_->lastShotTimeS * 10.0f)));
        ev.setInt("last_shot_beverage_out_g_x10", static_cast<int>(std::lround(workspace_->lastShotBeverageOutG * 10.0f)));
        ev.setInt("last_shot_target_yield_g_x10", static_cast<int>(std::lround(workspace_->lastShotTargetYieldG * 10.0f)));
        ev.setInt64("last_recommendation_at", workspace_->lastRecommendationAt);
        ev.setString("last_recommendation_id", workspace_->lastRecommendationId.c_str());
        ev.setString("mode", shadow ? "on_board_shadow" : "on_board_live");
        ev.setString("optimizer_profile_id", workspace_->activeProfileId.c_str());
        ev.setString("optimizer_profile_label", workspace_->activeProfileLabel.c_str());
        ev.setString("optimizer_configured_mode", "on_board_cpbo_level1");
        ev.setString("optimizer_effective_mode", shadow ? "on_board_cpbo_shadow" : "on_board_cpbo_live");
        ev.setString("optimizer_fallback_reason", workspace_->lastCpboCancelled ? "aborted_for_brew" : "");
        ev.setString("cpbo_profile_name", "embedded_ghvi");
        ev.setString("cpbo_comparison_mode", "best_incumbent");
        ev.setString("cpbo_optimization_run_id", workspace_->optimizationRunId.c_str());
        ev.setInt("cpbo_locally_converged", workspace_->trustRegion.locallyConverged ? 1 : 0);
        ev.setInt("cpbo_stopped_by_user", 0);
        if (workspace_->hasBestPoint && workspace_->bestPointIndex < workspace_->points.size()) {
            ev.setString("cpbo_best_shot_id", workspace_->points[workspace_->bestPointIndex].shotId.c_str());
        }
        char trBuf[24];
        std::snprintf(trBuf, sizeof(trBuf), "%.4f", workspace_->trustRegion.length);
        ev.setString("cpbo_trust_region_length", trBuf);
        ev.setInt("cpbo_trust_region_success_count", workspace_->trustRegion.successCount);
        ev.setInt("cpbo_trust_region_failure_count", workspace_->trustRegion.failureCount);
        ev.setString("cpbo_last_transition_action", trustActionName(workspace_->trustRegion.lastAction));
        ev.setInt("local_shot_count", workspace_->localShotCount);
        ev.setString("runtime_health_status", "healthy");
        ev.setString("runtime_health_summary",
                     shadow
                         ? "On-board ESP32-S3 CPBO + Level 1 MPC active (Shadow Mode)"
                         : "On-board ESP32-S3 CPBO + Level 1 MPC active (Live Actuation Armed)");
        ev.setString("runtime_health_storage_backend", persisted ? "littlefs_psram" : "esp32s3_psram");
        ev.setInt("runtime_health_storage_available", persisted ? 1 : 0);
        ev.setInt("onboard_present", 1);
        ev.setInt("onboard_shadow_only", shadow ? 1 : 0);
        ev.setInt("onboard_vfe_samples", static_cast<int>(workspace_->histZ.size()));
        ev.setInt("onboard_cpbo_points", static_cast<int>(workspace_->points.size()));
        ev.setInt("onboard_cpbo_comparisons", static_cast<int>(workspace_->comparisons.size()));
        ev.setInt("onboard_last_mpc_us", static_cast<int>(workspace_->lastMpcFitUs));
        ev.setInt("onboard_last_cpbo_ms", static_cast<int>(workspace_->lastCpboFitMs));
        ev.setInt("onboard_persisted", persistedStateLoaded_.load(std::memory_order_acquire) ? 1 : 0);
    }
    queueMainThreadEvent(std::move(ev));
}

#endif // defined(GAGGIMATE_ENABLE_ONBOARD_ESPRESSORL) && !defined(GAGGIMATE_SIM)
