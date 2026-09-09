export function communityUploadPipelineText(settings) {
  if (!settings?.rlCommunityUploadEnabled) {
    return 'Disabled';
  }
  if (settings?.communityUploadEffective) {
    return Number(settings?.communityUploadRetryCount || 0) > 0 ? 'Retrying' : 'Ready';
  }
  return 'Waiting for EspressoRL container';
}

export function communityUploadQueueText(settings) {
  return `${settings?.communityUploadPendingCount ?? 0} pending / ${
    settings?.communityUploadRetryCount ?? 0
  } retry / ${settings?.communityUploadRejectedCount ?? 0} rejected`;
}
