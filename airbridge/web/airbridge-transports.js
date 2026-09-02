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
const HEADER_LEN = 5;

const SERIAL_UUIDS = [
  {
    name: 'airbridge',
    service: identity.SERIAL_SERVICE_UUID,
    tx: identity.SERIAL_TX_CHAR_UUID,
    rx: identity.SERIAL_RX_CHAR_UUID,
  },
];

const BLE_RECONNECT_DELAYS_MS = [500, 1000, 2000, 4000, 8000];

function normalizeFrame(frame) {
  if (frame instanceof Uint8Array) return frame;
  if (ArrayBuffer.isView(frame)) {
    return new Uint8Array(frame.buffer, frame.byteOffset, frame.byteLength);
  }
  if (frame instanceof ArrayBuffer) return new Uint8Array(frame);
  throw new TypeError('frame must be a Uint8Array, ArrayBuffer, or typed array');
}

function stripFramePadding(data) {
  const frame = normalizeFrame(data);
  if (frame.length < HEADER_LEN) return frame;

  const payloadLen = (frame[3] << 8) | frame[4];
  const frameLen = HEADER_LEN + payloadLen;
  if (frameLen > frame.length) return frame;
  return frame.slice(0, frameLen);
}

/**
 * Thin WebHID transport adapter for Pocket AirBridge USB frames.
 * It only opens/closes the HID device, writes 64-byte reports, and forwards
 * received frames after trimming HID padding based on the protocol length byte.
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
   * Register the callback fired for each incoming unpadded frame.
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
    this.receiveCallback(stripFramePadding(new Uint8Array(data.buffer, data.byteOffset, data.byteLength)));
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
 * It only opens/closes GATT, writes frames to RX with response, and forwards TX
 * characteristic indications as Uint8Array frames.
 */
export class WebBluetoothAdapter {
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
  }

  /**
   * Open a GATT session, preferring a previously origin-granted device over the picker.
   * @returns {Promise<void>}
   */
  async connect() {
    if (!navigator.bluetooth) throw new Error('Web Bluetooth is not available in this browser');
    this.cancelReconnect();
    this.autoReconnect = true;

    if (await this.tryGrantedReconnect()) return;

    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [identity.SERIAL_SERVICE_UUID] }],
      optionalServices: [identity.SERIAL_SERVICE_UUID],
    });
    await this.openGattSessionSerialized();
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
    const granted = await navigator.bluetooth.getDevices();
    const device = granted.find(candidate => candidate.name === identity.BLE_NAME);
    if (!device) return false;

    // Retry rationale: on macOS the bonded HID daemon can hold the peripheral
    // briefly after a deploy handoff, so service discovery may fail a few times.
    // 6 attempts at 750 ms (~4.5 s total) absorb the transient without falling
    // back to the user-visible picker.
    for (let attempt = 1; attempt <= 6; attempt++) {
      try {
        this.device = device;
        await this.openGattSessionSerialized();
        return true;
      } catch (error) {
        console.debug(`getDevices reconnect attempt ${attempt}/6 failed:`, error);
        this.clearGattState();
        // disconnect() raced the bring-up: abort the retry loop, or the next
        // attempt would resurrect the session the user just killed.
        if (!this.autoReconnect) throw error;
        if (attempt < 6) await new Promise(resolve => setTimeout(resolve, 750));
      }
    }
    device.removeEventListener('gattserverdisconnected', this.handleDisconnected);
    return false;
  }

  /**
   * Connect GATT on this.device and bring up the AirBridge serial characteristics.
   * A disconnect() racing this bring-up bumps reconnectEpoch and disarms
   * autoReconnect; the staleness check after each awaited GATT step then tears
   * down the partially opened session and throws instead of letting a stale
   * session come alive. (connect() bumps the epoch too, so staleness also
   * requires autoReconnect to be disarmed — only disconnect() does both, and a
   * fresh connect() can still share this in-flight bring-up.)
   * @returns {Promise<void>}
   */
  async openGattSession() {
    const device = this.device;
    const epoch = this.reconnectEpoch;
    const staleSession = () => epoch !== this.reconnectEpoch && !this.autoReconnect;
    const throwIfStale = () => {
      if (staleSession()) throw new Error('GATT session cancelled by disconnect');
    };
    device.addEventListener('gattserverdisconnected', this.handleDisconnected);
    try {
      this.server = await device.gatt.connect();
      throwIfStale();

      const discoveredServices = await this.server.getPrimaryServices();
      throwIfStale();

      let service;
      let uuids;
      try {
        ({ service, uuids } = await this.findSerialService());
      } catch (error) {
        const discoveredUuids = discoveredServices.map(discovered => discovered.uuid).join(', ') || '(none)';
        console.debug('Web Bluetooth discovered services:', discoveredUuids);
        throw new Error(`${error.message}. Discovered services: ${discoveredUuids}`);
      }
      throwIfStale();

      this.matchedUuids = uuids;
      this.txChar = await service.getCharacteristic(uuids.tx);
      this.rxChar = await service.getCharacteristic(uuids.rx);
      throwIfStale();

      await this.txChar.startNotifications();
      throwIfStale();
      this.txChar.addEventListener('characteristicvaluechanged', this.handleCharacteristicValueChanged);
    } catch (error) {
      if (staleSession()) {
        device.removeEventListener('gattserverdisconnected', this.handleDisconnected);
        if (device.gatt?.connected) device.gatt.disconnect();
        if (this.server?.device === device) this.clearGattState();
      }
      throw error;
    }
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
    this.gattSessionPromise = this.openGattSession().finally(() => {
      if (this.gattSessionDevice === device) {
        this.gattSessionPromise = null;
        this.gattSessionDevice = null;
      }
    });
    return this.gattSessionPromise;
  }

  /**
   * Disconnect GATT, cancel pending auto-reconnect retries, and clear
   * characteristic listeners. The reconnectEpoch bump also makes an in-flight
   * openGattSession() tear down its partially opened session and throw.
   * @returns {Promise<void>}
   */
  async disconnect() {
    this.autoReconnect = false;
    this.cancelReconnect();
    const device = this.device;
    this.clearGattState();
    if (!device) return;

    device.removeEventListener('gattserverdisconnected', this.handleDisconnected);
    if (device.gatt?.connected) device.gatt.disconnect();
  }

  /**
   * Send one protocol frame to the RX characteristic using write-with-response.
   * @param {Uint8Array|ArrayBuffer|ArrayBufferView} frame Unpadded frame bytes.
   * @returns {Promise<void>}
   */
  async send(frame) {
    if (!this.rxChar) throw new Error('Web Bluetooth transport is not connected');
    await this.rxChar.writeValueWithResponse(normalizeFrame(frame));
  }

  /**
   * Register the callback fired for each incoming TX indication frame.
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

  async findSerialService() {
    for (const uuids of SERIAL_UUIDS) {
      try {
        const service = await this.server.getPrimaryService(uuids.service);
        return { service, uuids };
      } catch (_) {
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

  handleDisconnected() {
    this.clearGattState();
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
    if (this.txChar) {
      this.txChar.removeEventListener('characteristicvaluechanged', this.handleCharacteristicValueChanged);
    }
    this.server = null;
    this.txChar = null;
    this.rxChar = null;
    this.matchedUuids = null;
  }
}
