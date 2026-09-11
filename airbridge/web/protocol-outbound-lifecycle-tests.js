import { pair } from './protocol-outbound-tests.js';
import { OutboundTransfer } from './airbridge-outbound.js';
import * as p from './airbridge-protocol.js';

function check(value, label) { if (!value) throw new Error(label); }
function deadline(promise) {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error('cancellation stranded pending read')), 300);
  })]).finally(() => clearTimeout(timer));
}

export function outboundLifecycleTests(vendor) {
  const tests = [];
  for (const mode of ['reset', 'clearKeys']) tests.push([`Outbound ${mode} cancels pending hash reader`, async () => {
    // Given an unlocked session and an owned pending native read.
    const [session] = await pair();
    let entered, cancelled = 0, writes = 0;
    const ready = new Promise(resolve => { entered = resolve; });
    const source = new ReadableStream({type:'bytes', pull() { entered(); }, cancel() { cancelled++; }});
    const transfer = new OutboundTransfer({session, hashImplementation:vendor, send:async () => { writes++; }});
    const sent = transfer.sendFile({size:1, name:'a', type:'', stream:() => source}).catch(error => error);
    // When the crypto session is invalidated without closing the producer externally.
    await ready; session[mode]();
    try {
      const failure = await deadline(sent);
      // Then cancellation settles, unlocks the reader and emits no item frame.
      check(failure instanceof Error && cancelled === 1 && !source.locked && writes === 0, 'reset ownership');
    } finally { transfer.cancel(); await sent; }
  }]);
  for (const fault of ['cancel', 'release', 'both']) tests.push([`Outbound cleanup ${fault} preserves primary identity`, async () => {
    // Given an injected read failure accompanied by independent cleanup failures.
    const [session] = await pair(), primary = new Error('read primary'), cleanup = new Error('cleanup secondary');
    let cancels = 0, releases = 0;
    const file = {size:1, name:'a', type:'', stream:() => ({getReader:() => ({
      read:async () => { throw primary; },
      cancel:async () => { cancels++; if (fault !== 'release') throw cleanup; },
      releaseLock() { releases++; if (fault !== 'cancel') throw cleanup; },
    })})};
    const transfer = new OutboundTransfer({session, hashImplementation:vendor, send:async () => {}});
    // When hashing fails and cleanup also fails.
    const failure = await transfer.sendFile(file).catch(error => error);
    // Then the original failure wins, with one attempt at each cleanup operation.
    check(failure === primary, 'cleanup masked primary');
    check(cancels === 1 && releases === 1 && transfer.cleanupError === cleanup, 'cleanup accounting');
  }]);
  tests.push(['Outbound cleanup-only failure prevents ID allocation', async () => {
    const [session] = await pair(), cleanup = new Error('release only');
    const transfer = new OutboundTransfer({session, hashImplementation:vendor, send:async () => {}});
    const failure = await transfer.sendFile({size:0, name:'a', type:'', stream:() => ({getReader:() => ({
      read:async () => ({done:true}), releaseLock() { throw cleanup; }, cancel:async () => {},
    })})}).catch(error => error);
    check(failure === cleanup && session.streamOutboundId === 0, 'cleanup-only failure hidden');
  }]);
  tests.push(['Outbound reentrant transport cancellation observes late rejection', async () => {
    const [session] = await pair();
    let transfer, cancels = 0;
    transfer = new OutboundTransfer({session, hashImplementation:vendor, send:frame => {
      const msg = p.parseMessage(frame);
      if (msg.type === p.MSG.CANCEL) { cancels++; return Promise.resolve(); }
      transfer.cancel();
      return Promise.reject(new Error('late write failure'));
    }});
    const failure = await transfer.sendText('x').catch(error => error);
    check(failure instanceof Error && cancels === 1, 'reentrant cancellation');
  }]);
  for (const stage of ['hash', 'crypto', 'write']) tests.push([`Outbound ${stage} failure outranks native cleanup and permits next send`, async () => {
    const [session] = await pair(), primary = new Error(`${stage} primary`), cleanup = new Error('cancel cleanup');
    let streams = 0, cancels = 0, releases = 0, transfer;
    const native = new File([new Uint8Array(70000)],'failure.bin');
    const file = {size:native.size,name:native.name,type:'',stream() {
      streams++;
      const stream = native.stream(), getReader = stream.getReader.bind(stream);
      stream.getReader = options => {
        const reader = getReader(options), cancel = reader.cancel.bind(reader), release = reader.releaseLock.bind(reader);
        reader.cancel = async () => { cancels++; await cancel(); throw cleanup; };
        reader.releaseLock = () => { releases++; release(); };
        return reader;
      };
      return stream;
    }};
    const originalEncrypt = crypto.subtle.encrypt;
    const hashImplementation = stage === 'hash' ? {create() {
      return {update() { throw primary; },array() { throw new Error('unreachable hash finalization'); }};
    }} : vendor;
    if (stage === 'crypto') crypto.subtle.encrypt = async () => { throw primary; };
    const send = async frame => {
      const msg = p.parseV2Frame(frame, transfer.itemId);
      if (msg.type === p.MSG.CANCEL) return;
      if (stage === 'write' && msg.type === p.MSG.ITEM_DATA && transfer.itemId === 1) throw primary;
      transfer.receive(p.buildMessage(p.MSG.ACK,0,p.makeV2AckPayload(msg)));
    };
    transfer = new OutboundTransfer({session,hashImplementation,send});
    try {
      const failure = await transfer.sendFile(file).catch(error => error);
      check(failure === primary && transfer.cleanupError === cleanup, 'primary/cleanup identity');
      check(cancels === 1 && releases === streams, 'native reader cleanup counts');
    } finally { crypto.subtle.encrypt = originalEncrypt; }
    const burned = session.streamOutboundId;
    transfer = new OutboundTransfer({session,hashImplementation:vendor,send});
    await transfer.sendText('recovered');
    check(transfer.itemId === burned + 1 && transfer.snapshot().completed, 'next ID/send failed');
  }]);
  return tests;
}
