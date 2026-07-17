import { useState, useEffect, useContext, useCallback } from 'preact/hooks';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { ApiServiceContext } from '../../services/ApiService.js';
import { Spinner } from '../../components/Spinner.jsx';
import { faEdit } from '@fortawesome/free-solid-svg-icons/faEdit';
import { faSave } from '@fortawesome/free-solid-svg-icons/faSave';
import {
  deriveLegacyBalanceTaste,
  formatTasteTags,
  normalizeNotesTasteFields,
  TASTE_TAG_GROUPS,
} from '../../utils/tasteTags.js';

const optionalNumber = value => {
  if (value === '' || value === null || value === undefined) return null;
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
};

const hydrateShotParameters = (notes, summary) => {
  const hydrated = { ...notes };
  if (!summary) return hydrated;

  if (!hydrated.doseIn) {
    hydrated.doseIn = summary.dose_in_g ?? summary.dose_target_g ?? '';
  }
  if (!hydrated.doseOut) {
    hydrated.doseOut = summary.beverage_out_g ?? '';
  }
  if (!hydrated.targetYield) {
    hydrated.targetYield = summary.target_yield_g ?? '';
  }
  if (optionalNumber(summary.current_absolute_step) !== null) {
    hydrated.grindSettingMode = 'absolute';
    hydrated.grindSetting = summary.current_absolute_step;
  } else if (optionalNumber(summary.relative_grind_steps_from_reference) !== null) {
    hydrated.grindSettingMode = 'relative';
    hydrated.grindSetting = summary.relative_grind_steps_from_reference;
  }
  return hydrated;
};

const shotParameterBaseline = (notes, summary) => ({
  doseIn: optionalNumber(summary?.dose_in_g ?? summary?.dose_target_g ?? notes.doseIn),
  doseOut: optionalNumber(summary?.beverage_out_g ?? notes.doseOut),
  targetYield: optionalNumber(summary?.target_yield_g ?? notes.targetYield),
  grindSetting: optionalNumber(
    notes.grindSettingMode === 'absolute'
      ? (summary?.current_absolute_step ?? notes.grindSetting)
      : (summary?.relative_grind_steps_from_reference ?? notes.grindSetting),
  ),
  grindSettingMode: notes.grindSettingMode || 'relative',
});

export default function ShotNotesCard({ shot, onNotesUpdate, onNotesLoaded, onAutoTuningLoaded }) {
  const apiService = useContext(ApiServiceContext);

  const [notes, setNotes] = useState({
    id: shot.id,
    rating: 0,
    beanType: '',
    doseIn: '',
    doseOut: '',
    targetYield: '',
    ratio: '',
    grindSetting: '',
    grindSettingMode: 'relative',
    balanceTaste: '',
    tasteTags: [],
    notes: '',
  });

  const [loading, setLoading] = useState(false);
  const [isEditing, setIsEditing] = useState(false);
  const [initialLoaded, setInitialLoaded] = useState(false);
  const [autoTuningSummary, setAutoTuningSummary] = useState(null);
  const [parameterBaseline, setParameterBaseline] = useState(null);
  const [saveError, setSaveError] = useState('');

  // Calculate ratio function
  const calculateRatio = useCallback((doseIn, doseOut) => {
    if (doseIn && doseOut && parseFloat(doseIn) > 0 && parseFloat(doseOut) > 0) {
      return (parseFloat(doseOut) / parseFloat(doseIn)).toFixed(2);
    }
    return '';
  }, []);

  // Load notes ONLY on component mount
  useEffect(() => {
    if (initialLoaded) return; // Prevent reloading

    const loadNotes = async () => {
      try {
        const response = await apiService.request({
          tp: 'req:history:notes:get',
          id: shot.id,
        });

        let loadedNotes = {
          id: shot.id,
          rating: 0,
          beanType: '',
          doseIn: '',
          doseOut: '',
          targetYield: '',
          ratio: '',
          grindSetting: '',
          grindSettingMode: 'relative',
          balanceTaste: '',
          tasteTags: [],
          notes: '',
        };

        if (response.notes && Object.keys(response.notes).length > 0) {
          // Parse response.notes if it's a string
          let parsedNotes = response.notes;
          if (typeof response.notes === 'string') {
            try {
              parsedNotes = JSON.parse(response.notes);
            } catch (e) {
              console.warn('Failed to parse notes JSON:', e);
              parsedNotes = {};
            }
          }

          // Merge loaded notes with defaults
          loadedNotes = { ...loadedNotes, ...parsedNotes };
        }

        const autoTuning = response.auto_tuning || null;
        loadedNotes = hydrateShotParameters(loadedNotes, autoTuning);

        // Pre-populate doseOut with shot.volume if it's empty and shot.volume exists
        if (!loadedNotes.doseOut && shot.volume) {
          loadedNotes.doseOut = shot.volume.toFixed(1);
        }

        // Calculate ratio from loaded data
        if (loadedNotes.doseIn && loadedNotes.doseOut) {
          loadedNotes.ratio = calculateRatio(loadedNotes.doseIn, loadedNotes.doseOut);
        }

        const normalizedNotes = normalizeNotesTasteFields(loadedNotes);
        setNotes(normalizedNotes);
        setAutoTuningSummary(autoTuning);
        setParameterBaseline(shotParameterBaseline(normalizedNotes, autoTuning));
        setInitialLoaded(true);
        // Pass loaded notes to parent
        if (onNotesLoaded) {
          onNotesLoaded(normalizedNotes);
        }
        if (onAutoTuningLoaded) {
          onAutoTuningLoaded(response.auto_tuning || null);
        }
      } catch (error) {
        console.error('Failed to load notes:', error);

        // Even if loading fails, set up defaults
        const defaultNotes = {
          id: shot.id,
          rating: 0,
          beanType: '',
          doseIn: '',
          doseOut: shot.volume ? shot.volume.toFixed(1) : '',
          targetYield: '',
          ratio: '',
          grindSetting: '',
          grindSettingMode: 'relative',
          balanceTaste: '',
          tasteTags: [],
          notes: '',
        };

        setNotes(defaultNotes);
        setAutoTuningSummary(null);
        setParameterBaseline(shotParameterBaseline(defaultNotes, null));
        setInitialLoaded(true);
        if (onNotesLoaded) {
          onNotesLoaded(defaultNotes);
        }
        if (onAutoTuningLoaded) {
          onAutoTuningLoaded(null);
        }
      }
    };

    loadNotes();
  }, []); // No dependencies - only run once

  // Reset if shot changes
  useEffect(() => {
    if (notes.id !== shot.id) {
      setInitialLoaded(false);
      setIsEditing(false);
    }
  }, [shot.id, notes.id]);

  const saveNotes = async () => {
    setLoading(true);
    setSaveError('');
    const notesToSave = normalizeNotesTasteFields(notes);
    try {
      let shotCorrection;
      if (notesToSave.rlShotId && parameterBaseline) {
        const correction = {};
        const addChangedNumber = (field, wireKey) => {
          const rawValue = notesToSave[field];
          if (rawValue === '' || rawValue === null || rawValue === undefined) return;
          const parsed = Number(rawValue);
          if (!Number.isFinite(parsed)) {
            throw new Error(`${field} must be numeric`);
          }
          if (parameterBaseline[field] === null || Math.abs(parsed - parameterBaseline[field]) > 0.0001) {
            correction[wireKey] = parsed;
          }
        };
        addChangedNumber('doseIn', 'dose_in_g');
        addChangedNumber('doseOut', 'beverage_out_g');
        addChangedNumber('targetYield', 'target_yield_g');
        addChangedNumber(
          'grindSetting',
          notesToSave.grindSettingMode === 'absolute'
            ? 'current_absolute_step'
            : 'relative_grind_steps_from_reference',
        );
        if (Object.keys(correction).length > 0) {
          shotCorrection = correction;
        }
      }
      const response = await apiService.request({
        tp: 'req:history:notes:save',
        id: shot.id,
        notes: notesToSave,
        ...(shotCorrection ? { shot_correction: shotCorrection } : {}),
      });
      if (response.error) {
        throw new Error(response.error);
      }
      setNotes(notesToSave);
      const updatedSummary = response.auto_tuning || autoTuningSummary;
      setAutoTuningSummary(updatedSummary);
      setParameterBaseline(shotParameterBaseline(notesToSave, updatedSummary));
      setIsEditing(false);
      if (onNotesUpdate) {
        onNotesUpdate(notesToSave);
      }
      if (onAutoTuningLoaded) {
        onAutoTuningLoaded(updatedSummary || null);
      }
    } catch (error) {
      console.error('Failed to save notes:', error);
      setSaveError(error.message || 'Unable to save shot notes');
    } finally {
      setLoading(false);
    }
  };

  const handleInputChange = (field, value) => {
    setNotes(prev => {
      const newNotes = { ...prev, [field]: value };

      // Only recalculate ratio if we're changing doseIn or doseOut
      if ((field === 'doseIn' || field === 'doseOut') && initialLoaded) {
        const doseIn = field === 'doseIn' ? value : prev.doseIn;
        const doseOut = field === 'doseOut' ? value : prev.doseOut;
        newNotes.ratio = calculateRatio(doseIn, doseOut);
      }

      return newNotes;
    });
  };

  const toggleTasteTag = tag => {
    setNotes(prev => {
      const selected = new Set(prev.tasteTags || []);
      if (selected.has(tag)) {
        selected.delete(tag);
      } else {
        selected.add(tag);
      }
      const tasteTags = [...selected];
      return {
        ...prev,
        tasteTags,
        balanceTaste: deriveLegacyBalanceTaste(tasteTags),
      };
    });
  };

  const renderStars = (rating, editable = false) => {
    const stars = [];
    for (let i = 1; i <= 5; i++) {
      stars.push(
        <button
          key={i}
          type='button'
          disabled={!editable}
          onClick={() => editable && handleInputChange('rating', i)}
          className={`text-lg ${i <= rating ? 'text-yellow-400' : 'text-gray-300'} ${
            editable ? 'cursor-pointer hover:text-yellow-300' : 'cursor-default'
          }`}
        >
          ★
        </button>,
      );
    }
    return stars;
  };

  // Don't render until initial load is complete
  if (!initialLoaded) {
    return (
      <div className='mt-6 border-t pt-6'>
        <div className='flex items-center justify-center py-8'>
          <span className='loading loading-spinner loading-md'></span>
        </div>
      </div>
    );
  }

  return (
    <div className='border-t-base-content/10 accent mt-6 border-t-2 pt-6'>
      <div className='mb-4 flex items-center justify-between'>
        <h3 className='text-lg font-semibold'>Shot Notes</h3>
        {!isEditing ? (
          <button
            onClick={() => {
              setSaveError('');
              setIsEditing(true);
            }}
            className='btn btn-sm btn-outline'
          >
            <FontAwesomeIcon icon={faEdit} />
            Edit
          </button>
        ) : (
          <div className='flex gap-2'>
            <button
              onClick={() => {
                setSaveError('');
                setIsEditing(false);
              }}
              className='btn btn-sm btn-ghost'
              disabled={loading}
            >
              Cancel
            </button>
            <button onClick={saveNotes} className='btn btn-sm btn-primary' disabled={loading}>
              {loading ? (
                <Spinner size={4} />
              ) : (
                <>
                  <FontAwesomeIcon icon={faSave} />
                  Save
                </>
              )}
            </button>
          </div>
        )}
      </div>

      {saveError && <div className='alert alert-error mb-4 text-sm'>{saveError}</div>}

      <div className='grid grid-cols-1 gap-6 md:grid-cols-2 lg:grid-cols-4'>
        {/* Rating */}
        <div className='form-control'>
          <label className='mb-2 block text-sm font-medium'>Rating</label>
          <div className='flex gap-1'>{renderStars(notes.rating, isEditing)}</div>
        </div>

        {/* Bean Type */}
        <div className='form-control'>
          <label className='mb-2 block text-sm font-medium'>Bean Type</label>
          {isEditing ? (
            <input
              type='text'
              className='input input-bordered w-full'
              value={notes.beanType}
              onChange={e => handleInputChange('beanType', e.target.value)}
              placeholder='e.g., Single Origin, Blend'
            />
          ) : (
            <div className='input input-bordered bg-base-200 w-full cursor-default'>
              {notes.beanType || '—'}
            </div>
          )}
        </div>

        {/* Dose In */}
        <div className='form-control'>
          <label className='mb-2 block text-sm font-medium'>Dose In (g)</label>
          {isEditing ? (
            <input
              type='number'
              step='0.1'
              className='input input-bordered w-full'
              value={notes.doseIn}
              onChange={e => handleInputChange('doseIn', e.target.value)}
              placeholder='18.0'
            />
          ) : (
            <div className='input input-bordered bg-base-200 w-full cursor-default'>
              {notes.doseIn || '—'}
            </div>
          )}
        </div>

        {/* Dose Out */}
        <div className='form-control'>
          <label className='mb-2 block text-sm font-medium'>Dose Out (g)</label>
          {isEditing ? (
            <input
              type='number'
              step='0.1'
              className='input input-bordered w-full'
              value={notes.doseOut}
              onChange={e => handleInputChange('doseOut', e.target.value)}
              placeholder='36.0'
            />
          ) : (
            <div className='input input-bordered bg-base-200 w-full cursor-default'>
              {notes.doseOut || '—'}
            </div>
          )}
        </div>

        {/* Target Yield */}
        <div className='form-control'>
          <label className='mb-2 block text-sm font-medium'>Target Yield (g)</label>
          {isEditing ? (
            <input
              type='number'
              step='0.1'
              className='input input-bordered w-full'
              value={notes.targetYield}
              onChange={e => handleInputChange('targetYield', e.target.value)}
              placeholder='36.0'
            />
          ) : (
            <div className='input input-bordered bg-base-200 w-full cursor-default'>
              {notes.targetYield || 'â€”'}
            </div>
          )}
        </div>

        {/* Ratio */}
        <div className='form-control'>
          <label className='mb-2 block text-sm font-medium'>Ratio (1:{notes.ratio || '—'})</label>
          <div className='input input-bordered bg-base-200 w-full cursor-default'>
            {notes.ratio ? `1:${notes.ratio}` : '—'}
          </div>
        </div>

        {/* Grind Setting */}
        <div className='form-control'>
          <label className='mb-2 block text-sm font-medium'>
            {notes.rlShotId
              ? `Grind Setting (${notes.grindSettingMode === 'absolute' ? 'absolute steps' : 'steps from reference'})`
              : 'Grind Setting'}
          </label>
          {isEditing ? (
            <input
              type={notes.rlShotId ? 'number' : 'text'}
              step={notes.rlShotId ? '0.1' : undefined}
              className='input input-bordered w-full'
              value={notes.grindSetting}
              onChange={e => handleInputChange('grindSetting', e.target.value)}
              placeholder={notes.rlShotId ? '2.5' : 'e.g., 2.5, Medium-Fine'}
            />
          ) : (
            <div className='input input-bordered bg-base-200 w-full cursor-default'>
              {notes.grindSetting || '—'}
            </div>
          )}
        </div>

        {/* Taste Tags */}
        <div className='form-control md:col-span-2'>
          <label className='mb-2 block text-sm font-medium'>Taste Tags</label>
          {isEditing ? (
            <div className='space-y-3'>
              {TASTE_TAG_GROUPS.map(group => (
                <section key={group.key} className='space-y-2'>
                  <div className='text-base-content/60 text-xs font-semibold tracking-wide uppercase'>
                    {group.label}
                  </div>
                  <div className='flex flex-wrap gap-2'>
                    {group.tags.map(tag => {
                      const selected = notes.tasteTags?.includes(tag.value);
                      return (
                        <button
                          key={tag.value}
                          type='button'
                          className={`btn btn-xs h-auto min-h-7 whitespace-normal ${
                            selected ? 'btn-primary' : 'btn-outline'
                          }`}
                          onClick={() => toggleTasteTag(tag.value)}
                        >
                          {tag.label}
                        </button>
                      );
                    })}
                  </div>
                </section>
              ))}
            </div>
          ) : (
            <div className='input input-bordered bg-base-200 h-auto min-h-12 w-full cursor-default whitespace-normal'>
              {formatTasteTags(notes.tasteTags, '-')}
            </div>
          )}
        </div>
      </div>

      {/* Notes Text Area - Full Width */}
      <div className='form-control mt-6'>
        <label className='mb-2 block text-sm font-medium'>
          Notes{' '}
          {isEditing && <span className='text-xs text-gray-500'>({notes.notes.length}/200)</span>}
        </label>
        {isEditing ? (
          <textarea
            className='textarea textarea-bordered w-full'
            rows='4'
            value={notes.notes}
            maxLength={200}
            onChange={e => handleInputChange('notes', e.target.value)}
            placeholder='Tasting notes, brewing observations, etc...'
          />
        ) : (
          <div className='textarea textarea-bordered bg-base-200 min-h-[6rem] w-full cursor-default'>
            {notes.notes || 'No notes added'}
          </div>
        )}
      </div>
    </div>
  );
}
