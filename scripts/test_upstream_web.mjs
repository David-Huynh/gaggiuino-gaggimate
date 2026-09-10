import assert from 'node:assert/strict';
import { registerHooks } from 'node:module';
// Status parsing shares warning metadata with the UI; SVG assets are inert on the host.
registerHooks({ load(url, context, nextLoad) {
  if (new URL(url).pathname.endsWith('.svg')) return { format: 'module', source: 'export default ""', shortCircuit: true };
  return nextLoad(url, context);
} });
const { default: ApiService, machine } = await import('../web/src/services/ApiService.js');
import { parseBinaryShot } from '../web/src/pages/ShotHistory/parseBinaryShot.js';
import { calculatePumpedWater } from '../web/src/pages/ShotAnalyzer/services/analyzer/waterIntegration.js';

const api = Object.create(ApiService.prototype);
api._onStatus({ tp: 'evt:status', m: 1, puid: 'profile-a', ct: 93, bsp: true,
  up: false, uptime: 100, ctof: true, gta: true, scale: { w: 12.3, pr: true, seq: 42 } });
const count = machine.value.history.length;
api._onStatus({ tp: 'evt:status', up: true, warn: [], puid: 'profile-b' });
assert.equal(machine.value.history.length, count);
assert.equal(machine.value.status.scale.weightG, 12.3);
assert.equal(machine.value.status.brewStartPending, true);
assert.equal(machine.value.status.grindVolumetricAvailable, true);
assert.equal(machine.value.capabilities.tof, true);
api._onStatus({ tp: 'evt:status', ct: 92.9, pr: 8.9, bsp: false });
assert.equal(machine.value.status.selectedProfileId, 'profile-b');
assert.equal(machine.value.status.update, true);
assert.equal(machine.value.status.brewStartPending, false);
assert.equal(machine.value.history.length, count + 1);
console.log('PASS sparse status: preserves hardware scales, profile, updates and pending brew state');

function fixture(version, water = [0, 25, 50]) {
  const stride = version >= 7 ? 30 : version >= 6 ? 28 : 26;
  const data = new ArrayBuffer(512 + 3 * stride);
  const view = new DataView(data);
  view.setUint32(0, 0x544f4853, true);
  view.setUint8(4, version); view.setUint8(5, stride);
  view.setUint16(6, 512, true); view.setUint16(8, 250, true);
  view.setUint32(12, version >= 7 ? 0x3fff : 0x1fff, true);
  view.setUint32(16, 3, true); view.setUint32(20, 600, true);
  view.setUint16(108, 420, true); // settled final weight outlasts pumping
  for (let i = 0; i < 3; i++) {
    const offset = 512 + i * stride;
    const values = version >= 6 ? 4 : 2;
    if (version >= 6) view.setUint32(offset, [0, 380, 600][i], true);
    else view.setUint16(offset, i, true);
    view.setInt16(offset + values + 8, 200, true); // flow = 2 ml/s
    view.setUint16(offset + values + 16, i * 100, true);
    if (version >= 7) view.setUint16(offset + 28, water[i], true);
  }
  return data;
}
for (const version of [5, 6, 7]) {
  const shot = parseBinaryShot(fixture(version), 'fixture');
  assert.deepEqual(shot.samples.map(s => s.t), version >= 6 ? [0, 380, 600] : [0, 250, 500]);
  assert.equal(shot.volume, 42);
}
const missing = parseBinaryShot(fixture(7, [65535, 65535, 65535]), 'old-artifact');
assert.equal(missing.samples[0].wp, null);
assert.equal(calculatePumpedWater(missing.samples), 1.2); // fallback to measured flow, not zero water
assert.equal(calculatePumpedWater(parseBinaryShot(fixture(7), 'new').samples), 5);
const invalid = fixture(7); new DataView(invalid).setUint8(5, 26);
assert.throws(() => parseBinaryShot(invalid, 'invalid'), /device reports/);
console.log('PASS history: v5/v6/v7, real timing, final weight, unknown pumped water, invalid layout');
