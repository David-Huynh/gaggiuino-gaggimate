#pragma once

#include "AutoTuningPorts.h"
#include <optional>
#include <string>
#include <string_view>

namespace AutoTuning {

constexpr const char *PROVIDER_DISABLED = "disabled";
constexpr const char *PROVIDER_ON_BOARD = "on_board";
constexpr const char *PROVIDER_OFF_BOARD = "off_board";
constexpr float RECIPE_DOMAIN_GRIND_RADIUS_MIN_STEPS = 0.1f;
constexpr float RECIPE_DOMAIN_GRIND_RADIUS_MAX_STEPS = 1000.0f;
constexpr float RECIPE_DOMAIN_DOSE_MIN_G = 0.1f;
constexpr float RECIPE_DOMAIN_DOSE_MAX_G = 100.0f;
constexpr float RECIPE_DOMAIN_OUTPUT_MIN_G = 0.1f;
constexpr float RECIPE_DOMAIN_OUTPUT_MAX_G = 1000.0f;

struct RecipeDomain {
    float grindRadiusSteps = 10.0f;
    float doseMinG = 6.0f;
    float doseMaxG = 30.0f;
    float targetOutputMinG = 5.0f;
    float targetOutputMaxG = 250.0f;
};

struct RecommendationTargets {
    float nextDoseG = 0.0f;
    float targetYieldG = 0.0f;
    float targetRatio = 0.0f;
    float grindDeltaStepsFromCurrent = 0.0f;
};

enum class ProviderMode : unsigned char { Disabled, OnBoard, OffBoard };

struct OptimizerConfiguration {
    bool autoTuningEnabled = false;
    bool localOptimizationEnabled = false;
    bool optimizationPaused = false;
    ProviderMode providerMode = ProviderMode::Disabled;
    bool beanContextSelected = false;
};

bool validateRecipeDomain(RecipeDomain const &domain, std::string &reason);
bool validateRecommendationTargets(RecommendationTargets const &targets, RecipeDomain const &domain, std::string &reason);

class OptimizerProvider {
  public:
    virtual ~OptimizerProvider() = default;

    virtual bool available(OptimizerTransportPort const *transport) const = 0;
    virtual bool usesOffBoardTransport(OptimizerTransportPort const *transport) const = 0;
    virtual const char *status(OptimizerTransportPort const *transport) const = 0;
    virtual const char *summary(OptimizerTransportPort const *transport) const = 0;
};

class Router {
  public:
    explicit Router(OptimizerConfiguration configuration, OptimizerTransportPort const *transport = nullptr);

    OptimizerProvider const &provider() const;
    bool enabled() const;
    bool providerAvailable() const;
    bool optimizationActive() const;
    bool routeOffBoardTransport() const;
    bool acceptOffBoardStatus() const;
    bool acceptActionableRecommendations() const;
    bool acceptUserCommands() const;
    const char *providerStatus() const;
    const char *providerSummary() const;

  private:
    OptimizerConfiguration configuration;
    OptimizerTransportPort const *transport;
};

std::string normalizeProviderMode(std::string_view mode);
std::optional<ProviderMode> providerModeFromKey(std::string_view mode);
const char *providerModeKey(ProviderMode mode);
OptimizerProvider const &selectedProvider(ProviderMode mode);

} // namespace AutoTuning
