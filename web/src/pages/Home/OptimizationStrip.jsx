import PropTypes from 'prop-types';
import { FontAwesomeIcon } from '@fortawesome/react-fontawesome';
import { faRectangleList } from '@fortawesome/free-solid-svg-icons/faRectangleList';

export function OptimizationStrip({ beanContextName, enabled, hasContext, onToggle }) {
  return (
    <div className='bg-base-200/70 flex h-10 w-full shrink-0 items-center gap-2 rounded-md px-3'>
      <a href='/autotuning' className='flex min-w-0 flex-1 items-center gap-2'>
        <FontAwesomeIcon icon={faRectangleList} className='text-base-content/50 text-xs' />
        <span className='flex min-w-0 flex-col leading-tight'>
          <span className='text-base-content/50 text-[0.55rem] font-semibold tracking-[0.16em] uppercase'>
            Auto Tuning
          </span>
          <span className='text-base-content truncate text-xs font-semibold'>
            {beanContextName || 'No bean selected'}
          </span>
        </span>
      </a>
      <label className='flex shrink-0 items-center gap-1.5 text-[0.65rem] font-semibold'>
        <span>Local</span>
        <input
          type='checkbox'
          className='toggle toggle-primary toggle-xs'
          checked={enabled}
          onChange={onToggle}
          disabled={!hasContext}
          aria-label='Toggle local optimization'
        />
      </label>
    </div>
  );
}

OptimizationStrip.propTypes = {
  beanContextName: PropTypes.string,
  enabled: PropTypes.bool.isRequired,
  hasContext: PropTypes.bool.isRequired,
  onToggle: PropTypes.func.isRequired,
};
