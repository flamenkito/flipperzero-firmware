import * as p from './airbridge-protocol.js';
import { check, fixture, item, doneAcks } from './protocol-item-receiver-tests.js';

export function itemReceiverGenerationTests(vendor) {
  return ['sync', 'async', 'current'].map(mode => [`R2 receiver HELLO listener ${mode} failure retains operation ownership`, async () => {
    // Given the reviewer's item-1 listener that replaces itself with item 2.
    const f = await fixture(vendor), old = await item(f.sender), next = await item(f.sender);
    const failure = new Error('old hello listener failure');
    let nextHello;
    f.receiver.on('hello', detail => {
      if (detail.itemId !== old.id) return;
      if (mode !== 'current') {
        f.receiver.resetItem();
        nextHello = f.receiver.onMessage(next.hello);
      }
      if (mode === 'async') return Promise.resolve().then(() => { throw failure; });
      throw failure;
    });
    try {
      // When item 1 fails after its listener has already entered item 2.
      await f.receiver.onMessage(old.hello);
      await nextHello;
      // Then only a still-current failure is terminal; stale failure cannot affect item 2.
      const snapshot = f.receiver.snapshot();
      console.info('R2 ownership', JSON.stringify({mode, state:snapshot.state, itemId:snapshot.itemId,
        controls:f.sent.map(m => ({type:m.type, itemId:m.itemId}))}));
      if (mode === 'current') {
        check(f.errors[0] === failure && snapshot.state === 'terminal', 'current primary failure lost');
        check(f.sent.some(m => m.type === p.MSG.ERROR && m.itemId === old.id), 'current failure not reported');
        return;
      }
      check(snapshot.itemId === next.id && snapshot.state === 'hello', 'old listener terminated item 2');
      check(f.errors.length === 0 && !f.sent.some(m => m.type === p.MSG.ERROR), 'obsolete failure attributed to replacement');
      for (const frame of [...next.meta, ...next.data, next.done]) await f.receiver.onMessage(frame);
      check(f.items.length === 1 && f.items[0].itemId === next.id && doneAcks(f.sent) === 1, 'replacement did not complete');
    } finally { f.receiver.dispose(); }
  }]);
}
