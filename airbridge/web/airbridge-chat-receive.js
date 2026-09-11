import { addMessage } from './airbridge-ui.js';
import { MSG, parseV2RoutingFrame } from './airbridge-protocol.js';

export function routeReceiveControl(frame, receiver) {
  // BUSY rejects an outbound HELLO; it never owns the inbound transfer, even
  // when both directions allocated the same ID during simultaneous offers.
  if (!receiver || frame[0] !== MSG.ERROR) return null;
  // Pre-capability UTF-8 ERROR belongs to the pending outbound HELLO, not
  // the inbound item parser. Item-scoped controls still route here first.
  if (frame[0] === MSG.ERROR && !(frame[5] === 65 && frame[6] === 66 && frame[7] === 50 && frame[8] === 83)) return null;
  try {
    const msg = parseV2RoutingFrame(frame);
    if (msg.itemId === receiver.snapshot().itemId) return receiver.onMessage(frame);
  } catch (error) { return receiver.onMessage(frame); }
  return null;
}

export async function renderReceivedItem(transcript, item) {
  if (!item.isCurrent()) return;
  switch (item.kind) {
    case 'text': {
      const text = await item.blob.text();
      if (item.isCurrent()) addMessage(transcript, {side:'peer', kind:'text', text, hash:item.hash});
      break;
    }
    case 'attachment':
      addMessage(transcript, {side:'peer', kind:'attachment', verified:item});
      break;
    default: throw new Error('unsupported verified item kind');
  }
}
