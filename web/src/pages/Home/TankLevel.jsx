import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { faDroplet } from '@fortawesome/free-solid-svg-icons/faDroplet';
import PropTypes from 'prop-types';
import { formatTankLevelLabel, normalizeTankLevel } from './tankLevel.js';

export function TankLevel({ level, distance, compact = false }) {
  const normalizedLevel = normalizeTankLevel(level);
  const label = formatTankLevelLabel(level, distance);
  const levelColor = normalizedLevel < 20 ? 'bg-error' : 'bg-info';
  const iconColor = normalizedLevel < 20 ? 'text-error' : 'text-info';

  if (compact) {
    return (
      <div
        className='flex shrink-0 items-center justify-center gap-1.5'
        title={label}
        aria-label={label}
      >
        <FontAwesomeIcon icon={faDroplet} className={`${iconColor} text-xs`} />
        <span className='text-base-content font-semibold tabular-nums'>{normalizedLevel}%</span>
      </div>
    );
  }

  return (
    <div className='flex shrink-0 items-center gap-2' title={label} aria-label={label}>
      <FontAwesomeIcon icon={faDroplet} className={`${iconColor} text-sm`} />
      <div className='flex w-20 flex-col gap-1'>
        <div className='flex items-baseline justify-between gap-2 leading-none'>
          <span className='text-base-content/50 text-[0.65rem] font-medium uppercase'>Tank</span>
          <span className='text-base-content text-sm font-semibold tabular-nums'>
            {normalizedLevel}%
          </span>
        </div>
        <div className='bg-base-300 h-1.5 w-full overflow-hidden rounded-full'>
          <div
            className={`${levelColor} h-full rounded-full transition-[width] duration-700`}
            style={{ width: `${normalizedLevel}%` }}
          />
        </div>
      </div>
    </div>
  );
}

TankLevel.propTypes = {
  level: PropTypes.oneOfType([PropTypes.number, PropTypes.string]),
  distance: PropTypes.oneOfType([PropTypes.number, PropTypes.string]),
  compact: PropTypes.bool,
};
