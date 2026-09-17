import test from 'node:test';
import assert from 'node:assert/strict';
import {BlePackets, blePacketControl} from '../web/airbridge-ble-packets.js';

for (const count of [3, 2]) {
  test(`negotiates ${count} frames and preserves ordering, ownership and partial batches`, async () => {
    const writes = [], received = [];
    const packets = new BlePackets(async (bytes, control) => {
      if (control) {
        if (bytes.length > count * 64) throw new Error('MTU limit');
        packets.accept(bytes);
      } else writes.push(bytes.slice());
    }, bytes => received.push(bytes), {timeoutMs:10});
    await packets.negotiate();
    assert.equal(packets.batchSize, count);
    const input = Array.from({length:7}, (_, i) => new Uint8Array(64).fill(i + 1));
    const expected = input.map(x => x.slice());
    const sent = input.map(x => packets.send(x));
    input.forEach(x => x.fill(99));
    await Promise.all(sent);
    assert.equal(writes.length, Math.ceil(7 / count));
    writes.forEach(bytes => packets.accept(bytes));
    assert.deepEqual(received, expected);
    assert.equal(packets.pending, 0);
    packets.close();
  });
}

test('old FAP falls back without sending a commit', async () => {
  const writes = [];
  const packets = new BlePackets(async bytes => writes.push(bytes), () => {}, {timeoutMs:2});
  await packets.negotiate();
  assert.equal(packets.batchSize, 1);
  assert.deepEqual(writes.map(x => x.length), [192, 128]);
  await packets.send(new Uint8Array([1, 2, 3]));
  assert.deepEqual([...writes.at(-1)], [1, 2, 3]);
  packets.close();
});

test('a lost commit reply fails rather than falling back to incompatible framing', async () => {
  const packets = new BlePackets(async bytes => {
    if (bytes[5] === 1) packets.accept(bytes);
  }, () => {}, {timeoutMs:2});
  await assert.rejects(packets.negotiate(), /timed out/);
  packets.close();
});

test('an unsettled probe write cannot overlap a fallback connection', async () => {
  let writes = 0;
  const packets = new BlePackets(() => { writes++; return new Promise(() => {}); }, () => {}, {timeoutMs:2});
  await assert.rejects(packets.negotiate(), /timed out/);
  assert.equal(writes, 1);
  packets.close();
});

test('wrong nonce and truncated echo cannot enable batching', async () => {
  const packets = new BlePackets(async bytes => {
    const wrong = bytes.slice(); wrong[8] ^= 1;
    packets.accept(wrong);
    packets.accept(bytes.slice(0, 64));
  }, () => {}, {timeoutMs:2});
  await packets.negotiate();
  assert.equal(packets.batchSize, 1);
  packets.close();
});

test('disconnect rejects negotiation and queued writes, including an active batch', async () => {
  const packets = new BlePackets(async () => {}, () => {}, {timeoutMs:100});
  const negotiation = packets.negotiate();
  packets.close();
  await assert.rejects(negotiation, /closed/);
  let started;
  const active = new Promise(resolve => { started = resolve; });
  let release;
  const queued = new BlePackets(() => { started(); return new Promise(resolve => { release = resolve; }); }, () => {});
  const a = queued.send(new Uint8Array(64));
  const b = queued.send(new Uint8Array(64));
  const outcomes = Promise.allSettled([a, b]);
  await active;
  queued.close();
  assert((await outcomes).every(x => x.status === 'rejected'));
  release();
});

test('bounded queue and malformed multi-report packet fail closed', async () => {
  const packets = new BlePackets(() => new Promise(() => {}), () => assert.fail('unexpected frame'));
  const sends = Array.from({length:32}, () => packets.send(new Uint8Array(64)));
  const results = Promise.allSettled(sends);
  await assert.rejects(packets.send(new Uint8Array(64)), /queue full/);
  packets.accept(new Uint8Array(65));
  assert((await results).every(x => x.status === 'rejected'));
});

test('probe wire format is fixed and does not resemble an application frame', () => {
  const probe = blePacketControl(1, new Uint8Array([0, 1, 2, 3, 4, 5, 6, 7]), 2);
  assert.equal(probe.length, 128);
  assert.deepEqual([...probe.slice(0, 16)], [240,65,66,80,1,1,2,0,0,1,2,3,4,5,6,7]);
  assert(probe.slice(16).every(x => x === 165));
});
