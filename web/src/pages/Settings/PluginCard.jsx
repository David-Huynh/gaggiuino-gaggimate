import { faTrashCan } from '@fortawesome/free-solid-svg-icons/faTrashCan';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import homekitImage from '../../assets/homekit.png';
import { faCalendarDays } from '@fortawesome/free-solid-svg-icons/faCalendarDays';
import { computed } from '@preact/signals';
import { machine } from '../../services/ApiService.js';
import {
  communityUploadPipelineText,
  communityUploadQueueText,
} from '../../utils/communityUploadStatus.js';

const gearpumpAddon = computed(() => machine.value.capabilities.gearpumpAddon);
const AUTO_TUNING_PROVIDER_OPTIONS = [
  { value: 'off_board', label: 'Off-board EspressoRL' },
  { value: 'on_board', label: 'On-board' },
];

function formatStatusTime(timestamp) {
  const value = Number(timestamp || 0);
  if (!Number.isFinite(value) || value <= 0) {
    return 'Never';
  }
  return new Date(value * 1000).toLocaleString();
}

function lastShotStatusText(formData) {
  if (!formData.rlLastShotId) {
    return 'None';
  }
  return `${formatStatusTime(formData.rlLastShotAt)} - ${formData.rlLastShotId}`;
}

function mqttConfigured(formData) {
  const port = Number(formData.haPort);
  return Boolean(String(formData.haIP || '').trim()) && Number.isInteger(port) && port > 0 && port <= 65535;
}

function statusText(formData) {
  if (!formData.rlAutoTuningEnabled) {
    return 'Disabled';
  }
  if (formData.rlProviderMode === 'on_board') {
    return 'Not implemented';
  }
  if (formData.rlProviderMode === 'off_board' && !mqttConfigured(formData)) {
    return 'Needs MQTT';
  }
  if (!formData.rlStatusSeen) {
    return 'Add-on not seen';
  }
  return formData.rlAddonOnline ? 'Connected' : 'Offline';
}

function providerLabel(value) {
  return AUTO_TUNING_PROVIDER_OPTIONS.find(option => option.value === value)?.label || 'Disabled';
}

function providerStatusTone(formData) {
  if (!formData.rlAutoTuningEnabled) {
    return 'badge-neutral';
  }
  if (formData.rlProviderMode === 'off_board' && formData.rlAddonOnline) {
    return 'badge-success';
  }
  return 'badge-warning';
}

function applyStatusText(value) {
  const statuses = {
    applied: 'Applied',
    partially_applied: 'Partially applied',
    manual_required: 'Manual required',
    failed: 'Failed',
    unknown: 'Unknown',
  };
  return statuses[value] || 'None';
}

function StatusRow({ label, value }) {
  return (
    <div className='flex items-center justify-between gap-3 text-sm'>
      <span className='opacity-70'>{label}</span>
      <span className='max-w-44 truncate text-right font-medium'>{value}</span>
    </div>
  );
}

function CommunityUploadSettings({ formData, onChange }) {
  return (
    <div className='bg-base-200 rounded-lg p-4'>
      <div className='space-y-4'>
        <div className='flex items-start justify-between gap-4'>
          <div className='min-w-0'>
            <span className='block text-xl font-medium'>Anonymous Community Upload</span>
            <span className='text-sm opacity-70'>
              Share anonymized shot data for community research.
            </span>
          </div>
          <input
            id='rlCommunityUploadEnabled'
            name='rlCommunityUploadEnabled'
            value='rlCommunityUploadEnabled'
            type='checkbox'
            className='toggle toggle-primary shrink-0'
            checked={!!formData.rlCommunityUploadEnabled}
            onChange={onChange('rlCommunityUploadEnabled')}
            aria-label='Enable anonymous community upload'
          />
        </div>

        <div className='border-base-300 space-y-3 border-t pt-4'>
          <StatusRow label='Upload pipeline' value={communityUploadPipelineText(formData)} />
          {formData.rlCommunityUploadEnabled && (
            <>
              {formData.communityUploadSummary && (
                <p className='text-base-content/70 text-sm'>{formData.communityUploadSummary}</p>
              )}
              <StatusRow label='Queue' value={communityUploadQueueText(formData)} />
              <StatusRow
                label='Storage'
                value={
                  formData.communityUploadStorageAvailable
                    ? formData.communityUploadStorageBackend || 'Device'
                    : 'Unavailable'
                }
              />
              <div className='form-control'>
                <label htmlFor='rlUploadBaseUrl' className='mb-2 block text-sm font-medium'>
                  Supabase base URL
                </label>
                <input
                  id='rlUploadBaseUrl'
                  name='rlUploadBaseUrl'
                  type='url'
                  className='input input-bordered w-full'
                  value={formData.rlUploadBaseUrl || ''}
                  onChange={onChange('rlUploadBaseUrl')}
                  placeholder='https://project-ref.supabase.co'
                />
              </div>
              <StatusRow
                label='Device credential'
                value={formData.rlUploadCredentialConfigured ? 'Registered' : 'Pending'}
              />
            </>
          )}
        </div>
      </div>
    </div>
  );
}

export function PluginCard({
  formData,
  onChange,
  autowakeupSchedules,
  addAutoWakeupSchedule,
  removeAutoWakeupSchedule,
  updateAutoWakeupTime,
  updateAutoWakeupDay,
}) {
  return (
    <div className='space-y-4'>
      <div className='bg-base-200 rounded-lg p-4'>
        <div className='flex items-center justify-between'>
          <span className='text-xl font-medium'>Automatic Wakeup Schedule</span>
          <input
            id='autowakeupEnabled'
            name='autowakeupEnabled'
            value='autowakeupEnabled'
            type='checkbox'
            className='toggle toggle-primary'
            checked={!!formData.autowakeupEnabled}
            onChange={onChange('autowakeupEnabled')}
            aria-label='Enable Auto Wakeup'
          />
        </div>
        {formData.autowakeupEnabled && (
          <div className='border-base-300 mt-4 space-y-4 border-t pt-4'>
            <p className='text-sm opacity-70'>
              Automatically switch to brew mode at specified time(s) of day.
            </p>
            <div className='form-control'>
              <label className='mb-2 block text-sm font-medium'>Auto Wakeup Schedule</label>
              <div className='space-y-2'>
                {autowakeupSchedules?.map((schedule, scheduleIndex) => (
                  <div
                    key={scheduleIndex}
                    className='flex flex-wrap items-center gap-1 md:flex-nowrap'
                  >
                    {/* Time input */}
                    <div className='grow-1 text-center sm:text-start'>
                      <input
                        type='time'
                        className='input input-bordered input-sm md:input-md w-auto min-w-0 pr-6 text-center'
                        value={schedule.time}
                        onChange={e => updateAutoWakeupTime(scheduleIndex, e.target.value)}
                        disabled={!formData.autowakeupEnabled}
                      />
                    </div>

                    {/* Days toggle buttons */}
                    <div
                      className='join flex grow-8'
                      role='group'
                      aria-label='Days of week selection'
                    >
                      {['M', 'T', 'W', 'T', 'F', 'S', 'S'].map((dayLabel, dayIndex) => (
                        <button
                          key={dayIndex}
                          type='button'
                          className={`join-item btn btn-sm md:btn-md flex-grow ${schedule.days[dayIndex] ? 'btn-primary' : 'btn-neutral text-neutral-content/20'}`}
                          onClick={() =>
                            updateAutoWakeupDay(scheduleIndex, dayIndex, !schedule.days[dayIndex])
                          }
                          disabled={!formData.autowakeupEnabled}
                          aria-pressed={schedule.days[dayIndex]}
                          aria-label={
                            [
                              'Monday',
                              'Tuesday',
                              'Wednesday',
                              'Thursday',
                              'Friday',
                              'Saturday',
                              'Sunday',
                            ][dayIndex]
                          }
                          title={
                            [
                              'Monday',
                              'Tuesday',
                              'Wednesday',
                              'Thursday',
                              'Friday',
                              'Saturday',
                              'Sunday',
                            ][dayIndex]
                          }
                        >
                          {dayLabel}
                        </button>
                      ))}
                    </div>
                    {/* Delete button */}
                    {autowakeupSchedules.length > 1 ? (
                      <button
                        type='button'
                        onClick={() => removeAutoWakeupSchedule(scheduleIndex)}
                        className='btn btn-ghost btn-sm md:btn-md grow-1'
                        disabled={!formData.autowakeupEnabled}
                        title='Delete this schedule'
                      >
                        <FontAwesomeIcon icon={faTrashCan} className='text-base' />
                      </button>
                    ) : (
                      <div
                        className='btn btn-ghost btn-sm md:btn-md grow-1 cursor-not-allowed opacity-30'
                        title='Cannot delete the last schedule'
                      >
                        <FontAwesomeIcon icon={faTrashCan} className='text-base' />
                      </div>
                    )}
                  </div>
                ))}
                <button
                  type='button'
                  onClick={addAutoWakeupSchedule}
                  className='btn btn-primary btn-sm md:btn-md mt-2'
                  disabled={!formData.autowakeupEnabled}
                  aria-label='Add schedule'
                  title='Add schedule'
                >
                  <FontAwesomeIcon icon={faCalendarDays} />
                </button>
              </div>
            </div>
          </div>
        )}
      </div>

      <div className='bg-base-200 rounded-lg p-4'>
        <div className='flex items-center justify-between'>
          <span className='text-xl font-medium'>HomeKit</span>
          <input
            id='homekit'
            name='homekit'
            value='homekit'
            type='checkbox'
            className='toggle toggle-primary'
            checked={!!formData.homekit}
            onChange={onChange('homekit')}
            aria-label='Enable HomeKit'
          />
        </div>
        {formData.homekit && (
          <div className='border-base-300 mt-4 flex flex-col items-center justify-center gap-4 border-t pt-4'>
            <img src={homekitImage} alt='HomeKit Setup Code' />
            <p className='text-center'>
              Open the Home app on your iOS device, select Add Accessory, and enter the setup code
              shown above.
            </p>
          </div>
        )}
      </div>

      <div className='bg-base-200 rounded-lg p-4'>
        <div className='flex items-center justify-between'>
          <span className='text-xl font-medium'>Boiler Refill Plugin</span>
          <input
            id='boilerFillActive'
            name='boilerFillActive'
            value='boilerFillActive'
            type='checkbox'
            className='toggle toggle-primary'
            checked={!!formData.boilerFillActive}
            onChange={onChange('boilerFillActive')}
            aria-label='Enable Boiler Refill'
          />
        </div>
        {formData.boilerFillActive && (
          <div className='border-base-300 mt-4 grid grid-cols-2 gap-4 border-t pt-4'>
            <div className='form-control'>
              <label htmlFor='startupFillTime' className='mb-2 block text-sm font-medium'>
                On startup (s)
              </label>
              <input
                id='startupFillTime'
                name='startupFillTime'
                type='number'
                className='input input-bordered w-full'
                placeholder='0'
                value={formData.startupFillTime}
                onChange={onChange('startupFillTime')}
              />
            </div>
            <div className='form-control'>
              <label htmlFor='steamFillTime' className='mb-2 block text-sm font-medium'>
                On steam deactivate (s)
              </label>
              <input
                id='steamFillTime'
                name='steamFillTime'
                type='number'
                className='input input-bordered w-full'
                placeholder='0'
                value={formData.steamFillTime}
                onChange={onChange('steamFillTime')}
              />
            </div>
          </div>
        )}
      </div>

      <div className='bg-base-200 rounded-lg p-4'>
        <div className='flex items-center justify-between'>
          <span className='text-xl font-medium'>Smart Grind Plugin</span>
          <input
            id='smartGrindActive'
            name='smartGrindActive'
            value='smartGrindActive'
            type='checkbox'
            className='toggle toggle-primary'
            checked={!!formData.smartGrindActive}
            onChange={onChange('smartGrindActive')}
            aria-label='Enable Smart Grind'
          />
        </div>
        {formData.smartGrindActive && (
          <div className='border-base-300 mt-4 space-y-4 border-t pt-4'>
            <p className='text-sm opacity-70'>
              This feature controls a Tasmota Plug to turn off your grinder after the target has
              been reached.
            </p>
            <div className='form-control'>
              <label htmlFor='smartGrindIp' className='mb-2 block text-sm font-medium'>
                Tasmota IP
              </label>
              <input
                id='smartGrindIp'
                name='smartGrindIp'
                type='text'
                className='input input-bordered w-full'
                placeholder='0'
                value={formData.smartGrindIp}
                onChange={onChange('smartGrindIp')}
              />
            </div>
            <div className='form-control'>
              <label htmlFor='smartGrindMode' className='mb-2 block text-sm font-medium'>
                Mode
              </label>
              <select
                id='smartGrindMode'
                name='smartGrindMode'
                className='select select-bordered w-full'
                onChange={onChange('smartGrindMode')}
              >
                <option value='0' selected={formData.smartGrindMode?.toString() === '0'}>
                  Turn off at target
                </option>
                <option value='1' selected={formData.smartGrindMode?.toString() === '1'}>
                  Toggle off and on at target
                </option>
                <option value='2' selected={formData.smartGrindMode?.toString() === '2'}>
                  Turn on at start, off at target
                </option>
              </select>
            </div>
          </div>
        )}
      </div>

      <div className='bg-base-200 rounded-lg p-4'>
        <div className='space-y-4'>
          <div className='flex items-start justify-between gap-4'>
            <div className='min-w-0'>
              <span className='block text-xl font-medium'>Auto Tuning</span>
              <span className='text-sm opacity-70'>
                Choose where recipe recommendations are generated.
              </span>
            </div>
            <input
              id='rlAutoTuningEnabled'
              name='rlAutoTuningEnabled'
              value='rlAutoTuningEnabled'
              type='checkbox'
              className='toggle toggle-primary shrink-0'
              checked={!!formData.rlAutoTuningEnabled}
              onChange={onChange('rlAutoTuningEnabled')}
              aria-label='Enable Auto Tuning'
            />
          </div>
          {formData.rlAutoTuningEnabled && (
            <div className='border-base-300 space-y-3 border-t pt-4'>
              <div className='form-control'>
                <label htmlFor='rlProviderMode' className='mb-2 block text-sm font-medium'>
                  Provider
                </label>
                <select
                  id='rlProviderMode'
                  name='rlProviderMode'
                  className='select select-bordered w-full'
                  value={formData.rlProviderMode === 'on_board' ? 'on_board' : 'off_board'}
                  onChange={onChange('rlProviderMode')}
                >
                  {AUTO_TUNING_PROVIDER_OPTIONS.map(option => (
                    <option key={option.value} value={option.value}>
                      {option.label}
                    </option>
                  ))}
                </select>
              </div>
              <div className='flex items-center justify-between gap-3'>
                <span className='text-sm font-medium'>Provider status</span>
                <span className={`badge ${providerStatusTone(formData)}`}>
                  {statusText(formData)}
                </span>
              </div>
              <StatusRow
                label='Provider'
                value={providerLabel(formData.rlProviderMode || 'disabled')}
              />
              <StatusRow
                label='Details'
                value={formData.rlProviderSummary || statusText(formData)}
              />
              {formData.rlProviderMode === 'off_board' && !mqttConfigured(formData) && (
                <div className='alert alert-warning py-2 text-sm'>
                  Configure the MQTT broker below to use the off-board EspressoRL provider.
                </div>
              )}

              <div className='grid grid-cols-1 gap-3 sm:grid-cols-2'>
                <StatusRow label='Last status' value={formatStatusTime(formData.rlLastStatusAt)} />
                <StatusRow label='Last shot stored' value={lastShotStatusText(formData)} />
                <StatusRow
                  label='Last recommendation'
                  value={formData.rlLastRecommendationId || 'None'}
                />
                <StatusRow
                  label='Apply status'
                  value={applyStatusText(formData.rlRecommendationApplyStatus)}
                />
              </div>

              <StatusRow label='Optimizer' value='Preference Optimization' />
              <div className='grid grid-cols-1 gap-3 sm:grid-cols-2'>
                <StatusRow
                  label='Active bean'
                  value={formData.rlBeanContextName || 'No bean selected'}
                />
                <StatusRow
                  label='Active grinder'
                  value={formData.rlGrinderContextName || 'No grinder selected'}
                />
                <StatusRow
                  label='Local shots'
                  value={(formData.rlLocalShotCount ?? 0).toString()}
                />
              </div>
              <a href='/autotuning' className='btn btn-outline btn-sm w-full'>
                Open Auto Tuning
              </a>
            </div>
          )}

          <section className='border-base-300 space-y-4 border-t pt-4'>
            <span className='block text-base font-semibold'>Off-board MQTT</span>
            <div className='grid grid-cols-1 gap-4 sm:grid-cols-2'>
              <div className='form-control'>
                <label htmlFor='haIP' className='mb-2 block text-sm font-medium'>
                  MQTT host
                </label>
                <input
                  id='haIP'
                  name='haIP'
                  type='text'
                  className='input input-bordered w-full'
                  value={formData.haIP}
                  onChange={onChange('haIP')}
                />
              </div>
              <div className='form-control'>
                <label htmlFor='haPort' className='mb-2 block text-sm font-medium'>
                  MQTT port
                </label>
                <input
                  id='haPort'
                  name='haPort'
                  type='number'
                  min='1'
                  max='65535'
                  className='input input-bordered w-full'
                  value={formData.haPort}
                  onChange={onChange('haPort')}
                />
              </div>
              <div className='form-control'>
                <label htmlFor='haUser' className='mb-2 block text-sm font-medium'>
                  MQTT user
                </label>
                <input
                  id='haUser'
                  name='haUser'
                  type='text'
                  className='input input-bordered w-full'
                  value={formData.haUser}
                  onChange={onChange('haUser')}
                />
              </div>
              <div className='form-control'>
                <label htmlFor='haPassword' className='mb-2 block text-sm font-medium'>
                  MQTT password
                </label>
                <input
                  id='haPassword'
                  name='haPassword'
                  type='password'
                  className='input input-bordered w-full'
                  value={formData.haPassword}
                  onChange={onChange('haPassword')}
                />
              </div>
            </div>
            {formData.legacyHomeAssistantMqttAvailable !== false && (
              <div className='border-base-300 flex items-center justify-between gap-4 border-t pt-4'>
                <label htmlFor='homeAssistant' className='block text-sm font-medium'>
                  Legacy Home Assistant MQTT
                </label>
                <input
                  id='homeAssistant'
                  name='homeAssistant'
                  value='homeAssistant'
                  type='checkbox'
                  className='toggle toggle-primary shrink-0'
                  checked={!!formData.homeAssistant}
                  onChange={onChange('homeAssistant')}
                  aria-label='Enable legacy Home Assistant MQTT'
                />
              </div>
            )}
            {formData.legacyHomeAssistantMqttAvailable !== false && formData.homeAssistant && (
              <div className='form-control'>
                <label htmlFor='haTopic' className='mb-2 block text-sm font-medium'>
                  Discovery topic
                </label>
                <input
                  id='haTopic'
                  name='haTopic'
                  type='text'
                  className='input input-bordered w-full'
                  value={formData.haTopic}
                  onChange={onChange('haTopic')}
                />
              </div>
            )}
          </section>
        </div>
      </div>

      <CommunityUploadSettings formData={formData} onChange={onChange} />

      {gearpumpAddon.value && (
        <div className='bg-base-200 rounded-lg p-4'>
          <div className='flex items-center justify-between'>
            <span className='text-xl font-medium'>BLDC Pump Settings</span>
          </div>
          <div className='border-base-300 mt-4 space-y-4 border-t pt-4'>
            <p className='text-sm opacity-70'>
              The BLDC pump addon was detected in your system. You can change the pump control
              characteristics using the values below.
            </p>

            <div className='form-control'>
              <label htmlFor='commutationGain' className='mb-2 block text-sm font-medium'>
                Commutation Gain
              </label>
              <input
                id='commutationGain'
                name='commutationGain'
                type='number'
                className='input input-bordered w-full'
                placeholder='0'
                min='0'
                max='100'
                step='any'
                value={formData.commutationGain?.toString()}
                onChange={onChange('commutationGain')}
              />
            </div>

            <div className='form-control'>
              <label htmlFor='convergenceGain' className='mb-2 block text-sm font-medium'>
                Convergence Gain
              </label>
              <input
                id='convergenceGain'
                name='convergenceGain'
                type='number'
                className='input input-bordered w-full'
                placeholder='0'
                min='0'
                max='100'
                step='any'
                value={formData.convergenceGain?.toString()}
                onChange={onChange('convergenceGain')}
              />
            </div>

            <div className='form-control'>
              <label htmlFor='integralGain' className='mb-2 block text-sm font-medium'>
                Integral Gain
              </label>
              <input
                id='integralGain'
                name='integralGain'
                type='number'
                className='input input-bordered w-full'
                placeholder='0'
                min='0'
                max='100'
                step='any'
                value={formData.integralGain?.toString()}
                onChange={onChange('integralGain')}
              />
            </div>
            <div className='form-control'>
              <label htmlFor='maxPumpPower' className='mb-2 block text-sm font-medium'>
                Maximum Pump Power (0 - 1)
              </label>
              <input
                id='maxPumpPower'
                name='maxPumpPower'
                type='number'
                placeholder='0'
                min='0'
                max='1'
                step='any'
                className='input input-bordered w-full'
                value={formData.maxPumpPower?.toString()}
                onChange={onChange('maxPumpPower')}
              />
            </div>
            <div className='form-control'>
              <label htmlFor='pumpSlipCoeffs' className='mb-2 block text-sm font-medium'>
                Pump Slip Coefficients
              </label>
              <input
                id='pumpSlipCoeffs'
                name='pumpSlipCoeffs'
                type='text'
                className='input input-bordered w-full'
                placeholder='0,0,0,0'
                value={formData.pumpSlipCoeffs}
                onChange={onChange('pumpSlipCoeffs')}
              />
              <span className='mt-1 text-xs opacity-70'>
                Pressure polynomial (a,b,c,d) for vane-/gear-pump internal leakage. Leave at 0,0,0,0
                if uncalibrated.
              </span>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
