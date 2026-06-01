import { useCallback, useContext, useEffect, useRef, useState } from 'preact/hooks';
import { ApiServiceContext } from '../services/ApiService.js';

const RATING_DISMISS_MS = 45000;
const RECOMMENDATION_STATUSES = new Set(['', 'pending', 'shown']);
// Ids we've already auto-popped this browser session. Persisted so a page reload
// (which replays the pending prompts from the firmware) restores them as the
// minimized "reopen" pills instead of popping the modal in your face again.
const SEEN_STORAGE_KEY = 'rl_seen_prompt_ids';
const TASTE_TAGS = [
  { value: 'sour', label: 'Sour' },
  { value: 'bitter', label: 'Bitter' },
  { value: 'weak', label: 'Weak' },
  { value: 'harsh', label: 'Harsh' },
  { value: 'thin', label: 'Thin' },
  { value: 'astringent', label: 'Astringent' },
  { value: 'channeling_suspected', label: 'Channeling' },
  { value: 'balanced', label: 'Balanced' },
];

function formatDose(value) {
  const dose = Number(value);
  return Number.isFinite(dose) && dose > 0 ? `${dose.toFixed(1)}g` : '-';
}

function formatYield(value) {
  const yieldG = Number(value);
  return Number.isFinite(yieldG) && yieldG > 0 ? `${yieldG.toFixed(1)}g` : '-';
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

function loadSeen() {
  try {
    return new Set(JSON.parse(window.sessionStorage.getItem(SEEN_STORAGE_KEY) || '[]'));
  } catch {
    return new Set();
  }
}

export function RLPromptOverlay() {
  const apiService = useContext(ApiServiceContext);
  // Pending items are the source of the reopen pills; they persist until the user
  // acts (rate/skip, use/ignore) or the firmware clears them. `view` is whichever
  // modal is currently open (null = minimized to pills).
  const [pendingRating, setPendingRating] = useState(null); // { shot_id, recommendation_id }
  const [pendingRec, setPendingRec] = useState(null); // { recommendation_id, ...fields }
  const [view, setView] = useState(null); // { kind: 'rating'|'taste_tags'|'recommendation', ... }
  const seenRef = useRef(loadSeen());

  const markSeen = useCallback(id => {
    if (!id || seenRef.current.has(id)) {
      return;
    }
    seenRef.current.add(id);
    try {
      window.sessionStorage.setItem(SEEN_STORAGE_KEY, JSON.stringify([...seenRef.current]));
    } catch {
      /* sessionStorage unavailable — dedupe degrades to in-memory only */
    }
  }, []);

  useEffect(() => {
    if (!apiService) {
      return undefined;
    }

    const recommendationListener = apiService.on('evt:rl:recommendation', message => {
      const status = message.status || '';
      if (!message.recommendation_id || !RECOMMENDATION_STATUSES.has(status)) {
        // Resolved or no longer promptable — drop the matching pending prompt.
        setPendingRec(current =>
          current && current.recommendation_id === message.recommendation_id ? null : current,
        );
        return;
      }
      setPendingRec({ type: 'recommendation', ...message });
      const firstSee = !seenRef.current.has(message.recommendation_id);
      markSeen(message.recommendation_id);
      // Auto-pop only when nothing else is open and we haven't shown it before.
      if (firstSee) {
        setView(current => (current ? current : { kind: 'recommendation' }));
      }
    });

    const shotCompleteListener = apiService.on('evt:rl:shot-complete', message => {
      if (!message.shot_id) {
        return;
      }
      setPendingRating({ shot_id: message.shot_id, recommendation_id: message.recommendation_id });
      const firstSee = !seenRef.current.has(message.shot_id);
      markSeen(message.shot_id);
      // `nudge` (post-flush reminder) re-pops even a shot we already showed.
      if (message.nudge || firstSee) {
        setView({
          kind: 'rating',
          shot_id: message.shot_id,
          recommendation_id: message.recommendation_id,
        });
      }
    });

    const brewStartListener = apiService.on('evt:status', message => {
      if (!message.process?.a) {
        return;
      }
      // A shot/flush started — get the modal out of the way but keep the pending
      // pills so feedback is never lost.
      setView(null);
    });

    return () => {
      apiService.off('evt:rl:recommendation', recommendationListener);
      apiService.off('evt:rl:shot-complete', shotCompleteListener);
      apiService.off('evt:status', brewStartListener);
    };
  }, [apiService, markSeen]);

  // Timeout folds the rating flow back into a pill instead of discarding it.
  useEffect(() => {
    if (view?.kind !== 'rating' && view?.kind !== 'taste_tags') {
      return undefined;
    }
    const timeout = window.setTimeout(() => setView(null), RATING_DISMISS_MS);
    return () => window.clearTimeout(timeout);
  }, [view]);

  const minimize = useCallback(() => setView(null), []);

  const chooseRating = useCallback(
    rating => {
      if (!view?.shot_id) {
        return;
      }
      setView({
        kind: 'taste_tags',
        shot_id: view.shot_id,
        recommendation_id: view.recommendation_id,
        rating,
        taste_tags: [],
      });
    },
    [view],
  );

  const toggleTasteTag = useCallback(
    tag => {
      setView(current => {
        if (current?.kind !== 'taste_tags') {
          return current;
        }
        const currentTags = new Set(current.taste_tags || []);
        if (currentTags.has(tag)) {
          currentTags.delete(tag);
        } else {
          currentTags.add(tag);
        }
        return { ...current, taste_tags: Array.from(currentTags) };
      });
    },
    [],
  );

  const submitFeedback = useCallback(
    ({ skipped = false, clearTags = false } = {}) => {
      const shotId = view?.shot_id || pendingRating?.shot_id;
      if (!shotId) {
        return;
      }
      const message = {
        tp: 'req:rl:rating',
        shot_id: shotId,
        recommendation_id: view?.recommendation_id || pendingRating?.recommendation_id,
        skipped,
      };
      if (!skipped && Number.isFinite(Number(view?.rating))) {
        message.rating = Number(view.rating);
        message.taste_tags = clearTags ? [] : view.taste_tags || [];
      }
      apiService.send(message);
      setPendingRating(null);
      setView(null);
    },
    [apiService, view, pendingRating],
  );

  const useRecommendation = useCallback(() => {
    const recId = pendingRec?.recommendation_id;
    if (!recId) {
      return;
    }
    apiService.send({ tp: 'req:rl:recommendation:use', recommendation_id: recId });
    setPendingRec(null);
    setView(null);
  }, [apiService, pendingRec]);

  const ignoreRecommendation = useCallback(() => {
    const recId = pendingRec?.recommendation_id;
    if (!recId) {
      return;
    }
    apiService.send({ tp: 'req:rl:recommendation:ignore', recommendation_id: recId });
    setPendingRec(null);
    setView(null);
  }, [apiService, pendingRec]);

  // Minimized: render the reopen pills (if anything is pending).
  if (!view) {
    if (!pendingRating && !pendingRec) {
      return null;
    }
    return (
      <div className='fixed bottom-4 right-4 z-50 flex flex-col items-end gap-2'>
        {pendingRating && (
          <button
            type='button'
            className='btn btn-primary btn-sm shadow-lg'
            onClick={() =>
              setView({
                kind: 'rating',
                shot_id: pendingRating.shot_id,
                recommendation_id: pendingRating.recommendation_id,
              })
            }
          >
            ★ Rate last shot
          </button>
        )}
        {pendingRec && (
          <button
            type='button'
            className='btn btn-secondary btn-sm shadow-lg'
            onClick={() => setView({ kind: 'recommendation' })}
          >
            ◎ Recommendation
          </button>
        )}
      </div>
    );
  }

  return (
    <div className='fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4'>
      <div
        className='bg-base-100 border-base-300 relative w-full max-w-md rounded-lg border p-5 shadow-xl'
        role='dialog'
        aria-modal='true'
      >
        <button
          type='button'
          className='btn btn-ghost btn-xs absolute right-2 top-2'
          onClick={minimize}
          aria-label='Minimize'
          title='Minimize'
        >
          Minimize
        </button>

        {view.kind === 'recommendation' && pendingRec && (
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
                  {formatGrind(pendingRec.grind_delta_steps)}
                </div>
              </div>
              <div className='bg-base-200 rounded-md p-3'>
                <div className='text-base-content/60 text-xs uppercase'>Grind Dose</div>
                <div className='text-base font-semibold'>{formatDose(pendingRec.next_dose_g)}</div>
              </div>
              <div className='bg-base-200 rounded-md p-3'>
                <div className='text-base-content/60 text-xs uppercase'>Yield</div>
                <div className='text-base font-semibold'>
                  {formatYield(pendingRec.target_yield_g)}
                </div>
              </div>
            </div>

            <div className='text-base-content/70 text-sm'>
              Grind is manual. Use saves yield and only saves grind dose when grind-by-weight is
              enabled.
            </div>

            <div className='grid grid-cols-3 gap-2'>
              <button type='button' className='btn btn-primary' onClick={useRecommendation}>
                Use
              </button>
              <button type='button' className='btn btn-outline' onClick={minimize}>
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

        {view.kind === 'rating' && (
          <div className='space-y-4'>
            <div>
              <div className='text-base-content/60 text-xs font-semibold uppercase tracking-wide'>
                Shot Complete
              </div>
              <h2 className='text-xl font-bold'>Taste?</h2>
              <p className='text-base-content/60 text-sm'>
                You can rate this shot later from shot history.
              </p>
            </div>

            <div className='grid grid-cols-5 gap-2'>
              {[1, 2, 3, 4, 5].map(rating => (
                <button
                  key={rating}
                  type='button'
                  className='btn btn-primary aspect-square h-auto min-h-12 text-lg'
                  onClick={() => chooseRating(rating)}
                >
                  {rating}
                </button>
              ))}
            </div>

            <button
              type='button'
              className='btn btn-ghost w-full'
              onClick={() => submitFeedback({ skipped: true })}
            >
              Skip
            </button>
          </div>
        )}

        {view.kind === 'taste_tags' && (
          <div className='space-y-4'>
            <div>
              <div className='text-base-content/60 text-xs font-semibold uppercase tracking-wide'>
                Optional
              </div>
              <h2 className='text-xl font-bold'>What was off?</h2>
              <p className='text-base-content/60 text-sm'>
                You can rate this shot later from shot history.
              </p>
            </div>

            <div className='grid grid-cols-2 gap-2'>
              {TASTE_TAGS.map(tag => {
                const selected = view.taste_tags?.includes(tag.value);
                return (
                  <button
                    key={tag.value}
                    type='button'
                    className={`btn btn-sm ${selected ? 'btn-primary' : 'btn-outline'}`}
                    onClick={() => toggleTasteTag(tag.value)}
                  >
                    {tag.label}
                  </button>
                );
              })}
            </div>

            <div className='grid grid-cols-2 gap-2'>
              <button type='button' className='btn btn-primary' onClick={() => submitFeedback()}>
                Done
              </button>
              <button
                type='button'
                className='btn btn-ghost'
                onClick={() => submitFeedback({ clearTags: true })}
              >
                No Tags
              </button>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
