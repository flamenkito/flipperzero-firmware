export const MSG = Object.freeze({
  HELLO: 0x01,
  ITEM_META: 0x02,
  ITEM_DATA: 0x03,
  ACK: 0x04,
  NACK: 0x05,
  ITEM_DONE: 0x06,
  ERROR: 0x07,
  BUSY: 0x08,
  CANCEL: 0x09,
  KEY_OFFER: 0x0A,
  KEY_REPLY: 0x0B,
  KEY_CONFIRM: 0x0C,
  KEY_ABORT: 0x0D,
});

export const MAX_PAYLOAD = 59;
export const V2_DATA_SLICE_LEN = MAX_PAYLOAD - 8;
export const V2_SEGMENT_PLAINTEXT = 65536;
export const V2_SEGMENT_CIPHERTEXT = 65552;
export const V2_CONTROL_SEGMENT = 0xffffffff;
export const V2_MAX_DATA_WINDOW = 4;
export const AIRBRIDGE_CRYPTO_V1 = 1;
export const NACK_REASON = Object.freeze({
  MISSING: 1,
  MALFORMED: 2,
  AUTH_FAILED_RETRYABLE: 3,
  BUSY_WINDOW: 4,
});
export const CRYPTO_ROLE = Object.freeze({ USB: 1, BLE: 2 });
export const CRYPTO_STATE = Object.freeze({
  REQUIRED: 'crypto-required',
  HANDSHAKING: 'handshaking',
  SAS_PENDING: 'sas-pending',
  UNLOCKED: 'unlocked',
  ABORTED: 'aborted',
});

const HEADER_LEN = 5;
const META_PREFIX_LEN = 2;
const META_SLICE_LEN = MAX_PAYLOAD - META_PREFIX_LEN;
const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function requireUint16(value, name) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffff) {
    throw new RangeError(`${name} must be an integer from 0 to 65535`);
  }
}

function requireUint32(value, name) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) {
    throw new RangeError(`${name} must be an integer from 0 to 4294967295`);
  }
}

function normalizePayload(payload) {
  if (payload == null) return new Uint8Array(0);
  if (payload instanceof Uint8Array) return payload;
  if (ArrayBuffer.isView(payload)) {
    return new Uint8Array(payload.buffer, payload.byteOffset, payload.byteLength);
  }
  if (payload instanceof ArrayBuffer) return new Uint8Array(payload);
  throw new TypeError('payload must be a Uint8Array, ArrayBuffer, or typed array');
}

export function makeAckPayload(ackedType, ackedSeq) {
  requireUint16(ackedSeq, 'ACK sequence');
  if (!Number.isInteger(ackedType) || ackedType < 0 || ackedType > 0xff) {
    throw new RangeError('ACK type must be an integer from 0 to 255');
  }
  return new Uint8Array([ackedType, (ackedSeq >> 8) & 0xff, ackedSeq & 0xff]);
}

export function readAckSeq(msgOrSeq) {
  if (msgOrSeq instanceof Uint8Array || msgOrSeq instanceof ArrayBuffer || ArrayBuffer.isView(msgOrSeq)) {
    return readAckSeq(parseMessage(msgOrSeq));
  }
  if (!msgOrSeq || msgOrSeq.type !== MSG.ACK || msgOrSeq.payload.length !== 3) return null;
  return {
    type: msgOrSeq.payload[0],
    seq: (msgOrSeq.payload[1] << 8) | msgOrSeq.payload[2],
  };
}

export function makeNackPayload(nackedType, nackedSeq, reason = NACK_REASON.MISSING) {
  requireUint16(nackedSeq, 'NACK sequence');
  if (!Number.isInteger(nackedType) || nackedType < 0 || nackedType > 0xff) {
    throw new RangeError('NACK type must be an integer from 0 to 255');
  }
  if (!Object.values(NACK_REASON).includes(reason)) {
    throw new RangeError('NACK reason must be a known NACK_REASON value');
  }
  return new Uint8Array([nackedType, (nackedSeq >> 8) & 0xff, nackedSeq & 0xff, reason]);
}

export function readNackSeq(msgOrSeq) {
  if (msgOrSeq instanceof Uint8Array || msgOrSeq instanceof ArrayBuffer || ArrayBuffer.isView(msgOrSeq)) {
    return readNackSeq(parseMessage(msgOrSeq));
  }
  if (!msgOrSeq || msgOrSeq.type !== MSG.NACK || msgOrSeq.payload.length !== 4) return null;
  const reason = msgOrSeq.payload[3];
  if (!Object.values(NACK_REASON).includes(reason)) return null;
  return {
    type: msgOrSeq.payload[0],
    seq: (msgOrSeq.payload[1] << 8) | msgOrSeq.payload[2],
    reason,
  };
}

function assertSendFn(sendFn) {
  if (typeof sendFn !== 'function') throw new TypeError('sendFn must be a function');
}

function createTimeout(ms) {
  let timerId;
  const promise = new Promise((_, reject) => {
    timerId = setTimeout(() => reject(new Error('ACK timeout')), ms);
  });
  return { promise, clear: () => clearTimeout(timerId) };
}

async function waitForAttempt(sendFn, expectedType, expectedSeq, timeoutMs, attempt) {
  let settleAck;
  const ackPromise = new Promise(resolve => { settleAck = resolve; });
  const result = sendFn(attempt, settleAck);
  if (sendFn.length >= 2 && result && typeof result.then === 'function') await result;
  const waiter = sendFn.length < 2 && result && typeof result.then === 'function' ? result : ackPromise;
  const timeout = createTimeout(timeoutMs);

  try {
    const response = await Promise.race([waiter, timeout.promise]);
    const ack = readAckSeq(response);
    if (!ack || ack.type !== expectedType || ack.seq !== expectedSeq) {
      const actual = ack ? `type ${ack.type}, seq ${ack.seq}` : 'invalid ACK';
      throw new Error(`ACK mismatch: got ${actual}, expected type ${expectedType}, seq ${expectedSeq}`);
    }
  } finally {
    timeout.clear();
  }
}

function concatChunks(chunks) {
  const total = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const out = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    out.set(chunk, offset);
    offset += chunk.length;
  }
  return out;
}

function normalizeHash(hash) {
  return typeof hash === 'string' && hash.startsWith('sha256:') ? hash.slice(7) : hash;
}

function encodeErrorPayload(reason) {
  const message = String(reason || 'Protocol error');
  let truncated = '';
  let payload = new Uint8Array(0);
  for (const character of message) {
    const candidate = textEncoder.encode(`${truncated}${character}`);
    if (candidate.length > MAX_PAYLOAD) break;
    truncated += character;
    payload = candidate;
  }
  return payload.length === 0 ? textEncoder.encode('Protocol error') : payload;
}

/* characterization-only:start */
function metaTransferSize(meta) {
  return meta?.kind === 'encrypted' ? meta.encryptedSize : meta?.size;
}

function expectedDataChunks(meta) {
  if (Number.isInteger(meta?.chunks)) return Math.max(1, meta.chunks);
  const size = metaTransferSize(meta);
  return Number.isFinite(size) ? Math.max(1, Math.ceil(size / MAX_PAYLOAD)) : null;
}
/* characterization-only:end */

function isItemDataFrameType(type) {
  return type === MSG.HELLO || type === MSG.ITEM_META || type === MSG.ITEM_DATA || type === MSG.ITEM_DONE;
}

/**
 * Build a protocol message with a 5-byte header: type(1), seq(2 BE), len(2 BE).
 * @param {number} type Message type from MSG.
 * @param {number} seq Sequence number from 0 to 65535.
 * @param {Uint8Array|ArrayBuffer|ArrayBufferView} [payload] Message payload, max 59 bytes.
 * @returns {Uint8Array} Unpadded protocol frame of length 5 + payload.length.
 */
export function buildMessage(type, seq, payload = new Uint8Array(0)) {
  requireUint16(seq, 'seq');
  const body = normalizePayload(payload);
  if (body.length > MAX_PAYLOAD) {
    throw new RangeError(`payload must be ${MAX_PAYLOAD} bytes or less`);
  }

  const buf = new Uint8Array(HEADER_LEN + body.length);
  buf[0] = type & 0xff;
  buf[1] = (seq >> 8) & 0xff;
  buf[2] = seq & 0xff;
  buf[3] = (body.length >> 8) & 0xff;
  buf[4] = body.length & 0xff;
  buf.set(body, HEADER_LEN);
  return buf;
}

/**
 * Parse an unpadded or 64-byte padded protocol frame.
 * @param {Uint8Array|ArrayBuffer|ArrayBufferView} data Raw frame bytes.
 * @returns {{type:number, seq:number, len:number, payload:Uint8Array}|null} Parsed message, or null for malformed input.
 */
export function parseMessage(data) {
  let bytes;
  try {
    bytes = normalizePayload(data);
  } catch (_) {
    return null;
  }
  if (bytes.length < HEADER_LEN) return null;

  const len = (bytes[3] << 8) | bytes[4];
  if (len > MAX_PAYLOAD || bytes.length < HEADER_LEN + len) return null;

  return {
    type: bytes[0],
    seq: (bytes[1] << 8) | bytes[2],
    len,
    payload: bytes.slice(HEADER_LEN, HEADER_LEN + len),
  };
}

/**
 * Compute a SHA-256 digest as lowercase hex using the browser Web Crypto API.
 * @param {Uint8Array|ArrayBuffer|ArrayBufferView} buffer Data to hash.
 * @returns {Promise<string>} SHA-256 hex string.
 */
export async function sha256(buffer) {
  const bytes = normalizePayload(buffer);
  const hash = await crypto.subtle.digest('SHA-256', bytes);
  return Array.from(new Uint8Array(hash), b => b.toString(16).padStart(2, '0')).join('');
}

async function sha256Bytes(buffer) {
  const bytes = normalizePayload(buffer);
  return new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
}

function concatBytes(parts) {
  return concatChunks(parts.map(normalizePayload));
}

function ascii(text) {
  return textEncoder.encode(text);
}

function bytesEqual(left, right) {
  if (!left || !right || left.length !== right.length) return false;
  let diff = 0;
  for (let i = 0; i < left.length; i++) diff |= left[i] ^ right[i];
  return diff === 0;
}

function hexToBytes(hex) {
  const normalized = normalizeHash(hex);
  if (typeof normalized !== 'string' || !/^[0-9a-f]{64}$/i.test(normalized)) {
    throw new Error('SHA-256 hex digest is required');
  }
  const out = new Uint8Array(32);
  for (let i = 0; i < out.length; i++) out[i] = Number.parseInt(normalized.slice(i * 2, i * 2 + 2), 16);
  return out;
}

function bytesToBase64(bytes, urlSafe = false) {
  let binary = '';
  const input = normalizePayload(bytes);
  for (const byte of input) binary += String.fromCharCode(byte);
  const base64 = btoa(binary);
  return urlSafe ? base64.replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/u, '') : base64;
}

function base64ToBytes(value) {
  const normalized = String(value).replace(/-/g, '+').replace(/_/g, '/');
  const padded = normalized.padEnd(Math.ceil(normalized.length / 4) * 4, '=');
  const binary = atob(padded);
  const out = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
  return out;
}

function u32Bytes(value) {
  requireUint32(value, 'uint32');
  return new Uint8Array([(value >>> 24) & 0xff, (value >>> 16) & 0xff, (value >>> 8) & 0xff, value & 0xff]);
}

function u64Bytes(value) {
  const big = BigInt(value);
  if (big < 0n || big > 0xffff_ffff_ffff_ffffn) throw new RangeError('uint64 out of range');
  const out = new Uint8Array(8);
  for (let i = 7, current = big; i >= 0; i--, current >>= 8n) out[i] = Number(current & 0xffn);
  return out;
}

function readU32(bytes, offset) {
  return (((bytes[offset] << 24) >>> 0) | (bytes[offset + 1] << 16) | (bytes[offset + 2] << 8) | bytes[offset + 3]) >>> 0;
}

function readU64(bytes, offset) {
  let value = 0n;
  for (let i = 0; i < 8; i++) value = (value << 8n) | BigInt(bytes[offset + i]);
  return value;
}

function directionForRole(role) {
  return role === CRYPTO_ROLE.USB ? 'usb-to-ble' : 'ble-to-usb';
}

function oppositeDirection(role) {
  return role === CRYPTO_ROLE.USB ? 'ble-to-usb' : 'usb-to-ble';
}

function directionByte(direction) {
  if (direction === 'usb-to-ble') return 1;
  if (direction === 'ble-to-usb') return 2;
  throw new Error('unknown crypto direction');
}

function keyTypeForRole(role) {
  return role === CRYPTO_ROLE.USB ? MSG.KEY_OFFER : MSG.KEY_REPLY;
}

function expectedRemoteKeyType(role) {
  return role === CRYPTO_ROLE.USB ? MSG.KEY_REPLY : MSG.KEY_OFFER;
}

function abortReasonName(reason) {
  switch (reason) {
    case 1: return 'unsupported-version';
    case 2: return 'sas-mismatch';
    case 3: return 'timeout';
    case 4: return 'conflict';
    default: return 'protocol';
  }
}

/* characterization-only:start */
function makeAad({ cryptoVersion, itemId, direction, iv, encryptedSize }) {
  return concatBytes([
    ascii('AB1-AAD'),
    new Uint8Array([cryptoVersion, itemId >>> 24 & 0xff, itemId >>> 16 & 0xff, itemId >>> 8 & 0xff, itemId & 0xff, directionByte(direction)]),
    iv,
    u64Bytes(encryptedSize),
  ]);
}

function encodeInnerEnvelope(meta, data) {
  const kindByte = meta.kind === 'text' ? 1 : meta.kind === 'attachment' ? 2 : 0;
  if (!kindByte) throw new Error('plaintext item kind must be text or attachment');
  const nameBytes = textEncoder.encode(meta.name || '');
  const mimeBytes = textEncoder.encode(meta.mimeType || 'application/octet-stream');
  requireUint16(nameBytes.length, 'name length');
  requireUint16(mimeBytes.length, 'MIME length');
  const plain = normalizePayload(data);
  const plainHash = hexToBytes(meta.hash);
  const header = concatBytes([
    ascii('AB1'),
    u32Bytes(meta.itemId),
    new Uint8Array([kindByte, nameBytes.length >> 8 & 0xff, nameBytes.length & 0xff, mimeBytes.length >> 8 & 0xff, mimeBytes.length & 0xff]),
    u64Bytes(plain.length),
    plainHash,
  ]);
  return concatBytes([header, nameBytes, mimeBytes, plain]);
}

async function decodeInnerEnvelope(bytes) {
  const input = normalizePayload(bytes);
  if (input.length < 52 || textDecoder.decode(input.slice(0, 3)) !== 'AB1') throw new Error('Invalid encrypted item envelope');
  const itemId = readU32(input, 3);
  const kindByte = input[7];
  const nameLen = (input[8] << 8) | input[9];
  const mimeLen = (input[10] << 8) | input[11];
  const plainSize = readU64(input, 12);
  const plainSha256 = input.slice(20, 52);
  const nameStart = 52;
  const mimeStart = nameStart + nameLen;
  const dataStart = mimeStart + mimeLen;
  if (dataStart > input.length) throw new Error('Invalid encrypted item envelope lengths');
  const data = input.slice(dataStart);
  if (BigInt(data.length) !== plainSize) throw new Error('Encrypted item plaintext size mismatch');
  const hash = await sha256Bytes(data);
  if (!bytesEqual(hash, plainSha256)) throw new Error('Encrypted item plaintext SHA-256 mismatch');
  const kind = kindByte === 1 ? 'text' : kindByte === 2 ? 'attachment' : null;
  if (!kind) throw new Error('Invalid encrypted item kind');
  return {
    meta: {
      kind,
      itemId,
      name: textDecoder.decode(input.slice(nameStart, mimeStart)),
      mimeType: textDecoder.decode(input.slice(mimeStart, dataStart)),
      size: data.length,
      chunks: Math.max(1, Math.ceil(data.length / MAX_PAYLOAD)),
      hash: `sha256:${Array.from(plainSha256, b => b.toString(16).padStart(2, '0')).join('')}`,
    },
    data,
    hash: Array.from(plainSha256, b => b.toString(16).padStart(2, '0')).join(''),
  };
}
/* characterization-only:end */

/**
 * Encode a metadata object into META payload fragments. Each payload begins with
 * the 2-byte big-endian total fragment count followed by a JSON slice.
 * @param {object} metaObj JSON-serializable metadata.
 * @returns {Uint8Array[]} META payload fragments, each no larger than 59 bytes.
 */
export function encodeMeta(metaObj) {
  const json = textEncoder.encode(JSON.stringify(metaObj));
  const total = Math.max(1, Math.ceil(json.length / META_SLICE_LEN));
  requireUint16(total, 'META fragment count');

  const fragments = [];
  for (let i = 0; i < total; i++) {
    const chunk = json.slice(i * META_SLICE_LEN, (i + 1) * META_SLICE_LEN);
    const payload = new Uint8Array(META_PREFIX_LEN + chunk.length);
    payload[0] = (total >> 8) & 0xff;
    payload[1] = total & 0xff;
    payload.set(chunk, META_PREFIX_LEN);
    fragments.push(payload);
  }
  return fragments;
}

/**
 * Decode META payload fragments into the original metadata object.
 * @param {Uint8Array[]} fragments META payload fragments in sequence order.
 * @returns {object} Parsed metadata object.
 */
export function decodeMeta(fragments) {
  if (!Array.isArray(fragments) || fragments.length === 0) {
    throw new Error('META fragments are required');
  }

  const payloads = fragments.map(normalizePayload);
  const total = (payloads[0][0] << 8) | payloads[0][1];
  if (total <= 0 || payloads.length !== total) {
    throw new Error(`META fragment count mismatch: got ${payloads.length}, expected ${total}`);
  }

  for (const payload of payloads) {
    const fragmentTotal = (payload[0] << 8) | payload[1];
    if (payload.length < META_PREFIX_LEN || fragmentTotal !== total) {
      throw new Error('Invalid META fragment');
    }
  }

  const jsonBytes = concatChunks(payloads.map(payload => payload.slice(META_PREFIX_LEN)));
  return JSON.parse(textDecoder.decode(jsonBytes));
}

/**
 * Send or resend one frame until an ACK for expectedSeq arrives.
 * sendFn is called as sendFn(attempt, resolveAck). It may either return a
 * Promise resolving to an ACK message/sequence, or call resolveAck later.
 * @param {Function} sendFn Transport-neutral send callback.
 * @param {number} expectedType Message type expected inside the ACK payload.
 * @param {number} expectedSeq Sequence number expected inside the ACK payload.
 * @param {number} [timeoutMs=5000] Per-attempt timeout in milliseconds.
 * @param {number} [retries=3] Total send attempts before rejection.
 * @returns {Promise<void>} Resolves after the expected ACK arrives.
 */
export async function waitAck(sendFn, expectedType, expectedSeq, timeoutMs = 5000, retries = 3) {
  assertSendFn(sendFn);
  if (!Number.isInteger(expectedType) || expectedType < 0 || expectedType > 0xff) {
    throw new RangeError('expected ACK type must be an integer from 0 to 255');
  }
  requireUint16(expectedSeq, 'expectedSeq');
  if (!Number.isInteger(retries) || retries < 1) throw new RangeError('retries must be at least 1');

  let lastError;
  for (let attempt = 1; attempt <= retries; attempt++) {
    try {
      await waitForAttempt(sendFn, expectedType, expectedSeq, timeoutMs, attempt);
      return;
    } catch (error) {
      lastError = error;
      if (attempt < retries) await sleep(200);
    }
  }
  throw lastError;
}

export class AirBridgeCryptoSession {
  #invalidations = new Set();
  #itemCryptoMode = null;
  #outboundStream = null;
  #inboundStream = null;
  constructor(options) {
    if (!options || (options.role !== CRYPTO_ROLE.USB && options.role !== CRYPTO_ROLE.BLE)) throw new Error('crypto role must be USB or BLE');
    assertSendFn(options.sendFn);
    this.role = options.role;
    this.sendFn = options.sendFn;
    this.timeoutMs = options.timeoutMs ?? 1000;
    this.retries = options.retries ?? 3;
    this.onStateChange = typeof options.onStateChange === 'function' ? options.onStateChange : () => {};
    this.sender = new ItemSender(frame => this.sendFn(frame), { timeoutMs: this.timeoutMs, retries: this.retries });
    this.reset();
  }

  reset() {
    this.state = CRYPTO_STATE.REQUIRED;
    for (const callback of [...this.#invalidations]) callback();
    this.#outboundStream?.abort(); this.#outboundStream = null;
    this.#inboundStream?.abort(); this.#inboundStream = null;
    this.#itemCryptoMode = null;
    this.state = CRYPTO_STATE.REQUIRED;
    this.keyPair = null;
    this.localPubRaw = null;
    this.remotePubRaw = null;
    this.remoteRole = null;
    this.fragmentStores = new Map();
    this.fragmentPayloads = new Map();
    this.fragmentTimers = new Map();
    this.transcript = null;
    this.transcriptHash16 = null;
    this.sas = null;
    this.localAccepted = false;
    this.peerConfirmed = false;
    this.confirmPayload = null;
    this.keyId = null;
    this.keys = null;
    this.noncePrefixes = null;
    /* characterization-only:start */
    this.outboundCounter = 0n;
    /* characterization-only:end */
    this.streamOutboundId = 0;
    this.streamInboundId = 0;
    /* characterization-only:start */
    this.inboundReplay = new Set();
    /* characterization-only:end */
    this.emitState();
  }

  emitState() {
    this.onStateChange(this.getStatus());
  }

  onInvalidated(callback) {
    this.#invalidations.add(callback);
    return () => this.#invalidations.delete(callback);
  }

  getStatus() {
    return Object.freeze({ state: this.state, sas: this.sas, localAccepted: this.localAccepted, peerConfirmed: this.peerConfirmed, keyId: this.keyId });
  }

  isUnlocked() {
    return this.state === CRYPTO_STATE.UNLOCKED;
  }

  isCryptoFrame(msg) {
    return Boolean(msg && (msg.type === MSG.KEY_OFFER || msg.type === MSG.KEY_REPLY || msg.type === MSG.KEY_CONFIRM || msg.type === MSG.KEY_ABORT));
  }

  receiveAck(msg) {
    return this.sender.receiveAck(msg);
  }

  receiveNack(msg) {
    return this.sender.receiveNack(msg);
  }

  async start() {
    await this.ensureLocalKey();
    this.state = CRYPTO_STATE.HANDSHAKING;
    this.emitState();
    if (this.role === CRYPTO_ROLE.USB) await this.sendPublicKey(MSG.KEY_OFFER);
  }

  async acceptSas() {
    if (this.state !== CRYPTO_STATE.SAS_PENDING || !this.transcriptHash16) throw new Error('SAS is not pending');
    this.localAccepted = true;
    await this.sendConfirm();
    this.maybeUnlock();
  }

  async rejectSas() {
    await this.sendAbort(2, 'SAS mismatch');
    this.clearKeys(CRYPTO_STATE.ABORTED);
  }

  clearKeys(nextState = CRYPTO_STATE.REQUIRED) {
    for (const timer of this.fragmentTimers.values()) clearTimeout(timer);
    this.reset();
    this.state = nextState;
    this.emitState();
  }

  async onMessage(msg) {
    if (!this.isCryptoFrame(msg)) return false;
    if (msg.type === MSG.KEY_ABORT) {
      this.handleAbort(msg);
      return true;
    }
    try {
      switch (msg.type) {
        case MSG.KEY_OFFER:
        case MSG.KEY_REPLY:
          await this.handlePublicKeyFragment(msg);
          break;
        case MSG.KEY_CONFIRM:
          await this.handleConfirm(msg);
          break;
        default:
          return false;
      }
    } catch (error) {
      await this.sendAbort(4, error.message || 'crypto handshake failed');
      this.clearKeys(CRYPTO_STATE.ABORTED);
      throw error;
    }
    return true;
  }

  async ensureLocalKey() {
    if (this.keyPair && this.localPubRaw) return;
    if (!globalThis.crypto?.subtle) throw new Error('WebCrypto SubtleCrypto is required');
    this.keyPair = await crypto.subtle.generateKey({ name: 'ECDH', namedCurve: 'P-256' }, false, ['deriveBits']);
    this.localPubRaw = new Uint8Array(await crypto.subtle.exportKey('raw', this.keyPair.publicKey));
    if (this.localPubRaw.length !== 65 || this.localPubRaw[0] !== 0x04) throw new Error('WebCrypto returned malformed P-256 public key');
  }

  async sendPublicKey(type) {
    if (!this.localPubRaw) throw new Error('local public key missing');
    const fragments = [];
    const first = new Uint8Array(Math.min(MAX_PAYLOAD, 4 + this.localPubRaw.length));
    first[0] = AIRBRIDGE_CRYPTO_V1;
    first[1] = this.role;
    first[2] = (this.localPubRaw.length >> 8) & 0xff;
    first[3] = this.localPubRaw.length & 0xff;
    first.set(this.localPubRaw.slice(0, first.length - 4), 4);
    fragments.push(first);
    for (let offset = first.length - 4; offset < this.localPubRaw.length; offset += MAX_PAYLOAD) {
      fragments.push(this.localPubRaw.slice(offset, offset + MAX_PAYLOAD));
    }
    for (let seq = 0; seq < fragments.length; seq++) {
      try {
        await this.sender.sendWithAck(buildMessage(type, seq, fragments[seq]), seq);
      } catch (error) {
        await this.sendAbort(3, 'handshake timeout');
        this.clearKeys(CRYPTO_STATE.ABORTED);
        throw error;
      }
    }
  }

  async handlePublicKeyFragment(msg) {
    if (msg.type !== expectedRemoteKeyType(this.role)) throw new Error('unexpected crypto key frame for role');
    const key = `${msg.type}:${msg.seq}`;
    const previous = this.fragmentPayloads.get(key);
    if (previous) {
      if (!bytesEqual(previous, msg.payload)) throw new Error('conflicting duplicate key fragment');
      await this.sendAck(msg.type, msg.seq);
      return;
    }
    this.fragmentPayloads.set(key, msg.payload.slice());
    await this.sendAck(msg.type, msg.seq);

    let store = this.fragmentStores.get(msg.type);
    if (!store || msg.seq === 0) {
      if (msg.seq !== 0 && !store) throw new Error('missing first key fragment');
      if (msg.payload.length < 4) throw new Error('key fragment header too short');
      const version = msg.payload[0];
      const remoteRole = msg.payload[1];
      const totalLen = (msg.payload[2] << 8) | msg.payload[3];
      if (version !== AIRBRIDGE_CRYPTO_V1) throw new Error('unsupported crypto version');
      if (remoteRole === this.role || (remoteRole !== CRYPTO_ROLE.USB && remoteRole !== CRYPTO_ROLE.BLE)) throw new Error('duplicate or invalid crypto role');
      if (totalLen !== 65) throw new Error('wrong P-256 public key length');
      store = { version, remoteRole, totalLen, fragments: new Map([[0, msg.payload.slice(4)]]) };
      this.fragmentStores.set(msg.type, store);
      this.armFragmentTimeout(msg.type);
    } else {
      store.fragments.set(msg.seq, msg.payload.slice());
    }

    const bytes = concatChunks([...store.fragments.keys()].sort((a, b) => a - b).map(seq => store.fragments.get(seq)));
    if (bytes.length < store.totalLen) return;
    if (bytes.length > store.totalLen) throw new Error('key fragments exceed declared length');
    clearTimeout(this.fragmentTimers.get(msg.type));
    this.fragmentTimers.delete(msg.type);
    await this.finishPeerKey(store.remoteRole, bytes);
  }

  armFragmentTimeout(type) {
    clearTimeout(this.fragmentTimers.get(type));
    this.fragmentTimers.set(type, setTimeout(() => {
      void this.sendAbort(3, 'missing key fragment');
      this.clearKeys(CRYPTO_STATE.ABORTED);
    }, this.timeoutMs * (this.retries + 1) + 250));
  }

  async finishPeerKey(remoteRole, remotePubRaw) {
    await this.ensureLocalKey();
    if (remotePubRaw.length !== 65 || remotePubRaw[0] !== 0x04) throw new Error('malformed P-256 public key');
    this.remoteRole = remoteRole;
    this.remotePubRaw = remotePubRaw.slice();
    const remotePub = await crypto.subtle.importKey('raw', this.remotePubRaw, { name: 'ECDH', namedCurve: 'P-256' }, false, []);
    const sharedBits = new Uint8Array(await crypto.subtle.deriveBits({ name: 'ECDH', public: remotePub }, this.keyPair.privateKey, 256));
    await this.deriveSession(sharedBits);
    this.state = CRYPTO_STATE.SAS_PENDING;
    this.emitState();
    if (this.role === CRYPTO_ROLE.BLE) await this.sendPublicKey(MSG.KEY_REPLY);
  }

  async deriveSession(sharedBits) {
    const usbPub = this.role === CRYPTO_ROLE.USB ? this.localPubRaw : this.remotePubRaw;
    const blePub = this.role === CRYPTO_ROLE.BLE ? this.localPubRaw : this.remotePubRaw;
    this.transcript = concatBytes([ascii('PocketAirBridge-crypto-v1'), new Uint8Array([CRYPTO_ROLE.USB]), usbPub, new Uint8Array([CRYPTO_ROLE.BLE]), blePub]);
    const transcriptHash = await sha256Bytes(this.transcript);
    this.transcriptHash16 = transcriptHash.slice(0, 16);
    const sasHash = await sha256Bytes(concatBytes([ascii('PocketAirBridge SAS v1'), this.transcript]));
    const sasInt = ((sasHash[0] << 12) | (sasHash[1] << 4) | (sasHash[2] >> 4)) % 1000000;
    this.sas = String(sasInt).padStart(6, '0');
    const salt = await sha256Bytes(concatBytes([ascii('PocketAirBridge salt v1'), this.transcript]));
    const material = await crypto.subtle.importKey('raw', sharedBits, 'HKDF', false, ['deriveBits', 'deriveKey']);
    const deriveKey = info => crypto.subtle.deriveKey(
      { name: 'HKDF', hash: 'SHA-256', salt, info: ascii(info) },
      material,
      { name: 'AES-GCM', length: 256 },
      false,
      ['encrypt', 'decrypt'],
    );
    const deriveBytes = async (info, length) => new Uint8Array(await crypto.subtle.deriveBits({ name: 'HKDF', hash: 'SHA-256', salt, info: ascii(info) }, material, length * 8));
    this.keys = {
      'usb-to-ble': await deriveKey('PocketAirBridge v1 USB->BLE item key'),
      'ble-to-usb': await deriveKey('PocketAirBridge v1 BLE->USB item key'),
    };
    this.noncePrefixes = {
      'usb-to-ble': await deriveBytes('PocketAirBridge v1 usb-to-ble nonce prefix', 4),
      'ble-to-usb': await deriveBytes('PocketAirBridge v1 ble-to-usb nonce prefix', 4),
    };
    this.keyId = bytesToBase64(await deriveBytes('PocketAirBridge v1 key id', 16), true);
  }

  maybeUnlock() {
    if (this.state === CRYPTO_STATE.SAS_PENDING && this.localAccepted && this.peerConfirmed) {
      this.state = CRYPTO_STATE.UNLOCKED;
      this.emitState();
    } else {
      this.emitState();
    }
  }

  async sendConfirm() {
    const payload = concatBytes([new Uint8Array([AIRBRIDGE_CRYPTO_V1, 1]), this.transcriptHash16]);
    await this.sender.sendWithAck(buildMessage(MSG.KEY_CONFIRM, 0, payload), 0);
  }

  async handleConfirm(msg) {
    if (this.confirmPayload) {
      if (!bytesEqual(this.confirmPayload, msg.payload)) throw new Error('conflicting duplicate KEY_CONFIRM');
      await this.sendAck(MSG.KEY_CONFIRM, msg.seq);
      return;
    }
    if (msg.payload.length !== 18 || msg.payload[0] !== AIRBRIDGE_CRYPTO_V1 || msg.payload[1] !== 1) throw new Error('invalid KEY_CONFIRM');
    if (!bytesEqual(msg.payload.slice(2), this.transcriptHash16)) throw new Error('KEY_CONFIRM transcript mismatch');
    if (this.state !== CRYPTO_STATE.SAS_PENDING && this.state !== CRYPTO_STATE.UNLOCKED) throw new Error('KEY_CONFIRM outside SAS pending');
    this.confirmPayload = msg.payload.slice();
    await this.sendAck(MSG.KEY_CONFIRM, msg.seq);
    this.peerConfirmed = true;
    this.maybeUnlock();
  }

  handleAbort(msg) {
    const reason = msg.payload.length >= 2 ? msg.payload[1] : 0;
    const detail = msg.payload.length > 2 ? textDecoder.decode(msg.payload.slice(2)) : abortReasonName(reason);
    this.clearKeys(CRYPTO_STATE.ABORTED);
    const error = new Error(detail || 'crypto aborted');
    error.name = 'CryptoAbortError';
    throw error;
  }

  async sendAbort(reason, detail = '') {
    const reasonBytes = textEncoder.encode(detail).slice(0, MAX_PAYLOAD - 2);
    const payload = new Uint8Array(2 + reasonBytes.length);
    payload[0] = AIRBRIDGE_CRYPTO_V1;
    payload[1] = reason;
    payload.set(reasonBytes, 2);
    await this.sendFn(buildMessage(MSG.KEY_ABORT, 0, payload));
  }

  async sendAck(type, seq) {
    await this.sendFn(buildMessage(MSG.ACK, 0, makeAckPayload(type, seq)));
  }

  /* characterization-only:start */
  makeIv(direction, counter) {
    const prefix = this.noncePrefixes?.[direction];
    if (!prefix) throw new Error('missing nonce prefix');
    return concatBytes([prefix, u64Bytes(counter)]);
  }
  /* characterization-only:end */

  // Called only after the hash pass, immediately before HELLO. There is no
  // refund operation: cancel, error, timeout and success all burn the ID.
  allocateStreamItemId() {
    if (!this.isUnlocked()) throw new Error('crypto session is locked');
    this.#selectItemCryptoMode(2);
    if (this.streamOutboundId >= 0xffffffff) {
      this.clearKeys(CRYPTO_STATE.ABORTED);
      throw new Error('stream item ID wrapped; fresh ECDH session required');
    }
    return ++this.streamOutboundId;
  }

  acceptStreamItemId(itemId) {
    if (!this.isUnlocked()) throw new Error('crypto session is locked');
    this.#selectItemCryptoMode(2);
    requireUint32(itemId, 'itemId');
    if (itemId <= this.streamInboundId) throw new Error('stale stream item ID');
    this.streamInboundId = itemId;
  }

  // Hash-pass receipts are inputs; this API performs only the second stream pass.
  openEncryptedStream(header, source) {
    const stream = new V2EncryptedStream(this, header, source);
    this.#outboundStream?.abort(); this.#outboundStream = stream;
    return stream;
  }

  openSegmentReceiver(meta, {windowSize = 1, sliding = false} = {}) {
    requireV2Window(windowSize);
    const receiver = windowSize === 1 ? new V2SegmentReceiver(this, meta) :
      new V2WindowSegmentReceiver(this, meta, windowSize, sliding);
    this.#inboundStream?.abort(); this.#inboundStream = receiver;
    return receiver;
  }

  #selectItemCryptoMode(version) {
    if (this.#itemCryptoMode !== null && this.#itemCryptoMode !== version) throw new Error('mixed v1/v2 item crypto requires fresh ECDH');
    this.#itemCryptoMode = version;
  }

  /* characterization-only:start */
  async encryptItem(meta, data) {
    if (!this.isUnlocked()) throw new Error('crypto session is locked');
    this.#selectItemCryptoMode(1);
    const direction = directionForRole(this.role);
    const itemCounter = this.outboundCounter;
    if (itemCounter > 0xffff_ffff_ffff_ffffn) throw new Error('crypto item counter wrapped');
    this.outboundCounter++;
    const iv = this.makeIv(direction, itemCounter);
    const inner = encodeInnerEnvelope(meta, data);
    const encryptedSize = inner.length + 16;
    const aad = makeAad({ cryptoVersion: AIRBRIDGE_CRYPTO_V1, itemId: meta.itemId, direction, iv, encryptedSize });
    const encrypted = new Uint8Array(await crypto.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: aad, tagLength: 128 }, this.keys[direction], inner));
    const encryptedSha256 = await sha256(encrypted);
    return {
      meta: {
        kind: 'encrypted',
        cryptoVersion: AIRBRIDGE_CRYPTO_V1,
        alg: 'AES-GCM-256',
        keyId: this.keyId,
        direction,
        iv: bytesToBase64(iv),
        encryptedSize: encrypted.length,
        encryptedSha256: `sha256:${encryptedSha256}`,
        chunks: expectedDataChunks({ kind: 'encrypted', encryptedSize: encrypted.length }),
        itemId: meta.itemId,
      },
      data: encrypted,
    };
  }

  async decryptItem(meta, encrypted) {
    if (!this.isUnlocked()) throw new Error('crypto session is locked');
    this.#selectItemCryptoMode(1);
    if (meta.kind !== 'encrypted' || meta.cryptoVersion !== AIRBRIDGE_CRYPTO_V1 || meta.alg !== 'AES-GCM-256') throw new Error('encrypted item metadata required');
    if (meta.keyId !== this.keyId) throw new Error('encrypted item keyId mismatch');
    const direction = meta.direction;
    if (direction !== oppositeDirection(this.role)) throw new Error('encrypted item direction mismatch');
    const iv = base64ToBytes(meta.iv);
    if (iv.length !== 12 || !bytesEqual(iv.slice(0, 4), this.noncePrefixes[direction])) throw new Error('encrypted item IV prefix mismatch');
    const itemCounter = readU64(iv, 4);
    const replayKey = `${meta.keyId}:${direction}:${itemCounter}`;
    if (this.inboundReplay.has(replayKey)) throw new Error('encrypted item replay rejected');
    const cipher = normalizePayload(encrypted);
    if (cipher.length !== meta.encryptedSize) throw new Error('encrypted item size mismatch');
    const cipherHash = await sha256(cipher);
    if (normalizeHash(meta.encryptedSha256) !== cipherHash) throw new Error('encrypted item SHA-256 mismatch');
    const aad = makeAad({ cryptoVersion: meta.cryptoVersion, itemId: meta.itemId, direction, iv, encryptedSize: meta.encryptedSize });
    let plain;
    try {
      plain = new Uint8Array(await crypto.subtle.decrypt({ name: 'AES-GCM', iv, additionalData: aad, tagLength: 128 }, this.keys[direction], cipher));
    } catch (error) {
      throw new Error('AES-GCM authentication failed');
    }
    this.inboundReplay.add(replayKey);
    return decodeInnerEnvelope(plain);
  }
  /* characterization-only:end */
}

const v2Magic = new Uint8Array([65, 66, 50, 83]);
const v2WindowMagic = new Uint8Array([65, 66, 50, 87]);
const v2SlidingMagic = new Uint8Array([65, 66, 50, 80]);

function requireV2Window(size) {
  if (![1, 2, V2_MAX_DATA_WINDOW].includes(size)) throw new Error('invalid DATA window');
  return size;
}
const v2FatalDecoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });
const v2ItemTypes = [MSG.HELLO, MSG.ITEM_META, MSG.ITEM_DATA, MSG.ITEM_DONE, MSG.CANCEL];

function requireV2BigInt(value) {
  if (typeof value !== 'bigint' || value < 0n || value > 0xffffffffffffffffn) {
    throw new RangeError('uint64 BigInt required');
  }
  return value;
}

export function parseCanonicalUint64(value) {
  if (typeof value !== 'string' || value.length > 20 || !/^(0|[1-9][0-9]*)$/.test(value)) {
    throw new Error('canonical unsigned decimal uint64 string required');
  }
  return requireV2BigInt(BigInt(value));
}

export function toSafeV2Number(value) {
  requireV2BigInt(value);
  if (value > BigInt(Number.MAX_SAFE_INTEGER)) throw new RangeError('unsafe Number conversion');
  return Number(value);
}

function v2Text(value) {
  if (typeof value !== 'string') throw new TypeError('UTF-8 string required');
  const encoded = textEncoder.encode(value);
  if (v2FatalDecoder.decode(encoded) !== value) throw new Error('invalid UTF-8 source');
  return encoded;
}

function v2KeyBytes(keyId) {
  if (typeof keyId !== 'string' || !/^[A-Za-z0-9_-]{22}$/.test(keyId)) throw new Error('invalid keyId');
  const key = base64ToBytes(keyId);
  if (key.length !== 16 || bytesToBase64(key, true) !== keyId) throw new Error('noncanonical keyId');
  return key;
}

function validateV2Context({ type, seq, itemId, segment }) {
  if (!v2ItemTypes.includes(type)) throw new Error('invalid v2 item type');
  requireUint16(seq, 'seq');
  requireUint32(itemId, 'itemId');
  requireUint32(segment, 'segment');
  if (type !== MSG.ITEM_DATA && segment !== V2_CONTROL_SEGMENT) throw new Error('invalid control segment');
  if ([MSG.HELLO, MSG.ITEM_DONE, MSG.CANCEL].includes(type) && seq !== 0) throw new Error('invalid control seq');
}

export function makeV2AckPayload(context, reason) {
  validateV2Context(context);
  const windowSize = context.type === MSG.HELLO ? requireV2Window(context.windowSize ?? 1) : 1;
  const windowAck = reason === undefined && windowSize > 1;
  const ack = concatBytes([makeAckPayload(context.type, context.seq), windowAck ? (context.sliding ? v2SlidingMagic : v2WindowMagic) : v2Magic,
    u32Bytes(context.itemId), u32Bytes(context.segment)]);
  if (windowAck) return concatBytes([ack, new Uint8Array([windowSize])]);
  if (reason === undefined) return ack;
  if (!Object.values(NACK_REASON).includes(reason)) throw new Error('invalid NACK reason');
  return concatBytes([ack, new Uint8Array([reason])]);
}

export function buildV2Frame(type, seq, itemId, segment = V2_CONTROL_SEGMENT, data) {
  requireUint32(itemId, 'itemId');
  let payload;
  switch (type) {
    case MSG.HELLO: {
      const windowSize = requireV2Window(data?.windowSize ?? data ?? 1);
      const sliding = Boolean(data?.sliding);
      if (sliding && windowSize === 1) throw new Error('sliding DATA requires a window');
      payload = windowSize === 1 ? concatBytes([u32Bytes(itemId), v2Magic]) :
        concatBytes([u32Bytes(itemId), sliding ? v2SlidingMagic : v2WindowMagic, new Uint8Array([windowSize])]);
      break;
    }
    case MSG.ITEM_META: payload = normalizePayload(data); break;
    case MSG.ITEM_DATA:
      payload = concatBytes([u32Bytes(itemId), u32Bytes(segment), normalizePayload(data)]);
      break;
    case MSG.ITEM_DONE:
    case MSG.CANCEL: payload = u32Bytes(itemId); break;
    case MSG.ERROR: payload = concatBytes([v2Magic, u32Bytes(itemId), v2Text(data ?? '')]); break;
    case MSG.BUSY: payload = concatBytes([v2Magic, u32Bytes(itemId)]); break;
    default: throw new Error('invalid v2 frame type');
  }
  if (v2ItemTypes.includes(type)) validateV2Context({type, seq, itemId, segment});
  const frame = buildMessage(type, seq, payload);
  parseV2Frame(frame, itemId);
  return frame;
}

export function parseV2Frame(raw, activeItemId) {
  const msg = parseV2RoutingFrame(raw, activeItemId);
  validateV2ControlBody(msg);
  return msg;
}

function validateV2ControlBody(msg) {
  if (msg.type === MSG.ERROR) v2FatalDecoder.decode(msg.body);
}

function parseV2Envelope(raw) {
  const bytes = normalizePayload(raw);
  const msg = parseMessage(bytes);
  if (!msg || (bytes.length !== 5 + msg.len && bytes.length !== 64) || bytes.length > 64) {
    throw new Error('malformed v2 frame length');
  }
  if (bytes.slice(5 + msg.len).some(byte => byte !== 0)) throw new Error('nonzero frame padding');
  return msg;
}

export function parseV2RoutingFrame(raw, activeItemId) {
  const msg = parseV2Envelope(raw);
  const p = msg.payload;
  let itemId;
  let segment = V2_CONTROL_SEGMENT;
  let body = p;
  let windowSize;
  let sliding = false;
  const magicAt = offset => bytesEqual(p.slice(offset, offset + 4), v2Magic);
  switch (msg.type) {
    case MSG.HELLO:
      if (p.length === 8 && magicAt(4)) windowSize = 1;
      else if (p.length === 9 && [2, 4].includes(p[8]) &&
          (bytesEqual(p.slice(4, 8), v2WindowMagic) || bytesEqual(p.slice(4, 8), v2SlidingMagic))) {
        windowSize = p[8]; sliding = bytesEqual(p.slice(4, 8), v2SlidingMagic);
      }
      else throw new Error('Unsupported protocol version');
      itemId = readU32(p, 0);
      break;
    case MSG.ITEM_META:
      requireUint32(activeItemId, 'META active itemId');
      if (p.length < 3 || ((p[0] << 8) | p[1]) === 0) throw new Error('invalid META fragment');
      itemId = activeItemId;
      break;
    case MSG.ITEM_DATA:
      if (p.length < 8) throw new Error('missing DATA context');
      itemId = readU32(p, 0); segment = readU32(p, 4); body = p.slice(8);
      break;
    case MSG.ITEM_DONE:
    case MSG.CANCEL:
      if (p.length !== 4) throw new Error('missing item control context');
      itemId = readU32(p, 0);
      break;
    case MSG.ERROR:
    case MSG.BUSY:
      if (p.length < 8 || !magicAt(0) || (msg.type === MSG.BUSY && p.length !== 8) || msg.seq !== 0) throw new Error('invalid item control');
      itemId = readU32(p, 4); body = p.slice(8);
      break;
    case MSG.ACK:
    case MSG.NACK: {
      const windowAck = msg.type === MSG.ACK && p[0] === MSG.HELLO && p.length === 16 &&
        (bytesEqual(p.slice(3, 7), v2WindowMagic) || bytesEqual(p.slice(3, 7), v2SlidingMagic)) && [2, 4].includes(p[15]);
      if ((!windowAck && (p.length !== (msg.type === MSG.ACK ? 15 : 16) || !magicAt(3))) || msg.seq !== 0) throw new Error('invalid v2 ACK/NACK');
      const context = {type:p[0], seq:(p[1]<<8)|p[2], itemId:readU32(p,7), segment:readU32(p,11)};
      validateV2Context(context);
      if (msg.type === MSG.NACK && !Object.values(NACK_REASON).includes(p[15])) throw new Error('invalid NACK reason');
      return {...msg, ...context, type:msg.type, ackedType:context.type, ackedSeq:context.seq, seq:msg.seq,
        reason:msg.type === MSG.NACK ? p[15] : undefined, windowSize:windowAck ? p[15] : 1, sliding:windowAck && bytesEqual(p.slice(3, 7), v2SlidingMagic)};
    }
    default: throw new Error('Unsupported protocol version');
  }
  if (v2ItemTypes.includes(msg.type)) validateV2Context({...msg, itemId, segment});
  return {...msg, itemId, segment, body, ...(windowSize === undefined ? {} : {windowSize, sliding})};
}

export function compareV2Offers(leftId, leftRole, rightId, rightRole) {
  requireUint32(leftId, 'itemId'); requireUint32(rightId, 'itemId');
  if (![1,2].includes(leftRole) || ![1,2].includes(rightRole)) throw new Error('invalid role');
  return Math.sign(leftId - rightId || leftRole - rightRole);
}

export function v2StreamLayout(headerLen, payloadSize) {
  if (!Number.isInteger(headerLen) || headerLen < 50 || headerLen > 65536) throw new Error('invalid stream header length');
  requireV2BigInt(payloadSize);
  const streamPlaintextBytes = BigInt(headerLen) + payloadSize;
  const count = (streamPlaintextBytes + 65535n) / 65536n;
  if (count > 0xffffffffn) throw new RangeError('segment count overflow');
  const totalCiphertextBytes = requireV2BigInt(streamPlaintextBytes + 16n * count);
  return {streamPlaintextBytes, segmentCount:Number(count), totalCiphertextBytes};
}

export function makeV2Meta({keyId, direction, itemId, headerLen, payloadSize}) {
  v2KeyBytes(keyId); directionByte(direction); requireUint32(itemId, 'itemId');
  const layout = v2StreamLayout(headerLen, payloadSize);
  return {kind:'encrypted-stream', protocolVersion:2, cryptoVersion:1, keyId, direction, itemId,
    totalCiphertextBytes:String(layout.totalCiphertextBytes), maxSegmentPlaintextBytes:65536,
    maxSegmentCiphertextBytes:65552, segmentCount:layout.segmentCount};
}

export function validateV2Meta(meta, expected) {
  // Recognizable old/plain schemas are incompatible; malformed current v2
  // fields below retain their precise validation errors.
  if (meta && (['text', 'attachment', 'encrypted'].includes(meta.kind) ||
      (meta.protocolVersion !== undefined && meta.protocolVersion !== 2))) {
    throw new Error('Unsupported protocol version');
  }
  const fields = ['kind','protocolVersion','cryptoVersion','keyId','direction','itemId',
    'totalCiphertextBytes','maxSegmentPlaintextBytes','maxSegmentCiphertextBytes','segmentCount'];
  if (!meta || Object.keys(meta).length !== fields.length || !fields.every(field => Object.hasOwn(meta, field))) throw new Error('unsupported META fields');
  if (meta.kind !== 'encrypted-stream' || meta.protocolVersion !== 2 || meta.cryptoVersion !== 1) throw new Error('Unsupported protocol version');
  v2KeyBytes(meta.keyId); directionByte(meta.direction); requireUint32(meta.itemId, 'itemId');
  requireUint32(meta.segmentCount, 'segmentCount');
  if (!meta.segmentCount || meta.maxSegmentPlaintextBytes !== 65536 || meta.maxSegmentCiphertextBytes !== 65552) throw new Error('invalid segment limits');
  if (!expected || ['keyId','direction','itemId'].some(field => meta[field] !== expected[field])) throw new Error('META context mismatch');
  const total = parseCanonicalUint64(meta.totalCiphertextBytes);
  const final = total - BigInt(meta.segmentCount - 1) * 65552n;
  if (final < 17n || final > 65552n || (meta.segmentCount === 1 && final < 66n)) throw new Error('inconsistent ciphertext total');
  return {...meta, totalCiphertextBytes:total};
}

export function encodeV2Header({kind, name, mimeType, payloadSize, payloadSha256}) {
  const kindByte = kind === 'text' ? 1 : kind === 'attachment' ? 2 : 0;
  if (!kindByte) throw new Error('invalid plaintext kind');
  const n = v2Text(name), m = v2Text(mimeType);
  requireUint16(n.length, 'name length'); requireUint16(m.length, 'MIME length');
  v2StreamLayout(50 + n.length + m.length, payloadSize);
  return concatBytes([v2Magic, new Uint8Array([2,kindByte,n.length>>8,n.length&255,m.length>>8,m.length&255]),
    u64Bytes(payloadSize), hexToBytes(payloadSha256), n, m]);
}

export function decodeV2Header(raw) {
  const p = normalizePayload(raw);
  if (p.length < 50 || !bytesEqual(p.slice(0,4),v2Magic) || p[4] !== 2 || ![1,2].includes(p[5])) throw new Error('invalid AB2S header');
  const n = (p[6]<<8)|p[7], m = (p[8]<<8)|p[9], headerLen = 50 + n + m;
  if (headerLen > 65536 || p.length < headerLen) throw new Error('truncated or oversized header');
  const payloadSize = readU64(p,10);
  v2StreamLayout(headerLen,payloadSize);
  return {kind:p[5]===1?'text':'attachment', name:v2FatalDecoder.decode(p.slice(50,50+n)),
    mimeType:v2FatalDecoder.decode(p.slice(50+n,headerLen)), payloadSize,
    payloadSha256:Array.from(p.slice(18,50),b=>b.toString(16).padStart(2,'0')).join(''), headerLen};
}

export function makeV2Iv(noncePrefix, itemId, segmentIndex) {
  const prefix = normalizePayload(noncePrefix);
  if (prefix.length !== 4) throw new Error('nonce prefix must be four bytes');
  return concatBytes([prefix, u32Bytes(itemId), u32Bytes(segmentIndex)]);
}

export function makeV2Aad({cryptoVersion, keyId, direction, itemId, segmentIndex, segmentPlaintextLen, finalFlag}) {
  if (cryptoVersion !== 1 || typeof finalFlag !== 'boolean') throw new Error('invalid segment crypto context');
  if (!Number.isInteger(segmentPlaintextLen) || segmentPlaintextLen < 1 || segmentPlaintextLen > 65536 || (!finalFlag && segmentPlaintextLen !== 65536)) throw new Error('invalid segment plaintext length');
  return concatBytes([ascii('AB2-AAD'),new Uint8Array([cryptoVersion]),v2KeyBytes(keyId),
    new Uint8Array([directionByte(direction)]),u32Bytes(itemId),u32Bytes(segmentIndex),
    u32Bytes(segmentPlaintextLen),new Uint8Array([Number(finalFlag)])]);
}

export function verifyV2Completion(meta, proof) {
  const parsed = validateV2Meta(meta,meta);
  const layout = v2StreamLayout(proof.headerLen,proof.payloadSize);
  requireV2BigInt(proof.payloadBytes); requireV2BigInt(proof.streamPlaintextBytes);
  if (proof.authenticatedSegments !== parsed.segmentCount || layout.segmentCount !== parsed.segmentCount ||
      layout.totalCiphertextBytes !== parsed.totalCiphertextBytes || proof.streamPlaintextBytes !== layout.streamPlaintextBytes ||
      proof.payloadBytes !== proof.payloadSize || !bytesEqual(hexToBytes(proof.payloadSha256),hexToBytes(proof.actualSha256))) {
    throw new Error('stream completion verification failed');
  }
  return true;
}

export class V2StopAndWait {
  constructor() { this.pending = null; this.capabilityItemId = null; this.windowSize = 1; this.sliding = false; }

  begin(frame, itemId = this.capabilityItemId) {
    if (this.pending) throw new Error('one outstanding frame allowed');
    const ownedFrame = new Uint8Array(normalizePayload(frame));
    const msg = parseV2Frame(ownedFrame,itemId);
    validateV2Context(msg);
    if (msg.type !== MSG.HELLO && msg.itemId !== this.capabilityItemId) throw new Error('AB2S capability proof required');
    this.pending = {type:msg.type, seq:msg.seq, itemId:msg.itemId, segment:msg.segment, frame:ownedFrame, windowSize:msg.windowSize ?? 1, sliding:msg.sliding ?? false};
  }

  receive(frame) {
    if (!this.pending) return 'ignored';
    const p = this.pending;
    const raw = parseV2Envelope(frame);
    if (p.type === MSG.HELLO) {
      // KEY acknowledgements retain their old shape, but cannot prove AB2S.
      if (raw.type === MSG.ACK && raw.payload.length === 3 &&
          [MSG.KEY_OFFER, MSG.KEY_REPLY, MSG.KEY_CONFIRM].includes(raw.payload[0])) return 'ignored';
      const standardProof = raw.payload.length === 15 && bytesEqual(raw.payload.slice(3, 7), v2Magic);
      const windowProof = raw.payload.length === 16 && (bytesEqual(raw.payload.slice(3, 7), v2WindowMagic) || bytesEqual(raw.payload.slice(3, 7), v2SlidingMagic));
      const missingProof = raw.type === MSG.ACK && raw.seq === 0 && !standardProof && !windowProof;
      const unsupported = raw.type === MSG.ERROR && raw.seq === 0 &&
        bytesEqual(raw.payload, ascii('Unsupported protocol version'));
      if (missingProof || unsupported) {
        this.abort();
        throw new Error('Unsupported protocol version');
      }
    }
    const msg = parseV2RoutingFrame(frame);
    if (msg.itemId !== p.itemId) return 'ignored';
    validateV2ControlBody(msg);
    if (msg.type === MSG.ERROR && bytesEqual(msg.body, ascii('Unsupported protocol version'))) {
      this.abort(); throw new Error('Unsupported protocol version');
    }
    if ([MSG.ERROR,MSG.BUSY,MSG.CANCEL].includes(msg.type)) {
      this.abort();
      return msg.type === MSG.ERROR ? 'error' : msg.type === MSG.BUSY ? 'busy' : 'cancel';
    }
    if (![MSG.ACK,MSG.NACK].includes(msg.type) || msg.ackedType !== p.type || msg.ackedSeq !== p.seq || msg.segment !== p.segment) return 'ignored';
    if (msg.type === MSG.NACK) return 'retry';
    if (p.type === MSG.HELLO) {
      if (msg.windowSize > p.windowSize || (msg.sliding && !p.sliding)) throw new Error('unrequested DATA window');
      this.capabilityItemId = p.itemId;
      this.windowSize = msg.windowSize;
      this.sliding = msg.sliding;
    }
    if ([MSG.CANCEL,MSG.ITEM_DONE].includes(p.type)) this.capabilityItemId = null;
    this.pending = null;
    return 'ack';
  }

  abort() { this.pending = null; this.capabilityItemId = null; this.windowSize = 1; this.sliding = false; }
}

// Parser/order state only: no ciphertext accumulation or decryption. Task 4/6
// supplies authenticated segment and hash receipts before verifyCompletion.
export class V2ReceiveState {
  constructor({keyId, direction, session = null}) {
    v2KeyBytes(keyId); directionByte(direction);
    this.keyId = keyId; this.direction = direction; this.session = session;
    this.highWaterId = 0;
    this.abort();
  }

  abort() {
    this.itemId = null; this.meta = null; this.metaFragments = [];
    this.segmentIndex = 0; this.chunkInSegment = 0; this.ciphertextBytes = 0n;
    this.last = null; this.verified = false;
  }

  accept(raw) {
    let msg;
    try {
      msg = parseV2RoutingFrame(raw,this.itemId);
      if ([MSG.ERROR,MSG.BUSY].includes(msg.type) && msg.itemId !== this.itemId) return {action:'stale'};
      validateV2ControlBody(msg);
    }
    catch (error) {
      if (parseMessage(raw)?.type === MSG.HELLO && error.message === 'Unsupported protocol version') {
        return {action:'error', frame:buildMessage(MSG.ERROR,0,ascii(error.message))};
      }
      this.abort(); throw error;
    }
    const context = `${msg.type}:${msg.seq}:${msg.itemId}:${msg.segment}`;
    if (this.last?.context === context) {
      if (!bytesEqual(this.last.payload,msg.payload)) { this.abort(); throw new Error('conflicting duplicate frame'); }
      return {action:'duplicate', frame:this.last.ack.slice()};
    }
    if (msg.type === MSG.HELLO) {
      if (msg.itemId <= this.highWaterId) return {action:'stale'};
      if (this.itemId !== null) return {action:'busy',frame:buildV2Frame(MSG.BUSY,0,msg.itemId)};
      this.session?.acceptStreamItemId(msg.itemId);
      this.abort(); this.highWaterId = msg.itemId; this.itemId = msg.itemId;
    } else if (this.itemId === null || msg.itemId !== this.itemId) {
      return {action:'stale'};
    } else {
      try {
        switch (msg.type) {
          case MSG.ITEM_META: this.acceptMeta(msg); break;
          case MSG.ITEM_DATA: this.acceptData(msg); break;
          case MSG.ITEM_DONE:
            if (!this.verified) throw new Error('completion verification required before DONE ACK');
            this.finish(); break;
          case MSG.CANCEL: this.finish(); break;
          case MSG.ERROR:
          case MSG.BUSY: this.abort(); return {action:msg.type === MSG.ERROR?'error':'busy'};
          default: throw new Error('unexpected v2 item frame');
        }
      } catch (error) { this.abort(); throw error; }
    }
    const ack = buildMessage(MSG.ACK,0,makeV2AckPayload(msg));
    this.last = {context, payload:msg.payload.slice(), ack};
    return {action:'ack',frame:ack};
  }

  acceptMeta(msg) {
    if (this.meta || msg.seq !== this.metaFragments.length) throw new Error('META order mismatch');
    const count = (msg.payload[0]<<8)|msg.payload[1];
    if (msg.seq >= count || (this.metaFragments.length && count !== ((this.metaFragments[0][0]<<8)|this.metaFragments[0][1]))) throw new Error('META fragment count mismatch');
    this.metaFragments.push(msg.payload.slice());
    if (this.metaFragments.length === count) {
      const meta = decodeMeta(this.metaFragments);
      this.meta = validateV2Meta(meta,{keyId:this.keyId,direction:this.direction,itemId:this.itemId});
      this.metaFragments = [];
    }
  }

  acceptData(msg) {
    if (!this.meta || msg.segment !== this.segmentIndex || msg.seq !== this.chunkInSegment || this.segmentIndex >= this.meta.segmentCount) throw new Error('DATA context/order mismatch');
    const segmentLength = this.segmentIndex === this.meta.segmentCount - 1
      ? toSafeV2Number(this.meta.totalCiphertextBytes - BigInt(this.segmentIndex)*65552n) : 65552;
    const offset = this.chunkInSegment * V2_DATA_SLICE_LEN;
    const expectedLength = Math.min(V2_DATA_SLICE_LEN,segmentLength-offset);
    if (expectedLength <= 0 || msg.body.length !== expectedLength) throw new Error('DATA slice length mismatch');
    this.ciphertextBytes += BigInt(msg.body.length);
    if (offset + msg.body.length === segmentLength) { this.segmentIndex++; this.chunkInSegment = 0; }
    else this.chunkInSegment++;
  }

  verifyCompletion(proof) {
    try {
      if (!this.meta || this.segmentIndex !== this.meta.segmentCount || this.ciphertextBytes !== this.meta.totalCiphertextBytes) throw new Error('incomplete ciphertext');
      verifyV2Completion({...this.meta,totalCiphertextBytes:String(this.meta.totalCiphertextBytes)},proof);
      this.verified = true;
    } catch (error) { this.abort(); throw error; }
  }

  finish() {
    this.itemId = null; this.meta = null; this.metaFragments = [];
    this.verified = false; this.ciphertextBytes = 0n;
  }
}

function v2SegmentLength(meta, index) {
  requireUint32(index, 'segment index');
  if (index >= meta.segmentCount) throw new Error('segment index outside item');
  return index === meta.segmentCount - 1
    ? toSafeV2Number(BigInt(meta.totalCiphertextBytes) - BigInt(index) * 65552n) : 65552;
}

function v2SegmentAlgorithm(session, meta, index) {
  const iv = makeV2Iv(session.noncePrefixes[meta.direction], meta.itemId, index);
  const additionalData = makeV2Aad({...meta, segmentIndex:index,
    segmentPlaintextLen:v2SegmentLength(meta, index) - 16, finalFlag:index === meta.segmentCount - 1});
  return {name:'AES-GCM', iv, additionalData, tagLength:128};
}

function v2SourceIterator(source) {
  const fileSource = typeof source.stream === 'function';
  const stream = typeof source.stream === 'function' ? source.stream() : source;
  const reader = typeof stream.getReader === 'function'
    ? (fileSource ? stream.getReader({mode:'byob'}) : stream.getReader()) : null;
  const iterator = reader ? null : stream[Symbol.asyncIterator]();
  let ended = false, closed = false, closing = null, cancelRead = null;
  return {
    async next(consume) {
      if (closed) throw new Error('inactive encrypted stream');
      const cancelled = new Promise(resolve => { cancelRead = resolve; });
      try {
        const result = await Promise.race([
          Promise.resolve().then(() => {
            if (closed) throw new Error('inactive encrypted stream');
            return reader ? (fileSource ? reader.read(new Uint8Array(65536)) : reader.read()) : iterator.next();
          }).then(step => {
            if (closed) throw new Error('inactive encrypted stream');
            ended = Boolean(step.done);
            return {value:consume(step)};
          }),
          cancelled,
        ]);
        if (result.cancelled) throw new Error('inactive encrypted stream');
        return result.value;
      } finally { cancelRead = null; }
    },
    close(cancel) {
      if (cancel) { closed = true; cancelRead?.({cancelled:true}); }
      if (!closing) closing = Promise.resolve().then(async () => {
        let error;
        try {
          if (reader) { if (!ended) await reader.cancel(); }
          else await iterator.return?.();
        } catch (failure) { error = failure; }
        if (reader) {
          try { reader.releaseLock(); } catch (failure) { error ??= failure; }
        }
        return {error};
      });
      return closing;
    },
  };
}

class V2EncryptedStream {
  #session; #keys; #source; #plain; #cipher = null; #used; #size;
  #owner = null; #input = null; #cleanupError = null;
  #started = false; #closed = false;
  constructor(session, header, source) {
    if (!session.isUnlocked()) throw new Error('crypto session is locked');
    const encoded = encodeV2Header(header);
    if (!source || (typeof source.stream !== 'function' && typeof source.getReader !== 'function' &&
        typeof source[Symbol.asyncIterator] !== 'function')) throw new Error('stream source required');
    if (source.locked === true) throw new Error('stream source is locked');
    if (typeof source.stream === 'function' &&
        (!Number.isSafeInteger(source.size) || BigInt(source.size) !== header.payloadSize)) throw new Error('file size mismatch');
    this.#session = session; this.#keys = session.keys; this.#source = source;
    this.#size = header.payloadSize;
    this.#plain = new Uint8Array(V2_SEGMENT_PLAINTEXT);
    this.#plain.set(encoded); this.#used = encoded.length;
    this.meta = Object.freeze(makeV2Meta({keyId:session.keyId, direction:directionForRole(session.role),
      itemId:session.allocateStreamItemId(), headerLen:encoded.length, payloadSize:header.payloadSize}));
    Object.defineProperty(this, 'meta', {writable:false});
  }
  assertActive() {
    if (this.#closed || !this.#session.isUnlocked() || this.#session.keys !== this.#keys ||
        this.#session.streamOutboundId !== this.meta.itemId) throw new Error('inactive encrypted stream');
  }
  snapshot() {
    return Object.freeze({plaintextBytes:this.#plain?.byteLength ?? 0, ciphertextBytes:this.#cipher?.byteLength ?? 0,
      sourceBytes:this.#input?.byteLength ?? 0});
  }
  get cleanupError() { return this.#cleanupError; }
  abort() {
    this.#closed = true; this.#plain = null; this.#cipher = null; this.#source = null; this.#keys = null; this.#input = null;
    this.#owner?.close(true).then(({error}) => { if (error) this.#cleanupError = error; });
  }
  [Symbol.asyncIterator]() {
    this.assertActive();
    if (this.#started) throw new Error('encrypted stream is single-use');
    this.#started = true;
    const iterator = this.#iterate();
    return {
      next:() => iterator.next(),
      return:value => { this.abort(); return iterator.return(value); },
      throw:error => { this.abort(); return iterator.throw(error); },
      [Symbol.asyncIterator]() { return this; },
    };
  }
  #readInput() {
    return this.#owner.next(step => {
      this.assertActive();
      if (step.done) return true;
      const bytes = normalizePayload(step.value);
      if (bytes.length > 65536) throw new Error('source chunk exceeds 65536 bytes');
      // Copy inside the synchronous consumption callback: no borrowed result or
      // caller backing buffer crosses the subsequent crypto/read await.
      this.#input = new Uint8Array(bytes);
      return false;
    });
  }
  async *#iterate() {
    let offset = 0, payloadBytes = 0n, complete = false, primaryFailed = false;
    try {
      this.assertActive();
      this.#owner = v2SourceIterator(this.#source);
      this.#source = null;
      for (let index = 0; index < this.meta.segmentCount; index++) {
        this.assertActive();
        const length = v2SegmentLength(this.meta, index) - 16;
        while (this.#used < length) {
          if (!this.#input) {
            const ended = await this.#readInput();
            this.assertActive();
            if (ended) throw new Error('incomplete payload stream');
            payloadBytes += BigInt(this.#input.length);
            if (payloadBytes > this.#size) throw new Error('payload size mismatch');
            offset = 0;
          }
          const take = Math.min(length - this.#used, this.#input.length - offset);
          this.#plain.set(this.#input.subarray(offset, offset + take), this.#used);
          this.#used += take; offset += take;
          if (offset === this.#input.length) this.#input = null;
        }
        if (this.#input) { this.#input = this.#input.slice(offset); offset = 0; }
        const final = index === this.meta.segmentCount - 1;
        if (final) {
          for (;;) {
            const ended = await this.#readInput();
            this.assertActive();
            if (ended) break;
            const extraLength = this.#input.length; this.#input = null;
            if (extraLength) throw new Error('payload size mismatch');
          }
          if (payloadBytes !== this.#size || this.#input) throw new Error('payload size mismatch');
        }
        const algorithm = v2SegmentAlgorithm(this.#session, this.meta, index);
        this.#cipher = new Uint8Array(await crypto.subtle.encrypt(algorithm,
          this.#keys[this.meta.direction], this.#plain.subarray(0, length)));
        this.assertActive();
        this.#used = 0;
        if (this.#input) {
          this.#plain.set(this.#input); this.#used = this.#input.length;
          this.#input = null;
        }
        if (final) this.#plain = null;
        yield Object.freeze({index, final, bytes:this.#cipher});
        this.#cipher = null;
      }
      complete = true;
    } catch (error) { primaryFailed = true; throw error;
    } finally {
      this.#input = null; this.#plain = null; this.#cipher = null;
      if (!complete) this.abort();
      const cleanup = await this.#owner?.close(!complete);
      this.#owner = null;
      if (cleanup?.error) {
        this.#cleanupError = cleanup.error;
        if (!primaryFailed) { this.#closed = true; throw cleanup.error; }
      }
    }
  }
}

// Input must already be authenticated. This parser owns only unfinished header
// bytes; returned payload views belong to the caller, never to a replay cache.
export class V2HeaderParser {
  #buffer = new Uint8Array(50); #used = 0; #header = null; #failed = false;
  get header() { return this.#header; }
  push(raw) {
    if (this.#failed) throw new Error('header parser aborted');
    const bytes = normalizePayload(raw);
    let offset = 0;
    try {
      while (!this.#header && offset < bytes.length) {
        const take = Math.min(this.#buffer.length - this.#used, bytes.length - offset);
        this.#buffer.set(bytes.subarray(offset, offset + take), this.#used);
        this.#used += take; offset += take;
        if (this.#used !== this.#buffer.length) continue;
        const n = (this.#buffer[6] << 8) | this.#buffer[7];
        const m = (this.#buffer[8] << 8) | this.#buffer[9];
        const length = 50 + n + m;
        v2StreamLayout(length, readU64(this.#buffer, 10));
        if (this.#buffer.length < length) {
          const expanded = new Uint8Array(length); expanded.set(this.#buffer); this.#buffer = expanded;
        } else {
          this.#header = Object.freeze(decodeV2Header(this.#buffer)); this.#buffer = null;
        }
      }
      return bytes.subarray(offset);
    } catch (error) { this.abort(); throw error; }
  }
  finish() {
    if (!this.#header || this.#failed) { this.abort(); throw new Error('incomplete authenticated header'); }
    return this.#header;
  }
  abort() { this.#buffer = null; this.#header = null; this.#failed = true; }
}

class V2WindowSegmentReceiver {
  #inner; #meta; #window; #sliding; #entries = new Map(); #batch = null;
  #segment = 0; #next = 0; #draining = false; #closed = false;
  constructor(session, meta, windowSize, sliding = false) {
    this.#sliding = sliding;
    this.#meta = validateV2Meta(meta, meta);
    this.#window = requireV2Window(windowSize);
    this.#inner = new V2SegmentReceiver(session, meta);
  }
  snapshot() {
    const inner = this.#inner.snapshot();
    return Object.freeze({...inner, retrySliceBytes:inner.retrySliceBytes +
      [...this.#entries.values()].reduce((total, entry) => total + entry.bytes.length, 0),
      windowFrames:this.#entries.size});
  }
  abort(error = new Error('window receiver aborted')) {
    this.#closed = true; this.#inner.abort();
    for (const entry of this.#entries.values()) if (!entry.done) entry.reject(error);
    this.#entries.clear();
  }
  async push(context, raw) {
    try {
      this.#inner.assertActive();
      requireUint32(context.segmentIndex, 'segment index');
      requireUint16(context.chunkInSegment, 'chunk index');
      if (context.itemId !== this.#meta.itemId) throw new Error('stale item DATA');
      let bytes = normalizePayload(raw);
      if (!bytes.length || bytes.length > V2_DATA_SLICE_LEN) throw new Error('invalid DATA slice length');
      const key = `${context.segmentIndex}:${context.chunkInSegment}`;
      const previous = this.#entries.get(key);
      if (previous) {
        if (!bytesEqual(previous.bytes, bytes)) throw new Error('conflicting duplicate DATA');
        raw = null; bytes = null; context = null;
        await previous.completion;
        this.#inner.assertActive();
        return {action:'duplicate'};
      }
      const base = this.#sliding ? this.#next : Math.floor(this.#next / this.#window) * this.#window;
      if (context.segmentIndex !== this.#segment || context.chunkInSegment < this.#next ||
          context.chunkInSegment >= base + this.#window || this.#segment >= this.#meta.segmentCount ||
          context.chunkInSegment >= Math.ceil(v2SegmentLength(this.#meta, this.#segment) / V2_DATA_SLICE_LEN)) {
        throw new Error('DATA outside receive window');
      }
      const batch = `${this.#segment}:${base}`;
      if (this.#sliding) {
        for (const [id, entry] of this.#entries) {
          if (entry.done && (entry.context.segmentIndex !== this.#segment ||
              entry.context.chunkInSegment < this.#next - this.#window)) this.#entries.delete(id);
        }
      } else if (batch !== this.#batch) {
        if ([...this.#entries.values()].some(entry => !entry.done)) throw new Error('incomplete DATA window');
        this.#entries.clear(); this.#batch = batch;
      }
      let resolve, reject;
      const accepted = new Promise((yes, no) => { resolve = yes; reject = no; });
      const completion = accepted.then(() => {});
      completion.catch(() => {});
      this.#entries.set(key, {context:{...context}, bytes:new Uint8Array(bytes), resolve, reject, completion, done:false});
      raw = null; bytes = null; context = null;
      void this.#drain();
      return await accepted;
    } catch (error) { this.abort(error); throw error; }
  }
  async #drain() {
    if (this.#draining || this.#closed) return;
    this.#draining = true;
    try {
      while (!this.#closed) {
        const entry = this.#entries.get(`${this.#segment}:${this.#next}`);
        if (!entry || entry.done) break;
        const result = await this.#inner.push(entry.context, entry.bytes);
        this.#inner.assertActive();
        entry.done = true; entry.resolve(result);
        this.#next++;
        if (this.#next === Math.ceil(v2SegmentLength(this.#meta, this.#segment) / V2_DATA_SLICE_LEN)) {
          this.#segment++; this.#next = 0;
        }
      }
    } catch (error) { this.abort(error); }
    finally { this.#draining = false; }
  }
  finish() {
    if ([...this.#entries.values()].some(entry => !entry.done)) throw new Error('incomplete DATA window');
    return this.#inner.finish();
  }
}

class V2SegmentReceiver {
  #session; #keys; #meta; #cipher = null; #last = null; #tail = null;
  #index = 0; #seq = 0; #offset = 0; #busy = false; #closed = false;
  #header = null; #payloadBytes = 0n;
  #authentication = null;
  constructor(session, meta) {
    if (!session.isUnlocked()) throw new Error('crypto session is locked');
    this.#meta = validateV2Meta(meta, {keyId:session.keyId, direction:oppositeDirection(session.role), itemId:meta.itemId});
    session.acceptStreamItemId(meta.itemId);
    this.#session = session; this.#keys = session.keys;
  }
  snapshot() {
    return Object.freeze({ciphertextBytes:this.#cipher?.byteLength ?? 0, retrySliceBytes:this.#tail?.byteLength ?? 0,
      authenticatedSegments:this.#index, payloadBytes:this.#payloadBytes});
  }
  abort() { this.#closed = true; this.#cipher = null; this.#tail = null; this.#last = null; this.#header = null; this.#keys = null; }
  assertActive() {
    if (this.#closed || !this.#session.isUnlocked() || this.#session.keys !== this.#keys ||
        this.#session.streamInboundId !== this.#meta.itemId) throw new Error('inactive segment receiver');
  }
  async push(context, raw) {
    this.assertActive();
    if (context.itemId !== this.#meta.itemId) throw new Error('stale item DATA');
    let bytes = normalizePayload(raw);
    let authenticating = false;
    try {
      requireUint32(context.segmentIndex, 'segment index');
      requireUint16(context.chunkInSegment, 'chunk index');
      if (bytes.length < 1 || bytes.length > 51) throw new Error('invalid DATA slice length');
      if (this.#last?.index === context.segmentIndex && this.#last.seq === context.chunkInSegment) {
        const previous = this.#tail ?? this.#cipher.subarray(this.#last.offset, this.#offset);
        if (!bytesEqual(previous, bytes)) throw new Error('conflicting duplicate DATA');
        raw = null; bytes = null; context = null;
        if (this.#authentication) await this.#authentication;
        this.assertActive();
        return {action:'duplicate'};
      }
      if (this.#busy) throw new Error('concurrent segment input');
      if (context.segmentIndex !== this.#index || context.chunkInSegment !== this.#seq) throw new Error('DATA order mismatch');
      const length = v2SegmentLength(this.#meta, this.#index);
      if (bytes.length !== Math.min(51, length - this.#offset)) throw new Error('DATA slice length mismatch');
      this.#tail = null;
      if (!this.#cipher) this.#cipher = new Uint8Array(length);
      this.#cipher.set(bytes, this.#offset);
      this.#last = {index:this.#index, seq:this.#seq, offset:this.#offset};
      this.#offset += bytes.length; this.#seq++;
      bytes = null; raw = null; context = null;
      if (this.#offset !== length) return {action:'accepted'};
      this.#busy = true;
      authenticating = true;
      this.#authentication = this.#authenticate();
      return await this.#authentication;
    } catch (error) { this.abort(); throw error; }
    finally { if (authenticating) { this.#busy = false; this.#authentication = null; } }
  }
  async #authenticate() {
    const algorithm = v2SegmentAlgorithm(this.#session, this.#meta, this.#index);
    const plain = new Uint8Array(await crypto.subtle.decrypt(algorithm, this.#keys[this.#meta.direction], this.#cipher));
    this.assertActive();
    this.#tail = this.#cipher.slice(this.#last.offset);
    this.#cipher = null;
    let payload = plain;
    if (this.#index === 0) {
      const parser = new V2HeaderParser();
      payload = parser.push(plain); this.#header = parser.finish();
      const layout = v2StreamLayout(this.#header.headerLen, this.#header.payloadSize);
      if (layout.totalCiphertextBytes !== this.#meta.totalCiphertextBytes || layout.segmentCount !== this.#meta.segmentCount) throw new Error('header META size mismatch');
    }
    this.#payloadBytes += BigInt(payload.length);
    this.#index++; this.#seq = 0; this.#offset = 0;
    return {action:'authenticated', header:this.#header, payload};
  }
  // A length/authentication receipt, NOT a hash verification or completed item.
  finish() {
    this.assertActive();
    if (this.#busy || this.#index !== this.#meta.segmentCount || this.#payloadBytes !== this.#header?.payloadSize) {
      this.abort(); throw new Error('incomplete authenticated stream');
    }
    return {...this.#header, authenticatedSegments:this.#index, payloadBytes:this.#payloadBytes,
      streamPlaintextBytes:BigInt(this.#header.headerLen) + this.#payloadBytes};
  }
}

export class ItemSender {
  #streamWait = null;
  #streamControls = new Set();
  #stream = null;

  streamSnapshot() {
    return Object.freeze({...this.#stream?.snapshot(),
      ciphertextBytes:this.#stream?.snapshot().ciphertextBytes ?? 0,
      frameBytes:[...this.#streamControls].reduce((total, slot) => total + (slot.wait.pending?.frame.byteLength ?? 0), 0)});
  }

  receiveStreamControl(raw) {
    for (const slot of this.#streamControls) {
      if (!slot.settle) continue;
      try {
        const result = slot.wait.receive(raw);
        if (result === 'ignored') continue;
        slot.settle({result});
      } catch (error) { slot.settle({error}); }
      return true;
    }
    return false;
  }

  abortEncryptedStream(error = new Error('encrypted send aborted')) {
    this.#stream?.abort();
    for (const slot of this.#streamControls) slot.settle?.({error});
  }

  async #sendStreamFrame(raw, wait = this.#streamWait) {
    const stream = this.#stream, slot = {wait, settle:null};
    let frame = new Uint8Array(64); frame.set(raw); raw = null;
    wait.begin(frame, stream.meta.itemId);
    frame = null;
    this.#streamControls.add(slot);
    try {
      for (let attempt = 0; attempt <= this.retries; attempt++) {
        stream.assertActive();
        let timer, written = false;
        const control = new Promise(resolve => { slot.settle = resolve; });
        const deadline = new Promise(resolve => {
          timer = setTimeout(() => resolve(written ? {result:'retry'} : {error:new Error('transport write timed out')}), this.timeoutMs);
        });
        try {
          // The transport owns its copy; it cannot mutate the retained retry bytes.
          const sent = Promise.resolve().then(() => {
            stream.assertActive();
            return this.sendFn(wait.pending.frame.slice());
          }).then(() => { stream.assertActive(); written = true; });
          const response = await Promise.race([sent.then(() => control), control.then(result => result.error ? result : sent.then(() => result)), deadline]);
          stream.assertActive();
          if (response.error) throw response.error;
          if (response.result === 'ack') return;
          if (response.result !== 'retry') throw new Error(`Peer stream ${response.result}`);
        } finally { clearTimeout(timer); slot.settle = null; }
      }
      throw new Error('stream retry exhausted');
    } finally { this.#streamControls.delete(slot); }
  }

  async sendEncryptedStream(stream, options = {}) {
    if (this.#stream) throw new Error('one encrypted stream allowed');
    stream.assertActive();
    this.#stream = stream; this.#streamWait = new V2StopAndWait();
    const itemId = stream.meta.itemId;
    try {
      await this.#sendStreamFrame(buildV2Frame(MSG.HELLO, 0, itemId, V2_CONTROL_SEGMENT, {windowSize:this.windowSize, sliding:this.sliding}));
      stream.assertActive();
      options.onProgress?.(0n);
      stream.assertActive();
      const fragments = encodeMeta(stream.meta);
      for (let seq = 0; seq < fragments.length; seq++) {
        await this.#sendStreamFrame(buildMessage(MSG.ITEM_META, seq, fragments[seq]));
      }
      for await (const segment of stream) {
        const windowSize = this.#streamWait.windowSize;
        if (this.#streamWait.sliding) {
          const pending = [];
          const acknowledge = async () => {
            const next = pending.shift();
            await next.sent;
            stream.assertActive();
            options.onProgress?.(BigInt(segment.index) * 65536n + BigInt(Math.min(next.end, segment.bytes.length - 16)));
            stream.assertActive();
          };
          for (let offset = 0; offset < segment.bytes.length; offset += V2_DATA_SLICE_LEN) {
            if (pending.length === windowSize) await acknowledge();
            const wait = new V2StopAndWait();
            wait.capabilityItemId = itemId;
            const sent = this.#sendStreamFrame(buildV2Frame(MSG.ITEM_DATA, offset / V2_DATA_SLICE_LEN, itemId,
              segment.index, segment.bytes.subarray(offset, offset + V2_DATA_SLICE_LEN)), wait);
            sent.catch(error => this.abortEncryptedStream(error));
            pending.push({sent, end:offset + V2_DATA_SLICE_LEN});
          }
          while (pending.length) await acknowledge();
          continue;
        }
        for (let offset = 0; offset < segment.bytes.length; offset += V2_DATA_SLICE_LEN * windowSize) {
          const batch = [];
          for (let index = 0; index < windowSize && offset + index * V2_DATA_SLICE_LEN < segment.bytes.length; index++) {
            const position = offset + index * V2_DATA_SLICE_LEN;
            const wait = windowSize === 1 ? this.#streamWait : new V2StopAndWait();
            wait.capabilityItemId = itemId;
            batch.push(this.#sendStreamFrame(buildV2Frame(MSG.ITEM_DATA, position / V2_DATA_SLICE_LEN, itemId,
              segment.index, segment.bytes.subarray(position, position + V2_DATA_SLICE_LEN)), wait));
          }
          await Promise.all(batch);
          stream.assertActive();
          options.onProgress?.(BigInt(segment.index) * 65536n + BigInt(Math.min(offset + V2_DATA_SLICE_LEN * windowSize, segment.bytes.length - 16)));
          stream.assertActive();
        }
      }
      await this.#sendStreamFrame(buildV2Frame(MSG.ITEM_DONE, 0, itemId));
    } catch (error) {
      this.abortEncryptedStream(error);
      throw error;
    } finally {
      stream.abort(); this.#streamWait.abort(); this.#streamWait = null; this.#stream = null;
    }
  }

  /**
   * @param {Function} sendFn Async callback that writes a Uint8Array frame.
   * @param {{timeoutMs?:number, retries?:number, windowSize?:number}} [options] ACK/NACK retry options.
   */
  constructor(sendFn, options = {}) {
    assertSendFn(sendFn);
    this.sendFn = sendFn;
    this.timeoutMs = options.timeoutMs ?? 2000;
    this.retries = options.retries ?? 3;
    this.windowSize = requireV2Window(options.windowSize ?? 1);
    this.sliding = Boolean(options.sliding);
    this.ackCallbacks = new Set();
    this.nackCallbacks = new Set();
    this.pendingAcks = new Map();
    this.pendingErrors = new Set();
    this.sentFrames = new Map();
    this.retryCounts = new Map();
  }

  /**
   * Register a callback fired for every accepted ACK.
   * @param {Function} callback Called with the acknowledged sequence, ACK message, and acknowledged type.
   * @returns {Function} Unsubscribe callback.
   */
  onAck(callback) {
    if (typeof callback !== 'function') throw new TypeError('callback must be a function');
    this.ackCallbacks.add(callback);
    return () => this.ackCallbacks.delete(callback);
  }

  /**
   * Register a callback fired for every NACK that references a retained frame.
   * @param {Function} callback Called with the NACK detail and raw NACK message.
   * @returns {Function} Unsubscribe callback.
   */
  onNack(callback) {
    if (typeof callback !== 'function') throw new TypeError('callback must be a function');
    this.nackCallbacks.add(callback);
    return () => this.nackCallbacks.delete(callback);
  }

  /**
   * Feed an inbound ACK message to the sender's retry machinery.
   * @param {{type:number,payload:Uint8Array}|Uint8Array|ArrayBuffer|ArrayBufferView} msgOrSeq ACK message or frame.
   * @returns {boolean} True when the ACK was accepted.
   */
  receiveAck(msgOrSeq) {
    const ack = readAckSeq(msgOrSeq);
    if (!ack) return false;

    const key = this.ackKey(ack.type, ack.seq);
    const waiters = this.pendingAcks.get(key);
    if (!waiters) return false;
    this.pendingAcks.delete(key);
    for (const callback of this.ackCallbacks) callback(ack.seq, msgOrSeq, ack.type);
    for (const waiter of waiters) waiter.resolve({ kind: 'ack', message: msgOrSeq });
    if (ack.type === MSG.ITEM_DONE) this.clearCurrentItemFrames();
    return true;
  }

  /**
   * Feed an inbound NACK message to the sender's retry machinery.
   * @param {{type:number,payload:Uint8Array}|Uint8Array|ArrayBuffer|ArrayBufferView} msgOrSeq NACK message or frame.
   * @returns {boolean} True when the NACK referenced a retained frame.
   */
  receiveNack(msgOrSeq) {
    const nack = readNackSeq(msgOrSeq);
    if (!nack) return false;

    const key = this.ackKey(nack.type, nack.seq);
    const frame = this.sentFrames.get(key);
    if (!frame) return false;
    for (const callback of this.nackCallbacks) callback(nack, msgOrSeq);

    const waiters = this.pendingAcks.get(key);
    if (waiters) {
      this.pendingAcks.delete(key);
      for (const waiter of waiters) waiter.resolve({ kind: 'nack', message: msgOrSeq, nack });
      return true;
    }

    const error = this.consumeRetry(key, nack);
    if (error) {
      this.abortPending(error);
      void this.sendRetryExhaustedError(nack.type, nack.seq, error);
      return true;
    }

    void this.sendFn(frame).catch(sendError => this.abortPending(sendError));
    return true;
  }

  /**
   * Reject every in-flight send after a peer reports a protocol error.
   * @param {{type?:number,payload?:Uint8Array}|Uint8Array|ArrayBuffer|ArrayBufferView|string|Error} msgOrError ERROR message, frame, or decoded reason.
   * @returns {boolean} True when an in-flight send was rejected.
   */
  receiveError(msgOrError) {
    let message;
    if (typeof msgOrError === 'string') {
      message = msgOrError;
    } else if (msgOrError instanceof Error) {
      message = msgOrError.message;
    } else {
      const parsed = msgOrError && typeof msgOrError.type === 'number' ? msgOrError : parseMessage(msgOrError);
      if (!parsed || parsed.type !== MSG.ERROR) return false;
      message = textDecoder.decode(parsed.payload);
    }

    if (this.pendingErrors.size === 0) return false;
    const error = new Error(message || 'Peer reported a protocol error');
    error.name = 'PeerProtocolError';
    this.abortPending(error);
    return true;
  }

  /**
   * Send HELLO with the outbound item ID and wait for its ACK.
   * @param {number} itemId Unsigned 32-bit item identifier.
   * @returns {Promise<void>}
   */
  async sendHello(itemId) {
    requireUint32(itemId, 'itemId');
    const payload = new Uint8Array(4);
    payload[0] = (itemId >>> 24) & 0xff;
    payload[1] = (itemId >>> 16) & 0xff;
    payload[2] = (itemId >>> 8) & 0xff;
    payload[3] = itemId & 0xff;
    await this.sendWithAck(buildMessage(MSG.HELLO, 0, payload), 0);
  }

  /**
   * Send fragmented item metadata and wait for ACK after each fragment.
   * @param {object} meta Metadata object to JSON-encode.
   * @returns {Promise<void>}
   */
  async sendItemMeta(meta) {
    const fragments = encodeMeta(meta);
    for (let seq = 0; seq < fragments.length; seq++) {
      await this.sendWithAck(buildMessage(MSG.ITEM_META, seq, fragments[seq]), seq);
    }
  }

  /**
   * Send item data chunks and wait for ACK after each chunk.
   * @param {Iterable<Uint8Array|ArrayBuffer|ArrayBufferView>} chunks Payload chunks, each <= 59 bytes.
   * @returns {Promise<void>}
   */
  async sendItemData(chunks) {
    let seq = 0;
    for (const chunk of chunks) {
      await this.sendWithAck(buildMessage(MSG.ITEM_DATA, seq, normalizePayload(chunk)), seq);
      seq++;
    }
  }

  /**
   * Send ITEM_DONE and wait for ACK sequence 0.
   * @returns {Promise<void>}
   */
  async sendItemDone() {
    await this.sendWithAck(buildMessage(MSG.ITEM_DONE, 0), 0);
  }

  async sendWithAck(frame, expectedSeq) {
    const message = parseMessage(frame);
    if (!message) throw new Error('Cannot send malformed protocol frame');
    const expectedType = message.type;
    const sequence = expectedSeq ?? message.seq;
    requireUint16(sequence, 'expectedSeq');
    const key = this.ackKey(expectedType, sequence);
    if (expectedType === MSG.HELLO) this.clearCurrentItemFrames();
    this.sentFrames.set(key, new Uint8Array(frame));

    let lastError;
    for (;;) {
      try {
        await this.waitForAck(this.sentFrames.get(key), expectedType, sequence);
        return;
      } catch (error) {
        if (error?.name === 'PeerProtocolError' || error?.name === 'RetryExhaustedError') {
          this.clearCurrentItemFrames();
          throw error;
        }
        lastError = error;
        const exhausted = this.consumeRetry(key, lastError);
        if (exhausted) {
          await this.sendRetryExhaustedError(expectedType, sequence, lastError);
          this.abortPending(exhausted);
          this.clearCurrentItemFrames();
          throw exhausted;
        }
        if (error?.name !== 'PeerNackError') await sleep(200);
      }
    }
  }

  async waitForAck(frame, expectedType, expectedSeq) {
    let timeoutId;
    let waiter;
    const ackPromise = new Promise((resolve, reject) => {
      waiter = { resolve, reject };
    });
    this.addPendingAck(expectedType, expectedSeq, waiter);
    this.pendingErrors.add(waiter);

    try {
      await this.sendFn(frame);
      const timeout = new Promise((_, reject) => {
        timeoutId = setTimeout(() => reject(new Error('ACK timeout')), this.timeoutMs);
      });
      const response = await Promise.race([ackPromise, timeout]);
      if (response?.kind === 'nack') {
        const error = new Error(`NACK for type ${response.nack.type}, seq ${response.nack.seq}, reason ${response.nack.reason}`);
        error.name = 'PeerNackError';
        error.nack = response.nack;
        throw error;
      }
    } finally {
      clearTimeout(timeoutId);
      this.removePendingAck(expectedType, expectedSeq, waiter);
      this.pendingErrors.delete(waiter);
    }
  }

  ackKey(type, seq) {
    return `${type}:${seq}`;
  }

  addPendingAck(type, seq, waiter) {
    const key = this.ackKey(type, seq);
    const waiters = this.pendingAcks.get(key) ?? new Set();
    waiters.add(waiter);
    this.pendingAcks.set(key, waiters);
  }

  removePendingAck(type, seq, waiter) {
    const key = this.ackKey(type, seq);
    const waiters = this.pendingAcks.get(key);
    if (!waiters) return;
    waiters.delete(waiter);
    if (waiters.size === 0) this.pendingAcks.delete(key);
  }

  clearCurrentItemFrames() {
    this.sentFrames.clear();
    this.retryCounts.clear();
  }

  consumeRetry(key, cause) {
    const retriesUsed = this.retryCounts.get(key) ?? 0;
    if (retriesUsed >= this.retries) {
      const exhausted = new Error(`Retry exhausted for ${key}`);
      exhausted.name = 'RetryExhaustedError';
      exhausted.cause = cause;
      return exhausted;
    }
    this.retryCounts.set(key, retriesUsed + 1);
    return null;
  }

  abortPending(error) {
    for (const waiter of [...this.pendingErrors]) waiter.reject(error);
    this.pendingAcks.clear();
    this.pendingErrors.clear();
    this.clearCurrentItemFrames();
  }

  async sendRetryExhaustedError(type, seq, cause) {
    if (!isItemDataFrameType(type)) return;
    const reason = `Retry exhausted for type ${type}, seq ${seq}: ${cause?.message || 'no ACK/NACK'}`;
    await this.sendFn(buildMessage(MSG.ERROR, 0, encodeErrorPayload(reason)));
  }
}

export { ItemReceiver } from './airbridge-item-receiver.js';

/* characterization-only:start */
export class LegacyItemReceiver {
  /**
   * @param {{sendFn?:Function, busy?:boolean, nackDelayMs?:number}} [options] Optional response sender and initial busy state.
   */
  constructor(options = {}) {
    if (options.sendFn) assertSendFn(options.sendFn);
    this.sendFn = options.sendFn ?? null;
    this.itemValidator = null;
    this.nackDelayMs = options.nackDelayMs ?? 750;
    this.busy = Boolean(options.busy);
    this.listeners = new Map();
    this.expectedFrame = null;
    this.expectedFrameTimer = null;
    this.pendingDone = null;
    this.itemGeneration = 0;
    this.doneValidation = null;
    this.resetItem();
  }

  /**
   * Register an event listener. Events include hello, metaFragment, meta, data,
   * done, item, ack, nack, busy, rejected, error, and invalid.
   * @param {string} event Event name.
   * @param {Function} callback Listener callback.
   * @returns {Function} Unsubscribe callback.
   */
  on(event, callback) {
    if (typeof callback !== 'function') throw new TypeError('callback must be a function');
    const callbacks = this.listeners.get(event) ?? new Set();
    callbacks.add(callback);
    this.listeners.set(event, callbacks);
    return () => callbacks.delete(callback);
  }

  /**
   * Handle one parsed message or raw frame, update receiver state, and emit events.
   * @param {{type:number,seq:number,len:number,payload:Uint8Array}|Uint8Array|ArrayBuffer|ArrayBufferView} msg Message object or raw frame.
   * @returns {Promise<void>}
   */
  async onMessage(msg) {
    const parsed = msg && typeof msg.type === 'number' ? msg : parseMessage(msg);
    if (!parsed) {
      this.armExpectedFrameNack(NACK_REASON.MALFORMED);
      this.emit('invalid', new Error('Malformed message'));
      return;
    }

    switch (parsed.type) {
      case MSG.HELLO:
        await this.handleHello(parsed);
        break;
      case MSG.ITEM_META:
        await this.handleMeta(parsed);
        break;
      case MSG.ITEM_DATA:
        await this.handleData(parsed);
        break;
      case MSG.ITEM_DONE:
        await this.handleDone(parsed);
        break;
      case MSG.ACK:
        this.emit('ack', { ...readAckSeq(parsed), message: parsed });
        break;
      case MSG.NACK:
        this.emit('nack', { ...readNackSeq(parsed), message: parsed });
        break;
      case MSG.BUSY:
        this.emit('busy', parsed);
        break;
      case MSG.CANCEL:
        this.resetItem();
        this.emit('cancel', parsed);
        await this.sendAck(MSG.CANCEL, 0);
        break;
      case MSG.ERROR:
        this.resetItem();
        this.emit('error', textDecoder.decode(parsed.payload));
        break;
      default:
        this.emit('invalid', new Error(`Unknown message type 0x${parsed.type.toString(16)}`));
    }
  }

  /**
   * Set whether inbound item-start messages should receive BUSY instead of ACK.
   * @param {boolean} value Busy state.
   * @returns {void}
   */
  setBusy(value) {
    this.busy = Boolean(value);
  }

  /**
   * Set an async validator that runs before ITEM_DONE is acknowledged. The
   * validator may return replacement item detail, such as decrypted plaintext.
   * @param {Function|null} validator Item validator, or null to clear it.
   * @returns {void}
   */
  setItemValidator(validator) {
    if (validator !== null && typeof validator !== 'function') throw new TypeError('validator must be a function or null');
    this.itemValidator = validator;
  }

  resetItem() {
    this.advanceItemGeneration();
    this.clearExpectedFrame();
    this.meta = null;
    this.metaFragments = new Map();
    this.expectedMetaFragments = 0;
    this.chunks = new Map();
    this.pendingDone = null;
  }

  advanceItemGeneration() {
    this.itemGeneration++;
    this.doneValidation = null;
  }

  async handleHello(msg) {
    if (this.busy) {
      await this.sendMessage(MSG.BUSY, 0);
      this.emit('rejected', msg);
      return;
    }
    this.resetItem();
    const payload = msg.payload;
    const itemId = payload.length >= 4
      ? (((payload[0] << 24) >>> 0) | (payload[1] << 16) | (payload[2] << 8) | payload[3]) >>> 0
      : null;
    this.emit('hello', { itemId, message: msg });
    await this.sendAck(MSG.HELLO, 0);
    this.setExpectedFrame(MSG.ITEM_META, 0);
  }

  async handleMeta(msg) {
    try {
      if (this.expectedFrame && this.expectedFrame.type !== MSG.ITEM_META) {
        await this.sendRecoverableNack(this.expectedFrame.type, this.expectedFrame.seq, NACK_REASON.MISSING);
        return;
      }
      if (this.expectedFrame?.type === MSG.ITEM_META && msg.seq !== this.expectedFrame.seq) {
        await this.rejectUnexpectedFrame(MSG.ITEM_META, msg.seq, msg.payload);
        return;
      }
      if (this.metaFragments.has(msg.seq)) {
        await this.ackOrNackDuplicate(MSG.ITEM_META, msg.seq, msg.payload, this.metaFragments.get(msg.seq));
        return;
      }
      if (msg.payload.length < META_PREFIX_LEN) {
        await this.sendRecoverableNack(MSG.ITEM_META, msg.seq, NACK_REASON.MALFORMED);
        return;
      }
      const total = (msg.payload[0] << 8) | msg.payload[1];
      if (total <= 0) {
        await this.sendRecoverableNack(MSG.ITEM_META, msg.seq, NACK_REASON.MALFORMED);
        return;
      }
      if (!this.expectedMetaFragments || msg.seq === 0) {
        this.expectedMetaFragments = total;
      }
      if (total !== this.expectedMetaFragments) {
        await this.sendRecoverableNack(MSG.ITEM_META, msg.seq, NACK_REASON.MALFORMED);
        return;
      }

      if (msg.seq >= total) {
        await this.sendRecoverableNack(MSG.ITEM_META, msg.seq, NACK_REASON.MALFORMED);
        return;
      }
      if (this.metaFragments.size === 0) this.advanceItemGeneration();
      this.metaFragments.set(msg.seq, msg.payload);
      this.emit('metaFragment', { seq: msg.seq, total, payload: msg.payload });

      if (this.metaFragments.size < this.expectedMetaFragments) {
        await this.sendAck(MSG.ITEM_META, msg.seq);
        this.setExpectedFrame(MSG.ITEM_META, this.nextMissingMetaSeq());
        return;
      }

      const ordered = [];
      for (let i = 0; i < this.expectedMetaFragments; i++) {
        if (!this.metaFragments.has(i)) {
          await this.sendAck(MSG.ITEM_META, msg.seq);
          this.setExpectedFrame(MSG.ITEM_META, i);
          return;
        }
        ordered.push(this.metaFragments.get(i));
      }

      this.meta = decodeMeta(ordered);
      const transferSize = this.meta.kind === 'encrypted' ? this.meta.encryptedSize : this.meta.size;
      if (this.meta.chunks > 4096) throw new Error('META chunk count exceeds 4096');
      if (transferSize > 4 * 1024 * 1024) throw new Error('META size exceeds 4 MiB');
      this.chunks = new Map();
      await this.sendAck(MSG.ITEM_META, msg.seq);
      this.setExpectedFrame(MSG.ITEM_DATA, 0);
      this.emit('meta', this.meta);
    } catch (error) {
      this.emit('error', error);
      this.resetItem();
      await this.sendError(error.message || 'Invalid metadata');
    }
  }

  async handleData(msg) {
    if (!this.meta) {
      if (this.expectedFrame?.type === MSG.ITEM_META) {
        await this.sendRecoverableNack(MSG.ITEM_META, this.expectedFrame.seq, NACK_REASON.MISSING);
        return;
      }
        this.emit('invalid', new Error('ITEM_DATA received before ITEM_META'));
        return;
      }
    if (this.expectedFrame && this.expectedFrame.type !== MSG.ITEM_DATA) {
      await this.sendRecoverableNack(this.expectedFrame.type, this.expectedFrame.seq, NACK_REASON.MISSING);
      return;
    }
    if (this.expectedFrame?.type === MSG.ITEM_DATA && msg.seq !== this.expectedFrame.seq) {
      await this.rejectUnexpectedFrame(MSG.ITEM_DATA, msg.seq, msg.payload);
      return;
    }
    if (this.chunks.has(msg.seq)) {
      await this.ackOrNackDuplicate(MSG.ITEM_DATA, msg.seq, msg.payload, this.chunks.get(msg.seq));
      return;
    }
    this.chunks.set(msg.seq, msg.payload);
    this.emit('data', { seq: msg.seq, payload: msg.payload, received: this.chunks.size, meta: this.meta });
    await this.sendAck(MSG.ITEM_DATA, msg.seq);
    this.setExpectedAfterData();
    if (this.pendingDone && this.expectedFrame?.type === MSG.ITEM_DONE) {
      const pendingDone = this.pendingDone;
      this.pendingDone = null;
      await this.handleDone(pendingDone);
    }
  }

  async handleDone(msg) {
    const generation = this.itemGeneration;
    try {
      if (msg.payload.length !== 0) {
        this.setExpectedFrame(MSG.ITEM_DONE, msg.seq, NACK_REASON.MALFORMED);
        this.armExpectedFrameNack(NACK_REASON.MALFORMED);
        return;
      }
      if (!this.meta) {
        if (this.expectedFrame?.type === MSG.ITEM_META) {
          await this.sendRecoverableNack(MSG.ITEM_META, this.expectedFrame.seq, NACK_REASON.MISSING);
          if (generation !== this.itemGeneration) return;
          return;
        }
        throw new Error('No metadata received');
      }
      if (this.expectedFrame && this.expectedFrame.type !== MSG.ITEM_DATA && this.expectedFrame.type !== MSG.ITEM_DONE) {
        await this.sendRecoverableNack(this.expectedFrame.type, this.expectedFrame.seq, NACK_REASON.MISSING);
        if (generation !== this.itemGeneration) return;
        return;
      }
      if (this.expectedFrame?.type === MSG.ITEM_DATA) {
        this.pendingDone = msg;
        this.armExpectedFrameNack(NACK_REASON.MISSING);
        return;
      }
      const expectedChunks = expectedDataChunks(this.meta) ?? this.chunks.size;
      const ordered = [];
      for (let i = 0; i < expectedChunks; i++) {
        if (!this.chunks.has(i)) {
          this.pendingDone = msg;
          this.setExpectedFrame(MSG.ITEM_DATA, i, NACK_REASON.MISSING);
          this.armExpectedFrameNack(NACK_REASON.MISSING);
          return;
        }
        ordered.push(this.chunks.get(i));
      }

      const data = concatChunks(ordered);
      const meta = this.meta;
      let validation = this.doneValidation;
      if (!validation || validation.generation !== generation || validation.seq !== msg.seq) {
        const validator = this.itemValidator;
        validation = {
          generation,
          seq: msg.seq,
          promise: this.validateCompletedItem(meta, data, validator, generation),
          shared: Boolean(validator),
          emitted: false,
        };
        if (validator) this.doneValidation = validation;
      }

      const item = await validation.promise;
      if (generation !== this.itemGeneration || item === null) return;
      await this.sendAck(MSG.ITEM_DONE, msg.seq);
      if (generation !== this.itemGeneration) return;
      this.clearExpectedFrame();
      this.emit('done', msg);
      if (generation !== this.itemGeneration) return;
      if (!validation.shared || !validation.emitted) {
        validation.emitted = true;
        this.emit('item', item);
      }
    } catch (error) {
      if (generation !== this.itemGeneration) return;
      this.emit('error', error);
      if (generation !== this.itemGeneration) return;
      this.resetItem();
      await this.sendError(error.message || 'Item validation failed');
    }
  }

  async validateCompletedItem(meta, data, validator, generation) {
    const hash = await sha256(data);
    if (generation !== this.itemGeneration) return null;
    const expectedHash = normalizeHash(meta.kind === 'encrypted' ? meta.encryptedSha256 : meta.hash);
    if (expectedHash && hash !== expectedHash) {
      throw new Error(`Hash mismatch: expected ${expectedHash}, got ${hash}`);
    }

    let item = { meta, data, hash };
    if (validator) {
      const validated = await validator(item);
      if (generation !== this.itemGeneration) return null;
      if (validated !== undefined) {
        if (!validated || typeof validated !== 'object') throw new TypeError('Item validator must return item detail or undefined');
        item = validated;
      }
    }
    return item;
  }

  async sendAck(ackedType, seq) {
    await this.sendMessage(MSG.ACK, 0, makeAckPayload(ackedType, seq));
  }

  async sendNack(nackedType, seq, reason = NACK_REASON.MISSING) {
    await this.sendMessage(MSG.NACK, 0, makeNackPayload(nackedType, seq, reason));
  }

  async sendRecoverableNack(nackedType, seq, reason) {
    this.setExpectedFrame(nackedType, seq, reason);
    await this.sendNack(nackedType, seq, reason);
  }

  async sendError(reason) {
    await this.sendMessage(MSG.ERROR, 0, encodeErrorPayload(reason));
  }

  async sendMessage(type, seq, payload) {
    if (!this.sendFn) return;
    await this.sendFn(buildMessage(type, seq, payload));
  }

  emit(event, detail) {
    const callbacks = this.listeners.get(event);
    if (!callbacks) return;
    for (const callback of callbacks) callback(detail);
  }

  setExpectedFrame(type, seq, reason = NACK_REASON.MISSING) {
    if (seq == null) {
      this.clearExpectedFrame();
      return;
    }
    this.clearExpectedFrame();
    this.expectedFrame = { type, seq, reason };
    this.armExpectedFrameNack(reason);
  }

  armExpectedFrameNack(reason) {
    if (!this.expectedFrame) return;
    clearTimeout(this.expectedFrameTimer);
    this.expectedFrame.reason = reason;
    this.expectedFrameTimer = setTimeout(() => {
      const expected = this.expectedFrame;
      if (expected) void this.sendNack(expected.type, expected.seq, expected.reason);
    }, this.nackDelayMs);
  }

  clearExpectedFrame() {
    clearTimeout(this.expectedFrameTimer);
    this.expectedFrameTimer = null;
    this.expectedFrame = null;
  }

  nextMissingMetaSeq() {
    for (let seq = 0; seq < this.expectedMetaFragments; seq++) {
      if (!this.metaFragments.has(seq)) return seq;
    }
    return null;
  }

  contiguousDataChunks() {
    let seq = 0;
    while (this.chunks.has(seq)) seq++;
    return seq;
  }

  setExpectedAfterData() {
    const expectedChunks = expectedDataChunks(this.meta);
    const contiguous = this.contiguousDataChunks();
    if (expectedChunks === null || contiguous < expectedChunks) {
      this.setExpectedFrame(MSG.ITEM_DATA, contiguous);
      return;
    }
    this.setExpectedFrame(MSG.ITEM_DONE, 0);
  }

  async rejectUnexpectedFrame(type, seq, payload) {
    if (seq < this.expectedFrame.seq) {
      const accepted = type === MSG.ITEM_META ? this.metaFragments.get(seq) : this.chunks.get(seq);
      if (accepted) {
        await this.ackOrNackDuplicate(type, seq, payload, accepted);
        return;
      }
    }
    await this.sendRecoverableNack(type, this.expectedFrame.seq, NACK_REASON.MISSING);
  }

  async ackOrNackDuplicate(type, seq, payload, acceptedPayload) {
    if (bytesEqual(payload, acceptedPayload)) {
      await this.sendAck(type, seq);
      return;
    }
    await this.sendRecoverableNack(type, seq, NACK_REASON.MALFORMED);
  }
}
/* characterization-only:end */
