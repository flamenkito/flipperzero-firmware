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
});

export const MAX_PAYLOAD = 59;

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

export class ItemSender {
  /**
   * @param {Function} sendFn Async callback that writes a Uint8Array frame.
   * @param {{timeoutMs?:number, retries?:number}} [options] ACK/retry options.
   */
  constructor(sendFn, options = {}) {
    assertSendFn(sendFn);
    this.sendFn = sendFn;
    this.timeoutMs = options.timeoutMs ?? 5000;
    this.retries = options.retries ?? 3;
    this.ackCallbacks = new Set();
    this.pendingAcks = new Map();
    this.pendingErrors = new Set();
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
    for (const waiter of waiters) waiter.resolve(msgOrSeq);
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
    for (const waiter of [...this.pendingErrors]) waiter.reject(error);
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

    let lastError;
    for (let attempt = 1; attempt <= this.retries; attempt++) {
      try {
        await this.waitForAck(frame, expectedType, sequence);
        return;
      } catch (error) {
        if (error?.name === 'PeerProtocolError') throw error;
        lastError = error;
        if (attempt < this.retries) await sleep(200);
      }
    }
    throw lastError;
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
      await Promise.race([ackPromise, timeout]);
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
}

export class ItemReceiver {
  /**
   * @param {{sendFn?:Function, busy?:boolean}} [options] Optional ACK sender and initial busy state.
   */
  constructor(options = {}) {
    if (options.sendFn) assertSendFn(options.sendFn);
    this.sendFn = options.sendFn ?? null;
    this.busy = Boolean(options.busy);
    this.listeners = new Map();
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
        this.emit('nack', parsed);
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

  resetItem() {
    this.meta = null;
    this.metaFragments = new Map();
    this.expectedMetaFragments = 0;
    this.chunks = new Map();
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
  }

  async handleMeta(msg) {
    try {
      if (msg.payload.length < META_PREFIX_LEN) throw new Error('META fragment too short');
      const total = (msg.payload[0] << 8) | msg.payload[1];
      if (total <= 0) throw new Error('META fragment count is invalid');
      if (!this.expectedMetaFragments || msg.seq === 0) {
        this.expectedMetaFragments = total;
        this.metaFragments = new Map();
      }
      if (total !== this.expectedMetaFragments) throw new Error('META fragment count changed');

      if (msg.seq >= total) throw new Error('META fragment sequence is invalid');
      this.metaFragments.set(msg.seq, msg.payload);
      this.emit('metaFragment', { seq: msg.seq, total, payload: msg.payload });

      if (this.metaFragments.size < this.expectedMetaFragments) {
        await this.sendAck(MSG.ITEM_META, msg.seq);
        return;
      }

      const ordered = [];
      for (let i = 0; i < this.expectedMetaFragments; i++) {
        if (!this.metaFragments.has(i)) {
          await this.sendAck(MSG.ITEM_META, msg.seq);
          return;
        }
        ordered.push(this.metaFragments.get(i));
      }

      this.meta = decodeMeta(ordered);
      if (this.meta.chunks > 4096) throw new Error('META chunk count exceeds 4096');
      if (this.meta.size > 4 * 1024 * 1024) throw new Error('META size exceeds 4 MiB');
      this.chunks = new Map();
      await this.sendAck(MSG.ITEM_META, msg.seq);
      this.emit('meta', this.meta);
    } catch (error) {
      this.emit('error', error);
      this.resetItem();
      await this.sendError(error.message || 'Invalid metadata');
    }
  }

  async handleData(msg) {
    if (!this.meta) {
      this.emit('invalid', new Error('ITEM_DATA received before ITEM_META'));
      return;
    }
    if (!this.chunks.has(msg.seq)) this.chunks.set(msg.seq, msg.payload);
    this.emit('data', { seq: msg.seq, payload: msg.payload, received: this.chunks.size, meta: this.meta });
    await this.sendAck(MSG.ITEM_DATA, msg.seq);
  }

  async handleDone(msg) {
    try {
      if (!this.meta) throw new Error('No metadata received');
      const expectedChunks = this.meta.chunks ?? this.chunks.size;
      const ordered = [];
      for (let i = 0; i < expectedChunks; i++) {
        if (!this.chunks.has(i)) throw new Error(`Missing chunk ${i}`);
        ordered.push(this.chunks.get(i));
      }

      const data = concatChunks(ordered);
      const hash = await sha256(data);
      const expectedHash = normalizeHash(this.meta.hash);
      if (expectedHash && hash !== expectedHash) {
        throw new Error(`Hash mismatch: expected ${expectedHash}, got ${hash}`);
      }

      await this.sendAck(MSG.ITEM_DONE, msg.seq);
      this.emit('done', msg);
      this.emit('item', { meta: this.meta, data, hash });
    } catch (error) {
      this.emit('error', error);
      await this.sendError(error.message || 'Item validation failed');
    }
  }

  async sendAck(ackedType, seq) {
    await this.sendMessage(MSG.ACK, 0, makeAckPayload(ackedType, seq));
  }

  async sendError(reason) {
    const message = String(reason || 'Protocol error');
    let truncated = '';
    let payload = new Uint8Array(0);
    for (const character of message) {
      const candidate = textEncoder.encode(`${truncated}${character}`);
      if (candidate.length > MAX_PAYLOAD) break;
      truncated += character;
      payload = candidate;
    }
    if (payload.length === 0) payload = textEncoder.encode('Protocol error');
    await this.sendMessage(MSG.ERROR, 0, payload);
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
}
