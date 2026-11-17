// A readback affects presentation only. The requested target remains the
// authority for editing, profiles and optimizer context.
export function displayedTemperatureTarget(requested, controller) {
  const target = Number(requested);
  const reported = typeof controller === 'number' ? controller : NaN;
  const mismatch =
    Number.isFinite(reported) &&
    reported >= 0 &&
    Number.isFinite(target) &&
    Math.abs(reported - target) > 0.5;
  return { value: mismatch ? reported : target, mismatch };
}
