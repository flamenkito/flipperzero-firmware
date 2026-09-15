import { MSG, buildMessage, buildV2Frame, makeV2AckPayload, parseV2RoutingFrame,
  parseV2Frame, parseMessage, decodeMeta, validateV2Meta, CRYPTO_STATE } from './airbridge-protocol.js';
import { createReceiveAccumulator } from './airbridge-receive-accumulator.js';

export class ItemReceiver {
  #session; #send; #hash; #listeners = new Map(); #unsubscribe;
  #generation = 0; #active = null; #highWater = 0; #receipt = null;
  #timer = null; #timeout; #busy = false; #disposed = false;
  state = 'idle'; cleanupError = null;
  constructor({session, sendFn, hashImplementation, timeoutMs = 30000}) {
    if (!session || typeof sendFn !== 'function') throw new TypeError('receiver session and sender required');
    this.#session = session; this.#send = sendFn; this.#hash = hashImplementation;
    this.#timeout = timeoutMs;
    this.#unsubscribe = session.onInvalidated(() => this.resetItem());
  }
  on(event, callback) {
    const listeners = this.#listeners.get(event) ?? new Set();
    listeners.add(callback); this.#listeners.set(event, listeners);
    return () => listeners.delete(callback);
  }
  async #emit(event, detail, generation = this.#generation) {
    for (const callback of this.#listeners.get(event) ?? []) {
      if (generation !== this.#generation) return;
      await callback(detail);
    }
  }
  setBusy(value) { this.#busy = Boolean(value); }
  snapshot() {
    const segment = this.#active?.segment?.snapshot();
    return Object.freeze({state:this.state, itemId:this.#active?.id ?? null, generation:this.#generation,
      ciphertextBytes:segment?.ciphertextBytes ?? 0, retrySliceBytes:segment?.retrySliceBytes ?? 0,
      receivedCiphertextBytes:this.#active?.receivedCiphertextBytes ?? 0n,
      plaintextBytes:this.#active?.accumulator?.snapshot().retainedBytes ?? 0n,
      authenticatedSegments:segment?.authenticatedSegments ?? 0});
  }
  #clear(state) {
    const active = this.#active;
    this.#active = null; this.#receipt = null; this.state = state; this.#generation++;
    clearTimeout(this.#timer); this.#timer = null;
    let failure;
    for (const owner of [active?.segment, active?.accumulator]) {
      try { owner?.abort(); } catch (error) { failure ??= error; }
    }
    if (failure) { this.cleanupError = failure; throw failure; }
  }
  resetItem() {
    if (!this.#active && !this.#receipt && this.state === 'idle') return;
    this.#clear('idle');
  }
  dispose() {
    if (this.#disposed) return;
    this.#disposed = true; this.#unsubscribe(); this.resetItem();
  }
  #arm() {
    clearTimeout(this.#timer);
    const generation = this.#generation, id = this.#active.id;
    this.#timer = setTimeout(() => {
      if (generation === this.#generation) void this.#fail(new Error('receive timeout'), id);
    }, this.#timeout);
  }
  async #fail(error, id = this.#active?.id) {
    try { this.#clear('terminal'); } catch (cleanup) { this.cleanupError = cleanup; }
    const incompatible = error.message === 'Unsupported protocol version';
    if (incompatible) this.#session.clearKeys(CRYPTO_STATE.ABORTED);
    const generation = this.#generation;
    if (id != null || incompatible) {
      try {
        const reason = incompatible ? error.message : 'Item receive failed';
        await this.#send(id == null ? buildMessage(MSG.ERROR, 0, new TextEncoder().encode(reason)) :
          buildV2Frame(MSG.ERROR, 0, id, undefined, reason));
      }
      catch (cleanup) { this.cleanupError ??= cleanup; }
    }
    try { await this.#emit('error', error, generation); }
    catch (cleanup) { this.cleanupError ??= cleanup; }
  }
  async cancel() {
    const id = this.#active?.id;
    if (id == null) return;
    const owner = Object.freeze({itemId:id, generation:this.#generation + 1});
    this.#clear('terminal');
    await this.#send(buildV2Frame(MSG.CANCEL, 0, id));
    await this.#emit('cancel', owner, owner.generation);
    return owner;
  }
  #ack(msg) { return this.#send(buildMessage(MSG.ACK, 0, makeV2AckPayload(msg))); }
  onMessage(raw) {
    if (this.#disposed) return Promise.resolve();
    let msg;
    try { msg = parseV2RoutingFrame(raw, this.#active?.id); }
    catch (error) {
      // A rejected HELLO has not established item-scoped control capability.
      return this.#fail(error, parseMessage(raw)?.type === MSG.HELLO ? null : this.#active?.id);
    }
    if ([MSG.ACK, MSG.NACK].includes(msg.type)) return Promise.resolve();
    if (msg.type !== MSG.HELLO && msg.itemId !== this.#active?.id) {
      if (msg.type === MSG.ITEM_DONE && msg.itemId === this.#receipt) return this.#ack(msg);
      if ([MSG.ERROR, MSG.BUSY].includes(msg.type)) return Promise.resolve();
      if (msg.itemId <= this.#highWater || !this.#active) {
        return this.#send(buildV2Frame(MSG.ERROR, 0, msg.itemId, undefined, 'Inactive item'));
      }
      return this.#fail(new Error('item context mismatch'));
    }
    const owner = {generation:this.#generation, itemId:this.#active?.id};
    try {
      if (msg.type !== MSG.ITEM_DATA) parseV2Frame(raw, this.#active?.id);
      let operation;
      switch (msg.type) {
        case MSG.HELLO: operation = this.#hello(msg, owner); break;
        case MSG.ITEM_META: operation = this.#meta(msg); break;
        case MSG.ITEM_DATA: operation = this.#data(msg); break;
        case MSG.ITEM_DONE: operation = this.#done(msg); break;
        case MSG.CANCEL: {
          this.#clear('terminal');
          const generation = this.#generation;
          return this.#ack(msg).then(() => this.#emit('cancel', {itemId:msg.itemId}, generation));
        }
        case MSG.ERROR: return this.#fail(new Error(new TextDecoder('utf-8', {fatal:true}).decode(msg.body)));
        case MSG.BUSY: return this.#fail(new Error('peer busy'));
        default: throw new Error('unexpected item frame');
      }
      msg = null;
      return operation.catch(error => {
        if (owner.generation === this.#generation) return this.#fail(error, owner.itemId);
      });
    } catch (error) {
      if (owner.generation === this.#generation) return this.#fail(error, owner.itemId);
      return Promise.resolve();
    }
  }
  async #hello(msg, owner) {
    if (!this.#session.isUnlocked()) throw new Error('crypto session locked');
    if (this.#active?.id === msg.itemId && this.state === 'hello') return this.#ack(msg);
    if (msg.itemId <= this.#highWater) return this.#send(buildV2Frame(MSG.ERROR, 0, msg.itemId, undefined, 'Inactive item'));
    if (this.#busy || this.#active) {
      await this.#send(buildV2Frame(MSG.BUSY, 0, msg.itemId));
      return this.#emit('rejected', {itemId:msg.itemId});
    }
    owner.generation = ++this.#generation; owner.itemId = msg.itemId;
    this.#receipt = null; this.#highWater = msg.itemId;
    this.#active = {id:msg.itemId, fragments:[], meta:null, segment:null, accumulator:null, pendingDone:null, receivedCiphertextBytes:0n};
    this.state = 'hello'; this.#arm();
    const generation = this.#generation;
    await this.#emit('hello', {itemId:msg.itemId});
    if (generation === this.#generation) await this.#ack(msg);
  }
  async #meta(msg) {
    const active = this.#active, generation = this.#generation;
    if (!active || !['hello', 'meta', 'data'].includes(this.state)) throw new Error('META order mismatch');
    const last = active.fragments.at(-1);
    if (msg.seq === active.fragments.length - 1 && last) {
      if (last.length !== msg.payload.length || last.some((b, i) => b !== msg.payload[i])) throw new Error('conflicting META duplicate');
      return this.#ack(msg);
    }
    if (active.meta || msg.seq !== active.fragments.length) throw new Error('META order mismatch');
    const count = (msg.payload[0] << 8) | msg.payload[1];
    if (msg.seq >= count || (last && count !== ((last[0] << 8) | last[1]))) throw new Error('META count mismatch');
    active.fragments.push(new Uint8Array(msg.payload)); this.state = 'meta';
    if (active.fragments.length === count) {
      const meta = decodeMeta(active.fragments);
      active.meta = validateV2Meta(meta, {keyId:this.#session.keyId,
        direction:this.#session.role === 1 ? 'ble-to-usb' : 'usb-to-ble', itemId:active.id});
      active.segment = this.#session.openSegmentReceiver(meta);
      active.accumulator = createReceiveAccumulator(this.#hash);
      this.state = 'data';
      await this.#emit('meta', Object.freeze({...meta}));
    }
    if (generation === this.#generation) { this.#arm(); await this.#ack(msg); }
  }
  async #data(msg) {
    const active = this.#active, generation = this.#generation;
    if (!active?.segment || this.state !== 'data') throw new Error('DATA before META or after DONE');
    active.fragments = [];
    const context = {type:msg.type, seq:msg.seq, itemId:msg.itemId, segment:msg.segment};
    const sliceBytes = msg.body.length;
    const pending = active.segment.push({itemId:msg.itemId, segmentIndex:msg.segment, chunkInSegment:msg.seq}, msg.body);
    msg = null;
    const result = await pending;
    if (generation !== this.#generation) return;
    // Count accepted stream bytes, independently of the bounded working buffer.
    // Retransmitted DATA is acknowledged again but must not advance progress.
    if (result.action !== 'duplicate') active.receivedCiphertextBytes += BigInt(sliceBytes);
    if (result.action === 'authenticated') active.accumulator.appendAuthenticated(result.payload);
    this.#arm();
    await this.#ack(context);
    if (generation !== this.#generation) return;
    await this.#emit('data', this.snapshot());
  }
  #done(msg) {
    const active = this.#active;
    if (!active?.segment || !['data', 'done'].includes(this.state)) throw new Error('DONE before DATA');
    if (active.pendingDone) return active.pendingDone.then(() => {
      if (this.#receipt === msg.itemId) return this.#ack(msg);
    });
    this.state = 'done';
    active.pendingDone = Promise.resolve().then(() => this.#finalize(active, msg));
    return active.pendingDone;
  }
  async #finalize(active, msg) {
    const generation = this.#generation;
    const header = active.segment.finish();
    const receipt = active.accumulator.finalize(header.payloadSize, header.payloadSha256, header.name, header.mimeType);
    const isCurrent = () => generation === this.#generation && this.#active === active;
    const detail = Object.freeze({...receipt, kind:header.kind, itemId:active.id, isCurrent});
    await this.#emit('item', detail);
    if (!isCurrent()) return;
    try { this.#clear('terminal'); }
    catch (error) { return this.#fail(error, active.id); }
    this.#receipt = active.id;
    const completedGeneration = this.#generation;
    try { await this.#ack(msg); }
    catch (error) {
      if (completedGeneration === this.#generation) return this.#fail(error, active.id);
    }
  }
}
