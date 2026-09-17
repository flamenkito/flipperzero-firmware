import { test } from 'node:test';
import assert from 'node:assert/strict';

import { WebBluetoothAdapter } from '../../airbridge/web/airbridge-transports.js';
import { hidIngressTests } from '../web/protocol-hid-ingress-tests.js';

for (const [name, run] of hidIngressTests()) test(name, run);

const TX_UUID = '87825ec0-7398-8cb7-3242-b083eaa34f27';
const RX_UUID = '152f7eeb-e3b7-5898-ba41-7ff66121c98d';
const SERVICE_UUID = '7b871228-baf0-c5b4-5f46-9c2613d627a3';

function makeMockDevice({
  connectDelay = 0,
  connectGate = null,
  discoveryGate = null,
  onDiscovery = null,
  discoveryError = null,
  notifyError = null,
  packetFrames = 1,
  loseCommit = false,
} = {}) {
  const listeners = {};
  let connectCalls = 0;
  let notification;
  const txChar = {
    uuid: TX_UUID,
    startNotifications: async () => {
      if (notifyError) throw notifyError;
    },
    addEventListener: (_name, callback) => { notification = callback; },
    removeEventListener: () => { notification = null; },
  };
  const rxChar = { uuid: RX_UUID };
  // This fixture represents a legacy peripheral that rejects oversized writes.
  let responseWrite;
  Object.defineProperty(rxChar, 'writeValueWithResponse', {
    get: () => responseWrite,
    set: write => { responseWrite = async bytes => {
      if (bytes[0] === 0xf0) {
        if (packetFrames === 1 || bytes.length > packetFrames * 64) throw new RangeError('legacy ATT length');
        if (!(loseCommit && bytes[5] === 2)) notification?.({target:{value:new DataView(bytes.slice().buffer)}});
        return;
      }
      return write(bytes);
    }; },
  });
  const service = {
    uuid: SERVICE_UUID,
    getCharacteristic: async uuid => (uuid === TX_UUID ? txChar : rxChar),
  };
  const server = {
    device: null,
    getPrimaryServices: async () => {
      if (onDiscovery) onDiscovery();
      if (discoveryGate) await discoveryGate;
      if (discoveryError) throw discoveryError;
      return [service];
    },
    getPrimaryService: async () => service,
  };
  const gatt = {
    connected: false,
    connect: async () => {
      connectCalls += 1;
      if (connectDelay > 0) await new Promise(r => setTimeout(r, connectDelay));
      if (connectGate && connectCalls === 1) await connectGate;
      gatt.connected = true;
      server.device = device;
      return server;
    },
    disconnect: () => {
      gatt.connected = false;
    },
  };
  const device = {
    name: 'HP 725 K+M',
    gatt,
    addEventListener: (name, cb) => {
      listeners[name] = cb;
    },
    removeEventListener: (name) => {
      delete listeners[name];
    },
  };
  server.device = device;
  return { device, gatt, server, txChar, rxChar, listeners };
}

function installNavigatorBluetooth(device) {
  Object.defineProperty(globalThis, 'navigator', {
    value: {
      bluetooth: {
        requestDevice: async () => device,
        getDevices: undefined,
      },
    },
    configurable: true,
    writable: true,
  });
}

test('BLE adapter negotiates batching and resets framing on reconnect', async () => {
  const {device, rxChar} = makeMockDevice({packetFrames:3});
  installNavigatorBluetooth(device); installDomGlobals();
  const adapter = new WebBluetoothAdapter();
  const writes = [];
  rxChar.properties = {write:true, writeWithoutResponse:true};
  rxChar.writeValueWithResponse = async () => assert.fail('data used response write');
  rxChar.writeValueWithoutResponse = async bytes => writes.push(bytes.slice());
  try {
    await adapter.connect();
    assert.equal(adapter.packets.batchSize, 3);
    await Promise.all([1, 2, 3, 4].map(x => adapter.send(Uint8Array.of(x))));
    assert.deepEqual(writes.map(x => x.length), [192, 64]);
    assert.deepEqual([writes[0][0], writes[0][64], writes[0][128], writes[1][0]], [1, 2, 3, 4]);
    const old = adapter.packets;
    await adapter.disconnect();
    assert.equal(old.closed, true);
    assert.equal(adapter.packets, null);
    await adapter.connect();
    assert.notEqual(adapter.packets, old);
    assert.equal(adapter.packets.batchSize, 3);
  } finally { adapter.dispose(); }
});

function installDomGlobals() {
  const listeners = {};
  globalThis.document = {
    visibilityState: 'visible',
    addEventListener: (name, cb) => {
      listeners[name] = cb;
    },
    removeEventListener: (name) => {
      delete listeners[name];
    },
  };
  globalThis.window = {
    addEventListener: (name, cb) => {
      listeners[name] = cb;
    },
    removeEventListener: (name) => {
      delete listeners[name];
    },
  };
  return listeners;
}

test('BLE command writes preserve a BufferSource view and await transport completion', async () => {
  const { device, rxChar } = makeMockDevice();
  installNavigatorBluetooth(device);
  installDomGlobals();
  const adapter = new WebBluetoothAdapter();
  let release;
  const completion = new Promise(resolve => { release = resolve; });
  const writes = [];
  rxChar.properties = { write: true, writeWithoutResponse: true };
  rxChar.writeValueWithoutResponse = async bytes => { writes.push([...bytes]); await completion; };
  rxChar.writeValueWithResponse = async () => { assert.fail('extra GATT response requested'); };
  try {
    await adapter.connect();
    await assert.rejects(adapter.send(new Uint8Array(65)), /64 bytes or less/);
    const buffer = Uint8Array.of(99, 3, 0, 1, 0, 88);
    let settled = false;
    const sent = adapter.send(new DataView(buffer.buffer, 1, 4)).then(() => { settled = true; });
    await Promise.resolve();
    assert.deepEqual(writes, [[3, 0, 1, 0]]);
    assert.equal(settled, false, 'send settled before the transport write');
    release();
    await sent;
    assert.equal(settled, true);
    await adapter.disconnect();
    await assert.rejects(adapter.send(buffer), /not connected/);
    assert.equal(writes.length, 1, 'disconnected send reached the characteristic');
  } finally { release(); adapter.dispose(); }
});

for (const unsupported of ['characteristic', 'browser', 'properties']) {
  test(`BLE retains response writes when ${unsupported} lacks command-write support`, async () => {
    const { device, rxChar } = makeMockDevice();
    installNavigatorBluetooth(device);
    installDomGlobals();
    const adapter = new WebBluetoothAdapter();
    if (unsupported !== 'properties') rxChar.properties = { writeWithoutResponse: unsupported !== 'characteristic' };
    if (unsupported !== 'browser') rxChar.writeValueWithoutResponse = async () => { assert.fail('unsupported command write'); };
    const writes = [];
    rxChar.writeValueWithResponse = async bytes => { writes.push([...bytes]); };
    try {
      await adapter.connect();
      await adapter.send(Uint8Array.of(4, 0, 0).buffer);
      assert.deepEqual(writes, [[4, 0, 0]]);
    } finally { adapter.dispose(); }
  });
}

for (const method of ['writeValueWithoutResponse', 'writeValueWithResponse']) {
  test(`BLE ${method} failure propagates without replaying a possibly delivered frame`, async () => {
    const { device, rxChar } = makeMockDevice();
    installNavigatorBluetooth(device);
    installDomGlobals();
    const adapter = new WebBluetoothAdapter();
    const failure = new DOMException('link dropped during write', 'NetworkError');
    const writes = [];
    rxChar.properties = { writeWithoutResponse: method === 'writeValueWithoutResponse' };
    for (const name of ['writeValueWithoutResponse', 'writeValueWithResponse']) {
      rxChar[name] = async () => { writes.push(name); throw failure; };
    }
    try {
      await adapter.connect();
      await assert.rejects(adapter.send(Uint8Array.of(3, 0, 0)), error => error === failure);
      assert.deepEqual(writes, [method], 'transport silently replayed the write');
    } finally { adapter.dispose(); }
  });
}

test('BLE serializes a bounded write queue and owns queued bytes', async () => {
  const {device, rxChar} = makeMockDevice();
  installNavigatorBluetooth(device); installDomGlobals();
  const adapter = new WebBluetoothAdapter();
  let release, active = 0, peak = 0;
  const gate = new Promise(resolve => { release = resolve; });
  const writes = [];
  rxChar.properties = {writeWithoutResponse:true};
  rxChar.writeValueWithoutResponse = async bytes => {
    peak = Math.max(peak, ++active); writes.push([...bytes]);
    if (writes.length === 1) await gate;
    active--;
  };
  try {
    await adapter.connect();
    const buffer = Uint8Array.of(7);
    const pending = Array.from({length:8}, () => adapter.send(buffer));
    buffer[0] = 99;
    await assert.rejects(adapter.send(buffer), /queue full/);
    assert.equal(writes.length, 1);
    release(); await Promise.all(pending);
    assert.equal(peak, 1);
    assert.deepEqual(writes, Array.from({length:8}, () => [7]));
    await adapter.send(Uint8Array.of(8));
    assert.deepEqual(writes.at(-1), [8]);
  } finally { release(); adapter.dispose(); }
});

test('BLE reconnect discards queued writes even when a characteristic object is reused', async () => {
  const {device, rxChar} = makeMockDevice();
  installNavigatorBluetooth(device); installDomGlobals();
  const adapter = new WebBluetoothAdapter();
  let release;
  const gate = new Promise(resolve => { release = resolve; });
  const writes = [];
  rxChar.properties = {writeWithoutResponse:true};
  rxChar.writeValueWithoutResponse = async bytes => {
    writes.push(bytes[0]); if (bytes[0] === 1) await gate;
  };
  try {
    await adapter.connect();
    const first = adapter.send(Uint8Array.of(1));
    const queued = assert.rejects(adapter.send(Uint8Array.of(2)), /not connected/);
    await Promise.resolve();
    await adapter.disconnect(); await adapter.connect();
    await adapter.send(Uint8Array.of(3));
    release(); await first; await queued;
    assert.deepEqual(writes, [1, 3]);
  } finally { release(); adapter.dispose(); }
});

for (const stage of ['services', 'serial-service', 'notifications']) {
  test(`initial GATT drop during ${stage} retries the selected device without a competing reconnect`, async () => {
    const { device, gatt, server, txChar, listeners } = makeMockDevice();
    installNavigatorBluetooth(device);
    installDomGlobals();
    let picks = 0, connects = 0, failures = 0, schedules = 0;
    const statuses = [];
    navigator.bluetooth.requestDevice = async () => { picks++; return device; };
    const connect = gatt.connect;
    gatt.connect = async () => { connects++; return connect(); };
    const target = stage === 'notifications' ? txChar : server;
    const method = stage === 'services' ? 'getPrimaryServices' : stage === 'serial-service' ? 'getPrimaryService' : 'startNotifications';
    const original = target[method];
    target[method] = async (...args) => {
      if (failures++ === 0) {
        gatt.connected = false;
        listeners.gattserverdisconnected?.({target:device});
        throw new DOMException('GATT Server is disconnected. Cannot retrieve services.', 'NetworkError');
      }
      return original(...args);
    };
    const adapter = new WebBluetoothAdapter({setupRetryDelaysMs:[0, 0], onStatus: message => statuses.push(message)});
    adapter.scheduleReconnect = () => { schedules++; };
    try {
      await adapter.connect();
      assert.equal(adapter.isConnected(), true);
      assert.equal(connects, 2, 'setup did not reconnect GATT');
      assert.equal(picks, 1, 'retry reopened the picker');
      assert.equal(schedules, 0, 'drop spawned a competing background reconnect');
      assert.deepEqual(statuses, ['BLE reconnecting (2/3)']);
    } finally { adapter.dispose(); }
  });
}

for (const name of ['NetworkError', 'SecurityError']) {
  test(`initial ${name} has bounded retries and leaves no live connection`, async () => {
    const { device, gatt, server, listeners } = makeMockDevice();
    installNavigatorBluetooth(device);
    installDomGlobals();
    let connects = 0, schedules = 0;
    const connect = gatt.connect;
    gatt.connect = async () => { connects++; return connect(); };
    const failure = new DOMException('discovery failed', name);
    server.getPrimaryServices = async () => {
      gatt.connected = false;
      listeners.gattserverdisconnected?.({target:device});
      throw failure;
    };
    const adapter = new WebBluetoothAdapter({setupRetryDelaysMs:[0, 0]});
    adapter.scheduleReconnect = () => { schedules++; };
    try {
      await assert.rejects(adapter.connect(), error => error === failure);
      assert.equal(connects, name === 'NetworkError' ? 3 : 1);
      assert.equal(adapter.autoReconnect, false);
      assert.equal(schedules, 0);
      assert.equal(adapter.reconnectTimer, null);
      assert.equal(gatt.connected, false);
      assert.equal(adapter.server, null);
      assert.equal(listeners.gattserverdisconnected, undefined);
    } finally { adapter.dispose(); }
  });
}

test('disconnect during setup retry delay cannot resurrect the connection', async () => {
  const { device, gatt, server } = makeMockDevice();
  installNavigatorBluetooth(device);
  installDomGlobals();
  let connects = 0;
  const connect = gatt.connect;
  gatt.connect = async () => { connects++; return connect(); };
  server.getPrimaryServices = async () => { throw new DOMException('link dropped', 'NetworkError'); };
  const adapter = new WebBluetoothAdapter({setupRetryDelaysMs:[0, 0], onStatus: () => adapter.disconnect()});
  try {
    await assert.rejects(adapter.connect(), /GATT session cancelled/);
    assert.equal(connects, 1);
    assert.equal(gatt.connected, false);
    assert.equal(adapter.isConnected(), false);
    assert.equal(adapter.reconnectTimer, null);
  } finally { adapter.dispose(); }
});

test('real timeout wins, late settlement disconnects and clears state', async () => {
  let releaseConnect;
  const connectGate = new Promise(resolve => {
    releaseConnect = resolve;
  });
  const { device, gatt, listeners } = makeMockDevice({ connectGate });
  installNavigatorBluetooth(device);
  installDomGlobals();
  const adapter = new WebBluetoothAdapter({ connectTimeoutMs: 30 });
  adapter.device = device;

  // The REAL timeout wins while gatt.connect() is still pending.
  const sessionPromise = adapter.openGattSession();
  await assert.rejects(() => sessionPromise, /BLE connect timed out/);
  assert.equal(gatt.connected, false, 'link not connected before late settlement');

  // Release the deferred connect: the late settlement must disconnect + clear.
  releaseConnect();
  await new Promise(r => setTimeout(r, 30));
  assert.equal(gatt.connected, false, 'late settlement must disconnect the OS link');
  assert.equal(adapter.server, null, 'adapter state must be cleared');
  assert.equal(listeners.gattserverdisconnected, undefined, 'listeners must be removed');
  adapter.dispose();
});

test('replacement with different device: stale cleanup disconnects old only', async () => {
  let releaseA;
  const gateA = new Promise(resolve => {
    releaseA = resolve;
  });
  const deviceA = makeMockDevice({ connectGate: gateA }).device;
  const deviceB = makeMockDevice({}).device;
  let requestCalls = 0;
  Object.defineProperty(globalThis, 'navigator', {
    value: {
      bluetooth: {
        requestDevice: async () => (requestCalls++ === 0 ? deviceA : deviceB),
        getDevices: undefined,
      },
    },
    configurable: true,
    writable: true,
  });
  installDomGlobals();
  const adapter = new WebBluetoothAdapter({ connectTimeoutMs: 30 });

  // Attempt A via the PUBLIC path: requestDevice returns deviceA, times out.
  const connectA = adapter.connect();
  await assert.rejects(() => connectA, /BLE connect timed out/);

  // Attempt B via the PUBLIC path: requestDevice returns deviceB. B's
  // serialized bring-up awaits A's late cleanup; release A's gate so it runs.
  const connectB = adapter.connect();
  releaseA();
  await connectB;

  assert.equal(deviceA.gatt.connected, false, 'old device A must be disconnected');
  assert.equal(deviceB.gatt.connected, true, 'replacement device B must stay connected');
  adapter.handleDisconnected({target:deviceA});
  assert.equal(adapter.isConnected(), true, 'stale device event cleared the replacement session');
  adapter.dispose();
});

test('permanently-unresolved connect: retry fails with cleanup-pending error', async () => {
  const neverGate = new Promise(() => {});
  const deviceA = makeMockDevice({ connectGate: neverGate }).device;
  const deviceB = makeMockDevice({}).device;
  let requestCalls = 0;
  Object.defineProperty(globalThis, 'navigator', {
    value: {
      bluetooth: {
        requestDevice: async () => (requestCalls++ === 0 ? deviceA : deviceB),
        getDevices: undefined,
      },
    },
    configurable: true,
    writable: true,
  });
  installDomGlobals();
  const adapter = new WebBluetoothAdapter({ connectTimeoutMs: 30 });

  // Attempt A: gatt.connect() never settles, so A times out and its cleanup
  // stays pending forever.
  const connectA = adapter.connect();
  await assert.rejects(() => connectA, /BLE connect timed out/);

  // Attempt B: must fail with the cleanup-pending error, NOT hang.
  const connectB = adapter.connect();
  await assert.rejects(() => connectB, /BLE cleanup pending/);
  adapter.dispose();
});

test('normal successful connect leaves session live (no false cleanup)', async () => {
  const device = makeMockDevice({}).device;
  Object.defineProperty(globalThis, 'navigator', {
    value: {
      bluetooth: {
        requestDevice: async () => device,
        getDevices: undefined,
      },
    },
    configurable: true,
    writable: true,
  });
  installDomGlobals();
  const adapter = new WebBluetoothAdapter({ connectTimeoutMs: 30 });

  await adapter.connect();
  assert.equal(device.gatt.connected, true, 'session must be live after successful connect');
  assert.ok(adapter.server, 'server must be retained after successful connect');
  adapter.dispose();
});

test('hidden page invalidates an in-flight half-open discovery', async () => {
  let releaseDiscovery;
  let markDiscoveryStarted;
  const discoveryGate = new Promise(resolve => {
    releaseDiscovery = resolve;
  });
  const discoveryStarted = new Promise(resolve => {
    markDiscoveryStarted = resolve;
  });
  const { device, gatt, listeners } = makeMockDevice({
    discoveryGate,
    onDiscovery: markDiscoveryStarted,
  });
  installNavigatorBluetooth(device);
  const domListeners = installDomGlobals();
  const adapter = new WebBluetoothAdapter({});
  let reconnectSchedules = 0;
  const scheduleReconnect = adapter.scheduleReconnect.bind(adapter);
  adapter.scheduleReconnect = delayIndex => {
    reconnectSchedules += 1;
    return scheduleReconnect(delayIndex);
  };

  const connectPromise = adapter.connect();
  await discoveryStarted;
  assert.equal(gatt.connected, true, 'OS link must be up while discovery is pending');

  document.visibilityState = 'hidden';
  domListeners.visibilitychange();
  assert.equal(gatt.connected, false, 'hidden page must disconnect the half-open link');
  assert.equal(adapter.autoReconnect, false, 'hidden teardown must disarm auto-reconnect');
  assert.equal(listeners.gattserverdisconnected, undefined, 'disconnect listener must be removed');

  releaseDiscovery();
  await assert.rejects(() => connectPromise, /GATT session cancelled/);
  assert.equal(adapter.isConnected(), false, 'stale discovery must not establish a session');
  assert.equal(adapter.server, null, 'stale discovery must leave GATT state cleared');
  assert.equal(adapter.reconnectTimer, null, 'hidden teardown must leave no reconnect timer');
  assert.equal(reconnectSchedules, 0, 'hidden teardown must not schedule a reconnect');
  adapter.dispose();
});

test('hidden page preserves an already-connected healthy session', async () => {
  const { device, gatt } = makeMockDevice({});
  installNavigatorBluetooth(device);
  const domListeners = installDomGlobals();
  const adapter = new WebBluetoothAdapter({});

  await adapter.connect();
  document.visibilityState = 'hidden';
  domListeners.visibilitychange();

  assert.equal(gatt.connected, true, 'hidden page must preserve a healthy GATT link');
  assert.equal(adapter.isConnected(), true, 'healthy session must remain established');
  adapter.dispose();
});

test('discovery failure disconnects and clears state', async () => {
  const { device, gatt } = makeMockDevice({ discoveryError: new Error('service not found') });
  installNavigatorBluetooth(device);
  installDomGlobals();
  const adapter = new WebBluetoothAdapter({});

  adapter.device = device;
  await assert.rejects(() => adapter.openGattSession(), /service not found/);
  assert.equal(gatt.connected, false, 'discovery failure must disconnect the OS link');
  assert.equal(adapter.server, null, 'adapter state must be cleared');
  adapter.dispose();
});

test('startNotifications failure disconnects and clears state', async () => {
  const { device, gatt } = makeMockDevice({ notifyError: new Error('notify failed') });
  installNavigatorBluetooth(device);
  installDomGlobals();
  const adapter = new WebBluetoothAdapter({});

  adapter.device = device;
  await assert.rejects(() => adapter.openGattSession(), /notify failed/);
  assert.equal(gatt.connected, false, 'notify failure must disconnect the OS link');
  assert.equal(adapter.server, null, 'adapter state must be cleared');
  adapter.dispose();
});
