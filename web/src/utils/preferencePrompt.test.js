import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  PREFERENCE_ACTIONS,
  preferenceAnchorDetails,
  preferenceGoalSummary,
} from './preferencePrompt.js';

test('abstention is distinct from a tie', () => {
  assert.deepEqual(PREFERENCE_ACTIONS, ['new_better', 'anchor_better', 'tie', 'abstain']);
});

test('display the selected goal and never invent a balanced goal when it is missing', () => {
  assert.equal(
    preferenceGoalSummary({ taste_goal: { mode: 'custom', targets: { sweet: 'high' } } }),
    'sweet: high',
  );
  assert.equal(preferenceGoalSummary({ taste_goal: { mode: 'balanced' } }), 'Balanced');
  assert.equal(preferenceGoalSummary({}), 'Goal details unavailable');
});

test('anchor includes identity, recipe and actual output without confusing absolute and relative grind', () => {
  const prompt = {
    anchor_shot_id: 'older-shot',
    anchor: {
      timestamp: 1720000000,
      current_absolute_step: 0,
      relative_grind_steps_from_reference: 5,
      dose_g: 18,
      target_yield_g: 36,
      beverage_out_g: 35.5,
      profile_label: 'Morning espresso',
    },
  };
  const details = preferenceAnchorDetails(prompt);
  assert.equal(details.id, 'older-shot');
  assert.equal(details.grind, '0.0');
  assert.equal(details.dose, 18);
  assert.equal(details.targetYield, 36);
  assert.equal(details.actualYield, 35.5);
  assert.equal(details.profile, 'Morning espresso');
  assert.notEqual(details.date, 'Date unavailable');
  prompt.anchor.current_absolute_step = null;
  assert.equal(preferenceAnchorDetails(prompt).grind, '5.0 relative to reference');
  assert.deepEqual(preferenceAnchorDetails({ anchor_shot_id: 'older-shot' }), {
    id: 'older-shot',
    available: false,
  });
});
