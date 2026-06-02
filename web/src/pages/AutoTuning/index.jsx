import { useCallback, useContext, useEffect, useState } from 'preact/hooks';
import { ApiServiceContext } from '../../services/ApiService.js';
import { Spinner } from '../../components/Spinner.jsx';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { faPlus } from '@fortawesome/free-solid-svg-icons/faPlus';
import { faPlay } from '@fortawesome/free-solid-svg-icons/faPlay';
import { faPause } from '@fortawesome/free-solid-svg-icons/faPause';
import { faRotate } from '@fortawesome/free-solid-svg-icons/faRotate';
import { faChevronRight } from '@fortawesome/free-solid-svg-icons/faChevronRight';
import { faTrashCan } from '@fortawesome/free-solid-svg-icons/faTrashCan';
import { faBan } from '@fortawesome/free-solid-svg-icons/faBan';
import { faTriangleExclamation } from '@fortawesome/free-solid-svg-icons/faTriangleExclamation';
import { faUpload } from '@fortawesome/free-solid-svg-icons/faUpload';

function Panel({ title, subtitle, action, children, className = '' }) {
  return (
    <section className={`border-base-300 bg-base-100 rounded-md border p-4 ${className}`}>
      {(title || action) && (
        <div className='mb-4 flex flex-col gap-2 sm:flex-row sm:items-start sm:justify-between'>
          <div className='min-w-0'>
            {title && <h2 className='text-base font-semibold sm:text-lg'>{title}</h2>}
            {subtitle && (
              <div className='text-base-content/60 mt-1 text-xs break-words sm:text-sm'>
                {subtitle}
              </div>
            )}
          </div>
          {action && <div className='flex shrink-0 flex-wrap gap-2'>{action}</div>}
        </div>
      )}
      {children}
    </section>
  );
}

function StatCard({ label, value, tone = 'neutral' }) {
  const toneClass =
    {
      success: 'border-success/40 bg-success/10',
      warning: 'border-warning/40 bg-warning/10',
      error: 'border-error/40 bg-error/10',
      primary: 'border-primary/40 bg-primary/10',
      neutral: 'border-base-300 bg-base-200/40',
    }[tone] || 'border-base-300 bg-base-200/40';

  return (
    <div className={`min-w-0 rounded-md border p-3 ${toneClass}`}>
      <div className='text-base-content/60 text-xs tracking-wide uppercase'>{label}</div>
      <div className='text-base-content mt-1 min-h-6 text-lg leading-tight font-semibold break-words'>
        {value || 'None'}
      </div>
    </div>
  );
}

function InfoRow({ label, value, compact = false }) {
  return (
    <div className='border-base-300/70 flex min-w-0 flex-col gap-1 border-b py-2 last:border-b-0 sm:flex-row sm:items-center sm:justify-between'>
      <span className='text-base-content/60 shrink-0 text-xs tracking-wide uppercase'>{label}</span>
      <span
        className={`min-w-0 text-sm font-medium break-words ${
          compact ? 'text-base-content/70 font-mono text-xs' : 'text-base-content'
        }`}
      >
        {value || 'None'}
      </span>
    </div>
  );
}

function StatusPill({ children, tone = 'neutral' }) {
  const toneClass =
    {
      success: 'badge-success',
      warning: 'badge-warning',
      error: 'badge-error',
      primary: 'badge-primary',
      neutral: 'badge-neutral',
    }[tone] || 'badge-neutral';

  return <span className={`badge badge-sm whitespace-nowrap ${toneClass}`}>{children}</span>;
}

function bestRecipeText(settings) {
  if (settings?.rlBestKnownRecipe) {
    return settings.rlBestKnownRecipe;
  }
  return 'None yet';
}

function shortId(value) {
  if (!value) {
    return 'None';
  }
  if (value.length <= 24) {
    return value;
  }
  return `${value.slice(0, 12)}...${value.slice(-8)}`;
}

function localOptimizationLabel(localOn, paused) {
  if (paused) {
    return 'Paused';
  }
  return localOn ? 'On' : 'Off for shots';
}

function localOptimizationTone(localOn, paused) {
  if (paused) {
    return 'warning';
  }
  return localOn ? 'success' : 'neutral';
}

export function AutoTuning() {
  const apiService = useContext(ApiServiceContext);
  const [settings, setSettings] = useState(null);
  const [loading, setLoading] = useState(true);
  const [beanName, setBeanName] = useState('');
  const [busy, setBusy] = useState(false);

  const loadSettings = useCallback(async () => {
    const response = await fetch('/api/settings');
    const data = await response.json();
    setSettings(data);
    setLoading(false);
    if (!beanName && data.rlBeanContextName) {
      setBeanName(data.rlBeanContextName);
    }
  }, [beanName]);

  useEffect(() => {
    loadSettings();
  }, [loadSettings]);

  useEffect(() => {
    if (!apiService) {
      return undefined;
    }
    const listenerId = apiService.on('evt:rl:status', message => {
      setSettings(current => ({ ...(current || {}), ...message }));
    });
    return () => apiService.off('evt:rl:status', listenerId);
  }, [apiService]);

  const run = useCallback(
    async (tp, payload = {}) => {
      if (!apiService) {
        return;
      }
      setBusy(true);
      try {
        await apiService.request({ tp, ...payload });
        await loadSettings();
      } finally {
        setBusy(false);
      }
    },
    [apiService, loadSettings],
  );

  if (loading) {
    return (
      <div className='flex w-full items-center justify-center py-16'>
        <Spinner size={8} />
      </div>
    );
  }

  const enabled = settings?.homeAssistant && settings?.rlRatingEnabled;
  if (!enabled) {
    return (
      <div className='space-y-4'>
        <h1 className='text-2xl font-bold sm:text-3xl'>Auto Tuning</h1>
        <div className='border-base-300 bg-base-100 rounded-md border p-4'>
          Enable Home Assistant/MQTT and EspressoRL Auto Tuning in Settings to use this page.
        </div>
      </div>
    );
  }

  const contexts = settings?.rlBeanContexts || [];
  const activeName = settings?.rlBeanContextName || 'No bean selected';
  const localOn = !!settings?.rlLocalOptimizationEnabled;
  const paused = !!settings?.rlOptimizationPaused;
  const uploadRejectedCount = settings?.rlUploadQueueRejectedCount ?? 0;
  const hasRejectedUploads = uploadRejectedCount > 0;
  const lastRejectedText = settings?.rlUploadQueueLastRejectedRecordId
    ? `${settings.rlUploadQueueLastRejectedRecordId}: ${
        settings?.rlUploadQueueLastRejectedError || 'Rejected'
      }`
    : 'No rejected upload snapshots';

  return (
    <div className='mx-auto flex w-full max-w-6xl flex-col gap-4'>
      <div className='flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between'>
        <div className='min-w-0'>
          <h1 className='text-2xl font-bold sm:text-3xl'>Auto Tuning</h1>
          <div className='mt-2 flex flex-wrap items-center gap-2'>
            <StatusPill tone={localOptimizationTone(localOn, paused)}>
              Local optimization {localOptimizationLabel(localOn, paused)}
            </StatusPill>
            <StatusPill tone={settings?.rlCommunityUploadEnabled ? 'primary' : 'neutral'}>
              Community upload {settings?.rlCommunityUploadEnabled ? 'On' : 'Off'}
            </StatusPill>
          </div>
        </div>
        <div className='grid grid-cols-1 gap-2 sm:grid-cols-2 lg:flex lg:justify-end'>
          {paused ? (
            <button
              type='button'
              className='btn btn-primary btn-sm'
              disabled={busy}
              onClick={() => run('req:rl:optimization:resume')}
            >
              <FontAwesomeIcon icon={faPlay} />
              Resume
            </button>
          ) : (
            <button
              type='button'
              className='btn btn-outline btn-sm'
              disabled={busy}
              onClick={() => run('req:rl:optimization:pause')}
            >
              <FontAwesomeIcon icon={faPause} />
              Pause
            </button>
          )}
        </div>
      </div>

      <div className='grid grid-cols-1 gap-4 lg:grid-cols-3'>
        <Panel className='lg:col-span-2' title='Active Session'>
          <div className='grid grid-cols-1 gap-4 md:grid-cols-[minmax(0,1fr)_220px]'>
            <div className='min-w-0'>
              <div className='text-base-content/60 text-xs tracking-wide uppercase'>Optimizing</div>
              <div className='mt-1 text-2xl leading-tight font-semibold break-words'>
                {activeName}
              </div>
              <div className='mt-3 flex flex-wrap gap-2'>
                <StatusPill tone='primary'>{settings?.rlMode || 'No mode yet'}</StatusPill>
                <StatusPill tone={localOptimizationTone(localOn, paused)}>
                  {localOptimizationLabel(localOn, paused)}
                </StatusPill>
              </div>
            </div>
            <div className='bg-base-200/60 min-w-0 rounded-md p-3'>
              <InfoRow label='Context' value={shortId(settings?.rlBeanContextId)} compact />
              <InfoRow
                label='Recommendation'
                value={shortId(settings?.rlLastRecommendationId)}
                compact
              />
            </div>
          </div>
          <div className='bg-base-200/60 mt-4 rounded-md p-3'>
            <InfoRow label='Best recipe' value={bestRecipeText(settings)} />
          </div>
        </Panel>

        <Panel title='Progress'>
          <div className='grid grid-cols-2 gap-3'>
            <StatCard label='Local shots' value={(settings?.rlLocalShotCount ?? 0).toString()} />
            <StatCard label='Rated shots' value={(settings?.rlRatedShotCount ?? 0).toString()} />
            <StatCard label='Queued' value={(settings?.rlUploadQueueCount ?? 0).toString()} />
            <StatCard
              label='Rejected'
              value={uploadRejectedCount.toString()}
              tone={hasRejectedUploads ? 'warning' : 'neutral'}
            />
          </div>
        </Panel>
      </div>

      <Panel title='Bean Management'>
        <div className='grid grid-cols-1 gap-3 lg:grid-cols-[minmax(0,1fr)_auto_auto_auto]'>
          <label className='input input-bordered flex min-w-0 items-center gap-2'>
            <input
              type='text'
              className='min-w-0 grow'
              value={beanName}
              placeholder='Bean name'
              onInput={event => setBeanName(event.currentTarget.value)}
            />
          </label>
          <button
            type='button'
            className='btn btn-primary w-full lg:w-auto'
            disabled={busy}
            onClick={() => run('req:rl:context:start-bean', { name: beanName })}
          >
            <FontAwesomeIcon icon={faPlus} />
            New Bean
          </button>
          <button
            type='button'
            className='btn btn-outline w-full lg:w-auto'
            disabled={busy || !settings?.rlBeanContextId}
            onClick={() => run('req:rl:context:start-bag')}
          >
            <FontAwesomeIcon icon={faChevronRight} />
            New Bag
          </button>
          <button
            type='button'
            className='btn btn-outline w-full lg:w-auto'
            disabled={busy}
            onClick={() => run('req:rl:context:reset')}
          >
            <FontAwesomeIcon icon={faRotate} />
            Reset
          </button>
        </div>
      </Panel>

      <div className='grid grid-cols-1 gap-4 xl:grid-cols-2'>
        <Panel
          title='Upload Recovery'
          subtitle={lastRejectedText}
          action={
            <button
              type='button'
              className='btn btn-outline btn-sm'
              disabled={busy || !hasRejectedUploads}
              onClick={() => run('req:rl:upload:requeue', { limit: 50 })}
            >
              <FontAwesomeIcon icon={faUpload} />
              Retry Valid
            </button>
          }
        >
          <div className='text-base-content/70 text-sm break-words'>
            {hasRejectedUploads
              ? `${uploadRejectedCount} rejected snapshot${
                  uploadRejectedCount === 1 ? '' : 's'
                } waiting for review.`
              : 'Upload queue is clear.'}
          </div>
        </Panel>

        <Panel
          title='Last Shot Correction'
          subtitle={settings?.rlLastShotId || 'No shot available yet'}
        >
          <div className='grid grid-cols-2 gap-2 sm:grid-cols-3'>
            <button
              type='button'
              className='btn btn-outline btn-sm min-h-10 whitespace-normal'
              disabled={busy || !settings?.rlLastShotId}
              onClick={() =>
                run('req:rl:shot:correction', {
                  shot_id: settings?.rlLastShotId,
                  exclude_from_local_optimization: true,
                  correction_tags: ['changed_manually'],
                })
              }
            >
              <FontAwesomeIcon icon={faBan} />
              Exclude
            </button>
            <button
              type='button'
              className='btn btn-outline btn-sm min-h-10 whitespace-normal'
              disabled={busy || !settings?.rlLastShotId}
              onClick={() =>
                run('req:rl:shot:correction', {
                  shot_id: settings?.rlLastShotId,
                  exclude_from_local_optimization: true,
                  correction_tags: ['bad_puck_prep', 'channeling_suspected'],
                })
              }
            >
              <FontAwesomeIcon icon={faTriangleExclamation} />
              Bad Prep
            </button>
            <button
              type='button'
              className='btn btn-outline btn-sm min-h-10 whitespace-normal'
              disabled={busy || !settings?.rlLastShotId}
              onClick={() =>
                run('req:rl:shot:correction', {
                  shot_id: settings?.rlLastShotId,
                  grind_followed: false,
                  correction_tags: ['did_not_follow_grind', 'changed_manually'],
                })
              }
            >
              <FontAwesomeIcon icon={faBan} />
              Grind not followed
            </button>
            <button
              type='button'
              className='btn btn-outline btn-sm min-h-10 whitespace-normal'
              disabled={busy || !settings?.rlLastShotId}
              onClick={() =>
                run('req:rl:shot:correction', {
                  shot_id: settings?.rlLastShotId,
                  dose_followed: false,
                  correction_tags: ['did_not_follow_dose', 'changed_manually'],
                })
              }
            >
              <FontAwesomeIcon icon={faBan} />
              Dose not followed
            </button>
            <button
              type='button'
              className='btn btn-outline btn-sm min-h-10 whitespace-normal sm:col-span-1'
              disabled={busy || !settings?.rlLastShotId}
              onClick={() =>
                run('req:rl:shot:correction', {
                  shot_id: settings?.rlLastShotId,
                  yield_followed: false,
                  correction_tags: ['did_not_follow_yield', 'changed_manually'],
                })
              }
            >
              <FontAwesomeIcon icon={faBan} />
              Yield not followed
            </button>
          </div>
        </Panel>
      </div>

      <Panel title='Bean Contexts'>
        <div className='grid grid-cols-1 gap-3'>
          {contexts.length === 0 && (
            <div className='bg-base-200/60 rounded-md p-4 text-sm opacity-70'>
              No bean contexts yet.
            </div>
          )}
          {contexts.map(context => (
            <div
              key={context.id}
              className='border-base-300 bg-base-100 flex flex-col gap-3 rounded-md border p-4 sm:flex-row sm:items-center'
            >
              <div className='min-w-0 flex-1'>
                <div className='flex flex-wrap items-center gap-2'>
                  <span className='font-semibold'>{context.name || 'Unnamed bean'}</span>
                  <span className='badge badge-neutral'>Bag {context.bag_index || 1}</span>
                  {context.active && <span className='badge badge-primary'>Active</span>}
                  {context.status === 'retired' && <span className='badge'>Retired</span>}
                </div>
                <div className='text-base-content/50 mt-1 font-mono text-xs break-all'>
                  {context.id}
                </div>
              </div>
              <div className='grid grid-cols-2 gap-2 sm:flex sm:shrink-0'>
                <button
                  type='button'
                  className='btn btn-outline btn-sm w-full sm:w-auto'
                  disabled={busy || context.active}
                  onClick={() => run('req:rl:context:switch', { id: context.id })}
                >
                  <FontAwesomeIcon icon={faPlay} />
                  Switch
                </button>
                <button
                  type='button'
                  className='btn btn-error btn-outline btn-sm w-full sm:w-auto'
                  disabled={busy || context.status === 'retired'}
                  onClick={() => run('req:rl:context:retire', { id: context.id })}
                >
                  <FontAwesomeIcon icon={faTrashCan} />
                  Retire
                </button>
              </div>
            </div>
          ))}
        </div>
      </Panel>
    </div>
  );
}
