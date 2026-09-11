import { AirBridgeCryptoSession, CRYPTO_STATE, MSG, parseMessage, parseV2Frame, V2ReceiveState } from './airbridge-protocol.js';
import { createIncrementalSha256 } from './airbridge-incremental-sha256.js';
import { payloadDigestHex } from './airbridge-file-pass.js';

// Mock-only sender oracle: authenticate and count, never retain payload or create a Blob/card.
export function installMockStreamPeer(transport, role) {
  let state, receiver, hash, pendingConfirm;
  const session = new AirBridgeCryptoSession({role, sendFn:frame => transport.send(frame)});
  transport.onReceive(async frame => {
    const msg = parseMessage(frame);
    if (!msg) return;
    if (msg.type === MSG.ACK) { session.receiveAck(msg); return; }
    if (msg.type === MSG.NACK) { session.receiveNack(msg); return; }
    if (session.isCryptoFrame(msg)) {
      if (msg.type === MSG.KEY_CONFIRM && session.state !== CRYPTO_STATE.SAS_PENDING && session.state !== CRYPTO_STATE.UNLOCKED) { pendingConfirm = msg; return; }
      await session.onMessage(msg);
      if (pendingConfirm && session.state === CRYPTO_STATE.SAS_PENDING) {
        const confirm = pendingConfirm; pendingConfirm = null;
        await session.onMessage(confirm);
      }
      if (session.state === CRYPTO_STATE.SAS_PENDING && session.peerConfirmed && !session.localAccepted) await session.acceptSas();
      return;
    }
    if (!session.isUnlocked()) return;
    state ??= new V2ReceiveState({keyId:session.keyId, direction:role === 1 ? 'ble-to-usb' : 'usb-to-ble'});
    const parsed = parseV2Frame(frame, state.itemId);
    if (msg.type === MSG.ITEM_DONE) state.verifyCompletion({...receiver.finish(), actualSha256:payloadDigestHex(hash.finalize())});
    const result = state.accept(frame);
    if (state.meta && !receiver) {
      receiver = session.openSegmentReceiver({...state.meta, totalCiphertextBytes:String(state.meta.totalCiphertextBytes)});
      hash = createIncrementalSha256();
    }
    if (msg.type === MSG.ITEM_DATA && result.action === 'ack') {
      const authenticated = await receiver.push({itemId:parsed.itemId, segmentIndex:parsed.segment, chunkInSegment:parsed.seq}, parsed.body);
      if (authenticated.action === 'authenticated') hash.update(authenticated.payload);
    }
    if (msg.type === MSG.CANCEL || msg.type === MSG.ITEM_DONE) {
      receiver?.abort(); receiver = null; hash = null;
    }
    if (result.frame && transport.isConnected()) await transport.send(result.frame);
  });
  void session.start();
}
