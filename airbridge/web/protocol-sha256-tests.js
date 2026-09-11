// Shared Node/browser regression cases; not part of the deployed chat bundle.
import { makeV2Meta, verifyV2Completion } from './airbridge-protocol.js';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function equalBytes(actual, expected, label) {
  assert(actual instanceof Uint8Array, `${label}: digest is not Uint8Array`);
  assert(actual.length === expected.length && actual.every((b, i) => b === expected[i]), label);
}

function rejects(fn, pattern) {
  try { fn(); } catch (error) {
    assert(pattern.test(error.message), `unexpected rejection: ${error.message}`);
    return;
  }
  throw new Error(`expected rejection: ${pattern}`);
}

async function rejectsAsync(fn, pattern) {
  try { await fn(); } catch (error) {
    assert(pattern.test(error.message), `unexpected rejection: ${error.message}`);
    return;
  }
  throw new Error(`expected async rejection: ${pattern}`);
}

const hex = bytes => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
const reference = async bytes => new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
const generatedByte = offset => (offset * 131 + (offset >>> 8)) & 255;

export function incrementalSha256Tests(vendor) {
  // Import inside each case so missing implementations produce named red tests.
  const adapter = () => import('./airbridge-incremental-sha256.js');
  return [
    ['SHA-256 incremental published vectors, binary and padding boundaries', async () => {
      const { createIncrementalSha256 } = await adapter();
      const published = [
        ['', 'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855'],
        ['abc', 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad'],
        ['abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq',
          '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1'],
      ];
      const vectors = published.map(([text, expected]) => [new TextEncoder().encode(text), expected]);
      vectors.push([Uint8Array.from({ length: 256 }, (_, i) => i)]);
      for (const length of [55, 56, 63, 64, 65]) {
        vectors.push([Uint8Array.from({ length }, (_, i) => generatedByte(i))]);
      }
      for (const [input, publishedHex] of vectors) {
        const expected = await reference(input);
        if (publishedHex) assert(hex(expected) === publishedHex, 'published SHA-256 vector');
        for (const sizes of [[1], [7, 0, 19, 3, 64, 2], [input.length || 1]]) {
          const hash = createIncrementalSha256(vendor);
          hash.update(new Uint8Array(0));
          let offset = 0;
          let index = 0;
          while (offset < input.length) {
            const count = Math.min(sizes[index++ % sizes.length], input.length - offset);
            // Offset view must not accidentally hash its surrounding bytes.
            const storage = new Uint8Array(count + 4).fill(0xa5);
            storage.set(input.subarray(offset, offset + count), 2);
            hash.update(storage.subarray(2, 2 + count));
            storage.fill(0xff);
            offset += count;
          }
          equalBytes(hash.finalize(), expected, `length ${input.length}, chunks ${sizes}`);
        }
      }
    }],
    ['SHA-256 strict lifecycle, Uint8Array-only input and owned array output', async () => {
      const { createIncrementalSha256 } = await adapter();
      let upstreamArray;
      const wrapped = { create() {
        const upstream = vendor.create();
        return {
          update: chunk => upstream.update(chunk),
          array() { upstreamArray = upstream.array(); return upstreamArray; },
        };
      } };
      const hash = createIncrementalSha256(wrapped);
      for (const invalid of ['abc', [1], new ArrayBuffer(1), new DataView(new ArrayBuffer(1)),
        new Uint16Array(1), new Uint8ClampedArray(1), null, undefined, {}]) {
        rejects(() => hash.update(invalid), /Uint8Array/);
      }
      const input = new TextEncoder().encode('abc');
      hash.update(input);
      const digest = hash.finalize();
      assert(Array.isArray(upstreamArray), 'upstream .array() must be a plain Array');
      assert(Object.getPrototypeOf(digest) === Uint8Array.prototype, 'base Uint8Array digest');
      equalBytes(digest, await reference(input), 'strict digest');
      upstreamArray.fill(0);
      equalBytes(digest, await reference(input), 'digest owns copied upstream bytes');
      rejects(() => hash.finalize(), /finalized/);
      rejects(() => hash.update(input), /finalized/);
      rejects(() => hash.update(new Uint8Array(0)), /finalized/);
      equalBytes(createIncrementalSha256(vendor).finalize(), await reference(new Uint8Array()), 'independent state');
    }],
    ['SHA-256 generated multi-megabyte stream reads independently without materialization', async () => {
      const { sha256Stream } = await adapter();
      const size = 4 * 1024 * 1024 + 65;
      // The sole aggregate is the test-only WebCrypto oracle, not the hash pass.
      const oracle = Uint8Array.from({ length: size }, (_, i) => generatedByte(i));
      let offset = 0;
      let reads = 0;
      let updates = 0;
      let released = 0;
      let acquired = 0;
      let pending = false;
      const scratch = new Uint8Array(8191);
      const wrapped = { create() {
        const upstream = vendor.create();
        return { update(chunk) { updates++; upstream.update(chunk); }, array: () => upstream.array() };
      } };
      const forbidden = () => { throw new Error('full materialization requested'); };
      const stream = {
        arrayBuffer: forbidden, tee: forbidden, [Symbol.asyncIterator]: forbidden,
        getReader() {
          assert(++acquired === 1, 'reader acquired once');
          return {
            async read() {
              assert(!pending, 'concurrent reads');
              assert(updates === reads, 'next read requested before previous chunk hashed');
              pending = true;
              await Promise.resolve();
              pending = false;
              if (offset === size) return { done: true };
              const count = Math.min(scratch.length, size - offset);
              for (let i = 0; i < count; i++) scratch[i] = generatedByte(offset + i);
              offset += count;
              reads++;
              return { done: false, value: scratch.subarray(0, count) };
            },
            releaseLock() { released++; },
          };
        },
      };
      equalBytes(await sha256Stream(stream, wrapped), await reference(oracle), 'generated stream digest');
      assert(reads > 500 && reads === updates && released === 1, 'incremental read/update/release counts');
    }],
    ['SHA-256 stream EOF, invalid chunks and read errors release reader locks', async () => {
      const { sha256Stream } = await adapter();
      const empty = new ReadableStream({ start(controller) { controller.close(); } });
      equalBytes(await sha256Stream(empty, vendor), await reference(new Uint8Array()), 'empty stream');
      assert(!empty.locked, 'EOF lock released');
      for (const failRead of [false, true]) {
        let released = 0;
        const stream = { getReader: () => ({
          async read() {
            if (failRead) throw new Error('read failed');
            return { done: false, value: [1, 2, 3] };
          },
          releaseLock() { released++; },
        }) };
        await rejectsAsync(() => sha256Stream(stream, vendor), failRead ? /read failed/ : /Uint8Array/);
        assert(released === 1, 'error lock released');
      }
    }],
    ['SHA-256 one mutated streamed byte rejects final AB2S verification', async () => {
      const { sha256Stream } = await adapter();
      const input = Uint8Array.from({ length: 257 }, (_, i) => generatedByte(i));
      const digestOf = async source => {
        let offset = 0;
        return sha256Stream(new ReadableStream({ pull(controller) {
          if (offset === source.length) { controller.close(); return; }
          const end = Math.min(offset + 17, source.length);
          controller.enqueue(source.slice(offset, end));
          offset = end;
        } }, { highWaterMark: 0 }), vendor);
      };
      const original = await digestOf(input);
      equalBytes(original, await reference(input), 'original streamed digest');
      input[129] ^= 1;
      const mutated = await digestOf(input);
      equalBytes(mutated, await reference(input), 'mutated streamed digest');
      assert(hex(original) !== hex(mutated), 'one byte mutation changes digest');
      const payloadSize = BigInt(input.length);
      const meta = makeV2Meta({ keyId: 'AAAAAAAAAAAAAAAAAAAAAA', direction: 'usb-to-ble',
        itemId: 1, headerLen: 50, payloadSize });
      // Count/authentication receipts are modeled; segmented AEAD is a later todo.
      const proof = { authenticatedSegments: meta.segmentCount, streamPlaintextBytes: 50n + payloadSize,
        headerLen: 50, payloadSize, payloadBytes: payloadSize, payloadSha256: hex(original), actualSha256: hex(original) };
      verifyV2Completion(meta, proof);
      rejects(() => verifyV2Completion(meta, { ...proof, actualSha256: hex(mutated) }), /stream completion verification failed/);
    }],
  ];
}
