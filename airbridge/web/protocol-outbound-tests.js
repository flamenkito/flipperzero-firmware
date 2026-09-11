import * as p from './airbridge-protocol.js';

function check(value, message) { if (!value) throw new Error(message); }
export async function pair() {
  const sessions = [];
  for (const role of [1, 2]) sessions.push(new p.AirBridgeCryptoSession({role, sendFn:async frame => {
    const peer = sessions[role === 1 ? 1 : 0], msg = p.parseMessage(frame);
    if (msg.type === p.MSG.ACK) peer.receiveAck(msg); else await peer.onMessage(msg);
  }}));
  await sessions[1].start(); await sessions[0].start();
  await sessions[0].acceptSas(); await sessions[1].acceptSas();
  return sessions;
}

export function outboundTests(vendor) {
  const tests = [0, 19, 1031, 4194369].map(size => [`Outbound two passes ${size}`, async () => {
    // Given a generated File-shaped source with forbidden materializers.
    const { OutboundTransfer } = await import('./airbridge-outbound.js');
    const [session] = await pair();
    let acquisitions = 0, done = 0, data = 0;
    const progress = [];
    const file = {size, name:'generated.bin', type:'application/octet-stream', stream() {
      acquisitions++;
      let offset = 0;
      return new ReadableStream({type:'bytes', pull(controller) {
        if (offset === size) { controller.close(); controller.byobRequest?.respond(0); return; }
        const count = Math.min(8191, size - offset), bytes = new Uint8Array(count);
        for (let i = 0; i < count; i++) bytes[i] = (offset + i) % 251;
        offset += count; controller.enqueue(bytes);
      }});
    }, arrayBuffer() { throw new Error('forbidden materialization'); }};
    const transfer = new OutboundTransfer({session, hashImplementation:vendor,
      send:async frame => {
        const msg = p.parseV2Frame(frame, transfer.itemId);
        if (msg.type === p.MSG.ITEM_DONE) done++;
        if (msg.type === p.MSG.ITEM_DATA) data++;
        transfer.receive(p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload(msg)));
      }, onProgress:event => progress.push(event)});
    // When sending through the real segmented sender with an ACK-only test peer.
    await transfer.sendFile(file);
    // Then exactly two independent acquisitions and payload-only progress complete.
    check(acquisitions === 2, `stream acquisitions ${acquisitions}`);
    check(done === 1, 'missing DONE');
    for (const phase of ['Hashing', 'Sending']) {
      const events = progress.filter(e => e.phase === phase);
      check(events.length > 0 && events.at(-1).bytes === BigInt(size), 'payload progress total');
      check(events.every((e, i) => e.bytes >= (events[i - 1]?.bytes ?? 0n) && e.bytes <= BigInt(size)), 'nonmonotonic progress');
    }
    console.info('Outbound counts', JSON.stringify({size, acquisitions, done, data,
      hashing:progress.filter(e => e.phase === 'Hashing').length,
      sending:progress.filter(e => e.phase === 'Sending').length}));
  }]);
  for (const fault of ['short', 'long', 'mutation', 'read-error']) tests.push([`Outbound rejects second-pass ${fault}`, async () => {
    const { OutboundTransfer } = await import('./airbridge-outbound.js');
    const [session] = await pair();
    let acquisitions = 0, done = 0, cancels = 0;
    const file = {size:19, name:'a', type:'', stream() {
      const second = ++acquisitions === 2;
      if (second && fault === 'read-error') return new ReadableStream({type:'bytes', pull(c) { c.error(new Error('source error')); }});
      const length = second && fault === 'short' ? 18 : second && fault === 'long' ? 20 : 19;
      return new Blob([new Uint8Array(length).fill(second && fault === 'mutation' ? 1 : 0)]).stream();
    }};
    const transfer = new OutboundTransfer({session, hashImplementation:vendor, send:async frame => {
      const msg = p.parseV2Frame(frame, transfer.itemId);
      if (msg.type === p.MSG.ITEM_DONE) done++;
      if (msg.type === p.MSG.CANCEL) { cancels++; return; }
      transfer.receive(p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload(msg)));
    }});
    let failure;
    try { await transfer.sendFile(file); } catch (error) { failure = error; }
    check(failure && done === 0 && cancels === 1 && session.streamOutboundId === 1, 'drift completed or ID refunded');
  }]);
  for (const gate of ['hash-progress', 'hash-read', 'before-allocation', 'after-allocation', 'encrypt', 'write', 'ack', 'nack', 'timeout']) {
    tests.push([`Outbound cancellation at ${gate}`, async () => {
      const { OutboundTransfer } = await import('./airbridge-outbound.js');
      const [session] = await pair();
      let cancels = 0, hellos = 0, done = 0, readsCancelled = 0, trigger, release;
      const entered = new Promise(resolve => { trigger = resolve; });
      const held = new Promise(resolve => { release = resolve; });
      let acquisitions = 0;
      const file = {size:19, name:'a', type:'', stream() {
        acquisitions++;
        if (gate === 'hash-read') return new ReadableStream({type:'bytes', pull() { trigger(); }, cancel() { readsCancelled++; }});
        if (gate === 'before-allocation' && acquisitions === 2) transfer.cancel();
        return new Blob([new Uint8Array(19)]).stream();
      }};
      const originalOpen = session.openEncryptedStream.bind(session);
      session.openEncryptedStream = (...args) => {
        const stream = originalOpen(...args);
        if (gate === 'after-allocation') transfer.cancel();
        return stream;
      };
      const originalEncrypt = crypto.subtle.encrypt;
      if (gate === 'encrypt') crypto.subtle.encrypt = async function(...args) {
        trigger(); await held; return originalEncrypt.apply(this, args);
      };
      let dataAttempts = 0;
      const transfer = new OutboundTransfer({session, hashImplementation:vendor, timeoutMs:10,
        onProgress:event => { if (gate === 'hash-progress' && event.bytes > 0n) transfer.cancel(); },
        send:async frame => {
          const msg = p.parseV2Frame(frame, transfer.itemId);
          if (msg.type === p.MSG.CANCEL) { cancels++; return; }
          if (msg.type === p.MSG.HELLO) hellos++;
          if (msg.type === p.MSG.ITEM_DONE) done++;
          if (msg.type === p.MSG.ITEM_DATA) {
            dataAttempts++;
            if (gate === 'write') { trigger(); await held; }
            if (gate === 'ack') { trigger(); return; }
            if (gate === 'timeout') { if (dataAttempts === 2) trigger(); return; }
            if (gate === 'nack') {
              if (dataAttempts === 2) { trigger(); return; }
              transfer.receive(p.buildMessage(p.MSG.NACK, 0, p.makeV2AckPayload(msg, 1))); return;
            }
          }
          transfer.receive(p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload(msg)));
        }});
      let failure;
      const sending = transfer.sendFile(file).catch(error => { failure = error; });
      try {
        if (['hash-read', 'encrypt', 'write', 'ack', 'nack', 'timeout'].includes(gate)) {
          await entered; transfer.cancel(); transfer.cancel();
        }
        await sending;
        const beforeHello = ['hash-progress', 'hash-read', 'before-allocation', 'after-allocation'].includes(gate);
        check(failure && done === 0, 'cancel published completion');
        check(hellos === (beforeHello ? 0 : 1) && cancels === (beforeHello ? 0 : 1), 'cancel/HELLO count');
        check(session.streamOutboundId === (beforeHello && gate !== 'after-allocation' ? 0 : 1), 'ID burn boundary');
        if (gate === 'hash-read') check(readsCancelled === 1, 'native read cancellation');
        console.info('Outbound cancel', JSON.stringify({gate, hellos, cancels, done, id:session.streamOutboundId, dataAttempts}));
      } finally { release(); crypto.subtle.encrypt = originalEncrypt; }
    }]);
  }
  tests.push(['Outbound late write cannot affect next generation', async () => {
    const { OutboundTransfer } = await import('./airbridge-outbound.js');
    const [session] = await pair();
    let release, entered, current, oldProgress = 0, nextDone = 0, cancels = 0;
    const held = new Promise(resolve => { release = resolve; });
    const ready = new Promise(resolve => { entered = resolve; });
    const first = new OutboundTransfer({session, hashImplementation:vendor,
      isCurrent:() => current === first, onProgress:() => oldProgress++, send:async frame => {
        const msg = p.parseV2Frame(frame, first.itemId);
        if (msg.type === p.MSG.CANCEL) { cancels++; return; }
        if (msg.type === p.MSG.ITEM_DATA) { entered(); await held; }
        first.receive(p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload(msg)));
      }});
    current = first;
    const old = first.sendFile(new File(['first'], 'first')).catch(error => error);
    await ready;
    first.cancel(); first.cancel();
    await old;
    const frozenProgress = oldProgress;
    const next = new OutboundTransfer({session, hashImplementation:vendor, isCurrent:() => current === next,
      send:async frame => {
        const msg = p.parseV2Frame(frame, next.itemId);
        if (msg.type === p.MSG.ITEM_DONE) nextDone++;
        next.receive(p.buildMessage(p.MSG.ACK, 0, p.makeV2AckPayload(msg)));
      }});
    current = next;
    release();
    await next.sendText('second');
    check(oldProgress === frozenProgress && nextDone === 1 && cancels === 1 && next.itemId === 2, 'stale generation mutation');
  }]);
  return tests;
}
