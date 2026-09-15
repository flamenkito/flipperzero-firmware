import * as ui from './airbridge-ui.js';
import { chatCommand, loadChatTestPage, waitChat } from './protocol-chat-pages-tests.js';

function check(value, message) { if (!value) throw new Error(message); }

function waitFor(predicate, message, timeout = 2000) {
  const started = performance.now();
  return new Promise((resolve, reject) => {
    const probe = () => {
      if (predicate()) { resolve(); return; }
      if (performance.now() - started >= timeout) { reject(new Error(message)); return; }
      setTimeout(probe, 16);
    };
    probe();
  });
}

export function chatUiTests() {
  return [
    ...[
      ['USB', 'usb', [['you', 'you/usb'], ['peer', 'peer/ble']]],
      ['BLE', 'ble', [['you', 'you/ble'], ['peer', 'peer/usb']]],
      ['system on USB', 'usb', [['system', 'system']]],
      ['system on BLE', 'ble', [['system', 'system']]],
      ['endpoint absent', undefined, [['you', 'You'], ['peer', 'Peer'], ['system', 'System']]],
    ].map(([name, endpoint, labels]) => [`Chat prompt labels: ${name}`, () => {
      const originalEndpoint = document.body.dataset.endpoint;
      const transcript = document.createElement('section');
      transcript.setAttribute('aria-live', 'polite');
      try {
        if (endpoint === undefined) delete document.body.dataset.endpoint;
        else document.body.dataset.endpoint = endpoint;
        document.body.append(transcript);
        for (const [side, expected] of labels) {
          const text = '<img src=x onerror=alert(1)> & message';
          const row = ui.addMessage(transcript, {side, kind:'text', text});
          const sender = row.querySelector('.bubble > .meta-line > .sender');
          check(sender?.textContent === expected, `${side}: expected ${expected}, got ${sender?.textContent}`);
          check(sender.childNodes.length === 1 && sender.firstChild.nodeType === Node.TEXT_NODE, 'prompt is not real text');
          check(row.className === `message ${side}`, 'row class hooks changed');
          check(sender.previousElementSibling?.textContent, 'timestamp missing');
          check(row.querySelector('.bubble').lastElementChild.textContent === text && !row.querySelector('img'), 'message became markup');
        }
        check(transcript.getAttribute('aria-live') === 'polite', 'transcript live semantics changed');
      } finally {
        if (originalEndpoint === undefined) delete document.body.dataset.endpoint;
        else document.body.dataset.endpoint = originalEndpoint;
        transcript.remove();
      }
    }]),
    ['Chat empty state is a removable system transcript row', () => {
      const transcript = document.createElement('section');
      document.body.append(transcript);
      try {
        ui.clearTranscript(transcript, 'Nothing here yet.');
        const empty = transcript.querySelector(':scope > .message.system.empty');
        check(transcript.children.length === 1 && empty, 'empty state is not the sole outer transcript row');
        check(empty.querySelector('.bubble > .meta-line > .sender')?.textContent === 'system>', 'empty sender prompt changed');
        check(empty.querySelector('.bubble')?.lastElementChild?.textContent === 'Nothing here yet.', 'empty body text changed');
        ui.addMessage(transcript, {side:'system',kind:'text',text:'First record'});
        check(!transcript.querySelector('.empty'), 'first message did not remove empty row');
        check(transcript.children.length === 1 && transcript.textContent.includes('First record'), 'first message structure changed');
      } finally { transcript.remove(); }
    }],
    ['Chat compact attachment preserves full receipt contracts', () => {
      const hash = '0123456789abcdef'.repeat(4);
      const name = '../verbatim\\name\u202E.txt';
      const card = ui.renderAttachmentCard({name,mimeType:'text/plain',size:1234}, null, hash, false);
      try {
        document.body.append(card);
        const details = card.querySelector('details.attachment-hash-details');
        const full = card.querySelector('.attachment-hash-full');
        check(card.querySelector('.attachment-name')?.textContent === name, 'attachment name is not verbatim text');
        check(card.querySelector('.attachment-meta')?.textContent === '1.2 KB · text/plain', 'compact size/MIME summary changed');
        check(card.querySelector('[data-attachment-hash]')?.textContent === `sha256:${ui.shortHash(hash)} verified`, 'compact hash summary changed');
        check(details && !details.open && details.querySelector('summary')?.textContent === '[sha256]', 'full hash disclosure is not closed by default');
        check(full?.textContent === `sha256:${hash}` && card.textContent.includes(hash), 'full hash is absent from card text');
        check(card.dataset.verification === 'verified' && card.dataset.phase === 'Ready', 'attachment verification datasets changed');
        check(card.dataset.size === '1234' && card.dataset.sha256 === hash, 'attachment receipt datasets changed');
        check(!card.querySelector('img,script,iframe'), 'attachment metadata became markup');
      } finally { card.remove(); }
    }],
    ['Chat transcript controller pins, counts, jumps, and resets', () => {
      const transcript = document.createElement('section');
      const jump = document.createElement('button');
      let scrollTop = 0;
      Object.defineProperties(transcript, {
        clientHeight: {value:60}, clientWidth: {value:240},
        scrollHeight: {get:() => transcript.children.length * 30},
        scrollTop: {get:() => scrollTop, set:value => { scrollTop = Math.max(0, Math.min(Number(value), Math.max(0, transcript.scrollHeight - transcript.clientHeight))); }},
      });
      for (let index = 0; index < 4; index++) transcript.append(document.createElement('div'));
      document.body.append(transcript, jump);
      transcript.scrollTop = transcript.scrollHeight;
      const controller = ui.addMessage.createTranscriptController(transcript, jump, new EventTarget());
      try {
        ui.addMessage(transcript, {side:'peer',kind:'text',text:'pinned'});
        check(transcript.scrollHeight - transcript.clientHeight - transcript.scrollTop === 0, 'pinned append moved off bottom');
        transcript.scrollTop = 0; transcript.dispatchEvent(new Event('scroll'));
        ui.addMessage(transcript, {side:'peer',kind:'text',text:'unread one'});
        ui.addMessage(transcript, {side:'peer',kind:'text',text:'unread two'});
        check(scrollTop === 0 && !jump.hidden && jump.textContent === '[2 new] G', 'unpinned append moved or unread count differs');
        jump.click();
        check(transcript.scrollHeight - transcript.clientHeight - transcript.scrollTop === 0 && jump.hidden, 'jump did not pin and reset unread count');
        transcript.scrollTop = 0; transcript.dispatchEvent(new Event('scroll'));
        ui.addMessage(transcript, {side:'system',kind:'text',text:'before clear'});
        ui.clearTranscript(transcript, 'Reset row');
        check(jump.hidden && jump.textContent === '[0 new] G', 'clear did not reset unread count');
        check(transcript.querySelector(':scope > .message.system.empty'), 'clear did not restore structured empty row');
        controller.dispose(); controller.dispose();
        transcript.append(document.createElement('div')); transcript.scrollTop = 0; jump.click();
        check(scrollTop === 0 && controller.disposed, 'disposed jump listener still owns transcript position');
      } finally { controller.dispose(); transcript.remove(); jump.remove(); }
    }],
    ['Chat transcript controller detaches every synthetic host listener', () => {
      const OriginalResizeObserver = globalThis.ResizeObserver;
      const observers = [];
      class ProbeResizeObserver {
        constructor(callback) { this.callback = callback; this.disconnects = 0; observers.push(this); }
        observe(target) { this.target = target; }
        disconnect() { this.disconnects += 1; }
        fire() { this.callback([{target:this.target}]); }
      }
      const track = target => {
        const added = new Map(), removed = new Map();
        const add = target.addEventListener.bind(target), remove = target.removeEventListener.bind(target);
        target.addEventListener = (type, listener, options) => { added.set(type, (added.get(type) || 0) + 1); return add(type, listener, options); };
        target.removeEventListener = (type, listener, options) => { removed.set(type, (removed.get(type) || 0) + 1); return remove(type, listener, options); };
        return {added, removed};
      };
      const fixtures = [];
      globalThis.ResizeObserver = ProbeResizeObserver;
      try {
        for (let index = 0; index < 3; index++) {
          const transcript = document.createElement('section'), jump = document.createElement('button'), pageEvents = new EventTarget();
          const transcriptEvents = track(transcript), jumpEvents = track(jump), pageEventCounts = track(pageEvents);
          document.body.append(transcript, jump);
          const controller = ui.addMessage.createTranscriptController(transcript, jump, pageEvents);
          check(ui.addMessage.createTranscriptController(transcript, jump, pageEvents) === controller, 'host acquired a second live controller');
          fixtures.push({transcript,jump,pageEvents,controller,transcriptEvents,jumpEvents,pageEventCounts,observer:observers.at(-1)});
        }
        for (const fixture of fixtures) { fixture.transcript.remove(); fixture.observer.fire(); }
        for (const {jump,controller,transcriptEvents,jumpEvents,pageEventCounts,observer} of fixtures) {
          check(controller.disposed && observer.disconnects === 1, 'detached host observer was not disposed exactly once');
          check(transcriptEvents.added.get('scroll') === 1 && transcriptEvents.removed.get('scroll') === 1, 'scroll listener survived teardown');
          check(jumpEvents.added.get('click') === 1 && jumpEvents.removed.get('click') === 1, 'jump listener survived teardown');
          check(pageEventCounts.added.get('resize') === 1 && pageEventCounts.removed.get('resize') === 1, 'resize listener survived teardown');
          check(pageEventCounts.added.get('pagehide') === 1 && pageEventCounts.removed.get('pagehide') === 1, 'pagehide listener survived teardown');
          controller.dispose(); jump.remove();
        }
      } finally {
        for (const {controller,transcript,jump} of fixtures) { controller.dispose(); transcript.remove(); jump.remove(); }
        globalThis.ResizeObserver = OriginalResizeObserver;
      }
    }],
    ['Chat logger reports exact hidden rows and exposes the oldest', () => {
      const parent = document.createElement('section'), log = document.createElement('pre'), earlier = document.createElement('button');
      let scrollTop = 0;
      log.textContent = 'Waiting…'; earlier.className = 'log-earlier'; earlier.type = 'button';
      Object.defineProperties(log, {
        clientHeight:{value:40}, clientTop:{value:0},
        scrollHeight:{get:() => log.children.length * 20},
        scrollTop:{get:() => scrollTop, set:value => { scrollTop = Number(value); }},
      });
      log.getBoundingClientRect = () => ({top:100});
      parent.append(log, earlier); document.body.append(parent);
      try {
        const logger = ui.createLogger(log, {earlierControl:earlier});
        for (let index = 0; index < 5; index++) logger.log(`record ${index}`);
        [...log.children].forEach((row, index) => { row.getBoundingClientRect = () => ({top:100 + index * 20 - scrollTop}); });
        check([...log.children].every(row => row.title === row.textContent && /^\[\d{2}:\d{2}:\d{2}\]/.test(row.textContent)), 'log title or timestamp contract changed');
        scrollTop = 40; log.dispatchEvent(new Event('scroll'));
        check(!earlier.hidden && earlier.textContent === '[2 earlier] gg', 'hidden-row count is not exact');
        check(earlier instanceof HTMLButtonElement && earlier.tabIndex === 0, 'earlier control lost keyboard semantics');
        earlier.focus(); earlier.click();
        check(scrollTop === 0 && earlier.hidden && document.activeElement === log.firstElementChild, 'activation did not reveal and focus oldest hidden row');
        logger.clear();
        check(log.children.length === 0 && earlier.hidden, 'logger clear did not reset earlier control');
      } finally { parent.remove(); }
    }],
    ['Chat mobile earlier-log control stays inside its diagnostic rail', async () => {
      const pages = [];
      try {
        for (const endpoint of ['usb','ble']) pages.push(await loadChatTestPage(endpoint, `log-rail-${Date.now()}-${endpoint}`));
        for (const {iframe,win} of pages) {
          iframe.hidden = false; iframe.style.cssText = 'width:375px;height:900px;border:0';
          const doc = win.document, log = doc.getElementById('log'), earlier = doc.querySelector('.log-earlier');
          chatCommand(win, 'l');
          log.textContent = '';
          for (let index = 0; index < 30; index++) { const row=doc.createElement('div'); row.textContent=`record ${index}`; log.append(row); }
          log.scrollTop = 80; log.dispatchEvent(new win.Event('scroll')); win.scrollTo(0, 0);
          const rail = doc.querySelector('.diagnostic-rail'), hint = doc.getElementById('attachmentLimits');
          const before = earlier.getBoundingClientRect(), hintRect = hint.getBoundingClientRect(), railRect = rail.getBoundingClientRect();
          const overlapWidth = Math.max(0, Math.min(before.right, hintRect.right) - Math.max(before.left, hintRect.left));
          const overlapHeight = Math.max(0, Math.min(before.bottom, hintRect.bottom) - Math.max(before.top, hintRect.top));
          check(overlapWidth * overlapHeight === 0, 'earlier-log control overlaps attachment limits before diagnostic rail enters viewport');
          check(before.top >= railRect.top && before.bottom <= railRect.bottom, 'earlier-log control escapes diagnostic rail before it enters viewport');
          rail.scrollIntoView({block:'center'});
          const visible = earlier.getBoundingClientRect(), visibleRail = rail.getBoundingClientRect();
          check(visible.top >= visibleRail.top && visible.bottom <= visibleRail.bottom, 'earlier-log control escapes visible diagnostic rail');
        }
      } finally { for (const {qa,iframe} of pages) { qa?.dispose(); iframe.remove(); } }
    }],
    ['Chat pages pin aria, display precedence, and mock disclosure contracts', async () => {
      const room = `ui-contract-${Date.now()}`, pages = [];
      try {
        for (const endpoint of ['usb','ble']) pages.push(await loadChatTestPage(endpoint, room));
        for (const {win} of pages) {
          const doc = win.document, display = doc.getElementById('displayState'), panel = doc.getElementById('panelState');
          check(doc.getElementById('transcript').getAttribute('aria-live') === 'polite', 'transcript aria-live changed');
          check(doc.getElementById('cryptoPill').getAttribute('aria-live') === 'polite', 'crypto aria-live changed');
          check(display.textContent === 'state=idle' && display.dataset.state === 'idle', 'disconnected display is not exactly idle');
          const phaseLabel = win.getComputedStyle(panel, '::before').content.replace(/^(["'])(.*)\1$/, '$2');
          check(phaseLabel !== 'state=', 'transfer pane renders a second display-state label');
          check(doc.querySelector('.prompt-context').textContent.includes('disconnected locked'), 'visible connection and encryption status missing');
          check(panel.textContent === 'Ready' && panel.dataset.phase === 'Ready', 'authoritative Ready transfer phase changed');
          display.textContent = 'sentinel'; panel.dataset.phase = 'Sending';
          await waitChat(win, () => display.textContent === 'state=idle' && display.dataset.state === 'idle');
          panel.dataset.phase = 'Ready';
          const summary = doc.querySelector('.prompt-context');
          check(summary.textContent.includes('[mock]') && summary.getAttribute('role') === 'status', 'mock transport is not identified in the statusline');
          chatCommand(win, 'h');
          check(doc.getElementById('terminalOutput').textContent.includes('?mock=1&mockPeer=demo'), 'help buffer lost paired mock usage help');
          chatCommand(win, 'q');
        }
        for (const {win} of pages) win.document.getElementById('connectBtn').click();
        await Promise.all(pages.map(({win}) => waitChat(win, () => !win.document.getElementById('acceptSasBtn').disabled)));
        for (const {win} of pages) {
          const display = win.document.getElementById('displayState'), panel = win.document.getElementById('panelState');
          check(display.textContent === 'state=locked' && display.dataset.state === 'locked', 'connected pre-SAS display is not exactly locked');
          display.textContent = 'sentinel'; panel.dataset.phase = 'Hashing';
          await waitChat(win, () => display.textContent === 'state=locked' && display.dataset.state === 'locked');
          panel.dataset.phase = 'Ready';
        }
        for (const {win} of pages) win.document.getElementById('acceptSasBtn').click();
        await Promise.all(pages.map(({win,qa}) => waitChat(win, () => qa.ready()?.state === 'unlocked')));
        for (const {win} of pages) {
          const display = win.document.getElementById('displayState'), panel = win.document.getElementById('panelState');
          await waitChat(win, () => display.textContent === 'state=ready' && display.dataset.state === 'ready');
          display.textContent = 'sentinel'; panel.dataset.phase = 'Verifying';
          await waitChat(win, () => display.textContent === 'state=verifying' && display.dataset.state === 'verifying');
          panel.dataset.phase = 'Ready'; await waitChat(win, () => display.textContent === 'state=ready');
        }
        for (const {win} of pages) if (!win.document.getElementById('disconnectBtn').disabled) win.document.getElementById('disconnectBtn').click();
        await Promise.all(pages.map(({win}) => waitChat(win, () => win.document.getElementById('displayState').textContent === 'state=idle')));
      } finally { for (const {qa,iframe} of pages) { qa?.dispose(); iframe.remove(); } }
    }],
    ['Chat iframe removal disposes page-owned transcript controllers repeatedly', async () => {
      const pages = [];
      try {
        for (let index = 0; index < 4; index++) pages.push(await loadChatTestPage(index % 2 ? 'ble' : 'usb', `teardown-${Date.now()}-${index}`));
        const controllers = pages.map(({qa}) => qa.transcriptController());
        check(controllers.every(controller => controller && !controller.disposed), 'iframe did not own a live transcript controller');
        for (const {iframe} of pages) iframe.remove();
        await waitFor(() => controllers.every(controller => controller.disposed), 'iframe removal left a transcript observer/listener alive');
        check(new Set(controllers).size === controllers.length, 'separate iframes shared transcript ownership');
      } finally { for (const {qa,iframe} of pages) { qa?.dispose(); iframe.remove(); } }
    }],
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
        ui.setTransferPhase('Receiving', {bytes:0n,total:10n,reset:true});
        ui.setTransferPhase('Receiving', {bytes:5n});
        ui.setTransferPhase('Receiving', {bytes:3n});
        check(host.querySelector('#throughput').dataset.progress === '50', 'receive progress is not cumulative');
        check(host.querySelector('.ab-progress-value').textContent === '50%', 'receive ASCII progress disagrees with received bytes');
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
