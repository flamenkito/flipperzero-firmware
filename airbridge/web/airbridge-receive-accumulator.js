import { createIncrementalSha256 } from './airbridge-incremental-sha256.js';

// Caller must authenticate each chunk before appending. This owns O(file size)
// plaintext, never ciphertext. Finalization transfers Blob ownership to the caller;
// abort drops references, not a guarantee of secure erasure by the JS runtime.
export function createReceiveAccumulator(implementation = globalThis.sha256) {
  let chunks = [];
  let hasher = createIncrementalSha256(implementation);
  let retainedBytes = 0n;
  let state = 'active';

  function requireActive() {
    if (state !== 'active') throw new Error(`Receive accumulator is ${state}`);
  }

  function release(terminalState) {
    chunks = null;
    hasher = null;
    retainedBytes = 0n;
    state = terminalState;
  }

  function abort() {
    if (state === 'active') release('aborted');
  }

  return {
    appendAuthenticated(chunk) {
      requireActive();
      try {
        if (!(chunk instanceof Uint8Array)) throw new TypeError('Authenticated chunk must be Uint8Array');
        const owned = new Uint8Array(chunk);
        hasher.update(owned);
        if (owned.byteLength) chunks.push(owned);
        retainedBytes += BigInt(owned.byteLength);
      } catch (error) {
        abort();
        throw error;
      }
    },
    finalize(expectedSize, expectedHash, name, mime) {
      requireActive();
      try {
        if (typeof expectedSize !== 'bigint' || expectedSize < 0n || expectedSize > 0xffffffffffffffffn) {
          throw new TypeError('Expected size must be uint64 BigInt');
        }
        if (typeof expectedHash !== 'string' || expectedHash.length !== 64 || !/^[0-9a-f]+$/.test(expectedHash)) {
          throw new TypeError('Expected hash must be 64 lowercase hexadecimal characters');
        }
        if (typeof name !== 'string' || /[\u0000-\u001f\u007f\uD800-\uDFFF]/u.test(name)) {
          throw new TypeError('Name must be well-formed text without control characters');
        }
        if (typeof mime !== 'string' || /[^\x20-\x7e]/.test(mime)) {
          throw new TypeError('MIME must be printable ASCII');
        }
        if (retainedBytes !== expectedSize) throw new Error('Receive size mismatch');
        const actual = hasher.finalize();
        let difference = 0;
        // Fixed-length, non-short-circuit comparison; JS does not promise constant time.
        for (let i = 0; i < 32; i++) difference |= actual[i] ^ Number.parseInt(expectedHash.slice(i * 2, i * 2 + 2), 16);
        if (difference !== 0) throw new Error('Receive SHA-256 mismatch');
        const blob = new Blob(chunks, { type: mime });
        release('finalized');
        return Object.freeze({ blob, size: expectedSize, hash: expectedHash, name, mime });
      } catch (error) {
        abort();
        throw error;
      }
    },
    abort,
    // Scalar-only diagnostics: never expose retained plaintext or the hasher.
    snapshot() {
      return Object.freeze({ state, retainedBytes, retainedChunks: chunks?.length ?? 0, hasHasher: hasher !== null });
    },
  };
}
