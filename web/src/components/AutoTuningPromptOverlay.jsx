import { createPortal } from 'preact/compat';
import { useCallback, useContext, useEffect, useLayoutEffect, useRef, useState } from 'preact/hooks';
import { ApiServiceContext } from '../services/ApiService.js';
import { formatGrinderSettingTransition } from '../utils/grinderRecommendation.js';
import {
  PREFERENCE_ACTIONS,
  preferenceAnchorDetails,
  preferenceGoalSummary,
} from '../utils/preferencePrompt.js';

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
  const [dock, setDock] = useState(null);
  useEffect(() => setDock(document.getElementById('shot-prompt-dock')), []);
  const [pendingDose, setPendingDose] = useState(null);
  const [shotProgress, setShotProgress] = useState(null);
  const [pendingPreference, setPendingPreference] = useState(null);
  const [preferenceSending, setPreferenceSending] = useState(false);
  const [preferenceError, setPreferenceError] = useState('');
  const [recipeSending, setRecipeSending] = useState(false);
  const [recipeError, setRecipeError] = useState('');
  const [editingRecipe, setEditingRecipe] = useState(false);
  const [recipeGrind, setRecipeGrind] = useState('');
  const [recipeDose, setRecipeDose] = useState('');
  const [pendingRecommendation, setPendingRecommendation] = useState(null);
  const recommendationRef = useRef(null);
  recommendationRef.current = pendingRecommendation;
  const [view, setView] = useState(null);
  const seenRef = useRef(loadSeen());
  const pendingDoseId = useRef(null);
  pendingDoseId.current = pendingDose;

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

  // Subscribe before paint: the machine can replay pending prompts as soon as
  // the socket connects, before deferred effects would otherwise run.
  useLayoutEffect(() => {
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
      recommendationRef.current = prompt;
      setPendingRecommendation(prompt);
      setShotProgress(null);
      const key = `recommendation:${message.recommendation_id}`;
      const firstSeen = !seenRef.current.has(key);
      markSeen(key);
      setView(current =>
        current === 'progress'
          ? 'recommendation'
          : firstSeen
            ? current || 'recommendation'
            : current,
      );
    });

    const recommendationClearListener = apiService.on('evt:rl:recommendation-clear', () => {
      recommendationRef.current = null;
      setPendingRecommendation(null);
      setView(current => (current === 'recommendation' ? null : current));
    });

    const doseConfirmationListener = apiService.on('evt:rl:dose-confirmation', message => {
      const target = Number(message.dose_target_g);
      const revision = Number(message.prompt_revision);
      if (
        !message.shot_id ||
        !Number.isInteger(revision) ||
        revision <= 0 ||
        !Number.isFinite(target) ||
        target <= 0
      ) {
        return;
      }
      setPendingDose(current => {
        if (current?.shot_id === message.shot_id && Number(current.prompt_revision) >= revision) {
          return current;
        }
        return { ...message, prompt_revision: revision, dose_target_g: target };
      });
      setView('dose');
    });

    const doseResolvedListener = apiService.on('evt:rl:dose-confirmation-resolved', message => {
      if (message.persisted !== true) return;
      setPendingDose(current =>
        current?.shot_id === message.shot_id &&
        Number(current.prompt_revision) === Number(message.prompt_revision)
          ? null
          : current,
      );
      if (
        message.shot_id === pendingDoseId.current?.shot_id &&
        Number(message.prompt_revision) === Number(pendingDoseId.current?.prompt_revision)
      ) {
        setRecipeSending(false);
        const nextRecipeReady = !!recommendationRef.current;
        setShotProgress(
          nextRecipeReady
            ? null
            : { shot_id: message.shot_id, followed: message.followed, accepted: false },
        );
        setView(current =>
          current === 'dose' ? (nextRecipeReady ? 'recommendation' : 'progress') : current,
        );
      }
    });

    const shotCompleteListener = apiService.on('evt:rl:shot-complete', message => {
      if (!message.preference_feedback_required) {
        setShotProgress(current =>
          current?.shot_id === message.shot_id ? { ...current, accepted: true } : current,
        );
        return;
      }
      const revision = Number(message.prompt_revision);
      const validMode = ['global_previous', 'best_incumbent'].includes(message.comparison_mode);
      if (
        !message.shot_id ||
        !Number.isInteger(revision) ||
        revision <= 0 ||
        !message.install_id ||
        !message.optimization_run_id ||
        !message.anchor_shot_id ||
        message.anchor_shot_id === message.shot_id ||
        !validMode
      ) {
        return;
      }
      setShotProgress(null);
      const prompt = { ...message, prompt_revision: revision };
      setPendingPreference(current => {
        if (current?.shot_id === message.shot_id && Number(current.prompt_revision) >= revision) {
          return current;
        }
        return prompt;
      });
      const key = `preference:${message.optimization_run_id}:${message.shot_id}:${revision}`;
      const firstSeen = !seenRef.current.has(key);
      markSeen(key);
      setView(current => (firstSeen || current === 'progress' ? 'preference' : current));
    });

    const preferenceResolvedListener = apiService.on('evt:rl:preference-resolved', message => {
      setPendingPreference(current => {
        if (
          current?.shot_id !== message.shot_id ||
          current?.optimization_run_id !== message.optimization_run_id ||
          current?.prompt_revision !== message.prompt_revision
        )
          return current;
        setPreferenceSending(false);
        setPreferenceError('');
        setView(view => (view === 'preference' ? null : view));
        return null;
      });
    });

    const brewStartListener = apiService.on('evt:status', message => {
      if (message.process?.a) {
        setView(null);
      }
    });

    const clearListener = apiService.on('evt:rl:prompts-clear', message => {
      if (!message.shot_id) {
        setPendingDose(null);
        setShotProgress(null);
        setPendingPreference(null);
        setPendingRecommendation(null);
        setView(null);
        return;
      }
      setShotProgress(current => (current?.shot_id === message.shot_id ? null : current));
      setPendingDose(current => (current?.shot_id === message.shot_id ? null : current));
      setPendingPreference(current => (current?.shot_id === message.shot_id ? null : current));
      setView(current =>
        current === 'dose' || current === 'preference' || current === 'progress' ? null : current,
      );
    });

    return () => {
      apiService.off('evt:rl:recommendation', recommendationListener);
      apiService.off('evt:rl:recommendation-clear', recommendationClearListener);
      apiService.off('evt:rl:dose-confirmation', doseConfirmationListener);
      apiService.off('evt:rl:dose-confirmation-resolved', doseResolvedListener);
      apiService.off('evt:rl:shot-complete', shotCompleteListener);
      apiService.off('evt:rl:preference-resolved', preferenceResolvedListener);
      apiService.off('evt:status', brewStartListener);
      apiService.off('evt:rl:prompts-clear', clearListener);
    };
  }, [apiService, markSeen]);

  useEffect(() => {
    setPreferenceSending(false);
    setPreferenceError('');
  }, [
    pendingPreference?.shot_id,
    pendingPreference?.optimization_run_id,
    pendingPreference?.prompt_revision,
  ]);

  useEffect(() => {
    if (view !== 'preference') {
      return undefined;
    }
    const timeout = window.setTimeout(() => setView(null), PREFERENCE_DISMISS_MS);
    return () => window.clearTimeout(timeout);
  }, [view]);

  useEffect(() => {
    if (!preferenceSending) return undefined;
    const timeout = window.setTimeout(() => {
      setPreferenceSending(false);
      setPreferenceError('Answer not confirmed. Check the connection and try again.');
    }, 5000);
    return () => window.clearTimeout(timeout);
  }, [preferenceSending]);

  const submitPreference = useCallback(
    label => {
      if (!pendingPreference || preferenceSending || !PREFERENCE_ACTIONS.includes(label)) {
        return;
      }
      setPreferenceSending(true);
      setPreferenceError('');
      try {
        apiService.send({
          tp: 'req:rl:preference',
          install_id: pendingPreference.install_id,
          optimization_run_id: pendingPreference.optimization_run_id,
          new_shot_id: pendingPreference.shot_id,
          anchor_shot_id: pendingPreference.anchor_shot_id,
          comparison_mode: pendingPreference.comparison_mode,
          prompt_revision: pendingPreference.prompt_revision,
          label,
        });
      } catch {
        setPreferenceSending(false);
        setPreferenceError('Not connected. Your answer has not been sent; please try again.');
      }
    },
    [apiService, pendingPreference, preferenceSending],
  );

  useEffect(() => {
    setRecipeSending(false);
    setRecipeError('');
    setEditingRecipe(false);
    setRecipeGrind(pendingDose?.grind_setting == null ? '' : String(pendingDose.grind_setting));
    setRecipeDose(pendingDose?.dose_target_g == null ? '' : String(pendingDose.dose_target_g));
  }, [
    pendingDose?.shot_id,
    pendingDose?.prompt_revision,
    pendingDose?.grind_setting,
    pendingDose?.dose_target_g,
  ]);

  useEffect(() => {
    if (!recipeSending) return undefined;
    const timeout = window.setTimeout(() => {
      setRecipeSending(false);
      setRecipeError('Recipe not confirmed. Check the connection and try again.');
    }, 5000);
    return () => window.clearTimeout(timeout);
  }, [recipeSending]);

  const submitDoseConfirmation = action => {
    if (!pendingDose?.shot_id || recipeSending) return;
    const values = {};
    if (action === 'change') {
      const grind = Number(recipeGrind),
        dose = Number(recipeDose);
      if (
        !recipeGrind.trim() ||
        !recipeDose.trim() ||
        !Number.isFinite(grind) ||
        Math.abs(grind) > 10000 ||
        !Number.isFinite(dose) ||
        dose < 0.1 ||
        dose > 100
      ) {
        setRecipeError('Enter a valid grinder setting and a dose between 0.1 and 100 g.');
        return;
      }
      values.grind_setting = grind;
      values.dose_g = dose;
    }
    setRecipeSending(true);
    setRecipeError('');
    try {
      apiService.send({
        tp: 'req:rl:dose-confirmation',
        shot_id: pendingDose.shot_id,
        prompt_revision: pendingDose.prompt_revision,
        action,
        ...values,
      });
    } catch {
      setRecipeSending(false);
      setRecipeError('Not connected. Your recipe has not been saved; please try again.');
    }
  };

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
    if (!pendingDose && !pendingPreference && !pendingRecommendation && !shotProgress) {
      return null;
    }
    if (!dock) return null;
    const count = [pendingDose, pendingPreference, pendingRecommendation, shotProgress].filter(
      Boolean,
    ).length;
    return createPortal(
      <div className='flex items-center gap-3' aria-label='Pending shot prompts'>
        <button
          type='button'
          className='btn btn-circle btn-secondary relative min-h-12 min-w-12 shrink-0'
          aria-label={`Open shot prompts (${count} pending)`}
          onClick={() =>
            setView(
              pendingDose
                ? 'dose'
                : pendingPreference
                  ? 'preference'
                  : pendingRecommendation
                    ? 'recommendation'
                    : 'progress',
            )
          }
        >
          <svg
            width='24'
            height='24'
            viewBox='0 0 24 24'
            fill='none'
            stroke='currentColor'
            stroke-width='2'
            aria-hidden='true'
          >
            <path d='M4 4h16v12H9l-5 4V4z' />
            <path d='M8 8h8M8 12h5' />
          </svg>
          <span className='badge badge-sm absolute -top-1 -right-1'>{count}</span>
        </button>
        <span className='truncate text-sm'>
          {pendingDose
            ? 'Confirm recipe'
            : pendingPreference
              ? 'Compare shot'
              : pendingRecommendation
                ? 'Next recipe'
                : 'Shot status'}
        </span>
      </div>,
      dock,
    );
  }

  const anchor = preferenceAnchorDetails(pendingPreference);
  return (
    <div className='fixed inset-0 z-[10000] flex items-center justify-center bg-black/60 p-4 pb-[calc(1rem_+_env(safe-area-inset-bottom))]'>
      <div
        className='bg-base-100 border-base-300 relative max-h-[calc(100vh_-_2rem_-_env(safe-area-inset-bottom))] w-full max-w-md overflow-y-auto rounded-lg border p-5 shadow-xl'
        role='dialog'
        aria-modal='true'
      >
        <button
          type='button'
          className='btn btn-ghost btn-sm mb-3 ml-auto flex'
          aria-label='Minimize shot prompt'
          onClick={() => setView(null)}
        >
          Minimize
        </button>

        {view === 'progress' && shotProgress && (
          <div className='space-y-4' role='status'>
            <h2 className='text-xl font-bold'>
              {shotProgress.accepted ? 'Shot received by EspressoRL' : 'Recipe saved'}
            </h2>
            <p>
              {shotProgress.accepted
                ? 'EspressoRL acknowledged this shot. No taste comparison was requested.'
                : 'Waiting for EspressoRL to acknowledge this shot. A comparison or next recipe will open here when available.'}
            </p>
            {shotProgress.followed === false && (
              <p>
                Your recipe was marked uncertain, so it will not be used as a known recipe for
                optimization.
              </p>
            )}
            <p className='text-base-content/60 text-sm'>
              You can minimize this message. If no next step appears, check delivery and optimizer
              status in Auto-Tuning.
            </p>
            <a className='btn btn-outline w-full' href='/autotuning' onClick={() => setView(null)}>
              View Auto-Tuning status
            </a>
          </div>
        )}

        {view === 'dose' && pendingDose && (
          <div className='space-y-4'>
            <h2 className='text-xl font-bold'>Confirm the recipe you used</h2>
            <p>
              Grind{' '}
              {pendingDose.grind_setting == null
                ? 'unknown'
                : Number(pendingDose.grind_setting).toFixed(1)}
              {pendingDose.grind_is_absolute ? '' : ' steps from reference'}
              {' / Dose '}
              {formatDose(pendingDose.dose_target_g)}
            </p>
            <p className='text-base-content/60 text-sm'>
              {pendingDose.dose_measured
                ? 'Dose was measured. Please confirm the manual grinder setting.'
                : 'Did you use these settings? Your answer records the recipe for this shot.'}{' '}
              Not sure keeps the shot without using its recipe for optimization.
            </p>
            {editingRecipe && (
              <div className='space-y-3'>
                <label className='block'>
                  Grind {pendingDose.grind_is_absolute ? 'setting' : '(steps from reference)'}
                  <input
                    aria-label='Actual grinder setting'
                    type='number'
                    step='any'
                    className='input input-bordered w-full'
                    value={recipeGrind}
                    onInput={event => setRecipeGrind(event.currentTarget.value)}
                    disabled={recipeSending}
                  />
                </label>
                <label className='block'>
                  Dose (g)
                  <input
                    aria-label='Actual dose in grams'
                    type='number'
                    step='any'
                    min='0.1'
                    max='100'
                    className='input input-bordered w-full'
                    value={recipeDose}
                    onInput={event => setRecipeDose(event.currentTarget.value)}
                    disabled={recipeSending}
                  />
                </label>
              </div>
            )}
            {recipeError && (
              <p role='alert' className='text-error text-sm'>
                {recipeError}
              </p>
            )}
            <div className='grid gap-2'>
              <button
                type='button'
                className='btn btn-primary min-h-12'
                disabled={recipeSending || (!editingRecipe && pendingDose.grind_setting == null)}
                onClick={() => submitDoseConfirmation(editingRecipe ? 'change' : 'confirm')}
              >
                {recipeSending ? 'Saving...' : editingRecipe ? 'Save recipe' : 'Yes'}
              </button>
              {!editingRecipe && (
                <button
                  type='button'
                  className='btn btn-outline min-h-12'
                  disabled={recipeSending}
                  onClick={() => setEditingRecipe(true)}
                >
                  Change values
                </button>
              )}
              <button
                type='button'
                className='btn btn-ghost min-h-12'
                disabled={recipeSending}
                onClick={() => submitDoseConfirmation('unknown')}
              >
                Not sure
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
              <h2 className='text-xl font-bold'>Which is closer to your goal?</h2>
              <p className='text-sm'>Goal: {preferenceGoalSummary(pendingPreference)}</p>
              <p className='text-base-content/60 text-sm'>
                Compare this shot with the reference below. Choose “Can't compare” if you did not
                taste it or do not remember it.
              </p>
            </div>
            <div className='bg-base-200 rounded p-3 text-sm'>
              <strong>Reference shot</strong>
              {anchor.available ? (
                <div>
                  <div>{anchor.date}</div>
                  {anchor.profile && <div>{anchor.profile}</div>}
                  <div>Grind: {anchor.grind}</div>
                  <div>
                    Dose: {formatDose(anchor.dose)} · Target: {formatYield(anchor.targetYield)}
                  </div>
                  <div>Actual output: {formatYield(anchor.actualYield)}</div>
                </div>
              ) : (
                <div>Recipe details unavailable.</div>
              )}
              <div className='text-xs break-all'>ID: {anchor.id}</div>
            </div>
            {preferenceError && (
              <p role='alert' className='text-error text-sm'>
                {preferenceError}
              </p>
            )}
            <div className='grid grid-cols-1 gap-2'>
              <button
                type='button'
                className='btn btn-primary min-h-12'
                onClick={() => submitPreference('new_better')}
                disabled={preferenceSending}
              >
                New shot is closer
              </button>
              <button
                type='button'
                className='btn btn-outline min-h-12'
                onClick={() => submitPreference('tie')}
                disabled={preferenceSending}
              >
                No noticeable difference
              </button>
              <button
                type='button'
                className='btn btn-outline min-h-12'
                onClick={() => submitPreference('anchor_better')}
                disabled={preferenceSending}
              >
                Reference shot is closer
              </button>
              <button
                type='button'
                className='btn btn-ghost min-h-12'
                onClick={() => submitPreference('abstain')}
                disabled={preferenceSending}
              >
                Can't compare / don't remember
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
              Use applies machine-controllable targets and shows the intended grind. Move the
              grinder manually; confirm grind and dose after the shot.
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
