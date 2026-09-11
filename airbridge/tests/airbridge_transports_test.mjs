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
} = {}) {
  const listeners = {};
  let connectCalls = 0;
  const txChar = {
    uuid: TX_UUID,
    startNotifications: async () => {
      if (notifyError) throw notifyError;
    },
    addEventListener: () => {},
    removeEventListener: () => {},
  };
  const rxChar = { uuid: RX_UUID };
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
  return { device, gatt, server, txChar, listeners };
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
