import test from 'node:test';
import { createRequire } from 'node:module';
import { createChatOutbound } from '../web/airbridge-chat-outbound.js';
import { routeReceiveControl } from '../web/airbridge-chat-receive.js';
import { ItemReceiver } from '../web/airbridge-item-receiver.js';
import { AirBridgeCryptoSession } from '../web/airbridge-protocol.js';
import { arbitrationCases, runArbitration } from '../web/protocol-arbitration-tests.js';

const vendor = createRequire(import.meta.url)('../web/vendor/js-sha256-0.11.1.js');
function node() {
  return {dataset:{}, setAttribute(){}, removeAttribute(){}, append(){}, appendChild(){}, querySelector(){return null;}};
}
function endpoint() {
  let outbound, receiver, count = 0;
  const log = [];
  return {
    createSession:(role, sendFn) => new AirBridgeCryptoSession({role, sendFn}),
    install(session, send) {
      receiver = new ItemReceiver({session, sendFn:send, hashImplementation:vendor.sha256});
      receiver.on('item', () => { count++; });
      receiver.on('error', error => log.push(`Receive failed: ${error.message}`));
      outbound = createChatOutbound({session, transport:{send, isConnected:()=>true},
        setBusy:value => receiver.setBusy(value), log:message => log.push(message)});
    },
    receive(frame) {
      const control = routeReceiveControl(frame, receiver);
      if (control) return control;
      if (outbound.receive(frame)) return;
      return receiver.onMessage(frame);
    },
    send:text => outbound.sendText(text), cancelReceive:() => receiver.cancel(),
    snapshot:() => receiver.snapshot(), logs:() => log.join('\n'), items:() => count,
    dispose() { outbound?.dispose(); receiver?.dispose(); },
  };
}
for (const mode of arbitrationCases) test(`Arbitration ${mode}`, async () => {
  const originalDocument = globalThis.document, originalHash = globalThis.sha256;
  const nodes = new Map();
  globalThis.document = {createElement:node, getElementById(id) {
    if (!nodes.has(id)) nodes.set(id, node());
    return nodes.get(id);
  }};
  globalThis.sha256 = vendor.sha256;
  try { console.info(JSON.stringify(await runArbitration(mode, [endpoint(), endpoint()]))); }
  finally { globalThis.document = originalDocument; globalThis.sha256 = originalHash; }
});
