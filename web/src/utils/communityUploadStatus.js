export function communityUploadPipelineText(settings) {
  if (!settings?.rlCommunityUploadEnabled) {
    return 'Disabled';
  }
  if (settings?.communityUploadEffective) {
    return Number(settings?.communityUploadRetryCount || 0) > 0 ? 'Retrying' : 'Ready';
  }
  if (!settings?.rlUploadBaseUrl) {
    return 'Needs Supabase URL';
  }
  return settings?.communityUploadConfigured ? 'Starting' : 'Registration pending';
}

export function communityUploadQueueText(settings) {
  return `${settings?.communityUploadPendingCount ?? 0} pending / ${
    settings?.communityUploadRetryCount ?? 0
  } retry / ${settings?.communityUploadRejectedCount ?? 0} rejected`;
}
