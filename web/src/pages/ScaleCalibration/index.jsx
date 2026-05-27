import { useState, useCallback, useContext, useEffect, useRef } from 'preact/hooks';
import { useQuery } from 'preact-fetching';
import Card from '../../components/Card.jsx';
import { ApiServiceContext, machine } from '../../services/ApiService.js';

// Health bit constants — must mirror lib/NanoPbComm/src/GaggiMateComm.h.
const H = {
  NOT_CALIBRATED: 1 << 0,
  SAT_CH1: 1 << 1,
  SAT_CH2: 1 << 2,
  STALE: 1 << 3,
  TARE_FAILED: 1 << 4,
  TARE_NOISY: 1 << 5,
  TARING: 1 << 6,
  CALIBRATING: 1 << 7,
};

const CAL_ERROR_LABELS = {
  0: 'OK',
  1: 'Reference weight out of range',
  2: 'Too noisy — keep the scale still and retry',
  3: 'Computed factor implausible — did you forget to place the weight?',
  4: 'Hardware scale not present',
  5: 'Calibration timed out',
  6: 'Scale busy with another operation',
};

const RECAL_DAYS = 30;

function formatNumber(value, digits = 2, fallback = '—') {
  if (!Number.isFinite(value)) return fallback;
  return value.toFixed(digits);
}

function relativeTime(unixSeconds) {
  if (!unixSeconds || unixSeconds <= 0) return 'never';
  const elapsed = Date.now() / 1000 - unixSeconds;
  if (elapsed < 60) return 'just now';
  if (elapsed < 3600) return `${Math.floor(elapsed / 60)} min ago`;
  if (elapsed < 86400) return `${Math.floor(elapsed / 3600)} h ago`;
  return `${Math.floor(elapsed / 86400)} days ago`;
}

function HealthBadge({ healthBits, stddevG, calibrated, lastCalAgoDays }) {
  let color = 'bg-green-500';
  let label = 'OK';
  if (
    healthBits & (H.NOT_CALIBRATED | H.STALE | H.TARE_FAILED | H.SAT_CH1 | H.SAT_CH2)
  ) {
    color = 'bg-red-500';
    if (healthBits & H.NOT_CALIBRATED) label = 'NOT CALIBRATED';
    else if (healthBits & H.STALE) label = 'STALE';
    else if (healthBits & H.TARE_FAILED) label = 'TARE FAILED';
    else if (healthBits & (H.SAT_CH1 | H.SAT_CH2)) label = 'SATURATED';
  } else if (
    healthBits & (H.TARE_NOISY | H.TARING | H.CALIBRATING) ||
    (Number.isFinite(stddevG) && stddevG >= 0.1) ||
    (calibrated && lastCalAgoDays > RECAL_DAYS)
  ) {
    color = 'bg-yellow-500';
    if (healthBits & H.TARING) label = 'TARING';
    else if (healthBits & H.CALIBRATING) label = 'CALIBRATING';
    else if (healthBits & H.TARE_NOISY) label = 'NOISY TARE';
    else if (Number.isFinite(stddevG) && stddevG >= 0.1) label = 'NOISY';
    else label = 'RECAL DUE';
  } else if (!calibrated) {
    color = 'bg-red-500';
    label = 'NOT CALIBRATED';
  }
  return (
    <span className='inline-flex items-center gap-2 rounded-full bg-base-200 px-3 py-1 text-xs font-medium'>
      <span className={`inline-block h-2 w-2 rounded-full ${color}`} />
      {label}
    </span>
  );
}

function ProgressBar({ samples, target, stddevG, label }) {
  const pct = target > 0 ? Math.min(100, Math.round((samples / target) * 100)) : 0;
  return (
    <div className='space-y-1'>
      <div className='flex justify-between text-xs text-base-content/60'>
        <span>{label}</span>
        <span>
          {samples}/{target} · σ {formatNumber(stddevG, 3)} g
        </span>
      </div>
      <progress className='progress progress-primary w-full' value={pct} max='100' />
    </div>
  );
}

export function ScaleCalibration() {
  const apiService = useContext(ApiServiceContext);

  // Snapshot from WebSocket status events.
  const status = machine.value.status;
  const scale = status.scale || {};

  const [refWeight1, setRefWeight1] = useState(100);
  const [refWeight2, setRefWeight2] = useState(100);
  const [tareProgress, setTareProgress] = useState(null);
  const [calProgress, setCalProgress] = useState({ 1: null, 2: null });
  const [toast, setToast] = useState(null);
  const [diagOpen, setDiagOpen] = useState(false);

  // Tare windows are dictated by the controller; expose a sensible visual
  // target derived from observed totals if we ever see them.
  const tareTargetSamples = 32;
  const calTargetSamples = 64;

  // Settings (persisted cal factors / offsets / timestamps / cal-time stddev).
  const [settingsKey, setSettingsKey] = useState(0);
  const { data: scaleSettings = {} } = useQuery(`scale-settings-${settingsKey}`, async () => {
    const response = await fetch(`/api/settings`);
    const data = await response.json();
    return {
      scaleCalibration1: data.scaleCalibration1,
      scaleCalibration2: data.scaleCalibration2,
      scaleOffset1: data.scaleOffset1,
      scaleOffset2: data.scaleOffset2,
      scaleCalTimestamp1: data.scaleCalTimestamp1 ?? 0,
      scaleCalTimestamp2: data.scaleCalTimestamp2 ?? 0,
      scaleCalStddev1: data.scaleCalStddev1 ?? 0,
      scaleCalStddev2: data.scaleCalStddev2 ?? 0,
    };
  });

  // Subscribe to async tare/cal events.
  const toastTimer = useRef(null);
  const showToast = useCallback((kind, text) => {
    if (toastTimer.current) clearTimeout(toastTimer.current);
    setToast({ kind, text });
    toastTimer.current = setTimeout(() => setToast(null), 5000);
  }, []);

  useEffect(() => {
    if (!apiService) return;

    const idTareProg = apiService.on('evt:scale:tare:progress', m => {
      setTareProgress({ samples: m.samples ?? 0, stddevG: m.stddevG ?? 0 });
    });
    const idTareDone = apiService.on('evt:scale:tare:done', m => {
      setTareProgress(null);
      setSettingsKey(Date.now());
      if (m.success) {
        showToast('success', `Tare complete (σ ${formatNumber(m.std1, 3)} / ${formatNumber(m.std2, 3)} g)`);
      } else {
        const noisy = (m.healthBits & H.TARE_NOISY) !== 0;
        showToast('error', noisy ? 'Tare noisy — try again on a stable surface' : 'Tare failed — check the scale and retry');
      }
    });
    const idCalProg = apiService.on('evt:scale:cal:progress', m => {
      const ch = m.channel ?? 0;
      setCalProgress(prev => ({ ...prev, [ch]: { samples: m.samples ?? 0, stddevG: m.stddevG ?? 0 } }));
    });
    const idCalDone = apiService.on('evt:scale:cal:done', m => {
      const ch = m.channel ?? 0;
      setCalProgress(prev => ({ ...prev, [ch]: null }));
      setSettingsKey(Date.now());
      if (m.success) {
        showToast('success', `Channel ${ch} calibrated · factor ${formatNumber(m.factor, 2)}, σ ${formatNumber(m.stddevG, 3)} g`);
      } else {
        const reason = CAL_ERROR_LABELS[m.errorCode] || `Error ${m.errorCode}`;
        showToast('error', `Channel ${ch} cal failed — ${reason}`);
      }
    });

    return () => {
      apiService.off('evt:scale:tare:progress', idTareProg);
      apiService.off('evt:scale:tare:done', idTareDone);
      apiService.off('evt:scale:cal:progress', idCalProg);
      apiService.off('evt:scale:cal:done', idCalDone);
    };
  }, [apiService, showToast]);

  const calibrated =
    Number.isFinite(scaleSettings.scaleCalibration1) &&
    scaleSettings.scaleCalibration1 !== 0 &&
    Number.isFinite(scaleSettings.scaleCalibration2) &&
    scaleSettings.scaleCalibration2 !== 0;

  const onTare = useCallback(() => {
    apiService.send({ tp: 'req:scale:tare' });
  }, [apiService]);

  const onCalibrateChannel = useCallback(
    (channel, refWeight) => {
      const ref = parseFloat(refWeight);
      if (!Number.isFinite(ref) || ref <= 0) {
        showToast('error', 'Reference weight must be > 0 g');
        return;
      }
      apiService.send({ tp: 'req:scale:cal:start', channel, refWeight: ref });
    },
    [apiService, showToast],
  );

  const lastCalAgoDays1 = scaleSettings.scaleCalTimestamp1 > 0 ? (Date.now() / 1000 - scaleSettings.scaleCalTimestamp1) / 86400 : Infinity;
  const lastCalAgoDays2 = scaleSettings.scaleCalTimestamp2 > 0 ? (Date.now() / 1000 - scaleSettings.scaleCalTimestamp2) / 86400 : Infinity;
  const oldestCalAgo = Math.min(lastCalAgoDays1, lastCalAgoDays2);

  const tareDisabled =
    !scale.present || !!tareProgress || (Number.isFinite(scale.stddevG) && scale.stddevG > 0.5);

  return (
    <>
      <div className='mb-4 flex items-start justify-between gap-3'>
        <div>
          <h1 className='text-2xl font-bold sm:text-3xl'>Hardware Scale Calibration</h1>
          <p className='text-base-content/60 mt-1 text-sm'>
            HX711 dual-channel calibration. Tare with an empty scale, then calibrate each
            channel independently with a known reference weight.
          </p>
        </div>
        <HealthBadge
          healthBits={scale.healthBits ?? 0}
          stddevG={scale.stddevG}
          calibrated={calibrated}
          lastCalAgoDays={oldestCalAgo}
        />
      </div>

      {toast && (
        <div className={`alert mb-4 ${toast.kind === 'error' ? 'alert-error' : 'alert-success'}`}>
          <span>{toast.text}</span>
        </div>
      )}

      <div className='grid grid-cols-1 gap-4 lg:grid-cols-12'>
        <Card sm={12} lg={6} title='Live readings'>
          <div className='space-y-3'>
            <div className='bg-primary/10 rounded-lg p-4'>
              <div className='flex items-baseline justify-between'>
                <div>
                  <div className='text-base-content/60 text-xs uppercase tracking-wide'>Total weight</div>
                  <div className='mt-1 font-mono text-3xl font-semibold'>{formatNumber(scale.weightG, 2)} g</div>
                </div>
                <div className='text-right'>
                  <div className='text-base-content/60 text-xs uppercase tracking-wide'>Combined σ</div>
                  <div className='mt-1 font-mono text-lg'>{formatNumber(scale.stddevG, 3)} g</div>
                </div>
              </div>
              <div className='mt-2 flex items-center gap-2'>
                <span className={`inline-block h-2 w-2 rounded-full ${scale.present ? 'bg-green-500' : 'bg-red-500'}`} />
                <span className='text-base-content/50 text-xs'>{scale.present ? 'Scale detected' : 'Scale not detected'}</span>
              </div>
            </div>
            <div className='grid grid-cols-2 gap-3'>
              <div className='bg-base-200 rounded-lg p-3'>
                <div className='text-base-content/60 text-xs uppercase tracking-wide'>Channel 1</div>
                <div className='mt-1 font-mono text-lg font-semibold'>{formatNumber(scale.ch1G, 2)} g</div>
                <div className='text-base-content/50 text-xs mt-1'>σ {formatNumber(scale.ch1StdG, 3)} g</div>
                <div className='text-base-content/50 text-xs'>
                  factor {formatNumber(scaleSettings.scaleCalibration1, 2)} · offset{' '}
                  {scaleSettings.scaleOffset1 ?? '—'}
                </div>
              </div>
              <div className='bg-base-200 rounded-lg p-3'>
                <div className='text-base-content/60 text-xs uppercase tracking-wide'>Channel 2</div>
                <div className='mt-1 font-mono text-lg font-semibold'>{formatNumber(scale.ch2G, 2)} g</div>
                <div className='text-base-content/50 text-xs mt-1'>σ {formatNumber(scale.ch2StdG, 3)} g</div>
                <div className='text-base-content/50 text-xs'>
                  factor {formatNumber(scaleSettings.scaleCalibration2, 2)} · offset{' '}
                  {scaleSettings.scaleOffset2 ?? '—'}
                </div>
              </div>
            </div>
          </div>
        </Card>

        <Card sm={12} lg={6} title='Tare and calibrate'>
          <div className='space-y-6'>
            <div>
              <p className='text-base-content/60 mb-3 text-sm'>
                <strong>Step 1:</strong> Remove all weight, wait for σ &lt; 0.05 g, then tare.
              </p>
              <button
                className='btn btn-outline w-full'
                onClick={onTare}
                disabled={tareDisabled}
                title={tareDisabled && !tareProgress ? 'Wait until the scale settles (σ ≤ 0.5 g)' : ''}
              >
                {tareProgress ? 'Taring…' : 'Tare (zero both channels)'}
              </button>
              {tareProgress && (
                <div className='mt-3'>
                  <ProgressBar
                    samples={tareProgress.samples}
                    target={tareTargetSamples}
                    stddevG={tareProgress.stddevG}
                    label='Settling'
                  />
                </div>
              )}
            </div>

            <div className='divider'>Step 2 — calibrate channels</div>

            {[1, 2].map(ch => {
              const refState = ch === 1 ? refWeight1 : refWeight2;
              const setRef = ch === 1 ? setRefWeight1 : setRefWeight2;
              const prog = calProgress[ch];
              const ts = ch === 1 ? scaleSettings.scaleCalTimestamp1 : scaleSettings.scaleCalTimestamp2;
              const stdAtCal = ch === 1 ? scaleSettings.scaleCalStddev1 : scaleSettings.scaleCalStddev2;
              const factor = ch === 1 ? scaleSettings.scaleCalibration1 : scaleSettings.scaleCalibration2;
              const otherProg = calProgress[ch === 1 ? 2 : 1];
              const refValid = Number.isFinite(parseFloat(refState)) && parseFloat(refState) > 0;
              const disabled = !!prog || !!otherProg || !!tareProgress || !refValid || !scale.present;
              return (
                <div key={ch}>
                  <p className='text-base-content/60 mb-3 text-sm'>
                    Place a known weight on <strong>channel {ch}</strong>, enter its mass, then
                    press calibrate.
                  </p>
                  <div className='flex items-end gap-3'>
                    <div className='form-control flex-1'>
                      <label className='label text-sm font-medium'>
                        <span>Channel {ch} reference weight</span>
                      </label>
                      <label className='input w-full'>
                        <input
                          type='number'
                          value={refState}
                          onInput={e => setRef(e.currentTarget.value)}
                          min='1'
                          step='any'
                        />
                        <span aria-label='grams'>g</span>
                      </label>
                    </div>
                    <button
                      className='btn btn-primary'
                      onClick={() => onCalibrateChannel(ch, refState)}
                      disabled={disabled}
                    >
                      Calibrate Ch{ch}
                    </button>
                  </div>
                  {prog && (
                    <div className='mt-3'>
                      <ProgressBar
                        samples={prog.samples}
                        target={calTargetSamples}
                        stddevG={prog.stddevG}
                        label={`Calibrating channel ${ch}`}
                      />
                    </div>
                  )}
                  <div className='text-base-content/50 mt-2 text-xs'>
                    Last calibrated {relativeTime(ts)}
                    {Number.isFinite(factor) && factor !== 0
                      ? ` · factor ${formatNumber(factor, 2)} · σ ${formatNumber(stdAtCal, 3)} g`
                      : ''}
                  </div>
                </div>
              );
            })}
          </div>
        </Card>

        <Card sm={12} lg={12} title='Diagnostics'>
          <button
            className='btn btn-ghost btn-sm w-full justify-start'
            onClick={() => setDiagOpen(open => !open)}
          >
            {diagOpen ? '▾ Hide diagnostics' : '▸ Show diagnostics'}
          </button>
          {diagOpen && (
            <div className='mt-3 grid grid-cols-2 gap-3 text-sm'>
              <div>
                Health bits: <span className='font-mono'>0x{(scale.healthBits ?? 0).toString(16).padStart(4, '0')}</span>
              </div>
              <div>
                Sample seq: <span className='font-mono'>{scale.sampleSeq ?? 0}</span>
              </div>
              <div>
                Ch1 saturated: <span className='font-mono'>{scale.healthBits & H.SAT_CH1 ? 'yes' : 'no'}</span>
              </div>
              <div>
                Ch2 saturated: <span className='font-mono'>{scale.healthBits & H.SAT_CH2 ? 'yes' : 'no'}</span>
              </div>
              <div>
                Stale: <span className='font-mono'>{scale.healthBits & H.STALE ? 'yes' : 'no'}</span>
              </div>
              <div>
                Last tare ok: <span className='font-mono'>{scale.healthBits & H.TARE_FAILED ? 'NO' : 'yes'}</span>
              </div>
            </div>
          )}
        </Card>
      </div>
    </>
  );
}
