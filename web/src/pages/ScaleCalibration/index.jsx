import { useState, useCallback, useContext } from 'preact/hooks';
import { useQuery } from 'preact-fetching';
import Card from '../../components/Card.jsx';
import { ApiServiceContext } from '../../services/ApiService.js';

export function ScaleCalibration() {
  const [calKey, setCalKey] = useState(0);
  const [refWeight1, setRefWeight1] = useState(100);
  const [refWeight2, setRefWeight2] = useState(100);
  const [calStatus, setCalStatus] = useState('');
  const apiService = useContext(ApiServiceContext);

  const { data: scaleSettings = {} } = useQuery(`scale-settings-${calKey}`, async () => {
    const response = await fetch(`/api/settings`);
    const data = await response.json();
    return {
      scaleCalibration1: data.scaleCalibration1,
      scaleCalibration2: data.scaleCalibration2,
      scaleOffset1: data.scaleOffset1,
      scaleOffset2: data.scaleOffset2,
    };
  });

  const onTare = useCallback(() => {
    apiService.send({ tp: 'req:scale:tare' });
    setCalStatus('Taring — offsets will update shortly...');
    setTimeout(() => {
      setCalKey(Date.now());
      setCalStatus('');
    }, 3000);
  }, [apiService]);

  const onCalibrateChannel = useCallback(
    (channel, refWeight) => {
      apiService.send({ tp: 'req:scale:cal:start', channel, refWeight: parseFloat(refWeight) });
      setCalStatus(`Calibrating channel ${channel}...`);
      setTimeout(() => {
        setCalKey(Date.now());
        setCalStatus('');
      }, 3000);
    },
    [apiService],
  );

  return (
    <>
      <div className='mb-4'>
        <h1 className='text-2xl font-bold sm:text-3xl'>Hardware Scale Calibration</h1>
        <p className='text-base-content/60 mt-1 text-sm'>
          Calibrate the HX711 load cell channels. Tare with an empty scale, then place a known
          reference weight on each channel.
        </p>
      </div>

      <div className='grid grid-cols-1 gap-4 lg:grid-cols-12'>
        <Card sm={12} lg={6} title='Current Values'>
          <div className='grid grid-cols-2 gap-3'>
            <div className='bg-base-200 rounded-lg p-4'>
              <div className='text-base-content/60 text-xs uppercase tracking-wide'>
                Ch1 Calibration Factor
              </div>
              <div className='mt-1 font-mono text-lg font-semibold'>
                {scaleSettings.scaleCalibration1 != null ? scaleSettings.scaleCalibration1 : '—'}
              </div>
            </div>
            <div className='bg-base-200 rounded-lg p-4'>
              <div className='text-base-content/60 text-xs uppercase tracking-wide'>
                Ch2 Calibration Factor
              </div>
              <div className='mt-1 font-mono text-lg font-semibold'>
                {scaleSettings.scaleCalibration2 != null ? scaleSettings.scaleCalibration2 : '—'}
              </div>
            </div>
            <div className='bg-base-200 rounded-lg p-4'>
              <div className='text-base-content/60 text-xs uppercase tracking-wide'>Ch1 Offset</div>
              <div className='mt-1 font-mono text-lg font-semibold'>
                {scaleSettings.scaleOffset1 != null ? scaleSettings.scaleOffset1 : '—'}
              </div>
            </div>
            <div className='bg-base-200 rounded-lg p-4'>
              <div className='text-base-content/60 text-xs uppercase tracking-wide'>Ch2 Offset</div>
              <div className='mt-1 font-mono text-lg font-semibold'>
                {scaleSettings.scaleOffset2 != null ? scaleSettings.scaleOffset2 : '—'}
              </div>
            </div>
          </div>
        </Card>

        <Card sm={12} lg={6} title='Calibration'>
          <div className='space-y-6'>
            <div>
              <p className='text-base-content/60 mb-3 text-sm'>
                <strong>Step 1:</strong> Remove all weight from the scale and tare to zero both
                channels.
              </p>
              <button className='btn btn-outline w-full' onClick={onTare}>
                Tare (Zero Both Channels)
              </button>
            </div>

            <div className='divider'>Step 2 — Calibrate Channels</div>

            <div>
              <p className='text-base-content/60 mb-3 text-sm'>
                Place a known weight on channel 1, enter its mass, then press Calibrate.
              </p>
              <div className='flex items-end gap-3'>
                <div className='form-control flex-1'>
                  <label className='label text-sm font-medium'>
                    <span>Channel 1 Reference Weight</span>
                  </label>
                  <label className='input w-full'>
                    <input
                      type='number'
                      value={refWeight1}
                      onInput={e => setRefWeight1(e.currentTarget.value)}
                      min='1'
                      step='any'
                    />
                    <span aria-label='grams'>g</span>
                  </label>
                </div>
                <button
                  className='btn btn-primary'
                  onClick={() => onCalibrateChannel(1, refWeight1)}
                >
                  Calibrate Ch1
                </button>
              </div>
            </div>

            <div>
              <p className='text-base-content/60 mb-3 text-sm'>
                Place a known weight on channel 2, enter its mass, then press Calibrate.
              </p>
              <div className='flex items-end gap-3'>
                <div className='form-control flex-1'>
                  <label className='label text-sm font-medium'>
                    <span>Channel 2 Reference Weight</span>
                  </label>
                  <label className='input w-full'>
                    <input
                      type='number'
                      value={refWeight2}
                      onInput={e => setRefWeight2(e.currentTarget.value)}
                      min='1'
                      step='any'
                    />
                    <span aria-label='grams'>g</span>
                  </label>
                </div>
                <button
                  className='btn btn-primary'
                  onClick={() => onCalibrateChannel(2, refWeight2)}
                >
                  Calibrate Ch2
                </button>
              </div>
            </div>

            {calStatus && (
              <div className='alert alert-info'>
                <span>{calStatus}</span>
              </div>
            )}
          </div>
        </Card>
      </div>
    </>
  );
}
