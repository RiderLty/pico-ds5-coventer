import assert from 'node:assert/strict';
import { register } from 'node:module';
register('./ai-raw-loader.mjs', import.meta.url);
const store = new Map([['ds5_lua_draft_v1', JSON.stringify({ script: 'old browser draft' })]]);
globalThis.localStorage = { getItem: k => store.get(k), setItem: (k,v) => store.set(k,v), removeItem: k => store.delete(k) };
const { DeviceSession, deviceSession } = await import('../src/utils/deviceSession.js');
const { WorkspaceModel } = await import('../src/utils/workspaceModel.js');
const { initAiCfg, saveAiCfg, loadAiCfg } = await import('../src/utils/aiSettings.js');
const deferred = () => { let resolve; const promise = new Promise(r => { resolve = r; }); return { promise, resolve }; };
class FakeDevice {
    state = 'disconnected';
    source = 'device A';
    cfg = null;
    get connected() { return this.state === 'connected'; }
    onStateChange(fn) { this.changed = fn; }
    onLog() {} onError() {} onClose() {}
    stateTo(s) { this.state = s; this.changed(s); }
    async connect() { this.stateTo('connecting'); this.stateTo('connected'); }
    async disconnect() { this.stateTo('disconnected'); }
    async scriptRead() { return this.pendingRead ? this.pendingRead.promise : this.source; }
    async scriptStatus() { return { kvReady: true }; }
    async scriptError() { return ''; }
    async aiCfgGet() { return this.pendingCfg ? this.pendingCfg.promise : this.cfg; }
    async aiCfgSet(cfg) { if (this.failSave) throw new Error('KV failed'); this.cfg = cfg; }
}
const fake = new FakeDevice();
const session = new DeviceSession(fake);
const model = new WorkspaceModel(session);
assert.equal(model.getScript(), '');
assert.throws(() => model.edit('offline'));
await session.connect();
assert.equal(model.getScript(), '');
assert.throws(() => model.edit('too early'));
await model.read();
assert.equal(model.getScript(), 'device A');
model.edit('unsaved edit');
const oldEpoch = session.epoch;
fake.pendingRead = deferred();
const stale = assert.rejects(model.read());
await session.disconnect();
assert.equal(model.getScript(), 'unsaved edit');
assert.equal(model.getSnapshot().ready, false);
assert.throws(() => model.edit('offline overwrite'));
await session.connect();
fake.pendingRead.resolve('stale A'); await stale; fake.pendingRead = null;
fake.source = 'device B'; await model.read({ automatic: true });
assert.equal(model.getScript(), 'unsaved edit');
assert.equal(session.getSnapshot().ramSource, 'device B');
await model.read();
assert.equal(model.getScript(), 'device B');
model.edit('');
await session.disconnect(); await session.connect();
await model.read({ automatic: true });
assert.equal(model.getScript(), ''); // intentionally empty edits also survive
await model.read();
await session.disconnect(); await session.connect();
fake.source = 'fresh source'; await model.read({ automatic: true });
assert.equal(model.getScript(), 'fresh source');
assert.throws(() => model.edit('old AI result', '', oldEpoch));
model.dispose();

// Exercise real configuration initialization/migration against a fake device.
const original = deviceSession.device;
const board = new FakeDevice();
// Wire the singleton event handler while retaining the production session implementation.
board.changed = state => {
    if (state === 'connecting' || state === 'disconnected') { deviceSession.epoch++; deviceSession.operation = null; }
    deviceSession.update({ state, busy: '', aiCfg: null, aiReady: false, ramSource: null });
};
deviceSession.device = board;
await deviceSession.connect();
store.set('ds5_ai_cfg_v1', JSON.stringify({ apiKey: 'legacy', model: 'legacy model' }));
board.failSave = true;
await assert.rejects(initAiCfg(), /KV failed/);
assert.ok(store.has('ds5_ai_cfg_v1'));
assert.equal(deviceSession.getSnapshot().aiReady, false);
board.failSave = false;
await initAiCfg();
assert.equal(board.cfg.apiKey, 'legacy');
assert.equal(loadAiCfg().apiKey, 'legacy');
assert.equal(store.has('ds5_ai_cfg_v1'), false);
board.failSave = true;
await assert.rejects(saveAiCfg({ apiKey: 'failed' }), /KV failed/);
assert.equal(loadAiCfg().apiKey, 'legacy');
board.failSave = false;
await saveAiCfg({ apiKey: 'saved', model: 'new' });
assert.equal(board.cfg.apiKey, 'saved');
await deviceSession.disconnect();
assert.equal(loadAiCfg().apiKey, '');
await assert.rejects(saveAiCfg({ apiKey: 'offline' }));
await deviceSession.connect();
board.cfg = { apiKey: 'other board' };
store.set('ds5_ai_cfg_v1', JSON.stringify({ apiKey: 'must not override' }));
await initAiCfg();
assert.equal(loadAiCfg().apiKey, 'other board');
board.pendingCfg = deferred();
const staleCfg = assert.rejects(initAiCfg());
await deviceSession.disconnect(); await deviceSession.connect();
board.pendingCfg.resolve({ apiKey: 'old board secret' }); await staleCfg;
assert.equal(loadAiCfg().apiKey, '');
assert.equal(deviceSession.getSnapshot().aiReady, false);
board.pendingCfg = null;
board.cfg = [];
await assert.rejects(initAiCfg(), /格式/);
assert.equal(deviceSession.getSnapshot().aiReady, false);
await deviceSession.disconnect(); deviceSession.device = original;
console.log('PASS: authoritative device script, offline gates, reconnect races, AI KV migration/save/failure/session isolation');
