export const PREFERENCE_ACTIONS = ['new_better', 'anchor_better', 'tie', 'abstain'];

export function preferenceGoalSummary(prompt) {
  if (prompt?.taste_goal_summary) return prompt.taste_goal_summary;
  const goal = prompt?.taste_goal;
  if (!goal) return 'Goal details unavailable';
  if (goal.mode === 'balanced') return 'Balanced';
  return (
    Object.entries(goal.targets || {})
      .map(([attribute, level]) => `${attribute.replaceAll('_', ' ')}: ${level}`)
      .join(', ') || 'Goal details unavailable'
  );
}

export function preferenceAnchorDetails(prompt) {
  const anchor = prompt?.anchor;
  const id = prompt?.anchor_shot_id || 'Unknown';
  if (!anchor) return { id, available: false };
  const absolute = anchor.current_absolute_step;
  const grind = Number.isFinite(absolute) ? absolute : anchor.relative_grind_steps_from_reference;
  return {
    id,
    available: true,
    date:
      Number.isFinite(anchor.timestamp) && anchor.timestamp > 0
        ? new Date(anchor.timestamp * 1000).toLocaleString()
        : 'Date unavailable',
    grind: Number.isFinite(grind)
      ? `${grind.toFixed(1)}${Number.isFinite(absolute) ? '' : ' relative to reference'}`
      : 'Unknown',
    dose: anchor.dose_g,
    targetYield: anchor.target_yield_g,
    actualYield: anchor.beverage_out_g,
    profile: anchor.profile_label,
  };
}
