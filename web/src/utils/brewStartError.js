export function brewStartErrorMessage(reason) {
  switch (reason) {
    case 'tare_timeout':
      return 'Scale tare timed out. Try manual tare, then start again.';
    case 'tare_failed':
      return 'Scale tare failed or the scale is busy. Check the scale and try again.';
    case 'controller_not_ready':
      return 'Controller is not ready. Check its connection and firmware versions.';
    case 'queue_full':
      return 'Start was not accepted. The controller is busy; try again.';
    case 'process_busy':
      return 'The previous process has not finished. Stop it before starting again.';
    default:
      return '';
  }
}
