import * as p from './airbridge-protocol.js';
import { check, fixture, item, start, complete, doneAcks } from './protocol-item-receiver-tests.js';

export function itemReceiverLifecycleTests(vendor) {
  const tests = [];
  for (const state of ['idle', 'hello', 'meta', 'data', 'done', 'terminal']) {
    for (const action of ['cancel', 'reset', 'peer-cancel', 'error']) tests.push([`Receiver ${action} at ${state}`, async () => {
      // Given each receiver state, with DONE held at its consumer boundary.
      const f = await fixture(vendor), t = await item(f.sender, new Uint8Array(70000));
      let release, entered;
      const held = new Promise(resolve => { release = resolve; });
      const ready = new Promise(resolve => { entered = resolve; });
      const delivered = [];
      f.receiver.on('item', async value => { entered(); await held; if (value.isCurrent()) delivered.push(value); });
      if (state !== 'idle') await f.receiver.onMessage(t.hello);
      if (state === 'meta') await f.receiver.onMessage(t.meta[0]);
      if (['data', 'done', 'terminal'].includes(state)) {
        for (const frame of t.meta) await f.receiver.onMessage(frame);
        for (const frame of (state === 'data' ? t.data.slice(0, 1300) : t.data)) await f.receiver.onMessage(frame);
      }
      let pending;
      if (state === 'done' || state === 'terminal') {
        pending = f.receiver.onMessage(t.done); await ready;
        if (state === 'terminal') { release(); await pending; }
      }
      const before = doneAcks(f.sent);
      // When invalidating, including repeated local cancellation/reset.
      switch (action) {
        case 'cancel': await f.receiver.cancel(); await f.receiver.cancel(); break;
        case 'reset': f.receiver.resetItem(); f.receiver.resetItem(); break;
        case 'peer-cancel': await f.receiver.onMessage(p.buildV2Frame(p.MSG.CANCEL, 0, t.id)); break;
        case 'error': await f.receiver.onMessage(p.buildV2Frame(p.MSG.ERROR, 0, t.id, undefined, 'peer failure')); break;
        default: throw new Error('unknown action');
      }
      release(); await pending;
      // Then no late ACK/consumer, all private storage released, and recovery succeeds.
      check(doneAcks(f.sent) === before, 'late DONE ACK');
      check(state === 'terminal' || delivered.length === 0, 'stale consumer');
      check(f.receiver.snapshot().plaintextBytes === 0n && f.receiver.snapshot().ciphertextBytes === 0, 'retained buffers');
      await complete(f.receiver, await item(f.sender));
      check(delivered.length === (state === 'terminal' ? 2 : 1), 'next item failed');
      f.receiver.dispose();
    }]);
  }
  for (const type of [p.MSG.ITEM_DATA, p.MSG.ITEM_DONE, p.MSG.CANCEL, p.MSG.ACK, p.MSG.NACK, p.MSG.ERROR]) {
    tests.push([`Receiver stale control ${type} preserves next item`, async () => {
      // Given a cancelled item and a subsequent active item.
      const f = await fixture(vendor), old = await item(f.sender);
      await start(f.receiver, old); await f.receiver.cancel();
      const next = await item(f.sender); await start(f.receiver, next);
      let frame;
      switch (type) {
        case p.MSG.ITEM_DATA: frame = old.data[0]; break;
        case p.MSG.ACK:
        case p.MSG.NACK: frame = p.buildMessage(type, 0, p.makeV2AckPayload(p.parseV2Frame(old.data[0]), type === p.MSG.NACK ? 1 : undefined)); break;
        case p.MSG.ERROR: frame = p.buildV2Frame(type, 0, old.id, undefined, 'old'); frame[frame.length - 1] = 255; break;
        default: frame = p.buildV2Frame(type, 0, old.id);
      }
      const generation = f.receiver.snapshot().generation;
      // When a stale frame arrives.
      await f.receiver.onMessage(frame);
      // Then the new item remains valid and finishes normally.
      check(f.receiver.snapshot().generation === generation, 'stale control invalidated new generation');
      for (const data of next.data) await f.receiver.onMessage(data);
      await f.receiver.onMessage(next.done);
      check(f.items.length === 1 && doneAcks(f.sent) === 1, 'new item contaminated');
      f.receiver.dispose();
    }]);
  }
  tests.push(['Receiver exact duplicates are idempotent including reentrant DONE', async () => {
    // Given an item consumer that immediately retries DONE.
    const f = await fixture(vendor), t = await item(f.sender);
    let retry;
    f.receiver.on('item', () => { retry = f.receiver.onMessage(t.done); });
    await start(f.receiver, t);
    for (const frame of t.data) { await f.receiver.onMessage(frame); await f.receiver.onMessage(frame); }
    // When completion is retried from the callback.
    await f.receiver.onMessage(t.done); await retry;
    // Then it invokes the consumer once and ACKs both copies.
    check(f.items.length === 1 && doneAcks(f.sent) === 2 && f.errors.length === 0, 'reentrant completion failed');
    f.receiver.dispose();
  }]);
  tests.push(['Receiver cleanup-only failure is terminal and preserves identity', async () => {
    // Given an authenticated receiver whose segment cleanup fails.
    const f = await fixture(vendor), t = await item(f.sender);
    const failure = new Error('cleanup-only');
    const open = f.session.openSegmentReceiver.bind(f.session);
    f.session.openSegmentReceiver = meta => {
      const segment = open(meta), abort = segment.abort.bind(segment);
      segment.abort = () => { abort(); throw failure; };
      return segment;
    };
    // When finishing the item.
    await complete(f.receiver, t);
    // Then failure is surfaced, not silently swallowed after generation invalidation.
    check(f.errors[0] === failure && doneAcks(f.sent) === 0 && f.sent.at(-1).type === p.MSG.ERROR, 'cleanup-only failure swallowed');
    f.receiver.dispose();
  }]);
  for (const mode of ['consumer', 'primary-cleanup', 'timeout', 'busy']) tests.push([`Receiver terminal boundary ${mode}`, async () => {
    // Given a receiver with one explicitly injected external failure boundary.
    let expire;
    const f = await fixture(vendor), t = await item(f.sender);
    const primary = new Error('primary'), cleanup = new Error('cleanup');
    if (mode === 'consumer') f.receiver.on('item', () => { throw primary; });
    if (mode === 'primary-cleanup') {
      const open = f.session.openSegmentReceiver.bind(f.session);
      f.session.openSegmentReceiver = meta => {
        const segment = open(meta), abort = segment.abort.bind(segment);
        segment.push = () => { throw primary; };
        segment.abort = () => { abort(); throw cleanup; };
        return segment;
      };
    }
    const timer = globalThis.setTimeout;
    if (mode === 'timeout') globalThis.setTimeout = callback => { expire = callback; return 0; };
    try {
      if (mode === 'busy') f.receiver.setBusy(true);
      await start(f.receiver, t);
    } finally { globalThis.setTimeout = timer; }
    // When the failure boundary executes.
    switch (mode) {
      case 'timeout': {
        const failed = new Promise(resolve => f.receiver.on('error', resolve));
        expire(); await failed; break;
      }
      case 'busy': break;
      default:
        for (const frame of t.data) await f.receiver.onMessage(frame);
        await f.receiver.onMessage(t.done);
    }
    // Then primary identity wins, timeout fails closed, or BUSY leaves the receiver idle.
    check(doneAcks(f.sent) === 0, 'failure ACKed DONE');
    if (mode === 'consumer' || mode === 'primary-cleanup') check(f.errors[0] === primary, 'primary identity lost');
    if (mode === 'primary-cleanup') check(f.receiver.cleanupError === cleanup, 'secondary identity lost');
    if (mode === 'busy') check(f.sent[0].type === p.MSG.BUSY && f.items.length === 0, 'busy accepted item');
    if (mode === 'timeout') check(f.sent.at(-1).type === p.MSG.ERROR, 'timeout silent');
    f.receiver.dispose();
  }]);
  return tests;
}
