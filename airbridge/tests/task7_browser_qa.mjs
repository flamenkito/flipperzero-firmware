async (page) => {
  const root='/Users/asutov/projects/flipperzero-firmware/.omo/evidence/no-limit-transfer';
  const pages=[], screenshots=[], results=[], errors=[];
  const browserCdp=await page.context().browser().newBrowserCDPSession();
  const downloadDirectory='/var/folders/0l/fqf4wmtj1_7bf51mvzylwlvw0000gn/T/opencode';
  const room=`qa-${Date.now()}`;
  const check=(value,message)=>{if(!value) throw new Error(message);};
  const wait=(p,fn,arg)=>p.waitForFunction(fn,arg,{polling:50});
  async function capture(p,role,state) {
    await p.bringToFront();
    for(const width of [375,768,1280]) {
      await p.setViewportSize({width,height:900});
      const layout=await p.evaluate(()=>({width:document.documentElement.clientWidth,scroll:document.documentElement.scrollWidth,
        phase:document.getElementById('panelState').dataset.phase,
        clipped:[...document.querySelectorAll('button,input,.attachment-name,.progress-label,.panel-state')].filter(el=>{
          const r=el.getBoundingClientRect();return r.width>0&&(r.left<0||r.right>document.documentElement.clientWidth+1);
        }).map(el=>el.id||el.className)}));
      check(layout.width===layout.scroll && !layout.clipped.length,`${role} ${state} ${width}: ${JSON.stringify(layout)}`);
      const path=`${root}/task-7-${role}-${state}-${width}.png`;
      await p.screenshot({path,fullPage:true});screenshots.push({role,state,width,path,...layout});
    }
  }
  async function ready(p) { await p.bringToFront(); await wait(p,()=>document.getElementById('panelState').dataset.phase==='Ready'&&!document.getElementById('fileInput').disabled); }
  async function select(p,size,name=`payload-${size}.bin`) {
    await p.bringToFront();
    await p.evaluate(({name,size})=>{
      const transfer=new DataTransfer();transfer.items.add(new File([Uint8Array.from({length:size},(_,i)=>i%251)],name,{type:'application/octet-stream'}));
      const input=document.getElementById('fileInput');input.files=transfer.files;input.dispatchEvent(new Event('change'));
    },{name,size});
  }
  async function sendFile(p,other,size) {
    const count=await other.locator('.peer a[download]').count();
    await select(p,size);await p.locator('#sendFileBtn').click();
    await wait(other,n=>document.querySelectorAll('.peer a[download]').length===n+1,count);
    await ready(p);
    const receipt=await other.evaluate(async size=>{
      const link=[...document.querySelectorAll('.peer a[download]')].at(-1),card=link.closest('.attachment-card');
      const response=await fetch(link.href),bytes=new Uint8Array(await response.arrayBuffer());
      const digest=[...new Uint8Array(await crypto.subtle.digest('SHA-256',bytes))].map(b=>b.toString(16).padStart(2,'0')).join('');
      return {size,actual:bytes.length,equal:bytes.every((b,i)=>b===i%251),hash:digest,cardHash:card.dataset.sha256,
        name:link.download,mime:response.headers.get('content-type'),phase:card.dataset.phase,verification:card.dataset.verification};
    },size);
    check(receipt.actual===size&&receipt.equal&&receipt.hash===receipt.cardHash,'download receipt mismatch');
    results.push(receipt);
  }
  try {
    for(const role of ['usb','ble']) {
      const p=await page.context().newPage();pages.push(p);p.on('pageerror',e=>errors.push({role,error:e.message}));
      await p.setViewportSize({width:1280,height:900});
      await p.route('**/*',async route=>{
        const response=await route.fetch({headers:{'Cache-Control':'no-cache'}});
        if(route.request().url().includes(`chat-${role}.html`)) {
          const hook=`globalThis.task7={session:()=>cryptoSession,transport:()=>transport,receiver:()=>receiver};`;
          const body=(await response.text()).replace('</script>\n</body>',`${hook}\n</script>\n</body>`);
          await route.fulfill({response,body});
        } else await route.fulfill({response});
      });
      await p.goto(`http://127.0.0.1:8081/chat-${role}.html?mock=1&mockPeer=${room}&v=${Date.now()}`);
      await p.evaluate(()=>{
        const q=task7;q.created=[];q.revoked=[];q.phases=[];q.progress=[];q.gate=null;q.held=false;
        const create=URL.createObjectURL.bind(URL),revoke=URL.revokeObjectURL.bind(URL);
        URL.createObjectURL=blob=>{const url=create(blob);q.created.push(url);return url;};
        URL.revokeObjectURL=url=>{q.revoked.push(url);revoke(url);};
        File.prototype.arrayBuffer=()=>{throw new Error('forbidden File.arrayBuffer');};
        window.FileReader=class {constructor(){throw new Error('forbidden FileReader');}};
        q.wait=async mode=>{if(q.gate!==mode)return;q.gate=null;q.held=true;await new Promise(resolve=>{q.release=resolve;});q.held=false;};
        const stream=File.prototype.stream;
        File.prototype.stream=function(){const source=stream.call(this),get=source.getReader.bind(source);
          source.getReader=options=>{const reader=get(options),read=reader.read.bind(reader);reader.read=async(...args)=>{await q.wait('hash');return read(...args);};return reader;};return source;};
        const observer=new MutationObserver(()=>{
          const phase=document.getElementById('panelState').dataset.phase;
          const bytes=document.getElementById('throughput').dataset.bytes||'0';
          if(q.phases.at(-1)!==phase)q.phases.push(phase);
          const last=q.progress.at(-1);
          if(!last||last.phase!==phase)q.progress.push({phase,first:bytes,last:bytes,reports:1,regressed:false});
          else {last.regressed ||= BigInt(bytes)<BigInt(last.last);last.last=bytes;last.reports++;}
        });observer.observe(document.getElementById('panelState'),{subtree:true,childList:true,attributes:true});
        q.install=()=>{
          const transport=q.transport(),send=transport.send.bind(transport);let frames=0;
          transport.send=async frame=>{
            if(frame[0]===3){frames++;if(q.gate==='data'&&frames>=1300)await q.wait('data');
              if(q.tamper){q.tamper=false;frame=new Uint8Array(frame);frame[13]^=1;}}
            return send(frame);
          };
          const receiver=q.receiver(),receive=receiver.onMessage.bind(receiver);
          receiver.onMessage=async frame=>{if(frame[0]===6)await q.wait('done');return receive(frame);};
          const session=q.session(),open=session.openEncryptedStream.bind(session);
          session.openEncryptedStream=(header,source)=>{if(q.badHash){q.badHash=false;header={...header,payloadSha256:'0'.repeat(64)};}return open(header,source);};
        };
      });
    }
    for(const p of pages){await p.bringToFront();await p.locator('#connectBtn').click();}
    for(const p of pages){await p.bringToFront();await wait(p,()=>!document.getElementById('acceptSasBtn').disabled);}
    const sas=await Promise.all(pages.map(p=>p.locator('#sasCode').textContent()));check(sas[0]===sas[1],'SAS mismatch');
    for(let i=0;i<2;i++)await capture(pages[i],i?'ble':'usb','SAS');
    for(const p of pages){await p.bringToFront();await p.locator('#acceptSasBtn').click();}
    for(const p of pages){await p.bringToFront();await wait(p,()=>!document.getElementById('textInput').disabled);await p.evaluate(()=>task7.install());}
    for(let i=0;i<2;i++) {
      const p=pages[i],other=pages[1-i],role=i?'ble':'usb',peer=i?'usb':'ble';
      await p.bringToFront();
      await p.locator('#textInput').fill(`encrypted text ${role}`);await p.locator('#textInput').press('Enter');
      await wait(other,text=>document.getElementById('transcript').textContent.includes(text),`encrypted text ${role}`);await ready(p);
      await sendFile(p,other,0);await sendFile(p,other,21);
      const before=await other.evaluate(()=>task7.created.length);
      await p.evaluate(()=>{task7.gate='hash';});await select(p,4194369);await p.locator('#sendFileBtn').click();
      await wait(p,()=>task7.held);await capture(p,role,'Hashing');
      await p.evaluate(()=>{task7.gate='data';task7.release();});await wait(p,()=>task7.held);
      await capture(p,role,'Sending');await capture(other,peer,'Receiving');
      check(await other.evaluate(()=>task7.created.length)===before,'premature Blob URL');
      await other.evaluate(()=>{task7.gate='done';});await p.evaluate(()=>task7.release());
      await wait(other,()=>task7.held);
      await capture(other,peer,'Verifying');check(await other.evaluate(()=>task7.created.length)===before,'URL before verification');
      await other.evaluate(()=>task7.release());await ready(p);
      await capture(p,role,'Ready');
      const large=await other.evaluate(async()=>{const link=[...document.querySelectorAll('.peer a[download]')].at(-1);const bytes=new Uint8Array(await(await fetch(link.href)).arrayBuffer());return {size:bytes.length,equal:bytes.every((b,i)=>b===i%251),hash:link.closest('.attachment-card').dataset.sha256};});
      check(large.size===4194369&&large.equal,'large download differs');results.push({role,large});
      await browserCdp.send('Browser.setDownloadBehavior',{behavior:'allowAndName',downloadPath:downloadDirectory,eventsEnabled:true});
      const started=new Promise(resolve=>browserCdp.once('Browser.downloadWillBegin',resolve));
      const completed=new Promise(resolve=>{
        const progress=event=>{if(event.state==='completed'||event.state==='canceled'){browserCdp.off('Browser.downloadProgress',progress);resolve(event);}};
        browserCdp.on('Browser.downloadProgress',progress);
      });
      await other.bringToFront();await other.locator('.peer a[download]').last().click();
      const downloaded=await started,finished=await completed;
      check(downloaded.suggestedFilename==='payload-4194369.bin'&&finished.state==='completed'&&finished.receivedBytes===4194369,'actual download differs');
      results.push({download:{...downloaded,...finished,path:`${downloadDirectory}/${downloaded.guid}`}});
      for(const fault of ['cancel','tamper','hash']) {
        const beforeUrls=await Promise.all(pages.map(tab=>tab.evaluate(()=>task7.created.length)));
        if(fault==='cancel')await p.evaluate(()=>{task7.gate='data';});
        else await p.evaluate(f=>{task7[f==='tamper'?'tamper':'badHash']=true;},fault);
        await select(p,70000);await p.locator('#sendFileBtn').click();
        if(fault==='cancel') {await wait(p,()=>task7.held);await p.locator('#cancelBtn').click();await p.evaluate(()=>task7.release());}
        await wait(p,()=>['Failed','Cancelled'].includes(document.getElementById('panelState').dataset.phase)&&!document.getElementById('fileInput').disabled);
        await wait(other,()=>['Failed','Cancelled'].includes(document.getElementById('panelState').dataset.phase)&&!document.getElementById('fileInput').disabled);
        const terminal=await p.locator('#panelState').getAttribute('data-phase');
        check(terminal===(fault==='cancel'?'Cancelled':'Failed'),`${fault} terminal ${terminal}`);
        for(let j=0;j<2;j++)check(await pages[j].evaluate(()=>task7.created.length)===beforeUrls[j],`${fault} created URL`);
        if(fault!=='hash')await capture(p,role,terminal);
        results.push({role,fault,terminal,beforeUrls});
        await sendFile(p,other,7);
      }
    }
    const receipts=[];
    for(let i=0;i<2;i++) {
      const p=pages[i];await p.bringToFront();await p.locator('[data-remove-attachment]').first().click();
      await p.locator('#clearTranscriptBtn').click();await p.locator('#clearTranscriptBtn').click();
      receipts.push(await p.evaluate(()=>({created:task7.created.length,revoked:task7.revoked.length,unique:new Set(task7.revoked).size,phases:task7.phases,progress:task7.progress})));
    }
    await sendFile(pages[0],pages[1],9);
    await pages[0].locator('#disconnectBtn').click();
    for(let i=0;i<2;i++){
      const p=pages[i];await wait(p,()=>!document.getElementById('connectBtn').disabled);
      await capture(p,i?'ble':'usb','Disconnected');
      const urls=await p.evaluate(()=>({created:task7.created.length,revoked:task7.revoked.length,unique:new Set(task7.revoked).size,cards:document.querySelectorAll('a[download]').length}));
      check(urls.created===urls.revoked&&urls.unique===urls.revoked&&urls.cards===0,'disconnect URL cleanup failed');results.push({endpoint:i,urls});
    }
    check(!errors.length,JSON.stringify(errors));
    await page.evaluate(result=>{window.task7BrowserResults=result;},{sas,results,receipts,screenshots,errors});
    return {sas,results,screenshotCount:screenshots.length,receipts:receipts.map(r=>({...r,progress:r.progress.filter(p=>p.regressed)})),errors};
  } catch(error) {
    return {failed:error.stack,results,screenshots,errors,pages:await Promise.all(pages.map(p=>p.evaluate(()=>({log:document.getElementById('log').textContent,crypto:task7.session()?.getStatus(),phase:document.getElementById('panelState').textContent,held:task7.held,gate:task7.gate}))))};
  } finally {await browserCdp.send('Browser.setDownloadBehavior',{behavior:'default',eventsEnabled:false});await browserCdp.detach();for(const p of pages)await p.close();}
}
