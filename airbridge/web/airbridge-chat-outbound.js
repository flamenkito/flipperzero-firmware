import { OutboundTransfer } from './airbridge-outbound.js';
import { buildMessage, buildV2Frame, compareV2Offers, makeV2AckPayload, MSG, parseMessage, parseV2Frame, CRYPTO_STATE } from './airbridge-protocol.js';
import { addMessage, completeOutboundCard, setTransferPhase, updateCardProgress } from './airbridge-ui.js';

export function createChatOutbound({session, transport, setBusy, log}) {
  let active = null;
  let generation = 0, disposed = false, callbackError = null;
  const element = id => document.getElementById(id);
  const write = frame => transport.send(frame);
  function notify(item, message, level) {
    try { if (generation === item.generation) log(message, level); }
    catch (error) { callbackError = error; }
    finally {
      if (generation === item.generation) {
        try { setBusy(false); } catch (error) { callbackError ??= error; }
      }
    }
  }
  function cancel() {
    const item = active;
    if (!item) return;
    active = null;
    if (item.row) updateCardProgress(item.row, item.percent, 'Cancelled', 'cancelled');
    setTransferPhase('Cancelled');
    item.owner.cancel();
    notify(item, 'Transfer cancelled', 'warn');
  }
  async function send(input, kind) {
    if (disposed || active || !session.isUnlocked() || !transport.isConnected()) return;
    const inputElement = element(kind === 'text' ? 'textInput' : 'fileInput');
    const originalValue = inputElement.value;
    let draftChanged = false;
    const markDraftChanged = () => { draftChanged = true; };
    const inputEvent = kind === 'text' ? 'input' : 'change';
    inputElement.addEventListener?.(inputEvent, markDraftChanged);
    const item = {owner:null, row:null, percent:0, generation:++generation,
      rejectedOfferId:null, collisionSeen:false, capabilityProven:false};
    active = item;
    item.owner = new OutboundTransfer({session, send:write, isCurrent:() => active === item,
      timeoutMs:5000, retries:4, onProgress:event => {
        if (active !== item) return;
        if (event.phase === 'Sending') {
          item.capabilityProven = true;
          item.rejectedOfferId = null;
        }
        item.percent = event.total ? Number(event.bytes * 10000n / event.total) / 100 : 0;
        setTransferPhase(event.phase, event);
        if (item.row) updateCardProgress(item.row, item.percent, `${event.phase}: ${event.bytes}/${event.total} bytes`);
        if (event.phase === 'Sending' && event.bytes === event.total) setTransferPhase('Verifying');
      }});
    try {
      setBusy(true);
      if (active !== item) return;
      if (kind === 'attachment') item.row = addMessage(element('transcript'), {
        side:'you', kind, meta:{name:input.name, size:input.size, mimeType:input.type}, pending:true,
        progress:{percent:0, label:'Hashing: 0 bytes'},
      });
      const receipt = await (kind === 'text' ? item.owner.sendText(input) : item.owner.sendFile(input));
      if (active !== item) return;
      if (item.row) completeOutboundCard(item.row, receipt, input);
      else addMessage(element('transcript'), {side:'you', kind:'text', text:input});
      // A message or attachment prepared while this item was sending belongs
      // to the next transfer. Completion must not erase that newer draft.
      if (!draftChanged && inputElement.value === originalValue &&
          (kind === 'text' || !inputElement.files || inputElement.files[0] === input)) {
        inputElement.value = '';
      }
      active = null;
      setTransferPhase('Ready', {bytes:receipt.payloadSize,total:receipt.payloadSize});
      notify(item, `Sent encrypted ${kind}; plaintext SHA-256 ${receipt.payloadSha256}.`, 'ok');
    } catch (error) {
      if (active !== item) return;
      active = null;
      if (item.row) updateCardProgress(item.row, item.percent, 'Failed', 'cancelled');
      setTransferPhase('Failed', { error: error.message });
      item.owner.cancel(error);
      notify(item, `Send failed: ${error.message}`, 'error');
      if (error.message === 'Unsupported protocol version') session.clearKeys(CRYPTO_STATE.ABORTED);
    } finally {
      inputElement.removeEventListener?.(inputEvent, markDraftChanged);
      if (active === item) {
        active = null;
        setTransferPhase('Cancelled');
        setBusy(false);
      }
    }
  }
  const unsubscribe = session.onInvalidated(cancel);
  function dispose() { disposed = true; unsubscribe(); cancel(); }
  function receive(frame) {
    const msg = parseMessage(frame);
    if (!msg) return false;
    // The losing offer's CANCEL precedes its HELLO ACK on the ordered transport.
    // Consume that one rejection before same-ID outbound control dispatch.
    if (msg.type === MSG.CANCEL && active?.rejectedOfferId != null) {
      const control = parseV2Frame(frame);
      if (control.itemId === active.rejectedOfferId) {
        active.rejectedOfferId = null;
        return true;
      }
    }
    if (active?.owner.receive(frame)) return true;
    if ([MSG.ACK, MSG.NACK].includes(msg.type) && msg.payload.length > 4) return true;
    if ([MSG.ERROR, MSG.BUSY].includes(msg.type) && msg.payload[0] === 65 && msg.payload[1] === 66 && msg.payload[2] === 50 && msg.payload[3] === 83) return true;
    if (msg.type === MSG.CANCEL && msg.payload.length === 4) {
      const control = parseV2Frame(frame);
      if (control.itemId !== active?.owner.itemId) return false;
      cancel();
      void write(buildMessage(MSG.ACK, 0, makeV2AckPayload(control))).catch(() => {});
      return true;
    }
    if (msg.type === MSG.HELLO && active) {
      let offer;
      try { offer = parseV2Frame(frame); }
      catch (error) {
        if (error.message === 'Unsupported protocol version') return false;
        throw error;
      }
      const id = active.owner.itemId;
      if (id !== null && compareV2Offers(id, session.role, offer.itemId, session.role === 1 ? 2 : 1) < 0) {
        if (id === offer.itemId && !active.capabilityProven && !active.collisionSeen) {
          active.collisionSeen = true;
          active.rejectedOfferId = id;
        }
        void write(buildV2Frame(MSG.BUSY, 0, offer.itemId)).catch(() => {});
        return true;
      }
      cancel();
    }
    return false;
  }
  return {sendFile:file => send(file, 'attachment'), sendText:text => send(text, 'text'), cancel, receive, dispose,
    get callbackError() { return callbackError; }};
}
