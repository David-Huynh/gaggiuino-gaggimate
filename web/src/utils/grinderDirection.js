export function grindDirectionForStepDelta(deltaSteps, stepDirection = 'higher_is_finer') {
  const delta = Number(deltaSteps);
  if (!Number.isFinite(delta) || delta === 0) {
    return null;
  }
  const physicalDelta = stepDirection === 'higher_is_coarser' ? -delta : delta;
  return physicalDelta > 0 ? 'finer' : 'coarser';
}
