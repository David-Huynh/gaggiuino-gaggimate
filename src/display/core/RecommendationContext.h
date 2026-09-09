#pragma once
#include "AutoTuningModels.h"

namespace AutoTuning {
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
