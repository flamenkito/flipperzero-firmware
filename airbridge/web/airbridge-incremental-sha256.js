/**
 * Pure-JS incremental SHA-256. Load the pinned vendor classic script first in
 * browsers; Node callers pass its CommonJS export. Updates consume bytes
 * synchronously without retaining the caller's chunk. Finalization is terminal.
 * @param {{create: Function}} implementation Pinned js-sha256 implementation.
 * @returns {{update(chunk: Uint8Array): void, finalize(): Uint8Array}}
 */
export function createIncrementalSha256(implementation = globalThis.sha256) {
  const hash = implementation.create();
  let finalized = false;
  const requireActive = () => {
    if (finalized) throw new Error('SHA-256 already finalized');
  };
  return {
    update(chunk) {
      requireActive();
      if (!(chunk instanceof Uint8Array)) throw new TypeError('SHA-256 chunk must be Uint8Array');
      hash.update(chunk);
    },
    finalize() {
      requireActive();
      finalized = true;
      return new Uint8Array(hash.array());
    },
  };
}

/**
 * Hash one ReadableStream sequentially with constant additional memory.
 * Releases the reader lock on EOF or failure; does not cancel the caller's source.
 * @param {ReadableStream<Uint8Array>} stream
 * @param {{create: Function}} implementation Pinned js-sha256 implementation.
 * @returns {Promise<Uint8Array>} Owned 32-byte digest.
 */
export async function sha256Stream(stream, implementation = globalThis.sha256) {
  const hash = createIncrementalSha256(implementation);
  const reader = stream.getReader();
  try {
    while (true) {
      const { done, value } = await reader.read();
      if (done) return hash.finalize();
      hash.update(value);
    }
  } finally {
    reader.releaseLock();
  }
}
