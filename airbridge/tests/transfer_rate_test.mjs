import test from 'node:test';
import assert from 'node:assert/strict';
import {createTransferRateMeter} from '../web/airbridge-ui.js';

test('rate averages elapsed time rather than equally weighting ACK bursts', () => {
  let time = 0;
  const meter = createTransferRateMeter({now:() => time});
  assert.equal(meter.bps(), null);
  for (let second = 1; second <= 10; second++) {
    time = second * 1000;
    for (let ack = 1; ack <= 4; ack++) {
      meter.update(BigInt((second - 1) * 4096 + ack * 1024));
      meter.bps();
    }
    assert.equal(meter.bps(), 4096);
  }
  assert.equal(meter.averageBps(), 4096);
});

test('rate decays to zero during a stall without more progress events', () => {
  let time = 0;
  const meter = createTransferRateMeter({now:() => time});
  for (let i = 1; i <= 6; i++) {
    time += 500; meter.update(BigInt(i * 1024)); meter.bps();
  }
  assert.equal(meter.bps(), 2048);
  const rates = [];
  for (let i = 0; i < 6; i++) { time += 500; rates.push(meter.bps()); }
  assert(rates.every((rate, i) => !i || rate <= rates[i - 1]));
  assert.equal(rates.at(-1), 0);
});

test('duplicate progress, huge counters and phase reset cannot inflate speed', () => {
  let time = 0;
  const meter = createTransferRateMeter({now:() => time});
  const base = 2n ** 60n;
  meter.reset(base);
  time = 1000; meter.update(base + 1024n);
  assert.equal(meter.bps(), 1024);
  for (let i = 0; i < 100; i++) meter.update(base + 1024n);
  assert.equal(meter.bps(), 1024);
  time = 2000;
  assert.equal(meter.bps(), 512);
  meter.reset();
  assert.equal(meter.bps(), null);
  time = 3000; meter.update(2048n);
  assert.equal(meter.bps(), 2048);
  meter.update(0n);
  assert.equal(meter.bps(), null);
});
