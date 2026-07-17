#include "AutoTuning.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace AutoTuning {

bool validateRecipeDomain(RecipeDomain const &domain, std::string &reason) {
    const float values[] = {domain.grindRadiusSteps, domain.doseMinG, domain.doseMaxG, domain.targetOutputMinG,
                            domain.targetOutputMaxG};
    for (float value : values) {
        if (!std::isfinite(value) || value <= 0.0f) {
            reason = "recipe domain values must be positive and finite";
            return false;
        }
    }
    if (domain.grindRadiusSteps < RECIPE_DOMAIN_GRIND_RADIUS_MIN_STEPS ||
        domain.grindRadiusSteps > RECIPE_DOMAIN_GRIND_RADIUS_MAX_STEPS || domain.doseMinG < RECIPE_DOMAIN_DOSE_MIN_G ||
        domain.doseMinG > RECIPE_DOMAIN_DOSE_MAX_G || domain.doseMaxG < RECIPE_DOMAIN_DOSE_MIN_G ||
        domain.doseMaxG > RECIPE_DOMAIN_DOSE_MAX_G || domain.targetOutputMinG < RECIPE_DOMAIN_OUTPUT_MIN_G ||
        domain.targetOutputMinG > RECIPE_DOMAIN_OUTPUT_MAX_G || domain.targetOutputMaxG < RECIPE_DOMAIN_OUTPUT_MIN_G ||
        domain.targetOutputMaxG > RECIPE_DOMAIN_OUTPUT_MAX_G) {
        reason = "recipe domain is outside the integrity envelope";
        return false;
    }
    if (domain.doseMaxG <= domain.doseMinG) {
        reason = "maximum dose must exceed minimum dose";
        return false;
    }
    if (domain.targetOutputMaxG <= domain.targetOutputMinG) {
        reason = "maximum target output must exceed minimum target output";
        return false;
    }
    reason = "";
    return true;
}

bool validateRecommendationTargets(RecommendationTargets const &targets, RecipeDomain const &domain, std::string &reason) {
    if (!validateRecipeDomain(domain, reason)) {
        return false;
    }
    const float values[] = {targets.nextDoseG, targets.targetYieldG, targets.targetRatio};
    for (float value : values) {
        if (!std::isfinite(value) || value <= 0.0f) {
            reason = "recommendation targets must be positive and finite";
            return false;
        }
    }
    if (targets.nextDoseG < domain.doseMinG || targets.nextDoseG > domain.doseMaxG) {
        reason = "recommended dose is outside the configured recipe domain";
        return false;
    }
    if (targets.targetYieldG < domain.targetOutputMinG || targets.targetYieldG > domain.targetOutputMaxG) {
        reason = "recommended yield is outside the configured recipe domain";
        return false;
    }
    if (!std::isfinite(targets.grindDeltaStepsFromCurrent) ||
        std::fabs(targets.grindDeltaStepsFromCurrent) > RECIPE_DOMAIN_GRIND_RADIUS_MAX_STEPS) {
        reason = "recommended grind change is outside the integrity envelope";
        return false;
    }
    const float derivedRatio = targets.targetYieldG / targets.nextDoseG;
    const float ratioTolerance = 0.001f * std::max(1.0f, std::fabs(derivedRatio));
    if (std::fabs(derivedRatio - targets.targetRatio) > ratioTolerance) {
        reason = "recommended ratio does not match dose and yield";
        return false;
    }
    reason = "";
    return true;
}

namespace {

class DisabledProvider final : public OptimizerProvider {
  public:
    bool available(OptimizerTransportPort const *) const override { return false; }
    bool usesOffBoardTransport(OptimizerTransportPort const *) const override { return false; }
    const char *status(OptimizerTransportPort const *) const override { return "disabled"; }
    const char *summary(OptimizerTransportPort const *) const override { return "Auto Tuning disabled"; }
};

class OnBoardProvider final : public OptimizerProvider {
  public:
    bool available(OptimizerTransportPort const *) const override { return false; }
    bool usesOffBoardTransport(OptimizerTransportPort const *) const override { return false; }
    const char *status(OptimizerTransportPort const *) const override { return "unavailable"; }
    const char *summary(OptimizerTransportPort const *) const override { return "On-board optimizer is not implemented yet"; }
};

class OffBoardProvider final : public OptimizerProvider {
  public:
    bool available(OptimizerTransportPort const *transport) const override {
        return transport != nullptr && transport->configured();
    }
    bool usesOffBoardTransport(OptimizerTransportPort const *transport) const override { return available(transport); }
    const char *status(OptimizerTransportPort const *transport) const override {
        if (!available(transport)) {
            return "waiting_for_transport";
        }
        return transport->connected() ? "ready" : "transport_offline";
    }
    const char *summary(OptimizerTransportPort const *transport) const override {
        if (!available(transport)) {
            return "Off-board provider needs an optimizer transport";
        }
        return transport->connected() ? "Using off-board EspressoRL" : "Off-board EspressoRL is waiting for its transport";
    }
};

DisabledProvider disabledProvider;
OnBoardProvider onBoardProvider;
OffBoardProvider offBoardProvider;

} // namespace

std::string normalizeProviderMode(std::string_view mode) {
    while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.front()))) {
        mode.remove_prefix(1);
    }
    while (!mode.empty() && std::isspace(static_cast<unsigned char>(mode.back()))) {
        mode.remove_suffix(1);
    }
    return providerModeKey(providerModeFromKey(mode).value_or(ProviderMode::Disabled));
}

std::optional<ProviderMode> providerModeFromKey(std::string_view mode) {
    if (mode == PROVIDER_DISABLED) {
        return ProviderMode::Disabled;
    }
    if (mode == PROVIDER_ON_BOARD) {
        return ProviderMode::OnBoard;
    }
    if (mode == PROVIDER_OFF_BOARD) {
        return ProviderMode::OffBoard;
    }
    return std::nullopt;
}

const char *providerModeKey(ProviderMode mode) {
    switch (mode) {
    case ProviderMode::OnBoard:
        return PROVIDER_ON_BOARD;
    case ProviderMode::OffBoard:
        return PROVIDER_OFF_BOARD;
    case ProviderMode::Disabled:
    default:
        return PROVIDER_DISABLED;
    }
}

OptimizerProvider const &selectedProvider(ProviderMode mode) {
    if (mode == ProviderMode::OnBoard) {
        return onBoardProvider;
    }
    if (mode == ProviderMode::OffBoard) {
        return offBoardProvider;
    }
    return disabledProvider;
}

Router::Router(OptimizerConfiguration configuration, OptimizerTransportPort const *transport)
    : configuration(std::move(configuration)), transport(transport) {}

OptimizerProvider const &Router::provider() const { return selectedProvider(configuration.providerMode); }

bool Router::enabled() const { return configuration.autoTuningEnabled && configuration.providerMode != ProviderMode::Disabled; }

bool Router::providerAvailable() const { return enabled() && provider().available(transport); }

bool Router::optimizationActive() const {
    return providerAvailable() && configuration.localOptimizationEnabled && !configuration.optimizationPaused &&
           configuration.beanContextSelected;
}

bool Router::routeOffBoardTransport() const { return enabled() && provider().usesOffBoardTransport(transport); }

bool Router::acceptOffBoardStatus() const { return routeOffBoardTransport(); }

bool Router::acceptActionableRecommendations() const { return optimizationActive(); }

bool Router::acceptUserCommands() const { return enabled(); }

const char *Router::providerStatus() const { return enabled() ? provider().status(transport) : "disabled"; }

const char *Router::providerSummary() const {
    return enabled() ? provider().summary(transport) : disabledProvider.summary(transport);
}

} // namespace AutoTuning
