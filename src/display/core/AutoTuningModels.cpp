#include "AutoTuningModels.h"

namespace AutoTuning {
namespace {

template <typename Enum, std::size_t Size>
std::optional<Enum> enumFromKey(std::string_view key, std::array<const char *, Size> const &keys) {
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (key == keys[index]) {
            return static_cast<Enum>(index);
        }
    }
    return std::nullopt;
}

constexpr std::array<const char *, TASTE_GOAL_ATTRIBUTE_COUNT> TASTE_ATTRIBUTE_KEYS = {
    "fruity",    "citrus", "floral",           "sweet",  "nutty_cocoa",      "roasted",      "spice",
    "fermented", "sour",   "green_vegetative", "bitter", "astringent_harsh", "papery_stale", "salty",
};

constexpr std::array<const char *, 4> TASTE_LEVEL_KEYS = {"", "low", "medium", "high"};
constexpr std::array<const char *, 3> COMPARISON_MODE_KEYS = {"", "global_previous", "best_incumbent"};
constexpr std::array<const char *, 3> RECOMMENDATION_MODE_KEYS = {"", "cpbo_global_previous", "cpbo_best_incumbent"};
constexpr std::array<const char *, 10> RECOMMENDATION_STATUS_KEYS = {
    "", "pending", "shown", "accepted", "edited", "ignored", "used", "superseded", "expired", "failed"};
constexpr std::array<const char *, 7> DELIVERY_STATUS_KEYS = {
    "not_required", "awaiting_dose_confirmation", "pending", "awaiting_ack", "retry_wait", "accepted", "permanent_rejection"};
constexpr std::array<const char *, 3> PREFERENCE_LABEL_KEYS = {"new_better", "anchor_better", "tie"};
constexpr std::array<const char *, 4> FOLLOW_THROUGH_STATUS_KEYS = {"", "followed", "not_followed", "partially_followed"};

} // namespace

TasteGoal TasteGoal::balanced() { return TasteGoal{}; }

bool TasteGoal::valid() const {
    if (mode != TasteGoalMode::Balanced && mode != TasteGoalMode::Custom) {
        return false;
    }
    bool hasTarget = false;
    for (TasteLevel level : targets) {
        if (level != TasteLevel::Unspecified && level != TasteLevel::Low && level != TasteLevel::Medium &&
            level != TasteLevel::High) {
            return false;
        }
        hasTarget = hasTarget || level != TasteLevel::Unspecified;
    }
    return mode == TasteGoalMode::Custom ? hasTarget : !hasTarget;
}

bool TasteGoal::operator==(TasteGoal const &other) const { return mode == other.mode && targets == other.targets; }

const char *tasteAttributeKey(TasteAttribute attribute) {
    const auto index = static_cast<std::size_t>(attribute);
    return index < TASTE_ATTRIBUTE_KEYS.size() ? TASTE_ATTRIBUTE_KEYS[index] : "";
}

std::optional<TasteAttribute> tasteAttributeFromKey(std::string_view key) {
    return enumFromKey<TasteAttribute>(key, TASTE_ATTRIBUTE_KEYS);
}

const char *tasteLevelKey(TasteLevel level) {
    const auto index = static_cast<std::size_t>(level);
    return index < TASTE_LEVEL_KEYS.size() ? TASTE_LEVEL_KEYS[index] : "";
}

std::optional<TasteLevel> tasteLevelFromKey(std::string_view key) {
    auto level = enumFromKey<TasteLevel>(key, TASTE_LEVEL_KEYS);
    if (!level || *level == TasteLevel::Unspecified) {
        return std::nullopt;
    }
    return level;
}

bool DeliveryState::terminal() const {
    return status == DeliveryStatus::Accepted || status == DeliveryStatus::PermanentRejection;
}

bool DeliveryState::canTransitionTo(DeliveryStatus next) const {
    if (next == status) {
        return true;
    }
    if (terminal()) {
        return false;
    }
    switch (status) {
    case DeliveryStatus::NotRequired:
        return next == DeliveryStatus::Pending || next == DeliveryStatus::AwaitingDoseConfirmation;
    case DeliveryStatus::AwaitingDoseConfirmation:
        return next == DeliveryStatus::Pending || next == DeliveryStatus::NotRequired;
    case DeliveryStatus::Pending:
        return next == DeliveryStatus::AwaitingAcknowledgement || next == DeliveryStatus::NotRequired;
    case DeliveryStatus::AwaitingAcknowledgement:
        return next == DeliveryStatus::RetryWait || next == DeliveryStatus::Accepted ||
               next == DeliveryStatus::PermanentRejection;
    case DeliveryStatus::RetryWait:
        return next == DeliveryStatus::AwaitingAcknowledgement || next == DeliveryStatus::Accepted ||
               next == DeliveryStatus::PermanentRejection || next == DeliveryStatus::NotRequired;
    case DeliveryStatus::Accepted:
    case DeliveryStatus::PermanentRejection:
        return false;
    }
    return false;
}

const char *comparisonModeKey(ComparisonMode mode) {
    const auto index = static_cast<std::size_t>(mode);
    return index < COMPARISON_MODE_KEYS.size() ? COMPARISON_MODE_KEYS[index] : "";
}

std::optional<ComparisonMode> comparisonModeFromKey(std::string_view key) {
    auto value = enumFromKey<ComparisonMode>(key, COMPARISON_MODE_KEYS);
    return value && *value != ComparisonMode::None ? value : std::nullopt;
}

const char *recommendationModeKey(RecommendationMode mode) {
    const auto index = static_cast<std::size_t>(mode);
    return index < RECOMMENDATION_MODE_KEYS.size() ? RECOMMENDATION_MODE_KEYS[index] : "";
}

std::optional<RecommendationMode> recommendationModeFromKey(std::string_view key) {
    auto value = enumFromKey<RecommendationMode>(key, RECOMMENDATION_MODE_KEYS);
    return value && *value != RecommendationMode::Unknown ? value : std::nullopt;
}

const char *recommendationStatusKey(RecommendationStatus status) {
    const auto index = static_cast<std::size_t>(status);
    return index < RECOMMENDATION_STATUS_KEYS.size() ? RECOMMENDATION_STATUS_KEYS[index] : "";
}

std::optional<RecommendationStatus> recommendationStatusFromKey(std::string_view key) {
    auto value = enumFromKey<RecommendationStatus>(key, RECOMMENDATION_STATUS_KEYS);
    return value && *value != RecommendationStatus::Unknown ? value : std::nullopt;
}

const char *deliveryStatusKey(DeliveryStatus status) {
    const auto index = static_cast<std::size_t>(status);
    return index < DELIVERY_STATUS_KEYS.size() ? DELIVERY_STATUS_KEYS[index] : "";
}

std::optional<DeliveryStatus> deliveryStatusFromKey(std::string_view key) {
    return enumFromKey<DeliveryStatus>(key, DELIVERY_STATUS_KEYS);
}

const char *preferenceLabelKey(PreferenceLabel label) {
    const auto index = static_cast<std::size_t>(label);
    return index < PREFERENCE_LABEL_KEYS.size() ? PREFERENCE_LABEL_KEYS[index] : "";
}

std::optional<PreferenceLabel> preferenceLabelFromKey(std::string_view key) {
    return enumFromKey<PreferenceLabel>(key, PREFERENCE_LABEL_KEYS);
}

const char *followThroughStatusKey(FollowThroughStatus status) {
    const auto index = static_cast<std::size_t>(status);
    return index < FOLLOW_THROUGH_STATUS_KEYS.size() ? FOLLOW_THROUGH_STATUS_KEYS[index] : "";
}

FollowThroughStatus deriveFollowThrough(std::optional<bool> grindFollowed, std::optional<bool> doseFollowed,
                                        std::optional<bool> yieldFollowed) {
    const std::optional<bool> observations[] = {grindFollowed, doseFollowed, yieldFollowed};
    std::size_t known = 0;
    std::size_t followed = 0;
    for (std::optional<bool> observation : observations) {
        if (observation.has_value()) {
            ++known;
            followed += *observation ? 1U : 0U;
        }
    }
    if (known == 0) {
        return FollowThroughStatus::Unknown;
    }
    if (followed == known) {
        return FollowThroughStatus::Followed;
    }
    if (followed == 0) {
        return FollowThroughStatus::NotFollowed;
    }
    return FollowThroughStatus::PartiallyFollowed;
}

} // namespace AutoTuning
