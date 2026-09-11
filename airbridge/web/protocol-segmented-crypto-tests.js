import * as p from './airbridge-protocol.js';
import { createIncrementalSha256 } from './airbridge-incremental-sha256.js';

const enc = new TextEncoder();
function check(value, message = 'segmented crypto assertion failed') {
  if (!value) throw new Error(message);
}
function equal(a, b) {
  check(a.length === b.length && a.every((v, i) => v === b[i]), 'byte mismatch');
}
async function rejects(fn, pattern = null) {
  try { await fn(); } catch (error) {
    check(error instanceof Error || error instanceof DOMException, 'non-error rejection');
    if (pattern) check(pattern.test(error.message), `unexpected rejection: ${error.message}`);
    return error;
  }
  throw new Error('expected rejection');
}
async function pair() {
  const sessions = [];
  for (const role of [p.CRYPTO_ROLE.USB, p.CRYPTO_ROLE.BLE]) {
    sessions.push(new p.AirBridgeCryptoSession({role, sendFn: async frame => {
      const peer = sessions[role === 1 ? 1 : 0], msg = p.parseMessage(frame);
      if (msg.type === p.MSG.ACK) peer.receiveAck(msg);
      else await peer.onMessage(msg);
    }}));
  }
  await sessions[1].start(); await sessions[0].start();
  check(sessions[0].sas === sessions[1].sas, 'SAS mismatch');
  await sessions[0].acceptSas(); await sessions[1].acceptSas();
  return sessions;
}
function header(size, kind = 'attachment', name = '', mimeType = '') {
  return {kind, name, mimeType, payloadSize: BigInt(size), payloadSha256: '00'.repeat(32)};
}
async function* generated(size) {
  const scratch = new Uint8Array(8191);
  for (let offset = 0; offset < size;) {
    const length = Math.min(scratch.length, size - offset);
    for (let i = 0; i < length; i++) scratch[i] = (offset + i) % 251;
    offset += length;
    yield scratch.subarray(0, length);
  }
}
async function feed(receiver, segment, itemId) {
  let result;
  for (let offset = 0, seq = 0; offset < segment.bytes.length; offset += 51, seq++) {
    result = await receiver.push({itemId, segmentIndex: segment.index, chunkInSegment: seq},
      segment.bytes.subarray(offset, offset + 51));
  }
  return result;
}

async function generatedHeader(size, vendor, kind = 'attachment') {
  const hash = createIncrementalSha256(vendor);
  for await (const bytes of generated(size)) hash.update(bytes);
  return {...header(size, kind), payloadSha256:Array.from(hash.finalize(), b => b.toString(16).padStart(2, '0')).join('')};
}

async function promptly(promise) {
  let timer;
  try { return await Promise.race([promise, new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error('operation did not settle within 300ms')), 300);
  })]); } finally { clearTimeout(timer); }
}

export function segmentedCryptoTests(vendor) {
  const tests = [];
  for (const [label, size, kind] of [
    ['empty', 0, 'attachment'], ['text', 19, 'text'], ['attachment', 1031, 'attachment'],
    ['exact segment', 65486, 'attachment'], ['segment plus one', 65487, 'attachment'],
    ['multi-segment', 196700, 'attachment'], ['generated >4 MiB', 4194369, 'attachment'],
  ]) tests.push([`AB2S AEAD ${label} stream`, async () => {
    // Given a confirmed real ECDH pair and a bounded generated source.
    const [usb, ble] = await pair();
    const stream = usb.openEncryptedStream(await generatedHeader(size, vendor, kind), generated(size));
    const receiver = ble.openSegmentReceiver(stream.meta);
    const hash = createIncrementalSha256(vendor);
    let count = 0, bytes = 0, cipherBytes = 0;
    // When each independently encrypted segment is reconstructed from DATA slices.
    for await (const segment of stream) {
      check(segment.bytes.length <= 65552, 'ciphertext bound');
      check(segment.bytes.length === (segment.final ? Number(BigInt(stream.meta.totalCiphertextBytes) - BigInt(count) * 65552n) : 65552));
      const result = await feed(receiver, segment, stream.meta.itemId);
      check(result.action === 'authenticated', 'missing authentication');
      hash.update(result.payload);
      for (const byte of result.payload) check(byte === bytes++ % 251, 'payload ordering');
      cipherBytes += segment.bytes.length; count++;
    }
    // Then exact payload/segment totals hold, without claiming final hash/Blob completion.
    check(bytes === size && count === stream.meta.segmentCount);
    check(BigInt(cipherBytes) === BigInt(stream.meta.totalCiphertextBytes));
    check(receiver.finish().payloadBytes === BigInt(size));
    check(p.verifyV2Completion(stream.meta, {...receiver.finish(),
      actualSha256:Array.from(hash.finalize(), b => b.toString(16).padStart(2, '0')).join('')}));
    check(receiver.snapshot().ciphertextBytes === 0);
    check(stream.snapshot().ciphertextBytes === 0 && stream.snapshot().plaintextBytes === 0);
  }]);
  tests.push(['AB2S AEAD parser fixed fields and UTF-8 splits', async () => {
    // Given an authenticated header with a multibyte name at plaintext offset 50.
    const h = header(3, 'attachment', '€😀é', '文/plain');
    const encoded = p.encodeV2Header(h);
    for (let split = 1; split < encoded.length; split++) {
      const parser = new p.V2HeaderParser();
      // When the split falls at every byte, including fixed fields and UTF-8 interiors.
      check(parser.push(encoded.subarray(0, split)).length === 0);
      check(parser.push(encoded.subarray(split)).length === 0);
      // Then only the complete, fatal-decoded header is exposed.
      check(parser.finish().name === h.name && parser.header.mimeType === h.mimeType);
      equal(parser.push(new Uint8Array([1, 2, 3])), new Uint8Array([1, 2, 3]));
    }
    for (const length of [0, 4, 5, 6, 7, 8, 9, 10, 17, 18, 49, 50, encoded.length - 1]) {
      const parser = new p.V2HeaderParser(); parser.push(encoded.subarray(0, length));
      await rejects(() => parser.finish(), /incomplete/);
    }
    for (const offset of [0, 4, 5, 50]) {
      const bad = encoded.slice(); bad[offset] = 255;
      await rejects(() => { const parser = new p.V2HeaderParser(); parser.push(bad); parser.finish(); });
    }
    const max = p.encodeV2Header(header(0, 'attachment', 'a'.repeat(65486)));
    check(max.length === 65536);
    await rejects(() => p.encodeV2Header(header(0, 'attachment', 'a'.repeat(65487))), /header/);
  }]);
  tests.push(['AB2S AEAD metadata ciphertext sliced at 51 bytes', async () => {
    // Given metadata whose first UTF-8 sequence straddles ciphertext offsets 50/51.
    const [usb, ble] = await pair();
    const h = header(3, 'attachment', '€'.repeat(80), 'text/文');
    const stream = usb.openEncryptedStream(h, generated(3));
    const receiver = ble.openSegmentReceiver(stream.meta);
    // When reconstructing authenticated segment zero through exact 51-byte slices.
    for await (const segment of stream) {
      const result = await feed(receiver, segment, stream.meta.itemId);
      // Then names survive cuts without attempting to decode ciphertext as UTF-8.
      check(result.header.name === h.name && result.header.mimeType === h.mimeType);
      equal(result.payload, new Uint8Array([0, 1, 2]));
    }
  }]);
  for (const target of [0, 1]) tests.push([`AB2S AEAD ${target ? 'final' : 'non-final'} tag tamper`, async () => {
    const [usb, ble] = await pair();
    const stream = usb.openEncryptedStream(header(70000), generated(70000));
    const receiver = ble.openSegmentReceiver(stream.meta);
    let emitted = 0;
    for await (const segment of stream) {
      if (segment.index === target) {
        segment.bytes[segment.bytes.length - 1] ^= 1;
        const failure = await rejects(async () => { const result = await feed(receiver, segment, stream.meta.itemId); emitted += result.payload.length; });
        check(failure.name === 'OperationError', 'expected AES-GCM authentication rejection');
        break;
      }
      emitted += (await feed(receiver, segment, stream.meta.itemId)).payload.length;
    }
    check(emitted === (target ? 65486 : 0), 'tampered segment emitted plaintext');
    await rejects(() => receiver.finish());
    check(receiver.snapshot().ciphertextBytes === 0 && receiver.snapshot().retrySliceBytes === 0);
  }]);
  tests.push(['AB2S AEAD DATA order, replay and exact duplicate ownership', async () => {
    for (const fault of ['gap', 'reorder', 'conflict', 'short', 'overflow']) {
      const [usb, ble] = await pair();
      const stream = usb.openEncryptedStream(header(70000), generated(70000));
      const receiver = ble.openSegmentReceiver(stream.meta);
      for await (const segment of stream) {
        const context = {itemId:stream.meta.itemId, segmentIndex:0, chunkInSegment:0};
        const backing = new Uint8Array(70).fill(222); backing.set(segment.bytes.subarray(0, 51), 7);
        const owned = new DataView(backing.buffer, 7, 51);
        await receiver.push(context, owned); backing.fill(0);
        check((await receiver.push(context, segment.bytes.subarray(0, 51))).action === 'duplicate');
        await rejects(() => receiver.push({...context, itemId:0}, segment.bytes.subarray(0, 51)), /stale/);
        check(receiver.snapshot().ciphertextBytes === 65552, 'stale DATA changed active item');
        const bad = segment.bytes.slice(0, 51);
        const next = {...context, chunkInSegment:1};
        switch (fault) {
          case 'gap': next.chunkInSegment = 2; break;
          case 'reorder': next.segmentIndex = 1; break;
          case 'conflict': next.chunkInSegment = 0; bad[0] ^= 1; break;
          case 'short': break;
          case 'overflow': next.segmentIndex = 4294967296; break;
        }
        await rejects(() => receiver.push(next, fault === 'short' ? bad.subarray(0, 50) : bad));
        check(receiver.snapshot().ciphertextBytes === 0);
        await rejects(() => receiver.finish());
        break;
      }
      await rejects(() => ble.openSegmentReceiver(stream.meta), /stale/);
    }
  }]);
  tests.push(['AB2S AEAD session role, pre-SAS, key, burn and wrap gates', async () => {
    const locked = new p.AirBridgeCryptoSession({role:1, sendFn:async () => {}});
    await rejects(() => locked.openEncryptedStream(header(0), generated(0)), /locked/);
    const [usb, ble] = await pair();
    const first = usb.openEncryptedStream(header(0), generated(0)); first.abort();
    const second = usb.openEncryptedStream(header(0), generated(0));
    check(second.meta.itemId === first.meta.itemId + 1);
    await rejects(() => locked.openSegmentReceiver(second.meta), /locked/);
    await rejects(() => ble.openSegmentReceiver({...second.meta, keyId:'AAAAAAAAAAAAAAAAAAAAAA'}), /context/);
    await rejects(() => usb.openSegmentReceiver(second.meta), /context/);
    await rejects(() => ble.openSegmentReceiver({...second.meta, protocolVersion:1}), /Unsupported/);
    await rejects(() => ble.openSegmentReceiver({...second.meta, segmentCount:4294967296}));
    const receiver = ble.openSegmentReceiver(second.meta);
    for await (const segment of second) await feed(receiver, segment, second.meta.itemId);
    await rejects(() => ble.openSegmentReceiver(second.meta), /stale/);
    await rejects(async () => { for await (const unused of second) void unused; }, /inactive|single-use/);
    usb.streamOutboundId = 0xffffffff;
    await rejects(() => usb.openEncryptedStream(header(0), generated(0)), /wrapped/);
    check(!usb.isUnlocked() && usb.keys === null);
  }]);
  tests.push(['AB2S AEAD sender one-frame retry and retained-byte bounds', async () => {
    const [usb, ble] = await pair();
    const stream = usb.openEncryptedStream(await generatedHeader(4194369, vendor), generated(4194369));
    const receiver = ble.openSegmentReceiver(stream.meta);
    const hash = createIncrementalSha256(vendor);
    let peakCipher = 0, peakPlain = 0, peakFrame = 0, peakReceiver = 0, count = 0, retry = null;
    const sender = new p.ItemSender(async frame => {
      const msg = p.parseV2Frame(frame, stream.meta.itemId);
      const s = sender.streamSnapshot();
      peakCipher = Math.max(peakCipher, s.ciphertextBytes);
      peakPlain = Math.max(peakPlain, s.plaintextBytes);
      peakFrame = Math.max(peakFrame, s.frameBytes);
      check(frame.length === 64 && s.frameBytes === 64, 'exact retained frame bound');
      if (msg.type === p.MSG.ITEM_DATA) {
        const result = await receiver.push({itemId:msg.itemId, segmentIndex:msg.segment, chunkInSegment:msg.seq}, msg.body);
        if (result.action === 'authenticated') { count += result.payload.length; hash.update(result.payload); }
        const r = receiver.snapshot();
        peakReceiver = Math.max(peakReceiver, r.ciphertextBytes + r.retrySliceBytes + msg.body.length);
        if (msg.segment === 0 && msg.seq === 0 && !retry) {
          retry = frame.slice(); frame.fill(0);
          sender.receiveStreamControl(p.buildMessage(p.MSG.NACK, 0, p.makeV2AckPayload(msg, 1)));
          return;
        }
        if (msg.segment === 0 && msg.seq === 0) equal(frame, retry);
      }
      if (msg.type === p.MSG.ITEM_DONE) {
        check(p.verifyV2Completion(stream.meta, {...receiver.finish(),
          actualSha256:Array.from(hash.finalize(), b => b.toString(16).padStart(2, '0')).join('')}));
      }
      sender.receiveStreamControl(p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload(msg)));
    });
    await sender.sendEncryptedStream(stream);
    check(count === 4194369);
    check(peakCipher === 65552 && peakPlain === 65536 && peakFrame === 64);
    check(peakReceiver === 65603, 'receiver ciphertext + DATA bound');
    check(sender.sentFrames.size === 0 && sender.pendingAcks.size === 0, 'v1 frame retention used');
    check(sender.streamSnapshot().frameBytes === 0 && sender.streamSnapshot().ciphertextBytes === 0);
    console.info('AB2S measured bytes', JSON.stringify({peakCipher, peakPlain, peakFrame, peakReceiver, payload:count}));
  }]);
  tests.push(['AB2S AEAD actual IV/AAD bytes across items and directions', async () => {
    const sessions = await pair(), seen = new Set();
    const original = crypto.subtle.encrypt;
    let current, calls = 0;
    crypto.subtle.encrypt = async function(algorithm, key, plain) {
      const {session, meta} = current;
      const index = calls % 65;
      const iv = new Uint8Array(12); iv.set(session.noncePrefixes[meta.direction]);
      const view = new DataView(iv.buffer); view.setUint32(4, meta.itemId); view.setUint32(8, index);
      equal(algorithm.iv, iv);
      const aad = new Uint8Array(38); aad.set(enc.encode('AB2-AAD')); aad[7] = 1;
      const keyBytes = Uint8Array.from(atob(meta.keyId.replace(/-/g, '+').replace(/_/g, '/') + '=='), c => c.charCodeAt(0));
      aad.set(keyBytes, 8); aad[24] = session.role;
      const av = new DataView(aad.buffer); av.setUint32(25, meta.itemId); av.setUint32(29, index);
      av.setUint32(33, plain.byteLength); aad[37] = Number(index === 64);
      equal(algorithm.additionalData, aad);
      const id = Array.from(iv).join(','); check(!seen.has(id), 'repeated actual IV'); seen.add(id); calls++;
      return original.call(this, algorithm, key, plain);
    };
    try {
      for (const session of sessions) for (let item = 0; item < 3; item++) {
        const stream = session.openEncryptedStream(header(4194369), generated(4194369));
        current = {session, meta:stream.meta};
        for await (const segment of stream) check(segment.bytes.length <= 65552);
      }
    } finally { crypto.subtle.encrypt = original; }
    check(calls === 390 && seen.size === 390);
    const prefix = sessions[0].noncePrefixes['usb-to-ble'];
    check(String(p.makeV2Iv(prefix, 1, 0)) !== String(p.makeV2Iv(prefix, 0, 1)));
    for (const bad of [-1, 0x100000000, 1.5]) await rejects(() => p.makeV2Iv(prefix, 1, bad));
    console.info('AB2S actual unique IVs', calls);
  }]);
  tests.push(['AB2S AEAD authentication await and reset release no plaintext', async () => {
    for (const reset of [false, true]) {
      const [usb, ble] = await pair();
      const stream = usb.openEncryptedStream(header(0), generated(0));
      const receiver = ble.openSegmentReceiver(stream.meta);
      const original = crypto.subtle.decrypt;
      let unlock, enter;
      const gate = new Promise(resolve => { unlock = resolve; });
      const entered = new Promise(resolve => { enter = resolve; });
      crypto.subtle.decrypt = async function(...args) { enter(); await gate; return original.apply(this, args); };
      try {
        for await (const segment of stream) {
          let emitted = false;
          const pending = feed(receiver, segment, stream.meta.itemId).then(result => { emitted = true; return result; });
          await entered;
          check(!emitted && receiver.snapshot().authenticatedSegments === 0);
          check(receiver.snapshot().ciphertextBytes === segment.bytes.length);
          if (reset) ble.clearKeys();
          unlock();
          if (reset) { await rejects(() => pending, /inactive/); check(!emitted); }
          else check((await pending).action === 'authenticated');
        }
      } finally { unlock(); crypto.subtle.decrypt = original; }
    }
  }]);
  tests.push(['AB2S AEAD authenticated malformed header and wrong encryption key', async () => {
    for (const fault of ['utf8', 'incomplete', 'version', 'size', 'key', 'direction']) {
      const [usb, ble] = await pair();
      const stream = usb.openEncryptedStream(header(0, 'attachment', '€'), generated(0));
      const receiver = ble.openSegmentReceiver(stream.meta);
      const plain = p.encodeV2Header(header(0, 'attachment', '€'));
      switch (fault) {
        case 'utf8': plain[50] = 255; break;
        case 'incomplete': plain[7] = 4; break;
        case 'version': plain[4] = 1; break;
        case 'size': plain[17] = 1; break;
        case 'key': case 'direction': break;
      }
      const direction = fault === 'direction' ? 'ble-to-usb' : 'usb-to-ble';
      const key = fault === 'key' ? await crypto.subtle.generateKey({name:'AES-GCM', length:256}, false, ['encrypt']) : usb.keys[direction];
      const algorithm = {name:'AES-GCM', iv:p.makeV2Iv(usb.noncePrefixes[direction], stream.meta.itemId, 0),
        additionalData:p.makeV2Aad({...stream.meta, direction, segmentIndex:0, segmentPlaintextLen:plain.length, finalFlag:true}), tagLength:128};
      const bytes = new Uint8Array(await crypto.subtle.encrypt(algorithm, key, plain));
      await rejects(() => feed(receiver, {index:0, bytes}, stream.meta.itemId));
      await rejects(() => receiver.finish());
      check(receiver.snapshot().ciphertextBytes === 0);
      stream.abort();
    }
  }]);
  tests.push(['AB2S AEAD file stream, byte ownership and source length failures', async () => {
    const [usb, ble] = await pair();
    const file = new File([new Uint8Array([0, 1, 2])], 'data');
    let acquisitions = 0;
    const original = file.stream.bind(file);
    file.stream = () => { acquisitions++; return original(); };
    const stream = usb.openEncryptedStream(header(3), file);
    const receiver = ble.openSegmentReceiver(stream.meta);
    for await (const segment of stream) equal((await feed(receiver, segment, stream.meta.itemId)).payload, new Uint8Array([0, 1, 2]));
    check(acquisitions === 1);
    for (const size of [2, 4]) {
      const bad = usb.openEncryptedStream(header(3), generated(size));
      await rejects(async () => { for await (const segment of bad) void segment; }, /payload/);
      check(bad.snapshot().plaintextBytes === 0 && bad.snapshot().ciphertextBytes === 0);
    }
    const id = usb.streamOutboundId;
    await rejects(() => usb.openEncryptedStream(header(4), file), /size/);
    check(usb.streamOutboundId === id, 'invalid file burned an ID');
    let released = false;
    const source = {async *[Symbol.asyncIterator]() { try { yield new DataView(new Uint8Array([9, 0, 1, 2, 9]).buffer, 1, 3); } finally { released = true; } }};
    const viewStream = usb.openEncryptedStream(header(3), source);
    const viewReceiver = ble.openSegmentReceiver(viewStream.meta);
    for await (const segment of viewStream) equal((await feed(viewReceiver, segment, viewStream.meta.itemId)).payload, new Uint8Array([0, 1, 2]));
    check(released);
  }]);
  tests.push(['AB2S AEAD rejects mixed v1/v2 nonce domains in one session', async () => {
    const [usb] = await pair();
    const stream = usb.openEncryptedStream(header(0), generated(0));
    const hash = await p.sha256(new Uint8Array(0));
    await rejects(() => usb.encryptItem({kind:'text', itemId:1, name:'', mimeType:'', hash}, new Uint8Array(0)), /mixed/);
    stream.abort();
  }]);
  tests.push(['AB2S AEAD sender unsupported capability fails before META', async () => {
    const [usb] = await pair();
    const stream = usb.openEncryptedStream(header(0), generated(0));
    const types = [];
    const sender = new p.ItemSender(async frame => {
      const msg = p.parseMessage(frame); types.push(msg.type);
      sender.receiveStreamControl(p.buildMessage(p.MSG.ACK, 0, p.makeAckPayload(msg.type, msg.seq)));
    });
    await rejects(() => sender.sendEncryptedStream(stream), /Unsupported/);
    equal(types, [p.MSG.HELLO]);
    check(sender.streamSnapshot().frameBytes === 0);
    check(usb.openEncryptedStream(header(0), generated(0)).meta.itemId === 2);
  }]);
  tests.push(['AB2S AEAD maximum header stays entirely in segment zero', async () => {
    const [usb, ble] = await pair();
    const h = header(1, 'attachment', 'a'.repeat(65483) + '€');
    const stream = usb.openEncryptedStream(h, generated(1));
    const receiver = ble.openSegmentReceiver(stream.meta);
    let segments = 0;
    for await (const segment of stream) {
      const result = await feed(receiver, segment, stream.meta.itemId);
      check(result.header.headerLen === 65536 && result.header.name === h.name);
      check(result.payload.length === segment.index);
      check(segment.bytes.length === (segment.index === 0 ? 65552 : 17));
      const lastOffset = Math.floor((segment.bytes.length - 1) / 51) * 51;
      const duplicate = await receiver.push({itemId:stream.meta.itemId, segmentIndex:segment.index,
        chunkInSegment:lastOffset / 51}, segment.bytes.subarray(lastOffset));
      check(duplicate.action === 'duplicate'); segments++;
    }
    check(segments === 2 && receiver.finish().payloadBytes === 1n);
  }]);
  tests.push(['AB2S AEAD stalled transport retains only one write', async () => {
    const [usb] = await pair();
    const stream = usb.openEncryptedStream(header(0), generated(0));
    let writes = 0, release;
    const pendingWrite = new Promise(resolve => { release = resolve; });
    const sender = new p.ItemSender(() => { writes++; return pendingWrite; }, {timeoutMs:5, retries:2});
    try { await rejects(() => sender.sendEncryptedStream(stream), /timeout|timed out|exhausted/); }
    finally { release(); }
    check(writes === 1, 'overlapping transport writes retained retry copies');
    check(sender.streamSnapshot().frameBytes === 0);
  }]);
  tests.push(['AB2S AEAD source acquisition failure releases working buffers', async () => {
    const [usb] = await pair();
    const stream = usb.openEncryptedStream(header(0), {getReader() { throw new Error('reader unavailable'); }});
    await rejects(async () => { for await (const segment of stream) void segment; }, /reader unavailable/);
    check(stream.snapshot().plaintextBytes === 0, 'failed reader retained plaintext');
  }]);
  tests.push(['AB2S AEAD native File >4 MiB uses bounded stream reads', async () => {
    const [usb, ble] = await pair();
    const file = new File([new Uint8Array(4194369)], 'large.bin');
    const stream = usb.openEncryptedStream(header(file.size), file);
    const receiver = ble.openSegmentReceiver(stream.meta);
    let bytes = 0;
    for await (const segment of stream) {
      const result = await feed(receiver, segment, stream.meta.itemId);
      check(result.payload.every(byte => byte === 0)); bytes += result.payload.length;
    }
    check(bytes === file.size && receiver.finish().payloadBytes === BigInt(file.size));
  }]);
  tests.push(['AB2S AEAD replacing or resetting sessions releases active segments', async () => {
    const [usb, ble] = await pair();
    const first = usb.openEncryptedStream(header(70000), generated(70000));
    const incoming = ble.openSegmentReceiver(first.meta);
    const iterator = first[Symbol.asyncIterator]();
    const {value:segment} = await iterator.next();
    await incoming.push({itemId:first.meta.itemId, segmentIndex:0, chunkInSegment:0}, segment.bytes.subarray(0, 51));
    const second = usb.openEncryptedStream(header(0), generated(0));
    ble.openSegmentReceiver(second.meta);
    check(first.snapshot().ciphertextBytes === 0 && first.snapshot().plaintextBytes === 0);
    check(incoming.snapshot().ciphertextBytes === 0);
    await iterator.return();
    usb.clearKeys();
    check(second.snapshot().plaintextBytes === 0);
  }]);
  tests.push(['R1 pending ReadableStream read cancels on abort/reset/replacement/return', async () => {
    for (const mode of ['abort', 'clearKeys', 'reset', 'replace', 'return']) {
      const [session] = await pair();
      let controller, enter, cancelled = 0;
      const ready = new Promise(resolve => { enter = resolve; });
      const source = new ReadableStream({start(c) {controller = c;}, pull() {enter();}, cancel() {cancelled++;}});
      const stream = session.openEncryptedStream(header(1), source), iterator = stream[Symbol.asyncIterator]();
      const pending = iterator.next().catch(error => error);
      await ready; await Promise.resolve();
      const start = performance.now();
      switch (mode) {
        case 'abort': stream.abort(); break;
        case 'clearKeys': session.clearKeys(); break;
        case 'reset': session.reset(); break;
        case 'replace': session.openEncryptedStream(header(0), generated(0)); break;
        case 'return': break;
      }
      const closing = iterator.return();
      try {
        await promptly(Promise.all([pending, closing]));
        check(cancelled === 1 && !source.locked, `${mode} did not cancel/release source`);
        check(stream.snapshot().sourceBytes === 0 && stream.snapshot().plaintextBytes === 0);
        console.info('R1 settled', JSON.stringify({mode, cancelled, locked:source.locked, ms:performance.now()-start}));
      } finally {
        if (!cancelled) controller.close();
        await pending; await closing; session.clearKeys();
      }
    }
  }]);
  tests.push(['R1 generic pending next cancels with exactly one source return', async () => {
    const [session] = await pair();
    let enter, returned = 0;
    const ready = new Promise(resolve => {enter = resolve;});
    const source = {[Symbol.asyncIterator]() { return {
      next() { enter(); return new Promise(() => {}); }, return() {returned++; return {done:true};},
    }; }};
    const stream = session.openEncryptedStream(header(1), source), iterator = stream[Symbol.asyncIterator]();
    const pending = iterator.next().catch(error => error);
    await ready; stream.abort(); stream.abort();
    await promptly(Promise.all([pending, iterator.return()]));
    check(returned === 1 && stream.snapshot().sourceBytes === 0);
  }]);
  tests.push(['R1 early iteration break cancels the live producer once', async () => {
    const [session] = await pair(); let cancelled = 0;
    const source = new ReadableStream({pull(c) {c.enqueue(new Uint8Array(65536));}, cancel() {cancelled++;}});
    const stream = session.openEncryptedStream(header(70000), source);
    for await (const segment of stream) { check(segment.index === 0); break; }
    check(cancelled === 1 && !source.locked && stream.snapshot().sourceBytes === 0);
  }]);
  tests.push(['R2 locked source rejects synchronously without burn or replacement', async () => {
    const [session] = await pair();
    const active = session.openEncryptedStream(header(0), generated(0));
    const before = session.streamOutboundId;
    const source = new ReadableStream(), reader = source.getReader();
    try {
      let error;
      try { session.openEncryptedStream(header(0), source); } catch (caught) {error = caught;}
      check(error instanceof Error && /locked/.test(error.message), 'locked source opened');
      check(session.streamOutboundId === before, 'locked source burned ID');
      active.assertActive();
      console.info('R2 IDs', JSON.stringify({before, after:session.streamOutboundId}));
    } finally {reader.releaseLock(); session.clearKeys();}
  }]);
  tests.push(['R3 in-flight final DATA retry waits for one authentication result', async () => {
    for (const fault of ['exact', 'conflict', 'other']) {
      const [usb, ble] = await pair();
      const stream = usb.openEncryptedStream(header(0), generated(0));
      const receiver = ble.openSegmentReceiver(stream.meta), iterator = stream[Symbol.asyncIterator]();
      const {value:segment} = await iterator.next();
      const context = {itemId:stream.meta.itemId, segmentIndex:0, chunkInSegment:1};
      await receiver.push({...context, chunkInSegment:0}, segment.bytes.subarray(0,51));
      let enter, release, decrypts = 0, settled = false;
      const ready = new Promise(resolve=>{enter=resolve;}), gate = new Promise(resolve=>{release=resolve;});
      const original = crypto.subtle.decrypt;
      crypto.subtle.decrypt = async function(...args) {decrypts++; enter(); await gate; return original.apply(this,args);};
      try {
        const pending = receiver.push(context, segment.bytes.subarray(51)).catch(error=>error);
        await ready;
        const last = segment.bytes.slice(51); if (fault === 'conflict') last[0] ^= 1;
        const duplicate = receiver.push(fault === 'other' ? {...context, chunkInSegment:2} : context, last)
          .catch(error=>error).finally(()=>{settled=true;});
        await Promise.resolve(); await Promise.resolve();
        if (fault === 'exact') check(!settled, 'retry settled before authentication');
        release();
        const first = await pending, second = await duplicate;
        check(decrypts === 1, 'duplicate decrypted twice');
        if (fault === 'exact') {
          check(first.action === 'authenticated' && second.action === 'duplicate');
          check(!Object.hasOwn(second,'payload'), 'duplicate emitted payload');
        } else {
          check(first instanceof Error && second instanceof Error, 'concurrent conflict accepted');
          await rejects(()=>receiver.finish());
        }
        console.info('R3 result', JSON.stringify({fault, decrypts, first:first.action ?? first.message, second:second.action ?? second.message}));
      } finally {release(); crypto.subtle.decrypt = original; await iterator.return();}
    }
  }]);
  tests.push(['R4 relinquished 64-KiB subview cannot alias its 16-MiB backing', async () => {
    const [usb, ble] = await pair(); let bytes, reads = 0, sourcePeak = 0, copyWorkingBytes = 0;
    const source = {[Symbol.asyncIterator]() {return {
      next() {
        if (reads++) return {done:true}; bytes = new Uint8Array(16*1024*1024).fill(7);
        return {done:false, get value() {
          queueMicrotask(() => {
            const state = stream.snapshot();
            sourcePeak = Math.max(sourcePeak,state.sourceBytes);
            copyWorkingBytes = Math.max(copyWorkingBytes,state.plaintextBytes + state.sourceBytes + state.ciphertextBytes);
          });
          return bytes.subarray(0,65536);
        }};
      },
      return() {return {done:true};},
    };}};
    const stream = usb.openEncryptedStream(header(65536),source), receiver = ble.openSegmentReceiver(stream.meta);
    const iterator = stream[Symbol.asyncIterator]();
    let enter, release;
    const ready = new Promise(resolve=>{enter=resolve;}), gate = new Promise(resolve=>{release=resolve;});
    const original = crypto.subtle.encrypt;
    crypto.subtle.encrypt = async function(...args) {enter(); await gate; return original.apply(this,args);};
    try {
      const pending = iterator.next(); await ready;
      const held = stream.snapshot();
      bytes.fill(9,65486,65536); bytes = null; release();
      const first = (await pending).value; await feed(receiver,first,stream.meta.itemId);
      const second = (await iterator.next()).value;
      const result = await feed(receiver,second,stream.meta.itemId);
      check(result.payload.length === 50 && result.payload.every(byte=>byte===7), 'borrowed tail changed across encrypt await');
      check(sourcePeak === 65536 && held.sourceBytes === 50, 'owned source/residual missing from snapshot');
      check(copyWorkingBytes === 131072, 'source-copy working bytes omitted');
      await iterator.next();
      check(stream.snapshot().sourceBytes === 0);
      console.info('R4 alias', JSON.stringify({sourcePeak, copyWorkingBytes, held, tailBytes:result.payload.length, tailValue:result.payload[0], final:stream.snapshot()}));
    } finally {release(); crypto.subtle.encrypt = original; await iterator.return();}
  }]);
  tests.push(['Source cleanup preserves primary identity and surfaces cleanup-only failure', async () => {
    for (const mode of ['next', 'crypto', 'only']) {
      const [session] = await pair();
      const primary = new Error('primary operation failure'), cleanup = new Error('cleanup failure');
      let returned = 0;
      const source = {[Symbol.asyncIterator]() {return {
        next() {if(mode==='next') throw primary; return {done:true};},
        return() {returned++; throw cleanup;},
      };}};
      const stream = session.openEncryptedStream(header(mode==='next'?1:0),source);
      const original = crypto.subtle.encrypt;
      if (mode==='crypto') crypto.subtle.encrypt = async () => {throw primary;};
      try {
        const failure = await rejects(async()=>{for await(const segment of stream) void segment;});
        check(failure === (mode==='only'?cleanup:primary), 'cleanup replaced primary operation error');
        check(stream.cleanupError === cleanup && returned === 1, 'cleanup error/count not recorded');
        check(stream.snapshot().sourceBytes === 0 && stream.snapshot().plaintextBytes === 0);
      } finally {crypto.subtle.encrypt = original;}
    }
  }]);
  return tests;
}
