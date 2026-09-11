import * as p from './airbridge-protocol.js';
import { pair } from './protocol-outbound-tests.js';

export function check(value, message) { if (!value) throw new Error(message); }
export async function fixture(vendor, options = {}) {
  const [sender, session] = await pair();
  const sent = [], items = [], errors = [];
  const receiver = new p.ItemReceiver({session, hashImplementation:vendor,
    sendFn:async frame => { sent.push(p.parseV2Frame(frame)); }, ...options});
  receiver.on('item', item => { items.push(item); });
  receiver.on('error', error => { errors.push(error); });
  return {sender, session, receiver, sent, items, errors};
}
export async function item(sender, bytes = new Uint8Array([1, 2, 3]), changes = {}) {
  const header = {kind:'attachment', name:'test.bin', mimeType:'application/octet-stream',
    payloadSize:BigInt(bytes.length), payloadSha256:await p.sha256(bytes), ...changes};
  const stream = sender.openEncryptedStream(header, new Blob([bytes]));
  const id = stream.meta.itemId;
  const hello = p.buildV2Frame(p.MSG.HELLO, 0, id);
  const meta = p.encodeMeta(stream.meta).map((body, seq) => p.buildV2Frame(p.MSG.ITEM_META, seq, id, undefined, body));
  const data = [];
  for await (const segment of stream) {
    for (let offset = 0, seq = 0; offset < segment.bytes.length; offset += 51, seq++) {
      data.push(p.buildV2Frame(p.MSG.ITEM_DATA, seq, id, segment.index, segment.bytes.subarray(offset, offset + 51)));
    }
  }
  return {id, hello, meta, data, done:p.buildV2Frame(p.MSG.ITEM_DONE, 0, id)};
}
export async function start(receiver, transfer) {
  await receiver.onMessage(transfer.hello);
  for (const frame of transfer.meta) await receiver.onMessage(frame);
}
export async function complete(receiver, transfer) {
  await start(receiver, transfer);
  for (const frame of transfer.data) await receiver.onMessage(frame);
  await receiver.onMessage(transfer.done);
}
export function doneAcks(sent) { return sent.filter(m => m.type === p.MSG.ACK && m.ackedType === p.MSG.ITEM_DONE).length; }

export function itemReceiverTests(vendor) {
  const tests = [];
  for (const stage of ['idle', 'hello', 'meta']) tests.push([`Receiver DATA before ready at ${stage}`, async () => {
    // Given an idle or incompletely negotiated item.
    const f = await fixture(vendor), t = await item(f.sender);
    if (stage !== 'idle') await f.receiver.onMessage(t.hello);
    if (stage === 'meta') await f.receiver.onMessage(t.meta[0]);
    // When DATA arrives before complete META.
    await f.receiver.onMessage(t.data[0]);
    // Then peer-visible failure releases all ownership and the next item succeeds.
    check(f.sent.at(-1)?.type === p.MSG.ERROR && f.items.length === 0, 'missing terminal order failure');
    check(f.receiver.snapshot().plaintextBytes === 0n, 'retained plaintext');
    await complete(f.receiver, await item(f.sender));
    check(f.items.length === 1, 'recovery failed');
    f.receiver.dispose();
  }]);
  for (const fault of ['item', 'segment', 'seq', 'short', 'long', 'aead', 'hash']) tests.push([`Receiver rejects ${fault} and recovers`, async () => {
    // Given valid metadata and one item.
    const f = await fixture(vendor), t = await item(f.sender, new Uint8Array(200), fault === 'hash' ? {payloadSha256:'00'.repeat(32)} : {});
    await start(f.receiver, t);
    // When an invalid frame or authenticated hash reaches the receiver.
    const frames = t.data.map(frame => new Uint8Array(frame));
    const index = fault === 'aead' ? frames.length - 1 : 0;
    const m = p.parseV2Frame(frames[index]);
    switch (fault) {
      case 'item': frames[index] = p.buildV2Frame(p.MSG.ITEM_DATA, m.seq, t.id + 5, m.segment, m.body); break;
      case 'segment': frames[index] = p.buildV2Frame(p.MSG.ITEM_DATA, m.seq, t.id, 1, m.body); break;
      case 'seq': frames[index] = p.buildV2Frame(p.MSG.ITEM_DATA, 1, t.id, 0, m.body); break;
      case 'short': frames[index] = p.buildV2Frame(p.MSG.ITEM_DATA, m.seq, t.id, m.segment, m.body.subarray(1)); break;
      case 'long': {
        const last = p.parseV2Frame(frames.at(-1));
        frames[frames.length - 1] = p.buildV2Frame(p.MSG.ITEM_DATA, last.seq, t.id, last.segment, new Uint8Array(last.body.length + 1)); break;
      }
      case 'aead': frames[index][frames[index].length - 1] ^= 1; break;
      case 'hash': break;
      default: throw new Error('unknown test fault');
    }
    for (const frame of frames) await f.receiver.onMessage(frame);
    await f.receiver.onMessage(t.done);
    // Then no verified item or DONE ACK exists, and the next item is uncontaminated.
    check(f.items.length === 0 && doneAcks(f.sent) === 0 && f.sent.some(m => m.type === p.MSG.ERROR), 'invalid item completed');
    await complete(f.receiver, await item(f.sender));
    check(f.items.length === 1, 'failure contaminated recovery');
    f.receiver.dispose();
  }]);
  tests.push(['Receiver verifies Blob before awaited item callback and DONE ACK', async () => {
    // Given complete authenticated input and a held consumer.
    let release, entered;
    const held = new Promise(resolve => { release = resolve; });
    const ready = new Promise(resolve => { entered = resolve; });
    const f = await fixture(vendor), t = await item(f.sender);
    f.receiver.on('item', async value => { check(value.blob instanceof Blob, 'verified Blob absent'); entered(); await held; });
    await start(f.receiver, t);
    for (const frame of t.data) await f.receiver.onMessage(frame);
    check(f.items.length === 0, 'premature consumer');
    // When DONE finalizes and waits for the consumer.
    const pending = f.receiver.onMessage(t.done);
    await ready;
    // Then DONE is not ACKed until the consumer succeeds.
    check(doneAcks(f.sent) === 0, 'early DONE ACK');
    release(); await pending;
    check(doneAcks(f.sent) === 1 && (await f.items[0].blob.arrayBuffer()).byteLength === 3, 'completion missing');
    f.receiver.dispose();
  }]);
  return tests;
}
