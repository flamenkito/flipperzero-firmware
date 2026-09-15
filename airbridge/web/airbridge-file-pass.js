import { createIncrementalSha256 } from './airbridge-incremental-sha256.js';

export function payloadDigestHex(bytes) {
  return Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join('');
}

export class FileHashPass {
  #file; #owner; #reader; #hash; #expected; #ended = false; #closing;
  #nextPaintYield = 1024n * 1024n;
  #onAbort;
  bytes = 0n;
  digest = null;
  constructor(file, owner, expected = null) {
    owner.assertActive();
    this.#file = file; this.#owner = owner; this.#expected = expected;
    this.#hash = createIncrementalSha256(owner.hashImplementation);
    this.#reader = file.stream().getReader({mode:'byob'});
    this.#onAbort = () => { this.return().catch(error => owner.recordCleanupError(error)); };
    owner.signal.addEventListener('abort', this.#onAbort, {once:true});
    if (owner.signal.aborted) this.#onAbort();
  }
  [Symbol.asyncIterator]() { return this; }
  async next() {
    this.#owner.assertActive();
    try {
      const step = await this.#owner.wait(this.#reader.read(new Uint8Array(65536)));
      this.#owner.assertActive();
      if (step.done) {
        this.#ended = true;
        if (this.bytes !== BigInt(this.#file.size) || this.bytes !== this.#owner.size) throw new Error('file size mismatch');
        this.digest = payloadDigestHex(this.#hash.finalize());
        if (this.#expected !== null && this.digest !== this.#expected) throw new Error('file content changed between passes');
        await this.return();
        this.#owner.assertActive();
        return {done:true};
      }
      const bytes = new Uint8Array(step.value);
      this.bytes += BigInt(bytes.length);
      if (this.bytes > this.#owner.size) throw new Error('file size mismatch');
      this.#hash.update(bytes);
      if (this.#expected === null) {
        this.#owner.progress('Hashing', this.bytes);
        if (this.bytes >= this.#nextPaintYield) {
          while (this.#nextPaintYield <= this.bytes) this.#nextPaintYield += 1024n * 1024n;
          await this.#owner.wait(new Promise(resolve => setTimeout(resolve, 0)));
        }
      }
      this.#owner.assertActive();
      return {done:false, value:bytes};
    } catch (error) {
      try { await this.return(); } catch (cleanup) { this.#owner.recordCleanupError(cleanup); }
      throw error;
    }
  }
  return() {
    if (!this.#closing) this.#closing = Promise.resolve().then(async () => {
      this.#owner.signal.removeEventListener('abort', this.#onAbort);
      let failure;
      try { if (!this.#ended) await this.#reader.cancel(); } catch (error) { failure = error; }
      try { this.#reader.releaseLock(); } catch (error) { failure ??= error; }
      this.#hash = null;
      if (failure) throw failure;
      return {done:true};
    });
    return this.#closing;
  }
}
