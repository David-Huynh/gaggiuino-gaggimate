import { useCallback, useContext, useEffect, useRef, useState } from 'preact/hooks';
import { msg } from '@lingui/core/macro';
import { Trans, useLingui } from '@lingui/react/macro';
import { ApiServiceContext, machine } from '../../../services/ApiService.js';
import { downloadJson } from '../../../utils/download.js';
import { SystemTabSkeleton } from '../../../components/skeletons/SettingsSkeletons.jsx';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { faCheck } from '@fortawesome/free-solid-svg-icons/faCheck';
import { faCaretRight } from '@fortawesome/free-solid-svg-icons/faCaretRight';
import { Spinner } from '../../../components/Spinner.jsx';
import Section from '../../../components/Card.jsx';
import { PatchNotes } from '../PatchNotes.jsx';

const imageUrlToBase64 = async blob => {
  return new Promise((onSuccess, onError) => {
    try {
      const reader = new FileReader();
      reader.onload = function () {
        onSuccess(this.result);
      };
      reader.onerror = function () {
        onError(new Error('FileReader failed to read image blob.'));
      };
      reader.readAsDataURL(blob);
    } catch (e) {
      onError(e instanceof Error ? e : new Error(String(e)));
    }
  });
};

const getRssiStatusClass = rssi => {
  if (rssi < -90) return 'status-error';
  if (rssi < -80) return 'status-warning';
  return 'status-success';
};

const clampProgress = value => {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? Math.max(0, Math.min(100, Math.round(numeric))) : 0;
};

// A wire has no signal strength to grade; only BLE can be weak.
const getLinkStatusClass = (link, rssi) => {
  if (link === 'none') return 'status-error';
  return link === 'ble' ? getRssiStatusClass(rssi) : 'status-success';
};

function OtaProgressView({ phase, progress, error, headless, onBack }) {
  const { t, i18n } = useLingui();
  const finished = phase === 4;
  const failed = phase === 5;
  const getOtaPhaseText = p => {
    switch (p) {
      case 1:
        return headless ? t`Updating Headless firmware` : t`Updating Display firmware`;
      case 2:
        return headless ? t`Updating Headless filesystem` : t`Updating Display filesystem`;
      case 3:
        return t`Updating controller firmware`;
      case 5:
        return t`Update failed`;
      case 6:
        return t`Reconnecting to resume the firmware download`;
      default:
        return t`Finished`;
    }
  };

  return (
    <div className='flex flex-col items-center gap-4 py-12'>
      {!failed && !finished && <Spinner size={8} />}
      <span className='text-base-content text-xl font-medium'>{getOtaPhaseText(phase)}</span>
      {failed ? (
        <span role='alert' className='text-base-content/70 max-w-md text-center'>
          {i18n._(otaErrorMessage(error))}
        </span>
      ) : (
        <span className='text-base-content text-lg font-medium'>{finished ? 100 : progress}%</span>
      )}
      {!failed && (<>
      <div className='bg-base-300 h-3 w-full max-w-md overflow-hidden rounded'>
        <div
          className='bg-primary h-full transition-all duration-300'
          style={{ width: `${finished ? 100 : progress}%` }}
        />
      </div>
      <span className='text-base-content/70 max-w-md text-center text-sm'>
        Keep the machine powered. The update continues after this browser page is closed.
      </span>
      </>)}
      {failed && (
        <button type='button' className='btn btn-primary' onClick={onBack}>
          <Trans>Back to updates</Trans>
        </button>
      )}
      {finished && (
        <a href='/' className='btn btn-primary'>
          <Trans>Back</Trans>
        </a>
      )}
    </div>
  );
}

function otaErrorMessage(error) {
  switch (error) {
    case 'no_ota_partition':
      return msg`This flash layout requires a USB firmware update.`;
    case 'image_too_large':
      return msg`This firmware is too large for the update slot.`;
    case 'image_missing':
      return msg`The selected release does not contain firmware for this board.`;
    case 'image_invalid':
      return msg`Firmware verification failed. The current ESP32 firmware is still installed.`;
    case 'flash_failed':
      return msg`Writing the firmware failed. The current ESP32 firmware is still installed.`;
    case 'machine_busy':
      return msg`Finish the current machine operation before updating.`;
    case 'no_update':
      return msg`There is no newer firmware in the selected channel.`;
    case 'check_unavailable':
      return msg`Wait for the update check and confirm the machine is connected to Wi-Fi.`;
    case 'ota_unavailable':
      return msg`This board cannot be updated over Wi-Fi.`;
    case 'already_updating':
      return msg`An update is already running.`;
    case 'controller_update_failed':
      return msg`The controller update failed. Check the controller connection and retry.`;
    default:
      return msg`The firmware download could not be completed. Check the connection and retry.`;
  }
}

function StorageAndMemorySection({ formData }) {
  const { t } = useLingui();
  return (
    <Section title={t`Storage & Memory`} className='h-full'>
      <div className='grid grid-cols-1 gap-6'>
        {formData.spiffsTotal !== undefined && (
          <div className='flex flex-col space-y-2'>
            <span className='text-base-content/70 text-sm font-medium'>
              <Trans>Storage (LittleFS)</Trans>
            </span>
            <div className='bg-base-300 h-3 w-full overflow-hidden rounded'>
              <div
                className='bg-primary h-full transition-all'
                style={{ width: `${formData.spiffsUsedPct || 0}%` }}
              />
            </div>
            <div className='text-base-content/60 text-xs'>
              {((formData.spiffsUsed || 0) / 1024).toFixed(1)} KB /{' '}
              {(formData.spiffsTotal / 1024).toFixed(1)} KB ({formData.spiffsUsedPct}%)
            </div>
          </div>
        )}

        {formData.sdTotal !== undefined && (
          <div className='flex flex-col space-y-2'>
            <span className='text-base-content/70 text-sm font-medium'>
              <Trans>Storage (SD-Card)</Trans>
            </span>
            <div className='bg-base-300 h-3 w-full overflow-hidden rounded'>
              <div
                className='bg-primary h-full transition-all'
                style={{ width: `${formData.sdUsedPct || 0}%` }}
              />
            </div>
            <div className='text-base-content/60 text-xs'>
              {((formData.sdUsed || 0) / 1024 / 1024).toFixed(1)} MB /{' '}
              {(formData.sdTotal / 1024 / 1024).toFixed(1)} MB ({formData.sdUsedPct}%)
            </div>
          </div>
        )}

        {formData.heapTotal !== undefined && (
          <div className='flex flex-col space-y-2'>
            <span className='text-base-content/70 text-sm font-medium'>
              <Trans>Free Memory (Heap)</Trans>
            </span>
            <div className='bg-base-300 h-3 w-full overflow-hidden rounded'>
              <div
                className='bg-primary h-full transition-all'
                style={{
                  width: `${formData.heapTotal > 0 ? ((formData.heapTotal - formData.heapFree) / formData.heapTotal) * 100 : 0}%`,
                }}
              />
            </div>
            <div className='text-base-content/60 text-xs'>
              {((formData.heapTotal - formData.heapFree || 0) / 1024).toFixed(1)} kB /{' '}
              {(formData.heapTotal / 1024).toFixed(1)} kB (
              {(formData.heapTotal > 0
                ? ((formData.heapTotal - formData.heapFree) / formData.heapTotal) * 100
                : 0
              ).toFixed(2)}
              %) ({t`Frag:`}{' '}
              {(formData.heapFree > 0
                ? 100 - (formData.heapLargest * 100) / formData.heapFree
                : 0
              ).toFixed(2)}
              %)
            </div>
          </div>
        )}

        {formData.psramTotal > 0 && (
          <div className='flex flex-col space-y-2'>
            <span className='text-base-content/70 text-sm font-medium'>Memory (PSRAM)</span>
            <div className='bg-base-300 h-3 w-full overflow-hidden rounded'>
              <div
                className='bg-primary h-full transition-all'
                style={{
                  width: `${((formData.psramTotal - formData.psramFree) / formData.psramTotal) * 100 || 0}%`,
                }}
              />
            </div>
            <div className='text-base-content/60 text-xs'>
              {((formData.psramTotal - formData.psramFree || 0) / 1024 / 1024).toFixed(2)} MB /{' '}
              {(formData.psramTotal / 1024 / 1024).toFixed(2)} MB
            </div>
          </div>
        )}
      </div>
    </Section>
  );
}

function MaintenanceSection({
  downloadSupportData,
  onHistoryRebuild,
  rebuilding,
  rebuilt,
  rebuildProgress,
}) {
  const { t } = useLingui();
  const current = rebuildProgress.current;
  const total = rebuildProgress.total;
  return (
    <Section title={t`Maintenance & Support`} className='h-full'>
      <div className='flex flex-col flex-wrap gap-4 sm:flex-row'>
        <button type='button' className='btn btn-outline btn-sm' onClick={downloadSupportData}>
          <Trans>Download Support Data</Trans>
        </button>

        <a href='/setup' className='btn btn-outline btn-sm'>
          <Trans>Run Setup Wizard</Trans>
        </a>

        <button
          type='button'
          className='btn btn-outline btn-sm'
          onClick={onHistoryRebuild}
          disabled={rebuilding}
        >
          <Trans>Rebuild Shot History</Trans>
          {rebuilding && (
            <>
              <Spinner size={4} className='ml-2' />
              {rebuildProgress.total > 0 && (
                <span className='ml-2 text-xs'>
                  {rebuildProgress.current}/{rebuildProgress.total}
                </span>
              )}
            </>
          )}
          {rebuilt && (
            <span className='text-success ml-2'>
              <FontAwesomeIcon icon={faCheck} />
            </span>
          )}
        </button>
      </div>

      {rebuilding && (
        <div className='mt-4 max-w-md'>
          <div className='text-base-content/70 mb-1 text-xs'>
            {rebuildProgress.status === 'starting' ||
            rebuildProgress.status === 'scanning' ||
            rebuildProgress.total === 0
              ? t`Scanning shot history files...`
              : t`Processing shot history files (${current}/${total})`}
          </div>
          <div className='bg-base-300 h-2 w-full overflow-hidden rounded'>
            <div
              className={`h-full transition-all duration-300 ${
                rebuildProgress.total === 0 ? 'bg-primary animate-pulse' : 'bg-primary'
              }`}
              style={{
                width:
                  rebuildProgress.total > 0
                    ? `${(rebuildProgress.current / rebuildProgress.total) * 100}%`
                    : '30%',
              }}
            />
          </div>
        </div>
      )}
    </Section>
  );
}

export function SystemTab() {
  const { t, i18n } = useLingui();
  const apiService = useContext(ApiServiceContext);
  const [isLoading, setIsLoading] = useState(true);
  const [submitting, setSubmitting] = useState(false);
  const [formData, setFormData] = useState({});
  const [phase, setPhase] = useState(0);
  const [progress, setProgress] = useState(0);
  const [updateError, setUpdateError] = useState('');
  const [startingUpdate, setStartingUpdate] = useState(false);
  const connected = machine.value.connected;
  const rssi = machine.value.status.rssi;
  // Firmware without the link field only ever talks BLE.
  const controllerLink = machine.value.status.controllerLink ?? 'ble';
  const lat = machine.value.status.lat;
  const rtx = machine.value.status.rtx;
  const headless = !!machine.value.capabilities.headless;

  const downloadSupportData = useCallback(async () => {
    try {
      const settingsResponse = await fetch(`/api/settings`);
      const data = await settingsResponse.json();
      delete data.wifiPassword;
      delete data.haPassword;
      const coredumpBlob = await fetch(`/api/core-dump`).then(r => r.blob());
      let coredump = await imageUrlToBase64(coredumpBlob);
      coredump = coredump.substring(coredump.indexOf('base64,') + 7);
      const supportFile = {
        settings: data,
        versions: formData,
        coredump,
      };
      const ts = Date.now();
      downloadJson(supportFile, `support-${ts}.dat`);
    } catch (e) {
      console.error('Error downloading support data:', e);
    }
  }, [formData]);

  useEffect(() => {
    const listenerId = apiService.on('res:ota-settings', msg => {
      setFormData(msg);
      setIsLoading(false);
      setSubmitting(false);
      if (msg.phase > 0 && (msg.updating || msg.phase >= 4)) {
        setPhase(msg.phase);
        setProgress(clampProgress(msg.progress));
        setUpdateError(msg.updateError || '');
      }
    });
    return () => {
      apiService.off('res:ota-settings', listenerId);
    };
  }, [apiService]);

  useEffect(() => {
    const listenerId = apiService.on('evt:ota-progress', msg => {
      const nextPhase = Number(msg.phase) || 0;
      const nextProgress = nextPhase === 4 ? 100 : clampProgress(msg.progress);
      setPhase(nextPhase);
      setProgress(nextProgress);
      setUpdateError(msg.error || '');
    });
    return () => {
      apiService.off('evt:ota-progress', listenerId);
    };
  }, [apiService]);

  useEffect(() => {
    const listenerId = apiService.on('evt:history-rebuild-progress', msg => {
      setRebuildProgress({
        total: msg.total || 0,
        current: msg.current || 0,
        status: msg.status || '',
      });

      if (msg.status === 'completed' || msg.status === 'error') {
        setRebuilding(false);
        setRebuilt(msg.status === 'completed');
      }
    });
    return () => {
      apiService.off('evt:history-rebuild-progress', listenerId);
    };
  }, [apiService]);

  useEffect(() => {
    if (connected) {
      apiService.send({ tp: 'req:ota-settings' });
    }
  }, [apiService, connected]);

  const formRef = useRef();

  const onSubmit = useCallback(
    async e => {
      e.preventDefault();
      setSubmitting(true);
      const form = formRef.current;
      const data = new FormData(form);
      apiService.send({ tp: 'req:ota-settings', update: true, channel: data.get('channel') });
    },
    [apiService],
  );

  const onUpdate = useCallback(
    async component => {
      setStartingUpdate(true);
      setUpdateError('');
      try {
        const result = await apiService.request({ tp: 'req:ota-start', cp: component });
        if (result.error) setUpdateError(result.error);
      } catch {
        setUpdateError('check_unavailable');
      } finally {
        setStartingUpdate(false);
      }
    },
    [apiService],
  );

  const [rebuilding, setRebuilding] = useState(false);
  const [rebuilt, setRebuilt] = useState(false);
  const [rebuildProgress, setRebuildProgress] = useState({ total: 0, current: 0, status: '' });

  const onHistoryRebuild = useCallback(async () => {
    setRebuilt(false);
    setRebuilding(true);
    setRebuildProgress({ total: 0, current: 0, status: 'starting' });
    apiService.send({ tp: 'req:history:rebuild' });
  }, [apiService]);

  if (phase > 0) {
    return (
      <OtaProgressView
        phase={phase}
        progress={progress}
        error={updateError}
        headless={headless}
        onBack={() => {
          setPhase(0);
          setUpdateError('');
        }}
      />
    );
  }

  if (isLoading) {
    return <SystemTabSkeleton />;
  }

  const canUpdateController = formData.controllerOtaEnabled !== false;
  const updateBlocked = !connected || startingUpdate || submitting || formData.checking || formData.updating;
  return (
    <div className='space-y-4 sm:space-y-6 lg:grid lg:grid-cols-2 lg:gap-4'>
      {/* Firmware updates channel */}
      <Section title={t`System Version & Updates`} className='h-full'>
        <form ref={formRef} onSubmit={onSubmit} className='space-y-4'>
          <div className='form-control max-w-md'>
            <label htmlFor='channel' className='mb-2 block text-sm font-medium'>
              <Trans>Update Channel</Trans>
            </label>
            <div className='mb-2 flex w-full items-center gap-2'>
              <select id='channel' name='channel' className='select select-bordered grow'>
                <option value='latest' selected={formData.channel === 'latest'}>
                  {t({ message: 'Stable', context: 'update channel' })}
                </option>
                <option value='nightly' selected={formData.channel === 'nightly'}>
                  {t({ message: 'Nightly', context: 'update channel' })}
                </option>
              </select>
              <button type='submit' className='btn btn-secondary' disabled={submitting}>
                <Trans>Save Channel & Refresh</Trans>
              </button>
            </div>
          </div>
        </form>
        {formData.checking && <p role='status'><Trans>Checking for updates…</Trans></p>}
        {!formData.checking && formData.checkError === 'no_stable_release' && (
          <p role='status'><Trans>This fork has no Stable release. Select Nightly to check its published builds.</Trans></p>
        )}
        {!formData.checking && formData.checkError && formData.checkError !== 'no_stable_release' && (
          <p role='status'><Trans>The release version could not be checked. Refresh when the machine is online.</Trans></p>
        )}
        {updateError && <p role='alert'>{i18n._(otaErrorMessage(updateError))}</p>}

        <div className='border-base-content/5 mt-6 grid grid-cols-1 gap-6 border-t pt-6 md:grid-cols-2'>
          <div className='flex flex-col space-y-1'>
            <span className='text-base-content/70 text-sm font-medium'>
              <Trans>Hardware</Trans>
            </span>
            <span className='text-base-content font-semibold'>{formData.hardware}</span>
          </div>

          <div className='flex flex-col space-y-1'>
            <span className='text-base-content/70 text-sm font-medium'>
              <Trans>Controller Connection</Trans>
            </span>
            <span className='text-base-content flex items-center gap-2 font-semibold'>
              {controllerLink === 'uart' && (
                <Trans>
                  UART (Roundtrip: {lat} ms, Resends: {rtx})
                </Trans>
              )}
              {controllerLink === 'ble' && (
                <Trans>
                  Bluetooth, {rssi}dB (Roundtrip: {lat} ms, Resends: {rtx})
                </Trans>
              )}
              {controllerLink === 'none' && <Trans>Not connected</Trans>}
              <span
                className={`indicator-item status ${getLinkStatusClass(controllerLink, rssi)}`}
              />
            </span>
          </div>

          <div className='flex flex-col space-y-2'>
            <span className='text-base-content/70 text-sm font-medium'>
              <Trans>Controller Version</Trans>
            </span>
            <div className='flex flex-wrap items-center gap-2'>
              <span className='text-base-content font-semibold break-all'>
                {formData.controllerVersion}
              </span>
              {formData.controllerUpdateAvailable && (
                <>
                  <FontAwesomeIcon icon={faCaretRight} className='text-primary' />
                  <span className='text-primary text-sm font-bold'>{formData.latestVersion}</span>
                </>
              )}
            </div>
            {!canUpdateController && <span className='text-sm'><Trans>Controller updates use ST-Link.</Trans></span>}
          </div>

          <div className='flex flex-col space-y-2'>
            <span className='text-base-content/70 text-sm font-medium'>
              {headless ? <Trans>Headless Version</Trans> : <Trans>Display Version</Trans>}
            </span>
            <div className='flex flex-wrap items-center gap-2'>
              <span className='text-base-content font-semibold break-all'>
                {formData.displayVersion}
              </span>
              {formData.displayUpdateAvailable && (
                <>
                  <FontAwesomeIcon icon={faCaretRight} className='text-primary' />
                  <span className='text-primary text-sm font-bold'>{formData.latestVersion}</span>
                </>
              )}
            </div>
          </div>
          {/* Own grid row, so the update buttons below stay on one line */}
          <div className='flex md:col-span-2'>
            <PatchNotes
              repository={formData.repository}
              channel={formData.channel}
              // shortcut: when both are behind, the display wins even if the controller is older; both share one build.
              installedVersion={
                formData.displayUpdateAvailable || !formData.controllerUpdateAvailable
                  ? formData.displayVersion
                  : formData.controllerVersion
              }
              latestVersion={formData.latestVersion}
            />
          </div>
          {canUpdateController && (
            <button
              type='button'
              className='btn btn-secondary btn-sm'
              disabled={!formData.controllerUpdateAvailable || updateBlocked}
              onClick={() => onUpdate('controller')}
            >
              <Trans>Update Controller</Trans>
            </button>
          )}
          <button
            type='button'
            className='btn btn-secondary btn-sm'
            disabled={!formData.displayUpdateAvailable || updateBlocked}
            onClick={() => onUpdate('display')}
          >
            {headless ? <Trans>Update Headless</Trans> : <Trans>Update Display</Trans>}
          </button>
          {canUpdateController && (
            <div className='flex flex-col space-y-2 md:col-span-2'>
              <button
                type='button'
                className='btn btn-secondary btn-sm flex-auto'
                disabled={
                  (!formData.controllerUpdateAvailable && !formData.displayUpdateAvailable) ||
                  updateBlocked
                }
                onClick={() => onUpdate('both')}
              >
                <Trans>Update both</Trans>
              </button>
            </div>
          )}
        </div>

        <div className='alert alert-warning mt-6'>
          <span>
            <Trans>
              Make sure to backup your profiles from the profiles screen before updating the
              display.
            </Trans>
          </span>
        </div>
      </Section>

      <StorageAndMemorySection formData={formData} />

      <MaintenanceSection
        downloadSupportData={downloadSupportData}
        onHistoryRebuild={onHistoryRebuild}
        rebuilding={rebuilding}
        rebuilt={rebuilt}
        rebuildProgress={rebuildProgress}
      />
    </div>
  );
}
