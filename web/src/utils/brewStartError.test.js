import { test } from 'node:test';
import assert from 'node:assert/strict';
import { brewStartErrorMessage } from './brewStartError.js';

test('startup failures explain tare and connection errors', () => {
  assert.match(brewStartErrorMessage('tare_timeout'), /timed out/);
  assert.match(brewStartErrorMessage('tare_failed'), /Scale tare failed/);
  assert.match(brewStartErrorMessage('controller_not_ready'), /connection/);
  assert.match(brewStartErrorMessage('queue_full'), /not accepted/);
  assert.match(brewStartErrorMessage('process_busy'), /previous process/);
});

test('normal cancellation and empty status do not show an error', () => {
  for (const reason of ['', undefined, 'deactivated', 'cleared', 'mode_changed', 'flush_started']) {
    assert.equal(brewStartErrorMessage(reason), '');
  }
});
