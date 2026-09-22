#pragma once
#include "AutoTuningModels.h"

namespace AutoTuning {
// Restoring a recipe requires a matching, persisted user request. A retained
// recommendation by itself never authorizes a machine mutation.
inline bool restoreBestRequestMatches(Recommendation const &recommendation, std::string const &requestId) {
    return recommendation.mode == RecommendationMode::RestoreBest &&
           !recommendation.preferenceFeedbackRequired && !requestId.empty() &&
           recommendation.recommendationId == requestId;
}

// A missing profile is not permission to mutate whichever profile is selected.
inline bool recommendationContextMatches(Recommendation const &recommendation,
                                         std::string const &machine, std::string const &bean,
                                         std::string const &grinder, std::string const &profile,
                                         TasteGoal const &goal) {
    return recommendation.machineId == machine && !bean.empty() && !grinder.empty() && !profile.empty() &&
           recommendation.beanContextId == bean && recommendation.grinderContextId == grinder &&
           recommendation.profileId == profile && recommendation.tasteGoal == goal;
}
}
