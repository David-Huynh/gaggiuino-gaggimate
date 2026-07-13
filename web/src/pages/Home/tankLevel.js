export function normalizeTankLevel(value) {
  const numericValue = Number(value);
  if (!Number.isFinite(numericValue)) return 0;
  return Math.round(Math.max(0, Math.min(100, numericValue)));
}

export function normalizeTofDistance(value) {
  const numericValue = Number(value);
  if (!Number.isFinite(numericValue) || numericValue <= 0) return null;
  return Math.round(numericValue);
}

export function hasTankSensor(capability, distance) {
  return capability === true || normalizeTofDistance(distance) !== null;
}

export function formatTankLevelLabel(level, distance) {
  const normalizedLevel = normalizeTankLevel(level);
  const normalizedDistance = normalizeTofDistance(distance);
  return normalizedDistance === null
    ? `Tank level: ${normalizedLevel}%`
    : `Tank level: ${normalizedLevel}% (${normalizedDistance} mm ToF)`;
}
