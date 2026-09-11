import * as ui from './airbridge-ui.js';

function check(value, message) { if (!value) throw new Error(message); }

export function chatUiTests() {
  return [
    ['Chat detached outbound card rolls back its new URL and keeps the rejection identity', () => {
      // Given a pending card detached after its sender owns it.
      const row=ui.addMessage(document.body,{side:'you',kind:'attachment',meta:{name:'file'},pending:true});
      row.remove();
      const create=URL.createObjectURL,revoke=URL.revokeObjectURL;const created=[],revoked=[];
      URL.createObjectURL=blob=>{const url=create(blob);created.push(url);return url;};
      URL.revokeObjectURL=url=>{revoked.push(url);revoke(url);};
      try {
        // When a verified outbound receipt tries to replace the detached card.
        let failure;try{ui.completeOutboundCard(row,{name:'file',mimeType:'application/octet-stream',payloadSize:0n,payloadSha256:'a'.repeat(64)},new File([],'file'));}catch(error){failure=error;}
        // Then its original rejection is retained and no unreachable URL is owned.
        check(failure?.message==='Outbound attachment card is detached','replacement error lost');
        check(created.length===1&&revoked.length===1&&created[0]===revoked[0],'replacement URL leaked');
      } finally {row.remove();URL.createObjectURL=create;URL.revokeObjectURL=revoke;}
    }],
    ['Chat replacement failure rolls back the newly owned URL', () => {
      // Given an attached pending card whose native replacement fails.
      const row=ui.addMessage(document.body,{side:'you',kind:'attachment',meta:{name:'file'},pending:true});
      row.querySelector('.attachment-card').replaceWith=()=>{throw new Error('replacement blocked');};
      const create=URL.createObjectURL,revoke=URL.revokeObjectURL;const created=[],revoked=[];
      URL.createObjectURL=blob=>{const url=create(blob);created.push(url);return url;};
      URL.revokeObjectURL=url=>{revoked.push(url);revoke(url);};
      try {
        // When replacement rejects before the new card is connected.
        let failure;try{ui.completeOutboundCard(row,{name:'file',mimeType:'application/octet-stream',payloadSize:0n,payloadSha256:'a'.repeat(64)},new File([],'file'));}catch(error){failure=error;}
        // Then its rejection and exactly-once rollback both survive.
        check(failure?.message==='replacement blocked','replacement failure identity lost');
        check(created.length===1&&revoked.length===1&&created[0]===revoked[0],'replacement URL leaked');
      } finally {row.remove();URL.createObjectURL=create;URL.revokeObjectURL=revoke;}
    }],
    ['Chat verified card separates display and safe download names', async () => {
      // Given verified adversarial metadata, not markup.
      const name = '../folder\\report\u202E.txt', mime = 'text/<img src=x onerror=alert(1)>';
      const blob = new Blob(['abc'], {type:mime});
      const lifecycle = ui.createAttachmentLifecycle();
      try {
        // When a verified receipt is rendered.
        const card = ui.renderVerifiedAttachmentCard({blob,name,mime,size:3n,hash:'a'.repeat(64)}, lifecycle);
        document.body.append(card);
        // Then metadata is text, download is a safe basename, and verification is machine readable.
        check(card.querySelector('a').download === 'report.txt', 'unsafe download filename');
        check(card.querySelector('.attachment-name').textContent === name, 'display name not independently text-rendered');
        check(card.dataset.verification === 'verified' && card.dataset.phase === 'Ready', 'missing verified state');
        check(card.dataset.size === '3' && card.dataset.sha256 === 'a'.repeat(64), 'missing exact receipt metadata');
        check(!card.querySelector('img,script,iframe') && card.textContent.includes(mime), 'metadata became markup');
        check(card.querySelector('[data-remove-attachment]'), 'missing accessible remove control');
      } finally { lifecycle.dispose(); }
    }],
    ['Chat phase progress counts payload bytes monotonically within a phase', () => {
      // Given the same panel IDs used by both endpoint pages.
      const host = document.createElement('section');
      for (const id of ['panelState','throughput']) { const el=document.createElement('div'); el.id=id; host.append(el); }
      document.body.append(host);
      try {
        // When reports arrive out of order, terminal state preserves the high-water count.
        ui.setTransferPhase('Sending', {bytes:5n,total:10n});
        ui.setTransferPhase('Sending', {bytes:3n,total:10n});
        ui.setTransferPhase('Cancelled');
        // Then selectors expose the deterministic terminal and plaintext-byte receipt.
        check(host.querySelector('#panelState').dataset.phase === 'Cancelled', 'missing terminal phase');
        check(host.querySelector('#throughput').dataset.bytes === '5', 'payload progress regressed');
      } finally { host.remove(); }
    }],
    ['Chat verified download removal revokes once and removes its owner', () => {
      // Given a verified card with URL instrumentation.
      const originalCreate=URL.createObjectURL, originalRevoke=URL.revokeObjectURL;
      const created=[], revoked=[];
      URL.createObjectURL=blob=> { const url=originalCreate(blob); created.push(url); return url; };
      URL.revokeObjectURL=url=> { revoked.push(url); originalRevoke(url); };
      const lifecycle=ui.createAttachmentLifecycle();
      try {
        const card=ui.renderVerifiedAttachmentCard({blob:new Blob([]),name:'empty',mime:'',size:0n,hash:'b'.repeat(64)}, lifecycle);
        document.body.append(card);
        // When the actual remove button is pressed and cleanup repeats.
        const button=card.querySelector('[data-remove-attachment]');
        check(button, 'missing remove button'); button.click(); lifecycle.dispose(); lifecycle.dispose();
        // Then its URL was allocated/revoked exactly once and no broken live card survives.
        check(created.length===1 && revoked.length===1 && created[0]===revoked[0] && !card.isConnected, 'URL ownership mismatch');
      } finally { lifecycle.dispose(); URL.createObjectURL=originalCreate; URL.revokeObjectURL=originalRevoke; }
    }],
  ];
}
