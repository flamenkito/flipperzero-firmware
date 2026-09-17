export function blePacketControl(kind, nonce, count = 3) {
  const packet = new Uint8Array(kind === 1 ? count * 64 : 64).fill(0xa5);
  packet.set([0xf0, 65, 66, 80, 1, kind, count, 0]);
  packet.set(nonce, 8);
  return packet;
}

export class BlePackets {
  constructor(write, receive, {timeoutMs = 750, onError = () => {}} = {}) {
    this.write = write;
    this.receive = receive;
    this.timeoutMs = timeoutMs;
    this.onError = onError;
    this.batchSize = 1;
    this.queue = [];
    this.pending = 0;
    this.running = false;
    this.closed = false;
    this.waiter = null;
  }

  async exchange(packet) {
    let timer;
    let writeSettled = false;
    const response = new Promise((resolve, reject) => {
      this.waiter = {packet, resolve, reject};
      timer = setTimeout(() => reject(new Error('BLE packet negotiation timed out')), this.timeoutMs);
    });
    try {
      await Promise.all([response, Promise.resolve().then(() => this.write(packet, true)).finally(() => { writeSettled = true; })]);
    } catch (error) {
      error.packetWriteSettled = writeSettled;
      throw error;
    } finally {
      clearTimeout(timer);
      this.waiter = null;
    }
  }

  async negotiate() {
    const nonce = crypto.getRandomValues(new Uint8Array(8));
    for (const count of [3, 2]) {
      try {
        await this.exchange(blePacketControl(1, nonce, count));
      } catch (error) {
        if (this.closed || !error.packetWriteSettled) throw error;
        continue;
      }
      if (this.closed) throw new Error('BLE packet transport closed');
      // A lost commit reply must disconnect: the peripheral may already be
      // emitting aggregated notifications, so falling back would corrupt framing.
      await this.exchange(blePacketControl(2, nonce, count));
      if (this.closed) throw new Error('BLE packet transport closed');
      this.batchSize = count;
      return;
    }
  }

  accept(bytes) {
    if (this.closed) return;
    if (bytes.length >= 8 && bytes[0] === 0xf0 && bytes[1] === 65 && bytes[2] === 66 && bytes[3] === 80) {
      const waiter = this.waiter;
      if (waiter && bytes.length === waiter.packet.length && bytes.every((v, i) => v === waiter.packet[i])) {
        if (bytes[5] === 2) this.batchSize = bytes[6];
        waiter.resolve();
      }
      return;
    }
    if (bytes.length <= 64) {
      this.receive(bytes.slice());
    } else if (this.batchSize > 1 && bytes.length <= this.batchSize * 64 && bytes.length % 64 === 0) {
      for (let offset = 0; offset < bytes.length; offset += 64)
        this.receive(bytes.slice(offset, offset + 64));
    } else {
      const error = new Error('Invalid BLE packet length');
      this.close(error);
      this.onError(error);
    }
  }

  send(bytes) {
    if (this.closed) return Promise.reject(new Error('BLE packet transport closed'));
    if (bytes.length > 64) return Promise.reject(new RangeError('BLE frame must be 64 bytes or less'));
    if (this.pending >= 32) return Promise.reject(new Error('BLE write queue full'));
    const owned = this.batchSize > 1 ? new Uint8Array(64) : new Uint8Array(bytes.length);
    owned.set(bytes);
    this.pending++;
    const result = new Promise((resolve, reject) => this.queue.push({bytes:owned, resolve, reject}));
    if (!this.running) {
      this.running = true;
      this.timer = setTimeout(() => { this.timer = null; void this.pump(); }, 0);
    }
    return result;
  }

  async pump() {
    try {
      while (!this.closed && this.queue.length) {
        const batch = this.queue.splice(0, this.batchSize);
        this.active = batch;
        const packet = new Uint8Array(batch.reduce((n, entry) => n + entry.bytes.length, 0));
        let offset = 0;
        for (const entry of batch) { packet.set(entry.bytes, offset); offset += entry.bytes.length; }
        await this.write(packet, false);
        if (this.closed) break;
        for (const entry of batch) { this.pending--; entry.resolve(); }
        this.active = null;
      }
    } catch (error) {
      this.close(error);
      this.onError(error);
    } finally {
      this.running = false;
    }
  }

  close(error = new Error('BLE packet transport closed')) {
    this.closed = true;
    clearTimeout(this.timer);
    this.waiter?.reject(error);
    for (const entry of [...(this.active ?? []), ...this.queue]) entry.reject(error);
    this.active = null;
    this.queue = [];
    this.pending = 0;
  }
}
