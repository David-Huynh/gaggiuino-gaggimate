const AUTO_TUNING_RUNTIME_FIELDS = [
  'rlStatusSeen',
  'rlAddonOnline',
  'rlProviderStatus',
  'rlProviderSummary',
  'rlBeanContextId',
  'rlBeanContextName',
  'rlGrinderContextId',
  'rlGrinderContextName',
  'rlOptimizationPaused',
  'rlLocalOptimizationEnabled',
  'rlLastStatusAt',
  'rlLastShotId',
  'rlLastShotAt',
  'rlLastShotType',
  'rlLastShotTimeS',
  'rlLastShotBeverageOutG',
  'rlLastShotTargetYieldG',
  'rlLastRecommendationId',
  'rlLastRecommendationAt',
  'rlRecommendationApplyStatus',
  'rlMode',
  'rlOptimizerConfiguredMode',
  'rlOptimizerEffectiveMode',
  'rlOptimizerFallbackReason',
  'rlCPBOEffectiveProfileName',
  'rlCPBOEffectiveComparisonMode',
  'rlLocalShotCount',
];

export function mergeAutoTuningRuntimeStatus(formData, status) {
  const next = { ...formData };
  if (!status || typeof status !== 'object') {
    return next;
  }

  for (const field of AUTO_TUNING_RUNTIME_FIELDS) {
    if (Object.hasOwn(status, field)) {
      next[field] = status[field];
    }
  }

  return next;
}

export function selectAutoTuningProvider(formData, providerMode) {
  const enabled = providerMode !== 'disabled';
  return {
    ...formData,
    rlProviderMode: providerMode,
    rlAutoTuningEnabled: enabled,
  };
}
