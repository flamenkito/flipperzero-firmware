import * as p from './airbridge-protocol.js';
import { pair } from './protocol-outbound-tests.js';
import { check } from './protocol-item-receiver-tests.js';

const message = 'Unsupported protocol version';
const text = new TextEncoder();
function exactFailure(run) {
  let error;
  try { run(); } catch (caught) { error = caught; }
  check(error?.message === message, `expected exact incompatibility, got ${error?.message}`);
}

export function incompatibilityTests(vendor) {
  const tests = [];
  for (const fault of ['plain ACK', 'empty ACK', 'missing AB2S', 'malformed AB2S', 'unsupported streaming']) {
    tests.push([`No compatibility: ${fault} stops sender before META`, async () => {
      const [session] = await pair();
      const stream = session.openEncryptedStream({kind:'text', name:'', mimeType:'',
        payloadSize:0n, payloadSha256:await p.sha256(new Uint8Array())}, new Blob([]));
      const sent = [];
      const sender = new p.ItemSender(async frame => {
        const hello = p.parseV2Frame(frame); sent.push(hello.type);
        let payload = p.makeV2AckPayload(hello), type = p.MSG.ACK;
        switch (fault) {
          case 'plain ACK': payload = p.makeAckPayload(p.MSG.HELLO, 0); break;
          case 'empty ACK': payload = new Uint8Array(); break;
          case 'missing AB2S': payload = payload.slice(0, 7); break;
          case 'malformed AB2S': payload[3] = 0; break;
          case 'unsupported streaming': type = p.MSG.ERROR; payload = text.encode(message); break;
        }
        sender.receiveStreamControl(p.buildMessage(type, 0, payload));
      }, {timeoutMs:50, retries:0});
      let error;
      try { await sender.sendEncryptedStream(stream); } catch (caught) { error = caught; }
      check(error?.message === message, `wrong sender failure: ${error?.message}`);
      check(sent.length === 1 && sent[0] === p.MSG.HELLO, 'META/DATA escaped capability boundary');
      session.clearKeys();
    }]);
  }
  for (const fault of ['v1 HELLO', 'missing HELLO magic', 'plaintext META']) {
    tests.push([`No compatibility: receiver rejects ${fault}`, async () => {
      const [sender, session] = await pair();
      const sent = [], errors = [], items = [];
      const receiver = new p.ItemReceiver({session, hashImplementation:vendor,
        sendFn:async frame => sent.push(frame)});
      receiver.on('error', error => errors.push(error.message));
      receiver.on('item', item => items.push(item));
      try {
        if (fault === 'plaintext META') {
          await receiver.onMessage(p.buildV2Frame(p.MSG.HELLO, 0, 1));
          const fragments = p.encodeMeta({kind:'text', name:'legacy', size:0, chunks:0, hash:'sha256:'+'00'.repeat(32)});
          for (let seq = 0; seq < fragments.length; seq++) await receiver.onMessage(p.buildMessage(p.MSG.ITEM_META, seq, fragments[seq]));
        } else {
          const payload = new Uint8Array(fault === 'v1 HELLO' ? 4 : 8); payload[3] = 1;
          await receiver.onMessage(p.buildMessage(p.MSG.HELLO, 0, payload));
        }
        check(errors.length === 1 && errors[0] === message, `wrong receiver failure: ${errors}`);
        const last = p.parseMessage(sent.at(-1));
        const reason = fault === 'plaintext META' ? p.parseV2Frame(sent.at(-1)).body : last.payload;
        check(last.type === p.MSG.ERROR && new TextDecoder().decode(reason) === message, 'missing exact peer ERROR');
        check(!session.isUnlocked() && items.length === 0 && receiver.snapshot().plaintextBytes === 0n, 'incompatible session retained');
      } finally { receiver.dispose(); sender.clearKeys(); session.clearKeys(); }
    }]);
  }
  tests.push(['No compatibility: strict corruption errors are not relabeled', () => {
    const wait = new p.V2StopAndWait();
    wait.begin(p.buildV2Frame(p.MSG.HELLO, 0, 1));
    const ack = p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload({type:p.MSG.HELLO, seq:0, itemId:1, segment:0xffffffff}));
    const padded = new Uint8Array(64); padded.set(ack); padded[63] = 1;
    let error; try { wait.receive(padded); } catch (caught) { error = caught; }
    check(error?.message === 'nonzero frame padding', 'corruption became compatibility');
    exactFailure(() => p.validateV2Meta({kind:'text'}, {}));
    const meta = p.makeV2Meta({keyId:'AAAAAAAAAAAAAAAAAAAAAA', direction:'usb-to-ble', itemId:1, headerLen:50, payloadSize:0n});
    try { p.validateV2Meta({...meta, extra:1}, meta); } catch (caught) { error = caught; }
    check(error?.message === 'unsupported META fields', 'current v2 schema weakened');
  }]);
  return tests;
}
