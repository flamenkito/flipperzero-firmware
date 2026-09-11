import * as p from './airbridge-protocol.js';
import { OutboundTransfer } from './airbridge-outbound.js';
import { check, fixture, item, start, complete, doneAcks } from './protocol-item-receiver-tests.js';

export function itemReceiverAuthTests(vendor) {
  const tests = [];
  for (const fault of ['aead', 'hash']) tests.push([`Receiver ${fault} failure creates zero Blobs`, async () => {
    // Given all source material before observing receiver-only Blob construction.
    const f = await fixture(vendor), t = await item(f.sender, new Uint8Array(70000), fault === 'hash' ? {payloadSha256:'00'.repeat(32)} : {});
    if (fault === 'aead') t.data.at(-1)[t.data.at(-1).length - 1] ^= 1;
    const NativeBlob = globalThis.Blob;
    let blobs = 0;
    globalThis.Blob = class extends NativeBlob { constructor(...args) { super(...args); blobs++; } };
    // When the final segment or final digest fails verification.
    try { await complete(f.receiver, t); } finally { globalThis.Blob = NativeBlob; }
    // Then even a transient final Blob is forbidden, not just its callback or URL.
    check(blobs === 0 && f.items.length === 0 && doneAcks(f.sent) === 0, 'unverified Blob allocated');
    f.receiver.dispose();
  }]);
  for (const fault of ['magic', 'utf8', 'size', 'kind']) tests.push([`Receiver authenticated header rejects ${fault}`, async () => {
    // Given genuinely authenticated but malformed plaintext headers.
    const f = await fixture(vendor);
    const encrypt = crypto.subtle.encrypt;
    crypto.subtle.encrypt = function(algorithm, key, input) {
      const bytes = new Uint8Array(input);
      switch (fault) {
        case 'magic': bytes[0] = 0; break;
        case 'utf8': bytes[50] = 255; break;
        case 'size': bytes[17]++; break;
        case 'kind': bytes[5] = 99; break;
        default: throw new Error('unknown fault');
      }
      return encrypt.call(this, algorithm, key, bytes);
    };
    let transfer;
    try { transfer = await item(f.sender); } finally { crypto.subtle.encrypt = encrypt; }
    // When the receiver authenticates and parses the header.
    await complete(f.receiver, transfer);
    // Then parsing fails before any Blob or callback, and a new item succeeds.
    check(f.items.length === 0 && doneAcks(f.sent) === 0 && f.errors.length === 1, 'invalid header completed');
    await complete(f.receiver, await item(f.sender));
    check(f.items.length === 1, 'header fault contaminated next item'); f.receiver.dispose();
  }]);
  for (const action of ['duplicate', 'conflict', 'reset', 'disconnect', 'cancel']) tests.push([`Receiver held authentication ${action}`, async () => {
    // Given final DATA suspended in real WebCrypto decryption.
    const f = await fixture(vendor), t = await item(f.sender);
    await start(f.receiver, t);
    for (const frame of t.data.slice(0, -1)) await f.receiver.onMessage(frame);
    let release, entered, calls = 0;
    const gate = new Promise(resolve => { release = resolve; });
    const ready = new Promise(resolve => { entered = resolve; });
    const decrypt = crypto.subtle.decrypt;
    crypto.subtle.decrypt = async function(...args) { calls++; entered(); await gate; return decrypt.apply(this, args); };
    const last = t.data.at(-1), before = f.sent.length;
    let pending, retry;
    try {
      pending = f.receiver.onMessage(last); await ready;
      check(f.sent.length === before && f.items.length === 0, 'DATA ACK before authentication acceptance');
      // When retrying or invalidating that in-flight slice.
      switch (action) {
        case 'duplicate': retry = f.receiver.onMessage(last); break;
        case 'conflict': { const bad = new Uint8Array(last); bad[bad.length - 1] ^= 1; retry = f.receiver.onMessage(bad); break; }
        case 'reset': f.receiver.resetItem(); break;
        case 'disconnect': f.receiver.dispose(); break;
        case 'cancel': await f.receiver.cancel(); break;
        default: throw new Error('unknown action');
      }
      release(); await Promise.all([pending, retry]);
    } finally { release(); crypto.subtle.decrypt = decrypt; }
    // Then one authentication is shared, or cancellation suppresses every late acceptance.
    check(calls === 1, 'duplicate decryption');
    const acks = f.sent.slice(before).filter(m => m.type === p.MSG.ACK && m.ackedType === p.MSG.ITEM_DATA);
    check(acks.length === (action === 'duplicate' ? 2 : 0), 'stale DATA ACK');
    if (action === 'duplicate') { await f.receiver.onMessage(t.done); check(f.items.length === 1, 'duplicate lost original'); }
    else check(f.items.length === 0 && f.receiver.snapshot().plaintextBytes === 0n, 'invalidated plaintext escaped');
    f.receiver.dispose();
  }]);
  for (const size of [0, 21, 4194369]) tests.push([`Receiver streaming verified Blob ${size}`, async () => {
    // Given the real sender, authenticator and accumulator, with generated source bytes.
    let sender, receiver, peaks = {ciphertext:0, slice:0, combined:0, plaintext:0n};
    const f = await fixture(vendor, {sendFn:async frame => { sender.receive(frame); }});
    receiver = f.receiver;
    sender = new OutboundTransfer({session:f.sender, hashImplementation:vendor, send:async frame => {
      await receiver.onMessage(frame);
      const snap = receiver.snapshot();
      peaks.ciphertext = Math.max(peaks.ciphertext, snap.ciphertextBytes);
      peaks.slice = Math.max(peaks.slice, snap.retrySliceBytes);
      peaks.combined = Math.max(peaks.combined, snap.ciphertextBytes + snap.retrySliceBytes);
      if (snap.plaintextBytes > peaks.plaintext) peaks.plaintext = snap.plaintextBytes;
    }});
    const bytes = Uint8Array.from({length:size}, (_, i) => i % 251);
    // When sending through both real AB2S endpoints without a frame collection.
    await sender.sendFile(new File([bytes], 'generated.bin', {type:'application/octet-stream'}));
    // Then the verified Blob is byte-identical with bounded unauthenticated retention.
    const result = new Uint8Array(await f.items[0].blob.arrayBuffer());
    check(result.length === size && result.every((byte, i) => byte === bytes[i]), 'Blob byte mismatch');
    check(peaks.ciphertext <= 65552 && peaks.slice <= 51 && peaks.combined <= 65603, 'unbounded ciphertext');
    check(peaks.plaintext === BigInt(size), 'dishonest plaintext ownership');
    console.info('Receiver memory', JSON.stringify({size, ...peaks, plaintext:String(peaks.plaintext)}));
    receiver.dispose();
  }]);
  return tests;
}
