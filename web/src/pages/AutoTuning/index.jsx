import { useCallback, useContext, useEffect, useState } from 'preact/hooks';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { faGear } from '@fortawesome/free-solid-svg-icons/faGear';
import { faPause } from '@fortawesome/free-solid-svg-icons/faPause';
import { faPlay } from '@fortawesome/free-solid-svg-icons/faPlay';
import { faPlus } from '@fortawesome/free-solid-svg-icons/faPlus';
import { faSave } from '@fortawesome/free-solid-svg-icons/faSave';
import { faTrashCan } from '@fortawesome/free-solid-svg-icons/faTrashCan';
import { Spinner } from '../../components/Spinner.jsx';
import { ApiServiceContext } from '../../services/ApiService.js';
import { grindDirectionForStepDelta } from '../../utils/grinderDirection.js';
import {
  TASTE_LEVEL_LABELS,
  TASTE_LEVELS,
  TASTE_TAG_GROUPS,
  TASTE_TAG_LABELS,
} from '../../utils/tasteTags.js';

const PROMPTABLE_RECOMMENDATION_STATUSES = new Set(['', 'pending', 'shown']);
const PROVIDER_LABELS = {
  disabled: 'Disabled',
  off_board: 'Off-board EspressoRL',
  on_board: 'On-board',
};
const BALANCED_TASTE_GOAL = { schema_version: 1, mode: 'balanced', targets: {} };
const TASTE_GOAL_LEVELS = new Set(TASTE_LEVELS.slice(1));
const TASTE_GOAL_ATTRIBUTES = new Set(
  TASTE_TAG_GROUPS.flatMap(group => group.tags.map(tag => tag.value)),
);
const DEFAULT_RECIPE_DOMAIN = {
  grindRadiusSteps: 10,
  doseMinG: 6,
  doseMaxG: 30,
  targetOutputMinG: 5,
  targetOutputMaxG: 250,
};
const RECIPE_DOMAIN_ENVELOPE = {
  grind_radius_steps: [0.1, 1000],
  dose_min_g: [0.1, 100],
  dose_max_g: [0.1, 100],
  target_output_min_g: [0.1, 1000],
  target_output_max_g: [0.1, 1000],
};
const CPBO_PROFILES = new Set(['application', 'paper_fidelity']);
const CPBO_COMPARISON_MODES = new Set(['best_incumbent', 'global_previous']);

function cpboProfileName(value) {
  return CPBO_PROFILES.has(value) ? value : 'application';
}

function cpboComparisonMode(value) {
  return CPBO_COMPARISON_MODES.has(value) ? value : 'best_incumbent';
}

function normalizeTasteGoal(value) {
  if (
    !value ||
    typeof value !== 'object' ||
    value.schema_version !== 1 ||
    value.mode !== 'custom'
  ) {
    return { ...BALANCED_TASTE_GOAL, targets: {} };
  }
  const targets = {};
  for (const [attribute, level] of Object.entries(value.targets || {})) {
    if (TASTE_GOAL_ATTRIBUTES.has(attribute) && TASTE_GOAL_LEVELS.has(level)) {
      targets[attribute] = level;
    }
  }
  return Object.keys(targets).length
    ? { schema_version: 1, mode: 'custom', targets }
    : { ...BALANCED_TASTE_GOAL, targets: {} };
}

function tasteGoalSummary(goal) {
  const normalized = normalizeTasteGoal(goal);
  if (normalized.mode === 'balanced') {
    return 'Balanced';
  }
  return Object.entries(normalized.targets)
    .map(([attribute, level]) => `${TASTE_TAG_LABELS[attribute] || attribute} ${level}`)
    .join(', ');
}

function setTasteGoalTarget(goal, attribute, level) {
  const targets = { ...(goal?.targets || {}) };
  if (level === 'unspecified') {
    delete targets[attribute];
  } else {
    targets[attribute] = level;
  }
  return { schema_version: 1, mode: 'custom', targets };
}

function Panel({ title, action, children }) {
  return (
    <section className='border-base-300 bg-base-100 rounded-md border p-4'>
      {(title || action) && (
        <div className='mb-4 flex items-center justify-between gap-3'>
          {title && <h2 className='text-base font-semibold sm:text-lg'>{title}</h2>}
          {action}
        </div>
      )}
      {children}
    </section>
  );
}

function Modal({ title, onClose, children }) {
  return (
    <div className='fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4'>
      <div
        className='bg-base-100 border-base-300 max-h-[calc(100vh_-_2rem)] w-full max-w-lg overflow-y-auto rounded-md border p-4 shadow-xl'
        role='dialog'
        aria-modal='true'
      >
        <div className='mb-4 flex items-center justify-between gap-3'>
          <h2 className='text-lg font-semibold'>{title}</h2>
          <button type='button' className='btn btn-ghost btn-sm' onClick={onClose}>
            Close
          </button>
        </div>
        {children}
      </div>
    </div>
  );
}

function Drawer({ title, subtitle, onClose, children }) {
  return (
    <div className='fixed inset-0 z-50 flex justify-end bg-black/55'>
      <button
        type='button'
        className='absolute inset-0 cursor-default'
        aria-label='Close drawer'
        onClick={onClose}
      />
      <aside
        className='bg-base-100 border-base-300 relative flex h-full w-full flex-col border-l shadow-xl sm:max-w-lg'
        role='dialog'
        aria-modal='true'
      >
        <div className='border-base-300 flex shrink-0 items-start justify-between gap-3 border-b p-4'>
          <div className='min-w-0'>
            <h2 className='text-lg font-semibold'>{title}</h2>
            {subtitle && <div className='text-base-content/60 mt-1 text-sm'>{subtitle}</div>}
          </div>
          <button type='button' className='btn btn-ghost btn-sm shrink-0' onClick={onClose}>
            Close
          </button>
        </div>
        <div className='min-h-0 flex-1 overflow-y-auto p-4'>{children}</div>
      </aside>
    </div>
  );
}

function StatCard({ label, value, tone = 'neutral' }) {
  const tones = {
    neutral: 'border-base-300 bg-base-200/40',
    primary: 'border-primary/40 bg-primary/10',
    success: 'border-success/40 bg-success/10',
    warning: 'border-warning/40 bg-warning/10',
    error: 'border-error/40 bg-error/10',
  };
  return (
    <div className={`min-w-0 rounded-md border p-3 ${tones[tone] || tones.neutral}`}>
      <div className='text-base-content/60 text-xs tracking-wide uppercase'>{label}</div>
      <div className='mt-1 min-h-6 text-base leading-tight font-semibold break-words'>
        {value || 'None'}
      </div>
    </div>
  );
}

function InfoRow({ label, value }) {
  return (
    <div className='border-base-300/70 flex min-w-0 flex-col gap-1 border-b py-2 last:border-b-0 sm:flex-row sm:items-center sm:justify-between'>
      <span className='text-base-content/60 shrink-0 text-xs tracking-wide uppercase'>{label}</span>
      <span className='min-w-0 text-sm font-medium break-words'>{value || 'None'}</span>
    </div>
  );
}

function StatusPill({ tone = 'neutral', children }) {
  const tones = {
    neutral: 'badge-neutral',
    primary: 'badge-primary',
    success: 'badge-success',
    warning: 'badge-warning',
    error: 'badge-error',
  };
  return (
    <span className={`badge badge-sm whitespace-nowrap ${tones[tone] || tones.neutral}`}>
      {children}
    </span>
  );
}

function IconButton({ icon, label, onClick, tone = 'outline', disabled = false }) {
  return (
    <button
      type='button'
      className={`btn btn-square btn-sm shrink-0 ${tone === 'primary' ? 'btn-primary' : 'btn-outline'}`}
      disabled={disabled}
      onClick={onClick}
      aria-label={label}
      title={label}
    >
      <FontAwesomeIcon icon={icon} />
    </button>
  );
}

function optionalNumber(value) {
  if (value === '' || value === null || value === undefined) {
    return undefined;
  }
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : undefined;
}

function optionalText(value) {
  if (typeof value !== 'string') {
    return undefined;
  }
  const normalized = value.trim();
  return normalized && normalized.toLowerCase() !== 'null' ? normalized : undefined;
}

function formatNumber(value, digits = 1, suffix = '') {
  const number = optionalNumber(value);
  return number === undefined ? '-' : `${number.toFixed(digits)}${suffix}`;
}

function signedNumber(value) {
  const number = optionalNumber(value);
  return number === undefined ? '-' : `${number >= 0 ? '+' : ''}${number.toFixed(1)}`;
}

function recipeDomainForm(value = DEFAULT_RECIPE_DOMAIN) {
  return Object.fromEntries(
    Object.entries(DEFAULT_RECIPE_DOMAIN).map(([key, fallback]) => [
      key,
      String(optionalNumber(value?.[key]) ?? fallback),
    ]),
  );
}

function recipeDomainPayload(form) {
  return {
    grind_radius_steps: Number(form.grindRadiusSteps),
    dose_min_g: Number(form.doseMinG),
    dose_max_g: Number(form.doseMaxG),
    target_output_min_g: Number(form.targetOutputMinG),
    target_output_max_g: Number(form.targetOutputMaxG),
  };
}

function validRecipeDomainForm(form, currentDose) {
  const domain = recipeDomainPayload(form);
  const dose = optionalNumber(currentDose);
  return (
    Object.entries(domain).every(([key, value]) => {
      const [minimum, maximum] = RECIPE_DOMAIN_ENVELOPE[key];
      return Number.isFinite(value) && value >= minimum && value <= maximum;
    }) &&
    domain.dose_max_g > domain.dose_min_g &&
    domain.target_output_max_g > domain.target_output_min_g &&
    (dose === undefined || (dose >= domain.dose_min_g && dose <= domain.dose_max_g))
  );
}

function providerLabel(value) {
  return PROVIDER_LABELS[value] || 'Disabled';
}

function contextLabel(context, fallback = 'None') {
  if (!context) {
    return fallback;
  }
  const name = context.name || fallback;
  return Number(context.bag_index) > 0 ? `${name} - Bag ${context.bag_index}` : name;
}

function contextDropdownLabel(context) {
  const label = contextLabel(context);
  return context.status === 'retired' ? `${label} (retired)` : label;
}

function sortedContexts(contexts, activeId) {
  const rank = context => (context.id === activeId ? 0 : context.status === 'retired' ? 2 : 1);
  return [...contexts].sort((left, right) => rank(left) - rank(right));
}

function selectableContexts(contexts, activeId) {
  return contexts.filter(context => context.id === activeId || context.status !== 'retired');
}

function grinderAdjustmentMode(value) {
  return value === 'stepless' ? 'stepless' : 'stepped';
}

function emptyGrinderForm(context = null) {
  return {
    name: context?.name || '',
    currentAbsoluteStep:
      context?.current_absolute_step === undefined ? '' : String(context.current_absolute_step),
    micronsPerStep: context?.microns_per_step === undefined ? '' : String(context.microns_per_step),
    stepDirection: context?.step_direction || 'higher_is_finer',
    adjustmentMode: grinderAdjustmentMode(context?.grinder_adjustment_mode),
  };
}

function grinderPayload(form, { includeName = false, create = false } = {}) {
  const payload = {
    step_direction: form.stepDirection || 'higher_is_finer',
    grinder_adjustment_mode: grinderAdjustmentMode(form.adjustmentMode),
    reference_label: 'Initial setting',
  };
  if (includeName) {
    payload.name = form.name.trim();
  }
  const micronsPerStep = optionalNumber(form.micronsPerStep);
  if (micronsPerStep !== undefined) {
    payload.microns_per_step = micronsPerStep;
  }
  const currentAbsoluteStep = optionalNumber(form.currentAbsoluteStep);
  if (currentAbsoluteStep !== undefined) {
    payload.current_absolute_step = currentAbsoluteStep;
    if (create) {
      payload.absolute_reference_step = currentAbsoluteStep;
      payload.current_relative_step = 0;
    }
  }
  return payload;
}

function grinderPositionText(context) {
  const absolute = optionalNumber(context?.current_absolute_step);
  if (absolute !== undefined) {
    return `Setting ${absolute.toFixed(1)}`;
  }
  const relative = optionalNumber(context?.current_relative_step);
  return relative === undefined ? 'Not recorded' : `${signedNumber(relative)} steps from reference`;
}

function grinderCalibrationText(context) {
  if (!context || context.grinder_calibration_mode === 'uncalibrated') {
    return 'Not calibrated';
  }
  const adjustment =
    grinderAdjustmentMode(context.grinder_adjustment_mode) === 'stepless' ? 'Stepless' : 'Stepped';
  return `${adjustment}, ${formatNumber(context.microns_per_step, 1, ' um/step')}`;
}

function grinderSearchUrl(baseUrl, query) {
  if (!baseUrl || query.trim().length < 2) {
    return null;
  }
  try {
    const url = new URL(baseUrl, window.location.href);
    url.searchParams.set('q', query.trim());
    url.searchParams.set('limit', '6');
    return url.toString();
  } catch {
    return null;
  }
}

function suggestionSubtitle(suggestion) {
  const parts = [];
  if (suggestion.alias && suggestion.alias !== suggestion.name) {
    parts.push(suggestion.alias);
  }
  const microns = optionalNumber(suggestion.microns_per_step);
  if (microns !== undefined) {
    parts.push(`${microns.toFixed(1)} um/step`);
  }
  const min = optionalNumber(suggestion.min_steps);
  const max = optionalNumber(suggestion.max_steps);
  if (min !== undefined && max !== undefined) {
    parts.push(`${min.toFixed(0)}-${max.toFixed(0)} markers`);
  }
  return parts.join(' - ');
}

function applyGrinderSuggestion(form, suggestion) {
  const metadata =
    suggestion.metadata && typeof suggestion.metadata === 'object' ? suggestion.metadata : {};
  return {
    ...form,
    name: suggestion.name || form.name,
    micronsPerStep:
      suggestion.microns_per_step === undefined
        ? form.micronsPerStep
        : String(suggestion.microns_per_step),
    stepDirection: suggestion.step_direction || form.stepDirection,
    adjustmentMode: grinderAdjustmentMode(
      suggestion.grinder_adjustment_mode ||
        suggestion.adjustment_mode ||
        metadata.grinder_adjustment_mode ||
        metadata.adjustment_mode ||
        form.adjustmentMode,
    ),
  };
}

function currentRecommendation(settings, activeGrinder) {
  const recommendationId = optionalText(settings?.rlLastRecommendationId);
  if (!recommendationId) {
    return null;
  }
  const grindDeltaStepsFromCurrent = optionalNumber(
    settings.rlRecommendationGrindDeltaStepsFromCurrent,
  );
  const reportedCurrentAbsolute = settings.rlRecommendationHasCurrentAbsoluteStep
    ? optionalNumber(settings.rlRecommendationCurrentAbsoluteStep)
    : undefined;
  const reportedProjectedAbsolute = settings.rlRecommendationHasProjectedAbsoluteStep
    ? optionalNumber(settings.rlRecommendationProjectedAbsoluteStep)
    : undefined;
  const currentAbsolute =
    reportedCurrentAbsolute ?? optionalNumber(activeGrinder?.current_absolute_step);
  const projectedRelative = optionalNumber(
    settings.rlRecommendationProjectedRelativeStepFromReference,
  );
  const absoluteReference = optionalNumber(activeGrinder?.absolute_reference_step);
  const projectedAbsolute =
    reportedProjectedAbsolute ??
    (projectedRelative !== undefined && absoluteReference !== undefined
      ? absoluteReference + projectedRelative
      : currentAbsolute !== undefined && grindDeltaStepsFromCurrent !== undefined
        ? currentAbsolute + grindDeltaStepsFromCurrent
        : undefined);
  return {
    id: recommendationId,
    status: settings.rlRecommendationStatus || settings.rlRecommendationApplyStatus || '',
    grindDeltaStepsFromCurrent,
    projectedRelativeStepFromReference: projectedRelative,
    currentAbsoluteStep: currentAbsolute,
    projectedAbsoluteStep: projectedAbsolute,
    grinderStepDirection: activeGrinder?.step_direction || 'higher_is_finer',
    nextDoseG: optionalNumber(settings.rlRecommendationNextDoseG),
    targetYieldG: optionalNumber(settings.rlRecommendationTargetYieldG),
    targetRatio: optionalNumber(settings.rlRecommendationTargetRatio),
  };
}

function recommendationStatus(recommendation) {
  if (!recommendation) {
    return 'Waiting for a valid shot';
  }
  const labels = {
    accepted: 'Accepted',
    used: 'Accepted',
    ignored: 'Ignored',
    expired: 'Expired',
    superseded: 'Superseded',
  };
  return labels[recommendation.status] || 'Ready';
}

function recommendationGrind(recommendation) {
  if (!recommendation) {
    return '-';
  }
  if (
    Number.isFinite(recommendation.currentAbsoluteStep) &&
    Number.isFinite(recommendation.projectedAbsoluteStep)
  ) {
    return `Set ${recommendation.currentAbsoluteStep.toFixed(1)} -> ${recommendation.projectedAbsoluteStep.toFixed(1)}`;
  }
  const delta = Number(recommendation.grindDeltaStepsFromCurrent || 0);
  if (!Number.isFinite(delta) || Math.abs(delta) < 0.001) {
    return 'Keep grind';
  }
  const magnitude = Math.abs(delta);
  const steps = Number.isInteger(magnitude) ? magnitude.toFixed(0) : magnitude.toFixed(1);
  return `${steps} step${Math.abs(magnitude - 1) < 0.001 ? '' : 's'} ${grindDirectionForStepDelta(
    delta,
    recommendation.grinderStepDirection,
  )}`;
}

function runtimeHealth(settings) {
  const rejectedDeliveries = Number(settings?.rlLocalDeliveryRejectedCount || 0);
  const retryingDeliveries = Number(settings?.rlLocalDeliveryRetryCount || 0);
  const pendingDeliveries = Number(settings?.rlLocalDeliveryPendingCount || 0);
  if (rejectedDeliveries > 0) {
    return {
      tone: 'warning',
      summary: `${rejectedDeliveries} shot${rejectedDeliveries === 1 ? '' : 's'} rejected by local EspressoRL.`,
    };
  }
  if (retryingDeliveries > 0) {
    return {
      tone: 'warning',
      summary: `Retrying ${retryingDeliveries} shot deliver${retryingDeliveries === 1 ? 'y' : 'ies'}.`,
    };
  }
  if (pendingDeliveries > 0) {
    return {
      tone: 'neutral',
      summary: `Waiting for EspressoRL to accept ${pendingDeliveries} shot${pendingDeliveries === 1 ? '' : 's'}.`,
    };
  }
  const providerMode = settings?.rlProviderMode || 'disabled';
  if (providerMode === 'on_board') {
    return { tone: 'warning', summary: 'On-board optimization is not implemented yet.' };
  }
  if (providerMode === 'off_board' && !settings?.homeAssistant) {
    return { tone: 'warning', summary: 'Configure the MQTT broker to reach EspressoRL.' };
  }
  if (!settings?.rlStatusSeen) {
    return { tone: 'neutral', summary: 'Waiting for EspressoRL status.' };
  }
  if (!settings?.rlAddonOnline) {
    return { tone: 'warning', summary: 'EspressoRL is offline.' };
  }
  const status = settings?.rlRuntimeHealthStatus;
  return {
    tone:
      status === 'ready' || status === 'healthy'
        ? 'success'
        : status === 'error'
          ? 'error'
          : 'warning',
    summary: settings?.rlRuntimeHealthSummary || 'EspressoRL is connected.',
  };
}

function RuntimeDetails({ settings }) {
  const health = runtimeHealth(settings);
  const warnings = Array.isArray(settings?.rlRuntimeHealthWarnings)
    ? settings.rlRuntimeHealthWarnings
    : [];
  const waiting = Array.isArray(settings?.rlRuntimeHealthWaitingReasons)
    ? settings.rlRuntimeHealthWaitingReasons
    : [];
  const localPending = Number(settings?.rlLocalDeliveryPendingCount || 0);
  const localRetry = Number(settings?.rlLocalDeliveryRetryCount || 0);
  const localRejected = Number(settings?.rlLocalDeliveryRejectedCount || 0);
  const localDeliveryAttention = localPending + localRetry + localRejected > 0;
  const localDeliveryError = optionalText(settings?.rlLocalDeliveryLastError);
  return (
    <details className='border-base-300 mt-4 rounded-md border'>
      <summary className='flex cursor-pointer list-none items-center justify-between gap-3 p-3'>
        <span className='min-w-0 text-sm font-medium'>{health.summary}</span>
        <StatusPill tone={health.tone}>
          {localDeliveryAttention ? 'Delivery' : settings?.rlRuntimeHealthStatus || 'Status'}
        </StatusPill>
      </summary>
      <div className='border-base-300 space-y-2 border-t p-3'>
        <InfoRow label='Storage' value={settings?.rlRuntimeHealthStorageBackend || 'Unknown'} />
        {localDeliveryAttention && (
          <InfoRow
            label='Local delivery'
            value={`${localPending} pending / ${localRetry} retry / ${localRejected} rejected`}
          />
        )}
        {localDeliveryAttention && localDeliveryError && (
          <div className='text-warning text-sm'>
            Local delivery: {localDeliveryError}
          </div>
        )}
        {[...warnings, ...waiting].slice(0, 6).map(message => (
          <div key={message} className='text-base-content/70 text-sm'>
            {message}
          </div>
        ))}
      </div>
    </details>
  );
}

function GrinderFields({ form, setField, suggestions = [], searching = false, applySuggestion }) {
  return (
    <div className='space-y-4'>
      <label className='form-control'>
        <span className='label-text mb-1 text-sm'>Grinder name</span>
        <input
          type='search'
          className='input input-bordered w-full'
          value={form.name}
          onInput={event => setField('name', event.currentTarget.value)}
          autocomplete='off'
          placeholder='Start typing a grinder name'
        />
      </label>
      {searching && <div className='text-base-content/60 text-sm'>Searching...</div>}
      {suggestions.length > 0 && (
        <div className='border-base-300 divide-base-300 divide-y rounded-md border'>
          {suggestions.map(suggestion => (
            <button
              key={suggestion.catalog_id || `${suggestion.name}:${suggestion.alias || ''}`}
              type='button'
              className='hover:bg-base-200 flex w-full flex-col p-3 text-left'
              onClick={() => applySuggestion(suggestion)}
            >
              <span className='font-medium'>{suggestion.name}</span>
              {suggestionSubtitle(suggestion) && (
                <span className='text-base-content/60 text-xs'>
                  {suggestionSubtitle(suggestion)}
                </span>
              )}
            </button>
          ))}
        </div>
      )}
      <div className='grid grid-cols-1 gap-3 sm:grid-cols-2'>
        <label className='form-control'>
          <span className='label-text mb-1 text-sm'>Current setting</span>
          <input
            type='number'
            step={form.adjustmentMode === 'stepless' ? '0.1' : '1'}
            className='input input-bordered w-full'
            value={form.currentAbsoluteStep}
            onInput={event => setField('currentAbsoluteStep', event.currentTarget.value)}
            placeholder='Optional'
          />
        </label>
        <label className='form-control'>
          <span className='label-text mb-1 text-sm'>Microns per marker</span>
          <input
            type='number'
            min='0.1'
            step='0.1'
            className='input input-bordered w-full'
            value={form.micronsPerStep}
            onInput={event => setField('micronsPerStep', event.currentTarget.value)}
            placeholder='Optional'
          />
        </label>
        <label className='form-control'>
          <span className='label-text mb-1 text-sm'>Scale direction</span>
          <select
            className='select select-bordered w-full'
            value={form.stepDirection}
            onChange={event => setField('stepDirection', event.currentTarget.value)}
          >
            <option value='higher_is_finer'>Higher is finer</option>
            <option value='higher_is_coarser'>Higher is coarser</option>
          </select>
        </label>
        <label className='form-control'>
          <span className='label-text mb-1 text-sm'>Adjustment</span>
          <select
            className='select select-bordered w-full'
            value={form.adjustmentMode}
            onChange={event => setField('adjustmentMode', event.currentTarget.value)}
          >
            <option value='stepped'>Stepped</option>
            <option value='stepless'>Stepless</option>
          </select>
        </label>
      </div>
    </div>
  );
}

export function AutoTuning() {
  const apiService = useContext(ApiServiceContext);
  const [settings, setSettings] = useState(null);
  const [loading, setLoading] = useState(true);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [dialog, setDialog] = useState(null);
  const [drawer, setDrawer] = useState(null);
  const [beanName, setBeanName] = useState('');
  const [beanEditName, setBeanEditName] = useState('');
  const [grinderForm, setGrinderForm] = useState(emptyGrinderForm());
  const [grinderSuggestions, setGrinderSuggestions] = useState([]);
  const [grinderSearchLoading, setGrinderSearchLoading] = useState(false);
  const [tasteGoalDraft, setTasteGoalDraft] = useState({ ...BALANCED_TASTE_GOAL, targets: {} });
  const [recipeDomainDraft, setRecipeDomainDraft] = useState(recipeDomainForm());
  const [cpboProfileDraft, setCpboProfileDraft] = useState('application');
  const [cpboComparisonDraft, setCpboComparisonDraft] = useState('best_incumbent');
  const [doseTargetDraft, setDoseTargetDraft] = useState('18');

  const loadSettings = useCallback(async () => {
    const response = await fetch('/api/settings');
    if (!response.ok) {
      throw new Error('Unable to load Auto Tuning settings.');
    }
    const loaded = await response.json();
    setSettings(loaded);
    setDoseTargetDraft(String(optionalNumber(loaded.rlDoseTargetG) ?? 18));
    setLoading(false);
  }, []);

  useEffect(() => {
    loadSettings().catch(caught => {
      setError(caught.message);
      setLoading(false);
    });
  }, [loadSettings]);

  useEffect(() => {
    if (!apiService) {
      return undefined;
    }
    const statusListener = apiService.on('evt:rl:status', message => {
      setSettings(current => ({ ...(current || {}), ...message }));
      if (optionalNumber(message.rlDoseTargetG) !== undefined) {
        setDoseTargetDraft(String(message.rlDoseTargetG));
      }
    });
    const recommendationListener = apiService.on('evt:rl:recommendation', message => {
      setSettings(current => ({
        ...(current || {}),
        rlLastRecommendationId: message.recommendation_id,
        rlRecommendationStatus: message.status,
        rlRecommendationMode: message.mode,
        rlRecommendationGrindDeltaStepsFromCurrent: message.grind_delta_steps_from_current,
        rlRecommendationProjectedRelativeStepFromReference:
          message.projected_relative_step_from_reference,
        rlRecommendationHasCurrentAbsoluteStep: message.has_current_absolute_step,
        rlRecommendationCurrentAbsoluteStep: message.current_absolute_step,
        rlRecommendationHasProjectedAbsoluteStep: message.has_projected_absolute_step,
        rlRecommendationProjectedAbsoluteStep: message.projected_absolute_step,
        rlRecommendationNextDoseG: message.next_dose_g,
        rlRecommendationTargetYieldG: message.target_yield_g,
        rlRecommendationTargetRatio: message.target_ratio,
      }));
    });
    const recommendationClearListener = apiService.on('evt:rl:recommendation-clear', () => {
      setSettings(current => ({
        ...(current || {}),
        rlLastRecommendationId: '',
        rlRecommendationStatus: '',
        rlRecommendationApplyStatus: '',
      }));
    });
    apiService.request({ tp: 'req:rl:status:refresh' }).catch(() => {});
    return () => {
      apiService.off('evt:rl:status', statusListener);
      apiService.off('evt:rl:recommendation', recommendationListener);
      apiService.off('evt:rl:recommendation-clear', recommendationClearListener);
    };
  }, [apiService]);

  const run = useCallback(
    async (tp, payload = {}) => {
      if (!apiService) {
        return false;
      }
      setBusy(true);
      setError('');
      try {
        const response = await apiService.request({ tp, ...payload });
        if (response?.error) {
          throw new Error(response.error);
        }
        await loadSettings();
        return true;
      } catch (caught) {
        setError(caught?.message || 'Auto Tuning request failed.');
        return false;
      } finally {
        setBusy(false);
      }
    },
    [apiService, loadSettings],
  );

  const setGrinderField = useCallback((field, value) => {
    setGrinderForm(current => ({ ...current, [field]: value }));
  }, []);

  const grinderCatalogSearchBaseUrl = settings?.rlGrinderCatalogSearchUrl || '';
  useEffect(() => {
    if (dialog !== 'grinder-create') {
      setGrinderSuggestions([]);
      setGrinderSearchLoading(false);
      return undefined;
    }
    const url = grinderSearchUrl(grinderCatalogSearchBaseUrl, grinderForm.name);
    if (!url) {
      setGrinderSuggestions([]);
      return undefined;
    }
    const controller = new AbortController();
    setGrinderSearchLoading(true);
    const timeout = window.setTimeout(async () => {
      try {
        const response = await fetch(url, { signal: controller.signal });
        const data = response.ok ? await response.json() : {};
        setGrinderSuggestions(Array.isArray(data.suggestions) ? data.suggestions : []);
      } catch {
        if (!controller.signal.aborted) {
          setGrinderSuggestions([]);
        }
      } finally {
        if (!controller.signal.aborted) {
          setGrinderSearchLoading(false);
        }
      }
    }, 250);
    return () => {
      window.clearTimeout(timeout);
      controller.abort();
    };
  }, [dialog, grinderCatalogSearchBaseUrl, grinderForm.name]);

  if (loading) {
    return (
      <div className='flex w-full items-center justify-center py-16'>
        <Spinner size={8} />
      </div>
    );
  }
  if (!settings?.rlAutoTuningEnabled) {
    return (
      <div className='mx-auto w-full max-w-4xl space-y-4'>
        <h1 className='text-2xl font-bold sm:text-3xl'>Auto Tuning</h1>
        <Panel>Enable Auto Tuning in Settings to use this page.</Panel>
      </div>
    );
  }

  const beanContexts = Array.isArray(settings.rlBeanContexts) ? settings.rlBeanContexts : [];
  const grinderContexts = Array.isArray(settings.rlGrinderContexts)
    ? settings.rlGrinderContexts
    : [];
  const activeBean = beanContexts.find(context => context.id === settings.rlBeanContextId) || null;
  const activeGrinder =
    grinderContexts.find(context => context.id === settings.rlGrinderContextId) || null;
  const activeTasteGoal = normalizeTasteGoal(settings.rlTasteGoal);
  const activeTasteGoalSummary = tasteGoalSummary(activeTasteGoal);
  const recommendation = currentRecommendation(settings, activeGrinder);
  const recommendationPromptable =
    !!recommendation && PROMPTABLE_RECOMMENDATION_STATUSES.has(recommendation.status || '');
  const paused = !!settings.rlOptimizationPaused;
  const localOn = !!settings.rlLocalOptimizationEnabled;
  const recipeDomain = settings.rlRecipeDomain || DEFAULT_RECIPE_DOMAIN;
  const doseTarget = optionalNumber(doseTargetDraft);
  const doseTargetValid =
    doseTarget !== undefined &&
    doseTarget >= recipeDomain.doseMinG &&
    doseTarget <= recipeDomain.doseMaxG;

  const openBeanDrawer = () => {
    setBeanEditName(activeBean?.name || '');
    setDrawer('bean');
  };
  const openGrinderDrawer = () => {
    setGrinderForm(emptyGrinderForm(activeGrinder));
    setDrawer('grinder');
  };
  const openTasteGoalDrawer = () => {
    setTasteGoalDraft(activeTasteGoal);
    setDrawer('taste-goal');
  };
  const openAdvancedDrawer = () => {
    setRecipeDomainDraft(recipeDomainForm(settings.rlRecipeDomain));
    setCpboProfileDraft(cpboProfileName(settings.rlCPBOProfileName));
    setCpboComparisonDraft(cpboComparisonMode(settings.rlCPBOComparisonMode));
    setDrawer('advanced');
  };

  return (
    <div className='mx-auto flex w-full max-w-6xl flex-col gap-4'>
      <div className='flex flex-col gap-3 sm:flex-row sm:items-start sm:justify-between'>
        <div>
          <h1 className='text-2xl font-bold sm:text-3xl'>Auto Tuning</h1>
          <div className='mt-2 flex flex-wrap gap-2'>
            <StatusPill tone='primary'>{providerLabel(settings.rlProviderMode)}</StatusPill>
            <StatusPill tone={paused ? 'warning' : localOn ? 'success' : 'neutral'}>
              {paused ? 'Paused' : localOn ? 'Optimizing' : 'Observation only'}
            </StatusPill>
          </div>
        </div>
        <div className='flex flex-wrap gap-2'>
          <button
            type='button'
            className='btn btn-outline btn-sm'
            disabled={busy}
            onClick={openAdvancedDrawer}
          >
            <FontAwesomeIcon icon={faGear} />
            Advanced
          </button>
          <button
            type='button'
            className={paused ? 'btn btn-primary btn-sm' : 'btn btn-outline btn-sm'}
            disabled={busy}
            onClick={() => run(paused ? 'req:rl:optimization:resume' : 'req:rl:optimization:pause')}
          >
            <FontAwesomeIcon icon={paused ? faPlay : faPause} />
            {paused ? 'Resume' : 'Pause'}
          </button>
          <button
            type='button'
            className='btn btn-error btn-outline btn-sm'
            disabled={busy}
            onClick={() => {
              if (
                window.confirm(
                  'Reset local Auto Tuning shots, recommendations, queues, and contexts?',
                )
              ) {
                run('req:rl:local-reset');
              }
            }}
          >
            <FontAwesomeIcon icon={faTrashCan} />
            Reset local data
          </button>
        </div>
      </div>

      {error && <div className='alert alert-error text-sm'>{error}</div>}

      <Panel title='Active Session'>
        <div className='grid grid-cols-1 gap-3 md:grid-cols-2 xl:grid-cols-4'>
          <div>
            <div className='mb-1 text-xs tracking-wide uppercase opacity-60'>Bean context</div>
            <div className='flex gap-2'>
              <select
                className='select select-bordered min-w-0 flex-1'
                value={settings.rlBeanContextId || ''}
                disabled={busy}
                onChange={event => run('req:rl:context:switch', { id: event.currentTarget.value })}
              >
                <option value='' disabled>
                  Select bean
                </option>
                {selectableContexts(beanContexts, settings.rlBeanContextId).map(context => (
                  <option key={context.id} value={context.id}>
                    {contextDropdownLabel(context)}
                  </option>
                ))}
              </select>
              <IconButton
                icon={faGear}
                label='Manage bean contexts'
                onClick={openBeanDrawer}
                disabled={busy}
              />
              <IconButton
                icon={faPlus}
                label='Add bean context'
                tone='primary'
                disabled={busy}
                onClick={() => {
                  setBeanName('');
                  setDialog('bean-create');
                }}
              />
            </div>
          </div>

          <div>
            <div className='mb-1 text-xs tracking-wide uppercase opacity-60'>Grinder context</div>
            <div className='flex gap-2'>
              <select
                className='select select-bordered min-w-0 flex-1'
                value={settings.rlGrinderContextId || ''}
                disabled={busy}
                onChange={event =>
                  run('req:rl:grinder-context:switch', { id: event.currentTarget.value })
                }
              >
                <option value='' disabled>
                  Select grinder
                </option>
                {selectableContexts(grinderContexts, settings.rlGrinderContextId).map(context => (
                  <option key={context.id} value={context.id}>
                    {contextDropdownLabel(context)}
                  </option>
                ))}
              </select>
              <IconButton
                icon={faGear}
                label='Manage grinder contexts'
                onClick={openGrinderDrawer}
                disabled={busy}
              />
              <IconButton
                icon={faPlus}
                label='Add grinder context'
                tone='primary'
                disabled={busy}
                onClick={() => {
                  setGrinderForm(emptyGrinderForm());
                  setDialog('grinder-create');
                }}
              />
            </div>
          </div>

          <div>
            <div className='mb-1 text-xs tracking-wide uppercase opacity-60'>Taste goal</div>
            <button
              type='button'
              className='btn btn-outline h-12 w-full min-w-0 justify-between px-3 font-medium normal-case'
              disabled={busy || !activeBean || !activeGrinder}
              onClick={openTasteGoalDrawer}
            >
              <span className='min-w-0 truncate'>{activeTasteGoalSummary}</span>
              <FontAwesomeIcon icon={faGear} className='shrink-0' />
            </button>
          </div>

          <div>
            <div className='mb-1 text-xs tracking-wide uppercase opacity-60'>Dose target</div>
            <div className='flex gap-2'>
              <label className='input input-bordered flex h-12 min-w-0 flex-1 items-center gap-2'>
                <input
                  type='number'
                  min={recipeDomain.doseMinG}
                  max={recipeDomain.doseMaxG}
                  step='0.1'
                  className='min-w-0 flex-1'
                  value={doseTargetDraft}
                  disabled={busy}
                  onInput={event => setDoseTargetDraft(event.currentTarget.value)}
                  aria-label='Dose target in grams'
                />
                <span className='text-base-content/60 text-sm'>g</span>
              </label>
              <IconButton
                icon={faSave}
                label='Save dose target'
                tone='primary'
                disabled={busy || !doseTargetValid}
                onClick={() => run('req:rl:dose-target:set', { dose_target_g: doseTarget })}
              />
            </div>
          </div>
        </div>

        <div className='mt-4 grid grid-cols-1 gap-3 md:grid-cols-2'>
          <div className='bg-base-200/60 rounded-md p-3'>
            <InfoRow label='Active bean' value={contextLabel(activeBean, 'No bean selected')} />
            <InfoRow label='Active grinder' value={activeGrinder?.name || 'No grinder selected'} />
            <InfoRow label='Grinder position' value={grinderPositionText(activeGrinder)} />
            <InfoRow label='Calibration' value={grinderCalibrationText(activeGrinder)} />
          </div>
          <div className='bg-base-200/60 rounded-md p-3'>
            <InfoRow
              label='Profile scope'
              value={
                settings.rlOptimizerProfileLabel ||
                settings.rlOptimizerProfileId ||
                'Waiting for profile'
              }
            />
            <InfoRow label='Taste goal' value={activeTasteGoalSummary} />
            <InfoRow label='Provider' value={providerLabel(settings.rlProviderMode)} />
            <InfoRow label='Recommendation' value={recommendationStatus(recommendation)} />
          </div>
        </div>

        <RuntimeDetails settings={settings} />

        <div className='mt-5'>
          <div className='mb-3 flex items-center justify-between gap-3'>
            <h3 className='text-sm font-semibold tracking-wide uppercase'>
              Current Recommendation
            </h3>
            <span className='text-base-content/60 text-sm'>
              {recommendationStatus(recommendation)}
            </span>
          </div>
          {recommendation ? (
            <div className='grid grid-cols-1 gap-4 lg:grid-cols-[minmax(0,1fr)_auto]'>
              <div className='grid grid-cols-1 gap-3 sm:grid-cols-2 xl:grid-cols-4'>
                <StatCard
                  label='Grind'
                  value={recommendationGrind(recommendation)}
                  tone='primary'
                />
                <StatCard label='Dose' value={formatNumber(recommendation.nextDoseG, 1, 'g')} />
                <StatCard label='Yield' value={formatNumber(recommendation.targetYieldG, 1, 'g')} />
                <StatCard label='Ratio' value={formatNumber(recommendation.targetRatio, 2)} />
              </div>
              <div className='flex min-w-0 flex-col justify-end gap-2 lg:w-56'>
                <div className='text-base-content/70 text-sm'>
                  {recommendation.projectedRelativeStepFromReference === undefined
                    ? 'Relative position unavailable'
                    : `Projected position ${signedNumber(
                        recommendation.projectedRelativeStepFromReference,
                      )} steps from reference`}
                </div>
                <button
                  type='button'
                  className='btn btn-primary w-full'
                  disabled={busy || !recommendationPromptable}
                  onClick={() =>
                    run('req:rl:recommendation:use', { recommendation_id: recommendation.id })
                  }
                >
                  <FontAwesomeIcon icon={faPlay} />
                  Apply
                </button>
              </div>
            </div>
          ) : (
            <div className='bg-base-200/60 rounded-md p-4 text-sm opacity-70'>
              Pull a valid shot in this context. After each proposed shot, compare it with the
              requested anchor.
            </div>
          )}
        </div>
      </Panel>

      <Panel title='Progress'>
        <div className='grid grid-cols-1 gap-3 sm:grid-cols-3'>
          <StatCard label='Local shots' value={(settings.rlLocalShotCount ?? 0).toString()} />
          <StatCard label='Optimizer' value='CPBO' tone='primary' />
          <StatCard label='Provider' value={providerLabel(settings.rlProviderMode)} />
        </div>
      </Panel>

      {drawer === 'advanced' && (
        <Drawer
          title='Advanced Optimizer'
          subtitle='CPBO policy and recipe search space'
          onClose={() => setDrawer(null)}
        >
          <div className='space-y-5'>
            <section className='space-y-3'>
              <h3 className='text-sm font-semibold tracking-wide uppercase'>Search policy</h3>
              <div
                className='join join-vertical w-full sm:join-horizontal'
                role='radiogroup'
                aria-label='CPBO search policy'
              >
                {[
                  ['best_incumbent', 'Best incumbent / local'],
                  ['global_previous', 'Previous shot / global'],
                ].map(([value, label]) => {
                  const selected = cpboComparisonDraft === value;
                  return (
                    <button
                      key={value}
                      type='button'
                      role='radio'
                      aria-checked={selected}
                      className={`join-item btn btn-sm min-h-12 w-full min-w-0 whitespace-normal sm:flex-1 ${
                        selected ? 'btn-primary' : 'btn-outline'
                      }`}
                      onClick={() => setCpboComparisonDraft(value)}
                    >
                      {label}
                    </button>
                  );
                })}
              </div>
            </section>

            <section className='space-y-3'>
              <h3 className='text-sm font-semibold tracking-wide uppercase'>Compute profile</h3>
              <select
                className='select select-bordered w-full'
                value={cpboProfileDraft}
                onChange={event => setCpboProfileDraft(event.currentTarget.value)}
              >
                <option value='application'>Application</option>
                <option value='paper_fidelity'>Paper fidelity (slow)</option>
              </select>
            </section>

            <section className='space-y-3'>
              <h3 className='text-sm font-semibold tracking-wide uppercase'>Grinder</h3>
              <label className='form-control'>
                <span className='label-text mb-1 text-sm'>Radius from baseline (steps)</span>
                <input
                  type='number'
                  min='0.1'
                  max='1000'
                  step='0.1'
                  className='input input-bordered w-full'
                  value={recipeDomainDraft.grindRadiusSteps}
                  onInput={event =>
                    setRecipeDomainDraft(current => ({
                      ...current,
                      grindRadiusSteps: event.currentTarget.value,
                    }))
                  }
                />
              </label>
            </section>

            <section className='space-y-3'>
              <h3 className='text-sm font-semibold tracking-wide uppercase'>Dose</h3>
              <div className='grid grid-cols-2 gap-3'>
                {[
                  ['doseMinG', 'Minimum (g)'],
                  ['doseMaxG', 'Maximum (g)'],
                ].map(([key, label]) => (
                  <label key={key} className='form-control min-w-0'>
                    <span className='label-text mb-1 text-sm'>{label}</span>
                    <input
                      type='number'
                      min='0.1'
                      max='100'
                      step='0.1'
                      className='input input-bordered w-full min-w-0'
                      value={recipeDomainDraft[key]}
                      onInput={event =>
                        setRecipeDomainDraft(current => ({
                          ...current,
                          [key]: event.currentTarget.value,
                        }))
                      }
                    />
                  </label>
                ))}
              </div>
            </section>

            <section className='space-y-3'>
              <h3 className='text-sm font-semibold tracking-wide uppercase'>Target output</h3>
              <div className='grid grid-cols-2 gap-3'>
                {[
                  ['targetOutputMinG', 'Minimum (g)'],
                  ['targetOutputMaxG', 'Maximum (g)'],
                ].map(([key, label]) => (
                  <label key={key} className='form-control min-w-0'>
                    <span className='label-text mb-1 text-sm'>{label}</span>
                    <input
                      type='number'
                      min='0.1'
                      max='1000'
                      step='0.1'
                      className='input input-bordered w-full min-w-0'
                      value={recipeDomainDraft[key]}
                      onInput={event =>
                        setRecipeDomainDraft(current => ({
                          ...current,
                          [key]: event.currentTarget.value,
                        }))
                      }
                    />
                  </label>
                ))}
              </div>
            </section>

            <div className='grid grid-cols-2 gap-3'>
              <button
                type='button'
                className='btn btn-outline'
                disabled={busy}
                onClick={() => {
                  setRecipeDomainDraft(recipeDomainForm());
                  setCpboProfileDraft('application');
                  setCpboComparisonDraft('best_incumbent');
                }}
              >
                Restore defaults
              </button>
              <button
                type='button'
                className='btn btn-primary'
                disabled={busy || !validRecipeDomainForm(recipeDomainDraft, settings.rlDoseTargetG)}
                onClick={async () => {
                  const saved = await run('req:rl:cpbo-config:set', {
                    cpbo_profile_name: cpboProfileDraft,
                    cpbo_comparison_mode: cpboComparisonDraft,
                    recipe_domain: recipeDomainPayload(recipeDomainDraft),
                  });
                  if (saved) {
                    setDrawer(null);
                  }
                }}
              >
                <FontAwesomeIcon icon={faSave} /> Save
              </button>
            </div>
          </div>
        </Drawer>
      )}

      {drawer === 'bean' && (
        <Drawer
          title='Bean Contexts'
          subtitle={contextLabel(activeBean, 'No bean selected')}
          onClose={() => setDrawer(null)}
        >
          <div className='space-y-5'>
            {activeBean && (
              <section className='space-y-3'>
                <h3 className='text-sm font-semibold tracking-wide uppercase'>Selected bean</h3>
                <label className='form-control'>
                  <span className='label-text mb-1 text-sm'>Name</span>
                  <input
                    className='input input-bordered w-full'
                    value={beanEditName}
                    onInput={event => setBeanEditName(event.currentTarget.value)}
                  />
                </label>
                <div className='grid grid-cols-1 gap-2 sm:grid-cols-2'>
                  <button
                    type='button'
                    className='btn btn-primary'
                    disabled={busy || !beanEditName.trim()}
                    onClick={() =>
                      run('req:rl:context:update', { id: activeBean.id, name: beanEditName.trim() })
                    }
                  >
                    <FontAwesomeIcon icon={faSave} /> Save
                  </button>
                  <button
                    type='button'
                    className='btn btn-outline'
                    disabled={busy}
                    onClick={() => run('req:rl:context:start-bag')}
                  >
                    <FontAwesomeIcon icon={faPlus} /> Start next bag
                  </button>
                </div>
              </section>
            )}
            <section className='space-y-3'>
              <h3 className='text-sm font-semibold tracking-wide uppercase'>All bean contexts</h3>
              {sortedContexts(beanContexts, settings.rlBeanContextId).map(context => (
                <div
                  key={context.id}
                  className='border-base-300 flex items-center gap-3 rounded-md border p-3'
                >
                  <div className='min-w-0 flex-1'>
                    <div className='font-medium break-words'>{contextLabel(context)}</div>
                    <div className='text-base-content/60 text-xs capitalize'>
                      {context.status || 'available'}
                    </div>
                  </div>
                  {context.id !== settings.rlBeanContextId && context.status !== 'retired' && (
                    <button
                      type='button'
                      className='btn btn-outline btn-sm'
                      disabled={busy}
                      onClick={() => run('req:rl:context:switch', { id: context.id })}
                    >
                      Use
                    </button>
                  )}
                  <IconButton
                    icon={faTrashCan}
                    label={`Delete ${context.name}`}
                    disabled={busy}
                    onClick={() => {
                      if (window.confirm(`Delete ${contextLabel(context)}?`)) {
                        run('req:rl:context:delete', { id: context.id });
                      }
                    }}
                  />
                </div>
              ))}
            </section>
          </div>
        </Drawer>
      )}

      {drawer === 'grinder' && (
        <Drawer
          title='Grinder Contexts'
          subtitle={activeGrinder?.name || 'No grinder selected'}
          onClose={() => setDrawer(null)}
        >
          <div className='space-y-5'>
            {activeGrinder && (
              <section className='space-y-3'>
                <h3 className='text-sm font-semibold tracking-wide uppercase'>Selected grinder</h3>
                <GrinderFields form={grinderForm} setField={setGrinderField} />
                <button
                  type='button'
                  className='btn btn-primary w-full'
                  disabled={busy || !grinderForm.name.trim()}
                  onClick={() =>
                    run('req:rl:grinder-context:update', {
                      id: activeGrinder.id,
                      ...grinderPayload(grinderForm, { includeName: true }),
                    })
                  }
                >
                  <FontAwesomeIcon icon={faSave} /> Save grinder
                </button>
              </section>
            )}
            <section className='space-y-3'>
              <h3 className='text-sm font-semibold tracking-wide uppercase'>
                All grinder contexts
              </h3>
              {sortedContexts(grinderContexts, settings.rlGrinderContextId).map(context => (
                <div
                  key={context.id}
                  className='border-base-300 flex items-center gap-3 rounded-md border p-3'
                >
                  <div className='min-w-0 flex-1'>
                    <div className='font-medium break-words'>{context.name}</div>
                    <div className='text-base-content/60 text-xs'>
                      {grinderCalibrationText(context)}
                    </div>
                  </div>
                  {context.id !== settings.rlGrinderContextId && context.status !== 'retired' && (
                    <button
                      type='button'
                      className='btn btn-outline btn-sm'
                      disabled={busy}
                      onClick={() => run('req:rl:grinder-context:switch', { id: context.id })}
                    >
                      Use
                    </button>
                  )}
                  <IconButton
                    icon={faTrashCan}
                    label={`Delete ${context.name}`}
                    disabled={busy}
                    onClick={() => {
                      if (window.confirm(`Delete ${context.name}?`)) {
                        run('req:rl:grinder-context:delete', { id: context.id });
                      }
                    }}
                  />
                </div>
              ))}
            </section>
          </div>
        </Drawer>
      )}

      {drawer === 'taste-goal' && (
        <Drawer
          title='Taste Goal'
          subtitle={`${contextLabel(activeBean)} / ${activeGrinder?.name || 'No grinder selected'}`}
          onClose={() => setDrawer(null)}
        >
          <div className='space-y-5'>
            <div
              className='border-base-300 grid grid-cols-2 overflow-hidden rounded-md border'
              role='group'
              aria-label='Taste goal mode'
            >
              {[
                ['balanced', 'Balanced'],
                ['custom', 'Custom'],
              ].map(([mode, label]) => (
                <button
                  key={mode}
                  type='button'
                  className={`h-11 px-3 font-medium ${
                    tasteGoalDraft.mode === mode
                      ? 'bg-primary text-primary-content'
                      : 'bg-base-100 hover:bg-base-200'
                  }`}
                  onClick={() =>
                    setTasteGoalDraft(
                      mode === 'balanced'
                        ? { ...BALANCED_TASTE_GOAL, targets: {} }
                        : {
                            schema_version: 1,
                            mode: 'custom',
                            targets: { ...(tasteGoalDraft.targets || {}) },
                          },
                    )
                  }
                >
                  {label}
                </button>
              ))}
            </div>

            {tasteGoalDraft.mode === 'custom' &&
              TASTE_TAG_GROUPS.map(group => (
                <section key={group.key} className='space-y-3'>
                  <h3 className='text-sm font-semibold tracking-wide uppercase'>{group.label}</h3>
                  {group.tags.map(tag => (
                    <div key={tag.value} className='space-y-2'>
                      <div className='text-sm font-medium'>{tag.label}</div>
                      <div className='border-base-300 grid grid-cols-4 overflow-hidden rounded-md border'>
                        {TASTE_LEVELS.map((level, index) => {
                          const selected =
                            (tasteGoalDraft.targets?.[tag.value] || 'unspecified') === level;
                          return (
                            <button
                              key={level}
                              type='button'
                              className={`min-w-0 px-1 py-2 text-xs font-medium sm:text-sm ${
                                selected
                                  ? 'bg-primary text-primary-content'
                                  : 'bg-base-100 hover:bg-base-200'
                              }`}
                              onClick={() =>
                                setTasteGoalDraft(current =>
                                  setTasteGoalTarget(current, tag.value, level),
                                )
                              }
                            >
                              {TASTE_LEVEL_LABELS[index]}
                            </button>
                          );
                        })}
                      </div>
                    </div>
                  ))}
                </section>
              ))}

            <button
              type='button'
              className='btn btn-primary w-full'
              disabled={
                busy ||
                (tasteGoalDraft.mode === 'custom' &&
                  Object.keys(tasteGoalDraft.targets || {}).length === 0)
              }
              onClick={async () => {
                const saved = await run('req:rl:taste-goal:set', { taste_goal: tasteGoalDraft });
                if (saved) {
                  setDrawer(null);
                }
              }}
            >
              <FontAwesomeIcon icon={faSave} /> Save taste goal
            </button>
          </div>
        </Drawer>
      )}

      {dialog === 'bean-create' && (
        <Modal title='Add Bean Context' onClose={() => setDialog(null)}>
          <form
            className='space-y-4'
            onSubmit={async event => {
              event.preventDefault();
              await run('req:rl:context:start-bean', { name: beanName.trim() });
              setDialog(null);
            }}
          >
            <label className='form-control'>
              <span className='label-text mb-1 text-sm'>Bean name</span>
              <input
                className='input input-bordered w-full'
                value={beanName}
                onInput={event => setBeanName(event.currentTarget.value)}
                autocomplete='off'
                autofocus
                required
              />
            </label>
            <button
              type='submit'
              className='btn btn-primary w-full'
              disabled={busy || !beanName.trim()}
            >
              <FontAwesomeIcon icon={faPlus} /> Add bean
            </button>
          </form>
        </Modal>
      )}

      {dialog === 'grinder-create' && (
        <Modal title='Add Grinder Context' onClose={() => setDialog(null)}>
          <form
            className='space-y-4'
            onSubmit={async event => {
              event.preventDefault();
              await run(
                'req:rl:grinder-context:create',
                grinderPayload(grinderForm, { includeName: true, create: true }),
              );
              setDialog(null);
            }}
          >
            <GrinderFields
              form={grinderForm}
              setField={setGrinderField}
              suggestions={grinderSuggestions}
              searching={grinderSearchLoading}
              applySuggestion={suggestion => {
                setGrinderForm(current => applyGrinderSuggestion(current, suggestion));
                setGrinderSuggestions([]);
              }}
            />
            <button
              type='submit'
              className='btn btn-primary w-full'
              disabled={busy || !grinderForm.name.trim()}
            >
              <FontAwesomeIcon icon={faPlus} /> Add grinder
            </button>
          </form>
        </Modal>
      )}
    </div>
  );
}
