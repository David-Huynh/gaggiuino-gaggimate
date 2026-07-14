import Card from '../../components/Card.jsx';
import { useCallback, useState, useContext, useEffect } from 'preact/hooks';
import { HistoryChart } from './HistoryChart.jsx';
import { downloadJson } from '../../utils/download.js';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { faFileExport } from '@fortawesome/free-solid-svg-icons/faFileExport';
import { faTrashCan } from '@fortawesome/free-solid-svg-icons/faTrashCan';
import { faWeightScale } from '@fortawesome/free-solid-svg-icons/faWeightScale';
import { faClock } from '@fortawesome/free-solid-svg-icons/faClock';
import { faUpload } from '@fortawesome/free-solid-svg-icons/faUpload';
import { faStar } from '@fortawesome/free-solid-svg-icons/faStar';
import { faPlus } from '@fortawesome/free-solid-svg-icons/faPlus';
import { faMinus } from '@fortawesome/free-solid-svg-icons/faMinus';
import { faMagnifyingGlassChart } from '@fortawesome/free-solid-svg-icons/faMagnifyingGlassChart';
import { faRotate } from '@fortawesome/free-solid-svg-icons/faRotate';
import ShotNotesCard from './ShotNotesCard.jsx';
import { useConfirmAction } from '../../hooks/useConfirmAction.js';

import VisualizerUploadModal from '../../components/VisualizerUploadModal.jsx';
import { visualizerService } from '../../services/VisualizerService.js';
import { ApiServiceContext } from '../../services/ApiService.js';
import { Tooltip } from '../../components/Tooltip.jsx';

function round2(v) {
  if (v == null || Number.isNaN(v)) return v;
  return Math.round((v + Number.EPSILON) * 100) / 100;
}

function shortId(value) {
  if (!value) return '';
  const text = String(value);
  return text.length > 18 ? `${text.slice(0, 10)}...${text.slice(-6)}` : text;
}

function formatNumber(value, digits = 1, suffix = '') {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? `${numeric.toFixed(digits)}${suffix}` : '-';
}

function displayValue(...values) {
  for (const value of values) {
    if (value !== null && value !== undefined && String(value).trim() !== '') {
      return String(value);
    }
  }
  return '-';
}

function formatGrind(summary) {
  const parts = [];
  if (Number.isFinite(Number(summary?.current_absolute_step))) {
    parts.push(`setting ${formatNumber(summary.current_absolute_step, 1)}`);
  }
  if (Number.isFinite(Number(summary?.relative_grind_steps_from_reference))) {
    parts.push(`${formatNumber(summary.relative_grind_steps_from_reference, 1)} steps rel.`);
  }
  if (Number.isFinite(Number(summary?.relative_grind_um_from_reference))) {
    parts.push(`${formatNumber(summary.relative_grind_um_from_reference, 0, 'um')} rel.`);
  }
  return parts.length ? parts.join(' / ') : 'Unknown';
}

function formatRecommendation(summary) {
  if (!summary?.recommendation_id) return 'Baseline / none';
  const status = summary.recommendation_followed || summary.status || 'unknown';
  return `${shortId(summary.recommendation_id)} (${status})`;
}

function formatObserved(summary) {
  const observed = summary?.action_observed || {};
  const flags = [
    observed.grind === false ? 'grind unknown' : null,
    observed.dose === false ? 'dose unknown' : null,
    observed.target_yield === false ? 'yield target unknown' : null,
  ].filter(Boolean);
  return flags.length ? flags.join(', ') : 'action recorded';
}

function ContextMetric({ label, value }) {
  return (
    <div className='min-w-0'>
      <div className='text-base-content/50 text-[11px] font-semibold tracking-wide uppercase'>
        {label}
      </div>
      <div className='text-base-content mt-0.5 min-w-0 truncate text-sm font-medium'>{value}</div>
    </div>
  );
}

function AutoTuningContext({ summary, shot, onReprocess, reprocessStatus }) {
  if (!summary?.shot_id) {
    return null;
  }

  const dose = Number.isFinite(Number(summary.dose_in_g))
    ? formatNumber(summary.dose_in_g, 1, 'g')
    : 'Unknown';
  const yieldText = Number.isFinite(Number(summary.beverage_out_g))
    ? `${formatNumber(summary.beverage_out_g, 1, 'g')} out`
    : Number.isFinite(Number(shot.volume))
      ? `${formatNumber(shot.volume, 1, 'g')} out`
      : 'Unknown';
  const targetYield = Number.isFinite(Number(summary.target_yield_g))
    ? `target ${formatNumber(summary.target_yield_g, 1, 'g')}`
    : null;
  const ratio = Number.isFinite(Number(summary.brew_ratio))
    ? `1:${formatNumber(summary.brew_ratio, 2)}`
    : Number.isFinite(Number(summary.target_ratio))
      ? `target 1:${formatNumber(summary.target_ratio, 2)}`
      : null;
  return (
    <section className='border-base-content/10 mt-4 border-t pt-4'>
      <div className='mb-3 flex flex-wrap items-center justify-between gap-2'>
        <div>
          <div className='text-base-content/60 text-xs font-semibold tracking-wide uppercase'>
            Auto Tuning Context
          </div>
          <div className='text-base-content/70 text-sm'>
            {shortId(summary.shot_id)}
          </div>
        </div>
        <div className='flex flex-wrap items-center gap-2'>
          <div className='text-base-content/60 text-xs'>{formatObserved(summary)}</div>
          {summary.reprocess_available && (
            <button
              type='button'
              className='btn btn-outline btn-xs'
              onClick={onReprocess}
              disabled={reprocessStatus === 'sending'}
            >
              <FontAwesomeIcon icon={faRotate} />
              {reprocessStatus === 'sending' ? 'Sending' : 'Reprocess'}
            </button>
          )}
        </div>
      </div>
      {reprocessStatus && reprocessStatus !== 'sending' && (
        <div className={`mb-3 text-xs ${reprocessStatus === 'sent' ? 'text-success' : 'text-error'}`}>
          {reprocessStatus === 'sent' ? 'Shot queued for reprocessing.' : 'Shot could not be reprocessed.'}
        </div>
      )}
      <div className='grid grid-cols-1 gap-x-5 gap-y-3 sm:grid-cols-2 lg:grid-cols-4'>
        <ContextMetric
          label='Bean'
          value={displayValue(summary.bean_context_name, shortId(summary.bean_context_id))}
        />
        <ContextMetric
          label='Grinder'
          value={displayValue(summary.grinder_context_name, shortId(summary.grinder_context_id))}
        />
        <ContextMetric
          label='Profile'
          value={displayValue(summary.profile_label, shot.profile, shortId(summary.profile_id))}
        />
        <ContextMetric label='Grind' value={formatGrind(summary)} />
        <ContextMetric label='Dose' value={dose} />
        <ContextMetric
          label='Yield'
          value={[yieldText, targetYield, ratio].filter(Boolean).join(' / ')}
        />
        <ContextMetric label='Recommendation' value={formatRecommendation(summary)} />
      </div>
    </section>
  );
}

export default function HistoryCard({ shot, onDelete, onLoad, onNotesChanged }) {
  const apiService = useContext(ApiServiceContext);
  const [shotNotes, setShotNotes] = useState(shot.notes || null);
  const [autoTuningSummary, setAutoTuningSummary] = useState(null);
  const [expanded, setExpanded] = useState(false);
  const { armed: confirmDelete, armOrRun: confirmOrDelete } = useConfirmAction(4000);
  const [showUploadModal, setShowUploadModal] = useState(false);
  const [isUploading, setIsUploading] = useState(false);
  const [reprocessStatus, setReprocessStatus] = useState(null);

  const date = new Date(shot.timestamp * 1000);

  const onExport = useCallback(() => {
    if (!shot.loaded) return; // Only export loaded data
    const exportData = { ...shot, notes: shotNotes };
    if (Array.isArray(exportData.samples)) {
      exportData.samples = exportData.samples.map(s => ({
        t: s.t,
        tt: round2(s.tt),
        ct: round2(s.ct),
        tp: round2(s.tp),
        cp: round2(s.cp),
        fl: round2(s.fl),
        tf: round2(s.tf),
        pf: round2(s.pf),
        vf: round2(s.vf),
        v: round2(s.v),
        ev: round2(s.ev),
        pr: round2(s.pr),
        systemInfo: s.systemInfo,
        phaseNumber: s.phaseNumber,
        phaseDisplayNumber: s.phaseDisplayNumber,
      }));
    }
    exportData.volume = round2(exportData.volume);
    // duration left as integer ms
    downloadJson(exportData, 'shot-' + shot.id + '.json');
  }, [shot, shotNotes]);

  const reprocessShot = useCallback(async () => {
    setReprocessStatus('sending');
    try {
      const response = await apiService.request({ tp: 'req:history:rl:reprocess', id: shot.id });
      setReprocessStatus(response?.error ? 'error' : 'sent');
    } catch {
      setReprocessStatus('error');
    }
  }, [apiService, shot.id]);

  const handleNotesLoaded = useCallback(notes => {
    setShotNotes(notes);
  }, []);

  const handleNotesUpdate = useCallback(
    notes => {
      setShotNotes(notes);
      // Notify parent that notes changed (so it can reload the index)
      if (onNotesChanged) onNotesChanged();
    },
    [onNotesChanged],
  );
  const profileTitle = shot.profile || 'Unknown Profile';
  let formattedDate = 'No timestamp available';
  if (date.getFullYear() > 1970) {
    formattedDate =
      date.toLocaleDateString() +
      ' ' +
      date.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
  }

  const handleUpload = useCallback(
    async (username, password, rememberCredentials) => {
      setIsUploading(true);
      try {
        // Validate shot data
        if (!visualizerService.validateShot(shot)) {
          throw new Error('Shot data is invalid or incomplete');
        }

        // Fetch profile data if profileId is available
        let profileData = null;
        if (shot.profileId && apiService) {
          try {
            const profileResponse = await apiService.request({
              tp: 'req:profiles:load',
              id: shot.profileId,
            });
            if (profileResponse.profile) {
              profileData = profileResponse.profile;
            }
          } catch (error) {
            console.warn('Failed to fetch profile data:', error);
            // Continue without profile data
          }
        }

        // Include notes in shot data
        const shotWithNotes = {
          ...shot,
          notes: shotNotes,
        };

        await visualizerService.uploadShot(shotWithNotes, username, password, profileData);

        // Show success message
        alert('Shot uploaded successfully to visualizer.coffee!');
      } catch (error) {
        console.error('Upload failed:', error);
        alert(`Upload failed: ${error.message}`);
        throw error; // Re-throw to prevent modal from closing
      } finally {
        setIsUploading(false);
      }
    },
    [shot, shotNotes, apiService],
  );

  const canUpload = visualizerService.validateShot(shot);

  useEffect(() => {
    setAutoTuningSummary(null);
  }, [shot.id]);

  return (
    <Card sm={12} className='[&>.card-body]:p-2'>
      <div className='flex flex-col gap-2'>
        <div className='flex flex-row items-start gap-2'>
          <button
            className='border-base-content/20 text-base-content/60 hover:text-base-content hover:bg-base-content/10 hover:border-base-content/40 cursor-pointer rounded-md border p-2 transition-all duration-200'
            onClick={() => {
              const next = !expanded;
              setExpanded(next);
              if (next && !shot.loaded && onLoad) onLoad(shot.id);
            }}
            aria-label={expanded ? 'Collapse shot details' : 'Expand shot details'}
          >
            <FontAwesomeIcon icon={expanded ? faMinus : faPlus} className='h-3 w-3' />
          </button>

          <div className='min-w-0 flex-grow'>
            {/* Header Row */}
            <div className='mb-1 flex flex-row items-start justify-between gap-3'>
              <div className='min-w-0 flex-grow'>
                <h3 className='text-base-content truncate text-base font-semibold'>
                  {profileTitle}
                </h3>
                <p className='text-base-content/70 text-sm'>
                  #{shot.id} • {formattedDate}
                </p>
                {expanded &&
                  shot.loaded &&
                  shot.samples &&
                  shot.samples.length > 0 &&
                  shot.samples[0].systemInfo && (
                    <p className='text-base-content/60 text-xs italic'>
                      Brewed by{' '}
                      {shot.samples[0].systemInfo.shotStartedVolumetric ? 'Weight' : 'Time'}
                    </p>
                  )}
              </div>

              <div className='flex shrink-0 flex-row items-center gap-2'>
                {shot.incomplete && (
                  <span className='inline-flex items-center rounded-full bg-yellow-100 px-2 py-1 text-xs font-medium text-yellow-800'>
                    INCOMPLETE
                  </span>
                )}

                <div className='flex flex-row gap-1'>
                  <Tooltip content={shot.loaded ? 'Export' : 'Load first'}>
                    <button
                      disabled={!shot.loaded}
                      onClick={onExport}
                      className='text-base-content/50 hover:text-info hover:bg-info/10 cursor-pointer rounded-md p-2 transition-colors disabled:cursor-not-allowed disabled:opacity-40'
                      aria-label='Export shot data'
                    >
                      <FontAwesomeIcon icon={faFileExport} className='h-4 w-4' />
                    </button>
                  </Tooltip>

                  {/* Analyzer Button */}
                  <Tooltip content='Open in Analyzer'>
                    <a
                      href={`/analyzer/internal/${shot.id}`}
                      className='text-base-content/50 hover:text-primary hover:bg-primary/10 flex items-center justify-center rounded-md p-2 transition-colors'
                      aria-label='Open in Analyzer'
                    >
                      <FontAwesomeIcon icon={faMagnifyingGlassChart} className='h-4 w-4' />
                    </a>
                  </Tooltip>

                  <Tooltip
                    content={
                      canUpload
                        ? 'Upload to Visualizer.coffee'
                        : 'Load shot data first by expanding the shot'
                    }
                  >
                    <button
                      onClick={() => setShowUploadModal(true)}
                      disabled={!canUpload}
                      className={`group inline-block cursor-pointer items-center justify-between gap-2 rounded-md border border-transparent px-2.5 py-2 text-sm font-semibold ${
                        canUpload
                          ? 'text-success hover:bg-success/10 active:border-success/20'
                          : 'cursor-not-allowed text-gray-400'
                      }`}
                      aria-label='Upload to visualizer.coffee'
                    >
                      <FontAwesomeIcon icon={faUpload} />
                    </button>
                  </Tooltip>
                  <Tooltip content={confirmDelete ? 'Click to confirm delete' : 'Delete'}>
                    <button
                      onClick={() => {
                        confirmOrDelete(() => onDelete(shot.id));
                      }}
                      className={`cursor-pointer rounded-md p-2 transition-colors ${confirmDelete ? 'bg-error text-error-content font-semibold' : 'text-base-content/50 hover:text-error hover:bg-error/10'}`}
                      aria-label={confirmDelete ? 'Confirm deletion of shot' : 'Delete shot'}
                    >
                      <FontAwesomeIcon icon={faTrashCan} className='h-4 w-4' />
                      {confirmDelete && <span className='ml-2 hidden sm:inline'>Confirm</span>}
                    </button>
                  </Tooltip>
                </div>
              </div>
            </div>

            {/* Stats Row */}
            <div className='text-base-content/80 mb-1 flex flex-row items-center gap-4 text-sm'>
              <div className='flex items-center gap-1'>
                <FontAwesomeIcon icon={faClock} className='h-4 w-4' />
                <span>{(shot.duration / 1000).toFixed(1)}s</span>
              </div>

              {shot.volume && shot.volume > 0 && (
                <div className='flex items-center gap-1'>
                  <FontAwesomeIcon icon={faWeightScale} className='h-4 w-4' />
                  <span>{round2(shot.volume)}g</span>
                </div>
              )}

              {shot.rating && shot.rating > 0 ? (
                <div className='flex items-center gap-1'>
                  <FontAwesomeIcon icon={faStar} className='h-4 w-4 text-yellow-500' />
                  <span className='font-medium'>{shot.rating}/5</span>
                </div>
              ) : (
                <div className='text-base-content/50 flex items-center gap-1'>
                  <FontAwesomeIcon icon={faStar} className='h-4 w-4' />
                  <span>Not rated</span>
                </div>
              )}
            </div>

            {expanded && (
              <div className='border-base-content/20 mt-4 border-t pt-4'>
                {!shot.loaded && (
                  <div className='flex items-center justify-center py-8'>
                    <span className='text-base-content/70 text-sm'>Loading shot data...</span>
                  </div>
                )}
                {shot.loaded && <HistoryChart shot={shot} />}
                {shot.loaded && (
                  <AutoTuningContext
                    summary={autoTuningSummary}
                    shot={shot}
                    onReprocess={reprocessShot}
                    reprocessStatus={reprocessStatus}
                  />
                )}
                {shot.loaded && (
                  <ShotNotesCard
                    shot={shot}
                    onNotesLoaded={handleNotesLoaded}
                    onNotesUpdate={handleNotesUpdate}
                    onAutoTuningLoaded={setAutoTuningSummary}
                  />
                )}
              </div>
            )}
          </div>
        </div>
      </div>

      <VisualizerUploadModal
        isOpen={showUploadModal}
        onClose={() => setShowUploadModal(false)}
        onUpload={handleUpload}
        isUploading={isUploading}
        shotInfo={{
          profile: shot.profile,
          timestamp: shot.timestamp,
          duration: shot.duration,
          volume: shot.volume,
        }}
      />
    </Card>
  );
}
