import { WebHIDAdapter } from './airbridge-transports.js';
import { ItemReceiver, MSG, buildV2Frame, parseMessage } from './airbridge-protocol.js';
import { pair } from './protocol-outbound-tests.js';

const check = (value, message) => { if (!value) throw new Error(message); };
export const hidIngressCases = [
  'zero padding', 'offset ownership', 'nonzero padding', 'short header',
  'short unpadded', 'short padded', 'excess length', 'truncated payload',
  'wrong report ID', 'active malformed padding',
];

function receiverFixture() {
  let receiver, admitted = 0;
  const errors = [];
  return {
    install(session, sendFn) {
      receiver = new ItemReceiver({session, sendFn});
      receiver.on('hello', () => { admitted++; });
      receiver.on('error', error => errors.push(error.message));
    },
    receive:frame => receiver.onMessage(frame), snapshot:() => receiver.snapshot(),
    admissions:() => admitted, errors:() => errors,
    dispose:() => receiver?.dispose(),
  };
}

export async function runHidIngress(mode, fixture = receiverFixture()) {
  const sessions = await pair(), sent = [];
  const adapter = new WebHIDAdapter();
  let pending, delivered, dispatches = 0;
  fixture.install(sessions[0], async frame => sent.push(parseMessage(frame)));
  const unsubscribe = adapter.onReceive(frame => {
    delivered = frame; dispatches++;
    pending = Promise.resolve(fixture.receive(frame));
  });
  const report = (length = 64, id = 1) => {
    const parent = new Uint8Array(16 + length + 16).fill(0xa5);
    const view = new Uint8Array(parent.buffer, 16, length); view.fill(0);
    view.set(buildV2Frame(MSG.HELLO, 0, id).subarray(0, length));
    return {parent, view, data:new DataView(parent.buffer, 16, length)};
  };
  const dispatch = async (packet, reportId = 0) => {
    pending = null; delivered = null;
    adapter.handleInputReport({reportId, data:packet.data});
    packet.parent.fill(0xee);
    await pending;
  };
  const capabilityAcks = () => sent.filter(m => m.type === MSG.ACK && m.payload.length === 15 && m.payload[0] === MSG.HELLO).length;
  try {
    if (mode === 'active malformed padding') {
      await dispatch(report());
      check(fixture.snapshot().itemId === 1 && capabilityAcks() === 1, 'initial valid report failed');
    }
    const paddingOffsets = mode === 'nonzero padding' ? Array.from({length:51}, (_, i) => i + 13) : [63];
    for (const offset of paddingOffsets) {
      const lengths = {'short header':4, 'short unpadded':13, 'short padded':63, 'excess length':65};
      const packet = report(lengths[mode] ?? 64, mode === 'active malformed padding' ? 2 : 1);
      if (mode.includes('padding') && !['zero padding'].includes(mode)) packet.view[offset] = 127;
      if (mode === 'truncated payload') packet.view[4] = 60;
      const beforeAcks = capabilityAcks(), beforeAdmitted = fixture.admissions();
      await dispatch(packet, mode === 'wrong report ID' ? 1 : 0);
      const valid = mode === 'zero padding' || mode === 'offset ownership';
      if (valid) {
        check(fixture.snapshot().itemId === 1 && capabilityAcks() === 1, 'valid offset64-byte HELLO was rejected');
        check(delivered instanceof Uint8Array && delivered[0] === MSG.HELLO && delivered[12] === 83,
          'offset bytes changed after backing-buffer mutation');
        check(delivered.buffer !== packet.parent.buffer && delivered.buffer.byteLength <= 64,
          'adapter retained caller backing storage');
      } else {
        check(capabilityAcks() === beforeAcks && fixture.admissions() === beforeAdmitted,
          `${mode} offset=${offset}: malformed HID admitted item or emitted capability ACK`);
        check(fixture.snapshot().itemId === null, `${mode}: malformed frame retained active item`);
        if (Object.hasOwn(lengths, mode)) check(dispatches === 0, `${mode}: non64-byte report reached protocol callback`);
        if (mode === 'nonzero padding' || mode === 'active malformed padding') {
          check(fixture.errors().at(-1) === 'nonzero frame padding', 'lost strict terminal padding error');
        }
      }
    }
    return {mode, dispatches, admissions:fixture.admissions(), capabilityAcks:capabilityAcks(),
      state:fixture.snapshot().state, errors:fixture.errors()};
  } finally { unsubscribe(); fixture.dispose(); sessions.forEach(session => session.clearKeys()); }
}

export function hidIngressTests() {
  return [
    ...hidIngressCases.map(mode => [`WebHID ingress ${mode}`, () => runHidIngress(mode)]),
    ['WebHID outgoing report ID0 and64-byte ownership unchanged', async () => {
      const adapter = new WebHIDAdapter();
      let written, id;
      adapter.device = {opened:true, sendReport:async (reportId, report) => { id=reportId; written=report; }};
      const raw = buildV2Frame(MSG.HELLO, 0, 1);
      await adapter.send(raw); raw.fill(0xee);
      check(id === 0 && written.length === 64 && written[0] === MSG.HELLO && written[12] === 83,
        'outgoing report identity/size/ownership changed');
      check(written.subarray(13).every(byte => byte === 0), 'outgoing padding is not zero');
    }],
  ];
}
