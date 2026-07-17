import { useCallback, useContext, useEffect, useRef, useState } from 'preact/hooks';
import { ApiServiceContext } from '../services/ApiService.js';
import { formatGrinderSettingTransition } from '../utils/grinderRecommendation.js';

const PREFERENCE_DISMISS_MS = 45000;
const RECOMMENDATION_STATUSES = new Set(['', 'pending', 'shown']);
const SEEN_STORAGE_KEY = 'rl_seen_prompt_ids';

function formatDose(value) {
  const dose = Number(value);
  return Number.isFinite(dose) && dose > 0 ? `${dose.toFixed(1)}g` : '-';
}

function formatYield(value) {
  const yieldG = Number(value);
  return Number.isFinite(yieldG) && yieldG > 0 ? `${yieldG.toFixed(1)}g` : '-';
}

function formatGrind(recommendation) {
  return formatGrinderSettingTransition({
    currentAbsoluteStep: recommendation?.has_current_absolute_step
      ? recommendation.current_absolute_step
      : undefined,
    projectedAbsoluteStep: recommendation?.has_projected_absolute_step
      ? recommendation.projected_absolute_step
      : undefined,
    projectedRelativeStep: recommendation?.projected_relative_step_from_reference,
    deltaSteps: recommendation?.grind_delta_steps_from_current,
  });
}

function loadSeen() {
  try {
    return new Set(JSON.parse(window.sessionStorage.getItem(SEEN_STORAGE_KEY) || '[]'));
  } catch {
    return new Set();
  }
}

export function AutoTuningPromptOverlay() {
  const apiService = useContext(ApiServiceContext);
  const [pendingDose, setPendingDose] = useState(null);
  const [pendingPreference, setPendingPreference] = useState(null);
  const [pendingRecommendation, setPendingRecommendation] = useState(null);
  const [view, setView] = useState(null);
  const seenRef = useRef(loadSeen());

  const markSeen = useCallback(id => {
    if (!id || seenRef.current.has(id)) {
      return;
    }
    seenRef.current.add(id);
    try {
      window.sessionStorage.setItem(SEEN_STORAGE_KEY, JSON.stringify([...seenRef.current]));
    } catch {
      // sessionStorage is optional; in-memory deduplication still works.
    }
  }, []);

  useEffect(() => {
    if (!apiService) {
      return undefined;
    }

    const recommendationListener = apiService.on('evt:rl:recommendation', message => {
      const status = message.status || '';
      if (!message.recommendation_id || !RECOMMENDATION_STATUSES.has(status)) {
        setPendingRecommendation(current =>
          current?.recommendation_id === message.recommendation_id ? null : current,
        );
        return;
      }
      const prompt = { ...message };
      setPendingRecommendation(prompt);
      const key = `recommendation:${message.recommendation_id}`;
      const firstSeen = !seenRef.current.has(key);
      markSeen(key);
      if (firstSeen) {
        setView(current => current || 'recommendation');
      }
    });

    const recommendationClearListener = apiService.on('evt:rl:recommendation-clear', () => {
      setPendingRecommendation(null);
      setView(current => (current === 'recommendation' ? null : current));
    });

    const doseConfirmationListener = apiService.on('evt:rl:dose-confirmation', message => {
      const target = Number(message.dose_target_g);
      if (!message.shot_id || !Number.isFinite(target) || target <= 0) {
        return;
      }
      setPendingDose({ ...message, dose_target_g: target });
      setView('dose');
    });

    const doseResolvedListener = apiService.on('evt:rl:dose-confirmation-resolved', message => {
      setPendingDose(current => (current?.shot_id === message.shot_id ? null : current));
      setView(current => (current === 'dose' ? null : current));
    });

    const shotCompleteListener = apiService.on('evt:rl:shot-complete', message => {
      if (!message.preference_feedback_required) {
        return;
      }
      const validMode = ['global_previous', 'best_incumbent'].includes(message.comparison_mode);
      if (
        !message.shot_id ||
        !message.install_id ||
        !message.optimization_run_id ||
        !message.anchor_shot_id ||
        message.anchor_shot_id === message.shot_id ||
        !validMode
      ) {
        return;
      }
      const prompt = { ...message };
      setPendingPreference(prompt);
      const key = `preference:${message.optimization_run_id}:${message.shot_id}`;
      const firstSeen = !seenRef.current.has(key);
      markSeen(key);
      if (firstSeen) {
        setView('preference');
      }
    });

    const brewStartListener = apiService.on('evt:status', message => {
      if (message.process?.a) {
        setView(null);
      }
    });

    const clearListener = apiService.on('evt:rl:prompts-clear', message => {
      if (!message.shot_id) {
        setPendingDose(null);
        setPendingPreference(null);
        setPendingRecommendation(null);
        setView(null);
        return;
      }
      setPendingDose(current => (current?.shot_id === message.shot_id ? null : current));
      setPendingPreference(current => (current?.shot_id === message.shot_id ? null : current));
      setView(current => (current === 'dose' || current === 'preference' ? null : current));
    });

    return () => {
      apiService.off('evt:rl:recommendation', recommendationListener);
      apiService.off('evt:rl:recommendation-clear', recommendationClearListener);
      apiService.off('evt:rl:dose-confirmation', doseConfirmationListener);
      apiService.off('evt:rl:dose-confirmation-resolved', doseResolvedListener);
      apiService.off('evt:rl:shot-complete', shotCompleteListener);
      apiService.off('evt:status', brewStartListener);
      apiService.off('evt:rl:prompts-clear', clearListener);
    };
  }, [apiService, markSeen]);

  useEffect(() => {
    if (view !== 'preference') {
      return undefined;
    }
    const timeout = window.setTimeout(() => setView(null), PREFERENCE_DISMISS_MS);
    return () => window.clearTimeout(timeout);
  }, [view]);

  const submitPreference = useCallback(
    label => {
      if (!pendingPreference || !['new_better', 'anchor_better', 'tie'].includes(label)) {
        return;
      }
      apiService.send({
        tp: 'req:rl:preference',
        install_id: pendingPreference.install_id,
        optimization_run_id: pendingPreference.optimization_run_id,
        new_shot_id: pendingPreference.shot_id,
        anchor_shot_id: pendingPreference.anchor_shot_id,
        comparison_mode: pendingPreference.comparison_mode,
        label,
      });
      setPendingPreference(null);
      setView(null);
    },
    [apiService, pendingPreference],
  );

  const submitDoseConfirmation = useCallback(
    followed => {
      if (!pendingDose?.shot_id) {
        return;
      }
      apiService.send({
        tp: 'req:rl:dose-confirmation',
        shot_id: pendingDose.shot_id,
        followed: Boolean(followed),
      });
      setView(null);
    },
    [apiService, pendingDose],
  );

  const useRecommendation = useCallback(() => {
    if (!pendingRecommendation?.recommendation_id) {
      return;
    }
    apiService.send({
      tp: 'req:rl:recommendation:use',
      recommendation_id: pendingRecommendation.recommendation_id,
    });
    setPendingRecommendation(null);
    setView(null);
  }, [apiService, pendingRecommendation]);

  const ignoreRecommendation = useCallback(() => {
    if (!pendingRecommendation?.recommendation_id) {
      return;
    }
    apiService.send({
      tp: 'req:rl:recommendation:ignore',
      recommendation_id: pendingRecommendation.recommendation_id,
    });
    setPendingRecommendation(null);
    setView(null);
  }, [apiService, pendingRecommendation]);

  if (!view) {
    if (!pendingDose && !pendingPreference && !pendingRecommendation) {
      return null;
    }
    return (
      <div className='fixed right-4 bottom-[calc(1rem_+_env(safe-area-inset-bottom))] left-4 z-50 flex flex-col items-stretch gap-2 sm:left-auto sm:items-end'>
        {pendingDose && (
          <button
            type='button'
            className='btn btn-primary btn-sm shadow-lg'
            onClick={() => setView('dose')}
          >
            Confirm dose
          </button>
        )}
        {pendingPreference && (
          <button
            type='button'
            className='btn btn-primary btn-sm shadow-lg'
            onClick={() => setView('preference')}
          >
            Compare last shot
          </button>
        )}
        {pendingRecommendation && (
          <button
            type='button'
            className='btn btn-secondary btn-sm shadow-lg'
            onClick={() => setView('recommendation')}
          >
            Next recipe
          </button>
        )}
      </div>
    );
  }

  return (
    <div className='fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4 pb-[calc(1rem_+_env(safe-area-inset-bottom))]'>
      <div
        className='bg-base-100 border-base-300 relative max-h-[calc(100vh_-_2rem_-_env(safe-area-inset-bottom))] w-full max-w-md overflow-y-auto rounded-lg border p-5 shadow-xl'
        role='dialog'
        aria-modal='true'
      >
        <button
          type='button'
          className='btn btn-ghost btn-xs absolute top-2 right-2'
          onClick={() => setView(null)}
        >
          Minimize
        </button>

        {view === 'dose' && pendingDose && (
          <div className='space-y-4'>
            <div>
              <div className='text-base-content/60 text-xs font-semibold tracking-wide uppercase'>
                Shot dose
              </div>
              <h2 className='text-xl font-bold'>
                Did you use {formatDose(pendingDose.dose_target_g)}?
              </h2>
              <p className='text-base-content/60 text-sm'>
                The dose was not measured by grind by weight.
              </p>
            </div>
            <div className='grid grid-cols-2 gap-2'>
              <button
                type='button'
                className='btn btn-primary min-h-12'
                onClick={() => submitDoseConfirmation(true)}
              >
                Yes
              </button>
              <button
                type='button'
                className='btn btn-outline min-h-12'
                onClick={() => submitDoseConfirmation(false)}
              >
                No
              </button>
            </div>
          </div>
        )}

        {view === 'preference' && pendingPreference && (
          <div className='space-y-4'>
            <div>
              <div className='text-base-content/60 text-xs font-semibold tracking-wide uppercase'>
                Shot comparison
              </div>
              <h2 className='text-xl font-bold'>Which tasted better?</h2>
              <p className='text-base-content/60 text-sm'>
                Compare this shot with the{' '}
                {pendingPreference.comparison_mode === 'best_incumbent'
                  ? 'current best'
                  : 'previous'}{' '}
                shot.
              </p>
            </div>
            <div className='grid grid-cols-1 gap-2'>
              <button
                type='button'
                className='btn btn-primary min-h-12'
                onClick={() => submitPreference('new_better')}
              >
                New shot is better
              </button>
              <button
                type='button'
                className='btn btn-outline min-h-12'
                onClick={() => submitPreference('tie')}
              >
                No noticeable difference
              </button>
              <button
                type='button'
                className='btn btn-outline min-h-12'
                onClick={() => submitPreference('anchor_better')}
              >
                {pendingPreference.comparison_mode === 'best_incumbent'
                  ? 'Current best is better'
                  : 'Previous shot is better'}
              </button>
            </div>
          </div>
        )}

        {view === 'recommendation' && pendingRecommendation && (
          <div className='space-y-4'>
            <div>
              <div className='text-base-content/60 text-xs font-semibold tracking-wide uppercase'>
                Next shot
              </div>
              <h2 className='text-xl font-bold'>Next recipe</h2>
            </div>
            <div className='grid grid-cols-1 gap-2 sm:grid-cols-3'>
              <div className='bg-base-200 rounded-md p-3'>
                <div className='text-base-content/60 text-xs uppercase'>Grind</div>
                <div className='text-base font-semibold'>{formatGrind(pendingRecommendation)}</div>
              </div>
              <div className='bg-base-200 rounded-md p-3'>
                <div className='text-base-content/60 text-xs uppercase'>Dose</div>
                <div className='text-base font-semibold'>
                  {formatDose(pendingRecommendation.next_dose_g)}
                </div>
              </div>
              <div className='bg-base-200 rounded-md p-3'>
                <div className='text-base-content/60 text-xs uppercase'>Yield</div>
                <div className='text-base font-semibold'>
                  {formatYield(pendingRecommendation.target_yield_g)}
                </div>
              </div>
            </div>
            <p className='text-base-content/70 text-sm'>
              Grind remains manual. Use applies machine-controllable targets.
            </p>
            <div className='grid grid-cols-3 gap-2'>
              <button type='button' className='btn btn-primary' onClick={useRecommendation}>
                Use
              </button>
              <button type='button' className='btn btn-outline' onClick={() => setView(null)}>
                Later
              </button>
              <button
                type='button'
                className='btn btn-error btn-outline'
                onClick={ignoreRecommendation}
              >
                Ignore
              </button>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
