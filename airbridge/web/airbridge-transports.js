import { identity } from './airbridge-identity.js';

const HID_FILTERS = [
  { vendorId: 0x046D, productId: 0xC31C, usagePage: 0xFF00 },
  { vendorId: 0x413C, productId: 0x2113, usagePage: 0xFF00 },
  { vendorId: 0x045E, productId: 0x07F8, usagePage: 0xFF00 },
  { vendorId: 0x045E, productId: 0x07A5, usagePage: 0xFF00 },
  { vendorId: 0x03F0, productId: 0x5341, usagePage: 0xFF00 },
];

const PINNED_PIDS = new Set(
  HID_FILTERS.map(f => `${f.vendorId.toString(16).padStart(4,'0')}:${f.productId.toString(16).padStart(4,'0')}`),
);
const HID_REPORT_ID = 0x00;
const HID_REPORT_LEN = 64;

const SERIAL_UUIDS = [
  {
    name: 'airbridge',
    service: identity.SERIAL_SERVICE_UUID,
    tx: identity.SERIAL_TX_CHAR_UUID,
    rx: identity.SERIAL_RX_CHAR_UUID,
  },
];

const BLE_RECONNECT_DELAYS_MS = [500, 1000, 2000, 4000, 8000];
const BLE_CONNECT_TIMEOUT_MS = 25000;
const BLE_SETUP_RETRY_DELAYS_MS = [500, 1000];

function normalizeFrame(frame) {
  if (frame instanceof Uint8Array) return frame;
  if (ArrayBuffer.isView(frame)) {
    return new Uint8Array(frame.buffer, frame.byteOffset, frame.byteLength);
  }
  if (frame instanceof ArrayBuffer) return new Uint8Array(frame);
  throw new TypeError('frame must be a Uint8Array, ArrayBuffer, or typed array');
}

/**
 * Thin WebHID transport adapter for Pocket AirBridge USB frames.
 * It only opens/closes the HID device, writes 64-byte reports, and forwards
 * owned complete reports so the protocol parser can validate their padding.
 */
export class WebHIDAdapter {
  /**
   * @param {{onDisconnect?:Function}} [options] Optional lifecycle callbacks.
   */
  constructor(options = {}) {
    this.device = null;
    this.receiveCallback = null;
    this.disconnectCallback = options.onDisconnect ?? null;
    this.handleInputReport = this.handleInputReport.bind(this);
    this.handleDisconnect = this.handleDisconnect.bind(this);
  }

  /**
   * Connect to a Pocket AirBridge HID device, preferring pre-authorized devices.
   * @returns {Promise<void>}
   */
  async connect() {
    if (!navigator.hid) throw new Error('WebHID is not available in this browser');

    const cachedDevices = await navigator.hid.getDevices();
    const cachedPinnedDevices = cachedDevices.filter(
      ({ vendorId, productId }) =>
        PINNED_PIDS.has(`${vendorId.toString(16).padStart(4,'0')}:${productId.toString(16).padStart(4,'0')}`),
    );
    let device = cachedPinnedDevices.find(({ collections }) =>
      collections.some(c => c.usagePage === 0xFF00),
    );

    if (!device) {
      const devices = await navigator.hid.requestDevice({ filters: HID_FILTERS });
      if (!devices.length) throw new Error('No HID device selected');
      [device] = devices;
    }

    this.device = device;
    await this.device.open();
    this.device.addEventListener('inputreport', this.handleInputReport);
    navigator.hid.addEventListener('disconnect', this.handleDisconnect);
  }

  /**
   * Close the HID device and clear browser event listeners.
   * @returns {Promise<void>}
   */
  async disconnect() {
    const device = this.device;
    this.device = null;

    if (navigator.hid) navigator.hid.removeEventListener('disconnect', this.handleDisconnect);
    if (!device) return;

    device.removeEventListener('inputreport', this.handleInputReport);
    if (device.opened) await device.close();
  }

  /**
   * Send one protocol frame as a padded 64-byte HID report.
   * @param {Uint8Array|ArrayBuffer|ArrayBufferView} frame Unpadded frame bytes.
   * @returns {Promise<void>}
   */
  async send(frame) {
    if (!this.device || !this.device.opened) throw new Error('WebHID transport is not connected');

    const bytes = normalizeFrame(frame);
    if (bytes.length > HID_REPORT_LEN) throw new RangeError(`HID frame must be ${HID_REPORT_LEN} bytes or less`);

    const report = new Uint8Array(HID_REPORT_LEN);
    report.set(bytes);
    await this.device.sendReport(HID_REPORT_ID, report);
  }

  /**
   * Register the callback fired for each incoming complete report.
   * @param {Function} callback Receives an owned 64-byte Uint8Array report.
   * @returns {Function} Unsubscribe callback.
   */
  onReceive(callback) {
    if (typeof callback !== 'function') throw new TypeError('callback must be a function');
    this.receiveCallback = callback;
    return () => {
      if (this.receiveCallback === callback) this.receiveCallback = null;
    };
  }

  /**
   * Register the callback fired when the browser reports transport disconnect.
   * @param {Function} callback Disconnect callback.
   * @returns {Function} Unsubscribe callback.
   */
  onDisconnect(callback) {
    if (typeof callback !== 'function') throw new TypeError('callback must be a function');
    this.disconnectCallback = callback;
    return () => {
      if (this.disconnectCallback === callback) this.disconnectCallback = null;
    };
  }

  /**
   * @returns {boolean} True when the HID device is currently open.
   */
  isConnected() {
    return Boolean(this.device?.opened);
  }

  /**
   * @returns {string} Human-readable transport/device name.
   */
  getName() {
    return this.device?.productName || 'WebHID';
  }

  handleInputReport(event) {
    if (event.reportId !== HID_REPORT_ID || !this.receiveCallback) return;
    const data = event.data;
    if (data.byteLength !== HID_REPORT_LEN) return;
    const bytes = new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    this.receiveCallback(new Uint8Array(bytes));
  }

  handleDisconnect(event) {
    if (event.device !== this.device) return;
    const device = this.device;
    this.device = null;
    device.removeEventListener('inputreport', this.handleInputReport);
    navigator.hid.removeEventListener('disconnect', this.handleDisconnect);
    if (this.disconnectCallback) this.disconnectCallback();
  }
}

/**
 * Thin Web Bluetooth transport adapter for Pocket AirBridge BLE Serial frames.
 * It only opens/closes GATT, writes frames to RX, and forwards TX
 * characteristic notifications as Uint8Array frames.
 */
export class WebBluetoothAdapter {
  #writes = {tail:Promise.resolve(), pending:0};
  /**
   * @param {{onDisconnect?:Function}} [options] Optional lifecycle callbacks.
   */
  constructor(options = {}) {
    this.device = null;
    this.server = null;
    this.txChar = null;
    this.rxChar = null;
    this.matchedUuids = null;
    this.receiveCallback = null;
    this.disconnectCallback = options.onDisconnect ?? null;
    this.autoReconnect = false;
    this.reconnectTimer = null;
    this.reconnectEpoch = 0;
    this.gattSessionPromise = null;
    this.gattSessionDevice = null;
    this.handleCharacteristicValueChanged = this.handleCharacteristicValueChanged.bind(this);
    this.handleDisconnected = this.handleDisconnected.bind(this);
    this.handleVisibilityChange = this.handleVisibilityChange.bind(this);
    this.handleBeforeUnload = this.handleBeforeUnload.bind(this);
    this.connectAttemptId = 0;
    this.activeAttemptId = -1;
    this.pendingCleanup = null;
    this.connectTimeoutMs = options.connectTimeoutMs ?? BLE_CONNECT_TIMEOUT_MS;
    this.setupRetryDelaysMs = options.setupRetryDelaysMs ?? BLE_SETUP_RETRY_DELAYS_MS;
    this.statusCallback = options.onStatus ?? null;
    document.addEventListener('visibilitychange', this.handleVisibilityChange);
    window.addEventListener('beforeunload', this.handleBeforeUnload);
  }

  /**
   * Tear down a half-open GATT connect when the page becomes hidden, so a
   * stalled bring-up never leaves the OS link dangling after the user leaves.
   */
  handleVisibilityChange() {
    if (document.visibilityState === 'hidden') this.teardownHalfOpen();
  }

  /**
   * Tear down a half-open GATT connect on page unload.
   */
  handleBeforeUnload() {
    this.teardownHalfOpen();
  }

  teardownHalfOpen() {
    if (!this.device) return;
    if (this.device.gatt?.connected && !this.isConnected()) {
      this.disconnect();
    }
  }

  /**
   * Release lifecycle listeners and disconnect. Call when the adapter is no
   * longer used (e.g. the page switches transports or unloads).
   */
  dispose() {
    document.removeEventListener('visibilitychange', this.handleVisibilityChange);
    window.removeEventListener('beforeunload', this.handleBeforeUnload);
    this.disconnect();
  }

  /**
   * Open a GATT session, preferring a previously origin-granted device over the picker.
   * @returns {Promise<void>}
   */
  async connect() {
    if (!navigator.bluetooth) throw new Error('Web Bluetooth is not available in this browser');
    this.cancelReconnect();
    this.autoReconnect = true;

    const epoch = this.reconnectEpoch;
    const assertCurrent = () => {
      if (epoch !== this.reconnectEpoch || !this.autoReconnect) throw new Error('GATT session cancelled');
    };
    try {
      const reconnected = await this.tryGrantedReconnect();
      assertCurrent();
      if (reconnected) return;
      const device = await navigator.bluetooth.requestDevice({
        filters: [{ services: [identity.SERIAL_SERVICE_UUID] }],
        optionalServices: [identity.SERIAL_SERVICE_UUID],
      });
      assertCurrent();
      this.device = device;
      await this.openGattSessionWithRetry();
    } catch (error) {
      // Failed initial setup must not leave a background reconnect alive after
      // the page releases this adapter. Preserve any newer connect() owner.
      if (epoch === this.reconnectEpoch) await this.disconnect();
      throw error;
    }
  }

  async openGattSessionWithRetry() {
    const device = this.device, epoch = this.reconnectEpoch;
    const assertCurrent = () => {
      if (!this.autoReconnect || epoch !== this.reconnectEpoch || this.device !== device) throw new Error('GATT session cancelled');
    };
    for (let retry = 0; ; retry++) {
      assertCurrent();
      try {
        await this.openGattSessionSerialized();
        assertCurrent();
        return;
      } catch (error) {
        assertCurrent();
        // Retry link failures on the selected device. Permission, missing
        // service, timeout, and cancellation errors are not transient drops.
        if (error.name !== 'NetworkError' || retry >= this.setupRetryDelaysMs.length) throw error;
        this.statusCallback?.(`BLE reconnecting (${retry + 2}/${this.setupRetryDelaysMs.length + 1})`);
        await new Promise(resolve => setTimeout(resolve, this.setupRetryDelaysMs[retry]));
      }
    }
  }

  /**
   * Try to reconnect to a device this origin already granted, skipping the picker.
   * getDevices() needs no fresh advertising, which matters on macOS: once paired,
   * the HID daemon claims the peripheral, it stops advertising, and the picker
   * finds nothing — while a granted gatt.connect() still reaches it by address.
   * @returns {Promise<boolean>} True when the GATT session is live.
   */
  async tryGrantedReconnect() {
    // getDevices is flag-gated (#enable-experimental-web-platform-features);
    // when the flags are off it is undefined — skip straight to the picker.
    if (!navigator.bluetooth.getDevices) return false;
    const epoch = this.reconnectEpoch;
    const granted = await navigator.bluetooth.getDevices();
    const device = granted.find(candidate => candidate.name === identity.BLE_NAME);
    if (!device) return false;

    // Retry rationale: on macOS the bonded HID daemon can hold the peripheral
    // briefly after a deploy handoff, so service discovery may fail a few times.
    // 6 attempts at 750 ms (~4.5 s total) absorb the transient without falling
    // back to the user-visible picker.
    for (let attempt = 1; attempt <= 6; attempt++) {
      if (!this.autoReconnect || epoch !== this.reconnectEpoch) throw new Error('GATT session cancelled');
      try {
        this.device = device;
        await this.openGattSessionSerialized();
        return true;
      } catch (error) {
        console.debug(`getDevices reconnect attempt ${attempt}/6 failed:`, error);
        this.clearGattState();
        // disconnect() raced the bring-up: abort the retry loop, or the next
        // attempt would resurrect the session the user just killed.
        if (!this.autoReconnect || epoch !== this.reconnectEpoch) throw error;
        if (attempt < 6) {
          this.statusCallback?.(`BLE reconnecting (${attempt + 1}/6)`);
          await new Promise(resolve => setTimeout(resolve, 750));
        }
      }
    }
    device.removeEventListener('gattserverdisconnected', this.handleDisconnected);
    return false;
  }

  /**
   * Connect GATT on this.device and bring up the AirBridge serial characteristics.
   * A disconnect() or timeout invalidates the attempt ID. Each awaited GATT
   * step checks that ID and the physical link before proceeding. Keep handles
   * local until notification setup succeeds so a drop cannot publish a
   * partially connected session or replace a newer attempt's handles.
   * @returns {Promise<void>}
   */
  async openGattSession() {
    const attempt = {
      id: this.connectAttemptId,
      device: this.device,
    };
    this.activeAttemptId = attempt.id;
    const staleSession = () => attempt.id !== this.connectAttemptId;
    const throwIfStale = () => {
      if (staleSession()) throw new Error('GATT session cancelled');
      if (!attempt.device.gatt?.connected) throw new DOMException('BLE disconnected during setup', 'NetworkError');
    };
    const device = attempt.device;
    device.addEventListener('gattserverdisconnected', this.handleDisconnected);
    try {
      await this.withConnectTimeout(attempt, async () => {
        const server = await device.gatt.connect();
        throwIfStale();
        this.server = server;

        const discoveredServices = await server.getPrimaryServices();
        throwIfStale();

        let service;
        let uuids;
        try {
          ({ service, uuids } = await this.findSerialService(server));
        } catch (error) {
          if (error.name === 'NetworkError') throw error;
          const discoveredUuids = discoveredServices.map(discovered => discovered.uuid).join(', ') || '(none)';
          console.debug('Web Bluetooth discovered services:', discoveredUuids);
          throw new Error(`${error.message}. Discovered services: ${discoveredUuids}`);
        }
        throwIfStale();

        const txChar = await service.getCharacteristic(uuids.tx);
        throwIfStale();
        const rxChar = await service.getCharacteristic(uuids.rx);
        throwIfStale();

        await txChar.startNotifications();
        throwIfStale();
        this.matchedUuids = uuids;
        this.txChar = txChar;
        this.rxChar = rxChar;
        this.txChar.addEventListener('characteristicvaluechanged', this.handleCharacteristicValueChanged);
      });
      throwIfStale();
    } catch (error) {
      /* Every bring-up failure — timeout, stale attempt, discovery error,
       * startNotifications error, generic exception — must tear the OS link
       * down and reset adapter state so no dangling connection survives. */
      device.removeEventListener('gattserverdisconnected', this.handleDisconnected);
      if (device.gatt?.connected) device.gatt.disconnect();
      if (this.server?.device === device) this.clearGattState();
      throw error;
    }
  }

  /**
   * Bound the GATT connect + discovery + notification bring-up with a hard
   * deadline. A stalled Web Bluetooth promise must never leave the OS link
   * dangling: on timeout we invalidate the attempt (so a late completion
   * detects staleness and disconnects) and reject.
   * @param {Function} bringUp Async function performing the GATT bring-up.
   * @returns {Promise<void>}
   */
  async withConnectTimeout(attempt, bringUp) {
    const bringUpPromise = bringUp();
    let timer;
    const timeout = new Promise((_, reject) => {
      timer = setTimeout(() => {
        this.connectAttemptId += 1;
        reject(new Error('BLE connect timed out'));
      }, this.connectTimeoutMs);
    });
    try {
      await Promise.race([bringUpPromise, timeout]);
    } catch (error) {
      /* The timeout may have won while the bring-up is still settling. Track the
       * bring-up separately so its LATE settlement always tears the OS link down
       * and clears state — never leave a dangling connection behind. */
      if (this.connectAttemptId !== attempt.id) {
        this.pendingCleanup = bringUpPromise.then(
          () => this.cleanupStaleAttempt(attempt),
          () => this.cleanupStaleAttempt(attempt),
        );
      }
      throw error;
    } finally {
      clearTimeout(timer);
    }
  }

  /**
   * Tear down a stale attempt's late-settled GATT session. Uses the attempt's
   * OWN device reference (captured when the attempt started), never the mutable
   * this.device — a replacement session on a different device must not be
   * disconnected by an older attempt's cleanup. Guarded by the active attempt
   * id so a replacement session is never torn down.
   * @param {{id:number, device:object}} attempt The stale attempt record.
   */
  cleanupStaleAttempt(attempt) {
    if (this.activeAttemptId !== attempt.id && this.activeAttemptId !== -1) return;
    const device = attempt.device;
    if (device) {
      device.removeEventListener('gattserverdisconnected', this.handleDisconnected);
      if (device.gatt?.connected) device.gatt.disconnect();
    }
    this.clearGattState();
  }

  /**
   * Bring up the GATT session, sharing any in-flight bring-up on the same
   * device. A fresh connect() racing a pending auto-reconnect attempt must not
   * issue a second gatt.connect() — Chromium rejects concurrent connects.
   * @returns {Promise<void>}
   */
  openGattSessionSerialized() {
    const device = this.device;
    if (this.gattSessionPromise && this.gattSessionDevice === device) {
      return this.gattSessionPromise;
    }
    this.gattSessionDevice = device;
    this.gattSessionPromise = (async () => {
      /* Serialize later attempts until a stale attempt's late cleanup
       * completes, so a replacement session never races the old teardown. The
       * wait is bounded: a permanently-unresolved cleanup must fail the retry
       * with an explicit error instead of hanging forever. */
      if (this.pendingCleanup) await this.waitForCleanup();
      return this.openGattSession();
    })().finally(() => {
      if (this.gattSessionDevice === device) {
        this.gattSessionPromise = null;
        this.gattSessionDevice = null;
      }
    });
    return this.gattSessionPromise;
  }

  /**
   * Wait for a stale attempt's late cleanup to finish, bounded by the connect
   * timeout. If the cleanup never resolves, reject with an explicit
   * cleanup-pending error so the retry fails cleanly instead of hanging.
   * @returns {Promise<void>}
   */
  async waitForCleanup() {
    const cleanup = this.pendingCleanup;
    if (!cleanup) return;
    let timer;
    const deadline = new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error('BLE cleanup pending')), this.connectTimeoutMs);
    });
    try {
      await Promise.race([cleanup, deadline]);
    } finally {
      clearTimeout(timer);
    }
    if (this.pendingCleanup === cleanup) this.pendingCleanup = null;
  }

  /**
   * Disconnect GATT, cancel pending auto-reconnect retries, and clear
   * characteristic listeners. The reconnectEpoch bump also makes an in-flight
   * openGattSession() tear down its partially opened session and throw.
   * @returns {Promise<void>}
   */
  async disconnect() {
    this.autoReconnect = false;
    this.connectAttemptId += 1;
    this.activeAttemptId = -1;
    this.cancelReconnect();
    const device = this.device;
    this.clearGattState();
    if (!device) return;

    device.removeEventListener('gattserverdisconnected', this.handleDisconnected);
    if (device.gatt?.connected) device.gatt.disconnect();
  }

  /**
   * Send one protocol frame, preferring write-without-response when supported.
   * Protocol ACKs retain end-to-end delivery checks and backpressure.
   * @param {Uint8Array|ArrayBuffer|ArrayBufferView} frame Unpadded frame bytes.
   * @returns {Promise<void>}
   */
  async send(frame) {
    const rxChar = this.rxChar;
    if (!rxChar) throw new Error('Web Bluetooth transport is not connected');
    const writes = this.#writes;
    if (writes.pending >= 8) throw new Error('BLE write queue full');
    const view = normalizeFrame(frame);
    if (view.byteLength > 64) throw new RangeError('BLE frame must be 64 bytes or less');
    const bytes = new Uint8Array(view);
    writes.pending++;
    const sent = writes.tail.then(async () => {
      if (this.#writes !== writes || this.rxChar !== rxChar) throw new Error('Web Bluetooth transport is not connected');
      if (rxChar.properties?.writeWithoutResponse && typeof rxChar.writeValueWithoutResponse === 'function') {
        await rxChar.writeValueWithoutResponse(bytes);
      } else {
        await rxChar.writeValueWithResponse(bytes);
      }
    });
    writes.tail = sent.catch(() => {});
    try { await sent; }
    finally { writes.pending--; }
  }

  /**
   * Register the callback fired for each incoming TX notification frame.
   * @param {Function} callback Receives a Uint8Array frame.
   * @returns {Function} Unsubscribe callback.
   */
  onReceive(callback) {
    if (typeof callback !== 'function') throw new TypeError('callback must be a function');
    this.receiveCallback = callback;
    return () => {
      if (this.receiveCallback === callback) this.receiveCallback = null;
    };
  }

  /**
   * Register the callback fired when GATT disconnects.
   * @param {Function} callback Disconnect callback.
   * @returns {Function} Unsubscribe callback.
   */
  onDisconnect(callback) {
    if (typeof callback !== 'function') throw new TypeError('callback must be a function');
    this.disconnectCallback = callback;
    return () => {
      if (this.disconnectCallback === callback) this.disconnectCallback = null;
    };
  }

  /**
   * @returns {boolean} True when the BLE GATT connection is active.
   */
  isConnected() {
    return Boolean(this.device?.gatt?.connected && this.rxChar && this.txChar);
  }

  /**
   * @returns {string} Human-readable transport/device name.
   */
  getName() {
    return this.device?.name || 'Web Bluetooth';
  }

  async findSerialService(server = this.server) {
    for (const uuids of SERIAL_UUIDS) {
      try {
        const service = await server.getPrimaryService(uuids.service);
        return { service, uuids };
      } catch (error) {
        if (error.name !== 'NotFoundError') throw error;
        // A miss is expected while probing each supported serial-service UUID.
      }
    }
    throw new Error('AirBridge serial BLE service not found');
  }

  handleCharacteristicValueChanged(event) {
    if (!this.receiveCallback) return;
    const value = event.target.value;
    this.receiveCallback(new Uint8Array(value.buffer, value.byteOffset ?? 0, value.byteLength));
  }

  handleDisconnected(event) {
    if (event?.target && event.target !== this.device) return;
    const settingUp = Boolean(this.gattSessionPromise) || !this.txChar || !this.rxChar;
    this.clearGattState();
    // The active setup/reconnect attempt owns its failure and retry. Starting
    // another timer here races that attempt and can orphan a live GATT link.
    if (settingUp) return;
    if (!this.autoReconnect) {
      if (this.disconnectCallback) this.disconnectCallback();
      return;
    }
    this.cancelReconnect();
    this.scheduleReconnect(0);
  }

  /**
   * Retry openGattSession() after an unintentional drop, one attempt per
   * BLE_RECONNECT_DELAYS_MS entry; on exhaustion disarm and fire the
   * disconnect callback. A stale epoch (connect/disconnect/fresh drop since
   * scheduling) stops the chain; a disconnect() racing an in-flight attempt
   * tears down the session that attempt opened.
   * @param {number} delayIndex Index into BLE_RECONNECT_DELAYS_MS.
   */
  scheduleReconnect(delayIndex) {
    const epoch = this.reconnectEpoch;
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      if (epoch !== this.reconnectEpoch || !this.autoReconnect || !this.device) return;
      this.openGattSessionSerialized().then(
        () => {
          if (!this.autoReconnect) {
            const device = this.device;
            this.clearGattState();
            if (device) {
              device.removeEventListener('gattserverdisconnected', this.handleDisconnected);
              if (device.gatt?.connected) device.gatt.disconnect();
            }
          }
        },
        (error) => {
          if (epoch !== this.reconnectEpoch || !this.autoReconnect) return;
          console.debug(`BLE auto-reconnect attempt ${delayIndex + 1}/${BLE_RECONNECT_DELAYS_MS.length} failed:`, error);
          this.clearGattState();
          if (delayIndex + 1 < BLE_RECONNECT_DELAYS_MS.length) {
            this.scheduleReconnect(delayIndex + 1);
          } else {
            this.autoReconnect = false;
            if (this.disconnectCallback) this.disconnectCallback();
          }
        },
      );
    }, BLE_RECONNECT_DELAYS_MS[delayIndex]);
  }

  /**
   * Invalidate the pending auto-reconnect chain and clear its timer.
   */
  cancelReconnect() {
    this.reconnectEpoch += 1;
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  clearGattState() {
    this.#writes = {tail:Promise.resolve(), pending:0};
    if (this.txChar) {
      this.txChar.removeEventListener('characteristicvaluechanged', this.handleCharacteristicValueChanged);
    }
    this.server = null;
    this.txChar = null;
    this.rxChar = null;
    this.matchedUuids = null;
  }
}
