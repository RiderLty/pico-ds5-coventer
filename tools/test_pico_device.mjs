// Public API/lifecycle tests, including real WebUSB/Web Serial adapters with fake browser APIs.
import assert from "node:assert/strict";
import { webcrypto } from "node:crypto";
import { PicoDevice, PicoProtocol as P, PicoDeviceError } from "./pico-device.mjs";

const { CMD } = P;
const text = new TextEncoder();
const tick = () => new Promise((resolve) => setTimeout(resolve, 0));
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((ok, fail) => { resolve = ok; reject = fail; });
  return { promise, resolve, reject };
};

class Board {
  constructor() { this.commands = []; this.closed = 0; }
  async open() { this.opened = true; }
  async close() { this.opened = false; this.closed++; }
  async sendChunk(chunk) {
    if (chunk[0] & 1) {
      let total = 0, index = 4, scale = 1;
      while (true) {
        const b = chunk[index++];
        total += (b & 127) * scale;
        if (!(b & 128)) break;
        scale *= 128;
      }
      this.rx = { command: chunk[1], id: chunk[2] | chunk[3] << 8, total, body: [...chunk.subarray(index)] };
    } else this.rx.body.push(...chunk.subarray(1));
    if (!(chunk[0] & 2)) return;
    const m = this.rx;
    assert.equal(m.body.length, m.total);
    this.commands.push(m);
    if (this.hold) await this.hold.promise;
    if (this.silent) return;
    if (m.command === CMD.INFO) this.reply(m.command, m.id, text.encode("test board"));
    else if (m.command === CMD.RESET) this.reply(m.command, m.id);
    else if (m.command === CMD.TEST) {
      P.checkTestResponse(Uint8Array.from(m.body), m.total);
      this.reply(m.command, m.id, Uint8Array.from(m.body));
    } else if (m.command === CMD.SCRIPT && this.handleScript) this.handleScript(m);
    else if (m.command === 0x10) this.reply(m.command, m.id, Uint8Array.from(m.body));
    else this.reply(CMD.ERROR, m.id, Uint8Array.of(m.command, 1));
  }
  reply(command, id, body = new Uint8Array()) {
    const length = [];
    for (let n = body.length;; n >>>= 7) {
      length.push((n & 127) | (n > 127 ? 128 : 0));
      if (n <= 127) break;
    }
    let offset = 0;
    do {
      const header = offset === 0 ? [command, id & 255, id >>> 8, ...length] : [];
      const count = Math.min(body.length - offset, 61 - header.length);
      this.onchunk(Uint8Array.from([(offset === 0 ? 1 : 0) | (offset + count === body.length ? 2 : 0),
        ...header, ...body.subarray(offset, offset + count)]));
      offset += count;
    } while (offset < body.length);
  }
}

function setup(options = {}) {
  const boards = [];
  const device = new PicoDevice({ ...options, transportFactory: () => { const b = new Board(); boards.push(b); return b; } });
  return { device, boards };
}

// Connect/reset, typed views, snapshotting queued data, and transaction ordering.
{
  const { device, boards } = setup();
  const states = [];
  device.onStateChange((state) => states.push(state));
  assert.equal(await device.connect(), device);
  const [board] = boards;
  assert.equal(board.commands[0].command, CMD.RESET);
  assert.equal(device.connected, true);
  assert.equal(device.transportType, "winusb");
  await assert.rejects(device.connect(), { code: "STATE" });
  assert.equal(await device.info(), "test board");
  const bytes = Uint8Array.of(99, 1, 2, 3, 88);
  const result = device.call(0x10, new DataView(bytes.buffer, 1, 3));
  bytes.fill(0);
  assert.deepEqual(await result, Uint8Array.of(1, 2, 3));
  await assert.rejects(device.call(0x10, new Uint8Array(513)), RangeError);
  await assert.rejects(device.call(-1), TypeError);
  await assert.rejects(device.call(0x10, "text"), TypeError);
  await assert.rejects(device.send(CMD.TEST, new Uint8Array(4)), { code: "STATE" });

  // An INFO's 10ms timeout must not tick while a preceding request is held.
  board.hold = deferred();
  const first = device.call(0x10, Uint8Array.of(4));
  const second = device.info({ timeoutMs: 10 });
  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(board.commands.at(-1).command, 0x10);
  board.hold.resolve();
  assert.deepEqual(await first, Uint8Array.of(4));
  assert.equal(await second, "test board");
  await Promise.all([device.disconnect(), device.disconnect()]);
  assert.equal(board.closed, 1);
  assert.deepEqual(states, ["connecting", "connected", "disconnecting", "disconnected"]);
  await assert.rejects(device.info(), { code: "DISCONNECTED" });
}

// Message metadata, only broadcasts become logs, unsubscribe, isolated sync/async listeners.
{
  const { device, boards } = setup();
  await device.connect("serial");
  const [board] = boards;
  const logs = [], messages = [], errors = [];
  device.onMessage((m) => messages.push(m));
  const unsubscribe = device.on_log((s, m) => { logs.push(s); assert.equal(m.broadcast, true); });
  device.onError((error) => errors.push(error));
  const unthrow = device.onMessage(() => { throw new Error("listener failed"); });
  const unasync = device.onMessage(async () => { throw new Error("async listener failed"); });
  board.reply(CMD.PRINT, 65535, text.encode("你好"));
  await tick();
  assert.deepEqual(logs, ["你好"]);
  assert.equal(errors.length, 2);
  unthrow(); unasync(); unsubscribe();
  board.reply(CMD.PRINT, 65535, text.encode("unsubscribed"));
  board.reply(CMD.PRINT, 123, text.encode("late reply"));
  assert.equal(logs.length, 1);
  assert.equal(messages.at(-1).matched, false);
  assert.equal(messages.at(-1).broadcast, false);
  await assert.rejects(device.call(0x70), (error) => {
    assert.ok(error instanceof PicoDeviceError);
    assert.equal(error.deviceCode, 1);
    assert.equal(error.command, 0x70);
    assert.ok(error.messageId > 0 && error.messageId < 65535);
    return true;
  });
  assert.equal(await device.info(), "test board"); // Device errors keep the connection usable.
  const id = await device.send(0x10, Uint8Array.of(9));
  assert.equal(messages.at(-1).messageId, id);
  assert.equal(messages.at(-1).matched, false);
  await device.disconnect();
}

// Pre-aborted and queued cancellation never disturb an active transaction.
{
  const { device, boards } = setup({ maxQueue: 2 });
  await device.connect();
  const [board] = boards;
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(device.info({ signal: controller.signal }), { name: "AbortError" });
  board.hold = deferred();
  const active = device.info();
  const queuedController = new AbortController();
  const queued = device.info({ signal: queuedController.signal });
  const rejected = assert.rejects(queued, { name: "AbortError" });
  await assert.rejects(device.info(), { code: "QUEUE_FULL" });
  queuedController.abort();
  await rejected;
  board.hold.resolve();
  assert.equal(await active, "test board");
  assert.equal(board.commands.filter((m) => m.command === CMD.INFO).length, 1);
  assert.equal(device.connected, true);
  await device.disconnect();
}

// Active cancellation/timeout closes the session and rejects every queued call;
// reconnect gets a fresh transport and rejects any old callbacks.
for (const failure of ["timeout", "abort", "unplug"]) {
  const { device, boards } = setup();
  await device.connect();
  const [board] = boards;
  board.silent = true;
  const controller = new AbortController();
  const active = device.info({ timeoutMs: 15, signal: controller.signal });
  const next = device.info();
  const rejected = Promise.all([assert.rejects(active), assert.rejects(next)]);
  await tick();
  if (failure === "abort") controller.abort();
  if (failure === "unplug") board.onclose(new Error("unplug"));
  await rejected;
  await device.disconnect();
  assert.equal(device.state, "disconnected");
  assert.equal(board.closed, 1);
  await device.connect();
  board.onclose(new Error("old unplug"));
  assert.equal(await device.info(), "test board");
  await device.disconnect();
}

// A stalled write times out too, and cannot retain pending callers forever.
{
  const { device, boards } = setup();
  await device.connect();
  boards[0].sendChunk = () => new Promise(() => {});
  await assert.rejects(device.call(0x10, new Uint8Array(), { timeoutMs: 10 }), { code: "TIMEOUT" });
  await device.disconnect();
}

// Disconnect releases a send timeout even if the underlying write never settles.
{
  const { device, boards } = setup();
  await device.connect();
  boards[0].sendChunk = () => new Promise(() => {});
  const sending = device.send(0x10, new Uint8Array(), { timeoutMs: 10000 });
  const rejected = assert.rejects(sending, { code: "DISCONNECTED" });
  await tick();
  await device.disconnect();
  await rejected; // Also releases the internal send timeout, despite a hung driver.
}

// Browser pickers/opening can finish after disconnect; resources must close again.
{
  const board = new Board(), opening = deferred();
  board.open = async () => { await opening.promise; board.opened = true; };
  const device = new PicoDevice({ transportFactory: () => board });
  const connecting = device.connect();
  const rejected = assert.rejects(connecting, { code: "DISCONNECTED" });
  await device.disconnect();
  opening.resolve();
  await rejected;
  assert.equal(board.opened, false);
  assert.equal(device.state, "disconnected");
}

// Small sizes straddle chunk headers/CRC trailers/varint boundaries; the other
// suite streams all 32KiB..8MiB sizes through the production serial codec.
{
  if (!globalThis.crypto) Object.defineProperty(globalThis, "crypto", { value: webcrypto });
  const { device } = setup();
  await device.connect();
  for (const size of [4, 5, 60, 61, 62, 63, 64, 127, 128, 255, 256, 257, 513, 32768]) {
    const result = await device.test(size);
    assert.equal(result.body.length, size);
    assert.equal(result.uploadCrc, result.downloadCrc);
    assert.ok(result.durationMs >= 0);
  }
  await assert.rejects(device.test(3), RangeError);
  await assert.rejects(device.test(P.MAX_TEST + 1), RangeError);
  await device.disconnect();
}

// Independent SLIP framing helpers for fake USB/serial streams.
function encode(chunk) {
  const raw = [0, ...chunk];
  const crc = P.crc16(raw);
  raw.push(crc & 255, crc >>> 8);
  return Uint8Array.from([192, ...raw.flatMap((b) => b === 192 ? [219, 220] : b === 219 ? [219, 221] : [b]), 192]);
}
function wireBoard(push) {
  const board = new Board();
  board.onchunk = (chunk) => push(encode(chunk));
  return { board, accept: async (bytes) => {
    const raw = [];
    let escaped = false;
    for (const b of bytes) {
      if (b === 192) continue;
      if (b === 219) { escaped = true; continue; }
      raw.push(escaped ? (b === 220 ? 192 : 219) : b);
      escaped = false;
    }
    const crc = raw.pop() << 8 | raw.pop();
    assert.equal(P.crc16(raw), crc);
    await board.sendChunk(Uint8Array.from(raw.slice(1)));
  } };
}

const usbEvents = new EventTarget();
const navigatorMock = {
  usb: { addEventListener: usbEvents.addEventListener.bind(usbEvents), removeEventListener: usbEvents.removeEventListener.bind(usbEvents) },
  serial: {},
};
Object.defineProperty(globalThis, "navigator", { configurable: true, value: navigatorMock });
assert.deepEqual(PicoDevice.supported, { winusb: true, serial: true });

// Web Serial must release stream locks on close, including an ended read loop.
{
  let input, portClosed = 0;
  const { board, accept } = wireBoard((bytes) => input.enqueue(bytes));
  const port = {
    async open(options) {
      assert.equal(options.baudRate, 115200);
      this.readable = new ReadableStream({ start(controller) { input = controller; } });
      this.writable = new WritableStream({ write: accept });
    },
    async close() {
      assert.equal(this.readable.locked, false);
      assert.equal(this.writable.locked, false);
      portClosed++;
    },
  };
  const device = new PicoDevice();
  await device.connect("serial", { port, baudRate: 115200 });
  assert.equal(await device.info(), "test board");
  await device.test(513);
  assert.equal(board.commands[0].command, CMD.RESET);
  input.close();
  await tick();
  await device.disconnect();
  assert.equal(portClosed, 1);
}

// WebUSB: choose an alternate with bulk endpoints, preserve DataView offsets,
// validate short writes, clean up after interface selection failures.
function fakeUsb() {
  const waiting = [], incoming = [];
  const { accept } = wireBoard((bytes) => {
    const backing = new Uint8Array(bytes.length + 4);
    backing.set(bytes, 2);
    const result = { status: "ok", data: new DataView(backing.buffer, 2, bytes.length) };
    if (waiting.length) waiting.shift().resolve(result);
    else incoming.push(result);
  });
  return {
    closed: 0,
    configuration: { interfaces: [{ interfaceNumber: 6, alternate: { alternateSetting: 0 }, alternates: [
      { alternateSetting: 0, interfaceClass: 3, endpoints: [] },
      { alternateSetting: 1, interfaceClass: 255, endpoints: [
        { type: "bulk", direction: "in", endpointNumber: 8 },
        { type: "bulk", direction: "out", endpointNumber: 8 },
      ] },
    ] }] },
    async open() {},
    async claimInterface(i) { assert.equal(i, 6); },
    async selectAlternateInterface(i, a) { assert.equal(i, 6); assert.equal(a, 1); this.selected = true; },
    async transferIn() {
      if (incoming.length) return incoming.shift();
      const d = deferred(); waiting.push(d); return d.promise;
    },
    async transferOut(ep, bytes) {
      assert.equal(ep, 8);
      if (this.shortWrite) return { status: "ok", bytesWritten: bytes.length - 1 };
      await accept(bytes);
      return { status: "ok", bytesWritten: bytes.length };
    },
    async close() { this.closed++; for (const d of waiting.splice(0)) d.reject(new Error("closed")); },
  };
}
{
  const usb = fakeUsb(), device = new PicoDevice();
  await device.connect("winusb", { device: usb });
  assert.equal(device.usbDevice, usb);
  assert.equal(usb.selected, true);
  assert.equal(await device.info(), "test board");
  await device.test(513);
  usb.shortWrite = true;
  await assert.rejects(device.info(), { code: "TRANSFER" });
  await device.disconnect();
  assert.equal(usb.closed, 1);
  assert.equal(device.usbDevice, null);
}
{
  const usb = fakeUsb(), device = new PicoDevice();
  usb.configuration.interfaces = [];
  await assert.rejects(device.connect("winusb", { device: usb }), { code: "INTERFACE" });
  assert.equal(usb.closed, 1);
  assert.equal(device.state, "disconnected");
}
// Script helpers exercise real chunking, UTF-8, transactions and error matching.
class ScriptBoard extends Board {
  constructor() {
    super();
    this.source = text.encode("-- embedded default");
    this.ai = new Uint8Array();
    this.persisted = null;
    this.staged = null;
    this.error = "";
  }
  handleScript(m) {
    const body = Uint8Array.from(m.body), view = new DataView(body.buffer);
    let reply = new Uint8Array();
    switch (body[0]) {
      case 0:
        reply = text.encode(JSON.stringify({size: this.source.length, crc32: P.crc32(this.source), maxSize: 61440}));
        break;
      case 13:
      case 1:
        assert.equal(body.length, 9);
        this.staged = new Uint8Array(view.getUint32(1, true));
        this.expectedCrc = view.getUint32(5, true);
        this.received = 0;
        break;
      case 2: {
        assert.equal(view.getUint32(1, true), this.received);
        assert.ok(body.length >= 6 && body.length <= 512);
        this.staged.set(body.subarray(5), this.received);
        this.received += body.length - 5;
        break;
      }
      case 3:
        assert.equal(this.received, this.staged.length);
        assert.equal(P.crc32(this.staged), this.expectedCrc);
        if (this.failRun) {
          this.error = "script:1: syntax error";
          this.reply(CMD.ERROR, m.id, Uint8Array.of(CMD.SCRIPT, 5, ...text.encode(this.error)));
          return;
        }
        this.source = this.staged;
        this.staged = null;
        break;
      case 4: this.persisted = this.source.slice(); break;
      case 5:
        assert.equal(body.length, 7);
        reply = this.source.slice(view.getUint32(1, true), view.getUint32(1, true) + view.getUint16(5, true));
        if (this.corruptRead && reply.length) reply[0] ^= 1;
        break;
      case 6: this.source = text.encode("-- embedded default"); break;
      case 7: this.persisted = null; break;
      case 8: this.staged = null; break;
      case 9: assert.ok(this.persisted); this.source = this.persisted.slice(); break;
      case 10: reply = text.encode(this.error); break;
      case 11: reply = text.encode(JSON.stringify({size: this.ai.length, crc32: P.crc32(this.ai)})); break;
      case 12:
        reply = this.ai.slice(view.getUint32(1, true), view.getUint32(1, true) + view.getUint16(5, true));
        if (this.corruptAiRead && reply.length) reply[0] ^= 1;
        break;
      case 14:
        assert.equal(this.received, this.staged.length);
        assert.equal(P.crc32(this.staged), this.expectedCrc);
        this.ai = this.staged; this.staged = null; break;
      default: assert.fail("Unexpected script operation");
    }
    this.reply(CMD.SCRIPT, m.id, reply);
  }
}
for (const transport of ["winusb", "serial"]) {
  const board = new ScriptBoard();
  const device = new PicoDevice({transportFactory: () => board});
  await device.connect(transport);
  const initial = await device.scriptRead();
  assert.equal(await device.aiCfgGet(), null);
  const cfg = { apiKey: '秘密'.repeat(300), model: 'test', maxRounds: 12 };
  await device.aiCfgSet(cfg);
  assert.deepEqual(await device.aiCfgGet(), cfg);
  assert.equal(await device.scriptRead(), initial);
  await assert.rejects(device.aiCfgSet({ apiKey: '中'.repeat(1400) }), RangeError);
  board.corruptAiRead = true;
  await assert.rejects(device.aiCfgGet(), { code: 'SCRIPT' });
  board.corruptAiRead = false;
  for (const source of ["-- 你好\nmarker=1", "marker=2".padEnd(61440, " ")]) {
    // SAVE cannot interleave the BEGIN/WRITE/RUN sequence.
    const [status] = await Promise.all([device.scriptRun(source), device.scriptSave()]);
    assert.equal(status.size, text.encode(source).length);
    assert.equal(await device.scriptRead(), source);
    assert.equal(new TextDecoder().decode(board.persisted), source);
  }
  await device.scriptDefault();
  assert.equal(await device.scriptRead(), initial);
  await device.scriptReload();
  assert.equal((await device.scriptRead()).length, 61440);
  await device.scriptDelete();
  assert.equal(board.persisted, null);
  assert.deepEqual(await device.aiCfgGet(), cfg);
  const commands = board.commands.length;
  await assert.rejects(device.scriptRun(""), RangeError);
  await assert.rejects(device.scriptRun("中".repeat(20481)), RangeError);
  assert.equal(board.commands.length, commands);
  board.failRun = true;
  await assert.rejects(device.scriptRun("bad"), (error) => {
    assert.ok(error instanceof PicoDeviceError);
    assert.equal(error.deviceCode, 5);
    assert.equal(error.detail, "script:1: syntax error");
    return true;
  });
  assert.equal(board.staged, null); // error cleanup sends ABORT
  assert.equal(await device.scriptError(), "script:1: syntax error");
  board.corruptRead = true;
  await assert.rejects(device.scriptRead(), {code: "SCRIPT"});
  assert.equal(device.connected, true);
  await device.disconnect();
}
// Queued multi-request work must never migrate to a reconnected device.
{
  const boards = [];
  const device = new PicoDevice({transportFactory: () => { const b = new ScriptBoard(); boards.push(b); return b; }});
  await device.connect();
  boards[0].hold = deferred();
  const running = device.scriptRun("marker=42");
  const saving = device.scriptSave();
  const rejected = Promise.all([assert.rejects(running, {code: "DISCONNECTED"}), assert.rejects(saving, {code: "DISCONNECTED"})]);
  await tick();
  await device.disconnect();
  await device.connect();
  boards[0].hold.resolve();
  await rejected;
  assert.deepEqual(boards[1].commands.map(m => m.command), [CMD.RESET]);
  assert.ok((await device.scriptStatus()).size > 0);
  await device.disconnect();
}
console.log("PASS: PicoDevice API, queue/abort/timeout/reconnect, notifications, small boundaries, script API/60KiB/UTF-8/errors, WebUSB/Web Serial lifecycle");
