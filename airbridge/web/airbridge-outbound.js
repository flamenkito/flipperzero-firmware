import { buildV2Frame, encodeV2Header, ItemSender, MSG, parseMessage } from './airbridge-protocol.js';
import { FileHashPass } from './airbridge-file-pass.js';

export class OutboundTransfer {
  #session; #send; #sender; #stream; #onProgress; #current; #started = false;
  #controller = new AbortController();
  #helloAttempted = false; #cancelSent = false; #finished = false;
  #frames = 0; #hashReports = 0; #sendReports = 0; #ackedBytes = 0n;
  #unsubscribe; #cleanupError = null;
  itemId = null;
  size = 0n;
  constructor(options) {
    this.#session = options.session; this.#send = options.send;
    this.hashImplementation = options.hashImplementation;
    this.#onProgress = options.onProgress ?? (() => {});
    this.#current = options.isCurrent ?? (() => true);
    this.#unsubscribe = this.#session.onInvalidated(() => this.cancel());
    this.#sender = new ItemSender(async frame => {
      this.assertActive();
      if (parseMessage(frame).type === MSG.HELLO) this.#helloAttempted = true;
      this.#frames++;
      await this.wait(this.#send(frame));
      this.assertActive();
    }, options);
  }
  get signal() { return this.#controller.signal; }
  get helloAttempted() { return this.#helloAttempted; }
  get cleanupError() { return this.#cleanupError; }
  recordCleanupError(error) { this.#cleanupError ??= error; }
  snapshot() {
    return Object.freeze({itemId:this.itemId, helloAttempted:this.#helloAttempted,
      cancelAttempts:Number(this.#cancelSent), frameAttempts:this.#frames,
      hashReports:this.#hashReports, sendReports:this.#sendReports,
      acknowledgedBytes:this.#ackedBytes, completed:this.#finished});
  }
  assertActive() {
    this.signal.throwIfAborted();
    if (!this.#current() || !this.#session.isUnlocked()) throw new DOMException('Inactive transfer', 'AbortError');
  }
  async wait(promise) {
    let onAbort;
    const aborted = new Promise((_, reject) => {
      try { this.assertActive(); } catch (error) { reject(error); return; }
      onAbort = () => reject(this.signal.reason);
      this.signal.addEventListener('abort', onAbort, {once:true});
    });
    try {
      const value = await Promise.race([promise, aborted]);
      this.assertActive();
      return value;
    } finally { this.signal.removeEventListener('abort', onAbort); }
  }
  progress(phase, bytes) {
    this.assertActive();
    if (phase === 'Hashing') this.#hashReports++;
    else { this.#sendReports++; this.#ackedBytes = bytes; }
    this.#onProgress(Object.freeze({phase, bytes, total:this.size, itemId:this.itemId}));
    this.assertActive();
  }
  receive(frame) {
    if (![MSG.ACK, MSG.NACK, MSG.ERROR, MSG.BUSY, MSG.CANCEL].includes(parseMessage(frame)?.type)) return false;
    return this.#sender.receiveStreamControl(frame);
  }
  cancel(reason = new DOMException('Transfer cancelled', 'AbortError')) {
    if (this.#finished || this.signal.aborted) return;
    this.#controller.abort(reason);
    this.#unsubscribe();
    this.#stream?.abort();
    this.#sender.abortEncryptedStream(reason);
    if (this.#helloAttempted && !this.#cancelSent) {
      this.#cancelSent = true;
      // Invoke on the captured transport now: no queued continuation may target a replacement.
      try { Promise.resolve(this.#send(buildV2Frame(MSG.CANCEL, 0, this.itemId))).catch(() => {}); }
      catch (_) { /* Best-effort cancellation cannot restore a disconnected transport. */ }
    }
  }
  async sendFile(file, kind = 'attachment') {
    if (this.#started) throw new Error('outbound transfer is single-use');
    this.#started = true;
    let pass, failed = false;
    try {
      this.assertActive();
      if (!Number.isSafeInteger(file.size) || file.size < 0) throw new Error('invalid file size');
      this.size = BigInt(file.size);
      this.progress('Hashing', 0n);
      await this.wait(new Promise(resolve => setTimeout(resolve, 0)));
      pass = new FileHashPass(file, this);
      for await (const unused of pass) void unused;
      this.assertActive();
      const header = {kind, name:file.name, mimeType:file.type || 'application/octet-stream',
        payloadSize:pass.bytes, payloadSha256:pass.digest};
      const headerLength = encodeV2Header(header).length;
      pass = new FileHashPass(file, this, header.payloadSha256);
      this.assertActive();
      this.#stream = this.#session.openEncryptedStream(header, pass);
      this.itemId = this.#stream.meta.itemId;
      this.assertActive();
      await this.wait(this.#sender.sendEncryptedStream(this.#stream, {
        onProgress:plainBytes => {
          const payload = plainBytes > BigInt(headerLength) ? plainBytes - BigInt(headerLength) : 0n;
          this.progress('Sending', payload);
        },
      }));
      this.assertActive();
      await pass.return();
      this.assertActive();
      this.#finished = true;
      return Object.freeze({...header, itemId:this.itemId});
    } catch (error) { failed = true; this.cancel(error); throw error;
    } finally {
      this.#unsubscribe();
      try { await pass?.return(); } catch (cleanup) {
        this.recordCleanupError(cleanup);
        if (!failed) throw cleanup;
      }
    }
  }
  sendText(text) {
    const bytes = new TextEncoder().encode(text);
    if (bytes.length > 65536) return Promise.reject(new Error('Text exceeds 65536 bytes; send an attachment'));
    return this.sendFile(new File([bytes], 'message.txt', {type:'text/plain;charset=utf-8'}), 'text');
  }
}
