import { useCallback, useContext, useEffect, useState } from 'preact/hooks';
import { ApiServiceContext } from '../services/ApiService.js';

const RATING_DISMISS_MS = 45000;
const RECOMMENDATION_STATUSES = new Set(['', 'pending', 'shown']);

function formatDose(value) {
  const dose = Number(value);
  return Number.isFinite(dose) && dose > 0 ? `${dose.toFixed(1)}g` : '-';
}

function formatYield(value) {
  const yieldG = Number(value);
  return Number.isFinite(yieldG) && yieldG > 0 ? `${yieldG.toFixed(1)}g` : '-';
}

function formatRatio(value) {
  const ratio = Number(value);
  return Number.isFinite(ratio) && ratio > 0 ? `1:${ratio.toFixed(2)}` : '-';
}

function formatGrind(deltaSteps) {
  const delta = Number(deltaSteps || 0);
  if (!Number.isFinite(delta) || delta === 0) {
    return 'Keep grind';
  }
  const steps = Math.abs(delta);
  const label = steps === 1 ? 'step' : 'steps';
  return `${steps} ${label} ${delta > 0 ? 'finer' : 'coarser'}`;
}

export function RLPromptOverlay() {
  const apiService = useContext(ApiServiceContext);
  const [prompt, setPrompt] = useState(null);

  useEffect(() => {
    if (!apiService) {
      return undefined;
    }

    const recommendationListener = apiService.on('evt:rl:recommendation', message => {
      const status = message.status || '';
      if (!message.recommendation_id || !RECOMMENDATION_STATUSES.has(status)) {
        return;
      }
      setPrompt({
        type: 'recommendation',
        ...message,
      });
    });

    const shotCompleteListener = apiService.on('evt:rl:shot-complete', message => {
      if (!message.shot_id) {
        return;
      }
      setPrompt({
        type: 'rating',
        shot_id: message.shot_id,
        recommendation_id: message.recommendation_id,
      });
    });

    const brewStartListener = apiService.on('evt:status', message => {
      if (!message.process?.a) {
        return;
      }
      setPrompt(current => (current?.type === 'rating' ? null : current));
    });

    return () => {
      apiService.off('evt:rl:recommendation', recommendationListener);
      apiService.off('evt:rl:shot-complete', shotCompleteListener);
      apiService.off('evt:status', brewStartListener);
    };
  }, [apiService]);

  useEffect(() => {
    if (prompt?.type !== 'rating') {
      return undefined;
    }
    const timeout = window.setTimeout(() => setPrompt(null), RATING_DISMISS_MS);
    return () => window.clearTimeout(timeout);
  }, [prompt]);

  const close = useCallback(() => setPrompt(null), []);

  const useRecommendation = useCallback(() => {
    if (!prompt?.recommendation_id) {
      return;
    }
    apiService.send({
      tp: 'req:rl:recommendation:use',
      recommendation_id: prompt.recommendation_id,
    });
    setPrompt(null);
  }, [apiService, prompt]);

  const ignoreRecommendation = useCallback(() => {
    if (!prompt?.recommendation_id) {
      return;
    }
    apiService.send({
      tp: 'req:rl:recommendation:ignore',
      recommendation_id: prompt.recommendation_id,
    });
    setPrompt(null);
  }, [apiService, prompt]);

  const rateShot = useCallback(
    rating => {
      if (!prompt?.shot_id) {
        return;
      }
      apiService.send({
        tp: 'req:rl:rating',
        shot_id: prompt.shot_id,
        recommendation_id: prompt.recommendation_id,
        rating,
      });
      setPrompt(null);
    },
    [apiService, prompt],
  );

  if (!prompt) {
    return null;
  }

  return (
    <div className='fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4'>
      <div
        className='bg-base-100 border-base-300 w-full max-w-md rounded-lg border p-5 shadow-xl'
        role='dialog'
        aria-modal='true'
      >
        {prompt.type === 'recommendation' && (
          <div className='space-y-4'>
            <div>
              <div className='text-base-content/60 text-xs font-semibold uppercase tracking-wide'>
                Next Shot
              </div>
              <h2 className='text-xl font-bold'>BO Recommendation</h2>
            </div>

            <div className='grid grid-cols-1 gap-2 sm:grid-cols-3'>
              <div className='bg-base-200 rounded-md p-3'>
                <div className='text-base-content/60 text-xs uppercase'>Grind</div>
                <div className='text-base font-semibold'>
                  {formatGrind(prompt.grind_delta_steps)}
                </div>
              </div>
              <div className='bg-base-200 rounded-md p-3'>
                <div className='text-base-content/60 text-xs uppercase'>Grind Dose</div>
                <div className='text-base font-semibold'>{formatDose(prompt.next_dose_g)}</div>
              </div>
              <div className='bg-base-200 rounded-md p-3'>
                <div className='text-base-content/60 text-xs uppercase'>Yield</div>
                <div className='text-base font-semibold'>{formatYield(prompt.target_yield_g)}</div>
              </div>
            </div>

            <div className='text-base-content/70 text-sm'>
              Ratio {formatRatio(prompt.target_ratio)}. Grind is manual; Use saves yield and only
              saves grind dose when grind-by-weight is enabled.
            </div>

            <div className='grid grid-cols-3 gap-2'>
              <button type='button' className='btn btn-primary' onClick={useRecommendation}>
                Use
              </button>
              <button type='button' className='btn btn-outline' onClick={close}>
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

        {prompt.type === 'rating' && (
          <div className='space-y-4'>
            <div>
              <div className='text-base-content/60 text-xs font-semibold uppercase tracking-wide'>
                Shot Complete
              </div>
              <h2 className='text-xl font-bold'>Taste?</h2>
            </div>

            <div className='grid grid-cols-5 gap-2'>
              {[1, 2, 3, 4, 5].map(rating => (
                <button
                  key={rating}
                  type='button'
                  className='btn btn-primary aspect-square h-auto min-h-12 text-lg'
                  onClick={() => rateShot(rating)}
                >
                  {rating}
                </button>
              ))}
            </div>

            <button type='button' className='btn btn-ghost w-full' onClick={close}>
              Skip
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
