function check(value, message) { if (!value) throw new Error(message); }

export async function loadChatTestPage(endpoint, room) {
  const source=await (await fetch(`./chat-${endpoint}.html?qa=${Date.now()}`, {cache:'no-store'})).text();
  const base=new URL('.', location.href);
  const setup=endpoint==='usb'?'setupProtocol();':'receiver=new ItemReceiver({session:cryptoSession,sendFn:frame=>transport.send(frame)});wireProtocolEvents();';
  const hook=`
    globalThis.chatTest = {
      connect, disconnect,
      ready:()=>cryptoSession?.getStatus(),
      receiver:()=>receiver, outbound:()=>outbound,
      send:()=>sendSelectedFile(), text:()=>sendText(),
      transport:()=>transport,
      session:()=>cryptoSession,
      receive:${endpoint==='usb'?'handleInboundFrame':'handleFrame'},
      install(session,send) {
        receiver?.dispose();outbound?.dispose();outbound=null;cryptoSession=session;
        transport={send,isConnected:()=>true,getName:()=> 'Cleanup mock',disconnect:async()=>{}};
        ${setup} updateCryptoPanel();
      },
      runCleanup:async action=>(await import('./protocol-chat-cleanup-tests.js')).runChatCleanup('${endpoint}',action,chatTest),
      done:()=>!isSending && inboundHelloItemId==null,
      dispose:()=>{receiver?.dispose();outbound?.dispose();transport?.disconnect();}
    };
    const create=URL.createObjectURL.bind(URL), revoke=URL.revokeObjectURL.bind(URL);
    chatTest.urls={created:[],revoked:[]};
    URL.createObjectURL=blob=>{const url=create(blob);chatTest.urls.created.push(url);return url;};
    URL.revokeObjectURL=url=>{chatTest.urls.revoked.push(url);return revoke(url);};
  `;
  const iframe=document.createElement('iframe'); iframe.hidden=true;
  const loaded=new Promise((resolve,reject)=>{iframe.onload=resolve;iframe.onerror=reject;});
  iframe.srcdoc=source.replaceAll('location.search', JSON.stringify(`?mock=1&mockPeer=${room}`))
    .replace('<head>', `<head><base href="${base}">`)
    .replace('</script>\n</body>', `${hook}\n</script>\n</body>`);
  document.body.append(iframe); await loaded;
  return {iframe, win:iframe.contentWindow, qa:iframe.contentWindow.chatTest};
}

export function waitChat(win, predicate) {
  return new Promise((resolve,reject)=> {
    const checkState=()=>{if(predicate()){observer.disconnect();clearTimeout(timer);resolve();}};
    const observer=new win.MutationObserver(checkState);
    const timer=setTimeout(()=>{observer.disconnect();reject(new Error('chat state timeout: '+win.document.getElementById('log').textContent));},15000);
    observer.observe(win.document,{subtree:true,childList:true,attributes:true,characterData:true}); checkState();
  });
}

export function chatPageTests() {
  return [['Chat paired pages negotiate matching SAS without an internal third peer', async()=> {
    // Given both actual pages on one isolated mock channel.
    const room=`paired-${Date.now()}`, pages=[];
    try {
      for(const endpoint of ['usb','ble']) pages.push(await loadChatTestPage(endpoint,room));
      // When their real connection and SAS controls are used.
      for(const {win} of pages) win.document.getElementById('connectBtn').click();
      await Promise.all(pages.map(({win})=>waitChat(win,()=>!win.document.getElementById('acceptSasBtn').disabled)));
      const codes=pages.map(({win})=>win.document.getElementById('sasCode').textContent);
      check(codes[0]===codes[1] && /^\d{6}$/.test(codes[0]), `conflicting SAS: ${codes}`);
      for(const {win} of pages) win.document.getElementById('acceptSasBtn').click();
      await Promise.all(pages.map(({win,qa})=>waitChat(win,()=>qa.ready()?.state==='unlocked')));
      // Then both have one unlocked peer, and bidirectional text reaches the other transcript.
      for(let i=0;i<2;i++) {
        const {win,qa}=pages[i], peer=pages[1-i];
        const input=win.document.getElementById('textInput'); input.value=`paired text ${i}`;
        input.dispatchEvent(new win.Event('input')); win.document.getElementById('sendTextBtn').click();
        await waitChat(peer.win,()=>peer.win.document.getElementById('transcript').textContent.includes(`paired text ${i}`));
        await waitChat(win,()=>qa.done());
      }
      const files=[];
      for(let i=0;i<2;i++) for(const size of [0,21,4194369]) {
        const sender=pages[i], peer=pages[1-i];
        const before=peer.qa.urls.created.length;
        const bytes=Uint8Array.from({length:size},(_,j)=>j%251);
        const transfer=new sender.win.DataTransfer();
        transfer.items.add(new sender.win.File([bytes],`payload-${size}.bin`,{type:'application/octet-stream'}));
        const input=sender.win.document.getElementById('fileInput');input.files=transfer.files;
        input.dispatchEvent(new sender.win.Event('change'));
        sender.win.document.getElementById('sendFileBtn').click();
        check(peer.qa.urls.created.length===before,'URL allocated before verification');
        await waitChat(peer.win,()=>peer.qa.urls.created.length===before+1);
        await waitChat(sender.win,()=>sender.qa.done());
        const link=[...peer.win.document.querySelectorAll('.peer a[download]')].at(-1);
        const actual=new Uint8Array(await (await fetch(link.href)).arrayBuffer());
        check(actual.length===size && actual.every((v,j)=>v===bytes[j]),'download bytes differ');
        check(link.download===`payload-${size}.bin`,'download name differs');
        check(peer.win.document.getElementById('throughput').dataset.bytes===String(size),'receive counts transport overhead');
        files.push({direction:i,size,bytes:actual.length});
      }
      for(const {win,qa} of pages) {
        win.document.getElementById('clearTranscriptBtn').click();
        check(qa.urls.created.length===qa.urls.revoked.length && new Set(qa.urls.revoked).size===qa.urls.revoked.length,'clear URL receipt differs');
        check(!win.document.querySelector('a[download]'),'clear left download');
      }
      return {sas:codes[0],textDirections:2,files,urls:pages.map(p=>({created:p.qa.urls.created.length,revoked:p.qa.urls.revoked.length}))};
    } finally {for(const {qa,iframe} of pages){qa?.dispose();iframe.remove();}}
  }]];
}
