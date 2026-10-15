(() => {

"use strict";

// Browser global build. Also loaded by pico-device.mjs for ES module imports.

// ── 协议常量（与 src/proto.h 一致）──────────────────────────────────
const VID = 0x054c;
const CHUNK_MAX = 62;                     // [flags][data≤61]，与 PROTO_CHUNK_DATA_MAX 一致
const F_START = 0x01, F_END = 0x02, F_RESET = 0x04;
const CMD = { INFO: 0x01, PRINT: 0x02, RESET: 0x03, TEST: 0x04, SCRIPT: 0x20, ERROR: 0x7F };
const CMD_NAME = { [CMD.INFO]: "INFO", [CMD.PRINT]: "PRINT", [CMD.RESET]: "RESET", [CMD.TEST]: "TEST", [CMD.SCRIPT]: "SCRIPT", [CMD.ERROR]: "ERROR" };
const ID_RESERVED = 0x0000, ID_BROADCAST = 0xFFFF;
const MAX_BODY = 512, MAX_TEST = 8 * 1024 * 1024;
const ERROR_NAME = {1: "不支持的命令", 2: "消息长度非法", 3: "CRC32 校验失败", 4: "设备忙", 5: "脚本操作失败", 6: "KV 存储失败"};

const dec = new TextDecoder("utf-8");

function varintPut(v) {
  const out = [];
  while (v >= 0x80) { out.push((v & 0x7F) | 0x80); v >>>= 7; }
  out.push(v);
  return out;
}

function crc16(bytes) {
  let crc = 0xFFFF;
  for (const b of bytes) {
    crc ^= b << 8;
    for (let i = 0; i < 8; i++) {
      crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
    }
  }
  return crc;
}

// zlib CRC32（与设备端一致）
function crc32(bytes) {
  let crc = 0xFFFFFFFF;
  for (let i = 0; i < bytes.length; i++) {
    crc ^= bytes[i];
    for (let b = 0; b < 8; b++) {
      crc = (crc & 1) ? ((crc >>> 1) ^ 0xEDB88320) : (crc >>> 1);
    }
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

function checkTestResponse(resp, size) {
  if (resp.length !== size || size < 4) throw new Error(`回包长度 ${resp.length} ≠ ${size}`);
  // 位运算的 | 返回有符号 int32；DataView 始终返回无符号 uint32。
  const crcRecv = new DataView(resp.buffer, resp.byteOffset, resp.byteLength)
    .getUint32(size - 4, true);
  if (crc32(resp.subarray(0, size - 4)) !== crcRecv) throw new Error("回包 CRC32 不匹配");
  return crcRecv;
}

// ── SLIP 解码器（帧 = 0xC0 <转义 [seq][flags][data] + CRC16> 0xC0，与
//    设备端 uart_proto.c / usb_io.c 一致）─────────────────────────────
const SLIP_END = 0xC0, SLIP_ESC = 0xDB, SLIP_ESC_END = 0xDC, SLIP_ESC_ESC = 0xDD;

class SlipDecoder {
  constructor(onchunk, onframeCrcFail) {
    this.onchunk = onchunk;           // (chunk=[flags][data...]) => void
    this.onframeCrcFail = onframeCrcFail;
    this.inFrame = false;
    this.escape = false;
    this.buf = [];
  }

  byte(b) {
    if (b === SLIP_END) {
      if (this.inFrame && this.buf.length > 0) this._finish();
      this.inFrame = false;
      this.escape = false;
      this.buf = [];
      return;
    }
    if (!this.inFrame) {
      this.inFrame = true;
      this.escape = false;
      this.buf = [];
    }
    if (this.escape) {
      this.escape = false;
      if (b === SLIP_ESC_END) b = SLIP_END;
      else if (b === SLIP_ESC_ESC) b = SLIP_ESC;
      else { this.inFrame = false; return; }   // 非法转义，重同步
    } else if (b === SLIP_ESC) {
      this.escape = true;
      return;
    }
    if (this.buf.length >= 256) { this.inFrame = false; return; }   // 超长，重同步
    this.buf.push(b);
  }

  _finish() {
    const len = this.buf.length;
    if (len < 4) return;
    const body = this.buf.slice(0, len - 2);
    const crcRecv = this.buf[len - 2] | (this.buf[len - 1] << 8);
    if (crc16(body) !== crcRecv) {
      this.onframeCrcFail?.(len);
      return;
    }
    this.onchunk(Uint8Array.from(body.slice(1)));   // 剥 seq → [flags][data...]
  }
}

// Both transports use exactly the same on-wire framing.
function encodeFrame(seq, chunk) {
  const raw = [seq & 255, ...chunk];
  const crc = crc16(raw);
  raw.push(crc & 255, crc >>> 8);
  const frame = [SLIP_END];
  for (const b of raw) {
    if (b === SLIP_END) frame.push(SLIP_ESC, SLIP_ESC_END);
    else if (b === SLIP_ESC) frame.push(SLIP_ESC, SLIP_ESC_ESC);
    else frame.push(b);
  }
  frame.push(SLIP_END);
  return Uint8Array.from(frame);
}

class WinUsbTransport {
  constructor(onchunk, onclose, onerror) {
    Object.assign(this, { onchunk, onclose, onerror, seq: 0, closing: false, name: "winusb" });
    this.slip = new SlipDecoder(
      (chunk) => this.onchunk?.(chunk),
      () => this.onerror?.(new PicoError("链路 CRC16 校验失败，帧已丢弃", "FRAME_CRC")));
  }

  async open(options = {}) {
    if (!globalThis.navigator?.usb) throw new PicoError("当前浏览器不支持 WebUSB", "UNSUPPORTED");
    this.device = options.device ?? await navigator.usb.requestDevice({
      filters: options.filters ?? [{ vendorId: VID }],
    });
    await this.device.open();
    this.opened = true;
    if (!this.device.configuration) await this.device.selectConfiguration(options.configurationValue ?? 1);
    let alternate;
    const itf = this.device.configuration.interfaces.find((i) => {
      if (options.interfaceNumber !== undefined && i.interfaceNumber !== options.interfaceNumber) return false;
      alternate = i.alternates.find((a) => a.interfaceClass === 255 &&
        a.endpoints.some((e) => e.type === "bulk" && e.direction === "in") &&
        a.endpoints.some((e) => e.type === "bulk" && e.direction === "out"));
      return !!alternate;
    });
    if (!itf) throw new PicoError("未找到带 bulk IN/OUT 的 WinUSB 接口", "INTERFACE");
    this.interfaceNumber = itf.interfaceNumber;
    await this.device.claimInterface(this.interfaceNumber);
    if (itf.alternate?.alternateSetting !== alternate.alternateSetting) {
      await this.device.selectAlternateInterface(this.interfaceNumber, alternate.alternateSetting);
    }
    this.epIn = alternate.endpoints.find((e) => e.type === "bulk" && e.direction === "in").endpointNumber;
    this.epOut = alternate.endpoints.find((e) => e.type === "bulk" && e.direction === "out").endpointNumber;
    this.disconnectListener = (event) => {
      if (event.device === this.device && !this.closing) this.onclose?.(new PicoError("USB 设备已拔出", "DISCONNECTED"));
    };
    navigator.usb.addEventListener("disconnect", this.disconnectListener);
    this.readTask = this._readLoop().catch((e) => { if (!this.closing) this.onclose?.(e); });
  }

  async close() {
    this.closing = true;
    if (this.disconnectListener) navigator.usb.removeEventListener("disconnect", this.disconnectListener);
    // close() aborts outstanding transfers; releaseInterface() can wait on them.
    if (this.opened) {
      try { await this.device.close(); } finally { this.opened = false; }
    }
    await this.readTask;
  }

  async sendChunk(chunk) {
    const frame = encodeFrame(this.seq++, chunk);
    const result = await this.device.transferOut(this.epOut, frame);
    if (result.status !== "ok" || result.bytesWritten !== frame.length) {
      throw new PicoError("USB 写入失败或只写入部分帧", "TRANSFER");
    }
  }

  async _readLoop() {
    while (!this.closing) {
      const result = await this.device.transferIn(this.epIn, 64);
      if (this.closing) return;
      if (result.status !== "ok") throw new PicoError(`USB 读取失败：${result.status}`, "TRANSFER");
      if (result.data) {
        for (const b of new Uint8Array(result.data.buffer, result.data.byteOffset, result.data.byteLength)) this.slip.byte(b);
      }
    }
  }
}

class SerialTransport {
  constructor(onchunk, onclose, onerror) {
    Object.assign(this, { onchunk, onclose, onerror, seq: 0, closing: false, name: "serial" });
    this.slip = new SlipDecoder(
      (chunk) => this.onchunk?.(chunk),
      () => this.onerror?.(new PicoError("链路 CRC16 校验失败，帧已丢弃", "FRAME_CRC")));
  }

  async open(options = {}) {
    if (!globalThis.navigator?.serial) throw new PicoError("当前浏览器不支持 Web Serial", "UNSUPPORTED");
    this.port = options.port ?? await navigator.serial.requestPort({ filters: options.filters ?? [] });
    await this.port.open({ baudRate: options.baudRate ?? 4000000, dataBits: 8,
      stopBits: 1, parity: "none", flowControl: "none", bufferSize: 65536 });
    this.opened = true;
    this.writer = this.port.writable.getWriter();
    this.readTask = this._readLoop().catch((e) => { if (!this.closing) this.onclose?.(e); });
  }

  async close() {
    this.closing = true;
    try { await this.reader?.cancel(); } catch (_) { /* unplugged */ }
    await this.readTask;
    try { await this.writer?.abort(); } catch (_) { /* unplugged */ }
    this.writer?.releaseLock();
    if (this.opened) {
      try { await this.port.close(); } finally { this.opened = false; }
    }
  }

  async sendChunk(chunk) { await this.writer.write(encodeFrame(this.seq++, chunk)); }

  async _readLoop() {
    this.reader = this.port.readable.getReader();
    try {
      while (!this.closing) {
        const { value, done } = await this.reader.read();
        if (done || this.closing) break;
        for (const b of value) this.slip.byte(b);
      }
    } finally {
      this.reader.releaseLock();
      this.reader = null;
    }
    if (!this.closing) this.onclose?.(new PicoError("串口读取结束", "DISCONNECTED"));
  }
}

// ── PicoIO：消息层（分块/重组/request），对传输无感 ───────────────────
class ProtocolClient {
  constructor(transport) {
    this.transport = transport;
    this.onmessage = null;
    this.onchunk = null;      // 块计数钩子（UI 统计用）
    this.onclose = null;
    this.msgSeq = 0;
    this.pending = new Map();
    this.issuedIds = new Set();
    this.closed = false;
    this.txQueue = Promise.resolve();
    this.rx = null;
    transport.onchunk = (chunk) => {
      this.onchunk?.("rx", chunk);
      this._onChunk(chunk);
    };
    transport.onclose = (e) => this.close(e);
  }

  close(e = new Error("设备断开")) {
    if (this.closed) return;
    this.closed = true;
    this.rx = null;
    for (const p of this.pending.values()) {
      p.cleanup();
      p.reject(e);
    }
    this.pending.clear();
    this.onclose?.(e);
  }

  _allocateId() {
    if (this.closed) throw new Error("连接已关闭");
    // 同一会话不复用编号，迟到/重复回复不能命中新请求。
    for (let i = 0; i < ID_BROADCAST - 1; i++) {
      this.msgSeq = this.msgSeq % (ID_BROADCAST - 1) + 1;
      if (!this.issuedIds.has(this.msgSeq)) {
        this.issuedIds.add(this.msgSeq);
        return this.msgSeq;
      }
    }
    throw new Error("本会话请求编号已用完，请重新连接");
  }

  async send(cmd, body = new Uint8Array(0)) {
    const msgid = this._allocateId();
    await this._queueSend(cmd, body, msgid);
    return msgid;
  }

  _queueSend(cmd, body, msgid, isActive = () => true) {
    // 一个消息的全部块连续发送；INFO 与压测不能交错写入同一重组器。
    const job = this.txQueue.then(() => this._send(cmd, body, msgid, isActive));
    this.txQueue = job.catch(() => {});
    return job;
  }

  async _send(cmd, body, msgid, isActive) {
    const max = cmd === CMD.TEST ? MAX_TEST : MAX_BODY;
    if (body.length > max || (cmd === CMD.TEST && body.length < 4)) {
      throw new Error(`请求长度非法（上限 ${max}B）`);
    }
    const header = [cmd, msgid & 0xFF, (msgid >> 8) & 0xFF, ...varintPut(body.length)];
    let sent = 0;
    for (;;) {
      if (this.closed || !isActive()) throw new Error("请求已结束，停止发送");
      const cap = CHUNK_MAX - 1 - (sent === 0 ? header.length : 0);
      const n = Math.min(body.length - sent, cap);
      const chunk = [];
      const flags = (sent === 0 ? F_START : 0) | (sent + n >= body.length ? F_END : 0);
      chunk.push(flags);
      if (sent === 0) chunk.push(...header);
      chunk.push(...body.subarray(sent, sent + n));

      await this.transport.sendChunk(Uint8Array.from(chunk));
      this.onchunk?.("tx", chunk);
      sent += n;
      if (sent >= body.length) break;
    }
  }

  request(cmd, body = new Uint8Array(0), timeoutMs = 2000, signal) {
    if (signal?.aborted) return Promise.reject(abortError());
    let msgid;
    try { msgid = this._allocateId(); }
    catch (e) { return Promise.reject(e); }
    return new Promise((resolve, reject) => {
      // Register before writing: a reply may precede write() resolution.
      const cancel = (error) => {
        const p = this.pending.get(msgid);
        if (!p) return;
        p.cleanup();
        this.pending.delete(msgid);
        if (this.rx?.msgid === msgid) this.rx = null;
        reject(error);
      };
      const onAbort = () => cancel(abortError());
      const timer = setTimeout(() => cancel(new PicoError(`请求 ${CMD_NAME[cmd] ?? cmd} 超时`, "TIMEOUT")), timeoutMs);
      const cleanup = () => { clearTimeout(timer); signal?.removeEventListener("abort", onAbort); };
      this.pending.set(msgid, { cmd, resolve, reject, cleanup });
      signal?.addEventListener("abort", onAbort, { once: true });
      this._queueSend(cmd, body, msgid, () => this.pending.has(msgid)).catch(cancel);
    });
  }

  _abortRx(reason) {
    const m = this.rx;
    this.rx = null;
    if (!m) return;
    const p = this.pending.get(m.msgid);
    if (p && (p.cmd === m.cmd || m.cmd === CMD.ERROR)) {
      p.cleanup();
      this.pending.delete(m.msgid);
      p.reject(new PicoError(reason, "PROTOCOL"));
    }
  }

  _onChunk(chunk) {
    if (this.closed || chunk.length === 0) return;
    const flags = chunk[0];
    const data = chunk.subarray(1);
    if (chunk.length > CHUNK_MAX || (flags & ~(F_START | F_END | F_RESET))) {
      this._abortRx("非法消息块");
      return;
    }
    if (flags & F_RESET) {
      this._abortRx("设备复位了消息接收状态");
      return;
    }

    if (flags & F_START) {
      this._abortRx("回包被下一条消息中断");
      if (data.length < 4) return;
      const cmd = data[0];
      const msgid = data[1] | (data[2] << 8);
      if (msgid === ID_RESERVED) return;
      let len = 0, shift = 0, i = 3, valid = false;
      for (; i < data.length && shift < 28; i++, shift += 7) {
        len += (data[i] & 0x7F) * 2 ** shift;
        if (!(data[i] & 0x80)) { i++; valid = true; break; }
      }
      // 先检查再分配，异常帧不能导致无限制内存分配。
      const max = cmd === CMD.TEST && msgid !== ID_BROADCAST ? MAX_TEST : MAX_BODY;
      this.rx = {cmd, msgid, total: len, got: 0};
      if (!valid || len > max || (cmd === CMD.TEST && len < 4)) {
        this._abortRx("回包长度头非法或超出上限");
        return;
      }
      this.rx.buf = new Uint8Array(len);
      if (!this._feed(data.subarray(i))) return;
    } else if (this.rx) {
      if (!this._feed(data)) return;
    }

    if (this.rx && (flags & F_END)) {
      const m = this.rx;
      if (m.got !== m.total) {
        this._abortRx(`回包不完整：${m.got}/${m.total}B`);
        return;
      }
      this.rx = null;
      const body = m.buf;
      const p = this.pending.get(m.msgid);
      const errorReply = m.cmd === CMD.ERROR && body.length >= 2 && body[0] === p?.cmd;
      if (m.msgid !== ID_BROADCAST && p && (p.cmd === m.cmd || errorReply)) {
        p.cleanup();
        this.pending.delete(m.msgid);
        if (errorReply) p.reject(new PicoDeviceError(p.cmd, body[1], m.msgid, dec.decode(body.subarray(2))));
        else p.resolve(body);
      }
      // 主动推送与请求回复分开分发，迟到回复不会冒充日志事件。
      this.onmessage?.(m.cmd, body, m.msgid, !!p && (p.cmd === m.cmd || errorReply));
    }
  }

  _feed(data) {
    if (data.length > this.rx.total - this.rx.got) {
      this._abortRx("回包数据超出声明长度");
      return false;
    }
    this.rx.buf.set(data, this.rx.got);
    this.rx.got += data.length;
    return true;
  }
}

class PicoError extends Error {
  constructor(message, code, options) {
    super(message, options);
    this.name = "PicoError";
    this.code = code;
  }
}

class PicoDeviceError extends PicoError {
  constructor(command, deviceCode, messageId, detail = "") {
    super(`设备拒绝 ${CMD_NAME[command] ?? command}：${ERROR_NAME[deviceCode] ?? deviceCode}${detail ? `：${detail}` : ""}`, "DEVICE");
    this.name = "PicoDeviceError";
    Object.assign(this, { command, deviceCode, messageId, detail });
  }
}

function abortError() {
  const error = new PicoError("操作已取消", "ABORTED");
  error.name = "AbortError";
  return error;
}

function bytesOf(body) {
  if (body instanceof ArrayBuffer) return new Uint8Array(body);
  if (ArrayBuffer.isView(body)) return new Uint8Array(body.buffer, body.byteOffset, body.byteLength);
  throw new TypeError("body 必须是 ArrayBuffer 或 TypedArray/DataView");
}

function validateRequest(command, body) {
  if (!Number.isInteger(command) || command < 0 || command > 255) throw new TypeError("command 必须是 0..255 整数");
  const max = command === CMD.TEST ? MAX_TEST : MAX_BODY;
  if (body.length > max || (command === CMD.TEST && body.length < 4)) throw new RangeError(`请求长度非法（上限 ${max}B）`);
}

/** Transport-independent browser API. See docs/pico-device.md. */
class PicoDevice {
  constructor({ timeoutMs = 2000, maxQueue = 32, transportFactory } = {}) {
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0 || timeoutMs > 0x7fffffff) throw new RangeError("timeoutMs 非法");
    if (!Number.isInteger(maxQueue) || maxQueue < 1) throw new RangeError("maxQueue 非法");
    this.timeoutMs = timeoutMs;
    this.maxQueue = maxQueue;
    this._transportFactory = transportFactory ?? ((kind) => kind === "winusb" ? new WinUsbTransport() : new SerialTransport());
    this._state = "disconnected";
    this._session = null;
    this._connecting = false;
    this._listeners = new Map();
  }

  get state() { return this._state; }
  get connected() { return this.state === "connected"; }
  get transportType() { return this._session?.kind ?? null; }
  static get supported() {
    return { winusb: !!globalThis.navigator?.usb, serial: !!globalThis.navigator?.serial };
  }

  /** Call from a click handler so the browser can show its device picker. */
  async connect(kind = "winusb", options = {}) {
    if (kind !== "winusb" && kind !== "serial") throw new TypeError('连接类型必须是 "winusb" 或 "serial"');
    if (this._connecting || this.state !== "disconnected") throw new PicoError("请先断开当前连接", "STATE");
    this._connecting = true;
    const session = { kind, queue: Promise.resolve(), jobs: new Set(), closed: false };
    this._session = session;
    this._setState("connecting");
    try {
      const transport = this._transportFactory(kind);
      session.transport = transport;
      const protocol = new ProtocolClient(transport);
      session.protocol = protocol;
      protocol.onmessage = (command, body, messageId, matched) => {
        if (this._session === session && !session.closed) this.__on_message({ command, body, messageId,
          broadcast: messageId === ID_BROADCAST, matched });
      };
      protocol.onchunk = (direction, chunk) => {
        if (this._session === session && !session.closed) this._emit("chunk", { direction, bytes: chunk.length });
      };
      protocol.onclose = (error) => { void this._disconnect(error, session); };
      transport.onerror = (error) => { if (!session.closed) this._emit("error", error); };
      await transport.open(options);
      if (session.closed || this._session !== session) {
        // A browser picker cannot be dismissed programmatically. Close anything
        // it opens after disconnect(), without reviving the old session.
        await transport.close();
        throw new PicoError("连接已取消", "DISCONNECTED");
      }
      // Clear stale device RX/TX state before exposing a usable connection.
      if (options.reset !== false) await protocol.request(CMD.RESET, new Uint8Array(), this.timeoutMs);
      if (session.closed) throw new PicoError("连接已取消", "DISCONNECTED");
      this._setState("connected");
      return this;
    } catch (error) {
      await this._disconnect(error, session);
      throw error;
    } finally {
      this._connecting = false;
    }
  }

  async disconnect() { await this._disconnect(null, this._session); }

  async _disconnect(error, session) {
    if (!session) return;
    if (session.closed) return session.closeTask;
    session.closed = true;
    // Install the promise before invoking any subscriber (which may reenter).
    let finish;
    session.closeTask = new Promise((resolve) => { finish = resolve; });
    if (this._session === session) this._setState("disconnecting");
    const reason = error ?? new PicoError("设备已断开", "DISCONNECTED");
    for (const job of [...session.jobs]) job.finish(reason);
    session.protocol?.close(reason);
    try { await session.transport?.close(); }
    catch (closeError) { this._emit("error", closeError); }
    if (this._session === session) {
      this._session = null;
      this._setState("disconnected");
      if (error) this._emit("error", error);
      this._emit("close", error);
    }
    finish();
  }

  /** Resolves with a Uint8Array body. Requests run one at a time, through reply. */
  async call(command, body = new Uint8Array(), options = {}) {
    return this._enqueue(command, body, options, true);
  }

  /** Resolves with the assigned message ID after writing, without waiting for a reply. */
  async send(command, body = new Uint8Array(), options = {}) {
    if (command === CMD.RESET || command === CMD.TEST) {
      throw new PicoError("RESET/TEST 请使用 call() 等待完成后再发送下一条消息", "STATE");
    }
    return this._enqueue(command, body, options, false);
  }

  _enqueue(command, body, { timeoutMs = this.timeoutMs, signal } = {}, expectReply) {
    const session = this._session;
    if (!this.connected || !session || session.closed) return Promise.reject(new PicoError("设备未连接或连接已关闭", "DISCONNECTED"));
    body = bytesOf(body);
    validateRequest(command, body);
    if (!Number.isFinite(timeoutMs) || timeoutMs <= 0 || timeoutMs > 0x7fffffff) throw new RangeError("timeoutMs 非法");
    if (signal?.aborted) return Promise.reject(abortError());
    if (session.jobs.size >= this.maxQueue) return Promise.reject(new PicoError("请求队列已满", "QUEUE_FULL"));
    body = body.slice(); // Snapshot caller-owned data before yielding to the queue.
    return new Promise((resolve, reject) => {
      const job = { started: false, settled: false };
      const onAbort = () => {
        if (job.settled) return;
        const error = abortError();
        job.finish(error);
        // No on-wire CANCEL exists. A partial active request/reply needs a fresh
        // connection; queued cancellation never touches the transport.
        if (job.started) void this._disconnect(error, session);
      };
      job.finish = (error, value) => {
        if (job.settled) return;
        job.settled = true;
        session.jobs.delete(job);
        signal?.removeEventListener("abort", onAbort);
        if (error) job.cancelSend?.(error);
        error ? reject(error) : resolve(value);
      };
      session.jobs.add(job);
      signal?.addEventListener("abort", onAbort, { once: true });
      const run = async () => {
        if (job.settled || session.closed) return;
        job.started = true;
        let timer;
        try {
          let result;
          if (expectReply) {
            result = await this.__send_message(command, body, { timeoutMs, signal }, session);
          } else {
            result = await Promise.race([
              session.protocol.send(command, body),
              new Promise((_, fail) => { timer = setTimeout(() => fail(new PicoError("发送超时", "TIMEOUT")), timeoutMs); }),
              new Promise((_, fail) => { job.cancelSend = fail; }),
            ]);
          }
          job.finish(null, result);
        } catch (error) {
          job.finish(error);
          // A valid device rejection leaves framing synchronized. Other errors
          // may leave half a message on either side of the link.
          if (error.code !== "DEVICE") await this._disconnect(error, session);
        } finally { clearTimeout(timer); }
      };
      session.queue = session.queue.then(run, run);
    });
  }

  // Internal extension hooks. Application code should use call/send/listeners.
  __send_message(command, body, options, session = this._session) {
    return session.protocol.request(command, body, options.timeoutMs, options.signal);
  }

  __on_message(message) {
    this._emit("message", message);
    if (message.broadcast && message.command === CMD.PRINT) this._emit("log", dec.decode(message.body), message);
  }

  async info(options) { return dec.decode(await this.call(CMD.INFO, new Uint8Array(), options)); }
  async reset(options) { await this.call(CMD.RESET, new Uint8Array(), options); }

  // Serialize multi-request script transactions within this client.
  _scriptTransaction(fn) {
    const session = this._session;
    if (!this.connected || !session || session.closed) return Promise.reject(new PicoError("设备未连接", "DISCONNECTED"));
    const call = (body, options = {}) => {
      if (session !== this._session || session.closed) throw new PicoError("脚本操作所属连接已关闭", "DISCONNECTED");
      return this.call(CMD.SCRIPT, body, { timeoutMs: 30000, ...options });
    };
    const job = (session.scriptQueue ?? Promise.resolve()).then(() => fn(call));
    session.scriptQueue = job.catch(() => {});
    return job;
  }
  _scriptOp(op, options = {}) {
    return this._scriptTransaction(call => call(Uint8Array.of(op), options));
  }
  async scriptStatus(options) { return JSON.parse(dec.decode(await this._scriptOp(0, options))); }
  async scriptError(options) { return dec.decode(await this._scriptOp(10, options)); }

  /** Upload and run in RAM. Does not save; failures restore the embedded script. */
  scriptRun(source, options = {}) {
    const bytes = typeof source === "string" ? new TextEncoder().encode(source) : bytesOf(source).slice();
    if (!bytes.length || bytes.length > 60 * 1024) return Promise.reject(new RangeError("脚本必须为 1..61440 字节（UTF-8）"));
    return this._scriptTransaction(async (call) => {
      const begin = new Uint8Array(9); begin[0] = 1;
      const view = new DataView(begin.buffer);
      view.setUint32(1, bytes.length, true); view.setUint32(5, crc32(bytes), true);
      await call(begin, options);
      try {
        for (let offset = 0; offset < bytes.length; offset += 507) {
          const part = bytes.subarray(offset, offset + 507);
          const body = new Uint8Array(5 + part.length); body[0] = 2;
          new DataView(body.buffer).setUint32(1, offset, true); body.set(part, 5);
          await call(body, options);
        }
        await call(Uint8Array.of(3), options);
      } catch (error) {
        try { if (!options.signal?.aborted) await call(Uint8Array.of(8), options); } catch {}
        throw error;
      }
      return JSON.parse(dec.decode(await call(Uint8Array.of(0), options)));
    });
  }
  /** Read the current RAM source with a checksum to detect concurrent changes. */
  scriptRead(options = {}) {
    return this._scriptTransaction(async (call) => {
      const status = JSON.parse(dec.decode(await call(Uint8Array.of(0), options)));
      const bytes = new Uint8Array(status.size);
      for (let offset = 0; offset < bytes.length; offset += 512) {
        const count = Math.min(512, bytes.length - offset);
        const body = new Uint8Array(7); body[0] = 5;
        const view = new DataView(body.buffer);
        view.setUint32(1, offset, true); view.setUint16(5, count, true);
        const part = await call(body, options);
        if (part.length !== count) throw new PicoError("脚本读取长度不符", "SCRIPT");
        bytes.set(part, offset);
      }
      if (crc32(bytes) !== status.crc32) throw new PicoError("读取期间脚本发生变化，请重试", "SCRIPT");
      return dec.decode(bytes);
    });
  }
  scriptSave(options) { return this._scriptOp(4, options); }
  scriptDefault(options) { return this._scriptOp(6, options); }
  scriptDelete(options) { return this._scriptOp(7, options); }
  scriptReload(options) { return this._scriptOp(9, options); }

  /** Size includes the 4-byte CRC32 trailer; firmware generates a fresh reply. */
  async test(size, options = {}) {
    if (!Number.isInteger(size) || size < 4 || size > MAX_TEST) throw new RangeError(`size 必须是 4..${MAX_TEST}`);
    const body = new Uint8Array(size);
    for (let off = 0; off < size - 4; off += 65536) {
      globalThis.crypto.getRandomValues(body.subarray(off, Math.min(off + 65536, size - 4)));
    }
    const uploadCrc = crc32(body.subarray(0, size - 4));
    new DataView(body.buffer).setUint32(size - 4, uploadCrc, true);
    const start = globalThis.performance.now();
    const response = await this.call(CMD.TEST, body, {
      ...options, timeoutMs: options.timeoutMs ?? Math.ceil((size * 2 / 50000 + 30) * 1000),
    });
    const downloadCrc = checkTestResponse(response, size);
    const durationMs = globalThis.performance.now() - start;
    return { size, body: response, uploadCrc, downloadCrc, durationMs,
      mibPerSecond: (size * 2 / 1048576) / (durationMs / 1000) };
  }

  on(event, fn) {
    if (!["message", "log", "error", "close", "statechange", "chunk"].includes(event)) throw new TypeError(`未知事件 ${event}`);
    if (typeof fn !== "function") throw new TypeError("监听器必须是函数");
    if (!this._listeners.has(event)) this._listeners.set(event, new Set());
    const listeners = this._listeners.get(event);
    listeners.add(fn);
    return () => listeners.delete(fn);
  }
  onMessage(fn) { return this.on("message", fn); }
  onLog(fn) { return this.on("log", fn); }
  onError(fn) { return this.on("error", fn); }
  onClose(fn) { return this.on("close", fn); }
  onStateChange(fn) { return this.on("statechange", fn); }
  onChunk(fn) { return this.on("chunk", fn); }
  on_log(fn) { return this.onLog(fn); }
  on_message(fn) { return this.onMessage(fn); }

  _setState(state) { this._state = state; this._emit("statechange", state); }
  _emit(event, ...args) {
    for (const listener of [...(this._listeners.get(event) ?? [])]) {
      try {
        const result = listener(...args);
        if (result && typeof result.then === "function") result.catch((error) => this._listenerError(event, error));
      } catch (error) { this._listenerError(event, error); }
    }
  }
  _listenerError(event, error) {
    if (event === "error") console.error("PicoDevice error listener failed", error);
    else this._emit("error", error);
  }
}

const PicoProtocol = Object.freeze({
  CMD: Object.freeze(CMD), ID_RESERVED, ID_BROADCAST, MAX_BODY, MAX_TEST,
  crc16, crc32, checkTestResponse,
});
Object.assign(globalThis, { PicoDevice, PicoProtocol, PicoError, PicoDeviceError });
})();
