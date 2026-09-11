import * as p from './airbridge-protocol.js';
import { pair } from './protocol-outbound-tests.js';
import { check } from './protocol-item-receiver-tests.js';

export async function runIncompatiblePage(endpoint, fault, qa) {
  const sessions = await pair();
  const session = sessions[endpoint === 'usb' ? 0 : 1];
  const sent = [];
  const encoder = new TextEncoder();
  qa.install(session, async frame => {
    sent.push(p.parseMessage(frame));
    if (fault.startsWith('outbound') && frame[0] === p.MSG.HELLO) {
      const hello = p.parseV2Frame(frame);
      let reply;
      if (fault === 'outbound ERROR') reply = p.buildMessage(p.MSG.ERROR, 0, encoder.encode('Unsupported protocol version'));
      else {
        let payload = p.makeV2AckPayload(hello);
        if (fault === 'outbound plain ACK') payload = payload.slice(0, 3);
        else payload[3] = 0;
        reply = p.buildMessage(p.MSG.ACK, 0, payload);
      }
      await qa.receive(reply);
    }
  });
  try {
    if (fault.startsWith('outbound')) await qa.send();
    else if (fault === 'plaintext META') {
      await qa.receive(p.buildV2Frame(p.MSG.HELLO, 0, 1));
      const parts = p.encodeMeta({kind:'attachment', name:'old.txt', size:0});
      for (let seq = 0; seq < parts.length; seq++) await qa.receive(p.buildMessage(p.MSG.ITEM_META, seq, parts[seq]));
    } else {
      if (fault === 'pre-unlock') session.clearKeys(p.CRYPTO_STATE.ABORTED);
      await qa.receive(p.buildMessage(p.MSG.HELLO, 0, new Uint8Array([0, 0, 0, 1])));
    }
    const log = document.getElementById('log').textContent;
    check(log.includes('Unsupported protocol version'), `${endpoint}/${fault}: missing actionable log: ${log}`);
    check(!session.isUnlocked(), 'incompatibility did not lock session');
    check(document.querySelectorAll('a[download]').length === 0, 'incompatible download');
    check(!sent.some(m => [p.MSG.ITEM_META, p.MSG.ITEM_DATA].includes(m.type)), 'sent META/DATA');
    check(document.getElementById('sendTextBtn').disabled, 'incompatible session can send');
    return {endpoint, fault, reason:'Unsupported protocol version', locked:true, sent:sent.map(m => m.type)};
  } finally { qa.dispose(); sessions.forEach(s => s.clearKeys()); }
}

export function incompatibilityPageTests() {
  return ['usb', 'ble'].flatMap(endpoint =>
    ['v1 HELLO', 'plaintext META', 'pre-unlock', 'outbound plain ACK', 'outbound malformed AB2S', 'outbound ERROR'].map(fault =>
      [`No compatibility: ${endpoint} page ${fault}`, async () => {
        const source = await (await fetch(`./chat-${endpoint}.html?no-compat=${Date.now()}`, {cache:'no-store'})).text();
        const setup = endpoint === 'usb' ? 'setupProtocol();' :
          'receiver=new ItemReceiver({session:cryptoSession,sendFn:frame=>transport.send(frame)});wireProtocolEvents();';
        const receive = endpoint === 'usb' ? 'handleInboundFrame' : 'handleFrame';
        const log = endpoint === 'usb' ? '(message,level)=>logger.log(message,level)' : 'log';
        const hook = `
          globalThis.__incompatibility = {
            install(session,send) {
              receiver?.dispose();outbound?.dispose();outbound=null;cryptoSession=session;
              session.onStateChange=updateCryptoPanel;
              transport={send,isConnected:()=>true,getName:()=> 'Incompatibility mock'};
              ${setup}
              outbound=createChatOutbound({session,transport,setBusy:()=>updateControls(),log:${log}});
              updateCryptoPanel();
            },
            receive:${receive},send:()=>outbound.sendText('no compatibility'),
            dispose:()=>{outbound?.dispose();receiver?.dispose();},
            run:async()=> (await import('./protocol-incompatibility-page-tests.js')).runIncompatiblePage(
              ${JSON.stringify(endpoint)},${JSON.stringify(fault)},globalThis.__incompatibility)
          };`;
        const iframe = document.createElement('iframe'); iframe.hidden = true;
        const loaded = new Promise((resolve, reject) => { iframe.onload=resolve; iframe.onerror=reject; });
        iframe.srcdoc = source.replace('<head>', `<head><base href="${new URL('.', location.href)}">`)
          .replace('</script>\n</body>', `${hook}\n</script>\n</body>`);
        document.body.append(iframe);
        try { await loaded; return await iframe.contentWindow.__incompatibility.run(); }
        finally { iframe.contentWindow.__incompatibility?.dispose(); iframe.remove(); }
      }]));
}
