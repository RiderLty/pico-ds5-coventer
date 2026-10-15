// Exercise the actual browser protocol code without a browser or board.
import assert from "node:assert/strict";
import fs from "node:fs";
import vm from "node:vm";

const script = fs.readFileSync(new URL("./pico-device.js", import.meta.url), "utf8");
const context = vm.createContext({
  console, TextEncoder, TextDecoder, setTimeout, clearTimeout,
});
// Internal framing regressions, against the same standalone implementation the UI loads.
vm.runInContext(script.replace(/\}\)\(\);\s*$/, `
  globalThis.api = {PicoIO: ProtocolClient, SerialTransport, SlipDecoder, crc16, crc32, checkTestResponse, CMD};
})();`), context);
const { PicoIO, SerialTransport, SlipDecoder, crc16, crc32, checkTestResponse, CMD } = context.api;

// Standard CRC vectors, including a valid CRC32 whose high bit is set.
const vector = new TextEncoder().encode("123456789");
assert.equal(crc16(vector), 0x29B1);
assert.equal(crc32(vector), 0xCBF43926);
const response = new Uint8Array(vector.length + 4);
response.set(vector);
new DataView(response.buffer).setUint32(vector.length, crc32(vector), true);
assert.equal(checkTestResponse(response, response.length), 0xCBF43926);
response[0] ^= 1;
assert.throws(() => checkTestResponse(response, response.length), /CRC32/);

// A transport may deliver the reply before its write promise settles.
const immediate = new PicoIO({ async sendChunk(chunk) { this.onchunk(chunk); } });
assert.deepEqual(Array.from(await immediate.request(CMD.INFO, vector, 100)), Array.from(vector));
assert.equal(immediate.pending.size, 0);

// Two message senders must not interleave their chunks.
const flags = [];
const queued = new PicoIO({ async sendChunk(chunk) {
  assert.ok(chunk.length <= 62);
  flags.push(chunk[0]);
} });
await Promise.all([queued.send(CMD.TEST, new Uint8Array(180)), queued.send(CMD.INFO)]);
assert.deepEqual(flags, [1, 0, 0, 2, 3]);

// Missing data fails immediately at END; a stale or unrelated command cannot
// resolve the pending request just because the message ID happens to match.
const truncated = new PicoIO({ async sendChunk(chunk) {
  if (chunk[0] & 1) this.onchunk(Uint8Array.from([3, ...chunk.subarray(1)]));
} });
await assert.rejects(truncated.request(CMD.TEST, new Uint8Array(100), 100), /不完整/);
const unrelated = new PicoIO({ async sendChunk(chunk) {
  const wrong = Uint8Array.from(chunk);
  wrong[1] = CMD.PRINT;
  this.onchunk(wrong);
  this.onchunk(chunk);
} });
assert.equal((await unrelated.request(CMD.INFO, vector, 100)).length, vector.length);

// A failed write does not leave an orphan timer or permanently poison the queue.
let fail = true;
const flaky = new PicoIO({ async sendChunk(chunk) {
  if (fail) { fail = false; throw new Error("write failed"); }
  this.onchunk(chunk);
} });
await assert.rejects(flaky.request(CMD.INFO, vector, 100), /write failed/);
assert.equal(flaky.pending.size, 0);
assert.equal((await flaky.request(CMD.INFO, vector, 100)).length, vector.length);

// IDs 0 and 65535 are reserved. Even completed IDs are not reused in a session,
// so duplicate/late replies cannot match after wraparound.
const wrapping = new PicoIO({ async sendChunk(chunk) { this.onchunk(chunk); } });
wrapping.msgSeq = 65533;
await wrapping.request(CMD.INFO, vector);
assert.equal(wrapping.msgSeq, 65534);
await wrapping.request(CMD.INFO, vector);
assert.equal(wrapping.msgSeq, 1);
wrapping.msgSeq = 65534;
await wrapping.request(CMD.INFO, vector);
assert.equal(wrapping.msgSeq, 2);
wrapping.issuedIds = new Set(Array.from({length: 65534}, (_, i) => i + 1));
await assert.rejects(wrapping.request(CMD.INFO), /编号已用完/);

// Broadcasts go to the notification handler, never to pending requests.
let notifications = 0;
const broadcasts = new PicoIO({ async sendChunk(chunk) {
  this.onchunk(Uint8Array.from([3, CMD.PRINT, 255, 255, 1, 65]));
  this.onchunk(chunk);
} });
broadcasts.onmessage = (cmd, body, id) => {
  assert.equal(cmd, CMD.PRINT); assert.equal(id, 65535);
  assert.deepEqual(Array.from(body), [65]); notifications++;
};
await broadcasts.request(CMD.INFO, vector);
assert.equal(notifications, 1);

// Device errors carry both the original ID and command; unrelated errors cannot
// reject a pending request. Reserved IDs cannot be allocated by either API.
const errors = new PicoIO({ async sendChunk(chunk) {
  this.onchunk(Uint8Array.from([3, CMD.ERROR, chunk[2], chunk[3], 2, CMD.TEST, 1]));
  this.onchunk(Uint8Array.from([3, CMD.ERROR, chunk[2], chunk[3], 2, chunk[1], 1]));
} });
await assert.rejects(errors.request(CMD.INFO), /不支持的命令/);

// Malformed varints, oversize lengths, extra bytes, and interrupted responses
// fail deterministically without allocating the advertised large buffer.
for (const [makeReply, reason] of [
  [(c) => [3, c[1], c[2], c[3], 0x80], /长度头/],
  [(c) => [3, c[1], c[2], c[3], 0xFF, 0xFF, 0xFF, 0x7F], /上限/],
  [(c) => [3, c[1], c[2], c[3], 0, 42], /超出声明/],
]) {
  const malformed = new PicoIO({async sendChunk(c) { this.onchunk(Uint8Array.from(makeReply(c))); }});
  await assert.rejects(malformed.request(CMD.INFO), reason);
}
const interrupted = new PicoIO({async sendChunk(c) {
  this.onchunk(Uint8Array.from([1, c[1], c[2], c[3], 2, 1]));
  this.onchunk(Uint8Array.from([3, CMD.PRINT, 255, 255, 0]));
}});
await assert.rejects(interrupted.request(CMD.INFO), /中断/);

const disconnecting = new PicoIO({async sendChunk() {}});
const outstanding = disconnecting.request(CMD.INFO);
disconnecting.close(new Error("test disconnect"));
await assert.rejects(outstanding, /test disconnect/);
assert.equal(disconnecting.pending.size, 0);
await assert.rejects(disconnecting.request(CMD.INFO), /关闭/);

// Streaming every UI test size through 8M via the real serial encoder and SLIP decoder, with
// arbitrary reads split within escape sequences and CRC trailers.
for (const size of [32, 64, 256, 512, 1024, 2048, 4096, 8192].map((kib) => kib * 1024)) {
  const input = new Uint8Array(size);
  for (let i = 0; i < size - 4; i++) input[i] = (i * 37 + 0xC0) & 255;
  new DataView(input.buffer).setUint32(size - 4, crc32(input.subarray(0, size - 4)), true);
  const serial = new SerialTransport();
  const io = new PicoIO(serial);
  let crcFailures = 0;
  const decoder = new SlipDecoder((chunk) => serial.onchunk(chunk), () => crcFailures++);
  serial.writer = { async write(bytes) {
    for (let offset = 0; offset < bytes.length; offset += 7) {
      for (const byte of bytes.subarray(offset, offset + 7)) decoder.byte(byte);
    }
  } };
  const output = await io.request(CMD.TEST, input, 10000);
  assert.deepEqual(Buffer.from(output), Buffer.from(input));
  assert.equal(checkTestResponse(output, size), crc32(input.subarray(0, size - 4)));
  assert.equal(crcFailures, 0);
}
console.log("PASS: unsigned CRC32, immediate replies, TX serialization, error recovery, serial/SLIP 32K through 8M");
