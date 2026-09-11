import { loadChatTestPage } from './protocol-chat-pages-tests.js';
import { pair } from './protocol-outbound-tests.js';
import { item, complete, check } from './protocol-item-receiver-tests.js';

export async function runChatCleanup(endpoint, action, qa) {
  // Given a verified text consumer held before its asynchronous DOM publication.
  async function install() {
    const sessions=await pair(),sender=endpoint==='usb'?sessions[1]:sessions[0],session=endpoint==='usb'?sessions[0]:sessions[1];
    qa.install(session,async()=>{});return sender;
  }
  let sender=await install(),release,entered;
  const held=new Promise(resolve=>{release=resolve;}),started=new Promise(resolve=>{entered=resolve;});
  const original=Blob.prototype.text;let first=true;
  Blob.prototype.text=async function(){if(first){first=false;entered();await held;}return original.call(this);};
  const old=await item(sender,new TextEncoder().encode('obsolete text'),{kind:'text'});
  const pending=complete({onMessage:qa.receive},old);
  try {
    await started;
    // When the real lifecycle control invalidates the old receive and a newer card completes.
    if(action==='pagehide') window.dispatchEvent(new Event('pagehide'));
    else document.getElementById(action==='clear'?'clearTranscriptBtn':action==='cancel'?'cancelBtn':'disconnectBtn').click();
    if(action==='disconnect'||action==='pagehide')sender=await install();
    const next=await item(sender);
    await complete({onMessage:qa.receive},next);
    const before={phase:document.getElementById('panelState').dataset.phase,urls:qa.urls.created.length,
      controls:document.getElementById('fileInput').disabled,html:document.getElementById('transcript').textContent};
    release();await pending;
    const after={phase:document.getElementById('panelState').dataset.phase,urls:qa.urls.created.length,
      controls:document.getElementById('fileInput').disabled,html:document.getElementById('transcript').textContent};
    // Then late completion cannot overwrite the newer card, controls, or phase.
    check(JSON.stringify(before)===JSON.stringify(after),`${endpoint}/${action}: late completion mutated newer UI`);
    check(before.urls===1&&!before.html.includes('obsolete text'),'unverified or stale content escaped');
    window.dispatchEvent(new Event('pagehide'));window.dispatchEvent(new Event('pagehide'));
    check(qa.urls.revoked.length===1&&new Set(qa.urls.revoked).size===1,'pagehide revoke count differs');
    return {endpoint,action,created:1,revoked:1,lateSuppressed:true};
  } finally {release();Blob.prototype.text=original;qa.dispose();}
}

export function chatCleanupTests() {
  return ['usb','ble'].flatMap(endpoint=>['clear','cancel','disconnect','pagehide'].map(action=>[
    `Chat ${endpoint} ${action} suppresses late completion and drains URLs`,async()=>{
      const {qa,iframe}=await loadChatTestPage(endpoint,`cleanup-${Date.now()}`);
      try{return await qa.runCleanup(action);}finally{qa?.dispose();iframe.remove();}
    },
  ]));
}
