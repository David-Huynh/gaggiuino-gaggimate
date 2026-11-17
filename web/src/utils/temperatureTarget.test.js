import assert from 'node:assert/strict';
import test from 'node:test';
import { displayedTemperatureTarget } from './temperatureTarget.js';

test('shows a disagreeing controller target, including heating disabled', () => {
  assert.deepEqual(displayedTemperatureTarget(85, 145), { value: 145, mismatch: true });
  assert.deepEqual(displayedTemperatureTarget(85, 0), { value: 0, mismatch: true });
});

test('keeps the requested target and normal colour for matching or unavailable readback', () => {
  for (const readback of [85, 85.1, 84.9, null, undefined, NaN, Infinity, -1, '', false, '145']) {
    assert.deepEqual(displayedTemperatureTarget(85, readback), { value: 85, mismatch: false });
  }
});
