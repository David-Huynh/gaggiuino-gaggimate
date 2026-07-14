const COMMUNITY_UPLOAD_RUNTIME_FIELDS = [
  'rlCommunityUploadEnabled',
  'rlCommunityUploadPrompted',
  'rlUploadBaseUrl',
  'rlUploadCredentialConfigured',
  'communityUploadRequested',
  'communityUploadEffective',
  'communityUploadConfigured',
  'communityUploadStatus',
  'communityUploadSummary',
  'communityUploadStorageBackend',
  'communityUploadStorageAvailable',
  'communityUploadPendingCount',
  'communityUploadRetryCount',
  'communityUploadRejectedCount',
];

export function mergeCommunityUploadRuntimeStatus(formData, status) {
  const next = { ...formData };
  if (!status || typeof status !== 'object') {
    return next;
  }

  for (const field of COMMUNITY_UPLOAD_RUNTIME_FIELDS) {
    if (Object.hasOwn(status, field)) {
      next[field] = status[field];
    }
  }

  return next;
}

export function recordCommunityUploadConsent(formData, enabled) {
  return {
    ...formData,
    rlCommunityUploadEnabled: enabled,
    rlCommunityUploadPrompted: true,
  };
}
