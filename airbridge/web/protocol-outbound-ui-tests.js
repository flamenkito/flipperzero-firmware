import { pair } from './protocol-outbound-tests.js';
import { createChatOutbound } from './airbridge-chat-outbound.js';
import * as p from './airbridge-protocol.js';

export function outboundUiTests() {
  return ['setBusy', 'log', 'hash-caption'].map(gate => [`Outbound UI ${gate} reentrancy and ownership`, async () => {
    // Given real encrypted sending and a disposable DOM with injectable callbacks.
    const [session] = await pair();
    const host = document.createElement('div');
    for (const id of ['panelState','throughput','transcript','textInput','fileInput']) {
      const node = document.createElement(id.endsWith('Input') ? 'input' : 'div');
      node.id = id; host.append(node);
    }
    document.body.append(host);
    let controller, replaced = false, next, busy = false;
    const transport = {isConnected:() => true, send:async frame => {
      const msg = p.parseV2Frame(frame, new DataView(frame.buffer, frame.byteOffset).getUint32(5));
      if (msg.type !== p.MSG.CANCEL) controller.receive(p.buildMessage(p.MSG.ACK,0,p.makeV2AckPayload({...msg,itemId:session.streamOutboundId})));
    }};
    function replace() {
      if (replaced) return;
      replaced = true; controller.cancel();
      host.querySelector('#textInput').value = 'replacement draft';
      host.querySelector('#fileInput').value = 'replacement selection';
      next = controller.sendText('replacement');
    }
    controller = createChatOutbound({session,transport,setBusy:value => {
      busy = value;
      if (gate === 'setBusy' && value) replace();
    },log:() => { if (gate === 'log') replace(); }});
    try {
      // When callbacks reenter while the old transfer is publishing its state.
      await controller.sendFile(new File(['abc'],'test.bin'));
      await next;
      // Then old state cannot strand busy controls or leave a contradictory card.
      if (gate === 'setBusy' && host.querySelectorAll('.attachment-card').length) throw new Error('stale pending card inserted');
      if (gate === 'hash-caption') {
        const card = host.querySelector('.attachment-card');
        if (!card.textContent.includes('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad') || card.textContent.includes('pending')) throw new Error('authoritative hash missing');
      }
      if (busy) throw new Error('busy stranded');
      if (gate === 'log' && host.querySelector('#fileInput').value !== 'replacement selection') throw new Error('stale input clearing');
    } finally { controller.cancel(); host.remove(); }
  }]);
}
