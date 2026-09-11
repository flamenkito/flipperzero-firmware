import * as p from './airbridge-protocol.js';
import { pair } from './protocol-outbound-tests.js';
import { check, item, start, doneAcks } from './protocol-item-receiver-tests.js';

export async function runPageCancelRace(endpoint, qa) {
  // Given the actual page receiver and Cancel button with item 1 active.
  const sessions = await pair();
  const [sender, session] = endpoint === 'usb' ? [sessions[1], sessions[0]] : sessions;
  let release, entered;
  const held = new Promise(resolve => { release = resolve; });
  const ready = new Promise(resolve => { entered = resolve; });
  const sent = [];
  qa.install(session, async frame => {
    const msg = p.parseV2Frame(frame); sent.push(msg);
    if (msg.type === p.MSG.CANCEL) { entered(); await held; }
  });
  const old = await item(sender), next = await item(sender);
  const facade = {onMessage:qa.receive};
  try {
    await start(facade, old);
    check(!document.getElementById('cancelBtn').disabled, 'actual Cancel button disabled');
    // When cancellation waits for transport, item 2 negotiates before the old write resolves.
    document.getElementById('cancelBtn').click();
    await ready;
    await start(facade, next);
    const before = qa.snapshot(), beforePage = qa.pageState();
    release();
    await qa.pendingCancel;
    const after = qa.snapshot(), afterPage = qa.pageState();
    for (const frame of [...next.data, next.done]) await qa.receive(frame);
    const links = [...document.querySelectorAll('a[download]')];
    const result = {endpoint, before:before.itemId, after:after.itemId,
      beforeGeneration:before.generation, afterGeneration:after.generation, state:after.state,
      beforePage, afterPage, cards:links.length, doneAcks:doneAcks(sent),
      errors:sent.filter(m => m.type === p.MSG.ERROR).map(m => m.itemId)};
    console.info('R1 page cancellation', JSON.stringify(result));
    // Then the obsolete page continuation cannot reset receiver or UI ownership.
    check(before.itemId === next.id && after.itemId === next.id && after.generation === before.generation,
      `${endpoint}: old cancellation cleared item 2 (${JSON.stringify(result)})`);
    check(JSON.stringify(beforePage) === JSON.stringify(afterPage), `${endpoint}: old cancellation mutated replacement UI`);
    check(links.length === 1 && doneAcks(sent) === 1 && result.errors.length === 0, 'replacement failed to finalize');
    const bytes = new Uint8Array(await (await fetch(links[0].href)).arrayBuffer());
    check(bytes.length === 3 && bytes[0] === 1 && bytes[1] === 2 && bytes[2] === 3, 'replacement Blob contaminated');
    return result;
  } finally { release(); qa.dispose(); }
}

export function itemReceiverPageRaceTests() {
  return ['usb', 'ble'].map(endpoint => [`R1 ${endpoint} page cancellation cannot reset the next item`, async () => {
    const response = await fetch(`./chat-${endpoint}.html?receiver-race=${Date.now()}`, {cache:'no-store'});
    const source = await response.text();
    const setup = endpoint === 'usb' ? 'setupProtocol();' :
      'receiver = new ItemReceiver({session:cryptoSession,sendFn:frame=>transport.send(frame)}); wireProtocolEvents();';
    const receive = endpoint === 'usb' ? 'handleInboundFrame' : 'handleFrame';
    const hook = `
      globalThis.__receiverRace = {
        install(session, send) {
          receiver?.dispose(); outbound?.dispose(); outbound=null; cryptoSession=session;
          transport={send,isConnected:()=>true,getName:()=> 'Receiver race mock'};
          ${setup}
          const cancel=receiver.cancel.bind(receiver);
          receiver.cancel=()=> { const pending=cancel(); this.pendingCancel=pending; return pending; };
          updateCryptoPanel();
        },
        receive:${receive}, snapshot:()=>receiver.snapshot(),
        pageState:()=>({hello:inboundHelloItemId,meta:inboundMeta?.itemId??null,
          panel:$('panelState').textContent,throughput:$('throughput').textContent,cancelDisabled:$('cancelBtn').disabled}),
        dispose:()=>receiver?.dispose(),
        run:async()=> (await import('./protocol-item-receiver-page-race-tests.js')).runPageCancelRace('${endpoint}',globalThis.__receiverRace)
      };
    `;
    const iframe = document.createElement('iframe');
    iframe.hidden = true;
    const loaded = new Promise((resolve, reject) => { iframe.onload=resolve; iframe.onerror=reject; });
    iframe.srcdoc = source.replace('<head>', `<head><base href="${new URL('.', location.href)}">`)
      .replace('</script>\n</body>', `${hook}\n</script>\n</body>`);
    document.body.append(iframe);
    try { await loaded; return await iframe.contentWindow.__receiverRace.run(); }
    finally { iframe.contentWindow.__receiverRace?.dispose(); iframe.remove(); }
  }]);
}
