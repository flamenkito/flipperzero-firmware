import * as p from './airbridge-protocol.js';

const check = (value, message) => { if (!value) throw new Error(message); };
const tick = () => new Promise(resolve => setTimeout(resolve, 0));
async function until(predicate) {
  for (let attempt = 0; attempt < 500; attempt++) {
    if (predicate()) return;
    await tick();
  }
  throw new Error('arbitration fixture did not settle');
}

export const arbitrationCases = [
  'equal CANCEL first', 'equal BUSY first', 'equal receiver cancel',
  'USB lower ID', 'BLE lower ID', 'USB legitimate cancel', 'BLE legitimate cancel',
  'equal dropped offer then receiver cancel', 'equal retry then receiver cancel',
  'USB legitimate cancel before proof', 'BLE legitimate cancel before proof',
];

export async function runArbitration(mode, endpoints) {
  const sessions = endpoints.map((endpoint, i) => endpoint.createSession(i + 1, async frame => {
    const peer = sessions[1 - i], msg = p.parseMessage(frame);
    if (msg.type === p.MSG.ACK) peer.receiveAck(msg); else await peer.onMessage(msg);
  }));
  await sessions[1].start(); await sessions[0].start();
  await sessions[0].acceptSas(); await sessions[1].acceptSas();
  const queue = [[], []], wire = [[], []], pending = [];
  const settled = [false, false];
  let losingHello;
  const legitimate = mode.includes('legitimate');
  const winner = mode.startsWith('BLE') ? 1 : 0, loser = 1 - winner;
  const deliver = async (from, type) => {
    const index = type === undefined ? 0 : queue[from].findIndex(frame => frame[0] === type);
    check(index >= 0 && queue[from].length > 0, `missing frame ${type} from ${from}`);
    const [frame] = queue[from].splice(index, 1);
    await endpoints[1 - from].receive(frame);
    return frame;
  };
  try {
    for (let i = 0; i < 2; i++) endpoints[i].install(sessions[i], async frame => {
      const copy = new Uint8Array(frame);
      queue[i].push(copy); wire[i].push(copy);
    });
    if (mode.includes('lower ID')) sessions[loser].allocateStreamItemId();
    for (const i of legitimate ? [winner] : [0, 1]) {
      pending.push(endpoints[i].send(`arbitration ${mode}`).finally(() => { settled[i] = true; }));
    }
    await until(() => queue[winner].some(f => f[0] === p.MSG.HELLO) &&
      (legitimate || queue[loser].some(f => f[0] === p.MSG.HELLO)));
    if (!legitimate) {
      const left = p.parseV2Frame(queue[winner][0]), right = p.parseV2Frame(queue[loser][0]);
      check(p.compareV2Offers(left.itemId, winner + 1, right.itemId, loser + 1) < 0, 'wrong deterministic winner');
      losingHello = await deliver(loser, p.MSG.HELLO);
    }
    await deliver(winner, p.MSG.HELLO);
    if (!legitimate) {
      if (mode === 'equal BUSY first') {
        await deliver(winner, p.MSG.BUSY);
        check(endpoints[loser].snapshot().itemId === sessions[winner].streamOutboundId,
          `retired outbound BUSY aborted winning inbound: ${endpoints[loser].logs()}`);
      }
      const cancelled = mode.includes('dropped offer')
        ? queue[loser].splice(queue[loser].findIndex(f => f[0] === p.MSG.CANCEL), 1)[0]
        : await deliver(loser, p.MSG.CANCEL);
      check(p.parseV2Frame(cancelled).itemId === sessions[loser].streamOutboundId, 'loser CANCEL ownership');
      await tick();
      check(!settled[winner], `losing offer CANCEL aborted winning outbound: ${endpoints[winner].logs()}`);
      if (queue[winner].some(f => f[0] === p.MSG.BUSY)) await deliver(winner, p.MSG.BUSY);
      check(endpoints[loser].snapshot().itemId === sessions[winner].streamOutboundId,
        'retired outbound BUSY aborted winning inbound');
    }
    const beforeProof = mode.includes('before proof');
    if (mode.includes('retry then')) await endpoints[winner].receive(losingHello);
    if (!beforeProof) { await deliver(loser, p.MSG.ACK); await tick(); }
    const cancelWinner = legitimate || mode.includes('receiver cancel');
    if (cancelWinner) await endpoints[loser].cancelReceive();
    if (beforeProof) await deliver(loser, p.MSG.CANCEL);
    for (let attempt = 0; attempt < 1000 && !settled[winner]; attempt++) {
      for (const from of [winner, loser]) if (queue[from].length) await deliver(from);
      await tick();
    }
    check(settled[winner], 'winning outbound did not settle');
    await Promise.all(pending);
    const log = endpoints[winner].logs();
    if (cancelWinner) {
      check(!log.includes('Sent encrypted'), `legitimate CANCEL was swallowed: ${log}`);
      check(endpoints[loser].items() === 0, 'cancelled transfer published an item');
    } else {
      check(log.includes('Sent encrypted text'), `winner failed to complete: ${endpoints.map(e => e.logs()).join(' | ')}`);
      check(endpoints[loser].items() === 1, 'winner must publish exactly once');
    }
    return {mode, winner, cancelled:cancelWinner, received:endpoints[loser].items(),
      ids:sessions.map(s => s.streamOutboundId), logs:endpoints.map(e => e.logs()),
      controls:wire.map(frames => frames.filter(f => f[0] !== p.MSG.ITEM_DATA).map(f => f[0]))};
  } finally {
    endpoints.forEach(endpoint => endpoint.dispose());
    await Promise.allSettled(pending);
    sessions.forEach(session => session.clearKeys());
  }
}
