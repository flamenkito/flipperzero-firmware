import { hidIngressCases } from './protocol-hid-ingress-tests.js';

export function hidIngressPageTests() {
  return hidIngressCases.map(mode => [`USB page WebHID ingress ${mode}`, async () => {
    const source = await (await fetch(`./chat-usb.html?hid-ingress=${Date.now()}`, {cache:'no-store'})).text();
    const hook = `
      globalThis.__hidIngress = {
        admitted:0, failures:[],
        install(session,send) {
          receiver?.dispose();outbound?.dispose();outbound=null;cryptoSession=session;
          session.onStateChange=updateCryptoPanel;
          transport={send,isConnected:()=>true,getName:()=> 'HID ingress fixture'};
          setupProtocol();
          receiver.on('hello',()=>{this.admitted++;});
          receiver.on('error',error=>this.failures.push(error.message));
          updateCryptoPanel();
        },
        receive:handleInboundFrame,snapshot:()=>receiver.snapshot(),
        admissions:()=>globalThis.__hidIngress.admitted,errors:()=>globalThis.__hidIngress.failures,
        dispose:()=>receiver?.dispose(),
        run:async()=> (await import('./protocol-hid-ingress-tests.js')).runHidIngress(
          ${JSON.stringify(mode)},globalThis.__hidIngress)
      };`;
    const iframe = document.createElement('iframe'); iframe.hidden = true;
    const loaded = new Promise((resolve, reject) => { iframe.onload=resolve; iframe.onerror=reject; });
    iframe.srcdoc = source.replace('<head>', `<head><base href="${new URL('.', location.href)}">`)
      .replace('</script>\n</body>', `${hook}\n</script>\n</body>`);
    document.body.append(iframe);
    try { await loaded; return await iframe.contentWindow.__hidIngress.run(); }
    finally { iframe.contentWindow.__hidIngress?.dispose(); iframe.remove(); }
  }]);
}
