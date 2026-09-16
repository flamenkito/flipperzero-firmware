import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createRequire } from 'node:module';
import * as p from '../web/airbridge-protocol.js';
import { OutboundTransfer } from '../web/airbridge-outbound.js';
import { ItemReceiver } from '../web/airbridge-item-receiver.js';
import { pair } from '../web/protocol-outbound-tests.js';

const hashImplementation = createRequire(import.meta.url)('../web/vendor/js-sha256-0.11.1.js');

test('DATA windows require an explicit bounded HELLO capability receipt', () => {
  for (const windowSize of [1, 2, 4]) {
    const hello = p.buildV2Frame(p.MSG.HELLO, 0, 7, p.V2_CONTROL_SEGMENT, windowSize);
    const parsed = p.parseV2Frame(hello);
    assert.equal(parsed.windowSize, windowSize);
    assert.deepEqual([...hello.slice(9)], windowSize === 1 ? [65,66,50,83] : [65,66,50,87,windowSize]);
    const wait = new p.V2StopAndWait();
    wait.begin(hello);
    assert.equal(wait.receive(p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload(parsed))), 'ack');
    assert.equal(wait.windowSize, windowSize);
  }
  for (const windowSize of [0, 3, 8, 256]) assert.throws(() => p.buildV2Frame(p.MSG.HELLO, 0, 7, p.V2_CONTROL_SEGMENT, windowSize));
  const requested = p.parseV2Frame(p.buildV2Frame(p.MSG.HELLO, 0, 7, p.V2_CONTROL_SEGMENT, 2));
  const wait = new p.V2StopAndWait();
  wait.begin(p.buildV2Frame(p.MSG.HELLO, 0, 7, p.V2_CONTROL_SEGMENT, 2));
  assert.throws(() => wait.receive(p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload({...requested, windowSize:4}))), /unrequested/);
  wait.abort(); wait.begin(p.buildV2Frame(p.MSG.HELLO, 0, 7, p.V2_CONTROL_SEGMENT, 4));
  assert.equal(wait.receive(p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload({...requested, windowSize:1}))), 'ack');
  assert.equal(wait.windowSize, 1);
  wait.begin(p.buildV2Frame(p.MSG.HELLO, 0, 8, p.V2_CONTROL_SEGMENT, 4));
  assert.throws(() => wait.receive(p.buildMessage(p.MSG.ERROR, 0, new TextEncoder().encode('Unsupported protocol version'))), /Unsupported protocol version/);
  assert.equal(wait.capabilityItemId, null);
});

async function exchange({windowSize = 4, role = 0, fault, size = 70000} = {}) {
  const sessions = await pair();
  const source = sessions[role], target = sessions[1-role];
  const input = Uint8Array.from({length:size}, (_, i) => (i * 73 + 19) & 255);
  const dataAttempts = new Map(), ackAttempts = new Map();
  const progress = [], errors = [], writes = [];
  let item, peakFrames = 0, peakRetryBytes = 0, transfer, releaseWindow;
  const firstWindow = new Promise(resolve => { releaseWindow = resolve; });
  const receiver = new ItemReceiver({session:target, hashImplementation, sendFn:async frame => {
    const msg = p.parseV2Frame(frame);
    if (msg.type === p.MSG.ACK && msg.ackedType === p.MSG.ITEM_DATA) {
      const key = `${msg.segment}:${msg.ackedSeq}`;
      const attempt = (ackAttempts.get(key) ?? 0) + 1; ackAttempts.set(key, attempt);
      if (fault === 'drop-ack' && key === '0:0' && attempt === 1) return;
      if (fault === 'drop-final-ack' && msg.segment === 0 && msg.ackedSeq === 1285 && attempt === 1) return;
      if (fault === 'conflicting-retry' && key === '0:0' && attempt === 1) return;
    }
    transfer.receive(frame);
  }});
  receiver.on('item', detail => { item = detail; });
  receiver.on('error', error => errors.push(error.message));
  let reordered = [], reorderTimer;
  function deliver(frame) {
    void receiver.onMessage(frame).then(() => {
      const snapshot = receiver.snapshot();
      peakRetryBytes = Math.max(peakRetryBytes, snapshot.retrySliceBytes);
    });
  }
  transfer = new OutboundTransfer({session:source, hashImplementation, windowSize, timeoutMs:40, retries:2,
    onProgress:detail => progress.push(detail), send:async frame => {
      const msg = p.parseV2Frame(frame, transfer.itemId);
      writes.push({type:msg.type, seq:msg.seq, segment:msg.segment});
      if (msg.type === p.MSG.ITEM_DATA) {
        const key = `${msg.segment}:${msg.seq}`;
        const attempt = (dataAttempts.get(key) ?? 0) + 1; dataAttempts.set(key, attempt);
        peakFrames = Math.max(peakFrames, [...dataAttempts.keys()].filter(key => !ackAttempts.has(key)).length);
        if (dataAttempts.size >= windowSize) releaseWindow();
        if (fault === 'cancel' && msg.seq === 0) return;
        if (fault === 'timeout' && msg.seq === 0) return;
        if (fault === 'transport-error' && msg.seq === 1) throw new Error('transport write failed');
        if (fault === 'drop-data' && key === '0:0' && attempt === 1) return;
        if (fault === 'conflicting-retry' && key === '0:0' && attempt === 2) frame[13] ^= 1;
        if (fault === 'tamper-auth' && key === '0:0') frame[13] ^= 1;
        if (fault === 'out-of-window' && key === '0:0') frame[2] = windowSize;
        if (fault === 'reorder') {
          reordered.push(frame.slice());
          reorderTimer ??= setImmediate(() => {
            const batch = reordered.reverse(); reordered = []; reorderTimer = null;
            for (const bytes of batch) deliver(bytes);
          });
          return;
        }
      }
      queueMicrotask(() => deliver(frame.slice()));
    }});
  try {
    const sent = transfer.sendFile(new File([input], 'window.bin', {type:'application/octet-stream'}));
    if (fault === 'cancel' || fault === 'receiver-cancel') {
      await firstWindow;
      if (fault === 'receiver-cancel') await receiver.cancel(); else transfer.cancel();
      await assert.rejects(sent);
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(item, undefined);
      assert.equal(receiver.snapshot().retrySliceBytes, 0);
    } else if (['conflicting-retry', 'tamper-auth', 'out-of-window', 'timeout', 'transport-error'].includes(fault)) {
      await assert.rejects(sent);
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(item, undefined);
      assert.ok(!writes.some(frame => frame.type === p.MSG.ITEM_DONE));
      if (fault === 'conflicting-retry') assert.ok(errors.some(error => /conflicting duplicate/.test(error)));
      if (fault === 'out-of-window') assert.ok(errors.some(error => /outside receive window/.test(error)));
      assert.equal(receiver.snapshot().retrySliceBytes, 0);
    } else {
      const result = await sent;
      assert.deepEqual(new Uint8Array(await item.blob.arrayBuffer()), input);
      assert.equal(result.payloadSha256, hashImplementation(input));
      assert.equal(item.hash, result.payloadSha256);
      assert.equal(errors.length, 0, errors.join('; '));
      const sending = progress.filter(event => event.phase === 'Sending');
      assert.equal(sending.at(-1).bytes, BigInt(size));
      assert.ok(sending.every((event, i) => !i || event.bytes >= sending[i-1].bytes));
      assert.ok(peakRetryBytes <= windowSize * 51 + 51, `retained ${peakRetryBytes} retry bytes`);
      assert.ok(peakFrames <= windowSize, `outstanding ${peakFrames}`);
      if (fault?.startsWith('drop-')) assert.ok([...dataAttempts.values()].some(count => count > 1), 'loss did not trigger retransmission');
    }
    return {writes, peakFrames, peakRetryBytes};
  } finally {
    if (reorderTimer) clearImmediate(reorderTimer);
    receiver.dispose(); source.clearKeys(); target.clearKeys();
  }
}

for (const windowSize of [2, 4]) for (const role of [0, 1]) {
  test(`window ${windowSize} role ${role} crosses a segment boundary with verified payload`, async () => {
    const result = await exchange({windowSize, role});
    assert.equal(result.peakFrames, windowSize);
  });
}
for (const fault of ['drop-data', 'drop-ack', 'drop-final-ack', 'reorder', 'conflicting-retry', 'cancel',
  'receiver-cancel', 'tamper-auth', 'out-of-window', 'timeout', 'transport-error']) {
  test(`window ${fault} preserves bounded delivery and cleanup`, () => exchange({fault}));
}
