const HID_FILTERS = [
  { vendorId: 0x046D, productId: 0xC31C },
  { vendorId: 0x413C, productId: 0x2113 },
  { vendorId: 0x045E, productId: 0x07F8 },
  { vendorId: 0x045E, productId: 0x07A5 },
  { vendorId: 0x03F0, productId: 0x5341 },
];

const PINNED_PIDS = new Set(
  HID_FILTERS.map(f => `${f.vendorId.toString(16).padStart(4,'0')}:${f.productId.toString(16).padStart(4,'0')}`),
);
const HID_REPORT_ID = 0x00;
const HID_REPORT_LEN = 64;
const HEADER_LEN = 5;

const SERIAL_UUIDS = [
  // Real Flipper UUIDs decoded from `serial_service_uuid.inc`.
  {
    name: 'flipper-actual',
    service: '60fe0000-7acc-2a48-984a-7f2ed5b3e58f',
    tx: '61fe0000-228e-4145-9d4c-21edae82ed19',
    rx: '62fe0000-228e-4145-9d4c-21edae82ed19',
  },
  {
    name: 'browser-canonical',
    service: '8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000',
    tx: '19ed82ae-ed21-4c9d-4145-228e61fe0000',
    rx: '19ed82ae-ed21-4c9d-4145-228e62fe0000',
  },
  {
    name: 'firmware-byte-order',
    service: '0000fe60-cc7a-482a-984a-7f2ed5b3e58f',
    tx: '0000fe61-8e22-4541-9d4c-21edae82ed19',
    rx: '0000fe62-8e22-4541-9d4c-21edae82ed19',
  },
];

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
    // Prefer an exact vendorId+productId match from the pinned set; fall back to
    // vendorId-only (legacy entries) only when no pinned device is available.
    let device = cachedDevices.find(
      ({ vendorId, productId }) =>
        PINNED_PIDS.has(`${vendorId.toString(16).padStart(4,'0')}:${productId.toString(16).padStart(4,'0')}`),
    );
    if (!device) {
      device = cachedDevices.find(({ vendorId, productId }) =>
        HID_FILTERS.some(filter => filter.vendorId === vendorId && filter.productId == null),
      );
    }

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
    this.handleCharacteristicValueChanged = this.handleCharacteristicValueChanged.bind(this);
    this.handleDisconnected = this.handleDisconnected.bind(this);
  }

  /**
   * Open the Web Bluetooth picker, connect GATT, and subscribe to TX indications.
   * @returns {Promise<void>}
   */
  async connect() {
    if (!navigator.bluetooth) throw new Error('Web Bluetooth is not available in this browser');

    this.device = await navigator.bluetooth.requestDevice({
      filters: [{ namePrefix: 'Flipper' }],
      optionalServices: [0x3080, ...SERIAL_UUIDS.map(u => u.service)],
    });
    this.device.addEventListener('gattserverdisconnected', this.handleDisconnected);
    this.server = await this.device.gatt.connect();

    const discoveredServices = await this.server.getPrimaryServices();

    let service;
    let uuids;
    try {
      ({ service, uuids } = await this.findSerialService());
    } catch (error) {
      const discoveredUuids = discoveredServices.map(discovered => discovered.uuid).join(', ') || '(none)';
      console.debug('Web Bluetooth discovered services:', discoveredUuids);
      throw new Error(`${error.message}. Discovered services: ${discoveredUuids}`);
    }
    this.matchedUuids = uuids;
    this.txChar = await service.getCharacteristic(uuids.tx);
    this.rxChar = await service.getCharacteristic(uuids.rx);
    await this.txChar.startNotifications();
    this.txChar.addEventListener('characteristicvaluechanged', this.handleCharacteristicValueChanged);
  }

  /**
   * Disconnect GATT and clear characteristic listeners.
   * @returns {Promise<void>}
   */
  async disconnect() {
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
      } catch (_) {}
    }
    throw new Error('Flipper Serial BLE service not found');
  }

  handleCharacteristicValueChanged(event) {
    if (!this.receiveCallback) return;
    const value = event.target.value;
    this.receiveCallback(new Uint8Array(value.buffer, value.byteOffset ?? 0, value.byteLength));
  }

  handleDisconnected() {
    this.clearGattState();
    if (this.disconnectCallback) this.disconnectCallback();
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
