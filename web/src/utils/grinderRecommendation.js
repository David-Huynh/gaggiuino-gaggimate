function optionalFiniteNumber(value) {
  if (value === undefined || value === null || value === '') {
    return undefined;
  }
  const number = Number(value);
  return Number.isFinite(number) ? number : undefined;
}

export function grinderSettingTransition({
  currentAbsoluteStep,
  projectedAbsoluteStep,
  currentRelativeStep,
  projectedRelativeStep,
  deltaSteps,
}) {
  const delta = optionalFiniteNumber(deltaSteps);
  let current = optionalFiniteNumber(currentAbsoluteStep);
  let projected = optionalFiniteNumber(projectedAbsoluteStep);

  if (current !== undefined && projected === undefined && delta !== undefined) {
    projected = current + delta;
  } else if (current === undefined && projected !== undefined && delta !== undefined) {
    current = projected - delta;
  }
  if (current !== undefined && projected !== undefined) {
    return { current, projected, absolute: true };
  }

  projected = optionalFiniteNumber(projectedRelativeStep);
  current = optionalFiniteNumber(currentRelativeStep);
  if (current === undefined && projected !== undefined && delta !== undefined) {
    current = projected - delta;
  }
  return current !== undefined && projected !== undefined
    ? { current, projected, absolute: false }
    : null;
}

export function formatGrinderSettingTransition(values) {
  const transition = grinderSettingTransition(values);
  if (!transition) {
    return '-';
  }
  const text = `${transition.current.toFixed(1)} -> ${transition.projected.toFixed(1)}`;
  return transition.absolute ? text : `${text} rel.`;
}
