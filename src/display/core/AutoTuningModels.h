#ifndef AUTOTUNINGMODELS_H
#define AUTOTUNINGMODELS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AutoTuning {

using Timestamp = std::int64_t;

constexpr int TASTE_GOAL_SCHEMA_VERSION = 1;
constexpr std::size_t TASTE_GOAL_ATTRIBUTE_COUNT = 14;

enum class TasteAttribute : std::uint8_t {
    Fruity,
    Citrus,
    Floral,
    Sweet,
    NuttyCocoa,
    Roasted,
    Spice,
    Fermented,
    Sour,
    GreenVegetative,
    Bitter,
    AstringentHarsh,
    PaperyStale,
    Salty,
};

enum class TasteLevel : std::uint8_t { Unspecified, Low, Medium, High };
enum class TasteGoalMode : std::uint8_t { Balanced, Custom };

struct TasteGoal {
    TasteGoalMode mode = TasteGoalMode::Balanced;
    std::array<TasteLevel, TASTE_GOAL_ATTRIBUTE_COUNT> targets{};

    static TasteGoal balanced();
    bool valid() const;
    bool operator==(TasteGoal const &other) const;
    bool operator!=(TasteGoal const &other) const { return !(*this == other); }
};

const char *tasteAttributeKey(TasteAttribute attribute);
std::optional<TasteAttribute> tasteAttributeFromKey(std::string_view key);
const char *tasteLevelKey(TasteLevel level);
std::optional<TasteLevel> tasteLevelFromKey(std::string_view key);

template <typename T> class ArrayView {
  public:
    constexpr ArrayView() = default;
    constexpr ArrayView(T *data, std::size_t size) : values(data), count(size) {}

    constexpr T *data() const { return values; }
    constexpr std::size_t size() const { return count; }
    constexpr bool empty() const { return count == 0; }
    constexpr T &operator[](std::size_t index) const { return values[index]; }
    constexpr T *begin() const { return values; }
    constexpr T *end() const { return values + count; }

  private:
    T *values = nullptr;
    std::size_t count = 0;
};

enum class PumpTargetMode : std::uint8_t { Simple, Pressure, Flow };

struct ShotSample {
    float pressure = 0.0f;
    float targetPressure = 0.0f;
    float flow = 0.0f;
    float pumpFlow = 0.0f;
    float targetFlow = 0.0f;
    float temperature = 0.0f;
    float targetTemperature = 0.0f;
    float weight = 0.0f;
    PumpTargetMode pumpTargetMode = PumpTargetMode::Simple;
    bool valveOpen = false;
    std::uint16_t elapsedMs = 0;
};

struct LiveShotStarted {
    std::string shotId;
    std::string machineId;
    Timestamp startedAtMs = 0;
    std::uint16_t sampleIntervalMs = 250;
    std::string weightSource;
    std::string flowSource;
};

struct LiveShotSample {
    std::string shotId;
    std::string machineId;
    Timestamp timestampMs = 0;
    std::uint16_t sequence = 0;
    ShotSample sample;
};

struct LiveShotEnded {
    std::string shotId;
    std::string machineId;
    Timestamp endedAtMs = 0;
    std::uint16_t finalSequence = 0;
    std::uint16_t elapsedMs = 0;
    std::string endState;
};

struct GrinderSnapshot {
    std::string contextId;
    std::string contextName;
    std::string calibrationMode = "uncalibrated";
    std::string adjustmentMode = "stepped";
    std::string stepDirection = "higher_is_finer";
    std::string referenceLabel;
    std::optional<float> micronsPerStep;
    std::optional<float> currentAbsoluteStep;
    std::optional<float> absoluteReferenceStep;
    std::optional<float> relativeStepsFromReference;
    std::optional<float> relativeMicronsFromReference;
    bool observed = false;
};

struct RecipeSnapshot {
    std::string beanContextId;
    std::string beanContextName;
    TasteGoal tasteGoal = TasteGoal::balanced();
    GrinderSnapshot grinder;
    std::optional<float> doseTargetG;
    std::optional<float> targetYieldG;
    std::optional<float> targetRatio;
};

struct ProfileSnapshot {
    std::string id;
    std::string label;
    std::string type;
    std::string rawProfileHash;
    std::size_t phaseCount = 0;
    float temperatureC = 0.0f;
    bool available = false;
    bool rawProfileAvailable = false;
    std::optional<bool> flowValid;
    std::optional<bool> flowMasked;
};

enum class ComparisonMode : std::uint8_t { None, GlobalPrevious, BestIncumbent };
enum class RecommendationMode : std::uint8_t { Unknown, CpboGlobalPrevious, CpboBestIncumbent };
enum class RecommendationStatus : std::uint8_t {
    Unknown,
    Pending,
    Shown,
    Accepted,
    Edited,
    Ignored,
    Used,
    Superseded,
    Expired,
    Failed,
};

struct RecommendationReference {
    std::string recommendationId;
    std::string installId;
    std::string optimizationRunId;
    std::string anchorShotId;
    ComparisonMode comparisonMode = ComparisonMode::None;
    TasteGoal tasteGoal = TasteGoal::balanced();
    bool preferenceFeedbackRequired = false;
    float grindDeltaStepsFromCurrent = 0.0f;
    float grindDeltaMicronsFromCurrent = 0.0f;
    float projectedRelativeStepFromReference = 0.0f;
    float projectedRelativeMicronsFromReference = 0.0f;
    float nextDoseG = 0.0f;
    float targetYieldG = 0.0f;
    float targetRatio = 0.0f;

    bool present() const { return !recommendationId.empty(); }
};

struct FinalPhaseSnapshot {
    bool available = false;
    std::size_t index = 0;
    std::string name;
    std::string type;
    float elapsedS = 0.0f;
    std::string pumpTarget;
    std::optional<float> targetPressure;
    std::optional<float> targetFlow;
    bool valveOpen = false;
    float temperatureC = 0.0f;
    std::string shotEndState;
};

struct ShotRecord {
    std::string shotId;
    std::string machineId;
    std::string machineAdapter = "gaggimate";
    Timestamp timestamp = 0;
    bool utility = false;
    bool excludeFromLocalOptimization = false;
    bool localOptimizationEnabled = false;
    bool communityUploadEnabled = false;
    std::string communityUploadOwner = "gaggimate";
    float optimizationWeight = 1.0f;

    RecipeSnapshot recipe;
    ProfileSnapshot profile;
    RecommendationReference recommendation;
    FinalPhaseSnapshot finalPhase;

    std::optional<float> measuredDoseG;
    bool doseObserved = false;
    bool doseTargetConfirmed = false;
    std::optional<bool> grindFollowed;
    std::optional<bool> doseFollowed;
    std::optional<bool> yieldFollowed;

    std::string weightSource;
    std::string flowSource;
    std::string flowUnits = "g_per_s";
    std::string pumpFlowSource = "gaggimate_pump_model";
    std::string pumpFlowUnits = "ml_per_s";
    std::string pumpFlowInterpretation;
    bool pumpFlowCalibrationRequired = false;
    std::string predictiveWeightInterpretation;

    float beverageOutG = 0.0f;
    std::string beverageOutObservation = "control_cutoff";
    float shotTimeS = 0.0f;
    bool predictiveStopApplied = false;
    std::optional<float> predictiveStopDelayMs;
    std::optional<float> predictiveStopRateGPerS;
    std::optional<float> predictiveStopLeadG;
    std::optional<float> predictedFinalBeverageOutG;

    ArrayView<const ShotSample> samples;

    bool hasUsableDose() const { return (doseObserved && measuredDoseG.has_value()) || doseTargetConfirmed; }
};

struct CorrectedShotRecord {
    ShotRecord record;
    std::vector<ShotSample> samples;

    void bindSamples() { record.samples = ArrayView<const ShotSample>(samples.data(), samples.size()); }
};

struct ShotCompletion {
    std::string shotId;
    RecommendationReference recommendation;
    float doseTargetG = 0.0f;
};

struct ShotCaptureDisposition {
    bool doseConfirmationRequired = false;
    bool optimizerDeliveryRequired = false;
    bool communityUploadRequired = false;
};

struct Recommendation {
    std::string recommendationId;
    std::string sourceShotId;
    std::string installId;
    std::string machineId;
    std::string beanContextId;
    std::string grinderContextId;
    std::string profileId;
    std::string rawProfileHash;
    TasteGoal tasteGoal = TasteGoal::balanced();

    Timestamp createdAt = 0;
    Timestamp updatedAt = 0;
    std::optional<Timestamp> expiresAt;
    RecommendationMode mode = RecommendationMode::Unknown;
    RecommendationStatus status = RecommendationStatus::Unknown;
    std::string reason;
    std::string optimizationRunId;
    std::string comparisonAnchorShotId;
    ComparisonMode comparisonMode = ComparisonMode::None;
    bool preferenceFeedbackRequired = false;

    std::string grinderCalibrationMode;
    std::string grinderAdjustmentMode;
    std::string stepDirection = "higher_is_finer";
    std::string referenceLabel;
    std::optional<float> micronsPerStep;
    std::optional<float> currentAbsoluteStep;
    std::optional<float> absoluteReferenceStep;
    std::optional<float> projectedAbsoluteStep;
    float grindDeltaStepsFromCurrent = 0.0f;
    float grindDeltaMicronsFromCurrent = 0.0f;
    float projectedRelativeStepFromReference = 0.0f;
    float projectedRelativeMicronsFromReference = 0.0f;
    float nextDoseG = 0.0f;
    float targetYieldG = 0.0f;
    float targetRatio = 0.0f;
    std::optional<float> confidence;

    std::optional<Timestamp> acceptedAt;
    std::optional<Timestamp> ignoredAt;
    std::optional<Timestamp> editedAt;
    std::optional<Timestamp> usedAt;
    std::optional<Timestamp> supersededAt;
    std::optional<Timestamp> applyAcknowledgedAt;
    int shownCount = 0;
    std::string applyStatus;
    std::string applyError;
};

enum class DeliveryStatus : std::uint8_t {
    NotRequired,
    AwaitingDoseConfirmation,
    Pending,
    AwaitingAcknowledgement,
    RetryWait,
    Accepted,
    PermanentRejection,
};

struct DeliveryState {
    DeliveryStatus status = DeliveryStatus::NotRequired;
    int attemptCount = 0;
    Timestamp nextRetryAt = 0;
    std::optional<Timestamp> lastAttemptAt;
    std::optional<Timestamp> acknowledgedAt;
    std::string outcome;
    std::string lastError;

    bool terminal() const;
    bool canTransitionTo(DeliveryStatus next) const;
};

enum class PreferenceLabel : std::uint8_t { NewBetter, AnchorBetter, Tie };
enum class FollowThroughStatus : std::uint8_t { Unknown, Followed, NotFollowed, PartiallyFollowed };

struct PreferenceFeedback {
    std::string installId;
    std::string optimizationRunId;
    std::string newShotId;
    std::string anchorShotId;
    PreferenceLabel label = PreferenceLabel::Tie;
    ComparisonMode comparisonMode = ComparisonMode::None;
    TasteGoal tasteGoal = TasteGoal::balanced();
    std::string recommendationId;
};

struct ShotCorrection {
    std::string shotId;
    std::string source;
    std::optional<bool> excludeFromLocalOptimization;
    std::string shotType;
    std::optional<bool> grindFollowed;
    std::optional<bool> doseFollowed;
    std::optional<bool> yieldFollowed;
    std::optional<float> relativeGrindStepsFromReference;
    std::optional<float> currentAbsoluteStep;
    std::optional<float> doseInG;
    std::optional<float> targetYieldG;
    std::optional<float> beverageOutG;
    std::vector<std::string> tags;
};

const char *comparisonModeKey(ComparisonMode mode);
std::optional<ComparisonMode> comparisonModeFromKey(std::string_view key);
const char *recommendationModeKey(RecommendationMode mode);
std::optional<RecommendationMode> recommendationModeFromKey(std::string_view key);
const char *recommendationStatusKey(RecommendationStatus status);
std::optional<RecommendationStatus> recommendationStatusFromKey(std::string_view key);
const char *deliveryStatusKey(DeliveryStatus status);
std::optional<DeliveryStatus> deliveryStatusFromKey(std::string_view key);
const char *preferenceLabelKey(PreferenceLabel label);
std::optional<PreferenceLabel> preferenceLabelFromKey(std::string_view key);
const char *followThroughStatusKey(FollowThroughStatus status);
FollowThroughStatus deriveFollowThrough(std::optional<bool> grindFollowed, std::optional<bool> doseFollowed,
                                        std::optional<bool> yieldFollowed);

} // namespace AutoTuning

#endif // AUTOTUNINGMODELS_H
