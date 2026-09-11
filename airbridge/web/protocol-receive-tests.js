// Shared Node/browser tests. Authentication is the caller's prerequisite;
// segmented AEAD and chat integration are deliberately not exercised here.
export function receiveAssert(condition, message) {
  if (!condition) throw new Error(message);
}
export function receiveReject(fn) {
  try { fn(); } catch (error) {
    receiveAssert(error instanceof Error, 'typed failure');
    return;
  }
  throw new Error('expected rejection');
}
const hex = bytes => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
const digest = async bytes => hex(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)));
const load = () => import('./airbridge-receive-accumulator.js');

function released(accumulator, state) {
  const snapshot = accumulator.snapshot();
  receiveAssert(snapshot.state === state && snapshot.retainedChunks === 0
    && snapshot.retainedBytes === 0n && !snapshot.hasHasher, 'terminal ownership released');
  receiveReject(() => accumulator.appendAuthenticated(new Uint8Array()));
  receiveReject(() => accumulator.finalize(0n, '0'.repeat(64), '', ''));
  accumulator.abort();
  accumulator.abort();
  receiveAssert(accumulator.snapshot().state === state, 'idempotent terminal abort');
}

export function receiveAccumulatorTests(vendor) {
  return [
    ['Receive accumulator ordered owned chunks, incremental hashing and direct Blob parts', async () => {
      const { createReceiveAccumulator } = await load();
      const parts = [];
      const updates = [];
      const wrapped = { create() {
        const hash = vendor.create();
        return { update(chunk) { updates.push(chunk); hash.update(chunk); }, array: () => hash.array() };
      } };
      const acc = createReceiveAccumulator(wrapped);
      class AliasingBytes extends Uint8Array { slice() { return this; } }
      const parent = new AliasingBytes([99, 1, 2, 3, 99]);
      const view = parent.subarray(1, 4);
      acc.appendAuthenticated(view);
      receiveAssert(updates.length === 1 && updates[0] !== view
        && Object.getPrototypeOf(updates[0]) === Uint8Array.prototype, 'base owned chunk hashed immediately');
      parent.fill(0);
      acc.appendAuthenticated(new Uint8Array([4, 5]));
      receiveAssert(acc.snapshot().retainedBytes === 5n, 'BigInt count');
      const expected = await digest(new Uint8Array([1, 2, 3, 4, 5]));
      const NativeBlob = globalThis.Blob;
      let result;
      try {
        globalThis.Blob = class extends NativeBlob {
          constructor(chunks, options) { parts.push(...chunks); super(chunks, options); }
        };
        result = acc.finalize(5n, expected, '<img src=x>.bin', 'application/octet-stream');
      } finally { globalThis.Blob = NativeBlob; }
      receiveAssert(parts.length === 2 && parts.every((p, i) => p === updates[i]), 'Blob uses original owned parts, no concat');
      receiveAssert(result.name === '<img src=x>.bin' && result.mime === 'application/octet-stream'
        && result.size === 5n && result.hash === expected, 'verified metadata');
      receiveAssert(hex(new Uint8Array(await result.blob.arrayBuffer())) === '0102030405', 'immutable ordered Blob bytes');
      released(acc, 'finalized');
    }],
    ['Receive accumulator empty and generated above 4 MiB final verification', async () => {
      const { createReceiveAccumulator } = await load();
      for (const size of [0, 4 * 1024 * 1024 + 65]) {
        const bytes = Uint8Array.from({ length: size }, (_, i) => (i * 131 + (i >>> 8)) & 255);
        const acc = createReceiveAccumulator(vendor);
        for (let i = 0; i < size; i += 65536) acc.appendAuthenticated(bytes.subarray(i, i + 65536));
        const result = acc.finalize(BigInt(size), await digest(bytes), 'generated.bin', 'application/octet-stream');
        receiveAssert(result.blob.size === size && result.blob.type === result.mime, 'Blob size/type');
        receiveAssert(await digest(await result.blob.arrayBuffer()) === result.hash, 'full Blob hash');
        released(acc, 'finalized');
      }
    }],
    ['Receive accumulator wrong size/hash and malformed metadata fail closed without Blob', async () => {
      const { createReceiveAccumulator } = await load();
      const bytes = new Uint8Array([1, 2, 3]);
      const hash = await digest(bytes);
      const valid = [3n, hash, 'file.bin', 'application/octet-stream'];
      const invalid = [
        [0, 2n], [0, 4n], [0, 9007199254740993n], [0, -1n], [0, 3], [0, '3'],
        [0, null], [1, '0'.repeat(64)], [1, hash.toUpperCase()], [1, `sha256:${hash}`],
        [1, `${hash}\n`], [1, new Uint8Array(32)], [1, { toString: () => hash }],
        [2, null], [2, {}], [2, 'bad\u0000name'], [2, '\ud800'],
        [3, null], [3, {}], [3, 'text/plain\n'], [3, 'text/é'],
      ];
      const NativeBlob = globalThis.Blob;
      let created = 0;
      try {
        globalThis.Blob = class extends NativeBlob {
          constructor(...args) { created++; super(...args); }
        };
        for (const [index, value] of invalid) {
          const acc = createReceiveAccumulator(vendor);
          acc.appendAuthenticated(bytes);
          const args = [...valid]; args[index] = value;
          receiveReject(() => acc.finalize(...args));
          released(acc, 'aborted');
        }
        // Tampered authenticated payload still fails the authoritative final hash.
        const tampered = createReceiveAccumulator(vendor);
        tampered.appendAuthenticated(new Uint8Array([1, 2, 4]));
        receiveReject(() => tampered.finalize(...valid));
        released(tampered, 'aborted');
        receiveAssert(created === 0, 'no Blob on failed verification');
      } finally { globalThis.Blob = NativeBlob; }
    }],
    ['Receive accumulator cancel, disconnect/reset abort and invalid chunks release ownership', async () => {
      const { createReceiveAccumulator } = await load();
      for (const reason of ['cancel', 'disconnect', 'crypto reset']) {
        const acc = createReceiveAccumulator(vendor);
        acc.appendAuthenticated(new Uint8Array([1, 2, 3]));
        acc.abort(reason);
        released(acc, 'aborted');
      }
      for (const value of [null, undefined, [1], new ArrayBuffer(1), new DataView(new ArrayBuffer(1)), new Uint16Array(1)]) {
        const acc = createReceiveAccumulator(vendor);
        acc.appendAuthenticated(new Uint8Array([1]));
        receiveReject(() => acc.appendAuthenticated(value));
        released(acc, 'aborted');
      }
    }],
  ];
}
