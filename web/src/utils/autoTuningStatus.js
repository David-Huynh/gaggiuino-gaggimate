export function communityUploadPipelineText(settings) {
  if (settings?.rlCommunityUploadEffective) {
    return 'Enabled';
  }
  if (settings?.rlCommunityUploadEnabled) {
    return settings?.rlUploadBaseUrl ? 'Waiting for device registration' : 'Needs Supabase URL';
  }
  return 'Disabled';
}

export function runtimeCommunityUploadText(settings) {
  if (!settings?.rlRuntimeHealthCommunityUploadRequested) {
    return 'Off';
  }
  if (!settings?.rlUploadBaseUrl) {
    return 'Needs Supabase URL';
  }
  return settings?.rlRuntimeHealthUploadConfigured ? 'Configured' : 'Registration pending';
}
