import assert from 'node:assert/strict';
import { registerHooks } from 'node:module';
registerHooks({ load(url, context, nextLoad) {
  if (new URL(url).pathname.endsWith('.svg')) return { format: 'module', source: 'export default ""', shortCircuit: true };
  return nextLoad(url, context);
} });
const { default: ApiService } = await import('../web/src/services/ApiService.js');
globalThis.WebSocket = { OPEN: 1 };
const api = Object.create(ApiService.prototype);
api.listeners = {};
api.pendingRequests = new Set();
const sent = [];
api.socket = { readyState: 1, send: text => sent.push(JSON.parse(text)) };
api._scheduleReconnect = () => {};
const empty = () => {
  assert.equal(api.pendingRequests.size, 0);
  assert.deepEqual(api.listeners, {});
};
const reply = request => api._onMessage({ data: JSON.stringify({ tp: 'res:profiles:load', rid: request.rid, profile: { id: 'p' } }) });

for (let i = 0; i < 100; i++) {
  const abort = new AbortController();
  const promise = api.request({ tp: 'req:profiles:load', id: 'p' }, { signal: abort.signal });
  const check = assert.rejects(promise, { name: 'AbortError' });
  abort.abort(); // Navigate away before the selected profile arrives.
  await check;
  reply(sent.at(-1)); // A late response must not revive an abandoned request.
  empty();
}
const cancelled = new AbortController();
cancelled.abort();
const count = sent.length;
await assert.rejects(api.request({ tp: 'req:profiles:load' }, { signal: cancelled.signal }), { name: 'AbortError' });
assert.equal(sent.length, count);
const first = api.request({ tp: 'req:profiles:load' });
const second = api.request({ tp: 'req:profiles:load' });
const disconnected = Promise.all([first, second].map(p => assert.rejects(p, /disconnected/)));
api._onClose();
await disconnected;
empty();
api.socket.send = () => { throw new Error('send failed'); };
await assert.rejects(api.request({ tp: 'req:profiles:load' }), /send failed/);
empty();
api.socket.send = text => sent.push(JSON.parse(text));
const success = api.request({ tp: 'req:profiles:load' });
reply(sent.at(-1));
assert.equal((await success).profile.id, 'p');
empty();
console.log('PASS 100 abandoned requests, late responses, disconnect recovery, send failure and successful reply');
