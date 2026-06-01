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

function StatusCell({ label, value }) {
  return (
    <div className='border-base-300 rounded-md border p-3'>
      <div className='text-base-content/60 text-xs uppercase'>{label}</div>
      <div className='text-base-content mt-1 min-h-6 break-words text-sm font-semibold'>
        {value || 'None'}
      </div>
    </div>
  );
}

function bestRecipeText(settings) {
  if (settings?.rlBestKnownRecipe) {
    return settings.rlBestKnownRecipe;
  }
  return 'None yet';
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

  return (
    <div className='space-y-5'>
      <div className='flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between'>
        <div>
          <h1 className='text-2xl font-bold sm:text-3xl'>Auto Tuning</h1>
          <div className='text-base-content/60 text-sm'>Optimizing: {activeName}</div>
        </div>
        <div className='flex gap-2'>
          <button
            type='button'
            className='btn btn-outline btn-sm'
            disabled={busy || paused}
            onClick={() => run('req:rl:optimization:pause')}
          >
            <FontAwesomeIcon icon={faPause} />
            Pause
          </button>
          <button
            type='button'
            className='btn btn-primary btn-sm'
            disabled={busy || !paused}
            onClick={() => run('req:rl:optimization:resume')}
          >
            <FontAwesomeIcon icon={faPlay} />
            Resume
          </button>
        </div>
      </div>

      <div className='grid grid-cols-1 gap-3 sm:grid-cols-2 xl:grid-cols-4'>
        <StatusCell label='Current bean' value={activeName} />
        <StatusCell label='Context/session' value={settings?.rlBeanContextId} />
        <StatusCell label='Optimizer mode' value={settings?.rlMode || 'No mode yet'} />
        <StatusCell
          label='Local optimization'
          value={paused ? 'Paused' : localOn ? 'On' : 'Excluded'}
        />
        <StatusCell label='Latest recommendation' value={settings?.rlLastRecommendationId} />
        <StatusCell label='Local shots' value={(settings?.rlLocalShotCount ?? 0).toString()} />
        <StatusCell label='Rated shots' value={(settings?.rlRatedShotCount ?? 0).toString()} />
        <StatusCell label='Best known recipe' value={bestRecipeText(settings)} />
      </div>

      <div className='border-base-300 bg-base-100 rounded-md border p-4'>
        <div className='grid grid-cols-1 gap-3 lg:grid-cols-[1fr_auto_auto_auto]'>
          <label className='input input-bordered flex items-center gap-2'>
            <input
              type='text'
              className='grow'
              value={beanName}
              placeholder='Bean name'
              onInput={event => setBeanName(event.currentTarget.value)}
            />
          </label>
          <button
            type='button'
            className='btn btn-primary'
            disabled={busy}
            onClick={() => run('req:rl:context:start-bean', { name: beanName })}
          >
            <FontAwesomeIcon icon={faPlus} />
            New Bean
          </button>
          <button
            type='button'
            className='btn btn-outline'
            disabled={busy || !settings?.rlBeanContextId}
            onClick={() => run('req:rl:context:start-bag')}
          >
            <FontAwesomeIcon icon={faChevronRight} />
            New Bag
          </button>
          <button
            type='button'
            className='btn btn-outline'
            disabled={busy}
            onClick={() => run('req:rl:context:reset')}
          >
            <FontAwesomeIcon icon={faRotate} />
            Reset Dial-In
          </button>
        </div>
      </div>

      <div className='space-y-3'>
        <h2 className='text-lg font-semibold'>Bean Contexts</h2>
        <div className='grid grid-cols-1 gap-3'>
          {contexts.length === 0 && (
            <div className='border-base-300 bg-base-100 rounded-md border p-4 text-sm opacity-70'>
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
                <div className='text-base-content/50 mt-1 truncate text-xs'>{context.id}</div>
              </div>
              <div className='flex gap-2'>
                <button
                  type='button'
                  className='btn btn-outline btn-sm'
                  disabled={busy || context.active}
                  onClick={() => run('req:rl:context:switch', { id: context.id })}
                >
                  <FontAwesomeIcon icon={faPlay} />
                  Switch
                </button>
                <button
                  type='button'
                  className='btn btn-error btn-outline btn-sm'
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
      </div>
    </div>
  );
}
