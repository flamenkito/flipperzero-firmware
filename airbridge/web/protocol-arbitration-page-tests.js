import { arbitrationCases, runArbitration } from './protocol-arbitration-tests.js';

async function pageEndpoint(endpoint) {
  const source = await (await fetch(`./chat-${endpoint}.html?arbitration=${Date.now()}`, {cache:'no-store'})).text();
  const setup = endpoint === 'usb' ? 'setupProtocol();' :
    'receiver=new ItemReceiver({session:cryptoSession,sendFn:frame=>transport.send(frame)});wireProtocolEvents();';
  const receive = endpoint === 'usb' ? 'handleInboundFrame' : 'handleFrame';
  const log = endpoint === 'usb' ? '(message,level)=>logger.log(message,level)' : 'log';
  const hook = `
    globalThis.__arbitration = {
      count:0,
      createSession:(role,sendFn)=>new AirBridgeCryptoSession({role,sendFn}),
      install(session,send) {
        receiver?.dispose();outbound?.dispose();cryptoSession=session;
        session.onStateChange=updateCryptoPanel;
        transport={send,isConnected:()=>true,getName:()=> 'Arbitration mock'};
        ${setup}
        receiver.on('item',()=>{this.count++;});
        outbound=createChatOutbound({session,transport,setBusy:setSending,log:${log}});
        updateCryptoPanel();
      },
      receive:${receive},send:text=>outbound.sendText(text),cancelReceive:()=>receiver.cancel(),
      snapshot:()=>receiver.snapshot(),logs:()=>$('log').textContent,items:()=>globalThis.__arbitration.count,
      dispose:()=>{outbound?.dispose();receiver?.dispose();}
    };`;
  const iframe = document.createElement('iframe'); iframe.hidden = true;
  const loaded = new Promise((resolve, reject) => { iframe.onload=resolve; iframe.onerror=reject; });
  iframe.srcdoc = source.replace('<head>', `<head><base href="${new URL('.', location.href)}">`)
    .replace('</script>\n</body>', `${hook}\n</script>\n</body>`);
  document.body.append(iframe);
  try {
    await loaded;
    if (!iframe.contentWindow.__arbitration) throw new Error('arbitration page hook failed to load');
    return {qa:iframe.contentWindow.__arbitration, remove:()=>iframe.remove()};
  } catch (error) { iframe.remove(); throw error; }
}

export function arbitrationPageTests() {
  return arbitrationCases.map(mode => [`Actual pages arbitration ${mode}`, async () => {
    const pages = [];
    try {
      for (const name of ['usb', 'ble']) pages.push(await pageEndpoint(name));
      return await runArbitration(mode, pages.map(page => page.qa));
    } finally { for (const page of pages) { page.qa.dispose(); page.remove(); } }
  }]);
}
